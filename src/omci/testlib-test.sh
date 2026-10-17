#!/bin/sh
# Host test of testlib.sh itself: the helpers must fail the suite when the
# daemon is dead or a wait times out, and must not when the line shows up.
# No daemon and no container; a stub stands in for the qemu-user omcli.
# Run from anywhere: sh src/omci/testlib-test.sh
set -u
cd "$(dirname "$0")" || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
bad=0
ck() {
	if [ "$2" = "$3" ]; then echo "ok    $1"; else echo "FAIL  $1: got '$2', want '$3'"; bad=$((bad + 1)); fi
}

# Each case runs the helpers in a fresh shell, as a suite does, and prints
# the failure count they left. Q is the command that stands in for qemu-user.
run() { # <Q> <body>
	WAIT_S=1 Q=$1 sh -c '. ./testlib.sh; fail=0; log=$1; shift; '"$2" _ "$T/log" 2>/dev/null | tail -1
}

: > "$T/log"
ck "a wait for a line that is not there fails the suite" \
	"$(run true 'wait_for "$log" "^never" 1 >/dev/null; echo $fail')" 1
ck "need counts that failure once" \
	"$(run true 'need "$log" "^never" 1 >/dev/null; echo $fail')" 1
echo "hello" > "$T/log"
ck "a wait for a line that is there passes" \
	"$(run true 'wait_for "$log" "^hello" 1; echo $fail')" 0
# A daemon that answers: the stub omcli appends the answer line to the log.
printf '#!/bin/sh\necho "-> 0001" >> "%s"\n' "$T/log" > "$T/answer"
chmod +x "$T/answer"
: > "$T/log"
ck "inject_wait on a daemon that answers passes" \
	"$(run "$T/answer" 'inject_wait "$log" 00014a0a >/dev/null; echo $fail')" 0
: > "$T/log"
ck "inject_wait on a dead daemon (empty log) fails" \
	"$(run true 'inject_wait "$log" 00014a0a >/dev/null; echo $fail')" 1
ck "inject_wait fails at once when the frame cannot be sent" \
	"$(run false 'inject_wait "$log" 00014a0a; echo $fail')" 1
ck "settled on a dead daemon fails" \
	"$(run true 'settled "$log" >/dev/null; echo $fail')" 2
ck "omcid_up on a daemon that never serves fails" \
	"$(run true 'omcid_up "$log" 1 >/dev/null; echo $fail')" 1
ck "suite_exit with 256 failures is not 0" \
	"$(sh -c '. ./testlib.sh; fail=256; suite_exit'; echo $?)" 1
ck "suite_exit with none is 0" \
	"$(sh -c '. ./testlib.sh; fail=0; suite_exit'; echo $?)" 0

[ "$bad" -eq 0 ] || { echo "FAILED ($bad)"; exit 1; }
echo "all ok"
