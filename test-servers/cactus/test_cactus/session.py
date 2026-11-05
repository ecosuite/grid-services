"""One test, run as the hosted CACTUS's orchestrator and an operator would
run it: on a stack of its own, the test is started on the runner, the site
simulator provisions the site as ecosuite would and asks the worker to
register it, the test is followed, with the operator doing its scripted
act as each step starts waiting, until the runner finishes it, or its
steps are done and its last control has ended, or its time is up, then
finished, and its archive kept with the worker's log and the simulator's
trace."""

from __future__ import annotations

import json
import threading
import time
import zipfile
from dataclasses import asdict, dataclass, field, replace
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Callable

import httpx
from cactus_test_definitions.client.test_procedures import get_yaml_contents

from test_cactus import scenarios, site
from test_cactus.certificates import Certificates
from test_cactus.procedures import CSIP_AUS_VERSION, Procedure
from test_cactus.site import Scenario
from test_cactus.stack import COMPOSE_FILE, Stack, StackSettings
from test_common.certificates import DeviceCertificate
from test_common.e2e import Api

POLL_SECONDS = 5
# After a test's last control ends: the planner completes it and the worker
# answers on its next due work, within seconds; a poll of the controls, at
# the rate the server set, is allowed besides, for a control the worker had
# yet to store, and the runner records it.
CONTROL_POLL_SECONDS = 60
ANSWER_MARGIN_SECONDS = 30

Log = Callable[[str], None]


class Interrupted(Exception):
    pass


@dataclass(frozen=True)
class RunOptions:
    scenario: Scenario
    pen: int
    start_after: float
    keep: str


@dataclass
class Result:
    test: str
    description: str
    classes: list[str]
    outcome: str = "error"
    checks: dict[str, dict[str, Any]] = field(default_factory=dict)
    warnings: list[Any] = field(default_factory=list)
    steps_done: bool = False
    seconds: float = 0
    detail: str | None = None
    project: str = ""
    directory: str = ""

    @property
    def failed_checks(self) -> list[str]:
        return [name for name, check in self.checks.items() if not check.get("passed")]


def run_request(procedure: Procedure, device: DeviceCertificate, pen: int,
                run_id: str) -> dict[str, Any]:
    """What the orchestrator sends the runner to start a run
    (cactus-orchestrator api/run.py _build_run_request), for a device
    certificate."""
    return {
        "run_id": run_id,
        "test_definition": {"test_procedure_id": str(procedure.id),
                            "yaml_definition": get_yaml_contents(procedure.id)},
        "run_group": {
            "run_group_id": "grid-services",
            "name": "grid-services worker",
            "csip_aus_version": CSIP_AUS_VERSION,
            "test_certificates": {"aggregator": None, "device": device.cert_pem},
        },
        "test_config": {"pen": pen, "subscription_domain": None, "is_static_url": False},
        "test_user": {"user_id": "grid-services", "name": "grid-services"},
    }


def worker_view(api: Api, connection_id: int) -> str:
    row = api.one("SELECT health, registration, last_error, config_error "
                  "FROM connection_health WHERE connection_id = %s", connection_id)
    if row is None:
        return "not started"
    detail = row["last_error"] or row["config_error"]
    return f"{row['health']}, {row['registration']}" + (f": {detail}" if detail else "")


def command_results(api: Api, connection_id: int) -> list[str]:
    return [f"{row['kind']} {row['status']}" + (f": {row['message']}" if row["message"] else "")
            for row in api.all("SELECT kind, status, message FROM connection_commands "
                               "WHERE connection_id = %s AND status <> 'pending' ORDER BY id",
                               connection_id)]


def activity(api: Api, connection_id: int, after: int) -> list[dict[str, Any]]:
    return api.all("SELECT id, category, message FROM activity "
                   "WHERE connection_id = %s AND id > %s ORDER BY id", connection_id, after)


def steps_done(status: dict[str, Any]) -> bool:
    steps = status.get("stepStatus") or {}
    return bool(steps) and all(step.get("completedAt") for step in steps.values())


def preconditions_met(status: dict[str, Any]) -> bool:
    checks = status.get("preconditionChecks") or []
    return bool(checks) and all(check.get("success") for check in checks)


