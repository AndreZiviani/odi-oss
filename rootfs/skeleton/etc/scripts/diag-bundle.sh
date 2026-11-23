#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

#
# diag-bundle.sh [--full] [OUT] -- collect what a bug report about this
# stick needs into one tar.gz, with every secret redacted and the identity
# of the stick masked. The web UI serves it as GET /api/diag (odi-ui confd
# runs this and streams OUT); it is as usable from a shell:
#
#     /etc/scripts/diag-bundle.sh /tmp/odi-diag.tar.gz
#
# OUT defaults to /tmp/odi-diag.tar.gz. On success the last line on stdout
# is OUT and the exit status is 0; on failure OUT is removed and the status
# is 1. Busybox only.
#
# What goes in, under odi-diag/ in the archive: the previous boot ramlog
# (/proc/odi_ramlog_prev), /var/log/*, dmesg, the syslog ring (logread:
# the event= lines omcid sends only to syslog), /etc/odi-build, /etc/version,
# the readable /proc/odi_wdt files, the host-link state (host-link.txt),
# uptime, /proc/meminfo, /proc/mounts, /proc/cmdline, ps, each process's resident
# memory (rss.txt), df, the U-Boot slot variables from both environment copies
# (nv getenv and nv fallback), a scrape of the exporter, the config store
# (lastgood.xml, lastgood_hs.xml, odi.conf) with every secret value
# replaced by REDACTED, and under pon/ the GPON and OMCI state: diag (ONU
# state, alarms, GEM flows, port counters, the PON queue registers that
# cmd 23 and 25 write), /proc/odi_gpon/ (status, counters, flows, ploam,
# cam, ponq), /proc/odi_omci, and omcli (state, provision, flows, tcont,
# check (the MIB against the chip), conn, vlan (the VLAN handling and any
# FwdOp override; numbers only), voip (masked), and the MIB
# classes in OMCI_CLASSES), and the last OMCI frames as pon/omci.pcap
# (omcli frames, masked), and under module/ the optics module on /dev/i2c-0: A0h whole (a0.txt), A2h
# with the page select as found (a2.txt), and in chip.txt the page select,
# the laser driver identity (A2h page 2 byte 0xa0, 0x6a for a GN25L95;
# bytes 0x80 and 0xdc, the UX3320 variant markers) and the UX3320
# INIT_STATE (page 3 byte 0xf0), with the upper half of every page in
# I2C_PAGES that could be selected (a2-p<N>.txt). Selecting a page is the
# one write the bundle makes anywhere; the page found is put back and read
# back, and chip.txt records both. MANIFEST.txt lists each step, its exit
# status and its size. README.md, at the top, says what the bundle is, what
# was hidden and which file answers which question; SUMMARY.txt is one
# screen of facts that diag-summary.sh takes from the files above.
#
# What is redacted, in the config copies: the keys in SECRET_KEYS below,
# and any key whose name contains one of the words in SECRET_WORDS. An empty
# value stays empty, so "not set" is still visible. Then every non-empty
# secret value found -- those keys, plus the web UI password from
# /etc/config/confd.auth -- is scrubbed from EVERY file in the bundle, as
# text, as hex and as space-separated hex, in case a log quoted one. A value
# shorter than SHORT_LEN is scrubbed only where it stands as a whole token,
# so a USER_PASSWORD of "user" does not turn userland_ok into REDACTEDland_ok.
# confd.auth, the dropbear keys and /etc/passwd are never collected at all.
#
# What is masked: the serial number, the MACs, the LOID and the module
# serial (IDENT_KEYS, the serial in /proc/odi_gpon/status, the interface MACs, A0h
# bytes 68-83, which are also blanked in module/a0.txt). Many OLTs authenticate by
# serial number alone, so a bundle posted in a public issue would otherwise
# hand out what it takes to clone the line. Each is replaced, in every file
# and in every spelling (text, hex, MAC with and without separators), by
# MASKED- and its last four characters, which still tells two sticks apart.
# --full keeps them, for a bundle shared privately; the web UI never passes
# it. The PLOAM ring in /proc/odi_gpon/ploam prints message bodies, so the bodies
# that carry the password or a key fragment are always withheld, and those
# that carry the serial number unless --full: what is left of a value cut
# by the ten-byte window would escape the scrub.
#
# The phone lines come in as pon/omcli_voip.txt, from omcli voip, which masks
# the number, the display name and the login and never prints a password;
# omcli voip --secrets-list feeds every one of those strings, and the login
# of a server URL, to the scrub above, as it does the config store secrets.
# When that list cannot be had (omcid does not answer omcli state, or the
# list times out) and OMCI_POTS_PORTS allows phone lines, the free-text
# logs (log/, syslog.txt, dmesg.txt, ramlog_prev.txt) and the pcap are
# withheld rather than shipped without it, and MANIFEST.txt and README.md
# say so; the rest of the bundle is written. With phone lines off there
# are no SIP credentials to scrub, and the bundle is whole.
#
# The MIB classes are a list on purpose, not the whole MIB: an OLT can push
# credentials through OMCI (SIP user data, authentication methods, large
# strings, the TR-069 server), and none of that is in the classes below.
# omcli mib masks those classes by default (only --secrets prints them) and
# the bundle never passes it, so a class added here by mistake still would
# not leak; src/omci/writes-test.sh pins both halves.
#
# pon/omci.pcap is the masked export of omcli frames, with or without --full
# here: omcid overwrites the values of the classes above, the serial number
# and the MACs itself (src/omci/respond/frames.c). A pcap is binary and the
# scrub below would shift its records, so it is not scrubbed; instead, a
# pcap in which any secret, or any identity value unless --full, shows as
# text is withheld, and MANIFEST.txt says so.
#
# Every step is bounded: a command through busybox timeout (STEP_TIMEOUT_S
# each, METRICS_TIMEOUT_S for the exporter scrape, which runs diag and
# omcicli behind it with bounds of their own of 3 s and 2 s), a log file by
# LOG_FILE_MAX bytes and all of /var/log together by LOG_TOTAL_MAX, and the
# archive by BUNDLE_MAX, past which it is refused rather than served. When
# omcid does not answer omcli state, the other omcli steps are skipped, so a
# hung omcid costs one timeout rather than one per step. Each i2c command
# has its own STEP_TIMEOUT_S, and the first failure among the page selects
# ends them and goes to the restore.

