# Build environment for producing Debian .deb packages.
#
# Targets Debian stable so the resulting .deb is installable on the
# Debian release most users actually run, and on Ubuntu LTS releases
# tracking the same library versions. Switch FROM to debian:sid for
# an archive-readiness build (current lintian, latest debhelper,
# Standards-Version baseline closest to ftpmaster review).

FROM debian:stable-slim

ENV DEBIAN_FRONTEND=noninteractive \
    LC_ALL=C.UTF-8 \
    LANG=C.UTF-8

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        debhelper-compat \
        devscripts \
        equivs \
        fakeroot \
        git \
        lintian \
        pkgconf \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /build