def describe_step(step: dict[str, Any]) -> str:
    if step.get("completedAt"):
        return f"done at {step['completedAt']}"
    if step.get("startedAt"):
        return f"waiting for {step.get('eventStatus')}"
    return "not reached"


def stored_controls(api: Api, connection_id: int) -> dict[str, Any]:
    """How many controls the worker stored for the connection that the
    server has not cancelled or superseded, and when the last of them ends
    on this device, by its effective window, on this clock: the worker
    stores the server's times, so less the server's clock offset."""
    row = api.one(
        "SELECT count(e.id) AS controls, "
        "max(c.effective_end_at) "
        "- make_interval(secs => s.clock_offset_seconds) AS last_end, "
        "coalesce(s.program_poll_interval_seconds, %s) AS poll_seconds "
        "FROM connections n "
        "LEFT JOIN connection_state s ON s.connection_id = n.id "
        "LEFT JOIN (events e JOIN csipaus_events c "
        "           ON c.event_id = e.id AND c.kind = 'control') "
        "ON e.connection_id = n.id AND e.program_status NOT IN ('cancelled', 'superseded') "
        "WHERE n.id = %s "
        "GROUP BY s.program_poll_interval_seconds, s.clock_offset_seconds",
        CONTROL_POLL_SECONDS, connection_id)
    return row or {"controls": 0, "poll_seconds": CONTROL_POLL_SECONDS}


class Ending:
    """Whether a test is over: the runner has finished it (a finish-test
    action, after which it answers nothing), or its steps are done and, for
    a test that creates controls, every control the worker stored has ended,
    then a poll of the controls and a margin have passed, for the started
    and completed answers that come after the steps. The last steps'
    controls may not be stored yet when the steps are done, so such a test
    is given that long after them at least. Logs what it waits for when
    that changes."""

    def __init__(self, procedure: Procedure, api: Api, connection_id: int, log: Log):
        self.procedure = procedure
        self.api = api
        self.connection_id = connection_id
        self.log = log
        self.steps_done_at: float | None = None
        self.waiting_for = ""
        self.over = False

    def __call__(self, status: dict[str, Any]) -> bool:
        if status.get("timestampFinished"):
            self.over = True
            return True
        if not steps_done(status):
            return False
        now = time.monotonic()
        if self.steps_done_at is None:
            self.steps_done_at = now
            self.log("runner   steps done")
        if not self.procedure.creates_controls:
            self.over = True
            return True
        controls = stored_controls(self.api, self.connection_id)
        after = controls["poll_seconds"] + ANSWER_MARGIN_SECONDS
        left = self.steps_done_at + after - now
        if controls["controls"] and controls["last_end"] is None:
            waiting_for = (f"the worker to write its {controls['controls']} controls' "
                           f"effective windows, {after} s at most")
        elif controls["controls"]:
            last_end = controls["last_end"]
            left = max(left, (last_end - datetime.now(timezone.utc)).total_seconds() + after)
            waiting_for = (f"{controls['controls']} controls to end, the last at "
                           f"{last_end.astimezone():%H:%M:%S}, and {after} s more "
                           f"for the answers")
        else:
            waiting_for = f"the worker to store the test's controls, {after} s at most"
        if left > 0 and waiting_for != self.waiting_for:
            self.waiting_for = waiting_for
            self.log(f"waiting  for {waiting_for}: {left:.0f} s left")
        self.over = left <= 0
        return self.over


def started_at(step: dict[str, Any]) -> datetime:
    """When the runner started a step: an ISO time in UTC."""
    started = datetime.fromisoformat(step["startedAt"])
    if started.tzinfo is None:
        raise ValueError(f"the runner gave a step's start, {step['startedAt']}, "
                         f"without its time zone")
    return started


