# Image assembly: squashfs-tools writes the rootfs, qemu-user-static runs the
# cross-built busybox just far enough to ask it which applets it has.
#
# Debian's squashfs-tools accepts `-comp lzma`, which is what this device's
# squashfs reads, even though its own --help does not list it.
FROM debian:bookworm-slim

RUN apt-get update -qq \
	&& apt-get install -y -qq --no-install-recommends \
		squashfs-tools \
		qemu-user-static \
		openssl \
		binutils \
		file \
		xz-utils \
	&& rm -rf /var/lib/apt/lists/*

WORKDIR /work