# confd starts its children with no environment at all, and busybox timeout
# then looks only in /bin and /usr/bin: logread (/sbin) was "not found" in a
# bundle taken from the web UI. Appended, so a PATH the caller set wins.
PATH=${PATH:+$PATH:}/sbin:/usr/sbin:/bin:/usr/bin
export PATH
# Bytes, not characters: a secret or a log line need not be valid in the
# caller locale, and awk, sort and sed in a UTF-8 locale can fail on one
# (a BSD awk with "multibyte conversion failure", sort with "Illegal byte
# sequence"), which would drop that secret from the scrub.
LC_ALL=C
export LC_ALL

STEP_TIMEOUT_S=${STEP_TIMEOUT_S:-5}
HOST_AUTONEG_SH=${HOST_AUTONEG_SH:-/etc/scripts/host-autoneg.sh}
SAFE_MODE_SH=${SAFE_MODE_SH:-/etc/scripts/safe-mode.sh}
HOST_SERDES_SH=${HOST_SERDES_SH:-/etc/scripts/host-serdes.sh}
METRICS_TIMEOUT_S=${METRICS_TIMEOUT_S:-10}
LOG_FILE_MAX=${LOG_FILE_MAX:-262144}
LOG_TOTAL_MAX=${LOG_TOTAL_MAX:-1048576}
BUNDLE_MAX=${BUNDLE_MAX:-2097152}
CONFIG_DIR=${CONFIG_DIR:-/var/config}
LOG_DIR=${LOG_DIR:-/var/log}
METRICS_URL=${METRICS_URL:-http://127.0.0.1:9100/metrics}
# diag-summary.sh, next to this script.
case $0 in
*/*) DIAG_SUMMARY_SH=${DIAG_SUMMARY_SH:-${0%/*}/diag-summary.sh} ;;
*) DIAG_SUMMARY_SH=${DIAG_SUMMARY_SH:-/etc/scripts/diag-summary.sh} ;;
esac

SECRET_KEYS="GPON_PLOAM_PASSWD LOID_PASSWD LOID_PASSWD_OLD USER_PASSWORD
SUSER_PASSWORD E8BDUSER_PASSWORD SUPER_PASSWORD MAC_KEY HW_FON_KEYWORD
OMCI_FIXED_ME"
SECRET_WORDS="PASS PWD PSK SECRET TOKEN KEY COMMUNITY CRED"
IDENT_KEYS="GPON_SN ELAN_MAC_ADDR LOID LOID_OLD"
# Below SHORT_LEN characters a value is scrubbed as a whole token only: as
# text, where no letter or digit touches it on either side, and as hex,
# where no hex digit does. A short value is often a word or a piece of one
# (the factory USER_PASSWORD is "user", inside "userland"), and its hex
# spelling, 14 digits at most, can sit inside the hex of a register dump;
# a substring scrub there mangles the file and, by the hole it leaves,
# tells the reader the value. From eight characters on, an accidental
# match is unlikely: few words in logs, paths and names are that long, and
# the hex spelling (16 digits) is wider than any register word printed.
SHORT_LEN=8
# ONU-G, ONU2-G, PPTP Ethernet UNI, MAC bridge service profile and port,
# VLAN tagging filter, 802.1p mapper, T-CONT, ANI-G, GEM interworking TP,
# GEM port network CTP, priority queue, traffic scheduler, traffic
# descriptor, multicast GEM interworking TP, multicast operations profile
# and subscriber config, VEIP, extended VLAN tagging operation.
OMCI_CLASSES="256 257 11 45 47 84 130 262 263 266 268 277 278 280 281 309 310 329 171"
GPON_PROC_DIR=${GPON_PROC_DIR:-/proc/odi_gpon}
# Where rss.txt reads /proc/PID/status from; the test points it at a fixture.
PROC_DIR=${PROC_DIR:-/proc}
PONQ_BASE=0xf020a8	# PONQ_COUNT_MASK +0, through +239 (docs/SWITCH.md)
PONQ_WORDS=240
# The A2h pages whose upper half module/ dumps, in this order: 2 and 3 first,
# they carry the laser driver identity and state.
I2C_PAGES="2 3 0 4 5 6"

full=0
if [ "$1" = --full ]; then
	full=1
	shift
fi
out=${1:-/tmp/odi-diag.tar.gz}
work=$(mktemp -d /tmp/odi-diag.XXXXXX) || { echo "diag-bundle: cannot create a work directory in /tmp" >&2; exit 1; }
b=$work/odi-diag
mkdir "$b"
# restore_page is set while the module section has a page selected that
# is not the one it found (below): an exit then puts that page back.
restore_page=
cleanup() {
	[ -n "$restore_page" ] && timeout "$STEP_TIMEOUT_S" i2cset -y 0 0x51 0x7f "$restore_page" 2>/dev/null
	rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

fail() { echo "diag-bundle: $*" >&2; rm -f "$out"; exit 1; }

manifest() { printf '%-4s %8s  %s\n' "$1" "$2" "$3" >> "$b/MANIFEST.txt"; }
# withhold NAME WHY: NAME leaves the bundle, and its MANIFEST line says why
# instead of what was collected.
withhold() {
	rm -f "$b/$1"
	awk -v n="$1" '$3 != n' "$b/MANIFEST.txt" > "$work/manifest.tmp" &&
		cat "$work/manifest.tmp" > "$b/MANIFEST.txt" ||
		fail "cannot update MANIFEST.txt"
	rm -f "$work/manifest.tmp"
	manifest - - "$1 (withheld: $2)"
}
size_of() { wc -c < "$1" 2>/dev/null | tr -d ' '; }

# step NAME CMD ARGS...: run CMD under timeout into odi-diag/NAME, and
# return its status.
step() {
	name=$1
	shift
	timeout "$STEP_TIMEOUT_S" "$@" > "$b/$name" 2>&1
	rc=$?
	manifest "$rc" "$(size_of "$b/$name")" "$name"
	return "$rc"
}

# grab NAME FILE: copy one file, or /proc entry, bounded like a command.
grab() {
	if [ -e "$2" ]; then
		step "$1" cat "$2"
	else
		manifest - - "$1 (absent: $2)"
	fi
}

{
	echo "odi-oss diagnostics bundle"
	echo "created (stick clock, UTC): $(date -u '+%Y-%m-%d %H:%M:%S')"
	echo "uptime: $(cut -d' ' -f1 /proc/uptime 2>/dev/null) s"
	echo "redacted keys: $(echo "$SECRET_KEYS" | tr '\n' ' ')and any key containing: $SECRET_WORDS"
	if [ "$full" = 1 ]; then
		echo "identity: kept (--full): $IDENT_KEYS, the serial and the MACs"
	else
		echo "identity: masked: $IDENT_KEYS, the serial and the MACs"
	fi
	echo
	echo "rc      bytes  file"
} > "$b/MANIFEST.txt"

grab ramlog_prev.txt /proc/odi_ramlog_prev
step dmesg.txt dmesg
step syslog.txt logread
grab odi-build.txt /etc/odi-build
grab version.txt /etc/version
for f in watchdog_flag userland_ok clients; do
	grab "odi_wdt_$f.txt" "/proc/odi_wdt/$f"
done
step uptime.txt uptime
grab meminfo.txt /proc/meminfo
grab mounts.txt /proc/mounts
grab cmdline.txt /proc/cmdline
step ps.txt ps
step df.txt df

# rss.txt: the resident memory of each process, largest first, from
# /proc/PID/status (busybox ps shows only VSZ). One RSS that grows between
# two bundles is what /proc/meminfo alone cannot point at (#465). Kernel
# threads have no VmRSS and are left out. The name is the rest of the Name:
# line, so a name with a blank in it stays whole. The loop is one step, so
# STEP_TIMEOUT_S bounds it and MANIFEST.txt has the status of the sort.
RSS_AWK='$1 == "Name:" { sub(/^Name:[ \t]*/, ""); n = $0 } $1 == "VmRSS:" { r = $2 }
	END { if (r != "") printf "%8s kB  %6s  %s\n", r, pid, n }'
