"""The test VTN's state, kept in memory.

Everything the VTN knows about a VEN hangs off its `Ven`: the registration,
the events addressed to it, messages waiting for its next poll, and a log of
what it sent. Nothing survives a restart; a VEN that polls a restarted VTN is
asked to register again.
"""

from __future__ import annotations

import itertools
import uuid
from collections import deque
from dataclasses import asdict, dataclass, field
from datetime import datetime, timedelta, timezone
from typing import Any

from openleadr import enums, objects, utils
from test_common.control import InvalidRequest

# How much history each VEN keeps.
MESSAGE_LOG_SIZE = 500
REPORT_LOG_SIZE = 500


def now() -> datetime:
    return datetime.now(timezone.utc)


def new_id() -> str:
    return str(uuid.uuid4())


@dataclass
class Interval:
    duration_seconds: int
    value: float


@dataclass
class Signal:
    name: str
    type: str
    intervals: list[Interval]
    # Optional emix item: {"name", "description", "unit", "scale"}.
    measurement: dict[str, Any] | None = None
    id: str = field(default_factory=new_id)

    @property
    def duration_seconds(self) -> int:
        return sum(i.duration_seconds for i in self.intervals)

    def value_at(self, start: datetime, at: datetime) -> float:
        """The payload of the interval running at `at`, or 0 outside them."""
        offset = (at - start).total_seconds()
        if self.duration_seconds == 0:
            # Open-ended: the one interval runs until cancelled.
            return self.intervals[0].value if offset >= 0 else 0
        for interval in self.intervals:
            if 0 <= offset < interval.duration_seconds:
                return interval.value
            offset -= interval.duration_seconds
        return 0


@dataclass
class Event:
    event_id: str
    start: datetime
    signals: list[Signal]
    market_context: str
    response_required: bool = True
    test_event: bool = False
    priority: int = 0
    start_after_seconds: int | None = None
    comment: str | None = None
    modification_number: int = 0
    cancelled: bool = False
    created_at: datetime = field(default_factory=now)
    modified_at: datetime | None = None
    # The modification number last sent to the VEN, if any.
    delivered_modification: int | None = None
    # Every opt answer received for this event, through EiEvent or EiOpt:
    # {"modification_number", "opt", "via", "response_code", "at"}.
    responses: list[dict[str, Any]] = field(default_factory=list)

    @property
    def duration_seconds(self) -> int:
        return self.signals[0].duration_seconds

    def status(self, at: datetime | None = None) -> str:
        if self.cancelled:
            return "cancelled"
        at = at or now()
        end = self.start + timedelta(seconds=self.duration_seconds)
        if self.duration_seconds > 0 and at >= end:
            return "completed"
        if at >= self.start:
            return "active"
        return "far"

    def is_open(self) -> bool:
        """Whether the event still belongs in distributions to the VEN.

        A cancellation is sent until the VEN has seen it; a completed event
        is dropped, and the VEN closes it when it disappears from the list.
        """
        status = self.status()
        if status == "completed":
            return False
        if status == "cancelled":
            return self.delivered_modification != self.modification_number
        return True

    def latest_opt(self) -> str | None:
        return self.responses[-1]["opt"] if self.responses else None

    def to_openadr(self, ven_id: str) -> dict[str, Any]:
        """The event as an OpenLEADR payload dict, ready for a distribution."""
        at = now()
        active = self.status(at) == "active"
        tolerance = None
        if self.start_after_seconds:
            tolerance = {"startafter": timedelta(seconds=self.start_after_seconds)}
        event = objects.Event(
            event_descriptor=objects.EventDescriptor(
                event_id=self.event_id,
                modification_number=self.modification_number,
                modification_date_time=self.modified_at,
                market_context=self.market_context,
                event_status=self.status(),
                created_date_time=self.created_at,
                priority=self.priority,
                test_event=self.test_event,
                vtn_comment=self.comment,
            ),
            active_period=objects.ActivePeriod(
                dtstart=self.start,
                duration=timedelta(seconds=self.duration_seconds),
                tolerance=tolerance,
            ),
            event_signals=[
                objects.EventSignal(
                    signal_name=signal.name,
                    signal_type=signal.type,
                    signal_id=signal.id,
                    intervals=[
                        objects.Interval(
                            dtstart=None,
                            duration=timedelta(seconds=interval.duration_seconds),
                            signal_payload=interval.value,
                        )
                        for interval in signal.intervals
                    ],
                    measurement=_measurement(signal.measurement),
                    # 0 until the event is active (rule 14).
                    current_value=signal.value_at(self.start, at) if active else 0,
                )
                for signal in self.signals
            ],
            targets=[objects.Target(ven_id=ven_id)],
            response_required="always" if self.response_required else "never",
        )
        payload = asdict(event)
        # OpenLEADR's Event derives the status from the time alone, which
        # loses a cancellation.
        payload["event_descriptor"]["event_status"] = self.status()
        return payload

    def to_json(self) -> dict[str, Any]:
        return {
            "event_id": self.event_id,
            "modification_number": self.modification_number,
            "status": self.status(),
            "open": self.is_open(),
            "start": self.start,
            "duration_seconds": self.duration_seconds,
            "start_after_seconds": self.start_after_seconds,
            "signals": [asdict(s) for s in self.signals],
            "market_context": self.market_context,
            "response_required": self.response_required,
            "test_event": self.test_event,
            "priority": self.priority,
            "comment": self.comment,
            "created_at": self.created_at,
            "modified_at": self.modified_at,
            "delivered_modification": self.delivered_modification,
            "opt": self.latest_opt(),
            "responses": self.responses,
        }


