#!/usr/bin/env bash
#
# tools/check-release-ancestry.sh against throwaway repositories: a commit on
# the branch, the branch tip, a commit only on a side branch (the gate must
# refuse it), a missing branch, a bad commit, and a shallow clone whose
# history has to be deepened before the question can be answered.
set -u
cd "$(dirname "$0")/.." || exit 1
S=$PWD/tools/check-release-ancestry.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok() { echo "ok    $1"; pass=$((pass + 1)); }
no() { echo "FAIL  $1"; fail=$((fail + 1)); }

g() { git -c user.name=t -c user.email=t@example.invalid -c commit.gpgsign=false "$@"; }

mkdir "$T/origin" && cd "$T/origin" || exit 1
g init -q -b main .
echo 1 > f && g add f && g commit -q -m one
c1=$(g rev-parse HEAD)
echo 2 > f && g commit -q -am two
g checkout -q -b side
echo 3 > f && g commit -q -am side-only
side=$(g rev-parse HEAD)
g checkout -q main
echo 4 > f && g commit -q -am four
tip=$(g rev-parse HEAD)

g clone -q "file://$T/origin" "$T/full" 2>/dev/null
g clone -q --depth 1 "file://$T/origin" "$T/shallow" 2>/dev/null

# Run the script in a clone; returns its status, output in $T/out.
run() { (cd "$1" && shift && "$S" "$@") > "$T/out" 2>&1; }

run "$T/full" "$c1" && ok "an older commit of main passes" || no "an older commit of main passes"
run "$T/full" "$tip" && ok "the tip of main passes" || no "the tip of main passes"
if run "$T/full" "$side"; then no "a side-branch-only commit is refused"
else
	grep -q 'NOT an ancestor' "$T/out" && ok "a side-branch-only commit is refused" || no "refused, but without saying why"
fi
# The fetch must be what makes the shallow clone answer, not luck: HEAD there
# is the tip, but the older commit only exists after the deepening.
git -C "$T/shallow" rev-parse --is-shallow-repository | grep -q true || no "fixture: clone is shallow"
if run "$T/shallow" "$tip"; then ok "a shallow clone passes the tip"; else no "a shallow clone passes the tip"; fi
git -C "$T/shallow" rev-parse --is-shallow-repository | grep -q false && ok "the shallow clone was deepened" || no "the shallow clone was deepened"
run "$T/full" "$c1" nosuchbranch && no "a missing branch fails" || ok "a missing branch fails"
run "$T/full" deadbeefdeadbeef && no "an unknown commit fails" || ok "an unknown commit fails"
# The tag name decides: a final tag is judged by its commit, a pre-release
# tag (hyphen after the version) is exempt, and a name that merely contains
# a hyphen, like a branch, is not.
run "$T/full" "$side" main v9.9.9 && no "a final tag on a side branch is refused" || ok "a final tag on a side branch is refused"
run "$T/full" "$side" main v9.9.9-beta.1 && ok "a pre-release tag on a side branch passes" || no "a pre-release tag on a side branch passes"
run "$T/full" "$c1" main v9.9.9-beta.2 && ok "a pre-release tag on main passes" || no "a pre-release tag on main passes"
run "$T/full" "$side" main release-gate && no "a hyphenated branch name is still checked" || ok "a hyphenated branch name is still checked"
run "$T/full" "$c1" main v9.9.9 && ok "a final tag on main passes" || no "a final tag on main passes"
# A fetch that cannot happen fails closed instead of passing on stale refs.
git -C "$T/full" remote set-url origin "file://$T/nowhere"
run "$T/full" "$c1" && no "an unreachable origin fails closed" || ok "an unreachable origin fails closed"

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