step rss.txt sh -c 'for d in "$1"/[0-9]*; do
	[ -r "$d/status" ] || continue
	awk -v pid="${d##*/}" "$2" "$d/status" 2>/dev/null
done | sort -rn' sh "$PROC_DIR" "$RSS_AWK"

: > "$b/nv.txt"
for v in sw_active sw_commit sw_tryactive sw_version0 sw_version1; do
	for verb in getenv fallback; do
		printf '%s %s: ' "$verb" "$v" >> "$b/nv.txt"
		timeout "$STEP_TIMEOUT_S" nv "$verb" "$v" >> "$b/nv.txt" 2>&1 || echo "(rc $?)" >> "$b/nv.txt"
	done
done
manifest - "$(size_of "$b/nv.txt")" nv.txt

timeout "$METRICS_TIMEOUT_S" wget -q -O - "$METRICS_URL" > "$b/metrics.txt" 2>&1
manifest "$?" "$(size_of "$b/metrics.txt")" metrics.txt

# host-link.txt: the host SerDes mode (HOST_SERDES) and the host-port
# auto-negotiation (HOST_AUTONEG): the state host-serdes.sh and
# host-autoneg.sh keep (a trial, the boot fallback), the safe mode (this
# boot, the record, the boot count), then the words that decide the host link: the PCS
# control word (HOST_FIBER_CTRL), the port ability the link runs with, the
# forced-ability pair of port 0, the host SerDes lane's control and
# in-band negotiation words, and the words the SerDes modes differ in. All
# read before on a running stick.
{
	echo "# host-serdes.sh status"
	timeout "$STEP_TIMEOUT_S" sh "$HOST_SERDES_SH" status 2>&1
	echo "# host-autoneg.sh status"
	timeout "$STEP_TIMEOUT_S" sh "$HOST_AUTONEG_SH" status 2>&1
	echo "# safe-mode.sh status"
	timeout "$STEP_TIMEOUT_S" sh "$SAFE_MODE_SH" status 2>&1
	echo "# HOST_FIBER_CTRL 0x21c00, PORT_ABILITY(0) 0x198, PORT_FORCE_SELECT(0) 0x1b4, PORT_FORCE_ABILITY(0) 0x180,"
	echo "# SDS_CTRL 0x214 (bit 17: the host lane receiver is in sync; bits 4:0 the lane mode, 4 = 1000BASE-X),"
	echo "# SDS_AN_CTRL 0x21808 (bits 9:8 force in-band negotiation off, one bit per lane; bit 8 = the host lane)"
	echo "# SerDes pages, one word each, read-only: 0x214 host SerDes mode (bit 17 = in sync), 0x110 host port,"
	echo "# 0x21500 RX enable, 0x2160x 0x2161x 0x2166x 0x2168x 0x2169x 1.25G page, 0x2170x 0x2171x 0x2176x 3.125G page,"
	echo "# 0x215bx 0x215e4 analog, 0x21c04 fiber word 2, 0x22e4c"
	for a in 0x21c00 0x198 0x1b4 0x180 0x214 0x21808 0x110 0x21500 0x21600 0x2160c 0x21618 0x2166c \
		0x21680 0x2168c 0x21698 0x21700 0x2170c 0x21718 0x2176c 0x215b4 0x215b8 0x215bc 0x215e4 \
		0x21c04 0x22e4c; do
		echo "register get $a 1"
	done | timeout "$STEP_TIMEOUT_S" diag 2>&1
} > "$b/host-link.txt"
manifest - "$(size_of "$b/host-link.txt")" host-link.txt

# pon/: the GPON and OMCI state. One diag run reads it all, batched on
# stdin; omcli asks omcid one question per run.
mkdir "$b/pon"
# The transceiver reads are the ones the exporter makes on every scrape;
# SUMMARY.txt takes the optics from them.
printf '%s\n' "gpon get onu-state" "gpon get alarm-status" "gpon get flows" \
	"mib dump counter port all" "register get $PONQ_BASE $PONQ_WORDS" \
	"pon get transceiver rx-power" "pon get transceiver tx-power" \
	"pon get transceiver temperature" "pon get transceiver bias-current" \
	"pon get transceiver voltage" |
	timeout "$STEP_TIMEOUT_S" diag > "$b/pon/diag.txt" 2>&1
manifest "$?" "$(size_of "$b/pon/diag.txt")" pon/diag.txt
grab pon/odi_omci.txt /proc/odi_omci

