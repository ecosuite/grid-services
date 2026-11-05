# test-common

Code shared by the test servers and their end-to-end suites. Each server
pulls it in as the `test-common` package, through a path dependency in its
`pyproject.toml`. The modules are in `test_common/`.

| Module | Contents |
| --- | --- |
| `control.py` | Plumbing for the control APIs: the bearer token check, JSON errors and body parsing |
| `solarnetwork.py` | A fake SolarNetwork datum API for the worker to read telemetry from |
| `e2e.py` | The suites' side of the database, a control API client, and a client for the fake SolarNetwork |
| `fixtures.py` | A pytest plugin with the `timeout` and `listen` fixtures |
| `certificates.py` | Self-signed IEEE 2030.5 device certificates, with their LFDI and SFDI |

## The fake SolarNetwork

A test server mounts it beside its own routes, so the worker reaches it at
the server's address:

```python
solarnetwork = FakeSolarNetwork()
app.add_routes(solarnetwork.routes())          # what the worker reads
app.add_routes(solarnetwork.control_routes())  # what the suite feeds it
```

The worker reads `/solarquery/api/v1/sec/datum/list` and `.../mostRecent`
with SNWS2-signed requests. The fake checks each signature against the
tokens the suite registered, and records every request.

| Method and path | Effect |
| --- | --- |
| `PUT /api/solarnetwork/tokens/{token}` | `{"secret"}` registers a token |
| `PUT /api/solarnetwork/streams` | `{"node_id", "source_id", "datums"}` replaces a stream's datums |
| `GET /api/solarnetwork/requests` | Every request the worker made, and whether its signature checked out |

## Writing a suite

Load the shared fixtures with `pytest_plugins = ["test_common.fixtures"]`.
Then subclass `Api` and `Connection` from `test_common.e2e` for your
protocol's tables, and `ControlClient` for your server's control API. The
`e2e` extra installs what the suites need.
