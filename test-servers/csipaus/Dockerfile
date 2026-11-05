# syntax=docker/dockerfile:1
#
# The CSIP-AUS test server, with its tests alongside it. Built from
# test-servers/, so that it can take in test-servers/common:
#
#   docker build -f test-servers/csipaus/Dockerfile test-servers
#   docker compose up test-csipaus                      the server on 127.0.0.1:8081
#   docker compose run --rm test-csipaus pytest         its tests

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

WORKDIR /app/csipaus
COPY common/ /app/common/
COPY csipaus/pyproject.toml csipaus/uv.lock ./
RUN uv sync --locked --no-install-project
COPY csipaus/test_csipaus/ test_csipaus/
COPY csipaus/tests/ tests/

RUN useradd --system --no-create-home test-csipaus
USER test-csipaus

EXPOSE 8080
HEALTHCHECK --interval=5s --timeout=3s --retries=10 \
    CMD python -c "import os, urllib.request; port = os.environ.get('TEST_CSIPAUS_PORT') or os.environ.get('PORT') or 8080; urllib.request.urlopen(f'http://127.0.0.1:{port}/healthz')"
CMD ["python", "-m", "test_csipaus"]