def _measurement(spec: dict[str, Any] | None) -> objects.Measurement | None:
    if spec is None:
        return None
    name = spec["name"]
    namespace = enums._MEASUREMENT_NAMESPACES.get(name, "oadr")
    power_attributes = None
    if namespace == "power":
        # The schema requires them on power items.
        power_attributes = objects.PowerAttributes(**spec.get("power_attributes", {}))
    return objects.Measurement(
        name=name,
        description=spec.get("description", enums._MEASUREMENT_DESCRIPTIONS.get(name, name)),
        unit=spec["unit"],
        scale=spec.get("scale", "none"),
        power_attributes=power_attributes,
        ns=namespace,
    )


@dataclass
class Ven:
    ven_id: str
    ven_name: str
    registration_id: str | None = None
    registered_at: datetime | None = None
    last_contact_at: datetime | None = None
    events: dict[str, Event] = field(default_factory=dict)
    # Set when the events changed since the VEN last received them.
    events_changed: bool = False
    # Messages for the VEN's next polls, oldest first: (type, payload).
    outbox: deque[tuple[str, dict[str, Any]]] = field(default_factory=deque)
    # Reports the VEN offered, keyed by report specifier id.
    offered_reports: dict[str, dict[str, Any]] = field(default_factory=dict)
    # Report requests this VTN made, keyed by report request id.
    report_requests: dict[str, dict[str, Any]] = field(default_factory=dict)
    report_updates: deque[dict[str, Any]] = field(
        default_factory=lambda: deque(maxlen=REPORT_LOG_SIZE))
    # The VEN's oadrCreatedReport answers: response code, description and
    # the requests it still holds.
    report_answers: list[dict[str, Any]] = field(default_factory=list)
    opts: list[dict[str, Any]] = field(default_factory=list)
    messages: deque[dict[str, Any]] = field(
        default_factory=lambda: deque(maxlen=MESSAGE_LOG_SIZE))

    @property
    def registered(self) -> bool:
        return self.registration_id is not None

    def open_events(self) -> list[Event]:
        return [e for e in self.events.values() if e.is_open()]

    def take_distribution(self) -> list[dict[str, Any]]:
        """The events to distribute now, marked as delivered."""
        events = self.open_events()
        for event in events:
            event.delivered_modification = event.modification_number
        self.events_changed = False
        return utils.order_events([e.to_openadr(self.ven_id) for e in events]) or []

    def summary(self) -> dict[str, Any]:
        return {
            "ven_id": self.ven_id,
            "ven_name": self.ven_name,
            "registered": self.registered,
            "registration_id": self.registration_id,
            "registered_at": self.registered_at,
            "last_contact_at": self.last_contact_at,
            "open_events": len(self.open_events()),
            "queued_messages": [t for t, _ in self.outbox],
        }

    def to_json(self) -> dict[str, Any]:
        return {
            **self.summary(),
            "events": [e.to_json() for e in self.events.values()],
            "reports": {
                "offered": list(self.offered_reports.values()),
                "requests": list(self.report_requests.values()),
                "updates": len(self.report_updates),
            },
            "opts": self.opts,
        }


