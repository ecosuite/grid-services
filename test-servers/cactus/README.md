# CACTUS

[CACTUS](https://github.com/bsgip/cactus-runner) is ANU's test harness for
CSIP-AUS clients. `run.py` runs its test procedures against the worker, a
few at a time, each on a Docker Compose stack of its own, and records how
each one went.

## Running

```sh
uv run --project test-servers/cactus test-servers/cactus/run.py ALL-01
uv run --project test-servers/cactus test-servers/cactus/run.py --required --jobs 10
uv run --project test-servers/cactus test-servers/cactus/run.py --class G,L --list
```

Name the tests you want, or pick them by class. `--required` runs every
class the worker certifies to: A, C, G, L, DER-A, DER-G and DER-L. Picking
by class skips tests that aren't for CSIP-AUS v1.2 and those longer than
two hours, such as ALL-25-EXT, so name those to run them. Each stack takes
about 450 MB. `--build` builds the worker image from this checkout first.
Run `run.py --help` for the rest of the options.

For each test, `run.py` starts a fresh stack: Postgres, the migrations, the
production worker image, a site simulator, and the CACTUS runner, envoy and
database. It provisions a site, starts the test, does whatever the test
asks of an operator, and waits until the test and every control it created
have finished. Then it saves the logs and takes the stack down. Use
`--keep failed` or `--keep always` to leave a stack up so you can look at
it.

Results go in `results/<batch>/`, which git ignores. Each test gets a
`run.log` of what happened, the worker's and the simulator's logs, the
simulator's trace, and the runner's archive. The batch gets
`summary.md` and `summary.json`. `run.py` exits 0 when every test passes.

## The site simulator

`site_sim/` stands in for everything on Ecosuite's side: the API, SolarNode
and SolarNetwork, and the site's equipment. It only reaches the worker the
way production does. It logs into the database as the `ecosuite_api` role,
with the grants from the root README, and serves the SolarNetwork API the
worker reads from.

For each test it writes the site's connection, certificate, DER and
telemetry points, then asks the worker to register. After that it publishes
the plant's readings every 5 seconds. The plant is a 5 kW PV inverter, with
a 5 kW battery for the LOA tests, plus managed and unmanaged load. Acting
as Ecosuite's planner, it applies the caps in force from `csipaus_limits`
as they change, ramping where the rows say to, and moves each control's
`lifecycle` along as the site starts and finishes obeying it.

The trace checks the plant on every reading and logs a warning when a cap
isn't met in time. Warnings never fail a test.

## Fixtures

```sh
uv run --project test-servers/cactus test-servers/cactus/capture.py test-servers/cactus/results/<batch>/ALL-01
```

`capture.py` copies the server's responses from a run's archive into
`tests/fixtures/csipaus/cactus/<TEST>/`, so the unit tests can replay them.
Request bodies and headers, including the client certificate, are left out.

## Differences from hosted CACTUS

The stack runs the same images and test definitions as hosted CACTUS (the
versions are in `compose.yaml` and `pyproject.toml`), with the same
settings, behind a similar nginx. A few things differ:

- The device certificate is self-signed, and the proxy accepts any
  certificate where the hosted one checks it against SERCA. The LFDI the
  tests see is still the certificate's.
- Notifications are off, because the worker doesn't take them.
- `run.py` makes the run requests that hosted CACTUS's orchestrator would.
