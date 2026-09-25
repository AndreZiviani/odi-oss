#!/usr/bin/env bash
#
# Refuse a single quote inside a single-quoted inline shell block.
#
# Several scripts here run a build inside a container with
#
#     docker run ... bash -c <single-quoted block>
#
# and the whole block is ONE shell word. A single quote anywhere inside it --
# in code, or in an ordinary English contraction in a comment -- terminates
# that word, and everything after it runs in the outer shell instead. The
# result is usually still valid syntax, so `bash -n` and shellcheck both pass
# and the script simply does the wrong thing, quietly.
#
# This has now cost three separate debugging sessions: a comment reading "the
# vendor is own config", a comment about apostrophes that contained one, and
# an `echo` writing a header with single quotes around it.
#
# Detection is deliberately blunt: from a line that opens such a block to the
# line that closes it, no single quote is allowed at all.
set -eu

status=0
for f in "$@"; do
	awk -v file="$f" '
		/bash -c .$/ && /-c '"'"'$/ { inblock = 1; next }
		inblock && /^'"'"'$/         { inblock = 0; next }
		inblock && /'"'"'/ {
			printf "%s:%d: single quote inside an inline bash -c block\n", file, NR
			printf "    %s\n", $0
			bad = 1
		}
		END { exit bad ? 1 : 0 }
	' "$f" || status=1
done

if [ "$status" -ne 0 ]; then
	echo "Rephrase, or use double quotes. See the comment in $0." >&2
	exit 1
fi
echo "inline-quote check: clean"
