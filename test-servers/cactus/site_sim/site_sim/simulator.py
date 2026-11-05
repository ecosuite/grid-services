"""The site as a whole: given a scenario, it provisions the connection as
ecosuite would, and from then on SolarNode uploads the plant's datums to
the stand-in SolarNetwork every five seconds, where the worker reads them,
and ecosuite's planner applies the caps in force to the plant and moves
the controls' lifecycle.

The control API is the test's: the worker never sees it.

    GET  /health           ok
    POST /scenario         provisions the site; returns {"connection_id"}
    POST /operator/{act}   does an operator's act, from the next datum: on
                           the DER (isolate, reconnect, stop_power,
                           resume_power), with no body, or on the site's
                           unmanaged load (unmanaged_load), with
                           {"watts"}; returns {"der_connected",
                           "der_operating", "unmanaged_load_w"} as the
                           plant now measures them, the switches included
    GET  /trace            the scenario, the datums written, the operator's
                           acts, the caps applied with their ramps, the
                           lifecycle moves, the SolarNetwork requests seen,
                           the plant at each datum with its state and the
                           limits of the caps and returns still ramping,
                           and the checks of the plant: which ran, and
                           what they warn of
"""

from __future__ import annotations

import asyncio
import logging
import secrets
import time
from collections import deque
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from typing import Any, Callable

import psycopg
from aiohttp import web

from site_sim import solarnode
from site_sim.checks import Checks
from site_sim.plant import Plant
from site_sim.planner import Planner
from site_sim.provisioner import Provisioner
from site_sim.scenario import Scenario
from test_common.control import InvalidRequest, error, json_response, read_body, typed_field
from test_common.solarnetwork import FakeSolarNetwork

log = logging.getLogger("site_sim")

DATUM_SECONDS = 5
TOKEN = "site-sim"


@dataclass(frozen=True)
class Act:
    """An operator's act at a time: on the DER, which the site must have,
    and which takes no body, or on the rest of the site. `do` returns the
    act's value, for an act that takes one."""
    on_der: bool
    do: Callable[[Plant, dict[str, Any], datetime], float | None]


def der_act(method: Callable[[Plant, datetime], None]) -> Act:
    def do(plant: Plant, body: dict[str, Any], at: datetime) -> None:
        method(plant, at)
    return Act(on_der=True, do=do)


def unmanaged_load(plant: Plant, body: dict[str, Any], at: datetime) -> float:
    watts = typed_field(body, "watts", (int, float))
    if watts is None or watts < 0:
        raise InvalidRequest('unmanaged_load needs {"watts"}, 0 or more')
    plant.set_unmanaged_load(float(watts))
    return float(watts)


OPERATOR_ACTS = {
    "isolate": der_act(lambda plant, at: plant.isolate()),
    "reconnect": der_act(Plant.reconnect),
    "stop_power": der_act(lambda plant, at: plant.stop_power()),
    "resume_power": der_act(lambda plant, at: plant.resume_power()),
    "unmanaged_load": Act(on_der=False, do=unmanaged_load),
}


@web.middleware
async def control_errors(request: web.Request, handler: Any) -> web.StreamResponse:
    """A bad request is a 400, and a site that cannot be provisioned, for
    its database or its certificates, a 500, each saying why."""
    try:
        return await handler(request)
    except InvalidRequest as problem:
        return error(400, str(problem))
    except (psycopg.Error, OSError) as problem:
        log.exception("could not provision the site")
        return error(500, f"{type(problem).__name__}: {problem}")