class Operator:
    """The person at the site: does each act once, when the runner has
    started its step and not completed it, and the act's time into the step
    has come. "proceed" goes to the runner, and one it does not handle is an
    error; "restart_worker" power cycles the worker; the rest go to the
    site."""

    def __init__(self, acts: list[scenarios.Act], runner: httpx.Client, site_url: str,
                 restart_worker: Callable[[], None], log: Log):
        self.acts = acts
        self.runner = runner
        self.site_url = site_url
        self.restart_worker = restart_worker
        self.log = log
        self.done: set[int] = set()

    def __call__(self, status: dict[str, Any]) -> None:
        steps = status.get("stepStatus") or {}
        now = datetime.now(timezone.utc)
        for index, act in enumerate(self.acts):
            step = steps.get(act.step)
            if (index in self.done or step is None or not step.get("startedAt")
                    or step.get("completedAt")
                    or now < started_at(step) + timedelta(seconds=act.after_seconds)):
                continue
            self.done.add(index)
            if act.act == "proceed":
                self.proceed(act.step)
            elif act.act == "restart_worker":
                self.restart_worker()
            else:
                site.operate(self.site_url, act.act, act.body())
            watts = f" {act.watts:g} W" if act.watts is not None else ""
            self.log(f"operator {act.act}{watts} for {act.step}")


    def proceed(self, step: str) -> None:
        answer = self.runner.get("/proceed")
        if answer.is_error or not answer.json().get("handled"):
            raise RuntimeError(f"the runner did not handle proceed for {step}: "
                               f"{answer.status_code} {answer.text}")


def follow(runner: httpx.Client, api: Api, connection_id: int, until: float,
           done: Callable[[dict[str, Any]], bool], stop: threading.Event,
           log: Log, each: Callable[[dict[str, Any]], None] | None = None) -> dict[str, Any]:
    """Logs what changes, on the runner and in the worker, until the runner's
    status is `done` or `until`, and gives `each` every status. A step
    counting down is logged once."""
    step_states: dict[str, str] = {}
    seen_commands: set[str] = set()
    last_worker = ""
    last_activity = 0
    while True:
        if stop.is_set():
            raise Interrupted()
        status = runner.get("/status").raise_for_status().json()
        for name, step in (status.get("stepStatus") or {}).items():
            state = describe_step(step)
            kind = state.split(" ")[0]
            if step_states.get(name, "").split(" ")[0] != kind:
                log(f"step     {name}: {state}")
            step_states[name] = state
        if each is not None:
            each(status)
        for line in command_results(api, connection_id):
            if line not in seen_commands:
                seen_commands.add(line)
                log(f"command  {line}")
        worker = worker_view(api, connection_id)
        if worker != last_worker:
            last_worker = worker
            log(f"worker   {worker}")
        for row in activity(api, connection_id, last_activity):
            last_activity = row["id"]
            log(f"activity [{row['category']}] {row['message']}")
        if done(status) or time.monotonic() >= until:
            return status
        stop.wait(POLL_SECONDS)


def reporting_data(archive: Path) -> dict[str, Any]:
    """The runner's report of the run, from its archive: every check it
    made, including those /status leaves out, such as XSD validity."""
    with zipfile.ZipFile(archive) as contents:
        name = next(name for name in contents.namelist() if name.startswith("ReportingData"))
        return json.loads(contents.read(name))


def run_test(procedure: Procedure, certificates: Certificates, settings: StackSettings,
             options: RunOptions, directory: Path, project: str, stop: threading.Event,
             echo: bool) -> Result:
    directory.mkdir(parents=True, exist_ok=True)
    result = Result(test=str(procedure.id), description=procedure.description,
                    classes=list(procedure.classes), project=project,
                    directory=str(directory))
    started_at = time.monotonic()
    log_file = (directory / "run.log").open("w")

    def log(line: str) -> None:
        stamped = f"{datetime.now():%H:%M:%S} {line}"
        log_file.write(stamped + "\n")
        log_file.flush()
        if echo:
            print(f"  {stamped}", flush=True)

    stack = Stack(project, settings)
    try:
        log(f"{procedure.id}: {procedure.description}")
        log(f"stack    starting {project}")
        stack.up()
        log("stack    up")
        with httpx.Client(base_url=stack.runner_url(), timeout=30) as runner:
            api = Api(stack.database_url(), timeout=30)
            try:
                execute(procedure, certificates, options, runner, api, stack.site_sim_url(),
                        lambda: stack.restart("worker"), directory, stop, log, result)
            finally:
                api.close()
    except Interrupted:
        result.outcome = "interrupted"
        log("interrupted")
    except Exception as error:
        result.outcome = "error"
        result.detail = f"{type(error).__name__}: {error}"
        log(f"error    {result.detail}")
    finally:
        kept = {"worker.log": lambda: stack.logs("worker"),
                "site-sim.log": lambda: stack.logs("site-sim")}
        for name, contents in kept.items():
            try:
                (directory / name).write_text(contents())
            except Exception as error:
                log(f"could not save {name}: {error}")
        try:
            trace = site.trace(stack.site_sim_url())
            (directory / "site.json").write_text(json.dumps(trace, indent=2))
            for warning in trace["warnings"]:
                log(f"site     warning {warning['message']}")
        except Exception as error:
            log(f"could not save site.json: {error}")
        keep = options.keep == "always" or (options.keep == "failed"
                                           and result.outcome != "passed")
        if keep:
            log(f"stack    kept: docker compose -p {project} -f {COMPOSE_FILE} down -v")
        else:
            try:
                stack.down()
            except Exception as error:
                log(f"could not take the stack down: {error}")
        result.seconds = round(time.monotonic() - started_at)
        (directory / "result.json").write_text(json.dumps(asdict(result), indent=2))
        log_file.close()
    return result


