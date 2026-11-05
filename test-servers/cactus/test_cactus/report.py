"""A batch's summary: summary.json, and summary.md, its tables. The tables
list every test the worker certifies to, those the batch did not run as
untested, then any other test it ran."""

from __future__ import annotations

import json
from dataclasses import asdict
from pathlib import Path

from test_cactus import procedures
from test_cactus.procedures import Procedure
from test_cactus.session import Result

WITNESSED_CLASSES = ("DER-A", "DER-G", "DER-L")
STATUSES = {"passed": "pass", "failed": "fail", "error": "error", "interrupted": "interrupted"}


def note(result: Result | None) -> str:
    if result is None or result.outcome == "passed":
        return ""
    if result.detail:
        return result.detail.replace("|", "\\|").replace("\n", " ")[:200]
    return ", ".join(f"`{name}`" for name in result.failed_checks)


def row(procedure: Procedure, result: Result | None) -> str:
    status = STATUSES.get(result.outcome, result.outcome) if result else "untested"
    return (f"| {procedure.id} | {procedure.description} | {', '.join(procedure.classes)} "
            f"| {status} | {note(result)} |")


def section(title: str, rows: list[str]) -> str:
    header = ["| Test | Checks | Classes | Status | Failed checks |",
              "| --- | --- | --- | --- | --- |"]
    return f"{title}\n\n" + "\n".join(header + rows) + "\n" if rows else ""


def table(results: list[Result]) -> str:
    by_test = {result.test: result for result in results}
    scope = sorted(procedures.select([], list(procedures.REQUIRED_CLASSES)),
                   key=lambda procedure: str(procedure.id))
    in_scope = {str(procedure.id) for procedure in scope}
    witnessed = [procedure for procedure in scope
                 if set(procedure.classes) <= set(WITNESSED_CLASSES)]
    required = [procedure for procedure in scope if procedure not in witnessed]
    others = sorted((procedures.load(test) for test in by_test if test not in in_scope),
                    key=lambda procedure: str(procedure.id))
    return "\n".join(part for part in [
        section("Required:", [row(p, by_test.get(str(p.id))) for p in required]),
        section("Witnessed by ANU with a DER:", [row(p, by_test.get(str(p.id)))
                                                 for p in witnessed]),
        section("Others run:", [row(p, by_test[str(p.id)]) for p in others]),
    ] if part)


def write(directory: Path, results: list[Result], header: dict[str, str]) -> None:
    (directory / "summary.json").write_text(json.dumps(
        {**header, "results": [asdict(result) for result in results]}, indent=2))
    described = "\n".join(f"- {name}: {value}" for name, value in header.items())
    (directory / "summary.md").write_text(f"{described}\n\n{table(results)}")
