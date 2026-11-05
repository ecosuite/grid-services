"""ecosuite's planner, and the SolarNode instructions it sends, for one
connection: it applies the caps in force to the plant by the server's clock,
each with its row's ramp, and the setGradW in force, and moves each
control's lifecycle by the started rule. It applies again when the caps,
their ramps or the setGradW change. The setGradW in force is the one
DERSettings sends: that of the worker's last der_settings delivery, else
csipaus_der's. The rows in force cannot give it: a control's rows carry its
rampTms instead, and a default that sets no cap has no rows.

It reads as the ecosuite_api role and writes only events.lifecycle. It plans
when the worker signals grid_services_limits for the connection, and every
second besides, since caps and controls start and end with time, not with a
change to a row. Until the worker has read the server's clock it plans
nothing, and says so once.

The started rule: a control moves to control once the site applies the caps
in force while the control is in force (effective_start_at <= now <
effective_end_at, by the server's clock), whether or not it sets any of
them, and to completed once it has ended. The plant takes caps the moment
they are applied, so a pass applies the caps, then moves the controls.
Cancelled and superseded controls and defaults are left alone, and a control
without a window is skipped and logged. Failed is never written: the plant's
instructions cannot fail.
"""

from __future__ import annotations

import asyncio
import logging
from datetime import datetime, timedelta, timezone
from typing import Any, Callable

import psycopg

from site_sim.plant import Plant

log = logging.getLogger("site_sim.planner")

CHANNEL = "grid_services_limits"
SWEEP_SECONDS = 1
NOT_STARTED = ("received", "planned", "prep")

Record = Callable[[str, dict[str, Any]], None]


class Planner:
    def __init__(self, database_url: str, connection_id: int, plant: Plant, record: Record):
        self.database_url = database_url
        self.connection_id = connection_id
        self.plant = plant
        self.record = record
        self.signalled = asyncio.Event()
        self.said_no_clock = False
        self.said_no_window: set[int] = set()

    async def connect(self) -> psycopg.AsyncConnection:
        return await psycopg.AsyncConnection.connect(self.database_url, autocommit=True)

    async def run(self) -> None:
        async with await self.connect() as listening, await self.connect() as db:
            await listening.execute(f"LISTEN {CHANNEL}")
            listener = asyncio.create_task(self.listen(listening))
            try:
                while True:
                    try:
                        await self.plan(db)
                    except Exception:
                        log.exception("could not plan")
                    try:
                        await asyncio.wait_for(self.signalled.wait(), SWEEP_SECONDS)
                    except TimeoutError:
                        pass
                    self.signalled.clear()
            finally:
                listener.cancel()

    async def listen(self, listening: psycopg.AsyncConnection) -> None:
        async for notify in listening.notifies():
            if notify.payload == str(self.connection_id):
                self.signalled.set()

    async def plan(self, db: psycopg.AsyncConnection) -> None:
        state = await (await db.execute(
            "SELECT clock_offset_seconds FROM connection_state WHERE connection_id = %s",
            (self.connection_id,))).fetchone()
        offset = state[0] if state else None
        if offset is None:
            if not self.said_no_clock:
                self.said_no_clock = True
                log.info("the worker has not read the server's clock yet: planning nothing")
            return
        now = datetime.now(timezone.utc) + timedelta(seconds=offset)
        await self.apply_caps(db, now)
        await self.move_controls(db, now)

    async def apply_caps(self, db: psycopg.AsyncConnection, now: datetime) -> None:
        rows = await (await db.execute(
            "SELECT limit_name, value, ramp_seconds, set_grad_w FROM csipaus_limits "
            "WHERE connection_id = %s AND start_at <= %s AND (end_at IS NULL OR %s < end_at)",
            (self.connection_id, now, now))).fetchall()
        caps = {name: value for name, value, _, _ in rows}
        ramps = {name: {"ramp_seconds": float(seconds) if seconds is not None else None,
                        "set_grad_w": grad_w}
                 for name, _, seconds, grad_w in rows}
        set_grad_w = await self.settings_grad_w(db)
        if (caps != self.plant.caps or ramps != self.plant.cap_ramps
                or set_grad_w != self.plant.set_grad_w):
            at = datetime.now(timezone.utc)
            self.plant.apply_caps(caps, ramps, set_grad_w, at)
            self.record("caps", {"at": at, "server_at": now, "caps": caps, "ramps": ramps,
                                 "set_grad_w": set_grad_w})
            log.info("applied caps %s with ramps %s, setGradW %s", caps, ramps, set_grad_w)

    async def settings_grad_w(self, db: psycopg.AsyncConnection) -> float | None:
        """The setGradW DERSettings sends: the last der_settings delivery's,
        sent or failed, else csipaus_der's; None without either."""
        sent = await (await db.execute(
            "SELECT (entry ->> 'value')::float FROM report_deliveries d, "
            "jsonb_array_elements(d.payload) entry "
            "WHERE d.connection_id = %s AND d.report = 'der_settings' "
            "AND entry ->> 'rid' = 'set_grad_w' ORDER BY d.id DESC LIMIT 1",
            (self.connection_id,))).fetchone()
        if sent is not None:
            return sent[0]
        der = await (await db.execute(
            "SELECT set_grad_w FROM csipaus_der WHERE connection_id = %s",
            (self.connection_id,))).fetchone()
        return der[0] if der else None

    async def move_controls(self, db: psycopg.AsyncConnection, now: datetime) -> None:
        controls = await (await db.execute(
            "SELECT e.id, e.program_event_id, e.lifecycle, c.effective_start_at, "
            "c.effective_end_at FROM events e JOIN csipaus_events c ON c.event_id = e.id "
            "WHERE e.connection_id = %s AND e.source = 'program' AND c.kind = 'control' "
            "AND e.program_status NOT IN ('cancelled', 'superseded') "
            "AND e.lifecycle IN ('received', 'planned', 'prep', 'control') ORDER BY e.id",
            (self.connection_id,))).fetchall()
        for event_id, program_event_id, lifecycle, start, end in controls:
            if start is None or end is None:
                if event_id not in self.said_no_window:
                    self.said_no_window.add(event_id)
                    log.warning("control %s has no effective window: left as %s",
                                program_event_id, lifecycle)
                continue
            if end <= now:
                moved = "completed"
            elif start <= now and lifecycle in NOT_STARTED:
                moved = "control"
            else:
                continue
            cursor = await db.execute(
                "UPDATE events SET lifecycle = %s WHERE id = %s AND lifecycle = %s",
                (moved, event_id, lifecycle))
            if cursor.rowcount:
                self.record("lifecycle", {"server_at": now, "event_id": event_id,
                                          "program_event_id": program_event_id,
                                          "from": lifecycle, "to": moved})
                log.info("control %s: %s to %s", program_event_id, lifecycle, moved)
