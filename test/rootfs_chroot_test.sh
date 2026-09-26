#!/usr/bin/env bash
#
# Run the image's own boot scripts, on MIPS, without a device.
#
# qemu-user-static and binfmt give a chroot into the staged rootfs where every
# binary is the real big-endian MIPS one -- our busybox, our dropbear, our
# diag. So `/etc/init.d/rcS` runs as written, with the applets the image
# actually has, under the shell the image actually ships.
#
# WHAT THIS CANNOT SEE, and why the QEMU system boot exists as well:
#
#   - `init` and /etc/inittab. PID 1 here is the test harness.
#   - what /dev really holds: on the device it is devtmpfs, mounted by the
#     kernel; here the harness makes the few nodes it needs.
#   - mounts. proc, sysfs and ramfs are the host's; the squashfs is a
#     directory. Read-only-ness of / has to be simulated, not inherited.
#   - ptys, the network, and anything that needs a driver.
#
# What it DOES see is every applet the scripts call, every ash-vs-bash
# difference, every `set -e` abort, and the exit status of each service start
# -- which is where most of the first review's findings lived.
set -u
cd "$(dirname "$0")/.." || exit 1
STAGE=${STAGE:-build/rootfs}

[ -d "$STAGE" ] || { echo "no staged rootfs at $STAGE -- run make image" >&2; exit 1; }
# The freestanding toolchain image (toolchain/images.env), for its
# qemu-user-static; the amd64 variant, as this harness always ran.
IMAGE=$(toolchain/image.sh diag) || exit 1

# --- the script, run inside the container against a COPY of the stage -------
# NET_ADMIN so that `ifconfig lo 127.0.0.1 up` in rcS can actually succeed. A
# container has its own netns, so this touches nothing outside it -- and
# without it rcS aborts on that line under `set -e` and every line after it
# goes untested, which is a harness artefact and not a finding.
#
# rootfs/ is mounted as well as the stage: device nodes are applied by
# mksquashfs from rootfs/devices.pseudo at pack time and so are NOT in the
# staged tree. It is the only honest source for what /dev will hold at boot.
docker run --rm --platform linux/amd64 --cap-add=NET_ADMIN \
	-v "$PWD/$STAGE:/stage:ro" -v "$PWD/rootfs:/imagesrc:ro" \
	-v "$PWD/test:/t:ro" "$IMAGE" bash /t/rootfs_chroot_inner.sh
