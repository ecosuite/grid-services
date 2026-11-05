"""Registration, driven from both ends."""

from datetime import datetime, timezone

import psycopg
import pytest
from test_common.e2e import parse_time, wait_for


def test_register_and_unregister(connection, vtn):
    state = connection.register()

    assert state["registration"] == "registered"
    assert state["client_id"] == connection.ven_name
    assert state["program_poll_interval_seconds"] == vtn.info()["poll_interval_seconds"]
    ven = vtn.ven(connection.ven_name)
    assert ven["registered"]
    assert ven["registration_id"] == state["registration_id"]

    connection.run("unregister")

    assert connection.state()["registration"] == "unregistered"
    assert not vtn.ven(connection.ven_name)["registered"]


def test_connection_test_reports_registration(connection):
    before = connection.run("test")
    assert "no registration for this VEN" in before["message"]
    assert before["result"] == {"registered": False, "client_id": None}

    connection.register()
    after = connection.run("test")

    assert f"registered as {connection.ven_name}" in after["message"]
    assert after["result"] == {"registered": True, "client_id": connection.ven_name}
    # A test never registers or unregisters.
    assert connection.state()["registration"] == "registered"


def test_restart_resumes_registration(registered, vtn):
    registration_id = registered.state()["registration_id"]
    after = vtn.last_seq(registered.ven_name)

    registered.run("reload_config")

    # The restarted connection asks the VTN whether it is still registered,
    # rather than registering again.
    query = vtn.wait_for_message(registered.ven_name, "oadrQueryRegistration", after)
    assert query["reply_payload"]["registration_id"] == registration_id
    vtn.wait_for_message(registered.ven_name, "oadrRequestEvent", after)
    assert vtn.messages(registered.ven_name, "oadrCreatePartyRegistration", after) == []
    state = registered.state()
    assert state["registration"] == "registered"
    assert state["registration_id"] == registration_id


def test_program_cancels_registration(registered, vtn):
    vtn.cancel_registration(registered.ven_name)

    registered.wait_for_activity("The program cancelled this connection's registration")
    assert registered.state()["registration"] == "unregistered"
    ack = vtn.wait_for_message(registered.ven_name, "oadrCanceledPartyRegistration")
    assert ack["payload"]["response"]["response_code"] == 200
    assert not vtn.ven(registered.ven_name)["registered"]


def test_program_requests_reregistration(registered, vtn):
    registration_id = registered.state()["registration_id"]
    after = vtn.last_seq(registered.ven_name)

    vtn.request_reregistration(registered.ven_name)

    # Registering again references the current registration (rule 402).
    request = vtn.wait_for_message(registered.ven_name, "oadrCreatePartyRegistration", after)
    assert request["payload"]["registration_id"] == registration_id
    registered.wait_for_activity("The program asked this connection to register again")
    state = wait_for(lambda: registered.state()["registration"] == "registered"
                     and registered.state(), "the connection to be registered again", vtn.timeout)
    assert state["registration_id"] == registration_id


def test_command_results(connection, listen):
    results = listen("grid_services_command_results")

    command = connection.run("register")

    state = connection.state()
    assert command["result"] == {
        "client_id": state["client_id"],
        "registration_id": state["registration_id"],
        "poll_interval_seconds": state["program_poll_interval_seconds"],
    }
    results.wait_for(command["id"])


def test_activity_names_the_operator_and_carries_the_exchange(connection):
    before = datetime.now(timezone.utc)
    connection.run("test")
    connection.run("register")
    connection.run("unregister")

    for fragment in ("Connection test passed", "Registered with the program",
                     "Unregistered from the program"):
        entry = connection.wait_for_activity_entry(fragment)
        assert entry["actor"] == "e2e", fragment
        exchange = entry["detail"]["exchange"]
        assert [m["direction"] for m in exchange][:2] == ["to_program", "from_program"]
        for message in exchange:
            assert set(message) == {"direction", "at", "format", "body"}
            assert message["format"] == "xml"
            assert message["body"].lstrip().startswith("<")
            assert before.replace(microsecond=0) <= parse_time(message["at"]) \
                <= datetime.now(timezone.utc)


def test_health(connection):
    assert connection.health() == "unregistered"

    connection.register()
    # Healthy once the first poll has gone through.
    wait_for(lambda: connection.state()["last_poll_at"], "the first poll", connection.api.timeout)
    assert connection.health() == "healthy"

    connection.update(enabled=False)
    assert connection.health() == "disabled"


@pytest.mark.parametrize("retire, reason", [
    ({"enabled": False}, "disabled"),
    ({"archived_at": datetime.now(timezone.utc)}, "archived"),
])
def test_retired_connection_is_unregistered(registered, vtn, retire, reason):
    registered.update(**retire)

    registered.wait_for_state(lambda s: s["registration"] == "unregistered",
                              "to be unregistered")
    assert not vtn.ven(registered.ven_name)["registered"]
    entry = registered.wait_for_activity_entry(
        f"Unregistered from the program because the connection was {reason}")
    assert entry["actor"] == "system"

    # Nothing runs a retired connection, so its commands fail.
    command = registered.run("test", expect="failed")
    assert command["message"] == f"The connection is {reason}."


def test_registered_connection_cannot_move_to_another_program(registered):
    with pytest.raises(psycopg.errors.ObjectNotInPrerequisiteState):
        registered.update_settings(vtn_url="https://another-program.example")

    registered.run("unregister")
    registered.update_settings(vtn_url="https://another-program.example")
