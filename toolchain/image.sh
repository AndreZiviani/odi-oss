#!/usr/bin/env sh
#
# Print the toolchain image to build in, pulling it first if it is not here.
#
#     toolchain/image.sh oss         # uClibc-ng toolchain: kernel, busybox, dropbear, iproute2
#     toolchain/image.sh diag        # freestanding toolchain: everything in src/
#     toolchain/image.sh qemu-kernel # stock malta kernel for make test-qemu
#
# Both are pinned by digest in toolchain/images.env, the one place that says
# which toolchain this tree builds with. OSS_IMAGE and DIAG_IMAGE override
# the pins, for an image built locally from the odi-toolchain repository
# (toolchain/README.md has the recipe):
#
#     OSS_IMAGE=odi-toolchain-uclibc:local make packages
#
# A pinned reference is pulled on first use. An override that is not a
# registry reference must already exist locally; it is never pulled or
# built from here.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
# shellcheck source=images.env
. "$ROOT/toolchain/images.env"

case ${1:-} in
oss)  ref=${OSS_IMAGE:-$OSS_IMAGE_PINNED};   var=OSS_IMAGE;  local_tag=odi-toolchain-uclibc:local;       target=uclibc ;;
diag) ref=${DIAG_IMAGE:-$DIAG_IMAGE_PINNED}; var=DIAG_IMAGE; local_tag=odi-toolchain-freestanding:local; target=freestanding ;;
qemu-kernel) ref=${QEMU_KERNEL_IMAGE:-$QEMU_KERNEL_IMAGE_PINNED}; var=QEMU_KERNEL_IMAGE; local_tag=odi-toolchain-qemu-kernel-malta:local; target=qemu-kernel-malta ;;
*)    echo "usage: toolchain/image.sh oss|diag|qemu-kernel" >&2; exit 1 ;;
esac

if docker image inspect "$ref" >/dev/null 2>&1; then
	echo "$ref"
	exit 0
fi

fallback() {
	cat >&2 <<EOF
To build the image locally instead, from the odi-toolchain repository:

    git clone https://github.com/AndreZiviani/odi-toolchain
    make -C odi-toolchain $target     # tags $local_tag
    export $var=$local_tag
EOF
}

case $ref in
*/*@sha256:*|*/*:*) ;;
*)
	echo "toolchain/image.sh: no local image $ref ($var)" >&2
	fallback
	exit 1 ;;
esac

echo "pulling $ref" >&2
if ! docker pull -q "$ref" >&2; then
	cat >&2 <<EOF

toolchain/image.sh: could not pull $ref

If the package is still private, log in to ghcr.io first with a GitHub
token that has read:packages (once it is public no login is needed):

    echo "\$TOKEN" | docker login ghcr.io -u <github user> --password-stdin

EOF
	fallback
	exit 1
fi
echo "$ref"
