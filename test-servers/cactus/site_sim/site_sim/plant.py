"""The site's plant: its DER and its loads, behind the connection point.

The plant wants what the scenario gives it: the PV available, the
battery's power (positive discharging, negative charging), the managed
load and the unmanaged load. Generation G is PV plus battery discharge;
consumption C is battery charging plus the managed load. While the DER is
not both connected and operating, PV and battery are 0.

The planner applies the caps in force, which the plant keeps by their
csipaus_limits names, and the plant follows export, import, generation and
load by cutting what it wants, never adding to it: the generation cap
bounds G - C, the export cap G - C - u, the load cap C - G and the import
cap u + C - G, where u is the unmanaged load, which is never controlled. A
cut to G takes battery discharge before PV, and a cut to C battery
charging before the managed load. It keeps max_output without following
it.

A followed cap's limit ramps as its row says (explainer 6.1): when its
value goes up, the limit moves from the one in force then, a ramp under
way included, to the new value, over ramp_seconds, or at set_grad_w
hundredths of a percent of setMaxW a second (set_max_w, else rtg_max_w, as
DERSettings sends it). A ramp of 0, a row with neither, or set_grad_w on a
DER without a rating, steps at once. A value going down applies at once,
since anything under a tighter limit complies, and a cap that goes away is
lifted at once. A new ramp on an unchanged value changes nothing. What the
plant does is a function of the time.

The plant also follows energize and connect, the switches (explainer 2,
3.1). A switch at 0 applies at once: energize 0 stops the DER operating,
connect 0 disconnects it and stops it operating, and under either the
managed DER (PV, battery and managed load) neither generates nor consumes.
A switch lifted, to 1 or by its cap going away, returns 60 s later if it
is still lifted (explainer 5.3.1): the DER's state comes back with its
power, which ramps from 0 W toward setMaxW, bounding G and C each, at the
setGradW in force, as an NZS 4777.2 inverter does; it steps when no
setGradW is in force, or on a DER without a rating.

The operator can isolate and reconnect the DER, stop and resume its power,
and set the unmanaged load; each act shows in the next datum. Isolating
also stops it operating, as a real inverter would, and reconnecting
restores both, its power ramping from 0 W as a switch's return does. The
DER's state is the operator's and the switches'.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field, replace
from datetime import datetime, timedelta

from site_sim.scenario import Scenario

NOMINAL_VOLTAGE = 230.0
FOLLOWED = ("export", "import", "generation", "load")
SWITCHES = ("energize", "connect")
# The operator's reconnection, whose return ramps as a switch's does.
RECONNECT = "reconnect"
RETURNS = (*SWITCHES, RECONNECT)
# How long a lifted switch waits before the DER returns (AS/NZS 4777.2).
RETURN_AFTER = timedelta(seconds=60)
# setGradW is in hundredths of a percent of setMaxW a second.
GRAD_W_SCALE = 10000


@dataclass(frozen=True)
class Ramp:
    """A limit moving up from `start_w` at `at` to `end_w`, over `seconds`,
    or at `rate_w_per_s`."""
    at: datetime
    start_w: float
    end_w: float
    seconds: float | None = None
    rate_w_per_s: float | None = None

    def limit(self, now: datetime) -> float:
        elapsed = max((now - self.at).total_seconds(), 0.0)
        if self.seconds is not None:
            return self.start_w + (self.end_w - self.start_w) * min(elapsed / self.seconds, 1.0)
        assert self.rate_w_per_s is not None
        return min(self.end_w, self.start_w + self.rate_w_per_s * elapsed)


@dataclass(frozen=True)
class Switched:
    """A switch the caps turned off, and, once lifted, when the DER returns
    and the ramp its power returns at, None for a step."""
    off_at: datetime
    returns_at: datetime | None = None
    ramp: Ramp | None = None

    def off(self, at: datetime) -> bool:
        return self.returns_at is None or at < self.returns_at

    def ramping(self, at: datetime) -> bool:
        return (not self.off(at) and self.ramp is not None
                and self.ramp.limit(at) < self.ramp.end_w)


@dataclass(frozen=True)
class Wants:
    """What the plant would do uncapped, in W."""
    pv_w: float
    battery_w: float
    managed_w: float
    unmanaged_w: float


@dataclass(frozen=True)
class Output:
    """What the controllable DER does under the caps, in W, the battery
    positive discharging."""
    pv_w: float
    battery_w: float
    managed_w: float


@dataclass(frozen=True)
class Measurement:
    """What the meters see, in CSIP-AUS's signs: the site positive for
    import, the DER positive for generation; what the inverter says of its
    own state; and the plant's breakdown behind them."""
    site_w: float
    site_var: float
    site_v: float
    der_w: float
    der_var: float
    der_connected: bool
    der_operating: bool
    pv_w: float
    battery_w: float
    managed_w: float
    unmanaged_w: float

    @property
    def generation_w(self) -> float:
        return self.pv_w + max(self.battery_w, 0.0)

    @property
    def consumption_w(self) -> float:
        return self.managed_w + max(-self.battery_w, 0.0)


