"""The control API: plays the utility's part.

JSON over HTTP under /api, with the token check and errors from
`test_common.control`. The page at / is a thin client for the same API.

VENs are addressed by id or name. Times are ISO 8601 in UTC.
"""

from __future__ import annotations

import copy
import logging
from datetime import datetime, timedelta, timezone
from importlib import resources
from typing import Any

from aiohttp import web
from openleadr import enums
from test_common.control import InvalidRequest, json_response, read_body, typed_field

from test_vtn.messages import render
from test_vtn.openadr import OpenadrEndpoints
from test_vtn.state import Event, Interval, Signal, Ven, Vtn, new_id, now

log = logging.getLogger("test_vtn.control")

DEFAULT_START_IN_SECONDS = 60
DEFAULT_INTERVALS = [{"duration_seconds": 300, "value": 1}]


# -- Request parsing --------------------------------------------------------

def parse_time(text: str) -> datetime:
    try:
        parsed = datetime.fromisoformat(text.replace("Z", "+00:00"))
    except ValueError:
        raise InvalidRequest(f"start is not an ISO 8601 time: {text}") from None
    if parsed.tzinfo is None:
        raise InvalidRequest("start needs a time zone")
    return parsed.astimezone(timezone.utc)


def parse_start(body: dict[str, Any], default: datetime | None) -> datetime | None:
    if "start" in body and "start_in_seconds" in body:
        raise InvalidRequest("give start or start_in_seconds, not both")
    if "start" in body:
        return parse_time(typed_field(body, "start", str))
    if "start_in_seconds" in body:
        seconds = typed_field(body, "start_in_seconds", (int, float))
        return now().replace(microsecond=0) + timedelta(seconds=seconds)
    return default


def parse_signals(body: dict[str, Any]) -> list[Signal] | None:
    if "signals" in body and "intervals" in body:
        raise InvalidRequest("give signals or intervals, not both")
    if "intervals" in body:
        # Shorthand for a single SIMPLE level signal.
        raw = [{"name": "SIMPLE", "type": "level", "intervals": body["intervals"]}]
    elif "signals" in body:
        raw = body["signals"]
    else:
        return None
    if not isinstance(raw, list) or not raw:
        raise InvalidRequest("signals must be a non-empty list")

    signals = []
    for spec in raw:
        if not isinstance(spec, dict):
            raise InvalidRequest("each signal must be an object")
        name = typed_field(spec, "name", str)
        kind = typed_field(spec, "type", str)
        if name not in enums.SIGNAL_NAME.values and not str(name).startswith("x-"):
            raise InvalidRequest(f"unknown signal name {name}")
        if kind not in enums.SIGNAL_TYPE.values:
            raise InvalidRequest(f"unknown signal type {kind}")
        intervals = spec.get("intervals")
        if not isinstance(intervals, list) or not intervals:
            raise InvalidRequest(f"signal {name} needs intervals")
        parsed = []
        for interval in intervals:
            if not isinstance(interval, dict):
                raise InvalidRequest("each interval must be an object")
            duration = typed_field(interval, "duration_seconds", int)
            value = typed_field(interval, "value", (int, float))
            # A single interval of length 0 makes an open-ended event (rule 47).
            open_ended = duration == 0 and len(intervals) == 1
            if duration is None or duration <= 0 and not open_ended or value is None:
                raise InvalidRequest("each interval needs a positive duration_seconds and a "
                                     "value; only a lone interval may be 0, for an open-ended "
                                     "event")
            if name == "SIMPLE" and value not in (0, 1, 2, 3):
                raise InvalidRequest("SIMPLE signal levels are 0, 1, 2 or 3")
            parsed.append(Interval(duration, value))
        measurement = spec.get("measurement")
        if measurement is not None and not (
                isinstance(measurement, dict) and "name" in measurement and "unit" in measurement):
            raise InvalidRequest("a measurement needs a name and a unit")
        signals.append(Signal(name, kind, parsed, measurement))

    durations = {s.duration_seconds for s in signals}
    if len(durations) != 1:
        raise InvalidRequest("every signal must last as long as the event")
    return signals


def apply_options(event: Event, body: dict[str, Any]) -> None:
    for name, kind in (("market_context", str), ("response_required", bool),
                       ("test_event", bool), ("priority", int),
                       ("start_after_seconds", int), ("comment", str)):
        if name in body:
            setattr(event, name, typed_field(body, name, kind))
    if not event.market_context:
        raise InvalidRequest("market_context cannot be empty")
    if event.start_after_seconds is not None and event.start_after_seconds < 0:
        raise InvalidRequest("start_after_seconds cannot be negative")
    if event.priority is None or event.priority < 0:
        raise InvalidRequest("priority must be zero or more")


def check_renderable(event: Event, ven: Ven, vtn: Vtn) -> None:
    """Renders the event now, rather than failing on the VEN's next poll."""
    try:
        render("oadrDistributeEvent", vtn_id=vtn.vtn_id, request_id=new_id(),
               events=[event.to_openadr(ven.ven_id)])
    except Exception as problem:
        raise InvalidRequest(f"the event cannot be sent: {problem}") from None


