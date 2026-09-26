#!/usr/bin/env sh
#
# The image to run anything compiled with OUR toolchain in:
#
#     IMAGE=$(packages/oss-env.sh)
#
# The uClibc-ng toolchain (gcc 16.2.0, binutils 2.47, uClibc-ng 1.0.59) lives
# inside that image at /opt/oss, together with the host tools the kernel and
# package builds need. It is pinned by digest in toolchain/images.env and
# pulled on first use; toolchain/image.sh has the override (OSS_IMAGE) and the
# local-build fallback.
exec "$(cd "$(dirname "$0")/.." && pwd)/toolchain/image.sh" oss
