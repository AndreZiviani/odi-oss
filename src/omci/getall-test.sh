#!/bin/sh
# A Get with every attribute bit set (respond/omcimsg.c, the Get handler): the
# answer stays inside the 32-byte baseline contents, and says honestly what it
# left out.
#
# The report that prompted it: another open implementation found its Get
# wrote past the answer buffer for a class with several table attributes (a
# voice service profile has four after nine plain ones), because a table
# attribute answers with its 4-byte size and the size did not fit. omcid has
# no such overflow (the values stop at contents byte 27 and a table Get is
# answered on its own, 7 bytes), and this suite is what keeps it so: for
# every class of the model that has a table or other big attribute, and every
# class whose plain attributes do not all fit in 25 bytes, it sends a Get of
# attributes 1-16 and checks the answer against the shape G.988 gives it,
# worked out from the generated model (generated/omci_mib.c), not from omcid:
#
#   - a class with a big attribute answers the first one alone: result 0, the
#     mask with that one bit, its length as 4 bytes (high half zero), nothing
#     after (the table is then read by Get-Next, tables-test.sh);
#   - any other Get answers the attributes that fit in 25 bytes, in mask order,
#     under the mask answered, and everything after the first that does not
#     fit is named in the failed mask, the ones the class lacks in the
#     unsupported mask, as result 9 (the two masks at contents 28 and 30);
#     the bytes between the last value and the masks are zero.
#
# The same Get is also sent with the big attributes taken out of the mask, so
# the plain ones that follow a table in the class are covered, and the sizes
# of populated tables are read back (309 and the voice service profile).
#
# Run from src/omci, inside the toolchain image (make test-omci-getall):
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci "$(../../toolchain/image.sh diag)" \
#          sh getall-test.sh
set -u
Q=qemu-mips-static
fail=0
. ./testlib.sh

check() {
	if [ "$2" = "$3" ]; then
		echo "ok    $1"
	else
		echo "FAIL  $1"
		echo "--- expected"; printf '%s\n' "$3"
		echo "--- got";      printf '%s\n' "$2"
		fail=$((fail + 1))
	fi
}

frame() {
	_c=$5
	while [ "${#_c}" -lt 64 ]; do _c="${_c}0"; done
	printf '%s%s0a%s%s%s0000002800000000' "$1" "$2" "$3" "$4" "$_c"
}
answer() {
	grep -- "^-> $2" "$1" | tail -1 | cut -c20-83
}
send() {
	inject_wait "$1" "$(frame "$2" "$3" "$4" "$5" "$6")"
}

