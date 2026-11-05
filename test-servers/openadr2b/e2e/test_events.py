"""Events from the program to the database, and opt answers back."""

import time
from datetime import datetime, timedelta, timezone

from test_common.e2e import parse_time


def test_event_is_stored_and_answered(registered, vtn):
    event = vtn.create_event(registered.ven_name, start_in_seconds=120, intervals=[
        {"duration_seconds": 60, "value": 1},
        {"duration_seconds": 120, "value": 2},
    ], priority=1, comment="first")

    row = registered.wait_for_event(event["event_id"])
    assert row["source"] == "program"
    assert row["site_code"] == registered.site_code
    assert row["modification_number"] == 0
    assert row["program_status"] == "pending"
    assert row["start_at"] == parse_time(event["start"])
    assert row["duration_seconds"] == 180
    assert row["market_context"] == vtn.info()["market_context"]
    assert row["response_required"] is True
    assert row["program_comment"] == "first"
    [signal] = row["signals"]
    assert (signal["name"], signal["type"]) == ("SIMPLE", "level")
    assert [(i["offset_seconds"], i["duration_seconds"], i["value"]) for i in signal["intervals"]] \
        == [(0, 60, 1.0), (60, 120, 2.0)]
    assert (row["opt"], row["opt_source"]) == ("in", "default")

    response = vtn.wait_for_response(registered.ven_name, event["event_id"], 0)
    assert response["opt"] == "optIn"
    [sent] = registered.responses(row["id"])
    assert (sent["opt"], sent["modification_number"], sent["status"]) == ("in", 0, "sent")


def test_default_opt_out(make_connection, vtn):
    connection = make_connection(default_opt="out")
    connection.register()

    event = vtn.create_event(connection.ven_name)

    response = vtn.wait_for_response(connection.ven_name, event["event_id"], 0)
    assert response["opt"] == "optOut"
    row = connection.wait_for_event(event["event_id"])
    assert (row["opt"], row["opt_source"]) == ("out", "default")


def test_active_event(registered, vtn):
    event = vtn.create_event(registered.ven_name, start_in_seconds=0,
                             intervals=[{"duration_seconds": 300, "value": 2}])

    row = registered.wait_for_event(event["event_id"])
    assert row["program_status"] == "active"


def test_open_ended_event(registered, vtn):
    # OpenLEADR writes the zero duration as "P".
    event = vtn.create_event(registered.ven_name,
                             intervals=[{"duration_seconds": 0, "value": 1}])

    row = registered.wait_for_event(event["event_id"])
    assert row["duration_seconds"] == 0
    assert vtn.wait_for_response(registered.ven_name, event["event_id"], 0)["opt"] == "optIn"


def test_modification_updates_the_event(registered, vtn):
    event = vtn.create_event(registered.ven_name, start_in_seconds=120)
    registered.wait_for_event(event["event_id"])

    vtn.modify_event(registered.ven_name, event["event_id"], start_in_seconds=600,
                     intervals=[{"duration_seconds": 90, "value": 3}])

    row = registered.wait_for_event(event["event_id"], lambda r: r["modification_number"] == 1)
    assert row["duration_seconds"] == 90
    assert row["start_at"] == parse_time(vtn.event(registered.ven_name, event["event_id"])["start"])
    assert row["signals"][0]["intervals"][0]["value"] == 3.0
    assert vtn.wait_for_response(registered.ven_name, event["event_id"], 1)["opt"] == "optIn"


def test_operator_opt_out_reaches_program_and_survives_modification(registered, vtn):
    event = vtn.create_event(registered.ven_name, start_in_seconds=120)
    row = registered.wait_for_event(event["event_id"])

    registered.run("opt_out", event_id=row["id"])

    # The opt out qualifies the answer already given, through EiOpt.
    opt = vtn.wait_for_response(registered.ven_name, event["event_id"], 0, via="oadrCreateOpt")
    assert opt["opt"] == "optOut"
    row = registered.event(event["event_id"])
    assert (row["opt"], row["opt_source"]) == ("out", "operator")

    vtn.modify_event(registered.ven_name, event["event_id"], comment="changed")

    assert vtn.wait_for_response(registered.ven_name, event["event_id"], 1)["opt"] == "optOut"
    row = registered.wait_for_event(event["event_id"], lambda r: r["modification_number"] == 1)
    assert (row["opt"], row["opt_source"]) == ("out", "operator")


