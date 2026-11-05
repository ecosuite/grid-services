"""The server's state, kept in memory.

A client is whoever calls under `/d/{client}/`: one per certificate in a
real deployment, so a site's own device or an aggregator speaking for many
sites. Everything hangs off its `Client`: the end devices it registered,
each with its program, default control, controls, DER resources and
responses, its mirror usage points, and a log of every exchange. Nothing
survives a restart; a client that polls a restarted server finds its end
devices gone and registers them again.
"""

from __future__ import annotations

import itertools
import uuid
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from typing import Any

from test_common.control import InvalidRequest

# How much history each client keeps.
MESSAGE_LOG_SIZE = 500
RESPONSE_LOG_SIZE = 500
READING_LOG_SIZE = 500

# The DERControlBase fields the control API sets, all in watts but the
# two switches. CSIP-AUS adds the four limits.
LIMITS = ("opModExpLimW", "opModImpLimW", "opModGenLimW", "opModLoadLimW")
SWITCHES = ("opModConnect", "opModEnergize")

# DER resources a client writes, by the name that ends their path.
DER_RESOURCES = {
    "dercap": "DERCapability",
    "derg": "DERSettings",
    "ders": "DERStatus",
    "dera": "DERAvailability",
}


def now() -> datetime:
    return datetime.now(timezone.utc)


def new_mrid() -> str:
    return uuid.uuid4().hex.upper()


def parse_base(body: Any, what: str) -> dict[str, Any]:
    """A DERControlBase from the control API: limits in watts and the two
    switches, by their 2030.5 names."""
    if not isinstance(body, dict):
        raise InvalidRequest(f"{what} must be an object")
    base: dict[str, Any] = {}
    for name, value in body.items():
        if name in LIMITS:
            if isinstance(value, bool) or not isinstance(value, (int, float)) or value < 0:
                raise InvalidRequest(f"{name} must be a number of watts, zero or more")
            base[name] = value
        elif name in SWITCHES:
            if not isinstance(value, bool):
                raise InvalidRequest(f"{name} must be true or false")
            base[name] = value
        elif value is not None:
            raise InvalidRequest(f"{what} cannot set {name}; it takes "
                                 f"{', '.join(LIMITS + SWITCHES)}")
    return base


@dataclass
class DefaultControl:
    base: dict[str, Any]
    mrid: str = field(default_factory=new_mrid)
    version: int = 0
    changed_at: datetime = field(default_factory=now)

    def to_json(self) -> dict[str, Any]:
        return {"mrid": self.mrid, "version": self.version, "base": self.base,
                "changed_at": self.changed_at}


@dataclass
class Control:
    id: int
    start: datetime
    duration_seconds: int
    base: dict[str, Any]
    description: str | None = None
    randomize_start_seconds: int | None = None
    randomize_duration_seconds: int | None = None
    # Bit 0 asks for "received", bit 1 for "started" and "completed".
    response_required: int = 3
    mrid: str = field(default_factory=new_mrid)
    created_at: datetime = field(default_factory=now)
    cancelled_at: datetime | None = None

    @property
    def end(self) -> datetime:
        return self.start + timedelta(seconds=self.duration_seconds)

    def status(self, at: datetime | None = None) -> str:
        at = at or now()
        if self.cancelled_at is not None:
            return "cancelled"
        if at >= self.end:
            return "completed"
        return "active" if at >= self.start else "scheduled"

    def to_json(self, responses: list[dict[str, Any]]) -> dict[str, Any]:
        return {
            "id": self.id, "mrid": self.mrid, "status": self.status(),
            "start": self.start, "duration_seconds": self.duration_seconds,
            "end": self.end, "base": self.base, "description": self.description,
            "randomize_start_seconds": self.randomize_start_seconds,
            "randomize_duration_seconds": self.randomize_duration_seconds,
            "response_required": self.response_required,
            "created_at": self.created_at, "cancelled_at": self.cancelled_at,
            "responses": [r for r in responses if r["subject"] == self.mrid],
        }


@dataclass
class EndDevice:
    id: int
    lfdi: str
    sfdi: int
    pin: int
    changed_time: int
    enabled: bool = True
    device_category: str | None = None
    post_rate: int | None = None
    registered_at: datetime = field(default_factory=now)
    updated_at: datetime = field(default_factory=now)
    connection_point_id: str | None = None
    # DER resource name -> {"at", "data", "xml"}, as the client last wrote it.
    der: dict[str, dict[str, Any]] = field(default_factory=dict)
    program_mrid: str = field(default_factory=new_mrid)
    fsa_mrid: str = field(default_factory=new_mrid)
    primacy: int = 1
    default_control: DefaultControl | None = None
    controls: dict[int, Control] = field(default_factory=dict)
    responses: deque[dict[str, Any]] = field(
        default_factory=lambda: deque(maxlen=RESPONSE_LOG_SIZE))
    _control_ids: itertools.count = field(default_factory=lambda: itertools.count(1))

    def keys(self) -> set[str]:
        return {str(self.id), self.lfdi.upper(), str(self.sfdi)}

    def add_control(self, **fields: Any) -> Control:
        control = Control(id=next(self._control_ids), **fields)
        self.controls[control.id] = control
        return control

    def listed_controls(self, at: datetime | None = None) -> list[Control]:
        """What DERControlList shows: every control that has not ended,
        cancelled ones included until their end so that the client sees the
        cancellation. Ordered as 2030.5 orders events: by start, then the
        newest first."""
        at = at or now()
        listed = [c for c in self.controls.values() if c.end > at]
        listed.sort(key=lambda c: (c.start, -c.created_at.timestamp(), c.mrid))
        return listed

    def active_controls(self, at: datetime | None = None) -> list[Control]:
        at = at or now()
        return [c for c in self.listed_controls(at) if c.status(at) == "active"]

    def summary(self) -> dict[str, Any]:
        at = now()
        return {
            "id": self.id, "lfdi": self.lfdi, "sfdi": self.sfdi,
            "registered_at": self.registered_at,
            "connection_point_id": self.connection_point_id,
            "active_controls": len(self.active_controls(at)),
            "scheduled_controls": sum(c.status(at) == "scheduled"
                                      for c in self.controls.values()),
        }

    def to_json(self) -> dict[str, Any]:
        responses = list(self.responses)
        return {
            **self.summary(),
            "pin": self.pin, "changed_time": self.changed_time, "enabled": self.enabled,
            "device_category": self.device_category, "post_rate": self.post_rate,
            "updated_at": self.updated_at, "primacy": self.primacy,
            "der": {name: {"at": entry["at"], "data": entry["data"]}
                    for name, entry in self.der.items()},
            "default_control": self.default_control.to_json() if self.default_control else None,
            "controls": [c.to_json(responses) for c in
                         sorted(self.controls.values(), key=lambda c: c.id)],
            "responses": responses,
        }


