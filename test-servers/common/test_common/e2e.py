"""The parts of the end-to-end suites that every protocol shares.

`Api` does what the ecosuite API does: it writes connections and commands to
the grid-services database and reads back what the worker wrote. `Listener`
hears the database's change signals, as the planner and the server do.
`ControlClient` talks to a test server's control API, and
`SolarNetworkControl` feeds the stand-in SolarNetwork through it. None of
them talks to the worker directly.

Each suite adds its protocol's tables to `Api` and `Connection`, and its
server's control API to `ControlClient`.
"""

from __future__ import annotations

import base64
import json
import os
import time
import uuid
from dataclasses import dataclass
from datetime import datetime
from typing import Any, Callable, TypeVar

import httpx
import psycopg
import pytest
from nacl.public import PublicKey, SealedBox
from psycopg.rows import dict_row

T = TypeVar("T")

# Matches the worker's per-connection advisory lock (pg_try_advisory_lock).
ADVISORY_LOCK_CLASS = 0x76766E00


def setting(name: str, default: str | None = None) -> str:
    """A required setting from the environment; stops the run without it."""
    value = os.environ.get(name) or default
    if not value:
        pytest.exit(f"{name} is not set; see e2e/conftest.py", returncode=2)
    return value


def wait_for(check: Callable[[], T], what: str, timeout: float, interval: float = 0.25) -> T:
    """Polls `check` until it returns something truthy, and returns that."""
    deadline = time.monotonic() + timeout
    last: Any = None
    while True:
        last = check()
        if last:
            return last
        if time.monotonic() >= deadline:
            raise AssertionError(f"timed out after {timeout:g}s waiting for {what}; "
                                 f"last saw {last!r}")
        time.sleep(interval)


def parse_time(text: str) -> datetime:
    return datetime.fromisoformat(text.replace("Z", "+00:00"))


# -- The API's side ---------------------------------------------------------

