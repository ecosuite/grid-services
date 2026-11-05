"""The control API: plays the utility's part.

JSON over HTTP under /api, with the token check and errors from
`test_common.control`. The page at / is a thin client for the same API.

Clients are addressed by the name in their `/d/{client}/` path, end devices
by id, lFDI or sFDI. Times are ISO 8601 in UTC; limits are in watts.
"""

from __future__ import annotations

import logging
from datetime import datetime, timedelta, timezone
from importlib import resources
from typing import Any

from aiohttp import web
from test_common.control import InvalidRequest, json_response, read_body, typed_field

from test_csipaus.state import Client, DefaultControl, EndDevice, Server, now, parse_base

log = logging.getLogger("test_csipaus.control")

DEFAULT_START_IN_SECONDS = 60
DEFAULT_DURATION_SECONDS = 300
# randomizeStart and randomizeDuration are OneHourRangeType.
ONE_HOUR = 3600


def parse_start(body: dict[str, Any]) -> datetime:
    if "start" in body and "start_in_seconds" in body:
        raise InvalidRequest("give start or start_in_seconds, not both")
    if "start" in body:
        text = typed_field(body, "start", str)
        try:
            parsed = datetime.fromisoformat(text.replace("Z", "+00:00"))
        except ValueError:
            raise InvalidRequest(f"start is not an ISO 8601 time: {text}") from None
        if parsed.tzinfo is None:
            raise InvalidRequest("start needs a time zone")
        return parsed.astimezone(timezone.utc).replace(microsecond=0)
    seconds = typed_field(body, "start_in_seconds", (int, float), DEFAULT_START_IN_SECONDS)
    return now().replace(microsecond=0) + timedelta(seconds=seconds)


def randomize(body: dict[str, Any], name: str) -> int | None:
    value = typed_field(body, name, int)
    if value is not None and not -ONE_HOUR <= value <= ONE_HOUR:
        raise InvalidRequest(f"{name} must be within an hour either way")
    return value


