# syntax=docker/dockerfile:1
#
# Build, test and runtime images for the grid-services worker.
#
# Stages:
#   deps     - toolchain and libraries, all from Ubuntu packages
#   codegen  - deps plus the XSD compiler, for regenerating vendored
#              bindings; nothing else builds on it
#   build    - compiles the worker and the test suites
#   test     - runs the test suites as its command
#              `docker compose run --rm test`
#   runtime  - the worker binary, its shared libraries, dbmate and the
#              migrations. The default command runs the worker; deploys run
#              `dbmate ... up` in the same image first.

ARG UBUNTU_VERSION=22.04
ARG DBMATE_VERSION=2.36.0
ARG DBMATE_SHA256=47e284b3d8cbad1ba5f090495aa05afd1bbd5f35e2ed5577aad06da74ce780ce

FROM ubuntu:${UBUNTU_VERSION} AS deps

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config ca-certificates curl tzdata \
        libcurl4-openssl-dev libxerces-c-dev \
        libpqxx-dev libpq-dev \
        libspdlog-dev libfmt-dev nlohmann-json3-dev \
        libsodium-dev \
        libgtest-dev \
    && rm -rf /var/lib/apt/lists/*

# ---------------------------------------------------------------------------
FROM deps AS codegen

RUN apt-get update && apt-get install -y --no-install-recommends \
        xsdcxx python3 \
    && rm -rf /var/lib/apt/lists/*

# ---------------------------------------------------------------------------
FROM deps AS build

WORKDIR /src/grid-services
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGRID_SERVICES_WERROR=ON \
    && cmake --build build -j"$(nproc)"

ARG DBMATE_VERSION
ARG DBMATE_SHA256
RUN mkdir -p /out/bin /out/lib \
    && cp build/grid-services /out/bin/ \
    && cp -a build/liboadr.so* build/liboadrsd.so* /out/lib/ \
    && curl -fsSL -o /out/bin/dbmate \
        "https://github.com/amacneil/dbmate/releases/download/v${DBMATE_VERSION}/dbmate-linux-amd64" \
    && echo "${DBMATE_SHA256}  /out/bin/dbmate" | sha256sum -c - \
    && chmod +x /out/bin/dbmate

# ---------------------------------------------------------------------------
FROM build AS test

WORKDIR /src/grid-services/build
CMD ["ctest", "--output-on-failure"]

# ---------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION} AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates tzdata \
        libcurl4 libxerces-c3.2 libpqxx-6.4 libpq5 \
        libspdlog1 libfmt8 libsodium23 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --no-create-home grid-services

COPY --from=build /out/lib/ /usr/local/lib/
COPY --from=build /out/bin/ /usr/local/bin/
COPY db/migrations/ /app/db/migrations/

# Fail the image build if any shared library is missing.
RUN ldconfig && ! ldd /usr/local/bin/grid-services | grep 'not found'

USER grid-services
WORKDIR /app
CMD ["grid-services"]