# The cases, from the model: one line per Get, "class name mask prefix skip
# tail". prefix is the leading contents bytes that must match exactly, skip the
# bytes after it that hold values (not checked), tail every byte after those.
cases() {
awk '
function hex(v, n,   s) { s = sprintf("%0" n "x", v); return s }
function zeros(n,   s) { s = ""; while (n-- > 0) s = s "00"; return s }
function sim(cls, name, na, mask,   k, bit, n, ans, uns, fl, w, b, first) {
	# a big attribute is answered alone
	for (k = 1; k <= 16; k++) {
		bit = 2 ^ (16 - k)
		if (int(mask / bit) % 2 && k < na && B[cls, k]) {
			print cls, name, hex(mask, 4), "00" hex(bit, 4) "0000", 2, zeros(25)
			return
		}
	}
	n = 3; ans = 0; uns = 0; fl = 0
	for (k = 1; k <= 16; k++) {
		bit = 2 ^ (16 - k)
		if (!(int(mask / bit) % 2)) continue
		w = (k < na) ? W[cls, k] : 0
		if (!w) { uns += bit; continue }
		if (fl || n + w > 28) { fl += bit; continue }
		ans += bit; n += w
	}
	if (uns || fl)
		print cls, name, hex(mask, 4), "09" hex(ans, 4), n - 3, zeros(28 - n) hex(uns, 4) hex(fl, 4)
	else
		print cls, name, hex(mask, 4), "00" hex(ans, 4), n - 3, zeros(28 - n) "00000000"
}
/^static const struct omci_attr attr_[A-Za-z0-9_]*\[\] = \{/ {
	cur = $5; sub(/^attr_/, "", cur); sub(/\[\]/, "", cur); i = 0; next
}
cur != "" && /^\};/ { cur = ""; next }
cur != "" && /^\t\{ "/ {
	s = $0; gsub(/[{},"]/, "", s); split(s, a, " ")
	w = a[3]; if (a[2] == "OMCI_STRING" && w > 0) w--
	AW[cur, i] = w; AB[cur, i] = (a[2] == "OMCI_OCTETS" || w >= 26) ? 1 : 0
	i++; next
}
/^\t\{ +[0-9]+, "/ {
	s = $0; gsub(/[{},"]/, "", s); split(s, a, " ")
	cls = a[1] + 0; name = a[2]; arr = a[4]; sub(/^attr_/, "", arr); na = a[5] + 0
	nb = 0; sum = 0; bigmask = 0
	for (k = 1; k <= 16 && k < na; k++) {
		W[cls, k] = AW[arr, k]; B[cls, k] = AB[arr, k]
		if (B[cls, k]) { nb++; bigmask += 2 ^ (16 - k) }
		else sum += W[cls, k]
	}
	if (nb == 0 && sum <= 25) next
	sim(cls, name, na, 65535)
	if (nb) sim(cls, name, na, 65535 - bigmask)
}' generated/omci_mib.c
}

mkdir -p /var/config /var/run /var/log
rm -f /var/config/odi.conf /var/config/lastgood.xml /var/config/lastgood_hs.xml \
      /var/config/omci-identity.on /var/run/omcid-mib.snap /var/run/omcid-resume-decision
# Voice classes (the voice service profile among them) exist only with phone
# ports on.
printf 'OMCI_POTS_PORTS=1\n' > /var/config/odi.conf
printf 'state 5 (O5)\nonu_id 1\nsn 4f44495400000001\nalloc_ids 0\n' > /tmp/getall-gpon

log=/tmp/omcid-getall.log
: > "$log"
$Q respond/build/omcid -d -g /tmp/getall-gpon > "$log" 2>&1 &
pid=$!
omcid_up "$log"
send "$log" 0001 4f 0002 0000 ""                         # MIB reset

list=/tmp/getall-cases
cases > "$list"
total=$(wc -l < "$list")
check "the model has classes with several table attributes to cover (at least 8)" \
      "$([ "$(awk '{n[$1]++} END{c=0; for (k in n) if (n[k] == 2) c++; print c}' "$list")" -ge 8 ] && echo yes || echo no)" yes
echo "covering $total Gets"

t=0
while read -r cls name mask pfx skip tail; do
	t=$((t + 1))
	tci=$(printf 'c%03x' "$t")
	c4=$(printf '%04x' "$cls")
	send "$log" "$tci" 49 "$c4" 0001 "$mask"
	got=$(answer "$log" "$tci")
	p=${#pfx}
	# the bytes of values are not compared: replace them by what was answered
	vals=""
	[ "$skip" -gt 0 ] && vals=$(printf '%s' "$got" | cut -c$((p + 1))-$((p + skip * 2)))
	want="$pfx$vals$tail"
	check "class $cls $name, Get of $mask: shape of the answer" "$got" "$want"
done < "$list"

# --- sizes of populated tables ---------------------------------------------
# 309: two dynamic ACL rows are 48 bytes; its static table (attribute 8) is
# empty. The lowest table attribute in the mask is the one answered.
R1=000100fa0064e0000001e00000ff0000000000000000
send "$log" 0010 44 0135 0001 ""
send "$log" 0011 48 0135 0001 "0200$(printf '4001%s' "${R1#????}")"
send "$log" 0012 48 0135 0001 "0200$(printf '4002%s' "${R1#????}")"
send "$log" 0013 49 0135 0001 ffff
check "309: Get of every attribute answers the dynamic ACL's size, 48 bytes, alone" \
      "$(answer "$log" 0013 | cut -c1-14)" "00020000000030"
send "$log" 0014 49 0135 0001 fdff
check "309: without attribute 7 the static ACL is answered, empty" \
      "$(answer "$log" 0014 | cut -c1-14)" "00010000000000"

# Voice service profile: a row in the tone pattern table (20 bytes) and one in
# the tone event table (7 bytes).
send "$log" 0020 44 003a 0001 ""
send "$log" 0021 48 003a 0001 "0040$(printf '01%038d' 0)"     # 20 bytes
send "$log" 0022 48 003a 0001 "0020$(printf '01%012d' 0)"      # 7 bytes
send "$log" 0023 49 003a 0001 ffff
check "voice service profile: Get of every attribute answers the tone pattern size, 20 bytes, alone" \
      "$(answer "$log" 0023 | cut -c1-14)" "00004000000014"
send "$log" 0024 49 003a 0001 ffbf
check "voice service profile: without the tone patterns the tone events are answered, 7 bytes" \
      "$(answer "$log" 0024 | cut -c1-14)" "00002000000007"

check "omcid is still serving after the sweep" \
      "$(kill -0 "$pid" 2>/dev/null && echo up || echo down)" up
omcid_stop "$pid"

[ "$fail" -eq 0 ] || keep_log "$log" getall-test.log
if [ "$fail" -eq 0 ]; then echo "getall-test.sh: all checks passed"; else echo "FAILED ($fail)"; fi
exit "$fail"
