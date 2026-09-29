#!/bin/sh
#
# Back up a stick config store from a host, keeping a dated copy only when
# it changed.
#
# The provisioning identity (GPON serial, PLOAM password, LOID, MAC, VLAN)
# lives in one small jffs2 partition, and losing it is an outage that no
# reflash undoes. This pulls the same file the web UI Admin tab downloads,
# GET /api/backup on confd (odi-ui docs/API.md): both stores, every key, in
# clear -- so what it writes is as sensitive as the stick itself, and it is
# written mode 600 into a directory it creates mode 700.
#
# POSIX sh, curl and the usual text tools; runs on Linux and macOS. Every
# network step is bounded by curl -m, since macOS has no timeout(1).
#
# Environment:
#   ODI_HOST        the stick management address (required)
#   ODI_AUTH_FILE   a file holding user:password for the web UI, one line
#                   (the same shape as /etc/config/confd.auth on the stick)
#   ODI_USER, ODI_PASSWORD
#                   the same credential from the environment, when there is
#                   no ODI_AUTH_FILE
#   ODI_BACKUP_DIR  where copies go (default: ./odi-config/<ODI_HOST>)
#   ODI_KEEP        how many copies to keep (default: 30)
#   ODI_TIMEOUT     seconds for the whole download (default: 20)
#   ODI_PORT        confd port (default: 80, which odi-oss serves it on)
#
# Exit status: 0 when the backup was taken (new copy or unchanged), 1 when
# it could not be taken or stored, 2 on a usage error. stdout says which
# (and names any copy pruned); errors go to stderr. Never prints the
# credential.
#
#   ODI_HOST=192.168.1.1 ODI_AUTH_FILE=/etc/odi/ui.auth tools/config-backup.sh
#
# docs/SETTINGS.md ("Backing up from a host") has a cron and a systemd-timer
# example.
set -u

say() { printf 'config-backup: %s\n' "$*"; }
die() { printf 'config-backup: %s\n' "$1" >&2; exit "${2:-1}"; }

host=${ODI_HOST:-}
[ -n "$host" ] || die "set ODI_HOST to the stick address" 2
keep=${ODI_KEEP:-30}
timeout=${ODI_TIMEOUT:-20}
port=${ODI_PORT:-80}
dir=${ODI_BACKUP_DIR:-odi-config/$host}
case $keep in ''|*[!0-9]*|0) die "ODI_KEEP must be a positive number" 2 ;; esac
case $timeout in ''|*[!0-9]*|0) die "ODI_TIMEOUT must be a positive number of seconds" 2 ;; esac
case $port in ''|*[!0-9]*) die "ODI_PORT must be a number" 2 ;; esac

if [ -n "${ODI_AUTH_FILE:-}" ]; then
	[ -r "$ODI_AUTH_FILE" ] || die "cannot read ODI_AUTH_FILE $ODI_AUTH_FILE" 2
	cred=$(head -n 1 "$ODI_AUTH_FILE" | tr -d '\r\n')
elif [ -n "${ODI_USER:-}" ]; then
	cred=$ODI_USER:${ODI_PASSWORD:-}
else
	die "set ODI_AUTH_FILE, or ODI_USER and ODI_PASSWORD" 2
fi
case $cred in *:*) ;; *) die "the credential must be user:password" 2 ;; esac

command -v curl >/dev/null 2>&1 || die "curl is required"

umask 077
mkdir -p "$dir" || die "cannot create $dir"
chmod 700 "$dir" 2>/dev/null || true

tmp=$(mktemp "$dir/.incoming.XXXXXX") || die "cannot create a temporary file in $dir"
trap 'rm -f "$tmp" "$tmp.body" "$tmp.last"' EXIT
trap 'exit 1' HUP INT TERM

# The credential goes to curl on stdin as a config file (-K -), never on its
# command line, where any user on this host could read it from ps. Quotes and
# backslashes are escaped for the config-file string syntax.
esc=$(printf '%s' "$cred" | sed 's/[\\"]/\\&/g')
code=$(printf 'user = "%s"\n' "$esc" |
	curl -sS -K - -m "$timeout" --connect-timeout 5 \
		-o "$tmp" -w '%{http_code}' "http://$host:$port/api/backup") ||
	die "download from $host failed (curl exit $?, http ${code:-none})"
case $code in
200) ;;
401) die "$host refused the credential (http 401)" ;;
*) die "$host answered http $code to /api/backup" ;;
esac

# The backup body is the XML `flash all cs` and `flash all hs` print. Anything
# else -- an error page, an empty file -- is a failure, not a new copy.
grep -q '<Value Name=' "$tmp" || die "$host sent something that is not a config backup"

# confd opens the file with a comment naming its own build, which changes on
# every confd upgrade without any setting changing. Compare what follows it.
body() {
	awk 'NR == 1 && /^<!--/ { skip = 1 }
	     skip { if (/-->/) skip = 0; next }
	     { print }' "$1"
}

# The copies this script wrote, oldest first: the names sort by time.
copies() {
	for f in "$dir"/odi-config-[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]T[0-9][0-9][0-9][0-9][0-9][0-9]Z.xml; do
		[ -f "$f" ] && printf '%s\n' "${f##*/}"
	done | sort
}

last=$(copies | tail -n 1)
if [ -n "$last" ]; then
	body "$tmp" > "$tmp.body"
	body "$dir/$last" > "$tmp.last"
	if cmp -s "$tmp.body" "$tmp.last"; then
		say "unchanged since $last"
		exit 0
	fi
fi

name=odi-config-$(date -u +%Y%m%dT%H%M%SZ).xml
[ -e "$dir/$name" ] && die "$dir/$name already exists; run again in a second"
chmod 600 "$tmp"
mv "$tmp" "$dir/$name" || die "cannot store $dir/$name"
say "new copy $dir/$name"

# Keep the newest ODI_KEEP copies. Only names this script writes are
# counted or removed.
n=$(copies | wc -l | tr -d ' ')
if [ "$n" -gt "$keep" ]; then
	copies | head -n $((n - keep)) | while read -r old; do
		rm -f "$dir/$old" && say "pruned $old"
	done
fi
exit 0