# The PLOAM ring prints ten bytes of each message body. Withheld always:
# Password (us 0x02) and Encryption_Key (us 0x05). Withheld unless --full,
# for the serial number in them: Serial_Number_ONU (us 0x01), Acknowledge
# (us 0x09, it echoes a downstream message), Serial_Number_Mask (ds 0x02),
# Assign_ONU-ID (ds 0x03) and Disable_Serial_Number (ds 0x06, 0x81). The
# PON identifier of a PON-ID (ds 0x15, content bytes 2 to 8) names the OLT
# port, so unless --full it is xx there and MASKED on the pon_id line of
# status; the class and TOL stay.
# drops (the records around each drop out of O5) has PLOAM types and
# ONU-IDs only, never a body, so it is copied as it is.
for f in status counters flows ploam drops cam ponq; do
	if [ -e "$GPON_PROC_DIR/$f" ]; then
		timeout "$STEP_TIMEOUT_S" cat "$GPON_PROC_DIR/$f" > "$work/gpon-$f.raw" 2>&1
		rc=$?
		if [ "$f" = ploam ]; then
			awk -v full="$full" '
			$2 ~ /^(us|ds)$/ && $3 == "onu_id" && $5 == "type" && $7 == "content" {
				t = $2 " " $6
				if (t == "us 0x02" || t == "us 0x05" ||
				    (!full && (t == "us 0x01" || t == "us 0x09" || t == "ds 0x02" ||
					       t == "ds 0x03" || t == "ds 0x06" || t == "ds 0x81"))) {
					sub(/content .*/, "content (withheld)")
				} else if (!full && t == "ds 0x15" && NF >= 17) {
					for (i = 9; i <= 15; i++)
						$i = "xx"
				}
			}
			{ print }' "$work/gpon-$f.raw" > "$b/pon/odi_gpon-$f.txt"
		elif [ "$f" = status ] && [ "$full" = 0 ]; then
			sed 's/^\(pon_id .* id \)[0-9a-f]*/\1MASKED/' "$work/gpon-$f.raw" > "$b/pon/odi_gpon-$f.txt"
		else
			cp "$work/gpon-$f.raw" "$b/pon/odi_gpon-$f.txt"
		fi
		manifest "$rc" "$(size_of "$b/pon/odi_gpon-$f.txt")" "pon/odi_gpon-$f.txt"
	else
		manifest - - "pon/odi_gpon-$f.txt (absent: $GPON_PROC_DIR/$f)"
	fi
done

# ok once omcid gave the SIP secret list (empty or not).
sip_list=missing
if step pon/omcli_state.txt omcli state; then
	for c in provision flows tcont check conn vlan; do
		step "pon/omcli_$c.txt" omcli "$c"
	done
	# The phone lines an OLT provisioned, in the view that masks the number,
	# the display name and the login like the serial and never prints a
	# password. The secrets themselves go to the scrub list, below, so a
	# careless log line cannot give them away either.
	step pon/omcli_voip.txt omcli voip
	# omcli answers 0 with nothing to list when no voice is provisioned;
	# anything else, a part of a list included, is no list.
	if timeout "$STEP_TIMEOUT_S" omcli voip --secrets-list > "$work/voip.secrets" 2>/dev/null; then
		sip_list=ok
	else
		rm -f "$work/voip.secrets"
	fi
	for c in $OMCI_CLASSES; do
		step "pon/omcli_mib_$c.txt" omcli mib "$c"
	done
	# Outside the bundle until its check below has passed: whatever fails
	# on the way leaves it out.
	timeout "$STEP_TIMEOUT_S" omcli frames --pcap "$work/omci.pcap" > "$work/frames.out" 2>&1
	frames_rc=$?
else
	manifest - - "pon/omcli_* (skipped: omcid did not answer omcli state)"
fi

# module/: the optics module on /dev/i2c-0 (docs/TOOLS.md), to tell which
# laser driver the stick has and what state its calibration is in. A0h
# whole and A2h as found come first, with no write at all.
mkdir "$b/module"
step module/a0.txt i2cdump -y 0 0x50
step module/a2.txt i2cdump -y 0 0x51

# The module serial, A0h bytes 68-83, as text: blank, NUL and 0xff are
# padding, anything else unprintable is dropped. It joins the identity
# below, and unless --full its 16 bytes are blanked in a0.txt, hex and
# ASCII, as XX and X (how i2cdump shows a byte it could not read): the
# dump splits it over two lines, where no scrub rule would find it.
awk '
function h(c) { return index("0123456789abcdef", c) - 1 }
$1 == "40:" { for (i = 6; i <= 17; i++) s = s " " $i }
$1 == "50:" { for (i = 2; i <= 5; i++) s = s " " $i }
END {
	n = split(s, x, " ")
	for (i = 1; i <= n; i++) {
		if (x[i] !~ /^[0-9a-f][0-9a-f]$/)
			continue
		c = h(substr(x[i], 1, 1)) * 16 + h(substr(x[i], 2, 1))
		if (c > 32 && c < 127)
			v = v sprintf("%c", c)
		else if (c == 32 || c == 0 || c == 255)
			v = v " "
	}
	gsub(/^ +| +$/, "", v)
	if (v != "")
		print v
}' "$b/module/a0.txt" > "$work/module.serial"
if [ "$full" = 0 ]; then
	awk '
	# i2cdump: "40: " and 16 "hh ", three spaces, 16 ASCII characters.
	function blank(line, from, to,   j) {
		for (j = from; j <= to; j++) {
			line = substr(line, 1, 4 + 3 * j) "XX" substr(line, 7 + 3 * j)
			line = substr(line, 1, 55 + j) "X" substr(line, 57 + j)
		}
		return line
	}
	$1 == "40:" { $0 = blank($0, 4, 15) }
	$1 == "50:" { $0 = blank($0, 0, 3) }
	{ print }' "$b/module/a0.txt" > "$work/a0.blank" && mv "$work/a0.blank" "$b/module/a0.txt" ||
		fail "scrub failed: cannot mask the module serial in module/a0.txt"
fi

# The paged tables, in one short block: note A2h byte 127 (the page
# select), select each page in I2C_PAGES and read 127 back, since a chip
# can refuse a page and keep the one it had (a GN25L95 refuses 3), read
# the discriminators and dump the upper half of each page that took, then
# select the page as found again and read it back. The first failure ends
# the selects and goes straight to that restore; so does a signal, through
# the EXIT trap, which writes restore_page while it is set. The page
# select is the only write the kernel accepts on this bus (docs/TOOLS.md),
# and a flock keeps two bundles from interleaving their selects.
i2c() { timeout "$STEP_TIMEOUT_S" "$@" 2>&1; }
# gn25l95_table2: with page 2 selected on a GN25L95, the words of its
# configuration table that set the modulation, decoded: ER_CTRL (0xa5; bit 0
# is the automatic extinction-ratio loop, which lowers the modulation while
# the power-control loop raises the bias to compensate), MOD_DAC (0xab-0xac,
# most significant byte first), MOD_LIMIT (0xaf, 0xff = none) and PERSIST
# (0xc7; 0x03 keeps register changes out of the module EEPROM). Meanings from
# another open implementation's measurements on this module. Reads only.
gn25l95_table2() {
	er=$(i2c i2cget -y 0 0x51 0xa5) || er=
	hi=$(i2c i2cget -y 0 0x51 0xab) || hi=
	lo=$(i2c i2cget -y 0 0x51 0xac) || lo=
	lim=$(i2c i2cget -y 0 0x51 0xaf) || lim=
	per=$(i2c i2cget -y 0 0x51 0xc7) || per=
	case $er in
	0x??) [ $((er & 1)) = 1 ] && loop=on || loop=off
		echo "  ER_CTRL (0xa5): $er, automatic extinction-ratio loop $loop" ;;
	*) echo "  ER_CTRL (0xa5): unread" ;;
	esac
	case $hi$lo in
	0x??0x??) printf '  MOD_DAC (0xab-0xac): 0x%02x%02x\n' "$hi" "$lo" ;;
	*) echo "  MOD_DAC (0xab-0xac): unread" ;;
	esac
	case $lim in
	0xff) echo "  MOD_LIMIT (0xaf): 0xff, no limit" ;;
	0x??) echo "  MOD_LIMIT (0xaf): $lim" ;;
	*) echo "  MOD_LIMIT (0xaf): unread" ;;
	esac
	case $per in
	0x03) echo "  PERSIST (0xc7): 0x03, register changes stay out of the EEPROM" ;;
	0x??) echo "  PERSIST (0xc7): $per, register changes are copied to the EEPROM" ;;
	*) echo "  PERSIST (0xc7): unread" ;;
	esac
}

