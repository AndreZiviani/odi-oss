#!/usr/bin/env bash
# odi_switch_acl_clean_test.sh -- compiles and runs odi_switch_acl_clean_test.c
# with the host cc, then checks the ACL clean log lines it printed: the
# rows found before the clean (two valid rules, the first rule 1, four rows
# in use), the clean after one pass, a clean table found clean, and an
# engine that never finishes reported as not clean. Part of `make test-host`.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$(mktemp -t odi_switch_acl_clean_test.XXXXXX)
OUT=$(mktemp -t odi_switch_acl_clean_test_out.XXXXXX)
trap 'rm -f "$BIN" "$OUT"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I "$ROOT/test" -o "$BIN" \
	"$ROOT/test/odi_switch_acl_clean_test.c"
rc=0
"$BIN" > "$OUT" || rc=$?
grep -v '^odi_switch: platform' "$OUT"

fail=$rc
for want in \
	'odi_switch: ACL rows before the clean: 2/96 rules valid (first 1), 4/96 rows not empty' \
	'odi_switch: ACL rows clean after 1 pass(es): 0/96 rules valid, 0/96 rows not empty' \
	'odi_switch: ACL rows before the clean: 0/96 rules valid (first -1), 0/96 rows not empty' \
	'odi_switch: ACL rows NOT clean after 2 passes'; do
	if ! grep -qF "$want" "$OUT"; then
		echo "odi_switch_acl_clean_test: missing log line: $want"
		fail=1
	fi
done
[ "$fail" = 0 ] || { echo "odi_switch_acl_clean_test: FAILED"; exit 1; }
echo "odi_switch_acl_clean_test: log lines ok"