@dataclass
class MirrorUsagePoint:
    id: int
    mrid: str
    device_lfdi: str
    description: str | None
    role_flags: str
    service_category_kind: int
    status: int
    post_rate: int | None
    created_at: datetime = field(default_factory=now)
    # Mirror meter reading mRID -> {"description", "reading_type", "latest", "at"}.
    meter_readings: dict[str, dict[str, Any]] = field(default_factory=dict)
    posts: deque[dict[str, Any]] = field(
        default_factory=lambda: deque(maxlen=READING_LOG_SIZE))

    def summary(self) -> dict[str, Any]:
        return {
            "id": self.id, "mrid": self.mrid, "device_lfdi": self.device_lfdi,
            "description": self.description, "role_flags": self.role_flags,
            "service_category_kind": self.service_category_kind, "status": self.status,
            "post_rate": self.post_rate, "created_at": self.created_at,
            "meter_readings": list(self.meter_readings.values()),
        }

    def to_json(self) -> dict[str, Any]:
        return {**self.summary(), "posts": list(self.posts)}


@dataclass
class Client:
    name: str
    first_contact_at: datetime = field(default_factory=now)
    last_contact_at: datetime = field(default_factory=now)
    end_devices: dict[int, EndDevice] = field(default_factory=dict)
    mirror_usage_points: dict[int, MirrorUsagePoint] = field(default_factory=dict)
    messages: deque[dict[str, Any]] = field(
        default_factory=lambda: deque(maxlen=MESSAGE_LOG_SIZE))
    _device_ids: itertools.count = field(default_factory=lambda: itertools.count(1))
    _mup_ids: itertools.count = field(default_factory=lambda: itertools.count(1))

    def device(self, key: str) -> EndDevice:
        """An end device by id, lFDI or sFDI. KeyError when there is none."""
        wanted = key.upper()
        for device in self.end_devices.values():
            if wanted in device.keys():
                return device
        raise KeyError(key)

    def find_device(self, lfdi: str | None, sfdi: int) -> EndDevice | None:
        for device in self.end_devices.values():
            if device.sfdi == sfdi or lfdi and device.lfdi.upper() == lfdi.upper():
                return device
        return None

    def add_device(self, **fields: Any) -> EndDevice:
        device = EndDevice(id=next(self._device_ids), **fields)
        self.end_devices[device.id] = device
        return device

    def find_mup(self, mrid: str) -> MirrorUsagePoint | None:
        return next((m for m in self.mirror_usage_points.values()
                     if m.mrid.upper() == mrid.upper()), None)

    def add_mup(self, **fields: Any) -> MirrorUsagePoint:
        mup = MirrorUsagePoint(id=next(self._mup_ids), **fields)
        self.mirror_usage_points[mup.id] = mup
        return mup

    def summary(self) -> dict[str, Any]:
        return {
            "name": self.name, "first_contact_at": self.first_contact_at,
            "last_contact_at": self.last_contact_at,
            "end_devices": [d.summary() for d in self.end_devices.values()],
            "mirror_usage_points": len(self.mirror_usage_points),
        }

    def to_json(self) -> dict[str, Any]:
        return {
            **self.summary(),
            "end_devices": [d.to_json() for d in self.end_devices.values()],
            "mirror_usage_points": [m.summary() for m in self.mirror_usage_points.values()],
        }


@dataclass
class Server:
    poll_rate_seconds: int
    pin: int
    time_zone: str
    client_name_prefix: str = ""
    # The postRate a new mirror usage point is given.
    post_rate_seconds: int = 60
    clients: dict[str, Client] = field(default_factory=dict)
    # Exchanges under no /d/{client}/ path.
    unattributed: deque[dict[str, Any]] = field(
        default_factory=lambda: deque(maxlen=MESSAGE_LOG_SIZE))
    _sequence: itertools.count = field(default_factory=lambda: itertools.count(1))

    def client(self, name: str) -> Client:
        """The client calling under this name, created on first contact."""
        if self.client_name_prefix and not name.startswith(self.client_name_prefix):
            raise PermissionError(f"client names must start with {self.client_name_prefix}")
        client = self.clients.get(name)
        if client is None:
            client = self.clients[name] = Client(name)
        client.last_contact_at = now()
        return client

    def get(self, name: str) -> Client:
        """A client the control API names. KeyError when it never called."""
        return self.clients[name]

    def remove(self, name: str) -> None:
        del self.clients[name]

    def record(self, client: Client | None, entry: dict[str, Any]) -> dict[str, Any]:
        entry = {"seq": next(self._sequence), "at": now(), **entry}
        (client.messages if client else self.unattributed).append(entry)
        return entry
