# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# Image assembly: squashfs-tools writes the rootfs, qemu-user-static runs the
# cross-built busybox just far enough to ask it which applets it has.
#
# Debian's squashfs-tools accepts `-comp lzma`, which is what this device's
# squashfs reads, even though its own --help does not list it.
#
# Reproducible: the base is pinned by digest and every package comes from a
# dated snapshot.debian.org archive (the same date the odi-toolchain images
# use), so the tools -- mksquashfs above all, whose output depends on its
# version -- are the same bytes whenever this image is built. Bump the digest
# and the date together; the base must not be newer than the snapshot.
# Check-Valid-Until is off because a snapshot Release file expires by design.
FROM debian:bookworm-slim@sha256:3783cc01769c7b2b1b83a5c5ad96c815348e28ed7da68e2e3687004faa906251

RUN rm -f /etc/apt/sources.list /etc/apt/sources.list.d/* \
	&& printf '%s\n' \
		'Types: deb' \
		'URIs: http://snapshot.debian.org/archive/debian/20260925T000000Z' \
		'Suites: bookworm bookworm-updates' \
		'Components: main' \
		'Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg' \
		'' \
		'Types: deb' \
		'URIs: http://snapshot.debian.org/archive/debian-security/20260925T000000Z' \
		'Suites: bookworm-security' \
		'Components: main' \
		'Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg' \
		> /etc/apt/sources.list.d/debian.sources \
	&& printf '%s\n' \
		'Acquire::Check-Valid-Until "false";' \
		'Acquire::Retries "5";' \
		'Acquire::http::Timeout "30";' \
		> /etc/apt/apt.conf.d/99snapshot \
	&& apt-get update -qq \
	&& apt-get install -y -qq --no-install-recommends \
		squashfs-tools \
		qemu-user-static \
		openssl \
		binutils \
		file \
		xz-utils \
	&& rm -rf /var/lib/apt/lists/*

WORKDIR /work
