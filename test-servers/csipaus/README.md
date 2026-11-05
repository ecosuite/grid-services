# test-csipaus

A small CSIP-AUS utility server for trying grid-services by hand. Point a
connection at it, send controls from its web page, and watch what the
worker and Ecosuite do with them.

This isn't a conformance test; [CACTUS](../cactus/README.md) does that.
The server covers what a CSIP-AUS client walks and leaves the rest of IEEE
2030.5 out. In exchange, it's easy to control and to see into.

## Running

From the repository root:

```sh
docker compose up --build test-csipaus
```

The page and control API are on <http://127.0.0.1:8081>. Give a connection
this `dcap_url`:

```
http://test-csipaus:8080/d/<client name>/dcap
```

A real server knows its clients by their certificate. This one runs without
TLS, so the `/d/<client name>` part of the path does that job instead.

### Making a connection

A CSIP-AUS connection needs a client certificate even here. `connect.py`
makes one for a site and writes the connection to the database the way the
API would:

```sh
docker compose up --build
uv run --project test-servers/csipaus test-servers/csipaus/connect.py site-1 --register
```

It prints the LFDI and SFDI, and keeps the certificate in
`test-servers/csipaus/certs/` so the site keeps its LFDI from run to run.
Run it with `--help` for the other options, such as `--nmi`, `--pin` and
`--new-certificate`. A site can only have one live connection, so archive
the old one before connecting the same site again.

On Render, the Test environment runs it as `grid-services-test-csipaus`.
Connections there use `https://<its host>/d/<client name>/dcap`, and the
control API needs the token in `TEST_CSIPAUS_CONTROL_TOKEN`.

## What it does

It keeps everything in memory, so a restart forgets every device. A client
then finds its EndDevice gone and registers again.

- Clients register EndDevices, each with a Registration PIN and a
  connection point.
- Each device gets one program. From the control API you set its default
  control and schedule or cancel controls with export, import, generation
  and load limits, connect and energize.
- It records the client's responses to controls, the DER documents it
  sends, and the readings it posts.
- It also serves the fake SolarNetwork from [test-common](../common/README.md).

There are no subscriptions, so clients poll. Supersession isn't modelled.
Every exchange is logged with both documents.

## Configuration

| Variable | Default | Purpose |
| --- | --- | --- |
| `TEST_CSIPAUS_PORT`, `PORT` | `8080` | HTTP port |
| `TEST_CSIPAUS_POLL_SECONDS` | `60` | `pollRate` given to clients. Compose uses 5 |
| `TEST_CSIPAUS_POST_SECONDS` | `60` | `postRate` given to mirror usage points |
| `TEST_CSIPAUS_PIN` | `111115` | PIN in each new Registration |
| `TEST_CSIPAUS_TIME_ZONE` | `Australia/Sydney` | Time zone the `Time` resource reports |
| `TEST_CSIPAUS_CONTROL_TOKEN` | unset | Bearer token for `/api`. Unset leaves the API open |
| `TEST_CSIPAUS_CLIENT_NAME_PREFIX` | empty | Client names must start with this |
| `TEST_CSIPAUS_LOG_LEVEL` | `info` | `debug`, `info`, `warning` or `error` |

## Control API

The page uses a JSON API under `/api`, which you can also call directly.
Clients are addressed by name, and devices by id, LFDI or SFDI.

| Method and path | Effect |
| --- | --- |
| `GET /api/clients` | Every client that has called |
| `GET /api/clients/{client}/devices/{device}` | A device's registration, DER, default, controls and responses |
| `PATCH /api/clients/{client}/devices/{device}` | `{"pin", "primacy"}` |
| `PUT /api/clients/{client}/devices/{device}/default-control` | `{"base": {...}}` sets the default |
| `POST /api/clients/{client}/devices/{device}/controls` | Schedules a control |
| `POST /api/clients/{client}/devices/{device}/controls/{id}/cancel` | Cancels a control |
| `GET /api/clients/{client}/messages` | The exchange log |

A control looks like this. `base` is required, and the rest are defaults:

```json
{"start_in_seconds": 60, "duration_seconds": 300, "base": {"opModExpLimW": 0}}
```

`base` takes `opModExpLimW`, `opModImpLimW`, `opModGenLimW` and
`opModLoadLimW` in watts, and `opModConnect` and `opModEnergize` as
booleans. `test_csipaus/control.py` has the rest of the routes.

## Tests

`tests/` drives the server in process with a scripted client. If IEEE's
`sep.xsd` is in `specs/` or at `CSIPAUS_SEP_XSD`, every document the server
sends is also checked against it. IEEE doesn't allow `sep.xsd` to be
redistributed, so CI skips that check.

```sh
uv run pytest                                # here
docker compose run --rm test-csipaus pytest  # in the image
```

The server uses Python 3.12 and envoy-schema 1.x, with dependencies locked
in `uv.lock`. Its image is built from `test-servers/` so it can include
test-common:

```sh
docker build -f test-servers/csipaus/Dockerfile test-servers
```
