"""The site's SolarNode: the streams it uploads to SolarNetwork, and how
ecosuite binds them as telemetry points.

The site meter counts export positive, as many do, and the inverter counts
generation positive. Datums carry those raw signs; the points' multipliers
turn them into CSIP-AUS's, the site positive for import, as ecosuite's
readings sheet asks. The inverter also says whether it is connected and
operating, 1 or 0, which ecosuite binds as the DER's state.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from site_sim.plant import Measurement

NODE_ID = 1
SITE_METER = "/SITE/METER"
DER_INVERTER = "/DER/INV"
STALE_AFTER_SECONDS = 600


@dataclass(frozen=True)
class Point:
    """A telemetry point, and the CSIP-AUS reading it is posted as."""
    mirror: str
    reading: str
    kind: str
    units: str
    source_id: str
    property: str
    multiplier: float

    @property
    def point_id(self) -> str:
        """ecosuite's name for a reading's point, such as site-real-power."""
        return f"{self.mirror}-{self.reading.replace('_', '-')}"

    def source(self) -> dict[str, Any]:
        return {"node_id": NODE_ID, "source_id": self.source_id, "property": self.property,
                "property_class": "instantaneous", "reduce": "average",
                "multiplier": self.multiplier, "stale_after_seconds": STALE_AFTER_SECONDS}


@dataclass(frozen=True)
class StatePoint:
    """A telemetry point of kind state, 1 or 0, on the inverter's stream."""
    point_id: str
    property: str
    kind: str = "state"
    units: str = "none"

    def source(self) -> dict[str, Any]:
        return {"node_id": NODE_ID, "source_id": DER_INVERTER, "property": self.property,
                "property_class": "instantaneous", "reduce": "last",
                "multiplier": 1, "stale_after_seconds": STALE_AFTER_SECONDS}


POINTS = [
    Point("site", "real_power", "power_real", "W", SITE_METER, "watts", -1),
    Point("site", "reactive_power", "power_reactive", "var", SITE_METER, "reactivePower", -1),
    Point("site", "voltage", "voltage", "V", SITE_METER, "voltage", 1),
    Point("der", "real_power", "power_real", "W", DER_INVERTER, "watts", 1),
    Point("der", "reactive_power", "power_reactive", "var", DER_INVERTER, "reactivePower", 1),
]
CONNECTED = StatePoint("der-connected", "connected")
OPERATING = StatePoint("der-operating", "operating")
STATE_POINTS = [CONNECTED, OPERATING]


def datums(measurement: Measurement) -> dict[str, dict[str, float]]:
    """Each stream's datum for a measurement, by source id, with SolarNode's
    signs."""
    return {
        SITE_METER: {"watts": -measurement.site_w, "reactivePower": -measurement.site_var,
                     "voltage": measurement.site_v},
        DER_INVERTER: {"watts": measurement.der_w, "reactivePower": measurement.der_var,
                       CONNECTED.property: 1 if measurement.der_connected else 0,
                       OPERATING.property: 1 if measurement.der_operating else 0},
    }