class Vtn:
    def __init__(self, vtn_id: str, market_context: str,
                 poll_interval: timedelta, report_interval: timedelta,
                 ven_name_prefix: str = ""):
        self.vtn_id = vtn_id
        self.market_context = market_context
        self.poll_interval = poll_interval
        self.report_interval = report_interval
        self.ven_name_prefix = ven_name_prefix
        # Whether every telemetry report a VEN offers is requested as soon as
        # it is registered. The control API can turn it off for a test that
        # makes its own requests.
        self.auto_report_requests = True
        self.vens: dict[str, Ven] = {}
        # Exchanges that could not be tied to a VEN.
        self.unattributed: deque[dict[str, Any]] = deque(maxlen=MESSAGE_LOG_SIZE)
        self._sequence = itertools.count(1)

    # -- Lookup -------------------------------------------------------------

    def find(self, key: str | None) -> Ven | None:
        """A VEN by id, or failing that by name."""
        if key is None:
            return None
        if key in self.vens:
            return self.vens[key]
        return next((v for v in self.vens.values() if v.ven_name == key), None)

    def get(self, key: str) -> Ven:
        ven = self.find(key)
        if ven is None:
            raise KeyError(key)
        return ven

    def remove(self, key: str) -> None:
        del self.vens[self.get(key).ven_id]

    # -- Registration -------------------------------------------------------

    def register(self, ven_name: str, ven_id: str | None,
                 registration_id: str | None) -> Ven:
        """Registers a VEN, creating it on first contact.

        A VEN the operator did not provision is given its name as its id. A
        VEN that presents its current registration id keeps it (a
        re-registration); otherwise it gets a new one.
        """
        ven = self.find(ven_id) if ven_id else self.find(ven_name)
        if ven is None:
            ven = Ven(ven_id=ven_id or ven_name, ven_name=ven_name)
            self.vens[ven.ven_id] = ven
        if registration_id is None or registration_id != ven.registration_id:
            ven.registration_id = new_id()
        ven.registered_at = now()
        # Registration starts a new set of report requests.
        ven.report_requests.clear()
        return ven

    def forget_registration(self, ven: Ven) -> None:
        ven.registration_id = None
        ven.registered_at = None
        ven.outbox.clear()
        ven.report_requests.clear()

    # -- Events -------------------------------------------------------------

    def add_event(self, ven: Ven, event: Event) -> Event:
        if event.event_id in ven.events:
            raise InvalidRequest(f"event {event.event_id} already exists")
        ven.events[event.event_id] = event
        ven.events_changed = True
        return event

    def modify_event(self, ven: Ven, event: Event) -> Event:
        event.modification_number += 1
        event.modified_at = now()
        ven.events_changed = True
        return event

    def cancel_event(self, ven: Ven, event: Event) -> Event:
        if event.cancelled:
            raise InvalidRequest(f"event {event.event_id} is already cancelled")
        event.cancelled = True
        return self.modify_event(ven, event)

    # -- Log ----------------------------------------------------------------

    def record(self, ven: Ven | None, entry: dict[str, Any]) -> dict[str, Any]:
        entry = {"seq": next(self._sequence), "at": now(), **entry}
        (ven.messages if ven else self.unattributed).append(entry)
        return entry
