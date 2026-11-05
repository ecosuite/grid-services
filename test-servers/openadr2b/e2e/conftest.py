"""Configuration for the end-to-end suite.

    E2E_DATABASE_URL        the grid-services database, as the API reaches it
    E2E_VTN_URL             the test VTN, as the worker reaches it
    E2E_VTN_CONTROL_URL     the test VTN, as the suite reaches it
                            (default E2E_VTN_URL)
    E2E_VTN_CONTROL_TOKEN   the VTN's control token, if it has one
    E2E_WORKER_PUBLIC_KEY   the worker's public key, to seal secrets for it
    E2E_TIMEOUT_SECONDS     how long to wait for anything (default 30)

The fixtures every suite shares come from `test_common.fixtures`.

Each test creates its own connection and VEN and removes both afterwards,
so the suite is safe against a database and worker that others use.
"""

import os
from typing import Iterator

import pytest
from support import Api, Connection, VtnControl
from test_common.e2e import SolarNetworkControl, setting

pytest_plugins = ["test_common.fixtures"]


@pytest.fixture(scope="session")
def api(timeout: float) -> Iterator[Api]:
    api = Api(setting("E2E_DATABASE_URL"), setting("E2E_VTN_URL"), timeout,
              os.environ.get("E2E_WORKER_PUBLIC_KEY") or None)
    yield api
    api.close()


@pytest.fixture(scope="session")
def vtn(timeout: float) -> Iterator[VtnControl]:
    vtn = VtnControl(setting("E2E_VTN_CONTROL_URL", os.environ.get("E2E_VTN_URL")),
                     os.environ.get("E2E_VTN_CONTROL_TOKEN") or None, timeout)
    yield vtn
    vtn.close()


@pytest.fixture(scope="session")
def solarnetwork(timeout: float) -> Iterator[SolarNetworkControl]:
    """The stand-in SolarNetwork, which the test VTN serves."""
    solarnetwork = SolarNetworkControl(
        setting("E2E_VTN_CONTROL_URL", os.environ.get("E2E_VTN_URL")),
        os.environ.get("E2E_VTN_CONTROL_TOKEN") or None, timeout, setting("E2E_VTN_URL"))
    yield solarnetwork
    solarnetwork.close()


@pytest.fixture
def make_connection(api: Api, vtn: VtnControl) -> Iterator:
    """Creates connections for one test, and removes them and their VENs
    when it ends."""
    created: list[Connection] = []

    def make(**options) -> Connection:
        connection = api.create_connection(**options)
        created.append(connection)
        return connection

    yield make
    for connection in created:
        api.delete_connection(connection)
        vtn.delete_ven(connection.ven_name)


@pytest.fixture
def connection(make_connection) -> Connection:
    return make_connection()


@pytest.fixture
def registered(connection: Connection) -> Connection:
    """A connection registered with the test VTN."""
    state = connection.register()
    assert state["registration"] == "registered"
    return connection
