FROM ubuntu:22.04

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential gdb git curl ca-certificates python3 sudo \
        pkg-config libmicrohttpd-dev libcjson-dev \
        python3-flask python3-qrcode \
    && rm -rf /var/lib/apt/lists/*

RUN useradd -m -s /bin/bash jungle \
    && echo 'jungle ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/jungle \
    && chmod 0440 /etc/sudoers.d/jungle

USER jungle