class Simulator:
    def __init__(self, provisioner: Provisioner):
        self.provisioner = provisioner
        # Every request is kept: the trace is the whole run's.
        self.solarnetwork = FakeSolarNetwork(requests=deque())
        self.scenario: Scenario | None = None
        self.plant: Plant | None = None
        self.connection_id: int | None = None
        self.operator: list[dict[str, Any]] = []
        # What the planner did: "caps" applied and "lifecycle" moves.
        self.planned: dict[str, list[dict[str, Any]]] = {"caps": [], "lifecycle": []}
        # The plant at each datum, and the checks of it.
        self.breakdowns: list[dict[str, Any]] = []
        self.checks: Checks | None = None
        self.lock = asyncio.Lock()
        self.uploading: asyncio.Task | None = None
        self.planning: asyncio.Task | None = None

    def solarnetwork_routes(self) -> list[web.RouteDef]:
        return self.solarnetwork.routes()

    def control_routes(self) -> list[web.RouteDef]:
        return [
            web.get("/health", self.health),
            web.post("/scenario", self.post_scenario),
            web.post("/operator/{act}", self.post_operator),
            web.get("/trace", self.trace),
        ]

    async def close(self) -> None:
        for task in (self.uploading, self.planning):
            if task:
                task.cancel()

    # -- ecosuite's planner -------------------------------------------------

    def record(self, kind: str, entry: dict[str, Any]) -> None:
        entry = {"at": datetime.now(timezone.utc), **entry}
        self.planned[kind].append(entry)
        if kind == "caps" and self.checks is not None:
            self.checks.caps_applied(entry["caps"], entry["at"])

    @staticmethod
    def planner_stopped(task: asyncio.Task) -> None:
        if not task.cancelled() and task.exception() is not None:
            log.error("the planner stopped: %r", task.exception())

    # -- SolarNode ----------------------------------------------------------

    def upload(self, at: datetime) -> None:
        """Writes the plant's datum to each stream, as SolarNode uploads, and
        records and checks what the plant did."""
        assert self.plant is not None and self.checks is not None
        measurement = self.plant.measure(at)
        ramping = self.plant.ramping(at)
        for source_id, values in solarnode.datums(measurement).items():
            self.solarnetwork.streams.setdefault((solarnode.NODE_ID, source_id), []).append(
                {"created": at, **values})
        self.breakdowns.append({
            "at": at, "pv_w": measurement.pv_w, "battery_w": measurement.battery_w,
            "managed_w": measurement.managed_w, "unmanaged_w": measurement.unmanaged_w,
            "site_w": measurement.site_w, "der_w": measurement.der_w,
            "der_connected": measurement.der_connected,
            "der_operating": measurement.der_operating,
            "caps_followed": self.plant.followed_caps(), "caps_ramping": ramping})
        self.checks.datum(at, measurement, self.plant.caps, self.plant.caps_since, ramping)

    async def upload_every(self, seconds: int) -> None:
        """Uploads now, then on each multiple of `seconds`."""
        while True:
            now = round(time.time())
            self.upload(datetime.fromtimestamp(now, timezone.utc))
            await asyncio.sleep((now // seconds + 1) * seconds - time.time())

    # -- Control API --------------------------------------------------------

    async def health(self, request: web.Request) -> web.Response:
        return json_response({"status": "ok", "connection_id": self.connection_id})

    async def post_scenario(self, request: web.Request) -> web.Response:
        scenario = Scenario.from_json(await read_body(request))
        async with self.lock:
            if self.connection_id is not None:
                return error(409, f"the site is already provisioned, as connection "
                             f"{self.connection_id}")
            secret = secrets.token_hex(16)
            self.solarnetwork.set_token(TOKEN, secret)
            self.connection_id = await self.provisioner.provision(scenario, TOKEN, secret)
            self.scenario = scenario
            self.plant = Plant.for_scenario(scenario)
            self.checks = Checks(scenario.der)
            self.uploading = asyncio.create_task(self.upload_every(DATUM_SECONDS))
            planner = Planner(self.provisioner.database_url, self.connection_id, self.plant,
                              self.record)
            self.planning = asyncio.create_task(planner.run())
            self.planning.add_done_callback(self.planner_stopped)
        log.info("provisioned connection %s for %s", self.connection_id, scenario)
        return json_response({"connection_id": self.connection_id})

    async def post_operator(self, request: web.Request) -> web.Response:
        act = request.match_info["act"]
        if act not in OPERATOR_ACTS:
            return error(404, f"the operator has no act {act}; it has "
                         f"{', '.join(OPERATOR_ACTS)}")
        body = await read_body(request)
        if OPERATOR_ACTS[act].on_der and body:
            raise InvalidRequest(f"{act} takes no body")
        async with self.lock:
            if self.plant is None or self.scenario is None:
                return error(409, "the site is not provisioned yet")
            if OPERATOR_ACTS[act].on_der and self.scenario.der is None:
                return error(409, f"the site has no DER to {act}")
            at = datetime.now(timezone.utc)
            watts = OPERATOR_ACTS[act].do(self.plant, body, at)
            self.operator.append({"at": at, "act": act,
                                  **({"watts": watts} if watts is not None else {})})
            measurement = self.plant.measure(at)
            state = {"der_connected": measurement.der_connected,
                     "der_operating": measurement.der_operating,
                     "unmanaged_load_w": measurement.unmanaged_w}
        log.info("operator %s: %s", act, state)
        return json_response(state)

    async def trace(self, request: web.Request) -> web.Response:
        return json_response({
            "scenario": asdict(self.scenario) if self.scenario else None,
            "connection_id": self.connection_id,
            "datums": self.datums_written(),
            "operator": self.operator,
            "caps": self.planned["caps"],
            "lifecycle": self.planned["lifecycle"],
            "requests": list(self.solarnetwork.requests),
            "plant": self.breakdowns,
            "checks": self.checks.summary() if self.checks else None,
            "warnings": self.checks.warnings() if self.checks else [],
        })

    def datums_written(self) -> list[dict[str, Any]]:
        return sorted(({"node_id": node_id, "source_id": source_id, **datum}
                       for (node_id, source_id), datums in self.solarnetwork.streams.items()
                       for datum in datums),
                      key=lambda datum: (datum["created"], datum["source_id"]))