def test_cancellation(registered, vtn):
    event = vtn.create_event(registered.ven_name, start_in_seconds=120)
    registered.wait_for_event(event["event_id"])

    vtn.cancel_event(registered.ven_name, event["event_id"])

    row = registered.wait_for_event(event["event_id"], lambda r: r["program_status"] == "cancelled",
                                    "to be cancelled")
    assert row["modification_number"] == 1
    # A cancellation is always confirmed with optIn (rule 36).
    assert vtn.wait_for_response(registered.ven_name, event["event_id"], 1)["opt"] == "optIn"


def test_paused_connection_opts_out_of_new_events(registered, vtn):
    registered.run("pause")
    paused = vtn.create_event(registered.ven_name)

    assert vtn.wait_for_response(registered.ven_name, paused["event_id"], 0)["opt"] == "optOut"
    row = registered.wait_for_event(paused["event_id"])
    assert (row["opt"], row["opt_source"]) == ("out", "paused")

    registered.run("resume")
    resumed = vtn.create_event(registered.ven_name)

    assert vtn.wait_for_response(registered.ven_name, resumed["event_id"], 0)["opt"] == "optIn"


def test_event_without_response(registered, vtn):
    event = vtn.create_event(registered.ven_name, response_required=False)

    row = registered.wait_for_event(event["event_id"])
    assert row["response_required"] is False
    # The runner handles commands between polls, so once this one is done
    # the VEN has finished with the distribution.
    registered.run("test")
    assert vtn.event(registered.ven_name, event["event_id"])["responses"] == []
    assert registered.responses(row["id"]) == []


def test_randomized_start_is_passed_on(registered, vtn):
    event = vtn.create_event(registered.ven_name, start_after_seconds=120)

    row = registered.wait_for_event(event["event_id"])
    assert row["start_after_seconds"] == 120


PRICE_AND_DISPATCH = [
    {"name": "ELECTRICITY_PRICE", "type": "price",
     "intervals": [{"duration_seconds": 300, "value": 0.25}],
     "measurement": {"name": "currencyPerKWh", "description": "currencyPerKWh",
                     "unit": "USD", "scale": "none"}},
    {"name": "LOAD_DISPATCH", "type": "setpoint",
     "intervals": [{"duration_seconds": 300, "value": 5}],
     "measurement": {"name": "powerReal", "description": "RealPower",
                     "unit": "W", "scale": "k"}},
]


def test_price_and_dispatch_signals(make_connection, vtn):
    connection = make_connection(
        accepted_signals=["SIMPLE", "ELECTRICITY_PRICE", "LOAD_DISPATCH"])
    connection.register()

    event = vtn.create_event(connection.ven_name, signals=PRICE_AND_DISPATCH)

    row = connection.wait_for_event(event["event_id"])
    assert [s["name"] for s in row["signals"]] == ["ELECTRICITY_PRICE", "LOAD_DISPATCH"]
    assert vtn.wait_for_response(connection.ven_name, event["event_id"], 0)["opt"] == "optIn"


def test_signals_the_site_cannot_act_on_are_refused(registered, vtn):
    # A connection accepts only SIMPLE unless it is told otherwise.
    event = vtn.create_event(registered.ven_name, signals=PRICE_AND_DISPATCH)

    response = vtn.wait_for_response(registered.ven_name, event["event_id"], 0)
    assert (response["opt"], response["response_code"]) == ("optOut", 460)
    assert registered.event(event["event_id"]) is None


def test_unsupported_signal_is_refused(registered, vtn):
    event = vtn.create_event(registered.ven_name, signals=[
        {"name": "BID_LOAD", "type": "setpoint",
         "intervals": [{"duration_seconds": 300, "value": 1}]},
    ])

    response = vtn.wait_for_response(registered.ven_name, event["event_id"], 0)
    assert (response["opt"], response["response_code"]) == ("optOut", 460)
    assert registered.event(event["event_id"]) is None


def test_event_for_another_program_is_refused(make_connection, vtn):
    connection = make_connection(market_context="http://another-program.example")
    connection.register()

    event = vtn.create_event(connection.ven_name)

    response = vtn.wait_for_response(connection.ven_name, event["event_id"], 0)
    assert (response["opt"], response["response_code"]) == ("optOut", 462)
    assert connection.event(event["event_id"]) is None


