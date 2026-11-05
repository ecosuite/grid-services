# syntax=docker/dockerfile:1
#
# The test VTN, with the end-to-end suite alongside it so that the suite can
# run anywhere the VTN is deployed. Built from test-servers/, so that it can
# take in test-servers/common:
#
#   docker build -f test-servers/openadr2b/Dockerfile test-servers
#   docker compose up test-vtn                  the VTN on 127.0.0.1:8080
#   docker compose --profile e2e run --rm e2e   the suite

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

WORKDIR /app/openadr2b
COPY common/ /app/common/
COPY openadr2b/pyproject.toml openadr2b/uv.lock ./
RUN uv sync --locked --no-install-project
COPY openadr2b/test_vtn/ test_vtn/
COPY openadr2b/e2e/ e2e/

RUN useradd --system --no-create-home test-vtn
USER test-vtn

EXPOSE 8080
HEALTHCHECK --interval=5s --timeout=3s --retries=10 \
    CMD python -c "import os, urllib.request; port = os.environ.get('TEST_VTN_PORT') or os.environ.get('PORT') or 8080; urllib.request.urlopen(f'http://127.0.0.1:{port}/healthz')"
CMD ["python", "-m", "test_vtn"]
