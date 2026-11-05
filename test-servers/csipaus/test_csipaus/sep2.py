"""The IEEE 2030.5 side: the resources a CSIP-AUS client reads and writes.

envoy-schema supplies the documents. Its 1.x line writes the CSIP-AUS
extensions in https://csipaus.org/ns, the namespace of v1.1a and v1.2 that
the worker's bindings read. Every resource lives under

    /d/{client}/

and every href the server hands out keeps that prefix, so a client given
`/d/{client}/dcap` as its server URL identifies itself by URL, as the test
VTN's `/v/{ven name}` does, with or without client certificates.

The tree, for each end device a client registers:

    dcap                       DeviceCapability
    tm                         Time
    edev                       EndDeviceList; POST registers an EndDevice
    edev/{e}                   EndDevice
    edev/{e}/rg                Registration, with the PIN
    edev/{e}/cp                csipaus:ConnectionPoint; PUT sets it
    edev/{e}/fsa[/1]           FunctionSetAssignments
    edev/{e}/fsa/1/derp        DERProgramList with the one program
    edev/{e}/derp/1            DERProgram
    edev/{e}/derp/1/dderc      DefaultDERControl, once one is set
    edev/{e}/derp/1/derc[/{c}] DERControlList and DERControl
    edev/{e}/derp/1/actderc    the active DERControls
    edev/{e}/der[/1]           DERList and the one DER
    edev/{e}/der/1/{r}         dercap, derg, ders, dera; PUT writes them
    rsps/1/rsp                 POST takes a Response to a DERControl
    mup[/{m}]                  MirrorUsagePoints; POST creates one, or
                               posts MirrorMeterReadings to one
"""

from __future__ import annotations

import logging
import re
from datetime import datetime, timedelta, timezone
from functools import cache
from typing import Any, Awaitable, Callable
from zoneinfo import ZoneInfo

from aiohttp import web
from envoy_schema.server.schema.csip_aus.connection_point import (
    ConnectionPointLink,
    ConnectionPointRequest,
    ConnectionPointResponse,
)
from envoy_schema.server.schema.sep2.der import (
    DER,
    ActivePower,
    DefaultDERControl,
    DERAvailability,
    DERCapability,
    DERControlBase,
    DERControlListResponse,
    DERControlResponse,
    DERListResponse,
    DERProgramListResponse,
    DERProgramResponse,
    DERSettings,
    DERStatus,
)
from envoy_schema.server.schema.sep2.device_capability import DeviceCapabilityResponse
from envoy_schema.server.schema.sep2.end_device import (
    EndDeviceListResponse,
    EndDeviceRequest,
    EndDeviceResponse,
    RegistrationResponse,
)
from envoy_schema.server.schema.sep2.event import EventStatus
from envoy_schema.server.schema.sep2.function_set_assignments import (
    FunctionSetAssignmentsListResponse,
    FunctionSetAssignmentsResponse,
)
from envoy_schema.server.schema.sep2.identification import Link, ListLink
from envoy_schema.server.schema.sep2.metering_mirror import (
    MirrorMeterReading,
    MirrorMeterReadingListRequest,
    MirrorUsagePointListResponse,
    MirrorUsagePointRequest,
)
from envoy_schema.server.schema.sep2.metering_mirror import (
    MirrorUsagePoint as MirrorUsagePointDocument,
)
from envoy_schema.server.schema.sep2.response import Response, ResponseType
from envoy_schema.server.schema.sep2.time import TimeResponse
from envoy_schema.server.schema.sep2.types import DateTimeIntervalType, TimeQualityType
from lxml import etree
from pydantic import ValidationError
from pydantic_xml import BaseXmlModel

from test_csipaus.state import (
    DER_RESOURCES,
    LIMITS,
    Client,
    Control,
    EndDevice,
    MirrorUsagePoint,
    Server,
    now,
)

log = logging.getLogger("test_csipaus.sep2")

