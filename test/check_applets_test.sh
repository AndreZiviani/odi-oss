#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later
# tools/check-applets.py: each way a script can run a command is seen, so a
# command missing from the applet list fails the check wherever it sits,
# and what is not a command (an argument, a comment, a here-document, a
# case pattern, an fd number) does not. A small list stands in for
# packages/busybox/applets.txt, and the image check runs at the end.
# Needs bash and python3, like the tool.
set -u
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
LIST=$T/applets.txt
# The names odi-ui and the exporter need (OTHER_REPOS) are on it too.
echo "sh cat grep sleep timeout setsid sed xargs md5sum mkdir pidof ping reboot" > "$LIST"

# missing NAME SCRIPT: the check fails, and names NAME.
missing() {
	printf '%s\n' "$2" > "$T/s.sh"
	out=$(python3 -I tools/check-applets.py --applets "$LIST" "$T/s.sh" 2>&1); rc=$?
	if [ "$rc" = 1 ] && printf '%s\n' "$out" | grep -qE "runs \`(/[a-z/]*/)?$1\`"; then
		echo "ok    missing $1: $3"; pass=$((pass + 1))
	else
		echo "FAIL  missing $1: $3 (rc $rc)"; printf '%s\n' "$out" | sed 's/^/        /'; fail=$((fail + 1))
	fi
}
# clean SCRIPT WHAT: the check passes.
clean() {
	printf '%s\n' "$1" > "$T/s.sh"
	if out=$(python3 -I tools/check-applets.py --applets "$LIST" "$T/s.sh" 2>&1); then
		echo "ok    clean: $2"; pass=$((pass + 1))
	else
		echo "FAIL  clean: $2"; printf '%s\n' "$out" | sed 's/^/        /'; fail=$((fail + 1))
	fi
}

missing head 'head -1 /etc/x' "a plain command"
missing head 'cat /etc/x | head -1' "in a pipeline"
missing head 'x=$(cat /etc/x | head -1)' "in \$( )"
missing head 'x="a $(head -1 /etc/x) b"' "in \$( ) inside double quotes"
missing head 'x=`head -1 /etc/x`' "in backticks"
missing head 'x="`head -1 /etc/x`"' "in backticks inside double quotes"
missing head 'echo $(( $(head -1 /etc/x) + 1 ))' "in \$( ) inside arithmetic"
missing head 'cat <<EOF
size "$(head -1 /etc/x)"
EOF' "in a here-document that expands"
missing head 'sh -ec "head -1 /etc/x"' "in sh -ec"
missing head 'sh -ce "head -1 /etc/x"' "in sh -ce"
missing head 'sh -e -c "head -1 /etc/x"' "in sh -e -c"
missing head '# an example: cat <<EOF
head -1 /etc/x' "after a << in a comment"
missing head "echo '<<EOF'
head -1 /etc/x" "after a << in quotes"
missing head 'xargs -n 1 head -1 < /etc/x' "behind xargs -n 1"
missing head 'nice -n 5 head -1 /etc/x' "behind nice -n"
missing head '/bin/busybox head -1 /etc/x' "behind /bin/busybox"
missing head '( head -1 /etc/x )' "in a subshell"
missing head 'if head -1 /etc/x; then :; fi' "after if"
missing head 'true && head -1 /etc/x' "after &&"
missing head 'timeout -s KILL 5 head -1 /etc/x' "behind timeout and its options"
missing head 'setsid nohup head -1 /etc/x &' "behind setsid and nohup"
missing head 'X=1 Y=2 head -1 /etc/x' "after assignments"
missing head 'sh -c "head -1 /etc/x"' "in sh -c"
missing head '/bin/head -1 /etc/x' "by path"
missing head 'HEAD=${HEAD:-head}
"$HEAD" -1 /etc/x' "through a variable default"
missing head 'TIMEOUT=${TIMEOUT:-timeout}
"$TIMEOUT" "$S" head -1 /etc/x' "behind timeout through a variable"
missing head 'step() { name=$1; shift; timeout 5 "$@" > "$name"; }
step out.txt head -1 /etc/x' "behind a function that runs \"\$@\""
missing head 'case $1 in
a) head -1 /etc/x ;;
esac' "in a case branch"
missing head 'f() {
	head -1 /etc/x
}' "in a function body"
missing head 'exec /etc/scripts/respawn.sh x 0 /var/log/l head -1' "behind respawn.sh"

