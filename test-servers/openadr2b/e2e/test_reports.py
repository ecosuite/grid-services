"""Reports: the status report, and usage reports read from SolarNetwork."""

import time
import uuid
from datetime import datetime, timedelta, timezone
from typing import Any

import pytest
from support import Connection
from test_common.e2e import parse_time, wait_for

NODE_ID = 604
SOURCE_ID = "/E2E/1"


def updates(vtn, ven_name, report_name, since=None):
    return [u for u in vtn.reports(ven_name)["updates"]
            if u["report_name"] == report_name
            and (since is None or parse_time(u["at"]) > since)]


def interval_payloads(interval):
    """OpenLEADR gives one payload as a dict and several as a list."""
    entries = interval["report_payload"]
    return entries if isinstance(entries, list) else [entries]


def status_of(update):
    return interval_payloads(update["intervals"][0])[0]["payload_resource_status"]


def value_of(payload):
    """OpenLEADR unwraps payload_float for a lone payload but not in a list."""
    if "value" in payload:
        return payload["value"]
    wrapped = payload.get("payload_float") or {}
    return float(wrapped["value"]) if "value" in wrapped else None


def payloads(update):
    """(rID, value, quality) of every payload in an update, interval by interval."""
    return [(payload.get("r_id"), value_of(payload), payload.get("data_quality"))
            for interval in update["intervals"]
            for payload in interval_payloads(interval)]


# What the site measures: power and energy from one stream.
USAGE_POINTS = [
    {"point_id": "site-power", "kind": "power_real", "units": "W",
     "source": {"node_id": NODE_ID, "source_id": SOURCE_ID, "property": "watts",
                "property_class": "instantaneous"}},
    {"point_id": "site-energy", "kind": "energy_real", "units": "Wh",
     "source": {"node_id": NODE_ID, "source_id": SOURCE_ID, "property": "wattHours",
                "property_class": "accumulating"}},
]


def offer_usage(connection: Connection, specifier_id: str, **offer: Any) -> int:
    """Gives the connection the usage points, and offers them in a report as
    power and energy."""
    connection.add_points(USAGE_POINTS)
    resource_id = connection.site_code
    return connection.add_offer(specifier_id, [
        {"rid": "power", "point_id": "site-power", "resource_id": resource_id,
         "report_type": "usage", "reading_type": "Direct Read"},
        {"rid": "energy", "point_id": "site-energy", "resource_id": resource_id,
         "report_type": "usage", "reading_type": "Direct Read"},
    ], **offer)


def seed_stream(solarnetwork, seconds_back: int = 900, seconds_ahead: int = 300,
                watts: float = 100.0) -> None:
    """A datum every second around now: constant power, and energy growing
    by one watt-hour a second."""
    now = datetime.now(timezone.utc).replace(microsecond=0)
    datums = []
    for offset in range(-seconds_back, seconds_ahead):
        at = now + timedelta(seconds=offset)
        datums.append({"created": at.isoformat().replace("+00:00", "Z"),
                       "watts": watts, "wattHours": 1_000_000 + int(at.timestamp())})
    solarnetwork.set_stream(NODE_ID, SOURCE_ID, datums)


@pytest.fixture
def credentialed(make_connection, solarnetwork):
    """A connection with a SolarNetwork token the fake accepts, not yet
    registered, so a test can add offers first."""
    token = f"tok-{uuid.uuid4().hex[:8]}"
    secret = uuid.uuid4().hex
    solarnetwork.set_token(token, secret)
    connection: Connection = make_connection()
    connection.set_data_credential(solarnetwork.worker_facing_url, token, secret)
    return connection


@pytest.fixture
def manual_requests(vtn):
    """Turns the VTN's automatic report requests off for one test."""
    vtn.set_auto_report_requests(False)
    yield
    vtn.set_auto_report_requests(True)


# -- The status report ------------------------------------------------------