class ControlApi:
    def __init__(self, server: Server):
        self.server = server

    def routes(self) -> list[web.RouteDef]:
        device = "/api/clients/{client}/devices/{device}"
        return [
            web.get("/", self.page),
            web.get("/healthz", self.health),
            web.get("/api/server", self.describe_server),
            web.get("/api/clients", self.list_clients),
            web.get("/api/clients/{client}", self.get_client),
            web.delete("/api/clients/{client}", self.delete_client),
            web.get("/api/clients/{client}/messages", self.list_messages),
            web.get("/api/messages/unattributed", self.list_unattributed),
            web.get(device, self.get_device),
            web.patch(device, self.configure_device),
            web.delete(device, self.delete_device),
            web.put(f"{device}/default-control", self.set_default_control),
            web.delete(f"{device}/default-control", self.clear_default_control),
            web.get(f"{device}/controls", self.list_controls),
            web.post(f"{device}/controls", self.create_control),
            web.get(f"{device}/controls/{{control}}", self.get_control),
            web.post(f"{device}/controls/{{control}}/cancel", self.cancel_control),
            web.get("/api/clients/{client}/mirror-usage-points/{mup}", self.get_mup),
            web.patch("/api/clients/{client}/mirror-usage-points/{mup}", self.configure_mup),
        ]

    def client(self, request: web.Request) -> Client:
        return self.server.get(request.match_info["client"])

    def device(self, request: web.Request) -> EndDevice:
        return self.client(request).device(request.match_info["device"])

    def control(self, request: web.Request):
        device = self.device(request)
        try:
            control = device.controls[int(request.match_info["control"])]
        except (ValueError, KeyError):
            raise KeyError(f"control {request.match_info['control']}") from None
        return device, control

    # -- Handlers -----------------------------------------------------------

    async def page(self, request: web.Request) -> web.Response:
        html = resources.files("test_csipaus").joinpath("static/index.html").read_text()
        return web.Response(text=html, content_type="text/html")

    async def health(self, request: web.Request) -> web.Response:
        return json_response({"ok": True})

    async def describe_server(self, request: web.Request) -> web.Response:
        return json_response({
            "poll_rate_seconds": self.server.poll_rate_seconds,
            "post_rate_seconds": self.server.post_rate_seconds,
            "pin": self.server.pin,
            "time_zone": self.server.time_zone,
            "client_name_prefix": self.server.client_name_prefix,
            "dcap_path": "/d/{client}/dcap",
        })

    async def list_clients(self, request: web.Request) -> web.Response:
        return json_response([c.summary() for c in self.server.clients.values()])

    async def get_client(self, request: web.Request) -> web.Response:
        return json_response(self.client(request).to_json())

    async def delete_client(self, request: web.Request) -> web.Response:
        self.server.remove(request.match_info["client"])
        return web.Response(status=204)

    async def get_device(self, request: web.Request) -> web.Response:
        return json_response(self.device(request).to_json())

    async def configure_device(self, request: web.Request) -> web.Response:
        """{"pin": the Registration's PIN, "primacy": the program's}"""
        device = self.device(request)
        body = await read_body(request)
        pin = typed_field(body, "pin", int)
        primacy = typed_field(body, "primacy", int)
        if pin is not None:
            if not 0 <= pin <= 999999:
                raise InvalidRequest("pin must have at most six digits")
            device.pin = pin
        if primacy is not None:
            if not 0 <= primacy <= 255:
                raise InvalidRequest("primacy must be 0 to 255")
            device.primacy = primacy
        return json_response(device.to_json())

    async def delete_device(self, request: web.Request) -> web.Response:
        """Forgets an end device, as a utility that deregistered it would."""
        client = self.client(request)
        device = client.device(request.match_info["device"])
        del client.end_devices[device.id]
        return web.Response(status=204)

    async def set_default_control(self, request: web.Request) -> web.Response:
        """{"base": {...}} replaces the default control, raising its version."""
        device = self.device(request)
        body = await read_body(request)
        base = parse_base(body.get("base"), "base")
        if device.default_control is None:
            device.default_control = DefaultControl(base)
        else:
            device.default_control.base = base
            device.default_control.version += 1
            device.default_control.changed_at = now()
        log.info("set the default control of end device %s: %s", device.id, base)
        return json_response(device.default_control.to_json())

    async def clear_default_control(self, request: web.Request) -> web.Response:
        self.device(request).default_control = None
        return web.Response(status=204)

    async def list_controls(self, request: web.Request) -> web.Response:
        device = self.device(request)
        responses = list(device.responses)
        return json_response([c.to_json(responses) for c in device.controls.values()])

    async def create_control(self, request: web.Request) -> web.Response:
        device = self.device(request)
        body = await read_body(request)
        base = parse_base(body.get("base"), "base")
        if not base:
            raise InvalidRequest("base needs at least one limit or switch")
        duration = typed_field(body, "duration_seconds", int, DEFAULT_DURATION_SECONDS)
        if duration <= 0:
            raise InvalidRequest("duration_seconds must be positive")
        response_required = typed_field(body, "response_required", int, 3)
        if not 0 <= response_required <= 255:
            raise InvalidRequest("response_required is a byte of flags")
        control = device.add_control(
            start=parse_start(body), duration_seconds=duration, base=base,
            description=typed_field(body, "description", str),
            randomize_start_seconds=randomize(body, "randomize_start_seconds"),
            randomize_duration_seconds=randomize(body, "randomize_duration_seconds"),
            response_required=response_required)
        log.info("created control %s for end device %s", control.id, device.id)
        return json_response(control.to_json(list(device.responses)), status=201)

    async def get_control(self, request: web.Request) -> web.Response:
        device, control = self.control(request)
        return json_response(control.to_json(list(device.responses)))

    async def cancel_control(self, request: web.Request) -> web.Response:
        device, control = self.control(request)
        status = control.status()
        if status in ("cancelled", "completed"):
            raise InvalidRequest(f"control {control.id} is already {status}")
        control.cancelled_at = now()
        log.info("cancelled control %s for end device %s", control.id, device.id)
        return json_response(control.to_json(list(device.responses)))

    def mup(self, request: web.Request):
        client = self.client(request)
        try:
            return client.mirror_usage_points[int(request.match_info["mup"])]
        except (ValueError, KeyError):
            raise KeyError(f"mirror usage point {request.match_info['mup']}") from None

    async def get_mup(self, request: web.Request) -> web.Response:
        return json_response(self.mup(request).to_json())

    async def configure_mup(self, request: web.Request) -> web.Response:
        """{"post_rate_seconds": how often the client is to post readings}"""
        mup = self.mup(request)
        body = await read_body(request)
        post_rate = typed_field(body, "post_rate_seconds", int)
        if post_rate is not None:
            if post_rate <= 0:
                raise InvalidRequest("post_rate_seconds must be positive")
            mup.post_rate = post_rate
        return json_response(mup.to_json())

    async def list_messages(self, request: web.Request) -> web.Response:
        return json_response(self.filter_messages(self.client(request).messages, request))

    async def list_unattributed(self, request: web.Request) -> web.Response:
        return json_response(self.filter_messages(self.server.unattributed, request))

    @staticmethod
    def filter_messages(messages, request: web.Request) -> list[dict[str, Any]]:
        """?type= keeps exchanges carrying that document either way,
        ?after= only later entries, and ?xml=1 includes the documents."""
        wanted = request.query.get("type")
        try:
            after = int(request.query.get("after", "0"))
        except ValueError:
            raise InvalidRequest("after must be a sequence number") from None
        with_xml = request.query.get("xml") in ("1", "true")
        selected = []
        for message in messages:
            if message["seq"] <= after or wanted and wanted not in (
                    message.get("request_type"), message.get("response_type")):
                continue
            if not with_xml:
                message = {k: v for k, v in message.items() if not k.endswith("_xml")}
            selected.append(message)
        return selected
