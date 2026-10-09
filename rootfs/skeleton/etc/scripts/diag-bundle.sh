#!/bin/sh
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
# (/proc/odi_ramlog_prev), /var/log/*, dmesg, /etc/odi-build, /etc/version,
# the readable /proc/odi_wdt files, uptime, /proc/meminfo, /proc/mounts,
# /proc/cmdline, ps, the U-Boot slot variables from both environment copies
# (nv getenv and nv fallback), a scrape of the exporter, the config store
# (lastgood.xml, lastgood_hs.xml, odi.conf) with every secret value
# replaced by REDACTED, and under pon/ the GPON and OMCI state: diag (ONU
# state, alarms, GEM flows, port counters, the PON queue registers that
# cmd 23 and 25 write), /proc/odi_gpon, /proc/odi_omci, and omcli (state,
# provision, flows, tcont, conn, and the MIB classes in OMCI_CLASSES).
# MANIFEST.txt lists each step, its exit status and its size.
#
# What is redacted, in the config copies: the keys in SECRET_KEYS below,
# and any key whose name contains one of the words in SECRET_WORDS. An empty
# value stays empty, so "not set" is still visible. Then every non-empty
# secret value found -- those keys, plus the web UI password from
# /etc/config/confd.auth -- is scrubbed from EVERY file in the bundle, as
# text, as hex and as space-separated hex, in case a log quoted one.
# confd.auth, the dropbear keys and /etc/passwd are never collected at all.
#
# What is masked: the serial number, the MACs and the LOID (IDENT_KEYS, the
# serial in /proc/odi_gpon, the interface MACs). Many OLTs authenticate by
# serial number alone, so a bundle posted in a public issue would otherwise
# hand out what it takes to clone the line. Each is replaced, in every file
# and in every spelling (text, hex, MAC with and without separators), by
# MASKED- and its last four characters, which still tells two sticks apart.
# --full keeps them, for a bundle shared privately; the web UI never passes
# it. The PLOAM ring in /proc/odi_gpon prints message bodies, so the bodies
# that carry the password or a key fragment are always withheld, and those
# that carry the serial number unless --full: what is left of a value cut
# by the ten-byte window would escape the scrub.
#
# The MIB classes are a list on purpose, not the whole MIB: an OLT can push
# credentials through OMCI (SIP user data, authentication methods, large
# strings, the TR-069 server), and none of that is in the classes below.
#
# Every step is bounded: a command through busybox timeout (STEP_TIMEOUT_S
# each, METRICS_TIMEOUT_S for the exporter scrape, which runs diag and
# omcicli behind it with bounds of their own of 3 s and 2 s), a log file by
# LOG_FILE_MAX bytes and all of /var/log together by LOG_TOTAL_MAX, and the
# archive by BUNDLE_MAX, past which it is refused rather than served. When
# omcid does not answer omcli state, the other omcli steps are skipped, so a
# hung omcid costs one timeout rather than one per step.