# -- Routes -----------------------------------------------------------------

class ControlApi:
    def __init__(self, vtn: Vtn):
        self.vtn = vtn

    def routes(self) -> list[web.RouteDef]:
        return [
            web.get("/", self.page),
            web.get("/healthz", self.health),
            web.get("/api/vtn", self.describe_vtn),
            web.get("/api/vens", self.list_vens),
            web.get("/api/vens/{ven}", self.get_ven),
            web.delete("/api/vens/{ven}", self.delete_ven),
            web.post("/api/vens/{ven}/registration/cancel", self.cancel_registration),
            web.post("/api/vens/{ven}/registration/reregister", self.request_reregistration),
            web.get("/api/vens/{ven}/events", self.list_events),
            web.post("/api/vens/{ven}/events", self.create_event),
            web.get("/api/vens/{ven}/events/{event_id}", self.get_event),
            web.patch("/api/vens/{ven}/events/{event_id}", self.modify_event),
            web.post("/api/vens/{ven}/events/{event_id}/cancel", self.cancel_event),
            web.get("/api/vens/{ven}/reports", self.get_reports),
            web.post("/api/vens/{ven}/reports/request", self.request_report),
            web.post("/api/vens/{ven}/reports/{request_id}/cancel", self.cancel_report),
            web.patch("/api/vtn", self.configure_vtn),
            web.get("/api/vens/{ven}/messages", self.list_messages),
            web.get("/api/messages/unattributed", self.list_unattributed),
        ]

    def ven(self, request: web.Request) -> Ven:
        return self.vtn.get(request.match_info["ven"])

    def event(self, request: web.Request) -> tuple[Ven, Event]:
        ven = self.ven(request)
        event_id = request.match_info["event_id"]
        if event_id not in ven.events:
            raise KeyError(event_id)
        return ven, ven.events[event_id]

    # -- Handlers -----------------------------------------------------------

    async def page(self, request: web.Request) -> web.Response:
        html = resources.files("test_vtn").joinpath("static/index.html").read_text()
        return web.Response(text=html, content_type="text/html")

    async def health(self, request: web.Request) -> web.Response:
        return json_response({"ok": True})

    async def describe_vtn(self, request: web.Request) -> web.Response:
        return json_response({
            "vtn_id": self.vtn.vtn_id,
            "market_context": self.vtn.market_context,
            "poll_interval_seconds": self.vtn.poll_interval.total_seconds(),
            "report_interval_seconds": self.vtn.report_interval.total_seconds(),
            "auto_report_requests": self.vtn.auto_report_requests,
            "ven_name_prefix": self.vtn.ven_name_prefix,
            "openadr_path": "/OpenADR2/Simple/2.0b",
            "per_ven_path": "/v/{ven_name}/OpenADR2/Simple/2.0b",
        })

    async def list_vens(self, request: web.Request) -> web.Response:
        return json_response([v.summary() for v in self.vtn.vens.values()])

    async def get_ven(self, request: web.Request) -> web.Response:
        return json_response(self.ven(request).to_json())

    async def delete_ven(self, request: web.Request) -> web.Response:
        self.vtn.remove(request.match_info["ven"])
        return web.Response(status=204)

    async def cancel_registration(self, request: web.Request) -> web.Response:
        ven = self.ven(request)
        if not ven.registered:
            raise InvalidRequest(f"VEN {ven.ven_id} is not registered")
        ven.outbox.append(("oadrCancelPartyRegistration", {}))
        return json_response(ven.summary(), status=202)

    async def request_reregistration(self, request: web.Request) -> web.Response:
        ven = self.ven(request)
        if not ven.registered:
            raise InvalidRequest(f"VEN {ven.ven_id} is not registered")
        ven.outbox.append(("oadrRequestReregistration", {}))
        return json_response(ven.summary(), status=202)

    async def list_events(self, request: web.Request) -> web.Response:
        return json_response([e.to_json() for e in self.ven(request).events.values()])

    async def get_event(self, request: web.Request) -> web.Response:
        return json_response(self.event(request)[1].to_json())

    async def create_event(self, request: web.Request) -> web.Response:
        ven = self.ven(request)
        body = await read_body(request)
        start = parse_start(body, now().replace(microsecond=0)
                            + timedelta(seconds=DEFAULT_START_IN_SECONDS))
        signals = parse_signals(body) or parse_signals({"intervals": DEFAULT_INTERVALS})
        event = Event(event_id=typed_field(body, "event_id", str) or new_id(), start=start,
                      signals=signals, market_context=self.vtn.market_context)
        apply_options(event, body)
        check_renderable(event, ven, self.vtn)
        self.vtn.add_event(ven, event)
        log.info("created event %s for VEN %s", event.event_id, ven.ven_id)
        return json_response(event.to_json(), status=201)

    async def modify_event(self, request: web.Request) -> web.Response:
        ven, event = self.event(request)
        if event.cancelled:
            raise InvalidRequest(f"event {event.event_id} is cancelled")
        body = await read_body(request)
        if "event_id" in body:
            raise InvalidRequest("an event's id cannot change")
        changed = copy.deepcopy(event)
        changed.start = parse_start(body, event.start)
        changed.signals = parse_signals(body) or event.signals
        apply_options(changed, body)
        check_renderable(changed, ven, self.vtn)
        for name in ("start", "signals", "market_context", "response_required", "test_event",
                     "priority", "start_after_seconds", "comment"):
            setattr(event, name, getattr(changed, name))
        self.vtn.modify_event(ven, event)
        log.info("modified event %s for VEN %s", event.event_id, ven.ven_id)
        return json_response(event.to_json())

    async def cancel_event(self, request: web.Request) -> web.Response:
        ven, event = self.event(request)
        self.vtn.cancel_event(ven, event)
        log.info("cancelled event %s for VEN %s", event.event_id, ven.ven_id)
        return json_response(event.to_json())

    async def request_report(self, request: web.Request) -> web.Response:
        """Queues an oadrCreateReport for the VEN's next poll.

        {"report_specifier_id", "r_ids": [...] (default: every offered rID),
         "granularity_seconds": 60, "report_back_seconds": 60 (0 for one
         shot or history), "start_in_seconds": 0 (negative for history),
         "duration_seconds": 0 (until cancelled), "one_shot": false (no
         reportInterval at all)}
        """
        ven = self.ven(request)
        if not ven.registered:
            raise InvalidRequest(f"VEN {ven.ven_id} is not registered")
        body = await read_body(request)
        specifier_id = typed_field(body, "report_specifier_id", str)
        if not specifier_id:
            raise InvalidRequest("report_specifier_id is required")
        offered = ven.offered_reports.get(specifier_id)
        r_ids = typed_field(body, "r_ids", list) or (offered or {}).get("r_ids") or []
        if not r_ids:
            raise InvalidRequest(f"no rIDs known for {specifier_id}; pass r_ids")
        default = self.vtn.report_interval.total_seconds()
        granularity = typed_field(body, "granularity_seconds", (int, float))
        report_back = typed_field(body, "report_back_seconds", (int, float))
        start_in = typed_field(body, "start_in_seconds", (int, float)) or 0
        duration = typed_field(body, "duration_seconds", (int, float)) or 0
        one_shot = bool(typed_field(body, "one_shot", bool))
        report_request = OpenadrEndpoints(self.vtn).report_request(
            ven, specifier_id, [str(r) for r in r_ids],
            granularity=timedelta(seconds=default if granularity is None else granularity),
            report_back=timedelta(seconds=default if report_back is None else report_back),
            start=now().replace(microsecond=0) + timedelta(seconds=start_in),
            duration=timedelta(seconds=duration), one_shot=one_shot)
        ven.outbox.append(("oadrCreateReport", {"report_requests": [report_request]}))
        log.info("queued report request %s for VEN %s", report_request.report_request_id,
                 ven.ven_id)
        return json_response(ven.report_requests[report_request.report_request_id],
                             status=202)

    async def cancel_report(self, request: web.Request) -> web.Response:
        ven = self.ven(request)
        request_id = request.match_info["request_id"]
        record = ven.report_requests.get(request_id)
        if record is None:
            raise web.HTTPNotFound(text=f"no report request {request_id}")
        record["cancelled"] = True
        ven.outbox.append(("oadrCancelReport",
                           {"report_request_id": request_id, "report_to_follow": False}))
        return json_response(record, status=202)

    async def configure_vtn(self, request: web.Request) -> web.Response:
        body = await read_body(request)
        auto = typed_field(body, "auto_report_requests", bool)
        if auto is not None:
            self.vtn.auto_report_requests = auto
        return await self.describe_vtn(request)

    async def get_reports(self, request: web.Request) -> web.Response:
        ven = self.ven(request)
        return json_response({
            "offered": list(ven.offered_reports.values()),
            "requests": list(ven.report_requests.values()),
            "answers": ven.report_answers,
            "updates": list(ven.report_updates),
        })

    async def list_messages(self, request: web.Request) -> web.Response:
        return json_response(self.filter_messages(self.ven(request).messages, request))

    async def list_unattributed(self, request: web.Request) -> web.Response:
        return json_response(self.filter_messages(self.vtn.unattributed, request))

    @staticmethod
    def filter_messages(messages, request: web.Request) -> list[dict[str, Any]]:
        """?type= keeps one message type, ?after= only later entries, and
        ?xml=1 includes the raw XML."""
        wanted = request.query.get("type")
        try:
            after = int(request.query.get("after", "0"))
        except ValueError:
            raise InvalidRequest("after must be a sequence number") from None
        with_xml = request.query.get("xml") in ("1", "true")
        selected = []
        for message in messages:
            if message["seq"] <= after or wanted and message.get("type") != wanted:
                continue
            if not with_xml:
                message = {k: v for k, v in message.items() if not k.endswith("_xml")}
            selected.append(message)
        return selected