module_pages() {
	flock -n 9 || { echo "page work skipped: no page lock (another bundle is selecting pages, or flock failed)"; return 1; }
	p0=$(i2c i2cget -y 0 0x51 0x7f) || { echo "page select (A2h byte 127): unreadable: $p0"; return 1; }
	case $p0 in
	0x[0-9a-f][0-9a-f]) ;;
	*) echo "page select (A2h byte 127): unexpected: $p0"; return 1 ;;
	esac
	echo "page select (A2h byte 127) as found: $p0"
	restore_page=$p0
	for p in $I2C_PAGES; do
		i2c i2cset -y 0 0x51 0x7f "$p" > "$work/i2c.out" || { echo "select $p: failed: $(cat "$work/i2c.out")"; break; }
		got=$(i2c i2cget -y 0 0x51 0x7f) || { echo "select $p: read back failed: $got"; break; }
		if [ "$got" != "$(printf '0x%02x' "$p")" ]; then
			echo "select $p: reads back $got, did not take: page $p not shown"
			continue
		fi
		echo "select $p: reads back $got, page $p shown"
		case $p in
		2)
			for r in 0xa0 0x80 0xdc; do
				v=$(i2c i2cget -y 0 0x51 "$r") || v="failed: $v"
				echo "  page 2 byte $r: $v"
				[ "$r" = 0xa0 ] && id=$v
			done
			case $id in
			0x6a)
				echo "  laser driver: Semtech GN25L95 (page 2 byte 0xa0 = 0x6a)"
				gn25l95_table2
				;;
			0x??) echo "  laser driver: not a GN25L95 (page 2 byte 0xa0 is not 0x6a)" ;;
			*) echo "  laser driver: unknown (page 2 byte 0xa0 unread)" ;;
			esac
			;;
		3)
			v=$(i2c i2cget -y 0 0x51 0xf0) || v="failed: $v"
			echo "  page 3 byte 0xf0 (UX3320 INIT_STATE): $v"
			;;
		esac
		step "module/a2-p$p.txt" i2cdump -y -r 0x80-0xff 0 0x51 || break
	done
	i2c i2cset -y 0 0x51 0x7f "$restore_page" > "$work/i2c.out" || { echo "restore $restore_page: failed: $(cat "$work/i2c.out")"; return 1; }
	got=$(i2c i2cget -y 0 0x51 0x7f) || { echo "restore $restore_page: read back failed: $got"; return 1; }
	echo "page select restored: wrote $restore_page, reads back $got"
	[ "$got" = "$restore_page" ] || return 1
	restore_page=
}
module_pages > "$b/module/chip.txt" 2>&1 9> /tmp/odi-diag-i2c.lock
manifest "$?" "$(size_of "$b/module/chip.txt")" module/chip.txt

# /var/log: the tail of each file, newest lines being the ones that matter,
# within a total budget.
mkdir "$b/log"
left=$LOG_TOTAL_MAX
for f in "$LOG_DIR"/*; do
	[ -f "$f" ] || continue
	name=log/${f##*/}
	if [ "$left" -le 0 ]; then
		manifest - - "$name (skipped: LOG_TOTAL_MAX reached)"
		continue
	fi
	n=$LOG_FILE_MAX
	[ "$n" -gt "$left" ] && n=$left
	step "$name" tail -c "$n" "$f"
	left=$((left - $(size_of "$b/$name")))
done

# The config store, redacted by key. Each secret value found is also kept
# (outside the bundle) for the scrub below.
secrets=$work/secrets
: > "$secrets"
redact() {  # redact SRC DST
	# The key list on one line: a BSD awk refuses a -v value with a
	# newline in it, and the copy would then not be written at all.
	awk -v keys="$(printf '%s' "$SECRET_KEYS" | tr '\n' ' ')" -v words="$SECRET_WORDS" -v secrets="$secrets" '
	function secret(name,   i, n, w) {
		if (index(" " keys " ", " " name " "))
			return 1
		n = split(words, w, " ")
		for (i = 1; i <= n; i++)
			if (index(toupper(name), w[i]))
				return 1
		return 0
	}
	# The five entities xmlconfig writes, &amp; last (cs_value, rcs-lib.sh).
	function dec(v) {
		gsub(/&quot;/, "\"", v); gsub(/&apos;/, "\047", v)
		gsub(/&lt;/, "<", v); gsub(/&gt;/, ">", v); gsub(/&amp;/, "\\&", v)
		return v
	}
	BEGIN { gsub(/[ \t\n]+/, " ", keys) }
	# <Value Name="KEY" Value="value"/>, the xmlconfig line.
	match($0, /Name="[^"]*"/) {
		name = substr($0, RSTART + 6, RLENGTH - 7)
		if (secret(name) && match($0, /Value="[^"]*"/)) {
			val = substr($0, RSTART + 7, RLENGTH - 8)
			if (val != "") {
				print val >> secrets
				# As omcid and the logs see it, too (cfgstore.c).
				if (dec(val) != val)
					print dec(val) >> secrets
				$0 = substr($0, 1, RSTART - 1) "Value=\"REDACTED\"" substr($0, RSTART + RLENGTH)
			}
		}
		print
		next
	}
	# KEY=value, the odi.conf line.
	/^[A-Za-z_][A-Za-z0-9_]*=/ {
		eq = index($0, "=")
		name = substr($0, 1, eq - 1)
		val = substr($0, eq + 1)
		if (secret(name) && val != "") {
			print val >> secrets
			$0 = name "=REDACTED"
		}
	}
	{ print }' "$1" > "$2"
}
mkdir "$b/config"
for f in lastgood.xml lastgood_hs.xml odi.conf config-reseeded; do
	if [ -f "$CONFIG_DIR/$f" ]; then
		# A copy that fails can leave its secrets off the scrub list.
		redact "$CONFIG_DIR/$f" "$b/config/$f" || fail "scrub failed: cannot redact config/$f"
		manifest 0 "$(size_of "$b/config/$f")" "config/$f (redacted)"
	else
		manifest - - "config/$f (absent)"
	fi
