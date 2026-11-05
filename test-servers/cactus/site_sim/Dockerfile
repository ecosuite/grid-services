# syntax=docker/dockerfile:1
#
# The site simulator for CACTUS runs. Built from test-servers/, so that it
# can take in test-servers/common; test-servers/cactus/compose.yaml builds
# it for each run's stack:
#
#   docker build -f test-servers/cactus/site_sim/Dockerfile test-servers

ARG PYTHON_VERSION=3.12
ARG UV_VERSION=0.8.3

FROM ghcr.io/astral-sh/uv:${UV_VERSION} AS uv

FROM python:${PYTHON_VERSION}-slim-bookworm

COPY --from=uv /uv /usr/local/bin/uv

ENV UV_COMPILE_BYTECODE=1 \
    UV_LINK_MODE=copy \
    UV_PYTHON_DOWNLOADS=never \
    UV_PROJECT_ENVIRONMENT=/opt/venv \
    PATH=/opt/venv/bin:$PATH \
    PYTHONUNBUFFERED=1

WORKDIR /app/cactus/site_sim
COPY common/ /app/common/
COPY cactus/site_sim/pyproject.toml cactus/site_sim/uv.lock ./
RUN uv sync --locked --no-install-project
COPY cactus/site_sim/site_sim/ site_sim/

RUN useradd --system --no-create-home site-sim
USER site-sim

EXPOSE 8090 8091
HEALTHCHECK --interval=5s --timeout=3s --retries=10 \
    CMD python -c "import os, urllib.request; urllib.request.urlopen(f'http://127.0.0.1:{os.environ.get(\"SITE_SIM_CONTROL_PORT\", 8091)}/health')"
CMD ["python", "-m", "site_sim"]
