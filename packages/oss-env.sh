#!/usr/bin/env sh
#
# The build environment for anything compiled with OUR toolchain, and the tag
# of the image to run it in.
#
#     IMAGE=$(packages/oss-env.sh)
#
# Two things have to exist before an OSS-toolchain build can run:
#
#   the volume  odi-oss-toolchain-318      gcc 16.2.0, binutils 2.47 and
#                                          uClibc-ng 1.0.59, built by
#                                          toolchain/build-oss-toolchain.sh
#   the image   odi-oss-toolchain-builder  the same container that built it
#
# The image is built once and cached by docker; the volume cannot be rebuilt
# from here, so a missing one is an error with the command that makes it.
#
# The image is native to the host architecture (the vendor prebuilt toolchain this project
# started with was 32-bit x86 and forced an emulated linux/amd64 container on
# every build; it is gone).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)

OSS_VOLUME=${OSS_VOLUME:-odi-oss-toolchain-318}
OSS_IMAGE=${OSS_IMAGE:-odi-oss-toolchain-builder}

docker volume inspect "$OSS_VOLUME" >/dev/null 2>&1 || {
	echo "no toolchain volume $OSS_VOLUME" >&2
	echo "run toolchain/build-oss-toolchain.sh first -- it makes one" >&2
	exit 1
}

if ! docker image inspect "$OSS_IMAGE" >/dev/null 2>&1; then
	echo "building $OSS_IMAGE (once)" >&2
	docker build -q -t "$OSS_IMAGE" \
		-f "$ROOT/toolchain/Dockerfile.oss" "$ROOT/toolchain" >&2
fi

echo "$OSS_IMAGE"