done
# The web UI password: never collected, but scrubbed if a log quoted it.
if [ -f /etc/config/confd.auth ]; then
	cut -d: -f2- /etc/config/confd.auth > "$work/auth.pw" || fail "scrub failed: cannot read confd.auth"
	head -n 1 "$work/auth.pw" >> "$secrets" || fail "scrub failed: cannot list the web UI password"
	rm -f "$work/auth.pw"
fi

# The SIP credentials an OLT provisioned over OMCI (password, login, number,
# display name; and any login inside a server URL): one per line from omcli,
# scrubbed from every file like a secret of the config store. Only what
# omcli printed in full, a line at a time, is taken.
if [ -s "$work/voip.secrets" ]; then
	cat "$work/voip.secrets" >> "$secrets" || fail "scrub failed: cannot list the SIP secrets"
fi
# No list: phone lines off (OMCI_POTS_PORTS absent or 0, as omcid reads
# it: the first line for the key wins, as in flash and cfgstore.c) means
# no SIP credentials exist to scrub. Otherwise the files a
# careless log line could carry one into are withheld, not shipped.
sip_why=
if [ "$sip_list" = missing ]; then
	pots=$(sed -n 's/^OMCI_POTS_PORTS=//p' "$CONFIG_DIR/odi.conf" 2>/dev/null | head -n 1)
	case $pots in
	'' | 0) manifest - - "SIP secrets: no list from omcid; OMCI_POTS_PORTS is off, none to scrub" ;;
	*)
		sip_why="omcid gave no SIP secret list, so they could not be scrubbed"
		echo "diag-bundle: $sip_why; the logs and the pcap are left out" >&2
		for x in "$b"/log/*; do
			[ -f "$x" ] && withhold "log/${x##*/}" "$sip_why"
		done
		for x in syslog.txt dmesg.txt ramlog_prev.txt; do
			[ -f "$b/$x" ] && withhold "$x" "$sip_why"
		done
		;;
	esac
fi

# The identity: the IDENT_KEYS values from the config store, the serial in
# /proc/odi_gpon/status (16 hex digits; its first four bytes are the vendor
# id, so it is also turned into the text spelling the config uses), the MAC of
# every interface, and the module serial from A0h. Collected even with
# --full, which only skips the rules.
idents=$work/idents
: > "$idents"
for f in lastgood.xml lastgood_hs.xml odi.conf; do
	[ -f "$CONFIG_DIR/$f" ] || continue
	awk -v keys="$IDENT_KEYS" '
	function dec(v) {
		gsub(/&quot;/, "\"", v); gsub(/&apos;/, "\047", v)
		gsub(/&lt;/, "<", v); gsub(/&gt;/, ">", v); gsub(/&amp;/, "\\&", v)
		return v
	}
	BEGIN { gsub(/[ \t\n]+/, " ", keys) }
	match($0, /Name="[^"]*"/) {
		name = substr($0, RSTART + 6, RLENGTH - 7)
		if (index(" " keys " ", " " name " ") && match($0, /Value="[^"]*"/)) {
			v = substr($0, RSTART + 7, RLENGTH - 8)
			print v
			if (dec(v) != v)
				print dec(v)
		}
		next
	}
	/^[A-Za-z_][A-Za-z0-9_]*=/ {
		eq = index($0, "=")
		if (index(" " keys " ", " " substr($0, 1, eq - 1) " "))
			print substr($0, eq + 1)
	}' "$CONFIG_DIR/$f" >> "$idents" || fail "scrub failed: cannot list the identity in $f"
done
if [ -f "$work/gpon-status.raw" ]; then
	sed -n 's/^sn \([0-9a-f]\{16\}\)$/\1/p' "$work/gpon-status.raw" > "$work/gpon.sn" ||
		fail "scrub failed: cannot read the serial in the GPON status"
	awk '
	function h(c) { return index("0123456789abcdef", c) - 1 }
	{
		print
		v = ""
		for (i = 1; i < 8; i += 2)
			v = v sprintf("%c", h(substr($0, i, 1)) * 16 + h(substr($0, i + 1, 1)))
		if (v ~ /^[A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9]$/)
			print v toupper(substr($0, 9))
	}' "$work/gpon.sn" >> "$idents" || fail "scrub failed: cannot list the serial in the GPON status"