def test_status_report_is_offered_requested_and_delivered(registered, vtn):
    reports = wait_for(lambda: vtn.reports(registered.ven_name)
                       if vtn.reports(registered.ven_name)["requests"] else None,
                       "the VTN to request the status report", vtn.timeout)
    # With nothing configured, the status report is all the VEN offers.
    offered = {r["report_name"]: r["r_ids"] for r in reports["offered"]}
    assert offered == {"TELEMETRY_STATUS": ["status"]}

    wait_for(lambda: all(r["created"] for r in vtn.reports(registered.ven_name)["requests"]),
             "the VEN to create the requested reports", vtn.timeout)
    status = wait_for(lambda: updates(vtn, registered.ven_name, "TELEMETRY_STATUS"),
                      "a TELEMETRY_STATUS update", vtn.timeout)[0]
    assert status_of(status)["online"] is True
    assert status_of(status)["manual_override"] is False

    # The request and the delivery are on record for the API.
    requests = wait_for(registered.report_requests, "the request in the database",
                        vtn.timeout)
    assert requests[0]["report_name"] == "TELEMETRY_STATUS"
    assert requests[0]["kind"] == "periodic"
    assert requests[0]["status"] == "active"
    delivery = registered.wait_for_deliveries(1)[0]
    assert delivery["status"] == "sent"
    assert delivery["response_code"] == "200"
    assert delivery["payload"][0]["rid"] == "status"
    assert delivery["payload"][0]["online"] is True


def test_reports_are_periodic(registered, vtn):
    interval = vtn.info()["report_interval_seconds"]
    wait_for(lambda: len(updates(vtn, registered.ven_name, "TELEMETRY_STATUS")) >= 2,
             "a second TELEMETRY_STATUS update", vtn.timeout + interval)


def test_pause_is_reported_as_manual_override(registered, vtn):
    wait_for(lambda: updates(vtn, registered.ven_name, "TELEMETRY_STATUS"),
             "a TELEMETRY_STATUS update", vtn.timeout)

    registered.run("pause")
    paused_at = datetime.now(timezone.utc)

    update = wait_for(lambda: updates(vtn, registered.ven_name, "TELEMETRY_STATUS", paused_at),
                      "a TELEMETRY_STATUS update after pausing", vtn.timeout)[0]
    assert status_of(update)["manual_override"] is True


def test_unregistering_stops_reports(registered, vtn):
    wait_for(lambda: updates(vtn, registered.ven_name, "TELEMETRY_STATUS"),
             "a TELEMETRY_STATUS update", vtn.timeout)
    wait_for(lambda: [r for r in registered.report_requests() if r["status"] == "active"],
             "an active request in the database", vtn.timeout)

    registered.run("unregister")
    stopped_at = datetime.now(timezone.utc)

    # The requests went with the registration.
    assert all(r["status"] == "cancelled" for r in registered.report_requests())
    # Wait past two report intervals; nothing more may arrive.
    interval = vtn.info()["report_interval_seconds"]
    time.sleep(2 * interval + 1)
    assert updates(vtn, registered.ven_name, "TELEMETRY_STATUS", stopped_at) == []


# -- Usage reports from SolarNetwork -----------------------------------------

def test_usage_report_is_delivered_from_solarnetwork_datums(credentialed, vtn,
                                                            solarnetwork):
    seed_stream(solarnetwork)
    interval = int(vtn.info()["report_interval_seconds"])
    offer_usage(credentialed, "usage-1",
                min_sampling_seconds=interval, max_sampling_seconds=3600)
    credentialed.register()

    # The offer is in the metadata, with both data points, and the VTN
    # asked for it.
    reports = wait_for(lambda: vtn.reports(credentialed.ven_name)
                       if len(vtn.reports(credentialed.ven_name)["requests"]) == 2 else None,
                       "the VTN to request both reports", vtn.timeout)
    offered = {r["report_name"]: r["r_ids"] for r in reports["offered"]}
    assert offered == {"TELEMETRY_STATUS": ["status"], "TELEMETRY_USAGE": ["power", "energy"]}
    descriptions = {d["r_id"]: d for r in reports["offered"]
                    if r["report_name"] == "TELEMETRY_USAGE" for d in r["descriptions"]}
    assert descriptions["power"]["report_type"] == "usage"
    assert descriptions["power"]["reading_type"] == "Direct Read"
    assert descriptions["energy"]["reading_type"] == "Direct Read"

    # The values come from the datums: constant power, and one watt-hour a
    # second over the report-back period.
    usage = wait_for(lambda: updates(vtn, credentialed.ven_name, "TELEMETRY_USAGE"),
                     "a TELEMETRY_USAGE update", vtn.timeout)[0]
    values = {rid: (value, quality) for rid, value, quality in payloads(usage)}
    assert values["power"] == (100.0, "Quality Good - Non Specific")
    assert values["energy"] == (float(interval), "Quality Good - Non Specific")

    # What the API sees.
    delivery = credentialed.wait_for_deliveries(
        1, lambda d: any(p["rid"] == "power" for p in d["payload"]))[0]
    assert delivery["status"] == "sent"
    by_rid = {p["rid"]: p for p in delivery["payload"]}
    assert by_rid["power"]["value"] == 100.0
    assert by_rid["power"]["point_id"] == "site-power"
    assert by_rid["power"]["quality"] == "good"
    assert by_rid["energy"]["value"] == float(interval)
    assert by_rid["energy"]["point_id"] == "site-energy"
    assert by_rid["energy"]["duration_seconds"] == interval
    request = credentialed.api.one(
        "SELECT * FROM openadr2b_report_requests WHERE id = %s", delivery["request_id"])
    assert request["report_name"] == "TELEMETRY_USAGE"
    assert request["rids"] == ["power", "energy"]
    assert request["offer_id"] is not None

    # The worker signed its SolarNetwork requests properly.
    requests = [r for r in solarnetwork.requests()
                if r["query"].get("sourceId") == SOURCE_ID]
    assert requests, "the worker read no datums"
    assert all(r["authorized"] for r in requests), requests


