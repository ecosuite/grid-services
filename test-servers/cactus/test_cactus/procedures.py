"""CACTUS's client test procedures: which to run, and how long each takes.

A test takes as long as its definition makes it: its waits and the controls
it schedules run on the clock, one step after another. The estimate follows
the steps in order: a wait fires its duration after it is enabled, a started
or completed response no sooner than its control starts or ends, and any
other step when it is enabled. It is never shorter than the waits together or
the latest control's end, and orders a batch longest first and sets each
test's timeout.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from functools import cache
from typing import Any, Iterator

import yaml
from cactus_test_definitions.client.test_procedures import TestProcedureId, get_yaml_contents

# The classes a cloud client managing PV, batteries and controllable loads
# certifies to: every client's (A), generation (G), load (L) and connection
# points (C), and the witnessed tests of those with a DER (DER-A, DER-G,
# DER-L).
REQUIRED_CLASSES = ("A", "C", "G", "L", "DER-A", "DER-G", "DER-L")
# The CSIP-AUS version the worker speaks, and the CACTUS images test.
CSIP_AUS_VERSION = "v1.2"
# Tests longer than this, such as ALL-25-EXT's three days, run only when
# named.
LONG_SECONDS = 2 * 3600
# What a test takes beyond its waits and controls: polls to see what changed.
OVERHEAD_SECONDS = 90
TIMEOUT_MARGIN_SECONDS = 300

OFFSET = re.compile(r"([+-])\s*'?(\d+)\s*(second|minute|hour)")
UNIT_SECONDS = {"second": 1, "minute": 60, "hour": 3600}


@dataclass(frozen=True)
class Procedure:
    id: TestProcedureId
    description: str
    classes: tuple[str, ...]
    versions: tuple[str, ...]
    immediate_start: bool
    estimate_seconds: int
    creates_controls: bool

    @property
    def timeout_seconds(self) -> float:
        return self.estimate_seconds * 1.5 + TIMEOUT_MARGIN_SECONDS


def seconds(value: Any) -> int:
    """Seconds from a duration or a `$(now) + 5 minutes` offset."""
    if isinstance(value, (int, float)):
        return int(value)
    match = OFFSET.search(str(value))
    if not match:
        return 0
    amount = int(match.group(2)) * UNIT_SECONDS[match.group(3)]
    return amount if match.group(1) == "+" else -amount


def actions(definition: dict[str, Any]) -> Iterator[dict[str, Any]]:
    preconditions = definition.get("Preconditions") or {}
    yield from preconditions.get("init_actions") or []
    yield from preconditions.get("actions") or []
    for step in (definition.get("Steps") or {}).values():
        yield from step.get("actions") or []


def response_status(event: dict[str, Any]) -> tuple[int | None, str | None]:
    """The status and control tag a response step waits for, if any."""
    for check in event.get("checks") or []:
        if check.get("type") == "response-contents":
            parameters = check.get("parameters") or {}
            return parameters.get("status"), parameters.get("subject_tag")
    return None, None


def fire_seconds(event: dict[str, Any], enabled: int,
                 controls: dict[str, tuple[int, int]]) -> int:
    if event.get("type") == "wait":
        return enabled + seconds((event.get("parameters") or {}).get("duration_seconds"))
    status, tag = response_status(event)
    start, end = controls.get(tag or "", (enabled, enabled))
    return max(enabled, {2: start, 3: end}.get(status or 0, enabled))


def schedule(action: dict[str, Any], at: int) -> tuple[int, int]:
    parameters = action.get("parameters") or {}
    start = at + seconds(parameters.get("start", 0))
    return start, start + int(parameters.get("duration_seconds") or 0)


def chain_seconds(definition: dict[str, Any]) -> int:
    """When the steps finish, followed in order from the first."""
    steps = definition.get("Steps") or {}
    preconditions = definition.get("Preconditions") or {}
    controls = {(action.get("parameters") or {}).get("tag", ""): schedule(action, 0)
                for action in (preconditions.get("init_actions") or [])
                + (preconditions.get("actions") or [])
                if action.get("type") == "create-der-control"}
    enabled = {name: 0 for name in list(steps)[:1]}
    fired: dict[str, int] = {}
    while enabled:
        at, name = min((fire_seconds(steps[name].get("event") or {}, since, controls), name)
                       for name, since in enabled.items())
        fired[name] = at
        del enabled[name]
        for action in steps[name].get("actions") or []:
            parameters = action.get("parameters") or {}
            kind = action.get("type")
            if kind == "create-der-control":
                controls[parameters.get("tag", "")] = schedule(action, at)
            elif kind == "enable-steps":
                enabled.update({step: at for step in parameters.get("steps") or []
                                if step in steps and step not in fired})
            elif kind == "remove-steps":
                fired.update({step: fired.get(step, at) for step in parameters.get("steps") or []})
                enabled = {step: since for step, since in enabled.items() if step not in fired}
            elif kind == "finish-test":
                return at
    return max(fired.values(), default=0)


def estimate_seconds(definition: dict[str, Any]) -> int:
    events = [step.get("event") or {} for step in (definition.get("Steps") or {}).values()]
    waits = sum(seconds((event.get("parameters") or {}).get("duration_seconds"))
                for event in events if event.get("type") == "wait")
    control_ends = [seconds((action.get("parameters") or {}).get("start", 0))
                    + int((action.get("parameters") or {}).get("duration_seconds") or 0)
                    for action in actions(definition)
                    if action.get("type") == "create-der-control"]
    return max(waits, max(control_ends, default=0), chain_seconds(definition)) + OVERHEAD_SECONDS


@cache
def load(test: str) -> Procedure:
    test_id = TestProcedureId(test)
    definition = yaml.safe_load(get_yaml_contents(test_id))
    return Procedure(
        id=test_id,
        description=definition.get("Description", ""),
        classes=tuple(definition.get("Classes") or []),
        versions=tuple(definition.get("TargetVersions") or []),
        immediate_start=bool((definition.get("Preconditions") or {}).get("immediate_start")),
        estimate_seconds=estimate_seconds(definition),
        creates_controls=any(action.get("type") == "create-der-control"
                             for action in actions(definition)),
    )


def every() -> list[Procedure]:
    return [load(str(test_id)) for test_id in TestProcedureId]


def select(names: list[str], classes: list[str]) -> list[Procedure]:
    """The named tests, and those in any of the classes that test CSIP-AUS
    v1.2 and are short enough to run by class, longest first."""
    named = [load(name) for name in names]
    by_class = [procedure for procedure in every()
                if set(procedure.classes) & set(classes)
                and CSIP_AUS_VERSION in procedure.versions
                and procedure.estimate_seconds <= LONG_SECONDS]
    chosen = {procedure.id: procedure for procedure in named + by_class}
    return sorted(chosen.values(), key=lambda procedure: -procedure.estimate_seconds)
