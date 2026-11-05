"""Runs the server in process for each test. `sep2` is a CSIP-AUS client
calling as site-1, `api` the control API; see support.py."""

import pytest
from support import TOKEN, ControlClient, Sep2Client, end_device

from test_csipaus.__main__ import build_app
from test_csipaus.state import Server


@pytest.fixture
def server() -> Server:
    return Server(poll_rate_seconds=30, pin=111115, time_zone="Australia/Sydney",
                  client_name_prefix="")


@pytest.fixture
async def http(aiohttp_client, server):
    return await aiohttp_client(build_app(server, TOKEN))


@pytest.fixture
def sep2(http) -> Sep2Client:
    return Sep2Client(http, "site-1")


@pytest.fixture
def api(http) -> ControlClient:
    return ControlClient(http)


@pytest.fixture
async def registered(sep2) -> str:
    """Registers the end device and returns its href."""
    return await sep2.post("edev", end_device())