def test_one_shot_request_returns_the_latest_sample(credentialed, vtn, solarnetwork,
                                                     manual_requests):
    seed_stream(solarnetwork, watts=250)
    offer_usage(credentialed, "usage-1")
    credentialed.register()
    wait_for(lambda: "usage-1" in {r["report_specifier_id"]
                                   for r in vtn.reports(credentialed.ven_name)["offered"]},
             "the usage offer to be registered", vtn.timeout)

    request = vtn.request_report(credentialed.ven_name, report_specifier_id="usage-1",
                                 r_ids=["power"], one_shot=True, report_back_seconds=0)
    update = wait_for(lambda: updates(vtn, credentialed.ven_name, "TELEMETRY_USAGE"),
                      "the one-shot TELEMETRY_USAGE update", vtn.timeout)[0]
    assert update["report_request_id"] == request["report_request_id"]
    assert payloads(update) == [("power", 250.0, "Quality Good - Non Specific")]

    recorded = wait_for(
        lambda: [r for r in credentialed.report_requests()
                 if r["report_request_id"] == request["report_request_id"]
                 and r["status"] != "active"],
        "the one-shot request to complete", vtn.timeout)[0]
    assert recorded["kind"] == "one_shot"
    assert recorded["status"] == "completed"
    assert "asked for TELEMETRY_USAGE (usage-1" in " ".join(credentialed.activity())


def test_history_is_served_and_bounded_by_the_offer(credentialed, vtn, solarnetwork,
                                                     manual_requests):
    seed_stream(solarnetwork)
    offer_usage(credentialed, "history-1",
                report_name="HISTORY_USAGE", min_sampling_seconds=60,
                history_seconds=600)
    credentialed.register()
    wait_for(lambda: "history-1" in {r["report_specifier_id"]
                                     for r in vtn.reports(credentialed.ven_name)["offered"]},
             "the history offer to be registered", vtn.timeout)

    # Ten minutes of history at the offered minute sampling.
    request = vtn.request_report(credentialed.ven_name, report_specifier_id="history-1",
                                 r_ids=["energy"], report_back_seconds=0,
                                 granularity_seconds=0, start_in_seconds=-600,
                                 duration_seconds=600)
    update = wait_for(lambda: updates(vtn, credentialed.ven_name, "HISTORY_USAGE"),
                      "the HISTORY_USAGE update", vtn.timeout)[0]
    assert update["report_request_id"] == request["report_request_id"]
    values = payloads(update)
    assert len(values) == 10
    assert all(rid == "energy" and value == 60.0 and quality == "Quality Good - Non Specific"
               for rid, value, quality in values), values

    # More than the offered history is refused with 454 (rule 340).
    answers_before = len(vtn.reports(credentialed.ven_name)["answers"])
    refused = vtn.request_report(credentialed.ven_name, report_specifier_id="history-1",
                                 r_ids=["energy"], report_back_seconds=0,
                                 granularity_seconds=0, start_in_seconds=-1200,
                                 duration_seconds=1200)
    answer = wait_for(lambda: vtn.reports(credentialed.ven_name)["answers"][answers_before:],
                      "the VEN's answer to the oversized history request", vtn.timeout)[-1]
    assert str(answer["response_code"]) == "454"
    assert refused["report_request_id"] not in {
        r["report_request_id"] for r in credentialed.report_requests()}


