FROM ubuntu:22.04 AS toolchain

ARG TARGET=kindlehf
ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update -q && apt-get install -y -q \
    build-essential autoconf automake bison flex gawk \
    libtool libtool-bin libncurses-dev \
    curl file git gperf help2man texinfo unzip wget \
    libarchive-dev nettle-dev \
    meson libgtk2.0-dev libxrandr-dev \
    pkg-config python3 sudo \
    && rm -rf /var/lib/apt/lists/*

RUN useradd -m builder && echo "builder ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers

USER builder
WORKDIR /home/builder

RUN git clone --recursive --depth=1 https://github.com/koreader/koxtoolchain.git /home/builder/koxtoolchain \
    && cd /home/builder/koxtoolchain \
    && chmod +x ./gen-tc.sh \
    && ./gen-tc.sh ${TARGET}

RUN git clone --recursive --depth=1 https://github.com/KindleModding/kindle-sdk.git /home/builder/kindle-sdk

FROM toolchain AS sdk

ARG TARGET=kindlehf

ENV MESON_CROSS=/home/builder/x-tools/arm-${TARGET}-linux-gnueabihf/meson-crosscompile.txt
ENV PATH="/home/builder/x-tools/arm-${TARGET}-linux-gnueabihf/bin:${PATH}"

USER builder
WORKDIR /home/builder/kindle-sdk

RUN chmod +x ./gen-sdk.sh \
    && ./gen-sdk.sh ${TARGET}

RUN SYSROOT="/home/builder/x-tools/arm-${TARGET}-linux-gnueabihf/arm-${TARGET}-linux-gnueabihf/sysroot" && \
    sudo apt-get update -q && sudo apt-get install -y -q libxrandr-dev 2>/dev/null || true && \
    mkdir -p "$SYSROOT/usr/include/X11/extensions" && \
    cp /usr/include/X11/extensions/Xrandr.h  "$SYSROOT/usr/include/X11/extensions/" && \
    cp /usr/include/X11/extensions/randr.h   "$SYSROOT/usr/include/X11/extensions/"

WORKDIR /src
