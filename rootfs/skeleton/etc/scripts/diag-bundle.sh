#!/bin/sh
#
# diag-bundle.sh [OUT] -- collect what a bug report about this stick needs
# into one tar.gz, with every secret redacted. The web UI serves it as
# GET /api/diag (odi-ui confd runs this and streams OUT); it is as usable
# from a shell:
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
# (nv getenv and nv fallback), a scrape of the exporter, and the config
# store (lastgood.xml, lastgood_hs.xml, odi.conf) with every secret value
# replaced by REDACTED. MANIFEST.txt lists each step, its exit status and
# its size.
#
# What is redacted, in the config copies: the keys in SECRET_KEYS below,
# and any key whose name contains one of the words in SECRET_WORDS. An empty
# value stays empty, so "not set" is still visible. Then every non-empty
# secret value found -- those keys, plus the web UI password from
# /etc/config/confd.auth -- is scrubbed from EVERY file in the bundle, as
# text and as hex, in case a log quoted one. confd.auth, the dropbear keys
# and /etc/passwd are never collected at all. The serial number, the MAC
# and the LOID itself are kept: they identify the stick but authenticate
# nothing without the passwords.
#
# Every step is bounded: a command through busybox timeout (STEP_TIMEOUT_S
# each, METRICS_TIMEOUT_S for the exporter scrape, which runs diag and
# omcicli behind it with bounds of their own of 3 s and 2 s), a log file by LOG_FILE_MAX bytes and all of /var/log together by
# LOG_TOTAL_MAX, and the archive by BUNDLE_MAX, past which it is refused
# rather than served.

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

out=${1:-/tmp/odi-diag.tar.gz}
work=$(mktemp -d /tmp/odi-diag.XXXXXX) || { echo "diag-bundle: cannot create a work directory in /tmp" >&2; exit 1; }
b=$work/odi-diag
mkdir "$b"
trap 'rm -rf "$work"' EXIT
trap 'exit 1' HUP INT TERM

fail() { echo "diag-bundle: $*" >&2; rm -f "$out"; exit 1; }

manifest() { printf '%-4s %8s  %s\n' "$1" "$2" "$3" >> "$b/MANIFEST.txt"; }
size_of() { wc -c < "$1" 2>/dev/null | tr -d ' '; }

# step NAME CMD ARGS...: run CMD under timeout into odi-diag/NAME.
step() {
	name=$1
	shift
	timeout "$STEP_TIMEOUT_S" "$@" > "$b/$name" 2>&1
	manifest "$?" "$(size_of "$b/$name")" "$name"
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

# Scrub every secret value from every file, as text and as the lowercase
# and uppercase hex of its bytes. Values shorter than three characters are
# redacted in the config copies above but not scrubbed from the rest: a
# two-character match would shred the logs and hide nothing worth hiding.
script=$work/scrub.sed
: > "$script"
sort -u "$secrets" | while IFS= read -r s; do
	[ "${#s}" -ge 3 ] || continue
	hex=$(printf '%s' "$s" | od -An -v -tx1 | tr -d ' \n')
	for v in "$s" "$hex" "$(echo "$hex" | tr 'a-f' 'A-F')"; do
		printf 's/%s/REDACTED/g\n' "$(printf '%s' "$v" | sed 's/[]\/$*.^[]/\\&/g')" >> "$script"
	done
done
if [ -s "$script" ]; then
	find "$b" -type f | while IFS= read -r f; do
		sed -i -f "$script" "$f"
	done
fi
rm -f "$secrets" "$script"

tar -czf "$out.part" -C "$work" odi-diag || { rm -f "$out.part"; fail "tar failed"; }
n=$(size_of "$out.part")
if [ "${n:-0}" -gt "$BUNDLE_MAX" ]; then
	rm -f "$out.part"
	fail "the bundle came to $n bytes, over BUNDLE_MAX ($BUNDLE_MAX)"
fi
mv "$out.part" "$out" || fail "cannot write $out"
echo "$out"
