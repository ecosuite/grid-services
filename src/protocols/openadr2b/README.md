# OpenADR 2.0b

For each `openadr2b` connection the worker acts as a virtual VEN. It uses
simple HTTP pull, profile 2.0b, and is built on EPRI's liboadr. XML
signatures aren't supported.

## Settings

Each connection has one `openadr2b_connections` row, inserted in the same
transaction as the connection.

| Column | Meaning |
| --- | --- |
| `vtn_url` | The VTN's base URL. You can't change it while the connection is registered |
| `ven_name` | The VEN name sent when registering |
| `ven_id` | The VEN id, if the VTN operator gave you one. Otherwise the VTN assigns it |
| `market_context` | The program's market context. Events for any other are refused |
| `ca_cert_pem`, `verify_server_certificate` | How the VTN's certificate is checked |
| `accepted_signals` | Signals the site can act on: `SIMPLE` (the default), `ELECTRICITY_PRICE` and `LOAD_DISPATCH` |

A client certificate is optional. Give the connection one through
`client_credentials` if the VTN wants it.

## Events

New events get an `oadrCreatedEvent` answer using the connection's
`default_opt`, or opt out while the connection is paused. When an operator
changes their mind later, the worker sends an `oadrCreateOpt`. Opting out
works until the event ends, and opting in until it starts.

Answering opt in is a commitment, so only add a signal to
`accepted_signals` once Ecosuite can actually act on it.

The worker refuses events it can't act on and doesn't store them. That
covers events for another VEN or market context, signals it doesn't accept,
missing or inconsistent data, and events that have already ended. Each
refusal uses the response code the spec gives for it.

The worker compares every distribution against the database, so a restart
loses nothing. An event that drops out of the list is marked completed if
its time has passed, and cancelled otherwise. The worker doesn't randomize
start times itself: it stores `startafter` as `start_after_seconds` and
leaves the choice to Ecosuite.

## Reports

Reporting works as an offer and a request. Ecosuite sets up what the VEN
offers in `openadr2b_report_offers`, one row per report, each listing the
telemetry points it carries. The VTN then asks for what it wants, and each
request becomes an `openadr2b_report_requests` row. The worker handles
periodic, one-shot and history requests, and records every report it sends
in `report_deliveries`.

`TELEMETRY_STATUS` is always offered, and shows a manual override while the
connection is paused. `TELEMETRY_USAGE` and `HISTORY_USAGE` must follow the
alliance's profile, so for anything else use a report name of the
program's own. An offer the worker can't express makes the configuration
invalid.

Requests belong to a registration. After a restart, re-registration or
configuration change the worker registers its reports again and the VTN
asks again.

## Registration and polling

The worker polls at the interval the VTN asks for. A connection's
`poll_interval_seconds` can make that longer but never shorter, and
`poll_jitter` adds a random delay to each wait. When the VTN sends a
message the worker polls again straight away.

If the VTN cancels the registration, an operator needs to register again.
If it asks for a re-registration, the worker does that itself. HTTP 404 or
406 from the VTN is treated as permanent and not retried.

`connection_state.client_id` is the VEN id the VTN confirmed. Show it as
`VEN <id>`.

## Testing

[`test-servers/openadr2b`](../../../test-servers/openadr2b/README.md) has a
VTN to try the worker against, and the end-to-end suite.
