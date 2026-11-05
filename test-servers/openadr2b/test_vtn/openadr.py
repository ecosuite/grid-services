"""The OpenADR 2.0b side of the test VTN: simple HTTP pull endpoints.

OpenLEADR supplies schema validation, parsing and message rendering. The VTN
behaviour is ours: OpenLEADR's own server keeps state we cannot inspect,
answers every error with code 200, has no EiOpt handler, and requests
periodic reports without the reportInterval that rule 324 requires.

The endpoints are served twice:

    /OpenADR2/Simple/2.0b/{service}
    /v/{ven_name}/OpenADR2/Simple/2.0b/{service}

A VEN configured with the second form identifies itself by URL. That is the
only way to answer oadrQueryRegistration, which carries no venID, without
client certificates.
"""

from __future__ import annotations

import logging
from datetime import datetime, timedelta
from functools import partial
from typing import Any

from aiohttp import web
from lxml.etree import XMLSyntaxError
from openleadr import objects
from openleadr.messaging import parse_message, validate_xml_schema

from test_vtn.messages import render
from test_vtn.state import Ven, Vtn, new_id, now

log = logging.getLogger("test_vtn.openadr")

PREFIX = "/OpenADR2/Simple/2.0b"

# Which messages each endpoint accepts. oadrResponse is accepted anywhere.
SERVICES = {
    "EiRegisterParty": {"oadrQueryRegistration", "oadrCreatePartyRegistration",
                        "oadrCancelPartyRegistration", "oadrCanceledPartyRegistration"},
    "EiEvent": {"oadrRequestEvent", "oadrCreatedEvent"},
    "EiReport": {"oadrRegisterReport", "oadrRegisteredReport", "oadrCreatedReport",
                 "oadrUpdateReport", "oadrCanceledReport"},
    "EiOpt": {"oadrCreateOpt", "oadrCancelOpt"},
    "OadrPoll": {"oadrPoll"},
}

# Messages a VEN may send before it is registered.
UNREGISTERED_OK = {"oadrQueryRegistration", "oadrCreatePartyRegistration",
                   "oadrCancelPartyRegistration", "oadrResponse"}

OK = 200
OUT_OF_SEQUENCE = 450
INVALID_ID = 452
NOT_RECOGNIZED = 453
COMPLIANCE_ERROR = 459
NOT_REGISTERED_OR_AUTHORIZED = 463

# The reply to each request type, used for error answers.
REPLY_TYPES = {
    "oadrQueryRegistration": "oadrCreatedPartyRegistration",
    "oadrCreatePartyRegistration": "oadrCreatedPartyRegistration",
    "oadrCancelPartyRegistration": "oadrCanceledPartyRegistration",
    "oadrRequestEvent": "oadrDistributeEvent",
    "oadrRegisterReport": "oadrRegisteredReport",
    "oadrUpdateReport": "oadrUpdatedReport",
    "oadrCreateOpt": "oadrCreatedOpt",
    "oadrCancelOpt": "oadrCanceledOpt",
}

TELEMETRY_REPORTS = {"TELEMETRY_STATUS", "TELEMETRY_USAGE"}


class Refused(Exception):
    """Answers the request with an OpenADR error code."""

    def __init__(self, code: int, description: str):
        super().__init__(description)
        self.code = code
        self.description = description


class Reply:
    """A message to send back, or `None` type for an empty HTTP body."""

    def __init__(self, type: str | None, code: int = OK, description: str = "OK",
                 **fields: Any):
        self.type = type
        self.code = code
        self.description = description
        self.fields = fields


EMPTY = Reply(None)