CONTENT_TYPE = "application/sep+xml"
SEP_NS = "urn:ieee:std:2030.5:ns"
# EventStatus currentStatus codes.
EVENT_STATUS = {"scheduled": 0, "active": 1, "cancelled": 2}
RESPONSE_STATUS = {t.value: t.name.lower() for t in ResponseType}
DER_DOCUMENTS: dict[str, type[BaseXmlModel]] = {
    "dercap": DERCapability, "derg": DERSettings, "ders": DERStatus, "dera": DERAvailability,
}


class Refusal(Exception):
    """Refuses a request with an HTTP status and a reason for the log."""

    def __init__(self, status: int, reason: str):
        super().__init__(reason)
        self.status = status
        self.reason = reason


def epoch(at: datetime) -> int:
    return int(at.timestamp())


def active_power(watts: float) -> ActivePower:
    """Watts as 2030.5 writes them: an Int16 value and a power of ten."""
    multiplier = 0
    value = round(watts)
    while abs(value) > 32767:
        multiplier += 1
        value = round(watts / 10 ** multiplier)
    return ActivePower(value=value, multiplier=multiplier)


def control_base(base: dict[str, Any]) -> DERControlBase:
    fields: dict[str, Any] = {}
    for name, value in base.items():
        fields[name] = active_power(value) if name in LIMITS else value
    return DERControlBase(**fields)


def root_tag(xml: bytes) -> str | None:
    try:
        return etree.QName(etree.fromstring(xml)).localname
    except (etree.XMLSyntaxError, ValueError):
        return None


def element_names(data: Any) -> Any:
    """A dumped document keyed by element name: envoy-schema writes `type`
    as `type_`, and so on."""
    if isinstance(data, dict):
        return {k.removesuffix("_"): element_names(v) for k, v in data.items()}
    if isinstance(data, list):
        return [element_names(v) for v in data]
    return data


@cache
def dst_rules(zone: str, year: int) -> tuple[int, int, int, int]:
    """(tzOffset, dstOffset, dstStartTime, dstEndTime) for a year: the
    standard offset, what daylight saving adds, and when it starts and ends,
    found by walking the year an hour at a time."""
    tz = ZoneInfo(zone)
    at = datetime(year, 1, 1, tzinfo=timezone.utc)
    end = datetime(year + 1, 1, 1, tzinfo=timezone.utc)
    standard = dst = None
    start_time = end_time = 0
    previous = at.astimezone(tz).dst()
    while at < end:
        local = at.astimezone(tz)
        if local.dst():
            dst = local.dst()
        else:
            standard = local.utcoffset()
        if local.dst() != previous:
            if local.dst():
                start_time = epoch(at)
            else:
                end_time = epoch(at)
            previous = local.dst()
        at += timedelta(hours=1)
    standard = standard if standard is not None else at.astimezone(tz).utcoffset()
    return (int(standard.total_seconds()), int(dst.total_seconds()) if dst else 0,
            start_time, end_time)


