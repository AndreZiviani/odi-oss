#!/usr/bin/env bash
#
# rcs-lib.sh reads the PLOAM password from the config store XML the way omcid
# does (src/omci/respond/cfgstore.c): the five XML entities xmlconfig writes
# into a value are undone. A password with an ampersand (stored as a&amp;b)
# used to reach the driver with the entity still in it, which is a different
# password and fails to range, silently.
#
# Checked: cs_value on each entity, on every entity at once, on the order of
# the decoding (&amp;lt; is the text &lt;), on a plain hex value, on a
# missing key and file; and the gponpw step end to end, which must hand the
# driver the decoded text.
# shellcheck disable=SC2329 # the stubs are called by the sourced step
set -u
cd "$(dirname "$0")/.." || exit 1
LIB=$PWD/rootfs/skeleton/etc/scripts/rcs-lib.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0 fail=0
ok() { echo "ok    $1"; pass=$((pass + 1)); }
no() { echo "FAIL  $1"; printf '        wanted: %s\n        got:    %s\n' "$2" "$3"; fail=$((fail + 1)); }
eq() { if [ "$3" = "$2" ]; then ok "$1"; else no "$1" "$2" "$3"; fi; }

# shellcheck source=/dev/null
. "$LIB"

xml() {
	printf '<Config Name="ROOT">\n\t<Dir Name="MIB_TABLE">\n\t\t<Value Name="GPON_PLOAM_PASSWD" Value="%s"/>\n\t</Dir>\n</Config>\n' "$1" > "$T/cs.xml"
}
get() { xml "$1"; cs_value "$T/cs.xml" GPON_PLOAM_PASSWD; }

eq "plain hex is untouched" "3132333435" "$(get 3132333435)"
eq "&amp; is an ampersand" "a&b" "$(get 'a&amp;b')"
eq "&lt; and &gt;" "<x>" "$(get '&lt;x&gt;')"
eq "&quot;" 'a"b' "$(get 'a&quot;b')"
eq "&apos;" "a'b" "$(get 'a&apos;b')"
eq "every entity in one value" "a&b<c>d\"e'f" \
   "$(get 'a&amp;b&lt;c&gt;d&quot;e&apos;f')"
eq "&amp;lt; is the text &lt;, decoded once" "&lt;" "$(get '&amp;lt;')"
eq "a lone ampersand stays" "a&b" "$(get 'a&b')"
eq "an empty value is empty" "" "$(get '')"
eq "a missing key is empty" "" "$(cs_value "$T/cs.xml" NO_SUCH_KEY)"
eq "a missing file is empty" "" "$(cs_value "$T/none.xml" GPON_PLOAM_PASSWD)"

# The step itself: what pon_step is given for the driver.
xml 'a&amp;b'
out=$(
	# shellcheck disable=SC2034 # read by the sourced step
	CS=$T/cs.xml
	crumb() { :; }
	pon_step() { echo "step $1"; }
	pon_step_gponpw
)
eq "pon_step_gponpw hands the driver the decoded password" "step gponpw a&b" "$out"

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