class OpenadrEndpoints:
    def __init__(self, vtn: Vtn):
        self.vtn = vtn

    def routes(self) -> list[web.RouteDef]:
        routes = []
        for service in SERVICES:
            handler = partial(self.handle, service)
            routes.append(web.post(f"{PREFIX}/{service}", handler))
            routes.append(web.post(f"/v/{{ven_name}}{PREFIX}/{service}", handler))
        return routes

    # -- HTTP ---------------------------------------------------------------

    async def handle(self, service: str, request: web.Request) -> web.Response:
        if not request.content_type.lower().startswith("application/xml"):
            return web.Response(status=400, text="Content-Type must be application/xml")
        body = await request.read()
        try:
            validate_xml_schema(body)
            message_type, payload = parse_message(body)
        except (XMLSyntaxError, ValueError, KeyError) as error:
            log.warning("rejected a message that failed validation: %s", error)
            self.vtn.record(None, {"service": service, "error": str(error),
                                   "request_xml": body.decode(errors="replace")})
            return web.Response(status=400, text=f"XML failed validation: {error}")

        url_ven_name = request.match_info.get("ven_name")
        ven = self.identify(payload, url_ven_name)
        try:
            reply = self.dispatch(service, message_type, payload, ven, url_ven_name)
        except Refused as refused:
            reply = self.refusal(message_type, payload, refused)
        except Exception:
            log.exception("failed to handle %s", message_type)
            return web.Response(status=500, text="The test VTN failed; see its log")
        # Registration may have created the VEN.
        ven = ven or self.identify(reply.fields, url_ven_name)
        if ven is not None:
            ven.last_contact_at = now()

        try:
            text = self.compose(reply, payload, ven)
        except Exception:
            log.exception("failed to render %s", reply.type)
            return web.Response(status=500, text="The test VTN failed; see its log")
        self.log_exchange(ven, service, message_type, payload, reply, body, text)
        return web.Response(text=text, content_type="application/xml")

    def identify(self, payload: dict[str, Any], url_ven_name: str | None) -> Ven | None:
        return self.vtn.find(payload.get("ven_id")) or self.vtn.find(url_ven_name)

    def refusal(self, message_type: str, payload: dict[str, Any],
                refused: Refused) -> Reply:
        """An error answer, in the message type the request expects."""
        reply_type = REPLY_TYPES.get(message_type, "oadrResponse")
        fields: dict[str, Any] = {}
        if reply_type == "oadrCreatedPartyRegistration":
            fields = self.registration_fields(None)
        elif reply_type == "oadrCanceledPartyRegistration":
            fields = {"registration_id": payload.get("registration_id")}
        elif reply_type == "oadrDistributeEvent":
            fields = {"events": []}
        elif reply_type == "oadrRegisteredReport":
            fields = {"report_requests": []}
        elif reply_type in ("oadrCreatedOpt", "oadrCanceledOpt"):
            fields = {"opt_id": payload.get("opt_id")}
        log.warning("refused %s with %s: %s", message_type, refused.code, refused.description)
        return Reply(reply_type, refused.code, refused.description, **fields)

    def compose(self, reply: Reply, request: dict[str, Any], ven: Ven | None) -> str:
        if reply.type is None:
            return ""
        fields = {
            "vtn_id": self.vtn.vtn_id,
            "ven_id": ven.ven_id if ven else request.get("ven_id"),
            "request_id": new_id(),
            "response": {"response_code": reply.code,
                         "response_description": reply.description,
                         "request_id": request.get("request_id")},
            **reply.fields,
        }
        return render(reply.type, **fields)

    def log_exchange(self, ven: Ven | None, service: str, message_type: str,
                     payload: dict[str, Any], reply: Reply, body: bytes,
                     text: str) -> None:
        # An empty poll says nothing worth keeping.
        if message_type == "oadrPoll" and reply.type == "oadrResponse" and reply.code == OK:
            return
        reply_payload = parse_message(text)[1] if text else None
        self.vtn.record(ven, {
            "service": service,
            "type": message_type,
            "payload": payload,
            "reply_type": reply.type,
            "reply_code": reply.code,
            "reply_payload": reply_payload,
            "request_xml": body.decode(errors="replace"),
            "reply_xml": text,
        })

    # -- Dispatch -----------------------------------------------------------

    def dispatch(self, service: str, message_type: str, payload: dict[str, Any],
                 ven: Ven | None, url_ven_name: str | None) -> Reply:
        if message_type != "oadrResponse" and message_type not in SERVICES[service]:
            raise Refused(COMPLIANCE_ERROR, f"{message_type} does not belong on {service}")
        vtn_id = payload.get("vtn_id")
        if vtn_id is not None and vtn_id != self.vtn.vtn_id:
            raise Refused(INVALID_ID, f"this VTN is {self.vtn.vtn_id}, not {vtn_id}")

        if message_type not in UNREGISTERED_OK and (ven is None or not ven.registered):
            if message_type == "oadrPoll":
                return Reply("oadrRequestReregistration")
            raise Refused(NOT_REGISTERED_OR_AUTHORIZED, "the VEN is not registered")

        handler = getattr(self, f"on_{message_type}", None)
        if handler is None:
            raise Refused(COMPLIANCE_ERROR, f"{message_type} is not supported")
        if message_type in ("oadrQueryRegistration", "oadrCreatePartyRegistration"):
            return handler(payload, ven, url_ven_name)
        return handler(payload, ven)

    # -- Registration -------------------------------------------------------

    def registration_fields(self, ven: Ven | None) -> dict[str, Any]:
        registered = ven is not None and ven.registered
        return {
            "ven_id": ven.ven_id if registered else None,
            "registration_id": ven.registration_id if registered else None,
            "profiles": [{"profile_name": "2.0b",
                          "transports": [{"transport_name": "simpleHttp"}]}],
            "requested_oadr_poll_freq": self.vtn.poll_interval,
        }

    def on_oadrQueryRegistration(self, payload, ven, url_ven_name) -> Reply:
        # The query carries no venID, so only the URL can say who is asking.
        return Reply("oadrCreatedPartyRegistration",
                     **self.registration_fields(self.vtn.find(url_ven_name)))

    def on_oadrCreatePartyRegistration(self, payload, ven, url_ven_name) -> Reply:
        ven_name = payload.get("ven_name")
        if not ven_name:
            raise Refused(COMPLIANCE_ERROR, "venName is required")
        if url_ven_name is not None and ven_name != url_ven_name:
            raise Refused(INVALID_ID, f"this URL is for VEN {url_ven_name}, not {ven_name}")
        if not ven_name.startswith(self.vtn.ven_name_prefix):
            raise Refused(NOT_REGISTERED_OR_AUTHORIZED,
                          f"VEN names must start with {self.vtn.ven_name_prefix}")
        pull = payload.get("http_pull_model", True)
        if payload.get("transport_name") != "simpleHttp" or not pull:
            raise Refused(COMPLIANCE_ERROR, "this VTN supports simple HTTP pull only")
        registered = self.vtn.register(ven_name, payload.get("ven_id"),
                                       payload.get("registration_id"))
        log.info("registered VEN %s (%s) as %s", registered.ven_id, ven_name,
                 registered.registration_id)
        return Reply("oadrCreatedPartyRegistration", **self.registration_fields(registered))

    def on_oadrCancelPartyRegistration(self, payload, ven) -> Reply:
        registration_id = payload.get("registration_id")
        fields = {"registration_id": registration_id}
        if ven is None or not ven.registered:
            return Reply("oadrCanceledPartyRegistration", NOT_RECOGNIZED,
                         "no such registration", **fields)
        if registration_id != ven.registration_id:
            return Reply("oadrCanceledPartyRegistration", INVALID_ID,
                         "not this VEN's registration", **fields)
        self.vtn.forget_registration(ven)
        log.info("VEN %s cancelled its registration", ven.ven_id)
        return Reply("oadrCanceledPartyRegistration", **fields)

    def on_oadrCanceledPartyRegistration(self, payload, ven) -> Reply:
        # The VEN confirms a cancellation this VTN sent.
        self.vtn.forget_registration(ven)
        log.info("VEN %s confirmed the cancellation of its registration", ven.ven_id)
        return Reply("oadrResponse")

    def on_oadrResponse(self, payload, ven) -> Reply:
        return EMPTY

    # -- Polling and events -------------------------------------------------

    def on_oadrPoll(self, payload, ven) -> Reply:
        if ven.outbox:
            message_type, fields = ven.outbox.popleft()
            if message_type == "oadrCancelPartyRegistration":
                fields = {**fields, "registration_id": ven.registration_id}
            return Reply(message_type, **fields)
        if ven.events_changed:
            return Reply("oadrDistributeEvent", events=ven.take_distribution())
        return Reply("oadrResponse")

    def on_oadrRequestEvent(self, payload, ven) -> Reply:
        return Reply("oadrDistributeEvent", events=ven.take_distribution())

    def on_oadrCreatedEvent(self, payload, ven) -> Reply:
        unknown = []
        for response in payload.get("event_responses", []):
            event = ven.events.get(response.get("event_id"))
            if event is None:
                unknown.append(response.get("event_id"))
                continue
            event.responses.append({
                "modification_number": response.get("modification_number"),
                "opt": response.get("opt_type"),
                "via": "oadrCreatedEvent",
                "response_code": response.get("response_code"),
                "response_description": response.get("response_description"),
                "at": now(),
            })
        if unknown:
            raise Refused(INVALID_ID, f"unknown events: {', '.join(map(str, unknown))}")
        return Reply("oadrResponse")

    # -- Opt ----------------------------------------------------------------

    def on_oadrCreateOpt(self, payload, ven) -> Reply:
        opt = {
            "opt_id": payload.get("opt_id"),
            "opt_type": payload.get("opt_type"),
            "opt_reason": payload.get("opt_reason"),
            "event_id": payload.get("event_id"),
            "modification_number": payload.get("modification_number"),
            "at": now(),
        }
        ven.opts.append(opt)
        event = ven.events.get(opt["event_id"]) if opt["event_id"] else None
        if opt["event_id"] and event is None:
            raise Refused(INVALID_ID, f"unknown event {opt['event_id']}")
        if event is not None:
            if opt["modification_number"] not in (None, event.modification_number):
                raise Refused(OUT_OF_SEQUENCE,
                              f"event {event.event_id} is at modification "
                              f"{event.modification_number}")
            event.responses.append({
                "modification_number": event.modification_number,
                "opt": opt["opt_type"],
                "via": "oadrCreateOpt",
                "response_code": None,
                "response_description": opt["opt_reason"],
                "at": opt["at"],
            })
        return Reply("oadrCreatedOpt", opt_id=opt["opt_id"])

    def on_oadrCancelOpt(self, payload, ven) -> Reply:
        return Reply("oadrCanceledOpt", opt_id=payload.get("opt_id"))

    # -- Reports ------------------------------------------------------------

    def on_oadrRegisterReport(self, payload, ven) -> Reply:
        requests = []
        for report in payload.get("reports") or []:
            specifier_id = report.get("report_specifier_id")
            name = str(report.get("report_name", "")).removeprefix("METADATA_")
            descriptions = report.get("report_descriptions") or []
            ven.offered_reports[specifier_id] = {
                "report_specifier_id": specifier_id,
                "report_name": name,
                "r_ids": [d.get("r_id") for d in descriptions],
                "descriptions": descriptions,
                "at": now(),
            }
            if name in TELEMETRY_REPORTS and descriptions and self.vtn.auto_report_requests:
                interval = self.vtn.report_interval
                requests.append(self.report_request(
                    ven, specifier_id, [d.get("r_id") for d in descriptions],
                    granularity=interval, report_back=interval))
        return Reply("oadrRegisteredReport", report_requests=requests)

    def report_request(self, ven: Ven, specifier_id: str, r_ids: list[str],
                       granularity: timedelta, report_back: timedelta,
                       start: datetime | None = None,
                       duration: timedelta = timedelta(0),
                       one_shot: bool = False) -> objects.ReportRequest:
        """A request for a report the VEN offered. Without `one_shot` it
        carries a reportInterval starting at `start` (now by default) and
        running for `duration`, zero meaning until cancelled (rules 324 and
        325). A zero `report_back` with a duration in the past asks for
        history."""
        offered = ven.offered_reports.get(specifier_id, {})
        request = objects.ReportRequest(
            report_request_id=new_id(),
            report_specifier=objects.ReportSpecifier(
                report_specifier_id=specifier_id,
                granularity=granularity,
                report_back_duration=report_back,
                # OpenLEADR renders a zero timedelta as "P", which is not a
                # valid duration, so zero ("until cancelled") is spelled out.
                report_interval=None if one_shot else {
                    "dtstart": start or now(),
                    "duration": duration if duration else "PT0S"},
                specifier_payloads=[
                    objects.SpecifierPayload(r_id=r_id, reading_type="x-notApplicable")
                    for r_id in r_ids
                ],
            ),
        )
        ven.report_requests[request.report_request_id] = {
            "report_request_id": request.report_request_id,
            "report_specifier_id": specifier_id,
            "report_name": offered.get("report_name", specifier_id),
            "r_ids": list(r_ids),
            "granularity_seconds": granularity.total_seconds(),
            "report_back_seconds": report_back.total_seconds(),
            "start": None if one_shot else (start or now()),
            "duration_seconds": None if one_shot else duration.total_seconds(),
            "created": False,
            "cancelled": False,
            "at": now(),
        }
        return request

    def on_oadrCreatedReport(self, payload, ven) -> Reply:
        response = payload.get("response") or {}
        pending = [p.get("report_request_id") for p in payload.get("pending_reports") or []]
        ven.report_answers.append({
            "response_code": response.get("response_code"),
            "response_description": response.get("response_description"),
            "request_id": response.get("request_id"),
            "pending": pending,
            "at": now(),
        })
        for request_id in pending:
            request = ven.report_requests.get(request_id)
            if request is not None:
                request["created"] = True
        return Reply("oadrResponse")

    def on_oadrUpdateReport(self, payload, ven) -> Reply:
        for report in payload.get("reports") or []:
            request = ven.report_requests.get(report.get("report_request_id"), {})
            ven.report_updates.append({
                "report_request_id": report.get("report_request_id"),
                "report_specifier_id": report.get("report_specifier_id"),
                "report_name": request.get("report_name", report.get("report_name")),
                "intervals": report.get("intervals") or [],
                "at": now(),
            })
        return Reply("oadrUpdatedReport")

    def on_oadrCanceledReport(self, payload, ven) -> Reply:
        return Reply("oadrResponse")

    def on_oadrRegisteredReport(self, payload, ven) -> Reply:
        return Reply("oadrResponse")
