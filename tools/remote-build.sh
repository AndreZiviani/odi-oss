#!/usr/bin/env bash
#
# Build odi-oss on a remote Linux host instead of locally.
#
# WHY: the Docker images here are amd64, and on macOS every one of them runs
# under emulation -- slow, especially the kernel and toolchain builds. A
# native x86_64 Linux host with Docker runs the same images at full speed.
#
# WHAT THIS DOES, in order:
#   1. rsync the current working tree (tracked AND untracked source, so
#      uncommitted changes build too) to $ODI_REMOTE:$ODI_REMOTE_DIR.
#   2. run the given command on the remote host, inside that directory.
#   3. rsync out/image/*.tar, out/image/root-password*.txt and any build
#      logs back to the LOCAL out/image (logs land under out/image/logs/).
#
# WHAT IT DOES NOT DO: install Docker or touch a stick. The remote host
# needs the same prerequisites make image-all does (docs/BUILDING.md). The
# kernel tree is gigabytes and does not change on every build, so step 1
# excludes it: the first build there fetches it (kernel/tree.sh), and later
# ones reuse it.
#
# GITHUB_TOKEN, when set here, is handed to the remote command on stdin
# (never on a command line, never written to a file) and exported there:
# make releases needs it for the private confd repository on a host without
# a logged-in gh, and while the toolchain images are private, a pull needs
# it too. So this script also logs the remote Docker in with it before the
# build (GITHUB_ACTOR, default from `gh api user`, names the account) and
# logs it back out after -- the token only ever reaches the docker
# credential store, for the duration of the build, and nothing here writes
# it to a file of its own.
#
#   GITHUB_TOKEN=$(gh auth token) ODI_REMOTE=user@host tools/remote-build.sh 'make image-all'
#
# USAGE:
#   ODI_REMOTE=user@host tools/remote-build.sh '<command to run remotely>'
#
#   ODI_REMOTE=user@host tools/remote-build.sh \
#     'VERSION=odi-oss-yymmdd-x FRESH=1 kernel/build.sh && VERSION=odi-oss-yymmdd-x make image'
#
# ODI_REMOTE is required and has no default -- this script names no host of
# its own. ODI_REMOTE_DIR defaults to /root/odi/odi-oss.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

: "${ODI_REMOTE:?set ODI_REMOTE=user@host (or a Host alias from ~/.ssh/config)}"
ODI_REMOTE_DIR=${ODI_REMOTE_DIR:-/root/odi/odi-oss}

[ $# -ge 1 ] || {
	echo "usage: ODI_REMOTE=user@host $0 '<command to run remotely>'" >&2
	exit 1
}

say() { printf '\n== %s\n' "$*"; }

# Excludes match kernel/build.sh own gitignore intent: build output, the
# download cache, and the kernel tree, which is copied once by hand (or by
# a prior run of this script), not on every build -- kernel/618/mainline
# alone is well over a gigabyte and does not change between ordinary builds.
# No trailing slash on the mainline exclude: with one it matches only a
# directory, and a remote tree that links kernel/618/mainline to a shared
# copy loses the link to --delete.
say "syncing the working tree to $ODI_REMOTE:$ODI_REMOTE_DIR"
rsync -a --delete \
	--exclude='/out/' --exclude='/build/' --exclude='/dl/' \
	--exclude='/kernel/618/mainline' \
	--exclude='__pycache__/' --exclude='/refs/' \
	-e ssh \
	"$ROOT/" "$ODI_REMOTE:$ODI_REMOTE_DIR/"

say "building on $ODI_REMOTE: $*"
# SC2029: deliberate -- $ODI_REMOTE_DIR and the command both expand locally,
# then the whole line runs as one remote shell command. The command is
# typically VAR=val step && VAR=val make target, which needs a real shell on
# the far end, not an argv array.
# shellcheck disable=SC2029
if [ -n "${GITHUB_TOKEN:-}" ]; then
	GITHUB_ACTOR=${GITHUB_ACTOR:-$(gh api user --jq .login 2>/dev/null || true)}
	printf '%s\n' "$GITHUB_TOKEN" |
		ssh "$ODI_REMOTE" "
			set -e
			read -r TOKEN
			LOGGED_IN=0
			if [ -n '$GITHUB_ACTOR' ] && printf '%s' \"\$TOKEN\" | docker login ghcr.io -u '$GITHUB_ACTOR' --password-stdin >/dev/null 2>&1; then
				LOGGED_IN=1
			fi
			export GITHUB_TOKEN=\"\$TOKEN\"
			cd $ODI_REMOTE_DIR
			set +e
			( $* )
			RC=\$?
			set -e
			[ \"\$LOGGED_IN\" = 1 ] && docker logout ghcr.io >/dev/null 2>&1
			exit \$RC
		"
else
	ssh "$ODI_REMOTE" "cd $ODI_REMOTE_DIR && $*"
fi

say "syncing results back to $ROOT/out/image"
mkdir -p "$ROOT/out/image" "$ROOT/out/image/logs"
rsync -a \
	--include='*.tar' --include='root-password*.txt' --include='*/' --exclude='*' \
	"$ODI_REMOTE:$ODI_REMOTE_DIR/out/image/" "$ROOT/out/image/"
rsync -a \
	--include='*/' --include='*.log' --exclude='*' \
	"$ODI_REMOTE:$ODI_REMOTE_DIR/build/" "$ROOT/out/image/logs/" 2>/dev/null || true

say "done"
ls -l "$ROOT"/out/image/*.tar 2>/dev/null || true
