"""Runs the CSIP-AUS test server.

Configured through the environment:

    TEST_CSIPAUS_PORT, PORT            HTTP port (default 8080)
    TEST_CSIPAUS_POLL_SECONDS          pollRate given to clients (default 60)
    TEST_CSIPAUS_POST_SECONDS          postRate given to mirror usage points
                                       (default 60)
    TEST_CSIPAUS_PIN                   PIN in every new Registration (default 111115)
    TEST_CSIPAUS_TIME_ZONE             zone the Time resource reports
                                       (default Australia/Sydney)
    TEST_CSIPAUS_CONTROL_TOKEN         bearer token for /api; unset leaves it open
    TEST_CSIPAUS_CLIENT_NAME_PREFIX    client names must start with this prefix
    TEST_CSIPAUS_LOG_LEVEL             debug, info, warning or error (default info)
"""

import logging
import os
from zoneinfo import ZoneInfo

from aiohttp import web
from test_common.control import control_middleware
from test_common.solarnetwork import FakeSolarNetwork

from test_csipaus.control import ControlApi
from test_csipaus.sep2 import Sep2Endpoints
from test_csipaus.state import Server


def build_app(server: Server, token: str | None) -> web.Application:
    solarnetwork = FakeSolarNetwork()
    app = web.Application(middlewares=[control_middleware(token)])
    app.add_routes(Sep2Endpoints(server).routes())
    app.add_routes(solarnetwork.routes())
    app.add_routes(ControlApi(server).routes())
    app.add_routes(solarnetwork.control_routes())
    return app


def main() -> None:
    level = os.environ.get("TEST_CSIPAUS_LOG_LEVEL", "info").upper()
    logging.basicConfig(level=level, format="%(asctime)s %(levelname)s %(name)s: %(message)s")

    poll = int(os.environ.get("TEST_CSIPAUS_POLL_SECONDS", 60))
    if poll <= 0:
        raise SystemExit("TEST_CSIPAUS_POLL_SECONDS must be positive")
    post = int(os.environ.get("TEST_CSIPAUS_POST_SECONDS", 60))
    if post <= 0:
        raise SystemExit("TEST_CSIPAUS_POST_SECONDS must be positive")
    pin = int(os.environ.get("TEST_CSIPAUS_PIN", 111115))
    if not 0 <= pin <= 999999:
        raise SystemExit("TEST_CSIPAUS_PIN must have at most six digits")
    zone = os.environ.get("TEST_CSIPAUS_TIME_ZONE", "Australia/Sydney")
    ZoneInfo(zone)

    server = Server(poll_rate_seconds=poll, pin=pin, time_zone=zone,
                    client_name_prefix=os.environ.get("TEST_CSIPAUS_CLIENT_NAME_PREFIX", ""),
                    post_rate_seconds=post)
    token = os.environ.get("TEST_CSIPAUS_CONTROL_TOKEN") or None
    if token is None:
        logging.getLogger("test_csipaus").warning(
            "TEST_CSIPAUS_CONTROL_TOKEN is not set; the control API is open")
    port = int(os.environ.get("TEST_CSIPAUS_PORT") or os.environ.get("PORT") or 8080)
    web.run_app(build_app(server, token), host="0.0.0.0", port=port, access_log=None)


if __name__ == "__main__":
    main()
