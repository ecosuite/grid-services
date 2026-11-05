"""What each test asks of the site: its DER, the power its plant wants,
and the operator's acts, for the steps whose instructions ask the person at
the site to act. Each act is one of the site simulator's
(POST /operator/{act}), "proceed", which the operator sends the runner
(GET /proceed), or "restart_worker", a power cycle of the worker."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from test_cactus import site


@dataclass(frozen=True)
class Act:
    """An act the operator does once its step has started and
    `after_seconds` have passed, with `watts` for an act that takes them."""
    step: str
    act: str
    after_seconds: float = 0
    watts: float | None = None

    def body(self) -> dict[str, Any] | None:
        return {"watts": self.watts} if self.watts is not None else None


def stepped_load(der: dict[str, Any]) -> list[Act]:
    """GEN-13's and LOA-13's test load, in WAIT-TEST-END: 20%, 10% and 0% of
    the DER's rating, 120, 240 and 360 s in."""
    return [Act("WAIT-TEST-END", "unmanaged_load", seconds, share * der["rtg_max_w"])
            for seconds, share in ((120, 0.2), (240, 0.1), (360, 0.0))]


OPERATOR_ACTS: dict[str, list[Act]] = {
    "ALL-07": [Act("PUT-DERSTATUS-DISCONNECT", "isolate"),
               Act("PUT-DERSTATUS-CONNECT", "reconnect")],
    "ALL-08": [Act("PUT-DERSTATUS-DISCONNECT", "stop_power"),
               Act("PUT-DERSTATUS-CONNECT", "resume_power")],
    "GEN-13": stepped_load(site.PV5K),
    "LOA-13": stepped_load(site.PV5K_BAT5K),
    # Once the DER is seen ramping after it returns.
    "ALL-26": [Act("PROCEED-OBSERVE-DER", "proceed", 60)],
    "ALL-27": [Act("PROCEED-OBSERVE-DER", "proceed", 60)],
    # The worker is power cycled while comms are down, and comms come back
    # once the DER follows the default again.
    "ALL-30": [Act("CLIENT-POWER-CYCLE", "restart_worker"),
               Act("REESTABLISH-COMMUNICATION", "proceed", 120)],
}


def der(test: str) -> dict[str, Any]:
    """The site's DER: PV with a battery for the load tests, PV alone
    otherwise."""
    return dict(site.PV5K_BAT5K if test.startswith("LOA-") else site.PV5K)


def power(test: str) -> dict[str, float]:
    """The power the plant wants, as Scenario fields, where the test's
    instructions ask for more than the defaults: for GEN-*, ALL-26, 27 and
    28, exporting 70% of rating with the DER generating 4 kW; for ALL-11, 12
    and 23, whose default caps export and import at 0 W, the DER generating
    3.5 kW (70%) into an unmanaged load of as much; for LOA-*, importing 70%
    with the battery charging at 3 kW (60%); in GEN-13 and LOA-13 the test
    load starts at 0 W."""
    starts_at_zero = test in ("GEN-13", "LOA-13")
    unmanaged_load_w = 0.0 if starts_at_zero else 500.0
    if test.startswith("GEN-") or test in ("ALL-26", "ALL-27", "ALL-28"):
        return {"available_pv_w": 4000, "unmanaged_load_w": unmanaged_load_w}
    if test in ("ALL-11", "ALL-12", "ALL-23"):
        return {"available_pv_w": 4000, "unmanaged_load_w": 3500}
    if test.startswith("LOA-"):
        return {"available_pv_w": 0, "battery_w": -3000, "unmanaged_load_w": unmanaged_load_w}
    return {}


def acts(test: str) -> list[Act]:
    """The operator's acts for the test."""
    return OPERATOR_ACTS.get(test, [])
