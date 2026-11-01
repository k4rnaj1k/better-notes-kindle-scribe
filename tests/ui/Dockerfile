FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -q && apt-get install -y -q g++ meson ninja-build pkg-config libgtk2.0-dev xvfb xdotool imagemagick python3 fonts-dejavu-core >/dev/null && rm -rf /var/lib/apt/lists/*
