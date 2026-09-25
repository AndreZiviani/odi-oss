#!/bin/sh
# Push a replay script (tools/regtrace/sequence.py) to a running stick and
# execute it through our own freestanding diag. Pushes the CURRENT
# rootfs/skeleton/etc/scripts/regreplay to /tmp and runs that copy, rather
# than trusting whatever regreplay is baked into the running image: a stick
# flashed before a sequence.py/regreplay format change (the t-line column
# layout, for one) would otherwise silently misparse table-entry lines.
#
#     replay.sh <host> <root-password> <script>
set -eu
[ $# -eq 3 ] || { echo "usage: replay.sh <host> <root-password> <script>" >&2; exit 1; }
HOST=$1; PW=$2; SCRIPT=$3
HERE=$(cd "$(dirname "$0")" && pwd)
PUSH="$HERE/stick-push.exp"
CMD="$HERE/stick-cmd.exp"
REGREPLAY="$HERE/../../rootfs/skeleton/etc/scripts/regreplay"
expect "$PUSH" root "$PW" "$REGREPLAY" /tmp/regreplay.sh "$HOST"
expect "$PUSH" root "$PW" "$SCRIPT" /tmp/regreplay.txt "$HOST"
expect "$CMD" root "$PW" 'chmod +x /tmp/regreplay.sh; /tmp/regreplay.sh /tmp/regreplay.txt' 130 "$HOST" \
	| grep -v "^spawn\|assword:" | tr -d "\r" | grep "^regreplay:"
