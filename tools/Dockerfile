# Cross-compile image for Main_MiSTer (arm-none-linux-gnueabihf, gcc 10.2)
# Matches the toolchain that setup_default_toolchain.sh downloads upstream.
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        wget xz-utils make rsync ca-certificates \
    && rm -rf /var/lib/apt/lists/*

ARG GCC_VER=10.2-2020.11
RUN wget -q --no-check-certificate \
        https://developer.arm.com/-/media/Files/downloads/gnu-a/${GCC_VER}/binrel/gcc-arm-${GCC_VER}-x86_64-arm-none-linux-gnueabihf.tar.xz \
        -O /tmp/gcc.tar.xz \
    && mkdir -p /opt/toolchain \
    && tar xf /tmp/gcc.tar.xz -C /opt/toolchain --strip-components=1 \
    && rm /tmp/gcc.tar.xz

ENV PATH="/opt/toolchain/bin:${PATH}"
WORKDIR /work
