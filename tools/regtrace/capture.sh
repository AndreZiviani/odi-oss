#!/bin/sh
# Append the current register-trace ring of a running stick to a host file,
# labelled, then clear the ring on the stick.
#
#     capture.sh <host> <root-password> <label> <out-file>
#
# The label is what the phase is (for example "omci provisioning" or
# "speed test"); decode.py --summary groups by it.
set -eu
[ $# -eq 4 ] || { echo "usage: capture.sh <host> <root-password> <label> <out-file>" >&2; exit 1; }
HOST=$1; PW=$2; LABEL=$3; OUT=$4
HELPER=$(cd "$(dirname "$0")" && pwd)/stick-cmd.exp
# Built with single quotes around the remote command and the label spliced
# in through a close-quote/open-quote pair, not nested backslash escapes:
# a cut -d" " inside a doubly-quoted remote command survives expect and ssh
# quoting only by accident, and on the first real capture it did not
# (cut received a mangled delimiter and errored). read splits on IFS with
# no delimiter argument to mangle, and the single-quoted remote command
# passes through expect and ssh unchanged.
expect "$HELPER" root "$PW" 'read u _ < /proc/uptime; echo "== $u '"$LABEL"'"; cat /proc/rtk_regtrace; echo clear > /proc/rtk_regtrace' 60 "$HOST" \
	| grep -v "^spawn\|assword:" | tr -d "\r" >> "$OUT"
echo "appended $(grep -c . "$OUT") lines total to $OUT"
