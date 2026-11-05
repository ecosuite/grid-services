"""Runs the test VTN.

Configured through the environment:

    TEST_VTN_ID               vtnID sent to VENs (default test-vtn)
    TEST_VTN_PORT, PORT       HTTP port (default 8080)
    TEST_VTN_POLL_SECONDS     poll interval requested from VENs (default 10)
    TEST_VTN_REPORT_SECONDS   telemetry report interval requested (default 60)
    TEST_VTN_MARKET_CONTEXT   market context of events (default http://test-vtn/program)
    TEST_VTN_CONTROL_TOKEN    bearer token for /api; unset leaves it open
    TEST_VTN_VEN_NAME_PREFIX  registrations must use VEN names with this prefix
    TEST_VTN_LOG_LEVEL        debug, info, warning or error (default info)
"""

import logging
import os
from datetime import timedelta

from aiohttp import web
from test_common.control import control_middleware
from test_common.solarnetwork import FakeSolarNetwork

from test_vtn.control import ControlApi
from test_vtn.openadr import OpenadrEndpoints
from test_vtn.state import Vtn


def seconds(name: str, default: int) -> timedelta:
    value = int(os.environ.get(name, default))
    if value <= 0:
        raise SystemExit(f"{name} must be positive")
    return timedelta(seconds=value)


def build_app(vtn: Vtn, token: str | None) -> web.Application:
    solarnetwork = FakeSolarNetwork()
    app = web.Application(middlewares=[control_middleware(token)])
    app.add_routes(OpenadrEndpoints(vtn).routes())
    app.add_routes(solarnetwork.routes())
    app.add_routes(ControlApi(vtn).routes())
    app.add_routes(solarnetwork.control_routes())
    return app


def main() -> None:
    level = os.environ.get("TEST_VTN_LOG_LEVEL", "info").upper()
    logging.basicConfig(level=level, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    # OpenLEADR logs every message it renders at debug level.
    logging.getLogger("openleadr").setLevel(max(logging.INFO, logging.getLevelName(level)))

    vtn = Vtn(
        vtn_id=os.environ.get("TEST_VTN_ID", "test-vtn"),
        market_context=os.environ.get("TEST_VTN_MARKET_CONTEXT", "http://test-vtn/program"),
        poll_interval=seconds("TEST_VTN_POLL_SECONDS", 10),
        report_interval=seconds("TEST_VTN_REPORT_SECONDS", 60),
        ven_name_prefix=os.environ.get("TEST_VTN_VEN_NAME_PREFIX", ""),
    )
    token = os.environ.get("TEST_VTN_CONTROL_TOKEN") or None
    if token is None:
        logging.getLogger("test_vtn").warning(
            "TEST_VTN_CONTROL_TOKEN is not set; the control API is open")
    port = int(os.environ.get("TEST_VTN_PORT") or os.environ.get("PORT") or 8080)
    web.run_app(build_app(vtn, token), host="0.0.0.0", port=port, access_log=None)


if __name__ == "__main__":
    main()
