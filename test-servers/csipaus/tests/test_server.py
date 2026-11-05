"""The server's own behaviour: refusals, the exchange log, the control API's
token and the schema check."""

import pytest
import support
from support import Sep2Client, end_device

from test_csipaus.__main__ import build_app
from test_csipaus.state import Server


async def test_unknown_resources_and_methods_are_refused(sep2, registered, http):
    await sep2.get("nothing", status=404)
    await sep2.get("edev/9", status=404)
    response = await http.delete("/d/site-1/dcap")
    assert response.status == 405
    assert response.headers["Allow"] == "GET"


async def test_unreadable_documents_are_refused(sep2):
    await sep2.post("edev", "<EndDevice", status=400)
    # CSIP-AUS needs the lFDI.
    await sep2.post("edev", f'<EndDevice xmlns="{support.SEP}"><sFDI>1</sFDI>'
                            "<changedTime>0</changedTime></EndDevice>", status=400)


async def test_every_exchange_is_logged(sep2, api):
    await sep2.get("dcap")
    await sep2.post("edev", end_device())
    await sep2.get("edev/9", status=404)

    messages = await api.call("GET", "/clients/site-1/messages")
    assert [(m["method"], m["path"], m["status"]) for m in messages] == [
        ("GET", "dcap", 200), ("POST", "edev", 201), ("GET", "edev/9", 404)]
    assert messages[1]["request_type"] == "EndDevice"
    assert messages[1]["location"] == "/d/site-1/edev/1"
    assert messages[2]["error"] == "not found: edev/9"
    assert "request_xml" not in messages[1]

    posted = await api.call("GET", "/clients/site-1/messages?type=EndDevice&xml=1")
    assert len(posted) == 1 and posted[0]["request_xml"].startswith("<EndDevice")
    after = await api.call("GET", f"/clients/site-1/messages?after={messages[1]['seq']}")
    assert [m["path"] for m in after] == ["edev/9"]


async def test_the_control_api_needs_the_token(http):
    assert (await http.get("/api/clients")).status == 401
    # The client side and the page do not.
    assert (await http.get("/d/site-1/dcap")).status == 200
    assert (await http.get("/")).status == 200


async def test_client_names_can_be_restricted(aiohttp_client):
    http = await aiohttp_client(build_app(
        Server(poll_rate_seconds=30, pin=111115, time_zone="UTC",
               client_name_prefix="test-"), None))
    await Sep2Client(http, "site-1").get("dcap", status=403)
    await Sep2Client(http, "test-site-1").get("dcap")


async def test_solarnetwork_is_served_beside(http):
    response = await http.get("/solarquery/api/v1/sec/datum/mostRecent?nodeId=1&sourceId=a")
    assert response.status == 401


def test_schema_is_checked():
    """Every document the tests read is validated when sep.xsd is at hand."""
    if support.SCHEMA is None:
        pytest.skip("IEEE 2030.5 sep.xsd is not available; documents went unvalidated")
