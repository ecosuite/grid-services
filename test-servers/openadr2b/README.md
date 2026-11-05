# test-vtn

An OpenADR 2.0b VTN for testing grid-services, and the end-to-end suite
that runs the worker against it.

The suite plays both outside parts. Toward the database it acts as
Ecosuite's API: it inserts connections and commands, then reads state,
events, opt answers and activity. Toward the VTN it acts as the utility,
sending, changing and cancelling events. It never talks to the worker
directly.

## Running

From the repository root:

```sh
docker compose up --build                  # worker, database and test VTN
docker compose --profile e2e run --rm e2e  # the end-to-end suite
```

The VTN's page and control API are on <http://127.0.0.1:8080>. Give a
connection this VTN URL:

```
http://test-vtn:8080/v/<ven name>
```

OpenADR's registration query doesn't carry a VEN id, so the `/v/<ven name>`
part is how the VTN knows that a restarted worker is still registered.

On Render, the Test environment runs it as `grid-services-test-vtn`.
Connections there use `https://<its host>/v/<ven name>`, and the control
API needs the token in `TEST_VTN_CONTROL_TOKEN`.

## What it does

It keeps everything in memory. After a restart, VENs are asked to register
again, and the events they held disappear, so the VENs close them.

- Any VEN can register.
- You create, change and cancel events through the control API. Each VEN
  gets its full list of open events on its next poll.
- It records opt answers from `oadrCreatedEvent` and `oadrCreateOpt`.
- It requests every telemetry report a VEN offers, at
  `TEST_VTN_REPORT_SECONDS`, and you can make or cancel other requests
  through the control API.
- It can queue a registration cancellation or a re-registration request.
- It serves the fake SolarNetwork from [test-common](../common/README.md)
  for the worker's reports.

Only simple HTTP pull is supported, with no TLS, client certificates or XML
signatures.

## Configuration

| Variable | Default | Purpose |
| --- | --- | --- |
| `TEST_VTN_ID` | `test-vtn` | vtnID sent to VENs |
| `TEST_VTN_PORT`, `PORT` | `8080` | HTTP port |
| `TEST_VTN_POLL_SECONDS` | `10` | Poll interval asked of VENs. Compose uses 2 |
| `TEST_VTN_REPORT_SECONDS` | `60` | Telemetry report interval asked for. The suite uses 5 |
| `TEST_VTN_MARKET_CONTEXT` | `http://test-vtn/program` | Market context of new events |
| `TEST_VTN_CONTROL_TOKEN` | unset | Bearer token for `/api`. Unset leaves the API open |
| `TEST_VTN_VEN_NAME_PREFIX` | empty | VEN names must start with this |
| `TEST_VTN_LOG_LEVEL` | `info` | `debug`, `info`, `warning` or `error` |

## Control API

JSON under `/api`. VENs are addressed by id or name.

| Method and path | Effect |
| --- | --- |
| `GET /api/vens` | Every VEN the VTN knows |
| `GET /api/vens/{ven}` | One VEN's registration, events, reports and opts |
| `POST /api/vens/{ven}/events` | Creates an event |
| `PATCH /api/vens/{ven}/events/{id}` | Changes an event |
| `POST /api/vens/{ven}/events/{id}/cancel` | Cancels an event |
| `POST /api/vens/{ven}/reports/request` | Requests a report |
| `POST /api/vens/{ven}/registration/cancel` | Cancels the registration |
| `POST /api/vens/{ven}/registration/reregister` | Asks the VEN to register again |
| `GET /api/vens/{ven}/messages` | The exchange log |

An event with every field left at its default:

```json
{
  "start_in_seconds": 60,
  "signals": [{"name": "SIMPLE", "type": "level",
               "intervals": [{"duration_seconds": 300, "value": 1}]}]
}
```

A single interval with `duration_seconds` of 0 makes an open-ended event.
`test_vtn/control.py` has the rest of the routes and fields.

## The suite

`e2e/test_registration.py`, `e2e/test_events.py` and `e2e/test_reports.py`
cover registration, events and opting, and reports. Each test makes its own
connection and VEN with unique names and deletes them afterwards, so the
suite is safe to run against a database and worker other people are using.
Deleting connections needs more than the API role's grants, though. The
suite is configured through the environment; see `e2e/conftest.py`.

## Why not OpenLEADR's server

The VTN uses [OpenLEADR](https://github.com/OpenLEADR/openleadr-python)
for schema validation, parsing and message templates, but the VTN logic is
our own. OpenLEADR's server rewrites every response code to 200, has no
EiOpt handler, leaves `reportInterval` out of periodic report requests,
forgets cancellations when it recomputes an event's status, and zeroes
`currentValue` on active SIMPLE signals. Any of those would hide failures
or cause them.

The VTN uses Python 3.12, with dependencies locked in `uv.lock`. Its image
is built from `test-servers/` so it can include test-common:

```sh
docker build -f test-servers/openadr2b/Dockerfile test-servers
```