@dataclass
class Connection:
    api: Api
    id: int
    site_code: str
    site_id: uuid.UUID

    # Commands

    def command(self, kind: str, event_id: int | None = None) -> int:
        return self.api.one(
            "INSERT INTO connection_commands (connection_id, kind, event_id, requested_by) "
            "VALUES (%s, %s, %s, 'e2e') RETURNING id",
            self.id, kind, event_id)["id"]

    def run(self, kind: str, event_id: int | None = None,
            expect: str = "succeeded") -> dict[str, Any]:
        """Issues a command and waits for its result, as the API does."""
        command_id = self.command(kind, event_id)
        row = wait_for(
            lambda: self.api.one("SELECT * FROM connection_commands WHERE id = %s "
                                 "AND status IN ('succeeded', 'failed')", command_id),
            f"the {kind} command to complete (is the worker running?)",
            self.api.timeout)
        assert row["status"] == expect, f"{kind}: {row['status']}: {row['message']}"
        return row

    def register(self) -> dict[str, Any]:
        self.run("register")
        return self.state()

    # Configuration

    def update(self, **columns: Any) -> None:
        assignments = ", ".join(f"{name} = %s" for name in columns)
        self.api.execute(f"UPDATE connections SET {assignments} WHERE id = %s",
                         *columns.values(), self.id)

    # Telemetry

    def set_data_credential(self, host: str, token: str, secret: str) -> None:
        """Stores the SolarNetwork token, its secret sealed for the worker,
        as the API does."""
        self.api.execute(
            "INSERT INTO connection_data_credentials (connection_id, host, token, "
            "secret_sealed) VALUES (%s, %s, %s, %s) ON CONFLICT (connection_id) DO UPDATE "
            "SET host = EXCLUDED.host, token = EXCLUDED.token, "
            "secret_sealed = EXCLUDED.secret_sealed",
            self.id, host, token, self.api.seal(secret))

    def add_points(self, points: list[dict[str, Any]]) -> None:
        """Stores what the site measures, as telemetry points. A point the
        connection already has is left as it is."""
        for point in points:
            self.api.execute(
                "INSERT INTO telemetry_points (connection_id, point_id, kind, units, "
                "scale, description, source) VALUES (%s, %s, %s, %s, %s, %s, %s::jsonb) "
                "ON CONFLICT (connection_id, point_id) DO NOTHING",
                self.id, point["point_id"], point.get("kind", "power_real"),
                point.get("units", "W"), point.get("scale", "none"),
                point.get("description"), json.dumps(point["source"]))

    def report_deliveries(self) -> list[dict[str, Any]]:
        return self.api.all("SELECT * FROM report_deliveries WHERE connection_id = %s "
                            "ORDER BY id", self.id)

    def wait_for_deliveries(self, count: int,
                            where: Callable[[dict[str, Any]], bool] = lambda row: True
                            ) -> list[dict[str, Any]]:
        def check():
            rows = [d for d in self.report_deliveries() if where(d)]
            return rows if len(rows) >= count else None
        return wait_for(check, f"{count} report deliveries for connection {self.id}",
                        self.api.timeout)

    # What the worker wrote

    def state(self) -> dict[str, Any]:
        return self.api.one("SELECT * FROM connection_state WHERE connection_id = %s",
                            self.id) or {}

    def health(self) -> str:
        return self.api.one("SELECT health FROM connection_health WHERE connection_id = %s",
                            self.id)["health"]

    def wait_for_state(self, where: Callable[[dict[str, Any]], bool],
                       what: str) -> dict[str, Any]:
        def check():
            state = self.state()
            return state if state and where(state) else None
        return wait_for(check, f"connection {self.id} {what}", self.api.timeout)

    def events(self) -> list[dict[str, Any]]:
        return self.api.all("SELECT * FROM events WHERE connection_id = %s ORDER BY id",
                            self.id)

    def event(self, program_event_id: str) -> dict[str, Any] | None:
        return self.api.one("SELECT * FROM events WHERE connection_id = %s "
                            "AND program_event_id = %s", self.id, program_event_id)

    def wait_for_event(self, program_event_id: str,
                       where: Callable[[dict[str, Any]], bool] = lambda row: True,
                       what: str = "") -> dict[str, Any]:
        def check():
            row = self.event(program_event_id)
            return row if row and where(row) else None
        return wait_for(check, f"event {program_event_id} in the database {what}".strip(),
                        self.api.timeout)

    def responses(self, event_row_id: int) -> list[dict[str, Any]]:
        return self.api.all("SELECT * FROM event_responses WHERE event_id = %s ORDER BY id",
                            event_row_id)

    def activity(self) -> list[str]:
        return [row["message"] for row in self.activity_entries()]

    def activity_entries(self) -> list[dict[str, Any]]:
        return self.api.all("SELECT * FROM activity WHERE connection_id = %s ORDER BY id",
                            self.id)

    def wait_for_activity(self, fragment: str) -> str:
        return self.wait_for_activity_entry(fragment)["message"]

    def wait_for_activity_entry(self, fragment: str) -> dict[str, Any]:
        return wait_for(
            lambda: next((e for e in self.activity_entries() if fragment in e["message"]),
                         None),
            f"activity containing {fragment!r}", self.api.timeout)


class Listener:
    """Hears one of the database's change signals. Notifications wait on the
    session until they are read, so a test listens first, acts, and then asks
    what was heard."""

    def __init__(self, database_url: str, channel: str, timeout: float):
        self.db = psycopg.connect(database_url, autocommit=True)
        self.db.execute(f"LISTEN {channel}")
        self.channel = channel
        self.timeout = timeout
        self.payloads: list[str] = []

    def close(self) -> None:
        self.db.close()

    def heard(self, payload: Any, timeout: float | None = None) -> bool:
        """Whether `payload` has been notified, waiting up to the timeout."""
        expected = str(payload)
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while expected not in self.payloads:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False
            for notification in self.db.notifies(timeout=remaining, stop_after=1):
                self.payloads.append(notification.payload)
        return True

    def wait_for(self, payload: Any) -> None:
        assert self.heard(payload), (
            f"timed out after {self.timeout:g}s waiting for {payload} on {self.channel}; "
            f"heard {self.payloads!r}")