fi
# A MAC that cannot be read is not collected either; grep says 1 when the
# file is empty.
cat /sys/class/net/*/address > "$work/macs" 2>/dev/null
grep -v -x -i -e '00:00:00:00:00:00' -e 'ff:ff:ff:ff:ff:ff' "$work/macs" >> "$idents"
[ "$?" -le 1 ] || fail "scrub failed: cannot list the MACs"
if [ -f "$work/module.serial" ]; then
	cat "$work/module.serial" >> "$idents" || fail "scrub failed: cannot list the module serial"
fi

# The pcap, before the lists go: withheld when a value it should not carry
# is in it as text (see the header). Same lower bound as the scrub.
pcap=$work/omci.pcap
pcap_ok=0
if [ -f "$pcap" ]; then
	manifest "$frames_rc" "$(size_of "$pcap")" pon/omci.pcap
	# Withheld too when the values cannot be listed or a check cannot run:
	# a value nobody looked for is not shown to be absent.
	why=$sip_why
	if [ -n "$why" ]; then
		:
	elif ! { cat "$secrets" && { [ "$full" = 1 ] || cat "$idents"; }; } > "$work/pcap.list" ||
	   ! sort -u "$work/pcap.list" > "$work/pcap.values" ||
	   ! cat "$pcap" > /dev/null; then
		why="the check could not run"
	else
		# The loop ends 0 when every value was looked for, nonzero
		# when its list could not be opened.
		while IFS= read -r v; do
			[ "${#v}" -ge 3 ] || continue
			grep -q -F -e "$v" "$pcap"
			case $? in
			1) ;;
			0) why="a secret or identity value showed in it"; break ;;
			*) why="the check could not run"; break ;;
			esac
		done < "$work/pcap.values" || why="the check could not run"
	fi
	if [ -n "$why" ]; then
		withhold pon/omci.pcap "$why"
		rm -f "$pcap"
	else
		pcap_ok=1
	fi
	rm -f "$work/pcap.list" "$work/pcap.values"
elif [ -n "${frames_rc:-}" ]; then
	manifest "$frames_rc" - pon/omci.pcap
fi

# The scrub rules, as LENGTH TAB KIND TAB PATTERN TAB REPLACEMENT. They run
# longest first, so a value never loses a piece to a shorter one inside it.
# KIND is any (every occurrence), word (a whole token of text) or hex (a
# whole token of hex digits); see SHORT_LEN.
rules=$work/rules
: > "$rules"
# Every step from here to the scrub fails the bundle when it fails: a rule
# that was not written is a value that goes out as collected.
# An empty pattern is a helper (upper, lower, spaced) that failed. A tab
# would split the rule into the wrong columns and leave part of the value.
tab=$(printf '\t')
rule() {
	[ -n "$1" ] || fail "scrub failed: an empty rule"
	case $1 in *"$tab"*) fail "scrub failed: a value holds a tab" ;; esac
	printf '%d\t%s\t%s\t%s\n' "${#1}" "${3:-any}" "$1" "$2" >> "$rules" || fail "scrub failed: cannot write a rule"
}
# hexof VALUE: hx is VALUE in hex, two digits a byte. Checked, as tr hides
# an od that failed: a value would go without its hex rules.
hexof() {
	hx=$(printf '%s' "$1" | od -An -v -tx1 | tr -d ' \n')
	nb=$(printf '%s' "$1" | wc -c) || fail "scrub failed: cannot count a value"
	[ "${#hx}" -eq $((2 * nb)) ] || fail "scrub failed: cannot turn a value into hex"
	case $hx in *[!0-9a-f]*) fail "scrub failed: cannot turn a value into hex" ;; esac
}
# read_all LIST N: the loop over LIST read all its lines. A redirection
# that failed does not stop the shell, it skips the loop.
read_all() {
	lines=$(wc -l < "$1") || fail "scrub failed: cannot count ${1#"$work"/}"
	[ "$((lines))" = "$2" ] || fail "scrub failed: read $2 of $((lines)) lines of ${1#"$work"/}"
}
upper() { printf '%s' "$1" | tr 'a-z' 'A-Z'; }
lower() { printf '%s' "$1" | tr 'A-Z' 'a-z'; }
spaced() { printf '%s' "$1" | sed 's/../& /g; s/ $//'; }
# hexforms VALUE REPLACEMENT [KIND]: VALUE as hex, both cases, run together
# and space-separated (the kernel %ph spelling).
hexforms() {
	for x in "$1" "$(upper "$1")"; do
		rule "$x" "$2" "$3"
		[ "${#x}" -gt 2 ] && rule "$(spaced "$x")" "$2" "$3"
	done
}
# kinds VALUE: set tk and hk, the KIND of the text and of the hex rules.
kinds() {
	if [ "${#1}" -ge "$SHORT_LEN" ]; then
		tk=any hk=any
	else
		tk=word hk=hex
	fi
}

# Secrets: every value found, as text and as the hex of its bytes. Values
# shorter than three characters are redacted in the config copies above
# but not scrubbed from the rest: a two-character match would shred the
# logs and hide nothing worth hiding. Nor is a value made of one repeated
# character: the factory PLOAM password is twenty zeros, and scrubbing it
# replaced every run of zeros in the MIB dumps with REDACTED (seen in a
# reporter's bundle), while a password of all zeros is no secret to keep.
sort -u "$secrets" > "$work/secrets.sorted" || fail "scrub failed: cannot sort the secrets"
n=0
while IFS= read -r v; do
	n=$((n + 1))
	[ "${#v}" -ge 3 ] || continue
	first=${v%"${v#?}"}
	case "$v" in *[!"$first"]*) ;; *) continue ;; esac
	kinds "$v"
	rule "$v" REDACTED "$tk"
	hexof "$v"
	hexforms "$hx" REDACTED "$hk"
done < "$work/secrets.sorted"
read_all "$work/secrets.sorted" "$n"

# Identity, unless --full: MASKED- and the last four letters or digits, or
# MASKED alone for a value too short to give four away. The serial and the
# MACs are never short, but the LOID is free text from the ISP and can be:
# a LOID of 1234 scrubbed as a substring would cut digits out of every
# counter, so it gets the whole-token rule of a short secret.
if [ "$full" = 0 ]; then
	sort -u "$idents" > "$work/idents.sorted" || fail "scrub failed: cannot sort the identity values"
	n=0
	while IFS= read -r v; do
		n=$((n + 1))
		[ "${#v}" -ge 3 ] || continue
		bare=$(printf '%s' "$v" | tr -cd 'A-Za-z0-9')
		m=MASKED
		[ "${#bare}" -ge 10 ] && m=MASKED-${bare#"${bare%????}"}
		kinds "$v"
		for x in "$v" "$(upper "$v")" "$(lower "$v")"; do
			rule "$x" "$m" "$tk"
		done
		hexof "$v"
		hexforms "$hx" "$m" "$hk"
		# A MAC: every separator, both cases.
		if printf '%s' "$bare" | grep -q -x '[0-9A-Fa-f]\{12\}'; then
			lo=$(lower "$bare")
			for sep in : -; do
				x=$(printf '%s' "$lo" | sed "s/../&$sep/g; s/$sep\$//")
				rule "$x" "$m"
				rule "$(upper "$x")" "$m"
			done
			rule "$lo" "$m"
			rule "$(upper "$lo")" "$m"
		fi
		# A serial in its text spelling, vendor id and eight hex digits,
		# which tools print with the two halves in different cases (omcli
		# state: vendor id as set, digits lowercase); and the hex spelling
		# of the PLOAM messages and /proc/odi_gpon/status.
		if printf '%s' "$v" | grep -q -x '[A-Za-z0-9]\{4\}[0-9A-Fa-f]\{8\}'; then
			vid=$(printf '%s' "$v" | cut -c1-4)
			for x in "$vid" "$(upper "$vid")" "$(lower "$vid")"; do
				rule "$x$(lower "${v#????}")" "$m"
				rule "$x$(upper "${v#????}")" "$m"
			done
			hexof "$vid"
			hexforms "$hx$(lower "${v#????}")" "$m"
		fi
	done < "$work/idents.sorted"
	read_all "$work/idents.sorted" "$n"
fi

# README.md and SUMMARY.txt: the way into the bundle for someone who did
# not write it. Both are written here, before the scrub, so the scrub
# covers them like any other file. The summary reads only the files above.
readme() {
	img=$(sed -n 's/^image=//p' "$b/odi-build.txt" 2>/dev/null | head -n 1)
	if [ "$full" = 1 ]; then
		idline="NOT masked: this bundle was taken with --full and holds the serial number, the MACs and the LOID. Share it privately only."
	else
		idline="masked: the serial number, the MACs, the LOID and the module serial show as MASKED- and their last four characters."
	fi
	sipline=
	[ -n "$sip_why" ] && sipline="- Left out: the logs and the pcap, because $sip_why. Fetch /var/log/omcid.log by hand if it is needed, and send it privately, never to a public issue."
	cat <<-EOF
	# odi-oss diagnostics bundle

	Image: ${img:-unknown}. Taken: $(date -u '+%Y-%m-%d %H:%M:%S') UTC by the stick clock.

	Start with SUMMARY.txt: one screen of facts taken from the files below.
	MANIFEST.txt lists every step, its exit status and its size; a step
	with a non-zero status did not get its data.

	## What is hidden

	- Secrets: every password and key in the config store reads REDACTED,
	  and the same values are scrubbed from the other files, as text and hex.
	- Identity: $idline
	$sipline

	## Where to look

	| question | file |
	|---|---|
	| ONU state, alarms, GEM flows, port counters | pon/diag.txt, pon/odi_gpon-status.txt |
	| why the ONU left O5, LOS and LOF | dmesg.txt and syslog.txt, the event=onu_state, event=los and event=lof lines |
	| PLOAM messages and counts | pon/odi_gpon-ploam.txt |
	| OMCI: what the OLT provisioned, MIB resets | pon/omcli_*.txt, syslog.txt (event=mib_reset, event=unknown_me) |
	| optics: power, temperature, laser driver | pon/diag.txt (transceiver lines), module/ |
	| the previous boot, a watchdog reset | ramlog_prev.txt, odi_wdt_*.txt, nv.txt |
	| the host link to the router | host-link.txt |
	| settings | config/ (redacted) |
	| services and their logs | log/, ps.txt |
	| memory: which process grew, a full /var | meminfo.txt, rss.txt, df.txt |

	The event lines are documented in docs/TOOLS.md of odi-oss, in the
	section "Link and provisioning events".

	## What the bundle cannot tell

	A report needs these from the person who took it:

	- the ISP, and the line type (GPON, the OLT vendor if known)
	- the router behind the stick, and how it connects (VLAN, PPPoE or DHCP)
	- what was changed, and when, and what was seen (no sync, drops, slow)
	- whether the stock firmware works on the same line
	EOF
}
readme > "$b/README.md"
manifest - "$(size_of "$b/README.md")" README.md
timeout "$STEP_TIMEOUT_S" sh "$DIAG_SUMMARY_SH" "$b" > "$b/SUMMARY.txt" 2>&1
manifest "$?" "$(size_of "$b/SUMMARY.txt")" SUMMARY.txt

# A token rule is plain POSIX BRE, no \b, \< or alternation: a bound is a
# character outside the class, kept by \1 and \2, or the start or end of
# the line, each its own command. A match takes its bounds with it, so of
# two tokens one character apart the first pass replaces only one and the
# second pass the other.
script=$work/scrub.sed
sort -u "$rules" > "$work/rules.u" || fail "scrub failed: cannot sort the rules"
sort -t "$tab" -k1,1nr "$work/rules.u" > "$work/rules.s" || fail "scrub failed: cannot sort the rules"
while IFS="$tab" read -r _ kind pat rep; do
	p=$(printf '%s' "$pat" | sed 's/[]\/$*.^[]/\\&/g')
	[ -n "$p" ] || fail "scrub failed: cannot quote a rule"
	case $kind in
	word) c=A-Za-z0-9 ;;
	hex) c=0-9A-Fa-f ;;
	*) printf 's/%s/%s/g\n' "$p" "$rep"; continue ;;
	esac
	for _ in 1 2; do
		printf 's/\\([^%s]\\)%s\\([^%s]\\)/\\1%s\\2/g\n' "$c" "$p" "$c" "$rep"
	done
	printf 's/^%s\\([^%s]\\)/%s\\1/\n' "$p" "$c" "$rep"
	printf 's/\\([^%s]\\)%s$/\\1%s/\n' "$c" "$p" "$rep"
	printf 's/^%s$/%s/\n' "$p" "$rep"
done < "$work/rules.s" > "$script" || fail "scrub failed: cannot write the sed script"
# Exactly the commands the rules make (one for any, five for word and hex),
# or some were lost on the way (a full /tmp).
want=$(awk -F "$tab" '{ n += ($2 == "word" || $2 == "hex") ? 5 : 1 } END { print n + 0 }' "$work/rules.s") ||
	fail "scrub failed: cannot count the rules"
have=$(wc -l < "$script") || fail "scrub failed: cannot count the sed script"
[ "$((have))" = "$((want))" ] || fail "scrub failed: the sed script has $((have)) commands, the rules make $((want))"
if [ -s "$script" ]; then
	# Through a temporary file outside the bundle rather than sed -i, whose
	# argument differs between implementations (a BSD sed takes the -f
	# there as a backup suffix), so the scrub runs the same off the
	# device. The list is taken first, so the walk never meets a file the
	# loop writes. Any failure fails the bundle: a file the scrub could not
	# rewrite would otherwise go out as collected.
	find "$b" -type f > "$work/files" || fail "scrub failed: cannot list the bundle"
	n=0
	while IFS= read -r f; do
		n=$((n + 1))
		sed -f "$script" "$f" > "$work/scrub.out" || fail "scrub failed on ${f#"$work"/}"
		cat "$work/scrub.out" > "$f" || fail "scrub failed writing ${f#"$work"/}"
	done < "$work/files"
	read_all "$work/files" "$n"
	rm -f "$work/files" "$work/scrub.out"
fi
rm -f "$secrets" "$work/voip.secrets" "$work/frames.out" "$idents" "$rules" "$script" \
	"$work/secrets.sorted" "$work/idents.sorted" "$work/rules.u" "$work/rules.s" "$work"/gpon-*.raw "$work/module.serial" "$work/i2c.out"

# The pcap joins after the scrub, which would shift its records; only once
# its own check has passed (above).
if [ "$pcap_ok" = 1 ]; then
	mv "$pcap" "$b/pon/omci.pcap" || fail "cannot add pon/omci.pcap"
fi
tar -czf "$out.part" -C "$work" odi-diag || { rm -f "$out.part"; fail "tar failed"; }
n=$(size_of "$out.part")
if [ "${n:-0}" -gt "$BUNDLE_MAX" ]; then
	rm -f "$out.part"
	fail "the bundle came to $n bytes, over BUNDLE_MAX ($BUNDLE_MAX)"
fi
mv "$out.part" "$out" || fail "cannot write $out"
echo "$out"