def test_ended_events_are_closed(registered, vtn):
    # Long enough to outlast a poll interval, so the VEN sees it running.
    length = 3 * vtn.info()["poll_interval_seconds"]
    event = vtn.create_event(registered.ven_name, start_in_seconds=0,
                             intervals=[{"duration_seconds": int(length), "value": 1}])
    registered.wait_for_event(event["event_id"])
    time.sleep(max(0.0, (parse_time(event["start"]) + timedelta(seconds=length + 1)
                         - datetime.now(timezone.utc)).total_seconds()))

    # The next distribution no longer lists the ended event.
    vtn.create_event(registered.ven_name)

    row = registered.wait_for_event(event["event_id"],
                                    lambda r: r["program_status"] == "completed",
                                    "to be completed")
    assert row["modification_number"] == 0


def test_operator_opt_in_to_one_event_while_paused(registered, vtn):
    registered.run("pause")
    event = vtn.create_event(registered.ven_name, start_in_seconds=120)
    row = registered.wait_for_event(event["event_id"])
    assert (row["opt"], row["opt_source"]) == ("out", "paused")
    assert vtn.wait_for_response(registered.ven_name, event["event_id"], 0)["opt"] == "optOut"

    command = registered.run("opt_in", event_id=row["id"])

    assert command["result"] == {"opt": "in", "program_told": True}
    opt = vtn.wait_for_response(registered.ven_name, event["event_id"], 0, via="oadrCreateOpt")
    assert opt["opt"] == "optIn"
    row = registered.event(event["event_id"])
    assert (row["opt"], row["opt_source"]) == ("in", "operator")
    assert registered.state()["participation"] == "paused"
    entry = registered.wait_for_activity_entry("Opted in to event")
    assert (entry["actor"], entry["event_id"]) == ("e2e", row["id"])


def test_opt_in_is_refused_once_the_event_has_started(registered, vtn):
    event = vtn.create_event(registered.ven_name, start_in_seconds=0,
                             intervals=[{"duration_seconds": 300, "value": 1}])
    row = registered.wait_for_event(event["event_id"])
    registered.run("opt_out", event_id=row["id"])

    command = registered.run("opt_in", event_id=row["id"], expect="failed")

    assert command["message"] == "Event has already started."
    assert registered.event(event["event_id"])["opt"] == "out"


def test_event_changes_are_signalled(registered, vtn, listen):
    events = listen("grid_services_events")

    event = vtn.create_event(registered.ven_name, start_in_seconds=120)
    row = registered.wait_for_event(event["event_id"])
    events.wait_for(row["id"])

    # The planner's own change is not signalled back to it; the program's
    # cancellation is.
    events.payloads.clear()
    registered.api.execute("UPDATE events SET lifecycle = 'planned' WHERE id = %s", row["id"])
    assert not events.heard(row["id"], timeout=1)
    vtn.cancel_event(registered.ven_name, event["event_id"])
    events.wait_for(row["id"])


def test_event_state_follows_the_program_and_the_operator(registered, vtn):
    def state(row):
        return registered.api.one("SELECT state, end_at FROM event_state WHERE id = %s",
                                  row["id"])

    event = vtn.create_event(registered.ven_name, start_in_seconds=120,
                             intervals=[{"duration_seconds": 300, "value": 1}])
    row = registered.wait_for_event(event["event_id"])
    assert state(row)["state"] == "received"
    assert state(row)["end_at"] == row["start_at"] + timedelta(seconds=300)

    registered.run("opt_out", event_id=row["id"])
    assert state(row)["state"] == "opted_out"

    vtn.cancel_event(registered.ven_name, event["event_id"])
    registered.wait_for_event(event["event_id"], lambda r: r["program_status"] == "cancelled",
                              "to be cancelled")
    # The planner has not touched the event, yet it reads as cancelled.
    assert registered.event(event["event_id"])["lifecycle"] == "received"
    assert state(row)["state"] == "cancelled"


def test_events_carry_the_site(registered, vtn):
    site_id = registered.site_id

    event = vtn.create_event(registered.ven_name)

    row = registered.wait_for_event(event["event_id"])
    assert (row["site_code"], row["site_id"]) == (registered.site_code, site_id)
    assert all(entry["site_id"] == site_id for entry in registered.activity_entries())