def execute(procedure: Procedure, certificates: Certificates, options: RunOptions,
            runner: httpx.Client, api: Api, site_url: str,
            restart_worker: Callable[[], None], directory: Path,
            stop: threading.Event, log: Log, result: Result) -> None:
    run_id = datetime.now().strftime("%Y%m%d%H%M%S")
    initialised = runner.post(
        "/initialise", json=run_request(procedure, certificates.device, options.pen, run_id),
        timeout=120)
    if initialised.is_error:
        raise RuntimeError(f"the runner refused the test: {initialised.status_code} "
                           f"{initialised.text}")
    started = runner.get("/status").raise_for_status().json().get("timestampStart") is not None
    log("runner   initialised" + (", started" if started else ", in its precondition phase"))

    scenario = options.scenario
    if scenario.der is not None:
        scenario = replace(scenario, der=scenarios.der(str(procedure.id)),
                           **scenarios.power(str(procedure.id)))
    connection_id = site.provision(site_url, scenario)
    log(f"site     provisioned connection {connection_id}, asked to register")

    if not started:
        status = follow(runner, api, connection_id, time.monotonic() + options.start_after,
                        preconditions_met, stop, log)
        preconditions = {f"precondition {check.get('type')}":
                         {"passed": bool(check.get("success")),
                          "description": check.get("details") or None}
                         for check in status.get("preconditionChecks") or []}
        for name, check in preconditions.items():
            log(f"check    {'pass' if check['passed'] else 'FAIL'}  {name}"
                + (f": {check['description']}" if check["description"] else ""))
        start = runner.post("/start")
        if start.status_code == 412:
            # The runner will not start a test whose preconditions the client
            # has not met: the test fails on them.
            log("runner   refused to start: preconditions not met")
            (directory / "cactus.zip").write_bytes(
                runner.post("/finalize", timeout=120).raise_for_status().content)
            result.checks = {name: check for name, check in preconditions.items()
                             if not check["passed"]} or {"start": {"passed": False,
                                                                   "description": start.text}}
            result.outcome = "failed"
            return
        start.raise_for_status()
        log("runner   started")

    ending = Ending(procedure, api, connection_id, log)
    operator = Operator(scenarios.acts(str(procedure.id)), runner, site_url, restart_worker,
                        log)
    status = follow(runner, api, connection_id, time.monotonic() + procedure.timeout_seconds,
                    ending, stop, log, operator)
    result.steps_done = steps_done(status)
    if status.get("timestampFinished"):
        log("runner   finished the test" + ("" if result.steps_done else " with steps left"))
    elif not result.steps_done:
        log("runner   timed out with steps left")
    elif not ending.over:
        log(f"runner   timed out waiting for {ending.waiting_for}")

    archive = directory / "cactus.zip"
    archive.write_bytes(runner.post("/finalize", timeout=120).raise_for_status().content)
    report = reporting_data(archive)
    result.checks = report.get("checkResults") or {}
    result.warnings = report.get("warnings") or []
    for name, check in result.checks.items():
        log(f"check    {'pass' if check.get('passed') else 'FAIL'}  {name}"
            + (f": {check['description']}" if check.get("description") else ""))
    for warning in result.warnings:
        log(f"warning  {json.dumps(warning)}")
    passed = bool(result.checks) and not result.failed_checks
    result.outcome = "passed" if passed else "failed"
