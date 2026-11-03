# Build image for the 64-bit add-on: CoreELEC 22 (Amlogic-no, aarch64).
#
# CoreELEC 22 builds on Ubuntu 22.04 or newer. This follows CoreELEC's own build
# container (CoreELEC/tools/docker/noble/Dockerfile), plus what Cobalt's
# depot_tools needs and the development files CoreTube's host tests compile
# against.
FROM ubuntu:24.04

SHELL ["/bin/bash", "-o", "pipefail", "-c"]
ARG DEBIAN_FRONTEND=noninteractive

# CoreELEC refuses to build as root. Match the owner of the mounted folders.
ARG BUILD_UID=1000
ARG BUILD_GID=1000

# curl is required: without it a CoreELEC 22 build boots broken.
RUN apt-get update \
 && apt-get upgrade -y \
 && apt-get install -y --no-install-recommends \
      build-essential git python3 bash ca-certificates wget curl locales sudo \
 && rm -rf /var/lib/apt/lists/* \
 && locale-gen en_US.UTF-8 \
 && update-locale LANG=en_US.UTF-8 LANGUAGE=en_US:en
ENV LANG=en_US.UTF-8 \
    LANGUAGE=en_US:en \
    LC_ALL=en_US.UTF-8

# CoreELEC 22's build dependencies, as its noble container installs them, then
# CoreTube's: pkg-config and GStreamer/ALSA headers for the host tests, pytest.
RUN apt-get update && apt-get install -y --no-install-recommends \
      bash bc gcc-14 cpp-14 sed patch patchutils tar bzip2 gzip xz-utils zstd perl gawk gperf zip \
      unzip diffutils lzop make file g++-14 xfonts-utils xsltproc default-jre-headless python3 \
      libc6-dev libncurses5-dev libjson-perl libxml-parser-perl libparse-yapp-perl rdfind \
      golang-1.23-go git openssh-client rsync upx-ucl nodejs npm xxd cpio \
      pkg-config libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libasound2-dev python3-pytest \
 && ln -s /usr/lib/go-1.23 /usr/lib/go \
 && ln -s /usr/lib/go-1.23/bin/go /usr/bin/go \
 && ln -s /usr/lib/go-1.23/bin/gofmt /usr/bin/gofmt \
 && rm -rf /var/lib/apt/lists/*

# gcc 14 as the host compiler, as CoreELEC's noble container has it.
# build-essential brings gcc 13, whose packages own /usr/bin/gcc; diverting its
# links keeps them aside through later apt upgrades. Mounted repos belong to
# the host's user, not to the build user.
RUN git config --system --add safe.directory '*' \
 && for tool in gcc g++ cpp gcov; do \
      dpkg-divert --local --rename --divert "/usr/bin/${tool}.gcc-13" --add "/usr/bin/${tool}" \
      && ln -s "${tool}-14" "/usr/bin/${tool}"; \
    done \
 && gcc --version | grep -q ' 14\.' \
 && g++ --version | grep -q ' 14\.'

# Ubuntu 24.04's own user "ubuntu" takes UID 1000, so it goes first.
RUN userdel -r ubuntu \
 && groupadd -o -g "${BUILD_GID}" builder \
 && useradd -o -l -m -u "${BUILD_UID}" -g "${BUILD_GID}" -s /bin/bash builder \
 && adduser builder sudo \
 && echo '%sudo ALL=(ALL) NOPASSWD:ALL' >> /etc/sudoers

USER builder
WORKDIR /coretube
CMD ["bash"]
