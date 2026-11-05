"""A test's scenario, as POST /scenario gives it: the site's DER as ecosuite
describes it in csipaus_der, whether the DER is connected and operating, the
power the plant wants (the PV available, the battery's power, positive
discharging and negative charging, and the managed and unmanaged loads),
and the connection's PIN and connection point."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from test_common.control import InvalidRequest, typed_field

DER_COLUMNS = {
    "der_type", "rtg_max_w", "rtg_max_va", "rtg_max_var", "rtg_max_var_neg",
    "rtg_max_charge_rate_w", "rtg_max_discharge_rate_w", "rtg_max_wh", "rtg_v_nom",
    "set_max_w", "set_max_va", "set_max_var", "set_max_charge_rate_w",
    "set_max_discharge_rate_w", "set_grad_w", "modes_supported", "modes_enabled",
}
FIELDS = ("der", "der_connected", "der_operating", "available_pv_w", "unmanaged_load_w",
          "managed_load_w", "battery_w", "pin", "nmi")
UNSIGNED_POWERS = ("available_pv_w", "unmanaged_load_w", "managed_load_w")
POWERS = (*UNSIGNED_POWERS, "battery_w")


@dataclass(frozen=True)
class Scenario:
    der: dict[str, Any] | None
    der_connected: bool
    der_operating: bool
    available_pv_w: float
    unmanaged_load_w: float
    managed_load_w: float
    battery_w: float
    pin: int | None
    nmi: str | None

    @classmethod
    def from_json(cls, body: dict[str, Any]) -> Scenario:
        """Every field is required; der, pin and nmi may be null."""
        missing = [name for name in FIELDS if name not in body]
        if missing:
            raise InvalidRequest(f"the scenario needs {', '.join(missing)}")
        der = typed_field(body, "der", dict)
        if der is not None:
            unknown = sorted(set(der) - DER_COLUMNS)
            if unknown:
                raise InvalidRequest(f"der has no {', '.join(unknown)}")
        der_connected = typed_field(body, "der_connected", bool)
        der_operating = typed_field(body, "der_operating", bool)
        if der_connected is None or der_operating is None:
            raise InvalidRequest("der_connected and der_operating must be true or false")
        powers = {name: typed_field(body, name, (int, float)) for name in POWERS}
        if any(value is None for value in powers.values()):
            raise InvalidRequest(f"{', '.join(POWERS)} must be numbers")
        if any(powers[name] < 0 for name in UNSIGNED_POWERS):
            raise InvalidRequest(f"{', '.join(UNSIGNED_POWERS)} cannot be negative")
        check_battery(der, powers["battery_w"])
        return cls(der=der, der_connected=der_connected, der_operating=der_operating,
                   **{name: float(value) for name, value in powers.items()},
                   pin=typed_field(body, "pin", int), nmi=typed_field(body, "nmi", str))


def check_battery(der: dict[str, Any] | None, battery_w: float) -> None:
    """A battery's power must be within the DER's rate for it: discharging
    up to rtg_max_discharge_rate_w, charging up to rtg_max_charge_rate_w."""
    if battery_w == 0:
        return
    rate = "rtg_max_discharge_rate_w" if battery_w > 0 else "rtg_max_charge_rate_w"
    if der is None:
        raise InvalidRequest(f"battery_w is {battery_w:g}, but the site has no DER")
    limit = der.get(rate)
    if limit is None:
        raise InvalidRequest(f"battery_w is {battery_w:g}, but the DER has no {rate}")
    if abs(battery_w) > limit:
        raise InvalidRequest(f"battery_w is {battery_w:g}, beyond the DER's {rate} of {limit}")