def test_cancelled_requests_end(credentialed, vtn, solarnetwork, manual_requests):
    seed_stream(solarnetwork)
    offer_usage(credentialed, "usage-1", min_sampling_seconds=5)
    credentialed.register()
    wait_for(lambda: "usage-1" in {r["report_specifier_id"]
                                   for r in vtn.reports(credentialed.ven_name)["offered"]},
             "the usage offer to be registered", vtn.timeout)

    request = vtn.request_report(credentialed.ven_name, report_specifier_id="usage-1",
                                 granularity_seconds=5, report_back_seconds=5)
    wait_for(lambda: [r for r in credentialed.report_requests()
                      if r["report_request_id"] == request["report_request_id"]
                      and r["status"] == "active"],
             "the periodic request to be recorded", vtn.timeout)
    credentialed.wait_for_deliveries(1)

    vtn.cancel_report(credentialed.ven_name, request["report_request_id"])
    wait_for(lambda: [r for r in credentialed.report_requests()
                      if r["report_request_id"] == request["report_request_id"]
                      and r["status"] == "cancelled"],
             "the request to be cancelled in the database", vtn.timeout)
    credentialed.wait_for_activity("cancelled report request")


def test_unreadable_datums_are_reported_as_comm_failure(make_connection, vtn, solarnetwork):
    # A token the fake SolarNetwork does not know: every read is refused.
    connection: Connection = make_connection()
    connection.set_data_credential(solarnetwork.worker_facing_url, "unknown-token",
                                   "whatever")
    interval = int(vtn.info()["report_interval_seconds"])
    offer_usage(connection, "usage-1", min_sampling_seconds=interval)
    connection.register()

    usage = wait_for(lambda: updates(vtn, connection.ven_name, "TELEMETRY_USAGE"),
                     "a TELEMETRY_USAGE update", vtn.timeout)[0]
    assert all(quality == "Quality Bad - Comm Failure" for _, _, quality in payloads(usage))
    connection.wait_for_activity("Could not read datums for report usage-1")
    delivery = connection.wait_for_deliveries(
        1, lambda d: any(p["rid"] == "power" for p in d["payload"]))[0]
    assert delivery["status"] == "sent"
    assert delivery["payload"][0]["value"] is None
    assert delivery["payload"][0]["quality"] == "unreadable"


def test_offers_without_a_credential_are_a_configuration_error(make_connection, vtn):
    connection: Connection = make_connection()
    offer_usage(connection, "usage-1")
    state = connection.wait_for_state(lambda s: s["config_error"] is not None,
                                      "to report a configuration error")
    assert "no SolarNetwork credential" in state["config_error"]
    assert connection.health() == "invalid_config"


def test_changing_offers_registers_the_reports_again(credentialed, vtn, solarnetwork):
    seed_stream(solarnetwork)
    credentialed.register()
    wait_for(lambda: vtn.reports(credentialed.ven_name)["offered"],
             "the first metadata registration", vtn.timeout)
    assert {r["report_name"] for r in vtn.reports(credentialed.ven_name)["offered"]} == {
        "TELEMETRY_STATUS"}

    # Adding an offer is a configuration change: the worker restarts the
    # connection and registers its reports afresh, and the VTN asks for the
    # new report.
    offer_usage(credentialed, "usage-1",
                min_sampling_seconds=int(vtn.info()["report_interval_seconds"]))
    wait_for(lambda: "TELEMETRY_USAGE" in {
                 r["report_name"] for r in vtn.reports(credentialed.ven_name)["offered"]},
             "the usage report to be offered", vtn.timeout)
    wait_for(lambda: updates(vtn, credentialed.ven_name, "TELEMETRY_USAGE"),
             "a TELEMETRY_USAGE update", vtn.timeout)
    assert credentialed.state()["registration"] == "registered"