def bound(caps: dict[str, float], name: str, offset: float) -> float:
    return caps[name] + offset if name in caps else math.inf


def dispatch(wants: Wants, caps: dict[str, float], producing: bool,
             ceiling_w: float = math.inf) -> Output:
    """The PV, battery and managed load the plant runs at under the caps,
    with generation and consumption each at most `ceiling_w`."""
    pv = wants.pv_w if producing else 0.0
    battery = wants.battery_w if producing else 0.0
    u = wants.unmanaged_w
    surplus_at_most = min(bound(caps, "generation", 0.0), bound(caps, "export", u))
    deficit_at_most = min(bound(caps, "load", 0.0), bound(caps, "import", -u))
    wanted_g = min(pv + max(battery, 0.0), ceiling_w)
    wanted_c = min(wants.managed_w + max(-battery, 0.0), ceiling_w)
    first_g = max(min(wanted_g, wanted_c + surplus_at_most), 0.0)
    c = max(min(wanted_c, first_g + deficit_at_most), 0.0)
    g = max(min(first_g, c + surplus_at_most), 0.0)
    pv_out = min(pv, g)
    managed_out = min(wants.managed_w, c)
    return Output(pv_w=pv_out, battery_w=(g - pv_out) - (c - managed_out),
                  managed_w=managed_out)


