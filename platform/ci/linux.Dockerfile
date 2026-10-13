# syntax=docker/dockerfile:1
# Build each Debian package in its target architecture's Debian 12 userspace.
# Docker's binfmt/QEMU support is needed for the ARM and i386 matrix entries.
FROM debian:12-slim AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN --mount=type=secret,id=proxy_ca,mode=0444 \
    if [ -s /run/secrets/proxy_ca ]; then \
        printf 'Acquire::https::CaInfo "/run/secrets/proxy_ca";\n' > /etc/apt/apt.conf.d/99build-ca; \
        sed -i 's|http://|https://|g' /etc/apt/sources.list.d/debian.sources; \
    fi && apt-get update && apt-get install -y --no-install-recommends \
    build-essential ca-certificates curl dpkg-dev file git nasm patchelf \
    pkg-config python3 python3-pil qemu-user \
    libasound2-dev libflac-dev libgtk2.0-dev libsdl2-dev libvpx-dev \
    libvulkan-dev libgl1 && rm -rf /var/lib/apt/lists/* /etc/apt/apt.conf.d/99build-ca
WORKDIR /src
COPY Common.mak GNUmakefile README.md AUTHORS.md nblood.pk3 ./
COPY source/ source/
COPY package/ package/
COPY platform/ platform/
ARG PACKAGE_KIND=deb
ARG CPU_BASELINE=standard
ARG BUILD_JOBS=2
ARG NETBLOOD_REVISION
ARG SOURCE_DATE_EPOCH
ENV NETBLOOD_REVISION=${NETBLOOD_REVISION} SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH}
RUN --mount=type=secret,id=proxy_ca,mode=0444 \
    if [ -s /run/secrets/proxy_ca ]; then export CURL_CA_BUNDLE=/run/secrets/proxy_ca; fi; \
    platform/ci/linux-build.sh "${PACKAGE_KIND}" "${CPU_BASELINE}" /out "${BUILD_JOBS}"

FROM scratch AS artifact
COPY --from=build /out/ /
