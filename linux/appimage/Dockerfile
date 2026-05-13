# PoxChat AppImage build environment.
#
# Ubuntu 24.04 is the minimum supported base: meson.build requires GTK >= 4.14
# and GLib >= 2.80, which neither Ubuntu 22.04 nor Debian 12 provide. The
# resulting AppImage therefore runs on glibc 2.39+ systems (Ubuntu 24.04 /
# Debian 13 / Fedora 41 / Mint 22 and newer).

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Build deps (mirrors readme.md, minus libperl-dev — Perl plugin is disabled
# for the AppImage build since bundling a relocatable Perl is impractical).
# Python plugin links against system python3 at runtime, so libpython3-dev is
# needed at build time but the interpreter is NOT shipped inside the AppImage.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential meson ninja-build gettext pkg-config \
        libgtk-4-dev libssl-dev libdbus-glib-1-dev libcanberra-dev libnotify-dev \
        libluajit-5.1-dev libpython3-dev libpci-dev libx11-dev \
        libsqlite3-dev libjansson-dev libsoup-3.0-dev libwebsockets-dev \
        libsecret-1-dev \
        python3-cffi desktop-file-utils appstream-util \
        wget ca-certificates file patchelf zsync \
        librsvg2-common libgdk-pixbuf2.0-bin \
    && rm -rf /var/lib/apt/lists/*

# linuxdeploy, linuxdeploy-plugin-gtk, appimagetool.
# Extracted from their AppImage form because FUSE isn't available inside
# containers — running them directly would error with "fusermount: failed".
WORKDIR /opt

RUN wget -q https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage \
    && chmod +x linuxdeploy-x86_64.AppImage \
    && ./linuxdeploy-x86_64.AppImage --appimage-extract > /dev/null \
    && mv squashfs-root linuxdeploy \
    && rm linuxdeploy-x86_64.AppImage \
    && ln -s /opt/linuxdeploy/AppRun /usr/local/bin/linuxdeploy

RUN wget -q https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh \
    && chmod +x linuxdeploy-plugin-gtk.sh \
    && mv linuxdeploy-plugin-gtk.sh /usr/local/bin/linuxdeploy-plugin-gtk

RUN wget -q https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage \
    && chmod +x appimagetool-x86_64.AppImage \
    && ./appimagetool-x86_64.AppImage --appimage-extract > /dev/null \
    && mv squashfs-root appimagetool \
    && rm appimagetool-x86_64.AppImage \
    && ln -s /opt/appimagetool/AppRun /usr/local/bin/appimagetool

# The repo is mounted here by build-in-docker.sh.
WORKDIR /src
