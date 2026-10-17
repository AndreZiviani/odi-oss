#!/bin/sh
# Shared helpers of the OMCI suites (src/omci/*-test.sh). POSIX sh, so it
# runs under dash and busybox ash in the toolchain container.
#
# Source it after the suite has set Q (the qemu-user binary) and fail=0:
#
#   . ./testlib.sh
#
# Why it exists. omcid's -w is an idle exit in 5 s units, and a suite that
# waits for it (or sleeps a fixed time) pays that time on every run, however
# fast the daemon is. The daemon answers every injected frame in order and
# flushes its log after each one, and it exits cleanly on SIGTERM
# (respond/main.c, on_signal), so a suite can wait for the line it is going to
# check and then stop the daemon itself. Every wait here is a bounded poll:
# a daemon that never prints the line costs the timeout, once, and a clear
# message.

# wait_for <file> <pattern> <seconds> [count]: the pattern (a basic regular
# expression, as grep reads it) shows at least <count> times (default 1) in
# the file within the time, polled every 0.1 s. Returns 1 on timeout, saying
# what it waited for; the check that follows then fails on its own, or the
# caller counts it (need).
wait_for() {
	_wf_n=$(( $3 * 10 ))
	_wf_i=0
	while [ "$_wf_i" -lt "$_wf_n" ]; do
		[ "$(grep -c -- "$2" "$1" 2>/dev/null)" -ge "${4:-1}" ] && return 0
		sleep 0.1
		_wf_i=$((_wf_i + 1))
	done
	echo "TIMEOUT after $3 s waiting for ${4:-1} x /$2/ in $1"
	return 1
}

# need <file> <pattern> <seconds> [count]: wait_for, and a failed check when it
# times out, for a wait that no check after it would notice.
need() {
	wait_for "$@" || fail=$((fail + 1))
}

# wait_bounded <pid> <seconds>: wait for a child of this shell, which must end
# within the time or is killed outright (and its status is then 137). The wait
# builtin has no timeout of its own, and a poll on kill -0 does not work: a
# child that ended is a zombie until it is waited for, and still answers it.
# The status of the child is the return value.
wait_bounded() {
	# stdio detached: the sleep outlives the subshell that is killed below,
	# and one that held the stdout of a $(...) would keep it waiting.
	( sleep "$2"; kill -9 "$1" 2>/dev/null ) > /dev/null 2>&1 &
	_wb_wd=$!
	wait "$1" 2>/dev/null
	_wb_rc=$?
	kill "$_wb_wd" 2>/dev/null
	wait "$_wb_wd" 2>/dev/null
	return "$_wb_rc"
}

# omcid_stop <pid>: SIGTERM, which makes omcid deregister and exit 0
# (respond/main.c, on_signal), and wait for it. After 20 s a daemon that has
# not gone is killed and that is a failed check, so a hung daemon cannot hang
# the suite.
omcid_stop() {
	kill -TERM "$1" 2>/dev/null || return 0
	wait_bounded "$1" 20
	if [ "$?" = 137 ]; then
		echo "FAIL  omcid ($1) did not exit within 20 s of SIGTERM"
		fail=$((fail + 1))
	fi
	return 0
}

# omcid_up <log> [seconds]: omcid has started and is serving (it prints this
# line, flushed, just before its loop). Everything it printed at startup is in
# the log by then.
omcid_up() {
	wait_for "$1" 'answering as' "${2:-30}"
}

# tci_of <frame>: the transaction id, the first two bytes (four hex digits).
tci_of() {
	_t=${1#????}
	printf '%s' "${1%"$_t"}"
}

# inject_wait <log> <frame>: inject one frame and wait for omcid's answer to
# it. The queue is first in, first out and omcid answers in order, so when the
# answer is in the log every frame before it has been handled too. Every frame
# that asks for an answer (the AR bit, 0x40, of the message type) gets one.
inject_wait() {
	_iw_tci=$(tci_of "$2")
	_iw_n=$(grep -c -- "^-> $_iw_tci" "$1" 2>/dev/null || true)
	$Q cli/build/omcli --inject "$2" > /dev/null 2>&1
	wait_for "$1" "^-> $_iw_tci" 30 $((${_iw_n:-0} + 1))
}

# sync_send <log>: inject an inert Get of ONU-G (class 256, attribute 1) and
# wait for its answer: everything injected before it has been handled when it
# returns. For a batch sent with --inject-file, where the batch has no frame
# of its own to wait for. Each call uses a transaction id of its own.
SYNC_N=0
sync_send() {
	SYNC_N=$((SYNC_N + 1))
	inject_wait "$1" "$(printf 'ff%02x490a%s%s%s%060d00000028%08d' "$SYNC_N" 0100 0000 8000 0 0)"
}

# rebuilt <log>: how many times the bridge connections have been rebuilt.
# The rebuild runs one quiet second after the last OMCI frame (respond/main.c),
# so it is the end of a session as far as the datapath is concerned: a suite
# that checks what a session built waits for the count to go up, not for a
# time.
rebuilt() {
	grep -c 'bridge connections rebuilt' "$1" 2>/dev/null
}

# settled <log>: everything injected so far has been handled (the sentinel's
# answer) and the rebuild that follows the last frame by one quiet second has
# run, which is when a session is all in the log and in the MIB bookkeeping.
# The rebuild is pending after any session that changed the MIB, so a log that
# never shows one is a failed wait.
settled() {
	sync_send "$1"
	_st_n=$(rebuilt "$1")
	wait_for "$1" 'bridge connections rebuilt' 30 $((_st_n + 1))
}

# keep_log <file> <name>: a daemon log kept past the container, in out/test-logs
# of the mounted tree (ignored by git), for a suite that failed. <name> says
# which suite it is from, since the suites run side by side.
keep_log() {
	mkdir -p /src/out/test-logs 2>/dev/null && cp "$1" "/src/out/test-logs/$2" 2>/dev/null
	return 0
}
