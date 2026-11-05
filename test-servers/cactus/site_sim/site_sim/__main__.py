"""Runs the site simulator: the stand-in SolarNetwork the worker reads, and
the control API the test drives it with, each on a port of its own.

Configured through the environment:

    SITE_SIM_DATABASE_URL          the grid-services database, as the
                                   ecosuite_api role
    SITE_SIM_WORKER_PUBLIC_KEY     the worker's public key, to seal secrets
    SITE_SIM_CERTIFICATES          directory with device.pem, device.key and
                                   proxy-ca.pem (default /certs)
    SITE_SIM_SOLARNETWORK_PORT     SolarNetwork's port (default 8090)
    SITE_SIM_SOLARNETWORK_HOST     SolarNetwork as the worker reaches it
                                   (default http://site-sim:<port>)
    SITE_SIM_CONTROL_PORT          the control API's port (default 8091)
    SITE_SIM_LOG_LEVEL             debug, info, warning or error (default info)
"""

import asyncio
import logging
import os
import signal
from pathlib import Path

from aiohttp import web

from site_sim.provisioner import Provisioner
from site_sim.simulator import Simulator, control_errors


def required(name: str) -> str:
    value = os.environ.get(name)
    if not value:
        raise SystemExit(f"{name} is not set")
    return value


async def serve(simulator: Simulator, solarnetwork_port: int, control_port: int) -> None:
    solarnetwork = web.Application()
    solarnetwork.add_routes(simulator.solarnetwork_routes())
    control = web.Application(middlewares=[control_errors])
    control.add_routes(simulator.control_routes())
    runners = [web.AppRunner(solarnetwork, access_log=None),
               web.AppRunner(control, access_log=None)]
    try:
        for runner, port in zip(runners, (solarnetwork_port, control_port)):
            await runner.setup()
            await web.TCPSite(runner, "0.0.0.0", port).start()
        logging.getLogger("site_sim").info(
            "SolarNetwork on port %s, control API on port %s", solarnetwork_port, control_port)
        stop = asyncio.Event()
        for number in (signal.SIGTERM, signal.SIGINT):
            asyncio.get_running_loop().add_signal_handler(number, stop.set)
        await stop.wait()
    finally:
        await simulator.close()
        for runner in runners:
            await runner.cleanup()


def main() -> None:
    level = os.environ.get("SITE_SIM_LOG_LEVEL", "info").upper()
    logging.basicConfig(level=level, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    solarnetwork_port = int(os.environ.get("SITE_SIM_SOLARNETWORK_PORT", 8090))
    control_port = int(os.environ.get("SITE_SIM_CONTROL_PORT", 8091))
    provisioner = Provisioner(
        database_url=required("SITE_SIM_DATABASE_URL"),
        worker_public_key=required("SITE_SIM_WORKER_PUBLIC_KEY"),
        certificates=Path(os.environ.get("SITE_SIM_CERTIFICATES", "/certs")),
        solarnetwork_host=os.environ.get("SITE_SIM_SOLARNETWORK_HOST")
        or f"http://site-sim:{solarnetwork_port}")
    asyncio.run(serve(Simulator(provisioner), solarnetwork_port, control_port))


if __name__ == "__main__":
    main()
