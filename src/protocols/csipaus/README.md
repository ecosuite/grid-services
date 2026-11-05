# CSIP-AUS

For each `csipaus` connection the worker acts as a utility client: an IEEE
2030.5 EndDevice speaking the Common Smart Inverter Profile for Australia,
v1.1a and v1.2. It registers the site, keeps time with the server, stores
the server's controls as events, works out which caps are in force, and
reports the site's DER and readings back.

## Settings

Each connection has one `csipaus_connections` row, inserted in the same
transaction as the connection.

| Column | Meaning |
| --- | --- |
| `dcap_url` | The server's DeviceCapability URL, from the utility's interconnection handbook. The worker finds everything else from there. You can't change it while the connection is registered |
| `ca_cert_pem`, `verify_server_certificate` | How the server's certificate is checked |
| `connection_point_id` | The site's NMI, sent on registration. Leave it null to send none |
| `registration_pin` | The PIN the utility gave you. Leave it null to skip the check |

A client certificate is required, because the server knows the device by
it. The certificate's LFDI becomes `connection_state.client_id`. Show it in
groups of four, as IEEE 2030.5 does: `LFDI 3E4F-45AB-…`. Some utilities ask
for the SFDI instead, which is derived from the LFDI. A new certificate
means a new LFDI, so the server will see a new device.

## Registering

Registering looks through the server's EndDeviceList for our LFDI and
creates an EndDevice if there isn't one. It then checks the PIN, if the
server gives a Registration, and sends the NMI. Some utilities only add
devices themselves, out of band. They refuse the creation, and the command
fails until the utility has added the device. The `test` command only
looks for the EndDevice and never creates one, so it's a good first step.

CSIP-AUS gives a client no way to remove an EndDevice. Unregistering,
disabling and archiving only stop the worker polling. The utility keeps the
device and its controls until it removes them, so ask it to.

If the server loses the EndDevice, as some do routinely, the worker
registers it again by itself and the activity log says so.

## Polling

Each poll walks from DeviceCapability down to every program's default
control and control list. Each resource is fetched only when its own
`pollRate` says it's due, and 900 seconds is the default when the server
doesn't give one. The worker keeps the server's rates exactly and doesn't
add jitter, since CSIP-AUS servers spread their clients out themselves.

After a failure the worker retries as the CSIP-AUS explainer asks: after
10 seconds, then after 20, then at the poll rate.

The worker also keeps the server's time. `connection_state.clock_offset_seconds`
is how far ahead of the worker the server's clock is, and every time the
worker stores for CSIP-AUS is the server's.

## Controls as events

Each DERControl becomes an event keyed by its mRID, with each cap it sets
as a `limit` signal. Each program's DefaultDERControl becomes an
open-ended event too. Details such as the program, its primacy and the
control's randomized window go in `csipaus_events`.

Controls are network limits, so they can't be declined. `opt_out` and
`opt_in` are refused, and pausing a connection is only recorded. What
pausing means for the site is up to Ecosuite.

When a control asks for responses, the worker tells the server when it was
received, cancelled or superseded. It also says when the site started and
completed it. Those two follow what the site actually did, as Ecosuite
records it in the event's `lifecycle`, not the clock. A site that never
reports applying a control never says it started.

## Caps in force

After each poll the worker works out every cap's value over time and
writes it to `csipaus_limits`, then signals `grid_services_limits`.
Ecosuite applies those rows to the site.

For each cap, the control in force with the lowest program primacy wins,
and the newest one breaks ties. Only controls that set a cap compete for
it. Where no control applies, the default of the lowest-primacy program
that sets the cap holds. The worker applies its own randomization to each control's start
and end, derived from its LFDI so it stays the same across restarts.

If the worker loses contact with the server, or restarts, the stored caps
keep running: the controls, then the defaults until further notice.

Each row also carries the ramp into it, either the control's `rampTms` as
`ramp_seconds` or the default's `setGradW` as `set_grad_w`. Ecosuite
doesn't need any CSIP-AUS ramp rules of its own.

## Reporting

Between polls the worker tells the server about the site, using what the
API writes in `csipaus_der` and `csipaus_readings`. Every document it
sends, or tries to, is a `report_deliveries` row.

`csipaus_der` describes the site's DER as one device: its type, ratings,
settings, ramp rate and the caps it can act on. The worker sends it to the
server as DERCapability and DERSettings.

The DER's status comes from two telemetry points of kind `state`, bound
from the inverter's own status: whether it's connected and whether it's
operating. Each should read 1 or 0. The worker never guesses status from
power, so with either point unbound no DERStatus is sent. When the DER
disconnects from the grid, the worker fetches the programs again straight
away.

`csipaus_readings` says which telemetry points to post as readings: real
and reactive power for the site and the DER, plus voltage and frequency.
Site readings are positive for import and DER readings positive for
generation, so set each point's `multiplier` to match. Power goes out as a
16-bit number times a power of ten, so set `power_of_ten` from the site's
size. At the default of 1, values go in tens of watts, up to 327 kW.

Readings are averaged over windows of the server's post rate, and each
window is posted once SolarNetwork has all its data. A window the server
doesn't take is retried with later ones.

Set `GRID_SERVICES_CSIPAUS_PEN` to your IANA Private Enterprise Number. It
goes into the mRIDs of the worker's mirror usage points and readings.

## Errors and TLS

The worker reads server documents with the bindings in
[`vendor/csipaussd`](../../../vendor/csipaussd). It refuses any document
with an element the bindings don't know, because skipping it silently
could hide a limit. That's usually a server speaking another version, such
as CSIP-AUS v1.3. Refusals, server errors, 429s and 404s are all retried.
Other 4xx responses need an operator.

TLS is 1.2 with the connection's certificate. The worker offers
`ECDHE-ECDSA-AES128-CCM8`, the suite IEEE 2030.5 requires, then the common
AES-GCM suites for servers behind TLS terminators.

## Testing

[`test-servers/csipaus`](../../../test-servers/csipaus/README.md) is a
small utility server to try the worker against by hand, and
[`test-servers/cactus`](../../../test-servers/cactus/README.md) runs ANU's
CACTUS conformance tests.