clean '# head is not here
cat /etc/x' "a comment"
clean 'cat <<EOF
head -1
EOF' "a here-document"
clean "cat <<'EOF'
\$(head -1 /etc/x)
EOF" "a here-document that does not expand"
clean 'echo head; grep head /etc/x' "an argument"
clean 'case $1 in
head|tail) cat /etc/x ;;
esac' "a case pattern"
clean 'cat /etc/x 2>/dev/null 9>&1' "an fd number"
clean 'for head in a b; do echo "$head"; done' "a for list"
clean 'head() { sed -n 1p "$1"; }
head /etc/x' "a function of the same name"
clean '/etc/scripts/network.sh addr; /tmp/memprobe d 0' "a script in the skeleton, a pushed tool"
clean 'command -v head >/dev/null && echo yes' "command -v"
clean 'ip link; diag show; /sbin/dropbear -V' "our binaries and the other packages"
clean '"$@"' "a command built at run time"
clean 'cat <<EOF
a "b" $("cat" /etc/x)
EOF' "a quoted command in an expanding here-document"
clean 'xargs -n 1 cat < /etc/x' "the argument of xargs -n"

# A function counts only where it is defined, or in a script sourced with
# it: another script using the same name runs the applet.
printf 'head() { cat "$1"; }\n' > "$T/a.sh"
printf 'head -1 /etc/x\n' > "$T/b.sh"
if python3 -I tools/check-applets.py --applets "$LIST" "$T/a.sh" "$T/b.sh" 2>&1 | grep -q "b.sh:1: runs \`head\`"; then
	echo "ok    missing head: a function of another script does not count"; pass=$((pass + 1))
else echo "FAIL  missing head: a function of another script does not count"; fail=$((fail + 1)); fi
printf '. %s/a.sh\nhead -1 /etc/x\n' "$T" > "$T/b.sh"
if python3 -I tools/check-applets.py --applets "$LIST" "$T/a.sh" "$T/b.sh" >/dev/null 2>&1; then
	echo "ok    clean: a function of a sourced script"; pass=$((pass + 1))
else echo "FAIL  clean: a function of a sourced script"; fail=$((fail + 1)); fi

# What odi-ui and the exporter run is checked against the list too.
printf 'cat /etc/x\n' > "$T/s.sh"
sed 's/ ping//' "$LIST" > "$T/noping.txt"
if python3 -I tools/check-applets.py --applets "$T/noping.txt" "$T/s.sh" 2>&1 | grep -q 'runs `ping`'; then
	echo "ok    missing ping: odi-ui needs it"; pass=$((pass + 1))
else echo "FAIL  missing ping: odi-ui needs it"; fail=$((fail + 1)); fi

printf '"$@"\n' > "$T/s.sh"
out=$(python3 -I tools/check-applets.py --list "$T/s.sh") && echo "$out" | grep -q '"$@"' &&
	{ echo "ok    --list prints what was found"; pass=$((pass + 1)); } ||
	{ echo "FAIL  --list"; fail=$((fail + 1)); }

# And the image itself: the scripts make lint checks, against the real list.
if out=$(make -s check-applets 2>&1); then
	echo "ok    the on-device scripts against packages/busybox/applets.txt"; pass=$((pass + 1))
else
	echo "FAIL  the on-device scripts against packages/busybox/applets.txt"
	printf '%s\n' "$out" | sed 's/^/        /'; fail=$((fail + 1))
fi

echo; echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