@dataclass
class Plant:
    wants: Wants
    der_connected: bool
    der_operating: bool
    # Whether it was operating when it was isolated, which reconnecting
    # restores.
    operating_when_isolated: bool = False
    # The caps applied: export, import, generation and load in W, max_output
    # in percent, energize and connect 1 or 0; and when each one's value
    # was applied, by the wall clock.
    caps: dict[str, float] = field(default_factory=dict)
    caps_since: dict[str, datetime] = field(default_factory=dict)
    # Each cap's ramp as its row gives it: ramp_seconds and set_grad_w.
    cap_ramps: dict[str, dict[str, float | None]] = field(default_factory=dict)
    # The followed caps whose limit is moving up.
    ramps: dict[str, Ramp] = field(default_factory=dict)
    # The switches the caps turned off, until the DER has returned.
    switched: dict[str, Switched] = field(default_factory=dict)
    # The W that set_grad_w is a share of: setMaxW as DERSettings sends it.
    # None without a DER rating.
    set_max_w: float | None = None
    # The setGradW in force, as the planner applies it; None for none.
    set_grad_w: float | None = None
    # The return ramp after the operator's reconnection.
    reconnected: Ramp | None = None

    @classmethod
    def for_scenario(cls, scenario: Scenario) -> Plant:
        der = scenario.der or {}
        set_max_w = der.get("set_max_w") if der.get("set_max_w") is not None \
            else der.get("rtg_max_w")
        return cls(wants=Wants(pv_w=scenario.available_pv_w, battery_w=scenario.battery_w,
                               managed_w=scenario.managed_load_w,
                               unmanaged_w=scenario.unmanaged_load_w),
                   der_connected=scenario.der_connected, der_operating=scenario.der_operating,
                   operating_when_isolated=scenario.der_operating,
                   set_max_w=float(set_max_w) if set_max_w is not None else None)

    def isolate(self) -> None:
        if self.der_connected:
            self.operating_when_isolated = self.der_operating
        self.der_connected = False
        self.der_operating = False
        self.reconnected = None

    def reconnect(self, at: datetime) -> None:
        if not self.der_connected:
            self.der_operating = self.operating_when_isolated
            self.reconnected = self.return_ramp(at)
        self.der_connected = True

    def stop_power(self) -> None:
        self.der_operating = False

    def resume_power(self) -> None:
        self.der_operating = True

    def set_unmanaged_load(self, watts: float) -> None:
        self.wants = replace(self.wants, unmanaged_w=watts)

    def apply_caps(self, caps: dict[str, float], ramps: dict[str, dict[str, float | None]],
                   set_grad_w: float | None, at: datetime) -> None:
        """Applies the caps in force with their rows' ramps, and the
        setGradW in force; a cap whose value is unchanged keeps the time it
        was applied and its limit."""
        moving = {name: self.ramp_into(name, value, ramps.get(name, {}), at)
                  if self.caps.get(name) != value else self.ramps.get(name)
                  for name, value in caps.items() if name in FOLLOWED}
        self.ramps = {name: ramp for name, ramp in moving.items() if ramp is not None}
        self.set_grad_w = set_grad_w
        switched = {name: self.switch(name, caps.get(name), at) for name in SWITCHES}
        self.switched = {name: state for name, state in switched.items() if state is not None}
        self.caps_since = {name: self.caps_since.get(name, at)
                           if self.caps.get(name) == value else at
                           for name, value in caps.items()}
        self.caps = dict(caps)
        self.cap_ramps = {name: dict(ramp) for name, ramp in ramps.items()}

    def ramp_into(self, name: str, value: float, ramp: dict[str, float | None],
                  at: datetime) -> Ramp | None:
        """How a followed cap's limit moves to its new value: a ramp when it
        goes up and its row ramps, else None, a step."""
        start_w = self.limit(name, at)
        if value <= start_w:
            return None
        return self.ramp(start_w, value, ramp, at)

    def ramp(self, start_w: float, end_w: float, ramp: dict[str, float | None],
             at: datetime) -> Ramp | None:
        """A rise from `start_w` to `end_w` from `at`, as a row's ramp says,
        or None when it steps."""
        seconds = ramp.get("ramp_seconds")
        grad_w = ramp.get("set_grad_w")
        if seconds:
            return Ramp(at, start_w, end_w, seconds=float(seconds))
        if grad_w and self.set_max_w:
            return Ramp(at, start_w, end_w,
                        rate_w_per_s=grad_w / GRAD_W_SCALE * self.set_max_w)
        return None

    def return_ramp(self, at: datetime) -> Ramp | None:
        """The DER's power coming back from 0 W at `at`, at the setGradW in
        force, or None when it steps."""
        if not self.set_grad_w or not self.set_max_w:
            return None
        return Ramp(at, 0.0, self.set_max_w,
                    rate_w_per_s=self.set_grad_w / GRAD_W_SCALE * self.set_max_w)

    def switch(self, name: str, value: float | None, at: datetime) -> Switched | None:
        """A switch's state once its cap is applied: off at 0; lifted, it
        returns RETURN_AFTER later, ramping at the setGradW in force when it
        returns; None once nothing holds it."""
        state = self.switched.get(name)
        if value == 0:
            # Put back to 0 before it returned, it has been off all along.
            return Switched(off_at=state.off_at if state is not None and state.off(at) else at)
        if state is None:
            return None
        if state.off(at):
            returns_at = state.returns_at if state.returns_at is not None else at + RETURN_AFTER
            return Switched(off_at=state.off_at, returns_at=returns_at,
                            ramp=self.return_ramp(returns_at))
        return state if state.ramping(at) else None

    def limit(self, name: str, at: datetime) -> float:
        """A followed cap's limit at `at`: its ramp's, while it moves, else
        its value; unbounded without the cap."""
        ramp = self.ramps.get(name)
        if ramp is not None:
            return ramp.limit(at)
        return self.caps.get(name, math.inf)

    def followed_caps(self) -> dict[str, float]:
        return {name: value for name, value in self.caps.items()
                if name in FOLLOWED or name in SWITCHES}

    def switched_off(self, name: str, at: datetime) -> bool:
        state = self.switched.get(name)
        return state is not None and state.off(at)

    def returning(self, at: datetime) -> dict[str, float]:
        """The limit at `at` of each switch's return, and of the operator's
        reconnection, still ramping."""
        switches = {name: state.ramp.limit(at) for name, state in self.switched.items()
                    if state.ramp is not None and state.ramping(at)}
        reconnected = ({RECONNECT: self.reconnected.limit(at)}
                       if self.reconnected is not None
                       and self.reconnected.limit(at) < self.reconnected.end_w else {})
        return {**switches, **reconnected}

    def ceiling(self, at: datetime) -> float:
        """What the switches let the managed DER generate and consume at
        `at`: nothing while one is off, a return's ramped limit, else no
        bound."""
        if any(self.switched_off(name, at) for name in SWITCHES):
            return 0.0
        return min(self.returning(at).values(), default=math.inf)

    def ramping(self, at: datetime) -> dict[str, float]:
        """The limit at `at` of each followed cap still ramping, and of each
        return that is."""
        limits = {name: ramp.limit(at) for name, ramp in self.ramps.items()}
        return {**{name: limit for name, limit in limits.items()
                   if limit < self.ramps[name].end_w}, **self.returning(at)}

    def measure(self, at: datetime) -> Measurement:
        connected = self.der_connected and not self.switched_off("connect", at)
        operating = (self.der_operating and connected
                     and not self.switched_off("energize", at))
        limits = {name: self.limit(name, at) for name in self.caps if name in FOLLOWED}
        output = dispatch(self.wants, limits, connected and operating, self.ceiling(at))
        der_w = output.pv_w + output.battery_w
        return Measurement(site_w=self.wants.unmanaged_w + output.managed_w - der_w,
                           site_var=0.0, site_v=NOMINAL_VOLTAGE, der_w=der_w, der_var=0.0,
                           der_connected=connected, der_operating=operating,
                           pv_w=output.pv_w, battery_w=output.battery_w,
                           managed_w=output.managed_w, unmanaged_w=self.wants.unmanaged_w)