STEP_TIMEOUT_S=${STEP_TIMEOUT_S:-5}
METRICS_TIMEOUT_S=${METRICS_TIMEOUT_S:-10}
LOG_FILE_MAX=${LOG_FILE_MAX:-262144}
LOG_TOTAL_MAX=${LOG_TOTAL_MAX:-1048576}
BUNDLE_MAX=${BUNDLE_MAX:-2097152}
CONFIG_DIR=${CONFIG_DIR:-/var/config}
LOG_DIR=${LOG_DIR:-/var/log}
METRICS_URL=${METRICS_URL:-http://127.0.0.1:9100/metrics}

SECRET_KEYS="GPON_PLOAM_PASSWD LOID_PASSWD LOID_PASSWD_OLD USER_PASSWORD
SUSER_PASSWORD E8BDUSER_PASSWORD SUPER_PASSWORD MAC_KEY HW_FON_KEYWORD"
SECRET_WORDS="PASS PWD PSK SECRET TOKEN KEY COMMUNITY CRED"
IDENT_KEYS="GPON_SN ELAN_MAC_ADDR LOID LOID_OLD"
# ONU-G, ONU2-G, PPTP Ethernet UNI, MAC bridge service profile and port,
# VLAN tagging filter, 802.1p mapper, T-CONT, ANI-G, GEM interworking TP,
# GEM port network CTP, priority queue, traffic scheduler, traffic
# descriptor, multicast GEM interworking TP, multicast operations profile
# and subscriber config, VEIP, extended VLAN tagging operation.
OMCI_CLASSES="256 257 11 45 47 84 130 262 263 266 268 277 278 280 281 309 310 329 171"
GPON_PROC=${GPON_PROC:-/proc/odi_gpon}
PONQ_BASE=0xf020a8	# PONQ_COUNT_MASK +0, through +239 (docs/SWITCH.md)
PONQ_WORDS=240

full=0
if [ "$1" = --full ]; then
	full=1
	shift
fi
out=${1:-/tmp/odi-diag.tar.gz}
work=$(mktemp -d /tmp/odi-diag.XXXXXX) || { echo "diag-bundle: cannot create a work directory in /tmp" >&2; exit 1; }
b=$work/odi-diag
mkdir "$b"
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

fail() { echo "diag-bundle: $*" >&2; rm -f "$out"; exit 1; }

manifest() { printf '%-4s %8s  %s\n' "$1" "$2" "$3" >> "$b/MANIFEST.txt"; }
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

# pon/: the GPON and OMCI state. One diag run reads it all, batched on
# stdin; omcli asks omcid one question per run.
mkdir "$b/pon"
printf '%s\n' "gpon get onu-state" "gpon get alarm-status" "gpon get flows" \
	"mib dump counter port all" "register get $PONQ_BASE $PONQ_WORDS" |
	timeout "$STEP_TIMEOUT_S" diag > "$b/pon/diag.txt" 2>&1
manifest "$?" "$(size_of "$b/pon/diag.txt")" pon/diag.txt
grab pon/odi_omci.txt /proc/odi_omci

# The PLOAM ring prints ten bytes of each message body. Withheld always:
# Password (us 0x02) and Encryption_Key (us 0x05). Withheld unless --full,
# for the serial number in them: Serial_Number_ONU (us 0x01), Acknowledge
# (us 0x09, it echoes a downstream message), Serial_Number_Mask (ds 0x02),
# Assign_ONU-ID (ds 0x03) and Disable_Serial_Number (ds 0x06, 0x81).
if [ -e "$GPON_PROC" ]; then
	timeout "$STEP_TIMEOUT_S" cat "$GPON_PROC" > "$work/gpon.raw" 2>&1
	rc=$?
	awk -v full="$full" '
	$2 ~ /^(us|ds)$/ && $3 == "onu_id" && $5 == "type" && $7 == "content" {
		t = $2 " " $6
		if (t == "us 0x02" || t == "us 0x05" ||
		    (!full && (t == "us 0x01" || t == "us 0x09" || t == "ds 0x02" ||
			       t == "ds 0x03" || t == "ds 0x06" || t == "ds 0x81"))) {
			sub(/content .*/, "content (withheld)")
		}
	}
	{ print }' "$work/gpon.raw" > "$b/pon/odi_gpon.txt"
	manifest "$rc" "$(size_of "$b/pon/odi_gpon.txt")" pon/odi_gpon.txt
else
	manifest - - "pon/odi_gpon.txt (absent: $GPON_PROC)"
fi

if step pon/omcli_state.txt omcli state; then
	for c in provision flows tcont conn; do
		step "pon/omcli_$c.txt" omcli "$c"
	done
	for c in $OMCI_CLASSES; do
		step "pon/omcli_mib_$c.txt" omcli mib "$c"
	done
else
	manifest - - "pon/omcli_* (skipped: omcid did not answer omcli state)"
fi

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
	awk -v keys="$SECRET_KEYS" -v words="$SECRET_WORDS" -v secrets="$secrets" '
	function secret(name,   i, n, w) {
		if (index(" " keys " ", " " name " "))
			return 1
		n = split(words, w, " ")
		for (i = 1; i <= n; i++)
			if (index(toupper(name), w[i]))
				return 1
		return 0
	}
	BEGIN { gsub(/[ \t\n]+/, " ", keys) }
	# <Value Name="KEY" Value="value"/>, the xmlconfig line.
	match($0, /Name="[^"]*"/) {
		name = substr($0, RSTART + 6, RLENGTH - 7)
		if (secret(name) && match($0, /Value="[^"]*"/)) {
			val = substr($0, RSTART + 7, RLENGTH - 8)
			if (val != "") {
				print val >> secrets
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
for f in lastgood.xml lastgood_hs.xml odi.conf; do
	if [ -f "$CONFIG_DIR/$f" ]; then
		redact "$CONFIG_DIR/$f" "$b/config/$f"
		manifest "$?" "$(size_of "$b/config/$f")" "config/$f (redacted)"
	else
		manifest - - "config/$f (absent)"
	fi
done
# The web UI password: never collected, but scrubbed if a log quoted it.
if [ -f /etc/config/confd.auth ]; then
	cut -d: -f2- /etc/config/confd.auth | head -n 1 >> "$secrets"
fi

# The identity: the IDENT_KEYS values from the config store, the serial in
# /proc/odi_gpon (16 hex digits; its first four bytes are the vendor id, so
# it is also turned into the text spelling the config uses), and the MAC of
# every interface. Collected even with --full, which only skips the rules.
idents=$work/idents
: > "$idents"
for f in lastgood.xml lastgood_hs.xml odi.conf; do
	[ -f "$CONFIG_DIR/$f" ] || continue
	awk -v keys="$IDENT_KEYS" '
	BEGIN { gsub(/[ \t\n]+/, " ", keys) }
	match($0, /Name="[^"]*"/) {
		name = substr($0, RSTART + 6, RLENGTH - 7)
		if (index(" " keys " ", " " name " ") && match($0, /Value="[^"]*"/))
			print substr($0, RSTART + 7, RLENGTH - 8)
		next
	}
	/^[A-Za-z_][A-Za-z0-9_]*=/ {
		eq = index($0, "=")
		if (index(" " keys " ", " " substr($0, 1, eq - 1) " "))
			print substr($0, eq + 1)
	}' "$CONFIG_DIR/$f" >> "$idents"
done
if [ -f "$work/gpon.raw" ]; then
	sed -n 's/^sn \([0-9a-f]\{16\}\)$/\1/p' "$work/gpon.raw" | awk '
	function h(c) { return index("0123456789abcdef", c) - 1 }
	{
		print
		v = ""
		for (i = 1; i < 8; i += 2)
			v = v sprintf("%c", h(substr($0, i, 1)) * 16 + h(substr($0, i + 1, 1)))
		if (v ~ /^[A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9]$/)
			print v toupper(substr($0, 9))
	}' >> "$idents"
fi
cat /sys/class/net/*/address 2>/dev/null |
	grep -v -x -i -e '00:00:00:00:00:00' -e 'ff:ff:ff:ff:ff:ff' >> "$idents"

# The scrub rules, as LENGTH TAB PATTERN TAB REPLACEMENT. They run longest
# first, so a value never loses a piece to a shorter one inside it.
rules=$work/rules
: > "$rules"
rule() { printf '%d\t%s\t%s\n' "${#1}" "$1" "$2" >> "$rules"; }
hexof() { printf '%s' "$1" | od -An -v -tx1 | tr -d ' \n'; }
upper() { printf '%s' "$1" | tr 'a-z' 'A-Z'; }
lower() { printf '%s' "$1" | tr 'A-Z' 'a-z'; }
spaced() { printf '%s' "$1" | sed 's/../& /g; s/ $//'; }
# hexforms VALUE REPLACEMENT: VALUE as hex, both cases, run together and
# space-separated (the kernel %ph spelling).
hexforms() {
	for x in "$1" "$(upper "$1")"; do
		rule "$x" "$2"
		[ "${#x}" -gt 2 ] && rule "$(spaced "$x")" "$2"
	done
}

# Secrets: every value found, as text and as the hex of its bytes. Values
# shorter than three characters are redacted in the config copies above
# but not scrubbed from the rest: a two-character match would shred the
# logs and hide nothing worth hiding.
sort -u "$secrets" | while IFS= read -r v; do
	[ "${#v}" -ge 3 ] || continue
	rule "$v" REDACTED
	hexforms "$(hexof "$v")" REDACTED
done

# Identity, unless --full: MASKED- and the last four letters or digits, or
# MASKED alone for a value too short to give four away.
if [ "$full" = 0 ]; then
	sort -u "$idents" | while IFS= read -r v; do
		[ "${#v}" -ge 3 ] || continue
		bare=$(printf '%s' "$v" | tr -cd 'A-Za-z0-9')
		m=MASKED
		[ "${#bare}" -ge 10 ] && m=MASKED-${bare#"${bare%????}"}
		for x in "$v" "$(upper "$v")" "$(lower "$v")"; do
			rule "$x" "$m"
		done
		hexforms "$(hexof "$v")" "$m"
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
		# of the PLOAM messages and /proc/odi_gpon.
		if printf '%s' "$v" | grep -q -x '[A-Za-z0-9]\{4\}[0-9A-Fa-f]\{8\}'; then
			vid=$(printf '%s' "$v" | cut -c1-4)
			for x in "$vid" "$(upper "$vid")" "$(lower "$vid")"; do
				rule "$x$(lower "${v#????}")" "$m"
				rule "$x$(upper "${v#????}")" "$m"
			done
			hexforms "$(hexof "$vid")$(lower "${v#????}")" "$m"
		fi
	done
fi

script=$work/scrub.sed
sort -u "$rules" | sort -t "$(printf '\t')" -k1,1nr | while IFS="$(printf '\t')" read -r _ pat rep; do
	printf 's/%s/%s/g\n' "$(printf '%s' "$pat" | sed 's/[]\/$*.^[]/\\&/g')" "$rep"
done > "$script"
if [ -s "$script" ]; then
	find "$b" -type f | while IFS= read -r f; do
		sed -i -f "$script" "$f"
	done
fi
rm -f "$secrets" "$idents" "$rules" "$script" "$work/gpon.raw"

tar -czf "$out.part" -C "$work" odi-diag || { rm -f "$out.part"; fail "tar failed"; }
n=$(size_of "$out.part")
if [ "${n:-0}" -gt "$BUNDLE_MAX" ]; then
	rm -f "$out.part"
	fail "the bundle came to $n bytes, over BUNDLE_MAX ($BUNDLE_MAX)"
fi
mv "$out.part" "$out" || fail "cannot write $out"
echo "$out"
