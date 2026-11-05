"""Copies CACTUS runs' envoy exchanges into the worker's unit test fixtures.

For each run directory, results/<batch>/<TEST>, reads the requests and
responses in its cactus.zip and rewrites tests/fixtures/csipaus/cactus/<TEST>/:
each response body, and manifest.json listing the exchanges in the order the
run made them. tests/support/cactus_fixture.hpp loads them.

    uv run --project test-servers/cactus test-servers/cactus/capture.py test-servers/cactus/results/<batch>/ALL-01
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import zipfile
from dataclasses import dataclass
from itertools import takewhile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parent.parent
RESULTS = HERE / "results"
FIXTURES = REPOSITORY / "tests" / "fixtures" / "csipaus" / "cactus"
# The runner's ENVOY_PROXY_PREFIX (compose.yaml), which it strips from the
# paths it records; the worker requested them with it.
ENVOY_PREFIX = "/envoy"


@dataclass
class Exchange:
    request_id: int
    name: str
    method: str
    path: str
    epoch: float
    status: int
    headers: dict[str, str]
    body: str | None

    def body_file(self) -> str | None:
        if self.body is None:
            return None
        xml = "xml" in self.headers.get("content-type", "")
        return f"{self.name}{'.xml' if xml else '.txt'}"

    def manifest_entry(self) -> dict[str, object]:
        entry: dict[str, object] = {"method": self.method, "path": ENVOY_PREFIX + self.path,
                                    "status": self.status, "epoch": self.epoch}
        if body := self.body_file():
            entry["body"] = body
        if location := self.headers.get("location"):
            entry["location"] = location
        return entry


def comments_and_rest(text: str) -> tuple[dict[str, str], list[str]]:
    lines = text.split("\n")
    comments = list(takewhile(lambda line: line.startswith("# "), lines))
    fields = dict(line[2:].split(": ", 1) for line in comments if ": " in line)
    return fields, lines[len(comments):]


def read_exchange(archive: zipfile.ZipFile, request_name: str) -> Exchange:
    name = Path(request_name).stem
    response_name = f"requests/{name}.response"
    if response_name not in archive.namelist():
        raise SystemExit(f"{request_name} has no {response_name}")

    request_fields, request_lines = comments_and_rest(archive.read(request_name).decode())
    method, path, _ = request_lines[0].split(" ", 2)

    _, response_lines = comments_and_rest(archive.read(response_name).decode())
    status = int(response_lines[0].split(" ")[1])
    blank = response_lines.index("") if "" in response_lines else len(response_lines)
    headers = {header.lower(): value
               for header, value in (line.split(": ", 1) for line in response_lines[1:blank])}
    body = "\n".join(response_lines[blank + 1:]) if blank < len(response_lines) else None

    return Exchange(int(name.split("-")[0]), name, method, path,
                    float(request_fields["Epoch"]), status, headers, body)


def capture(run: Path) -> Path:
    zip_path = run.resolve() / "cactus.zip"
    with zipfile.ZipFile(zip_path) as archive:
        exchanges = sorted(
            (read_exchange(archive, member) for member in archive.namelist()
             if member.startswith("requests/") and member.endswith(".request")),
            key=lambda exchange: exchange.request_id)

    test = run.resolve().name
    fixture = FIXTURES / test
    shutil.rmtree(fixture, ignore_errors=True)
    fixture.mkdir(parents=True)
    for exchange in exchanges:
        if body_file := exchange.body_file():
            (fixture / body_file).write_text(exchange.body or "", encoding="utf-8")
    manifest = {"test": test, "source": str(zip_path.relative_to(RESULTS)),
                "exchanges": [exchange.manifest_entry() for exchange in exchanges]}
    (fixture / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"{test}: {len(exchanges)} exchanges into {fixture.relative_to(REPOSITORY)}")
    return fixture


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("runs", nargs="+", type=Path, metavar="RUN",
                        help="a test's run directory, results/<batch>/<TEST>")
    for run in parser.parse_args().runs:
        capture(run)
    return 0


if __name__ == "__main__":
    sys.exit(main())
