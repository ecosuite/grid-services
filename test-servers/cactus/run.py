"""Runs CACTUS test procedures against the worker, side by side.

Each test runs on a stack of its own (compose.yaml): the worker and its
database, the site simulator, and the CACTUS test stack, as the hosted
CACTUS starts a pod for each run. A batch runs `--jobs` tests at a time,
longest first, and is kept in results/<batch>/: for each test, the runner's
archive, the worker's and the simulator's logs, the simulator's trace, what
the run saw and its result, and for the batch summary.json and summary.md,
a table of how each test went.

    uv run --project test-servers/cactus test-servers/cactus/run.py ALL-01
    uv run --project test-servers/cactus test-servers/cactus/run.py --required --jobs 8
    uv run --project test-servers/cactus test-servers/cactus/run.py --class G,L --list
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import threading
import time
from concurrent.futures import Future, ThreadPoolExecutor
from datetime import datetime
from pathlib import Path

from test_cactus import certificates, nmi, procedures, report, site
from test_cactus.procedures import Procedure
from test_cactus.session import Result, RunOptions, run_test
from test_cactus.site import Scenario
from test_cactus.stack import COMPOSE_FILE, StackSettings

HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parent.parent
RESULTS = HERE / "results"
WORKER_IMAGE = "grid-services:latest"
# A PEN other than 0, so the runner's checks of mRIDs see how the worker
# writes it.
PEN = 12345


def ensure_worker_image(image: str, build: bool) -> None:
    exists = subprocess.run(["docker", "image", "inspect", image],
                            capture_output=True).returncode == 0
    if build or not exists:
        if image != WORKER_IMAGE:
            raise SystemExit(f"{image} is not there, and only {WORKER_IMAGE} is built here")
        print(f"Building {image}", flush=True)
        subprocess.run(["docker", "compose", "build", "worker"], cwd=REPOSITORY, check=True)


def build_site_sim() -> None:
    """Builds the site simulator's image from this checkout, once for the
    batch rather than in each stack; unchanged, it takes seconds."""
    print("Building the site simulator", flush=True)
    subprocess.run(["docker", "compose", "--file", str(COMPOSE_FILE), "build", "--quiet",
                    "site-sim"], check=True)


def connection_point(value: str) -> str:
    """An NMI of 10 letters and digits with its checksum added; anything
    else as it is."""
    return nmi.with_checksum(value) if len(value) == 10 and value.isalnum() else value


def minutes(seconds: float) -> str:
    return f"{seconds / 60:.0f} min"


def describe(result: Result) -> str:
    if result.outcome == "passed":
        return "passed"
    if result.outcome == "failed":
        return "failed: " + ", ".join(result.failed_checks)
    return f"{result.outcome}" + (f": {result.detail}" if result.detail else "")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("tests", nargs="*", help="test procedures, such as ALL-01")
    parser.add_argument("--class", dest="classes", default="",
                        help="also every test in these classes, comma separated, such as A,G")
    parser.add_argument("--required", action="store_true",
                        help="also every test in the classes we certify to: "
                             + ", ".join(procedures.REQUIRED_CLASSES))
    parser.add_argument("--list", action="store_true", help="list the tests and stop")
    parser.add_argument("--summarise", metavar="BATCH",
                        help="write a batch's summary.md again from its summary.json, and stop")
    parser.add_argument("--jobs", type=int, default=6, help="tests run at once (default 6)")
    parser.add_argument("--build", action="store_true",
                        help=f"build {WORKER_IMAGE} first, from this checkout")
    parser.add_argument("--image", default=WORKER_IMAGE,
                        help=f"the worker image to test (default {WORKER_IMAGE})")
    parser.add_argument("--cactus-image-tag",
                        help="the CACTUS images' tag (default compose.yaml's, 194-v12)")
    parser.add_argument("--keep", choices=["never", "failed", "always"], default="never",
                        help="leave a test's stack up afterwards, to look at (default never)")
    parser.add_argument("--start-after", type=float, default=120,
                        help="for a test with a precondition phase, seconds to give the worker "
                             "before starting it, unless its checks pass sooner (default 120)")
    parser.add_argument("--pin", type=int, default=site.PIN,
                        help=f"the registration PIN the connection checks (default {site.PIN})")
    parser.add_argument("--no-pin", action="store_true", help="check no PIN")
    parser.add_argument("--no-der", action="store_true",
                        help="describe no DER for the site: the worker sends no "
                             "DERCapability or DERSettings, though readings still flow")
    parser.add_argument("--nmi", metavar="ID", default=site.NMI, type=connection_point,
                        help=f"the connection point the worker sends; 10 characters get "
                             f"their checksum added (default {site.NMI})")
    parser.add_argument("--pen", type=int, default=PEN,
                        help=f"the PEN the run and the worker's mRIDs share (default {PEN})")
    args = parser.parse_args()

    if args.summarise:
        directory = RESULTS / args.summarise
        summary = json.loads((directory / "summary.json").read_text())
        results = [Result(**result) for result in summary.pop("results")]
        report.write(directory, results, summary)
        print((directory / "summary.md").read_text())
        return 0

    classes = [name for name in args.classes.split(",") if name]
    classes += list(procedures.REQUIRED_CLASSES) if args.required else []
    if not args.tests and not classes:
        parser.error("name tests, or choose them with --class or --required")
    selected = procedures.select(args.tests, classes)
    longest = max(procedure.estimate_seconds for procedure in selected)
    total = sum(procedure.estimate_seconds for procedure in selected)
    print(f"{len(selected)} tests, about {minutes(total)} one after another; "
          f"the longest about {minutes(longest)}")
    if args.list:
        for procedure in selected:
            print(f"  {procedure.id:12} {minutes(procedure.estimate_seconds):>7}  "
                  f"{','.join(procedure.classes):18} {procedure.description}")
        return 0

    ensure_worker_image(args.image, args.build)
    build_site_sim()
    certs = certificates.ensure()
    settings = StackSettings(worker_image=args.image, pen=args.pen,
                             cactus_image_tag=args.cactus_image_tag)
    scenario = Scenario(der=None if args.no_der else dict(site.PV5K),
                        pin=None if args.no_pin else args.pin, nmi=args.nmi)
    options = RunOptions(scenario=scenario, pen=args.pen, start_after=args.start_after,
                         keep=args.keep)
    batch = datetime.now().strftime("%Y%m%d-%H%M%S")
    directory = RESULTS / batch
    directory.mkdir(parents=True, exist_ok=True)
    print(f"Batch {batch}: {directory}, {args.jobs} at a time", flush=True)

    stop = threading.Event()
    echo = len(selected) == 1
    started_at = time.monotonic()
    lock = threading.Lock()
    running: set[str] = set()

    def run(procedure: Procedure) -> Result:
        with lock:
            running.add(str(procedure.id))
        print(f"{datetime.now():%H:%M:%S} start  {procedure.id} "
              f"(about {minutes(procedure.estimate_seconds)})", flush=True)
        result = run_test(procedure, certs, settings, options,
                          directory / str(procedure.id),
                          f"gs-cactus-{batch}-{str(procedure.id).lower()}", stop, echo)
        with lock:
            running.discard(str(procedure.id))
            still = ", ".join(sorted(running)) or "none"
        print(f"{datetime.now():%H:%M:%S} {'done' if result.outcome == 'passed' else 'FAIL'}"
              f"   {procedure.id} {describe(result)} in {minutes(result.seconds)}; "
              f"running: {still}", flush=True)
        return result

    executor = ThreadPoolExecutor(max_workers=args.jobs)
    futures: list[Future[Result]] = [executor.submit(run, procedure) for procedure in selected]
    try:
        while not all(future.done() for future in futures):
            time.sleep(1)
    except KeyboardInterrupt:
        print("Interrupted: taking the stacks down", flush=True)
        stop.set()
    executor.shutdown(wait=True, cancel_futures=True)
    results = [future.result() for future in futures
               if future.done() and not future.cancelled()]

    passed = sum(1 for result in results if result.outcome == "passed")
    report.write(directory, results, {
        "batch": batch,
        "worker image": args.image,
        "CACTUS images": args.cactus_image_tag or "compose.yaml's",
        "took": minutes(time.monotonic() - started_at),
        "passed": f"{passed} of {len(results)}",
    })
    print()
    print(report.table(results))
    print(f"{passed} of {len(results)} passed in {minutes(time.monotonic() - started_at)}; "
          f"{directory / 'summary.md'}")
    return 0 if results and passed == len(results) == len(selected) else 1


if __name__ == "__main__":
    sys.exit(main())
