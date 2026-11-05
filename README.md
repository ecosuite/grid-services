# grid-services

[![CI](https://github.com/ecosuite/grid-services/actions/workflows/ci.yml/badge.svg)](https://github.com/ecosuite/grid-services/actions/workflows/ci.yml)

grid-services connects Ecosuite sites to utility and demand response
programs. It is a single worker process. For each site it registers with
the program, polls it for events and controls, answers them, and sends back
the site's telemetry.

It speaks two protocols:

- [OpenADR 2.0b](src/protocols/openadr2b/README.md), as a virtual VEN.
- [CSIP-AUS](src/protocols/csipaus/README.md) v1.1a and v1.2, the
  Australian profile of IEEE 2030.5, as a utility client. We test it
  against ANU's CACTUS conformance harness.

The worker talks to the rest of Ecosuite only through Postgres. It has no
inbound ports, and everything it needs to pick up where it left off is in
the database, so you can restart it at any time.

## How it works

A supervisor watches the `connections` table and keeps one runner thread
going for each enabled connection. A runner carries out its connection's
commands, polls the program, and keeps `connection_state` up to date. The
protocol details live in an adapter, which turns what the program sends
into the shared tables. Each runner holds a Postgres advisory lock on its
connection, so two workers never run the same one.

The worker stops at "event written to the database". Deciding what a site
should do about an event, and telling the site to do it, is Ecosuite's job.

Telemetry comes from SolarNetwork. Client keys and SolarNetwork tokens are
sealed with the worker's public key before they go into the database, and
only the worker can open them.

The code is laid out like this:

- `src/core` has the protocol-neutral pieces: the runner, the event sink,
  and the adapter and store interfaces. It depends on neither Postgres nor
  any protocol, and the build enforces that.
- `src/protocols/<protocol>` has one protocol's adapter and its tables.
- `src/store` is all of the SQL.
- `src/datum` reads SolarNetwork, and `src/crypto` handles sealed secrets.
- `db/migrations` is the database schema, as dbmate migrations.
- `tests` has the unit tests (with an in-memory store) and the Postgres
  integration tests.
- `test-servers` has a test server for each protocol, the end-to-end
  suites, and the CACTUS harness.
- `vendor` has EPRI's liboadr, the IEEE 2030.5 and CSIP-AUS bindings, and
  the CodeSynthesis XSD runtime. We maintain all of it ourselves.

## The database

The worker owns the database, and the migrations are its contract with
Ecosuite. Ecosuite's API writes connections, their settings, credentials,
telemetry points and commands. The worker writes connection state, events,
answers, activity and report deliveries. The migrations comment the tables
and their less obvious columns, so `\d+` in psql is a good reference.

A few rules for clients:

- Insert a connection and its protocol settings in one transaction.
- Change configuration by updating rows. A trigger tells the worker, and it
  restarts the connection.
- Seal client keys and SolarNetwork secrets with libsodium's
  `crypto_box_seal` and the worker's public key, as base64.
  `grid-services seal PUBLIC_KEY < key.pem` does the same thing.
- Send commands (`register`, `unregister`, `test`, `pause`, `resume`,
  `opt_out`, `opt_in`, `reload_config`) by inserting into
  `connection_commands`, and wait for the result by listening or polling.
  By default, a command still pending after two minutes fails.
- Retire a connection by archiving it, or stop it for now by disabling it.
  Either way the worker unregisters it first, where the protocol allows.
- Read events through the `event_state` view, and health through
  `connection_health`.

`site_id` is Ecosuite's site id and the key everything joins on.
`site_code` is only for display, because codes repeat across projects.

The worker sends Postgres notifications when things change. Each one's
payload is a single id:

| Channel | Sent when |
| --- | --- |
| `grid_services_config` | A connection or its settings changed |
| `grid_services_commands` | A command was inserted |
| `grid_services_command_results` | A command finished |
| `grid_services_events` | An event changed or was deleted |
| `grid_services_health` | A connection's health may have changed |
| `grid_services_reports` | A report request started or ended, or a report was sent |
| `grid_services_limits` | A CSIP-AUS connection's caps changed |

Notifications aren't queued, so read the tables once after you connect.

## Running it locally

You need Docker. From the repository root:

```sh
docker compose up --build                                            # Postgres, migrations, worker and test servers
docker compose --profile test run --rm test                          # unit and integration tests
TEST_VTN_REPORT_SECONDS=5 docker compose --profile e2e run --rm e2e  # OpenADR end-to-end tests
docker compose run --rm worker grid-services keygen                  # a new worker key pair
```

Postgres is on `127.0.0.1:5433` (database, user and password are all
`grid_services`). The OpenADR test VTN is on <http://127.0.0.1:8080> and
the CSIP-AUS test server on <http://127.0.0.1:8081>.

The integration tests drop the schema in `GRID_SERVICES_TEST_DATABASE_URL`,
so never point that at a real database.

## Configuration

| Variable | Purpose |
| --- | --- |
| `GRID_SERVICES_DATABASE_URL` | libpq connection string. Required |
| `GRID_SERVICES_SECRET_KEY` | The worker's secret key, for opening sealed secrets |
| `GRID_SERVICES_WORKER_ID` | Recorded in `connection_state.worker_id`. Defaults to the hostname |
| `GRID_SERVICES_LOG_LEVEL` | `trace`, `debug`, `info`, `warn` or `error` |
| `GRID_SERVICES_CSIPAUS_PEN` | Your IANA Private Enterprise Number, used in CSIP-AUS reading mRIDs. Defaults to 0 |

## Deploying

We deploy on Render, and `render.yaml` describes it. `master` deploys to
Production and `stage` to Test once CI passes, and Test also runs both
test servers. Anywhere else, the `Dockerfile` builds the worker image,
which includes dbmate and the migrations. Run the migrations in that image
before each new version starts:

```sh
dbmate --env=GRID_SERVICES_DATABASE_URL --migrations-dir=/app/db/migrations --no-dump-schema up
```

For each environment, once:

1. Generate a key pair with `grid-services keygen`. Give the worker the
   secret key and the API the public key.
2. Create the role the API logs in as. It must not be a member of the role
   that owns the tables, because that's how the triggers tell the worker
   apart from a client.

   ```sql
   CREATE ROLE ecosuite_api LOGIN PASSWORD '...';
   GRANT USAGE ON SCHEMA public TO ecosuite_api;
   GRANT SELECT, INSERT, UPDATE, DELETE ON connections, client_credentials, openadr2b_connections,
       csipaus_connections, telemetry_points, openadr2b_report_offers, connection_data_credentials,
       csipaus_der, csipaus_readings, events TO ecosuite_api;
   GRANT SELECT, INSERT ON connection_commands, activity TO ecosuite_api;
   GRANT SELECT ON connection_state, connection_health, event_state, event_responses,
       csipaus_events, csipaus_limits, openadr2b_report_requests, report_deliveries
       TO ecosuite_api;
   GRANT USAGE ON ALL SEQUENCES IN SCHEMA public TO ecosuite_api;
   ```

   Extend the grants when a migration adds a table.

To add a migration, create a new timestamped file in `db/migrations` with
`-- migrate:up` and `-- migrate:down` sections. Never edit one that has
already run somewhere shared.

## Tests

CI runs these on every push and pull request:

- `liboadr_tests` and `csipaus_tests`, for the vendored library and the
  CSIP-AUS bindings.
- `grid_services_tests`, the worker's unit tests and the Postgres
  integration tests, which also run every migration up and down.
- The end-to-end suite in `test-servers/openadr2b`, and the CSIP-AUS test
  server's own tests.

The [CACTUS harness](test-servers/cactus/README.md) takes much longer, so
you run it by hand.

Some checks validate documents against IEEE's `sep.xsd`. IEEE doesn't
allow it to be redistributed, so it isn't in this repository, and those
checks are skipped when it's missing.

## License

grid-services is released under the [MIT License](LICENSE). The vendored
code keeps its own licenses: liboadr is BSD 3-Clause
([EPRI_LICENSE.txt](EPRI_LICENSE.txt)), and the CodeSynthesis XSD runtime
is GPLv2 with the FLOSS exception in `vendor/xsd-4.0.0/`.
