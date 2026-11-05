"""The site simulator's control API (site_sim/), for a run to give the site
its scenario and read back what it did.

    connection_id = provision(base_url, Scenario())
    operate(base_url, "isolate")
    operate(base_url, "unmanaged_load", {"watts": 1000})
    trace(base_url)["datums"]
"""

from __future__ import annotations

from dataclasses import asdict, dataclass, field
from typing import Any

import httpx

from test_cactus import nmi

# A 5 kW photovoltaic inverter that acts on every CSIP-AUS cap, as ecosuite
# describes it in csipaus_der.
PV5K: dict[str, Any] = {
    "der_type": 4,
    "rtg_max_w": 5000, "rtg_max_va": 5000, "rtg_max_var": 3000, "rtg_max_var_neg": 3000,
    "rtg_v_nom": 230,
    "modes_supported": ["export", "import", "generation", "load", "energize", "connect"],
    "modes_enabled": ["export", "import", "generation", "load", "energize", "connect"],
}
# PV5K with a 5 kW battery (type 83, combined PV and storage). The worker
# sends the charge and discharge rates as setMaxChargeRateW and
# setMaxDischargeRateW, which the runner's maxImportW and maxExportW are.
PV5K_BAT5K: dict[str, Any] = {
    **PV5K,
    "der_type": 83,
    "rtg_max_charge_rate_w": 5000, "rtg_max_discharge_rate_w": 5000,
}
# Envoy's STATIC_REGISTRATION_PIN, 11111, with its check digit: the PIN the
# tests' instructions give for EndDevices registered out of band.
PIN = 111115
# The connection point: an NMI with its checksum, 11 characters, as the
# explainer (10.1) asks.
NMI = nmi.with_checksum("4407000000")


@dataclass(frozen=True)
class Scenario:
    """The site's DER (None for none), whether its inverter says it is
    connected and operating, the PV it has, the unmanaged and managed loads
    it takes, its battery's power (positive discharging, negative charging),
    and the PIN and connection point its connection checks and sends."""
    der: dict[str, Any] | None = field(default_factory=lambda: dict(PV5K))
    der_connected: bool = True
    der_operating: bool = True
    available_pv_w: float = 3000
    unmanaged_load_w: float = 1000
    managed_load_w: float = 0
    battery_w: float = 0
    pin: int | None = PIN
    nmi: str | None = NMI


def provision(base_url: str, scenario: Scenario, timeout: float = 30) -> int:
    """Provisions the site as ecosuite would and asks the worker to register
    it; returns its connection id."""
    response = httpx.post(f"{base_url.rstrip('/')}/scenario", json=asdict(scenario),
                          timeout=timeout)
    if response.is_error:
        raise RuntimeError(f"the site simulator refused the scenario: "
                           f"{response.status_code} {response.text}")
    return response.json()["connection_id"]


def operate(base_url: str, act: str, body: dict[str, Any] | None = None,
            timeout: float = 30) -> dict[str, Any]:
    """Does an operator's act on the site; returns whether the DER is then
    connected and operating, and the unmanaged load."""
    response = httpx.post(f"{base_url.rstrip('/')}/operator/{act}", json=body,
                          timeout=timeout)
    if response.is_error:
        raise RuntimeError(f"the site simulator refused to {act}: "
                           f"{response.status_code} {response.text}")
    return response.json()


def trace(base_url: str, timeout: float = 30) -> dict[str, Any]:
    """The scenario, the datums written, the operator's acts, the planner's
    moves, the SolarNetwork requests seen, the plant at each datum, and the
    checks of it with their warnings."""
    return httpx.get(f"{base_url.rstrip('/')}/trace", timeout=timeout).raise_for_status().json()