class Sep2Endpoints:
    def __init__(self, server: Server):
        self.server = server
        self.handlers: list[tuple[str, re.Pattern, Callable[..., Awaitable[Any]]]] = []
        for methods, pattern, handler in [
            ("GET", r"dcap", self.device_capability),
            ("GET", r"tm", self.time),
            ("GET", r"edev", self.end_device_list),
            ("POST", r"edev", self.register),
            ("GET", r"edev/(?P<e>\d+)", self.end_device),
            ("GET", r"edev/(?P<e>\d+)/rg", self.registration),
            ("GET", r"edev/(?P<e>\d+)/cp", self.connection_point),
            ("PUT POST", r"edev/(?P<e>\d+)/cp", self.set_connection_point),
            ("GET", r"edev/(?P<e>\d+)/fsa", self.fsa_list),
            ("GET", r"edev/(?P<e>\d+)/fsa/1", self.fsa),
            ("GET", r"edev/(?P<e>\d+)/fsa/1/derp", self.program_list),
            ("GET", r"edev/(?P<e>\d+)/derp/1", self.program),
            ("GET", r"edev/(?P<e>\d+)/derp/1/dderc", self.default_control),
            ("GET", r"edev/(?P<e>\d+)/derp/1/derc", self.control_list),
            ("GET", r"edev/(?P<e>\d+)/derp/1/derc/(?P<c>\d+)", self.control),
            ("GET", r"edev/(?P<e>\d+)/derp/1/actderc", self.active_control_list),
            ("GET", r"edev/(?P<e>\d+)/der", self.der_list),
            ("GET", r"edev/(?P<e>\d+)/der/1", self.der),
            ("GET", r"edev/(?P<e>\d+)/der/1/(?P<r>dercap|derg|ders|dera)", self.der_resource),
            ("PUT POST", r"edev/(?P<e>\d+)/der/1/(?P<r>dercap|derg|ders|dera)",
             self.write_der_resource),
            ("POST", r"rsps/1/rsp", self.respond),
            ("GET", r"mup", self.mup_list),
            ("POST", r"mup", self.create_mup),
            ("GET", r"mup/(?P<m>\d+)", self.mup),
            ("POST", r"mup/(?P<m>\d+)", self.post_readings),
            ("DELETE", r"mup/(?P<m>\d+)", self.delete_mup),
        ]:
            for method in methods.split():
                self.handlers.append((method, re.compile(pattern), handler))

    def routes(self) -> list[web.RouteDef]:
        return [web.route("*", "/d/{client}/{path:.*}", self.handle)]

    # -- Dispatch -----------------------------------------------------------

    async def handle(self, request: web.Request) -> web.Response:
        name = request.match_info["client"]
        path = request.match_info["path"].strip("/")
        body = await request.read()
        try:
            client = self.server.client(name)
        except PermissionError as problem:
            self.server.record(None, self.entry(request, path, body, 403, str(problem)))
            return web.Response(status=403, text=str(problem))

        status, document, headers, reason = 404, None, {}, f"no resource at {path}"
        matched = [(m, p.fullmatch(path), h) for m, p, h in self.handlers]
        matched = [(m, match, h) for m, match, h in matched if match]
        handler = next(((match, h) for m, match, h in matched if m == request.method), None)
        if matched and handler is None:
            status, reason = 405, f"{request.method} is not allowed on {path}"
            headers = {"Allow": ", ".join(sorted({m for m, _, _ in matched}))}
        elif handler is not None:
            match, function = handler
            try:
                status, document, headers = await function(
                    client, body=body, query=request.query, **match.groupdict())
                reason = None
            except Refusal as refusal:
                status, reason = refusal.status, refusal.reason
            except KeyError as missing:
                status, reason = 404, f"not found: {missing.args[0]}"
            except (ValidationError, etree.XMLSyntaxError, ValueError) as problem:
                status, reason = 400, f"cannot read the {root_tag(body) or 'body'}: {problem}"

        xml = document.to_xml(skip_empty=True) if isinstance(document, BaseXmlModel) \
            else document
        self.server.record(client, self.entry(request, path, body, status, reason, xml,
                                              headers.get("Location")))
        if reason:
            log.info("%s %s from %s: %s %s", request.method, path, name, status, reason)
        if xml is not None:
            return web.Response(status=status, body=xml, content_type=CONTENT_TYPE,
                                headers=headers)
        return web.Response(status=status, headers=headers,
                            text=reason if status >= 400 else None)

    @staticmethod
    def entry(request: web.Request, path: str, body: bytes, status: int,
              reason: str | None, xml: bytes | None = None,
              location: str | None = None) -> dict[str, Any]:
        return {
            "method": request.method, "path": path, "status": status,
            "request_type": root_tag(body) if body else None,
            "response_type": root_tag(xml) if xml else None,
            "location": location, "error": reason,
            "request_xml": body.decode(errors="replace") if body else None,
            "response_xml": xml.decode(errors="replace") if xml else None,
        }

    # -- Helpers ------------------------------------------------------------

    @staticmethod
    def href(client: Client, path: str) -> str:
        return f"/d/{client.name}/{path}"

    @staticmethod
    def page(items: list, query) -> tuple[list, int]:
        """2030.5 paging: `s` is the first index, `l` how many."""
        try:
            start = int(query.get("s", 0))
            limit = int(query.get("l", 255))
        except ValueError:
            raise Refusal(400, "s and l must be integers") from None
        return items[start:start + max(limit, 0)], len(items)

    @staticmethod
    def device(client: Client, e: str) -> EndDevice:
        device = client.end_devices.get(int(e))
        if device is None:
            raise KeyError(f"edev/{e}")
        return device

    @staticmethod
    def created(href: str, status: int = 201) -> tuple[int, None, dict[str, str]]:
        return status, None, {"Location": href}

    def end_device_document(self, client: Client, device: EndDevice) -> EndDeviceResponse:
        base = f"edev/{device.id}"
        return EndDeviceResponse(
            href=self.href(client, base),
            lFDI=device.lfdi, sFDI=device.sfdi, changedTime=device.changed_time,
            enabled=device.enabled, deviceCategory=device.device_category,
            postRate=device.post_rate,
            DERListLink=ListLink(href=self.href(client, f"{base}/der"), all_=1),
            FunctionSetAssignmentsListLink=ListLink(href=self.href(client, f"{base}/fsa"),
                                                    all_=1),
            RegistrationLink=Link(href=self.href(client, f"{base}/rg")),
            ConnectionPointLink=ConnectionPointLink(href=self.href(client, f"{base}/cp")),
        )

    def control_document(self, client: Client, device: EndDevice,
                         control: Control) -> DERControlResponse:
        at = now()
        status = control.status(at)
        changed = (control.cancelled_at if status == "cancelled"
                   else control.start if status == "active" else control.created_at)
        return DERControlResponse(
            href=self.href(client, f"edev/{device.id}/derp/1/derc/{control.id}"),
            mRID=control.mrid, description=control.description, version=0,
            replyTo=self.href(client, "rsps/1/rsp"),
            responseRequired=f"{control.response_required:02X}",
            creationTime=epoch(control.created_at),
            EventStatus_=EventStatus(currentStatus=EVENT_STATUS[status],
                                     dateTime=epoch(changed), potentiallySuperseded=False),
            interval=DateTimeIntervalType(start=epoch(control.start),
                                          duration=control.duration_seconds),
            randomizeStart=control.randomize_start_seconds,
            randomizeDuration=control.randomize_duration_seconds,
            DERControlBase_=control_base(control.base),
        )

    # -- Discovery and registration -----------------------------------------

    async def device_capability(self, client: Client, **_):
        return 200, DeviceCapabilityResponse(
            href=self.href(client, "dcap"), pollRate=self.server.poll_rate_seconds,
            TimeLink=Link(href=self.href(client, "tm")),
            EndDeviceListLink=ListLink(href=self.href(client, "edev"),
                                       all_=len(client.end_devices)),
            MirrorUsagePointListLink=ListLink(href=self.href(client, "mup"),
                                              all_=len(client.mirror_usage_points)),
        ), {}

    async def time(self, client: Client, **_):
        at = now()
        tz_offset, dst_offset, dst_start, dst_end = dst_rules(self.server.time_zone, at.year)
        local = at.astimezone(ZoneInfo(self.server.time_zone))
        return 200, TimeResponse(
            href=self.href(client, "tm"), currentTime=epoch(at),
            dstEndTime=dst_end, dstOffset=dst_offset, dstStartTime=dst_start,
            localTime=epoch(at) + int(local.utcoffset().total_seconds()),
            quality=TimeQualityType.level_3_source, tzOffset=tz_offset,
        ), {}

    async def end_device_list(self, client: Client, query, **_):
        devices, total = self.page(list(client.end_devices.values()), query)
        return 200, EndDeviceListResponse(
            href=self.href(client, "edev"), all_=total, results=len(devices),
            pollRate=self.server.poll_rate_seconds,
            EndDevice=[self.end_device_document(client, d) for d in devices],
        ), {}

    async def register(self, client: Client, body: bytes, **_):
        document = EndDeviceRequest.from_xml(body)
        if not document.lFDI:
            raise Refusal(400, "CSIP-AUS needs the EndDevice's lFDI")
        if not re.fullmatch(r"[0-9A-Fa-f]{40}", document.lFDI):
            raise Refusal(400, f"lFDI {document.lFDI} is not 40 hex digits")
        device = client.find_device(document.lFDI, document.sFDI)
        if device is None:
            device = client.add_device(lfdi=document.lFDI.upper(), sfdi=document.sFDI,
                                       pin=self.server.pin,
                                       changed_time=document.changedTime)
            log.info("client %s registered end device %s (lFDI %s)", client.name,
                     device.id, device.lfdi)
        device.changed_time = document.changedTime
        device.enabled = document.enabled if document.enabled is not None else True
        device.device_category = document.deviceCategory
        device.post_rate = document.postRate
        device.updated_at = now()
        return self.created(self.href(client, f"edev/{device.id}"))

    async def end_device(self, client: Client, e: str, **_):
        return 200, self.end_device_document(client, self.device(client, e)), {}

    async def registration(self, client: Client, e: str, **_):
        device = self.device(client, e)
        return 200, RegistrationResponse(
            href=self.href(client, f"edev/{device.id}/rg"),
            pollRate=self.server.poll_rate_seconds,
            dateTimeRegistered=epoch(device.registered_at), pIN=device.pin,
        ), {}

    async def connection_point(self, client: Client, e: str, **_):
        device = self.device(client, e)
        if device.connection_point_id is None:
            raise Refusal(404, "the connection point has not been set")
        return 200, ConnectionPointResponse(href=self.href(client, f"edev/{device.id}/cp"),
                                            id=device.connection_point_id), {}

    async def set_connection_point(self, client: Client, e: str, body: bytes, **_):
        device = self.device(client, e)
        document = ConnectionPointRequest.from_xml(body)
        # envoy-schema also reads an <id>, which some clients write.
        point = document.id or document.id_v11
        if not point:
            raise Refusal(400, "the ConnectionPoint has no connectionPointId")
        device.connection_point_id = point
        return 204, None, {}

    # -- Controls -----------------------------------------------------------

    async def fsa_list(self, client: Client, e: str, query, **_):
        device = self.device(client, e)
        fsas, total = self.page([self.fsa_document(client, device)], query)
        return 200, FunctionSetAssignmentsListResponse(
            href=self.href(client, f"edev/{device.id}/fsa"), all_=total, results=len(fsas),
            pollRate=self.server.poll_rate_seconds, FunctionSetAssignments=fsas,
        ), {}

    async def fsa(self, client: Client, e: str, **_):
        return 200, self.fsa_document(client, self.device(client, e)), {}

    def fsa_document(self, client: Client, device: EndDevice) -> FunctionSetAssignmentsResponse:
        return FunctionSetAssignmentsResponse(
            href=self.href(client, f"edev/{device.id}/fsa/1"), mRID=device.fsa_mrid,
            description="Test server assignments", version=0,
            DERProgramListLink=ListLink(href=self.href(client, f"edev/{device.id}/fsa/1/derp"),
                                        all_=1),
            TimeLink=Link(href=self.href(client, "tm")),
        )

    async def program_list(self, client: Client, e: str, query, **_):
        device = self.device(client, e)
        programs, total = self.page([self.program_document(client, device)], query)
        return 200, DERProgramListResponse(
            href=self.href(client, f"edev/{device.id}/fsa/1/derp"), all_=total,
            results=len(programs), pollRate=self.server.poll_rate_seconds,
            DERProgram=programs,
        ), {}

    async def program(self, client: Client, e: str, **_):
        return 200, self.program_document(client, self.device(client, e)), {}

    def program_document(self, client: Client, device: EndDevice) -> DERProgramResponse:
        base = f"edev/{device.id}/derp/1"
        at = now()
        return DERProgramResponse(
            href=self.href(client, base), mRID=device.program_mrid,
            description="Test server program", version=0, primacy=device.primacy,
            DefaultDERControlLink=Link(href=self.href(client, f"{base}/dderc"))
            if device.default_control else None,
            DERControlListLink=ListLink(href=self.href(client, f"{base}/derc"),
                                        all_=len(device.listed_controls(at))),
            ActiveDERControlListLink=ListLink(href=self.href(client, f"{base}/actderc"),
                                              all_=len(device.active_controls(at))),
        )

    async def default_control(self, client: Client, e: str, **_):
        device = self.device(client, e)
        default = device.default_control
        if default is None:
            raise Refusal(404, "no default control has been set")
        return 200, DefaultDERControl(
            href=self.href(client, f"edev/{device.id}/derp/1/dderc"), mRID=default.mrid,
            description="Test server default", version=default.version,
            DERControlBase_=control_base(default.base),
        ), {}

    async def control_list(self, client: Client, e: str, query, **_):
        device = self.device(client, e)
        return self.controls(client, device, query, device.listed_controls(), "derc")

    async def active_control_list(self, client: Client, e: str, query, **_):
        device = self.device(client, e)
        return self.controls(client, device, query, device.active_controls(), "actderc")

    def controls(self, client: Client, device: EndDevice, query, controls: list[Control],
                 name: str):
        controls, total = self.page(controls, query)
        return 200, DERControlListResponse(
            href=self.href(client, f"edev/{device.id}/derp/1/{name}"), all_=total,
            results=len(controls),
            DERControl=[self.control_document(client, device, c) for c in controls],
        ), {}

    async def control(self, client: Client, e: str, c: str, **_):
        device = self.device(client, e)
        control = device.controls.get(int(c))
        if control is None:
            raise KeyError(f"derc/{c}")
        return 200, self.control_document(client, device, control), {}

    async def respond(self, client: Client, body: bytes, **_):
        """A Response, or the DERControlResponse that CSIP-AUS sends, to a
        DERControl."""
        tag = root_tag(body)
        if tag not in ("Response", "DERControlResponse"):
            raise Refusal(400, f"expected a DERControlResponse, not {tag}")
        root = etree.fromstring(body)
        root.tag = f"{{{SEP_NS}}}Response"
        document = Response.from_xml(etree.tostring(root))
        subject = document.subject.upper()
        for device in client.end_devices.values():
            control = next((c for c in device.controls.values() if c.mrid == subject), None)
            if control is not None:
                break
        else:
            raise Refusal(400, f"no DERControl has the mRID {subject}")
        status = int(document.status) if document.status is not None else None
        device.responses.append({
            "at": now(), "subject": subject, "control_id": control.id,
            "status": status, "status_name": RESPONSE_STATUS.get(status),
            "end_device_lfdi": document.endDeviceLFDI,
            "created": datetime.fromtimestamp(document.createdDateTime, timezone.utc)
            if document.createdDateTime is not None else None,
        })
        return self.created(self.href(client, f"rsps/1/rsp/{len(device.responses)}"))

    # -- DER ----------------------------------------------------------------

    async def der_list(self, client: Client, e: str, query, **_):
        device = self.device(client, e)
        ders, total = self.page([self.der_document(client, device)], query)
        return 200, DERListResponse(
            href=self.href(client, f"edev/{device.id}/der"), all_=total, results=len(ders),
            pollRate=self.server.poll_rate_seconds, DER_=ders,
        ), {}

    async def der(self, client: Client, e: str, **_):
        return 200, self.der_document(client, self.device(client, e)), {}

    def der_document(self, client: Client, device: EndDevice) -> DER:
        base = f"edev/{device.id}/der/1"
        return DER(
            href=self.href(client, base),
            AssociatedDERProgramListLink=ListLink(
                href=self.href(client, f"edev/{device.id}/fsa/1/derp"), all_=1),
            CurrentDERProgramLink=Link(href=self.href(client, f"edev/{device.id}/derp/1")),
            DERAvailabilityLink=Link(href=self.href(client, f"{base}/dera")),
            DERCapabilityLink=Link(href=self.href(client, f"{base}/dercap")),
            DERSettingsLink=Link(href=self.href(client, f"{base}/derg")),
            DERStatusLink=Link(href=self.href(client, f"{base}/ders")),
        )

    async def der_resource(self, client: Client, e: str, r: str, **_):
        device = self.device(client, e)
        entry = device.der.get(r)
        if entry is None:
            raise Refusal(404, f"the client has not written its {DER_RESOURCES[r]}")
        return 200, entry["xml"].encode(), {}

    async def write_der_resource(self, client: Client, e: str, r: str, body: bytes, **_):
        device = self.device(client, e)
        tag = root_tag(body)
        if tag != DER_RESOURCES[r]:
            raise Refusal(400, f"{r} takes a {DER_RESOURCES[r]}, not {tag}")
        document = DER_DOCUMENTS[r].from_xml(body)
        device.der[r] = {"at": now(), "data": element_names(
            document.model_dump(mode="json", exclude_none=True)), "xml": body.decode()}
        return 204, None, {}

    # -- Metering mirror ----------------------------------------------------

    def mup_document(self, client: Client, mup: MirrorUsagePoint) -> MirrorUsagePointDocument:
        return MirrorUsagePointDocument(
            href=self.href(client, f"mup/{mup.id}"), mRID=mup.mrid,
            description=mup.description, version=0, roleFlags=mup.role_flags,
            serviceCategoryKind=mup.service_category_kind, status=mup.status,
            deviceLFDI=mup.device_lfdi, postRate=mup.post_rate,
        )

    async def mup_list(self, client: Client, query, **_):
        mups, total = self.page(list(client.mirror_usage_points.values()), query)
        return 200, MirrorUsagePointListResponse(
            href=self.href(client, "mup"), all_=total, results=len(mups),
            pollRate=self.server.poll_rate_seconds,
            mirrorUsagePoints=[self.mup_document(client, m) for m in mups],
        ), {}

    async def create_mup(self, client: Client, body: bytes, **_):
        document = MirrorUsagePointRequest.from_xml(body)
        mup = client.find_mup(document.mRID)
        if mup is None:
            # The server sets the post rate, as a utility's does.
            mup = client.add_mup(
                mrid=document.mRID.upper(), device_lfdi=document.deviceLFDI.upper(),
                description=document.description, role_flags=document.roleFlags,
                service_category_kind=int(document.serviceCategoryKind),
                status=document.status, post_rate=self.server.post_rate_seconds)
        self.record_readings(mup, document.mirrorMeterReadings or [])
        return self.created(self.href(client, f"mup/{mup.id}"))

    async def mup(self, client: Client, m: str, **_):
        mup = client.mirror_usage_points.get(int(m))
        if mup is None:
            raise KeyError(f"mup/{m}")
        return 200, self.mup_document(client, mup), {}

    async def post_readings(self, client: Client, m: str, body: bytes, **_):
        mup = client.mirror_usage_points.get(int(m))
        if mup is None:
            raise KeyError(f"mup/{m}")
        tag = root_tag(body)
        if tag == "MirrorMeterReading":
            readings = [MirrorMeterReading.from_xml(body)]
        elif tag == "MirrorMeterReadingList":
            readings = MirrorMeterReadingListRequest.from_xml(body).mirrorMeterReadings or []
        else:
            raise Refusal(400, f"expected MirrorMeterReading or MirrorMeterReadingList, "
                               f"not {tag}")
        self.record_readings(mup, readings)
        return self.created(self.href(client, f"mup/{mup.id}"))

    async def delete_mup(self, client: Client, m: str, **_):
        if client.mirror_usage_points.pop(int(m), None) is None:
            raise KeyError(f"mup/{m}")
        return 204, None, {}

    @staticmethod
    def record_readings(mup: MirrorUsagePoint, readings: list[MirrorMeterReading]) -> None:
        at = now()
        for reading in readings:
            known = mup.meter_readings.setdefault(reading.mRID.upper(), {
                "mrid": reading.mRID.upper(), "description": reading.description,
                "reading_type": None, "latest": None, "at": None})
            if reading.description:
                known["description"] = reading.description
            if reading.readingType is not None:
                known["reading_type"] = reading.readingType.model_dump(
                    mode="json", exclude_none=True)
            values = []
            if reading.reading is not None:
                values.append(reading.reading)
            for reading_set in reading.mirrorReadingSets or []:
                values.extend(reading_set.readings or [])
            for value in values:
                entry = {
                    "value": value.value,
                    "start": value.timePeriod.start if value.timePeriod else None,
                    "duration": value.timePeriod.duration if value.timePeriod else None,
                    "quality_flags": value.qualityFlags,
                }
                mup.posts.append({"at": at, "mrid": known["mrid"], **entry})
                known["latest"], known["at"] = entry, at
