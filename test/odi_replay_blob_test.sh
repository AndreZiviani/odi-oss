#!/usr/bin/env bash
# odi_replay_blob_test.sh -- compiles and runs odi_replay_blob_test.c (the
# blob parser and the GPON init replay, unity build against the mock, no
# kernel), then has tools/regtrace/replayblob.py validate and dump each
# table the image ships, so the Python writer and the C reader are checked
# against the same bytes. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FW="$ROOT/rootfs/skeleton/lib/firmware/odi"
BIN=$(mktemp -t odi_replay_blob_test.XXXXXX)
trap 'rm -f "$BIN"' EXIT

cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" "$ROOT/test/odi_replay_blob_test.c"
"$BIN"

for spec in sdkinit:7429 modload:3619 gpon_init:2528; do
	name=${spec%%:*}
	want=${spec#*:}
	head=$(python3 "$ROOT/tools/regtrace/replayblob.py" dump "$FW/$name.bin" | sed -n 1p)
	case "$head" in
	"# odi replay blob: table=$name version=1 records=$want "*) ;;
	*) echo "odi_replay_blob_test: $name.bin: unexpected header: $head" >&2; exit 1 ;;
	esac
done
echo "odi_replay_blob_test: replayblob.py agrees on all three tables"
