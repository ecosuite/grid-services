"""One run's stack: the worker, its database, the site simulator and a
CACTUS test stack, as a compose project of its own (compose.yaml)."""

from __future__ import annotations

import os
import subprocess
from dataclasses import dataclass
from pathlib import Path

COMPOSE_FILE = Path(__file__).resolve().parent.parent / "compose.yaml"
DATABASE = "grid_services"


class StackError(RuntimeError):
    pass


@dataclass(frozen=True)
class StackSettings:
    worker_image: str
    pen: int
    cactus_image_tag: str | None = None


class Stack:
    def __init__(self, project: str, settings: StackSettings):
        self.project = project
        self.environment = {
            **os.environ,
            "GRID_SERVICES_IMAGE": settings.worker_image,
            "GRID_SERVICES_CSIPAUS_PEN": str(settings.pen),
            **({"CACTUS_IMAGE_TAG": settings.cactus_image_tag}
               if settings.cactus_image_tag else {}),
        }

    def compose(self, *arguments: str) -> str:
        result = subprocess.run(
            ["docker", "compose", "--file", str(COMPOSE_FILE), "--project-name", self.project,
             *arguments],
            env=self.environment, capture_output=True, text=True)
        if result.returncode != 0:
            raise StackError(f"docker compose {' '.join(arguments)} failed: "
                             f"{result.stderr.strip()[-2000:]}")
        return result.stdout

    def up(self) -> None:
        self.compose("up", "--detach", "--wait", "--quiet-pull")

    def address(self, service: str, port: int) -> str:
        """Where the service's port is published on this host, as host:port."""
        return self.compose("port", service, str(port)).strip()

    def database_url(self) -> str:
        return f"postgresql://{DATABASE}:{DATABASE}@{self.address('db', 5432)}/{DATABASE}"

    def runner_url(self) -> str:
        return f"http://{self.address('cactus-runner', 8080)}"

    def site_sim_url(self) -> str:
        return f"http://{self.address('site-sim', 8091)}"

    def restart(self, service: str) -> None:
        """Restarts the service alone and at once, as a power cycle: no
        orderly shutdown, and nothing it depends on restarts."""
        self.compose("restart", "--no-deps", "--timeout", "0", service)

    def logs(self, service: str) -> str:
        return self.compose("logs", "--no-color", "--timestamps", service)

    def down(self) -> None:
        self.compose("down", "--volumes", "--remove-orphans", "--timeout", "5")
