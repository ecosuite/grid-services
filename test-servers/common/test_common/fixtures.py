"""Fixtures every end-to-end suite shares, as a pytest plugin. A suite loads
it with `pytest_plugins = ["test_common.fixtures"]` and supplies an `api`
fixture that returns its `test_common.e2e.Api`.

    E2E_TIMEOUT_SECONDS     how long to wait for anything (default 30)
"""

import os
from typing import Iterator

import pytest

from test_common.e2e import Api, Listener


@pytest.fixture(scope="session")
def timeout() -> float:
    return float(os.environ.get("E2E_TIMEOUT_SECONDS", "30"))


@pytest.fixture
def listen(api: Api) -> Iterator:
    """Listens to the database's change signals for one test."""
    listeners: list[Listener] = []

    def listen(channel: str) -> Listener:
        listener = api.listen(channel)
        listeners.append(listener)
        return listener

    yield listen
    for listener in listeners:
        listener.close()