class Api:
    def __init__(self, database_url: str, timeout: float,
                 worker_public_key: str | None = None):
        self.database_url = database_url
        self.db = psycopg.connect(database_url, autocommit=True, row_factory=dict_row)
        self.timeout = timeout
        self.worker_public_key = worker_public_key

    def seal(self, plaintext: str) -> str:
        """Seals a secret for the worker, as the API does: libsodium
        crypto_box_seal with the worker's public key, base64 with padding."""
        assert self.worker_public_key, "E2E_WORKER_PUBLIC_KEY is not set"
        box = SealedBox(PublicKey(base64.b64decode(self.worker_public_key)))
        return base64.b64encode(box.encrypt(plaintext.encode())).decode()

    def listen(self, channel: str) -> Listener:
        return Listener(self.database_url, channel, self.timeout)

    def close(self) -> None:
        self.db.close()

    def execute(self, sql: str, *params: Any) -> None:
        self.db.execute(sql, params)

    def one(self, sql: str, *params: Any) -> dict[str, Any] | None:
        return self.db.execute(sql, params).fetchone()

    def all(self, sql: str, *params: Any) -> list[dict[str, Any]]:
        return self.db.execute(sql, params).fetchall()

    def insert_connection(self, protocol: str,
                          default_opt: str = "in") -> tuple[int, str, uuid.UUID]:
        """Inserts a connection for a new, unique site, and returns its id,
        site code and site id. The caller inserts the protocol's settings in
        the same transaction, as the API must."""
        site_code = f"e2e-{uuid.uuid4().hex[:10]}"
        site_id = uuid.uuid4()
        connection_id = self.one(
            "INSERT INTO connections (site_code, site_id, protocol, default_opt) "
            "VALUES (%s, %s, %s, %s) RETURNING id",
            site_code, site_id, protocol, default_opt)["id"]
        return connection_id, site_code, site_id

    def locked(self, connection: Connection) -> bool:
        """Whether a runner holds the connection's advisory lock."""
        return self.one(
            "SELECT count(*) > 0 AS locked FROM pg_locks WHERE locktype = 'advisory' "
            "AND objsubid = 2 AND classid::bigint = %s AND objid::bigint = %s",
            ADVISORY_LOCK_CLASS, connection.id)["locked"]

    def delete_connection(self, connection: Connection) -> None:
        """Disables the connection, waits for the worker to unregister it and
        let go of it, then deletes everything it left behind."""
        connection.update(enabled=False)
        wait_for(lambda: connection.state().get("registration") != "registered"
                 and not self.locked(connection),
                 f"the worker to unregister and release connection {connection.id}",
                 self.timeout)
        with self.db.transaction():
            self.execute("DELETE FROM activity WHERE site_id = %s", connection.site_id)
            self.execute("DELETE FROM events WHERE site_id = %s", connection.site_id)
            self.execute("DELETE FROM connections WHERE id = %s", connection.id)


# -- The test servers' side -------------------------------------------------

class ControlClient:
    """A test server's control API: JSON under /api, with the server's
    bearer token if it has one."""

    def __init__(self, base_url: str, token: str | None, timeout: float):
        headers = {"Authorization": f"Bearer {token}"} if token else {}
        self.http = httpx.Client(base_url=base_url.rstrip("/") + "/api", headers=headers,
                                 timeout=10)
        self.timeout = timeout

    def close(self) -> None:
        self.http.close()

    def _call(self, method: str, path: str, **kwargs: Any) -> Any:
        response = self.http.request(method, path, **kwargs)
        if response.status_code >= 400:
            raise AssertionError(f"{method} {path}: {response.status_code} {response.text}")
        return response.json() if response.content else None


class SolarNetworkControl(ControlClient):
    """The stand-in SolarNetwork, through the control API of the test server
    that serves it."""

    def __init__(self, base_url: str, token: str | None, timeout: float,
                 worker_facing_url: str | None = None):
        super().__init__(base_url, token, timeout)
        # The server as the worker reaches it: the host a connection's
        # SolarNetwork credential names.
        self.worker_facing_url = (worker_facing_url or base_url).rstrip("/")

    def set_token(self, token: str, secret: str) -> None:
        self._call("PUT", f"/solarnetwork/tokens/{token}", json={"secret": secret})

    def set_stream(self, node_id: int, source_id: str, datums: list[dict[str, Any]]) -> None:
        self._call("PUT", "/solarnetwork/streams",
                   json={"node_id": node_id, "source_id": source_id, "datums": datums})

    def requests(self) -> list[dict[str, Any]]:
        return self._call("GET", "/solarnetwork/requests")
