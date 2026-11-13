# SPDX-License-Identifier: Apache-2.0
#
# Build container for ps-msgr: every build (host and armhf) runs in here, in
# CI and locally. Debian trixie is the only target OS, so building on trixie
# guarantees binaries match the board's glibc.
#
#   docker build -t psmsgr-build -f docker/build.Dockerfile docker
#   docker/run.sh cmake --workflow --preset dev

# ruff's x86-64 and arm64 wheels, fetched and checked by BuildKit.
FROM scratch AS ruff-wheels
ADD --checksum=sha256:15e7d226246961db9235098333caa13063906d3851136b84c2900b82f5daa1df \
    https://files.pythonhosted.org/packages/1a/41/d83af9879a7b6e8bf5fe16b1da0b134049d2f5d3afac12defb0897cb84bd/ruff-0.16.8-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl /
ADD --checksum=sha256:8efeae3bbe414a5efefda11a792dfb51ef90ac48d50c4830de2f644caf3e8659 \
    https://files.pythonhosted.org/packages/23/f2/311a08776d75d81c7676e20b6b020ae63cbe881fcdc7a8dd64e6e18bdd93/ruff-0.16.8-py3-none-manylinux_2_17_aarch64.manylinux2014_aarch64.whl /

FROM debian:trixie

ARG DEBIAN_FRONTEND=noninteractive

# libc6:armhf (multiarch) lets dpkg-shlibdeps resolve armhf dependencies for
# CPack; the cross toolchain itself brings /usr/arm-linux-gnueabihf for qemu.
RUN dpkg --add-architecture armhf \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential \
        crossbuild-essential-armhf \
        libc6:armhf \
        clang \
        clang-format \
        libclang-rt-dev \
        cmake \
        ninja-build \
        qemu-user \
        gdb-multiarch \
        file \
        git \
        golang-go \
        ca-certificates \
        python3 \
        python3-pip \
        python3-pytest \
        python3-setuptools \
        python3-venv \
        zlib1g-dev \
        abigail-tools \
 && rm -rf /var/lib/apt/lists/*

# The .NET SDK (current LTS) for the C# binding, from Microsoft's apt
# repository: trixie does not package it. BuildKit checks the repository's
# signing key, and apt checks the packages against it. The runtime packages
# are pinned too, so that the SDK's ">=" dependencies cannot float.
# zlib1g-dev (above) is what Native AOT links against, with clang.
ADD --checksum=sha256:d45224d594d969f084232deaaf97c58ca502a9d964c362d7aaef5a76e16b3dd1 \
    https://packages.microsoft.com/keys/microsoft-2025.asc /usr/share/keyrings/microsoft-2025.asc
ARG DOTNET_SDK_VERSION=10.0.404-1
ARG DOTNET_RUNTIME_VERSION=10.0.15-1
RUN chmod 644 /usr/share/keyrings/microsoft-2025.asc \
 && echo "deb [signed-by=/usr/share/keyrings/microsoft-2025.asc] https://packages.microsoft.com/debian/13/prod trixie main" \
        > /etc/apt/sources.list.d/microsoft-prod.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
        dotnet-sdk-10.0=${DOTNET_SDK_VERSION} \
        dotnet-runtime-10.0=${DOTNET_RUNTIME_VERSION} \
        dotnet-hostfxr-10.0=${DOTNET_RUNTIME_VERSION} \
        dotnet-runtime-deps-10.0=${DOTNET_RUNTIME_VERSION} \
        dotnet-host=${DOTNET_RUNTIME_VERSION} \
        dotnet-targeting-pack-10.0=${DOTNET_RUNTIME_VERSION} \
        dotnet-apphost-pack-10.0=${DOTNET_RUNTIME_VERSION} \
        aspnetcore-runtime-10.0=${DOTNET_RUNTIME_VERSION} \
        aspnetcore-targeting-pack-10.0=${DOTNET_RUNTIME_VERSION} \
 && rm -rf /var/lib/apt/lists/*
ENV DOTNET_CLI_TELEMETRY_OPTOUT=1 \
    DOTNET_NOLOGO=1 \
    DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 \
    DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE=1

# ruff for the Python binding (trixie does not package it): the pinned
# wheels from the ruff-wheels stage, installed into a venv without network.
RUN --mount=type=bind,from=ruff-wheels,target=/wheels \
    python3 -m venv /opt/ruff \
 && /opt/ruff/bin/pip install --no-cache-dir --no-index --find-links /wheels ruff==0.16.8 \
 && ln -s /opt/ruff/bin/ruff /usr/local/bin/ruff

# cmocka 2.0 for the C tests: trixie ships 1.1.7, which predates the current
# API. Built as a static library for the host (/usr/local) and for armhf (the
# cross root, which the toolchain file searches), so test binaries need no
# cmocka on the board. BuildKit fetches the release and checks its SHA-256.
ARG CMOCKA_VERSION=2.0.2
ADD --checksum=sha256:39f92f366bdf3f1a02af4da75b4a5c52df6c9f7e736c7d65de13283f9f0ef416 \
    https://cmocka.org/files/2.0/cmocka-${CMOCKA_VERSION}.tar.xz /tmp/
RUN set -eu; cd /tmp; \
    tar xf cmocka-${CMOCKA_VERSION}.tar.xz; \
    common="-G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DWITH_EXAMPLES=OFF"; \
    cmake -S cmocka-${CMOCKA_VERSION} -B build-host $common \
          -DCMAKE_INSTALL_PREFIX=/usr/local; \
    cmake -S cmocka-${CMOCKA_VERSION} -B build-armhf $common \
          -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm \
          -DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc \
          -DCMAKE_INSTALL_PREFIX=/usr/arm-linux-gnueabihf; \
    cmake --build build-host --target install; \
    cmake --build build-armhf --target install; \
    rm -rf /tmp/*

WORKDIR /src
