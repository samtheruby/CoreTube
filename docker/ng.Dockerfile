# Build image for the 32-bit add-on: CoreELEC 21 (Amlogic-ng, arm).
#
# CoreELEC 21 builds on Ubuntu 20.04. This follows CoreELEC's own build
# container (CoreELEC/tools/docker/focal/Dockerfile), plus rdfind (asked for by
# its scripts/checkdeps), what Cobalt's depot_tools needs, and the development
# files CoreTube's host tests compile against.
FROM ubuntu:20.04

SHELL ["/bin/bash", "-o", "pipefail", "-c"]
ARG DEBIAN_FRONTEND=noninteractive

# CoreELEC refuses to build as root. Match the owner of the mounted folders.
ARG BUILD_UID=1000
ARG BUILD_GID=1000

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

# CoreELEC's build dependencies, as its focal container installs them, then
# CoreTube's: pkg-config and GStreamer/ALSA headers for the host tests, pytest,
# and 32-bit x86 runtime libraries for V8's build tools (torque and friends are
# 32-bit x86 programs when V8 targets 32-bit ARM).
RUN apt-get update && apt-get install -y --no-install-recommends \
      wget bash bc cpio gcc-10 sed patch patchutils tar bzip2 gzip xz-utils zstd perl gawk gperf zip \
      unzip diffutils lzop make file g++-10 xfonts-utils xsltproc default-jre-headless python3 \
      libc6-dev libncurses5-dev libjson-perl libxml-parser-perl libparse-yapp-perl \
      golang-1.22-go git openssh-client rdfind \
      pkg-config libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libasound2-dev python3-pytest \
      libc6-i386 lib32gcc-s1 \
 && ln -s /usr/lib/go-1.22 /usr/lib/go \
 && ln -s /usr/lib/go-1.22/bin/go /usr/bin/go \
 && ln -s /usr/lib/go-1.22/bin/gofmt /usr/bin/gofmt \
 && rm -rf /var/lib/apt/lists/*

# gcc 10 as the host compiler, as CoreELEC's focal container has it.
# build-essential brings gcc 9, whose packages own /usr/bin/gcc; diverting its
# links keeps them aside through later apt upgrades. Mounted repos belong to
# the host's user, not to the build user.
RUN git config --system --add safe.directory '*' \
 && for tool in gcc g++ cpp gcov; do \
      dpkg-divert --local --rename --divert "/usr/bin/${tool}.gcc-9" --add "/usr/bin/${tool}" \
      && ln -s "${tool}-10" "/usr/bin/${tool}"; \
    done \
 && gcc --version | grep -q ' 10\.' \
 && g++ --version | grep -q ' 10\.'

# -o allows a UID or GID the image already uses.
RUN groupadd -o -g "${BUILD_GID}" builder \
 && useradd -o -l -m -u "${BUILD_UID}" -g "${BUILD_GID}" -s /bin/bash builder \
 && adduser builder sudo \
 && echo '%sudo ALL=(ALL) NOPASSWD:ALL' >> /etc/sudoers

USER builder
WORKDIR /coretube
CMD ["bash"]
