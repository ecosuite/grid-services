"""The trace's checks of the plant, as warnings: they never fail a test,
and say what a witness at the site would see.

The cap check runs on every datum. A cap the plant follows is judged once
its current value has been applied for 15 s (explainer 6.1), counted from
when the planner applied it, within the lower of 4% of the DER's rating or
100 W (ARENA 2.3.1). An excess counts only while the managed DER still has
something to cut: generation for export and generation caps, consumption
for import and load caps, since unmanaged load is never controlled (6.2).
An energize or connect switch at 0 is judged once applied for 2 s, by the
larger of generation and consumption, while either is above 0. Each run of
exceeding datums is one warning. A cap the plant keeps but does not follow
is warned once each time it is applied.

The ramp check judges each datum of a cap still ramping against its
ramped limit at that datum, and each datum of a switch's return ramp, or
the operator's reconnection's, by the larger of generation and consumption
over its limit, with the cap check's tolerance and only while the DER has
something left to cut; each run of datums over it is one warning.
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timedelta
from typing import Any

from site_sim.plant import FOLLOWED, RETURNS, SWITCHES, Measurement

MEET_WITHIN = timedelta(seconds=15)
# Assumed: the explainer gives no bound for energize and connect.
SWITCH_WITHIN = timedelta(seconds=2)
TOLERANCE_SHARE = 0.04
TOLERANCE_MAX_W = 100.0
UNFOLLOWED = ("max_output",)


@dataclass
class Exceeding:
    """A run of datums over one cap, or over its ramp."""
    check: str
    cap: str
    # None for a return whose cap went away, or the operator's reconnection.
    value: float | None
    first: datetime
    last: datetime
    datums: int
    largest_excess_w: float

    def warning(self, tolerance_w: float) -> dict[str, Any]:
        switch = self.cap in RETURNS
        if self.check == "caps":
            over = f"{self.cap} {self.value:g}" if switch else f"{self.cap} cap of {self.value:g} W"
            after = f", {meet_within(self.cap).seconds} s or more after it was applied"
        else:
            over = f"{self.cap} return ramp" if switch else f"{self.cap} ramp to {self.value:g} W"
            after = ", over the ramped limit at each"
        return {"check": self.check, "cap": self.cap, "value": self.value,
                "first": self.first, "last": self.last, "datums": self.datums,
                "largest_excess_w": self.largest_excess_w,
                "message": f"{over} exceeded by up to {self.largest_excess_w:g} W "
                           f"(tolerance {tolerance_w:g} W) in {self.datums} datums from "
                           f"{self.first:%H:%M:%S} to {self.last:%H:%M:%S}{after}"}


def meet_within(cap: str) -> timedelta:
    return SWITCH_WITHIN if cap in SWITCHES else MEET_WITHIN


def excess(cap: str, value: float, measurement: Measurement) -> float | None:
    """How far a datum is over a cap, or a switch's limit in W, or None
    when the managed DER has nothing left to cut for it."""
    g = measurement.generation_w
    c = measurement.consumption_w
    u = measurement.unmanaged_w
    over = {"export": (g - c - u, g), "generation": (g - c, g),
            "import": (u + c - g, c), "load": (c - g, c),
            **{returning: (max(g, c), max(g, c)) for returning in RETURNS}}
    flow, controllable = over[cap]
    return flow - value if controllable > 0 else None


class Checks:
    def __init__(self, der: dict[str, Any] | None):
        rating = der.get("rtg_max_w") if der is not None else None
        self.tolerance_w = (min(TOLERANCE_SHARE * rating, TOLERANCE_MAX_W)
                            if rating is not None else None)
        self.not_run = (None if self.tolerance_w is not None
                        else "the site has no DER" if der is None
                        else "the DER has no rtg_max_w to judge the tolerance from")
        self.applied: dict[str, float] = {}
        self.unfollowed: list[dict[str, Any]] = []
        self.exceeding: dict[tuple[str, str], Exceeding] = {}
        self.exceeded: list[Exceeding] = []

    def caps_applied(self, caps: dict[str, float], at: datetime) -> None:
        """Warns of each cap the plant does not follow, as it is applied."""
        self.unfollowed += [
            {"check": "unfollowed", "cap": name, "value": value, "at": at,
             "message": f"{name} {value:g} applied at {at:%H:%M:%S}: the plant keeps it "
                        f"but does not follow it"}
            for name, value in caps.items()
            if name in UNFOLLOWED and self.applied.get(name) != value]
        self.applied = dict(caps)

    def datum(self, at: datetime, measurement: Measurement, caps: dict[str, float],
              since: dict[str, datetime], ramping: dict[str, float]) -> None:
        """Judges a datum against the caps applied 15 s or more and the
        switches at 0 applied 2 s or more, and against the limit of each
        cap or switch return still ramping."""
        if self.tolerance_w is None:
            return
        judged = {
            **{("caps", name): excess(name, caps[name], measurement) for name in FOLLOWED
               if name in caps and at - since[name] >= MEET_WITHIN},
            **{("caps", name): excess(name, 0.0, measurement) for name in SWITCHES
               if caps.get(name) == 0 and at - since[name] >= SWITCH_WITHIN},
            **{("ramps", name): excess(name, limit, measurement)
               for name, limit in ramping.items()}}
        over = {key: amount for key, amount in judged.items()
                if amount is not None and amount > self.tolerance_w}
        # A cap given a new value is not judged for 15 s, and a ramp that
        # ends is no longer judged, so their runs end.
        for key in set(self.exceeding) - set(over):
            self.exceeded.append(self.exceeding.pop(key))
        for (check, name), amount in over.items():
            run = self.exceeding.get((check, name))
            if run is None:
                self.exceeding[(check, name)] = Exceeding(check, name, caps.get(name), at, at,
                                                          1, amount)
            else:
                run.last = at
                run.datums += 1
                run.largest_excess_w = max(run.largest_excess_w, amount)

    def warnings(self) -> list[dict[str, Any]]:
        runs = sorted([*self.exceeded, *self.exceeding.values()], key=lambda run: run.first)
        warnings = [*self.unfollowed, *(run.warning(self.tolerance_w) for run in runs)]
        return sorted(warnings, key=lambda warning: warning.get("at") or warning["first"])

    def summary(self) -> dict[str, dict[str, Any]]:
        caps = ({"ran": True, "tolerance_w": self.tolerance_w,
                 "meet_within_seconds": MEET_WITHIN.seconds,
                 "switch_within_seconds": SWITCH_WITHIN.seconds}
                if self.not_run is None else {"ran": False, "why": self.not_run})
        ramps = ({"ran": True, "tolerance_w": self.tolerance_w}
                 if self.not_run is None else {"ran": False, "why": self.not_run})
        return {"caps": caps, "unfollowed": {"ran": True, "caps": list(UNFOLLOWED)},
                "ramps": ramps}
