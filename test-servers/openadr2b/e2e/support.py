"""What the OpenADR 2.0b suite adds to the shared end-to-end parts.

`Api` and `Connection` add the openadr2b tables to `test_common.e2e`'s, and
`VtnControl` does what a utility does: it sends events through the test VTN
and reads what the VEN sent back.
"""

from __future__ import annotations

import json
import uuid
from dataclasses import dataclass
from typing import Any, Callable

from test_common import e2e
from test_common.e2e import wait_for

# -- The API's side ---------------------------------------------------------

@dataclass
class Connection(e2e.Connection):
    ven_name: str

    def update_settings(self, **columns: Any) -> None:
        assignments = ", ".join(f"{name} = %s" for name in columns)
        self.api.execute(f"UPDATE openadr2b_connections SET {assignments} "
                         "WHERE connection_id = %s", *columns.values(), self.id)

    def add_offer(self, specifier_id: str, data_points: list[dict[str, Any]],
                  report_name: str = "TELEMETRY_USAGE", min_sampling_seconds: int = 60,
                  max_sampling_seconds: int = 3600, history_seconds: int = 0) -> int:
        """Offers the program a report carrying telemetry points, which
        `data_points` name."""
        return self.api.one(
            "INSERT INTO openadr2b_report_offers (connection_id, report_name, specifier_id, "
            "min_sampling_seconds, max_sampling_seconds, history_seconds, data_points) "
            "VALUES (%s, %s, %s, %s, %s, %s, %s::jsonb) RETURNING id",
            self.id, report_name, specifier_id, min_sampling_seconds,
            max_sampling_seconds, history_seconds, json.dumps(data_points))["id"]

    def report_requests(self) -> list[dict[str, Any]]:
        return self.api.all("SELECT * FROM openadr2b_report_requests "
                            "WHERE connection_id = %s ORDER BY id", self.id)


class Api(e2e.Api):
    def __init__(self, database_url: str, vtn_url: str, timeout: float,
                 worker_public_key: str | None = None):
        super().__init__(database_url, timeout, worker_public_key)
        self.vtn_url = vtn_url.rstrip("/")

    def create_connection(self, default_opt: str = "in",
                          market_context: str | None = None,
                          accepted_signals: list[str] | None = None) -> Connection:
        """Inserts a connection and its settings in one transaction, as the
        API must. Its VTN URL names the VEN, so the test VTN can answer
        registration queries without client certificates."""
        with self.db.transaction():
            connection_id, site_code, site_id = self.insert_connection("openadr2b",
                                                                       default_opt)
            ven_name = site_code
            self.execute(
                "INSERT INTO openadr2b_connections (connection_id, vtn_url, ven_name, "
                "market_context, accepted_signals) VALUES (%s, %s, %s, %s, %s)",
                connection_id, f"{self.vtn_url}/v/{ven_name}", ven_name, market_context,
                accepted_signals or ["SIMPLE"])
        return Connection(self, connection_id, site_code, site_id, ven_name)


# -- The utility's side -----------------------------------------------------

class VtnControl(e2e.ControlClient):
    def info(self) -> dict[str, Any]:
        return self._call("GET", "/vtn")

    def ven(self, ven: str) -> dict[str, Any] | None:
        response = self.http.get(f"/vens/{ven}")
        if response.status_code == 404:
            return None
        response.raise_for_status()
        return response.json()

    def delete_ven(self, ven: str) -> None:
        response = self.http.delete(f"/vens/{ven}")
        if response.status_code not in (204, 404):
            response.raise_for_status()

    def create_event(self, ven: str, **body: Any) -> dict[str, Any]:
        body.setdefault("event_id", f"ev-{uuid.uuid4().hex[:8]}")
        return self._call("POST", f"/vens/{ven}/events", json=body)

    def modify_event(self, ven: str, event_id: str, **body: Any) -> dict[str, Any]:
        return self._call("PATCH", f"/vens/{ven}/events/{event_id}", json=body)

    def cancel_event(self, ven: str, event_id: str) -> dict[str, Any]:
        return self._call("POST", f"/vens/{ven}/events/{event_id}/cancel")

    def event(self, ven: str, event_id: str) -> dict[str, Any]:
        return self._call("GET", f"/vens/{ven}/events/{event_id}")

    def cancel_registration(self, ven: str) -> None:
        self._call("POST", f"/vens/{ven}/registration/cancel")

    def request_reregistration(self, ven: str) -> None:
        self._call("POST", f"/vens/{ven}/registration/reregister")

    def reports(self, ven: str) -> dict[str, Any]:
        return self._call("GET", f"/vens/{ven}/reports")

    def request_report(self, ven: str, **body: Any) -> dict[str, Any]:
        return self._call("POST", f"/vens/{ven}/reports/request", json=body)

    def cancel_report(self, ven: str, request_id: str) -> dict[str, Any]:
        return self._call("POST", f"/vens/{ven}/reports/{request_id}/cancel")

    def set_auto_report_requests(self, enabled: bool) -> None:
        self._call("PATCH", "/vtn", json={"auto_report_requests": enabled})

    def messages(self, ven: str, type: str | None = None, after: int = 0) -> list[dict[str, Any]]:
        params: dict[str, Any] = {"after": after}
        if type:
            params["type"] = type
        return self._call("GET", f"/vens/{ven}/messages", params=params)

    def last_seq(self, ven: str) -> int:
        messages = self.messages(ven)
        return messages[-1]["seq"] if messages else 0

    def wait_for_response(self, ven: str, event_id: str, modification_number: int,
                          via: str = "oadrCreatedEvent") -> dict[str, Any]:
        """The VEN's opt answer for one modification of an event."""
        def check():
            return next((r for r in self.event(ven, event_id)["responses"]
                         if r["modification_number"] == modification_number
                         and r["via"] == via), None)
        return wait_for(check, f"the VEN's {via} for event {event_id} "
                               f"modification {modification_number}", self.timeout)

    def wait_for_message(self, ven: str, type: str, after: int = 0,
                         where: Callable[[dict[str, Any]], bool] = lambda m: True
                         ) -> dict[str, Any]:
        return wait_for(
            lambda: next((m for m in self.messages(ven, type, after) if where(m)), None),
            f"a {type} from VEN {ven}", self.timeout)
