#!/bin/sh
# The VoIP classes (G.988 9.9, SIP over OMCI): what an OLT provisions for a
# phone line, accepted and held while OMCI_POTS_PORTS is on, and unknown to
# the OLT, exactly as before they were modelled, while it is off.
#
# Covered, with the key off: a Get, Create, Set and Test of a VoIP class are
# answered as for a class omcid has no model of, and the MIB upload is the
# same. With the key on (a synthetic provisioning, every value made up): the
# large strings, authentication method and network address the servers and
# the number live in, TCP/UDP config data, the SIP agent (its Create, the
# G.988 defaults of what the Create does not carry, the SIP response table),
# the dial plan table (add, remove, its row count), the voice service
# profile's four tables (rows by index, index 0 clears, a Get of the whole
# mask answered with the first table's size), the RTP and media profiles,
# the application service profile, the feature access codes, the SIP user
# (the display name kept out of the log and masked in a dump), the voice CTP
# and a PM entity; the ONU-created classes refused a Create; a respawn that
# resumes it all from the snapshot; a MIB reset that drops it.
#
# Run from src/omci, inside the toolchain image (make test-omci-voip):
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci "$(../../toolchain/image.sh diag)" \
#          sh voip-test.sh
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

# frame <tci> <type> <class> <inst> <contents hex>: one baseline frame, the
# contents padded with zeros to 32 bytes; trailer, zero CRC (not checked).
frame() {
	_c=$5
	while [ "${#_c}" -lt 64 ]; do _c="${_c}0"; done
	printf '%s%s0a%s%s%s0000002800000000' "$1" "$2" "$3" "$4" "$_c"
}

# answer <log> <tci>: the contents of the last answer to that tci (64 hex).
answer() {
	grep -- "^-> $2" "$1" | tail -1 | cut -c20-83
}

send() {
	inject_wait "$1" "$(frame "$2" "$3" "$4" "$5" "$6")"
}

# table <log> <tci base> <class> <inst> <mask>: Get the table attribute, then
# walk it with Get-Next; prints the table bytes as hex, empty when it is empty.
table() {
	_l=$1; _b=$2; _cl=$3; _in=$4; _m=$5
	send "$_l" "${_b}0" 49 "$_cl" "$_in" "$_m"
	_a=$(answer "$_l" "${_b}0")
	_len=$((0x$(echo "$_a" | cut -c7-14)))
	_out=""
	_seq=0
	while [ $((_seq * 29)) -lt "$_len" ]; do
		send "$_l" "${_b}$((_seq + 1))" 5a "$_cl" "$_in" "$_m$(printf '%04x' "$_seq")"
		_n=$((_len - _seq * 29))
		[ "$_n" -gt 29 ] && _n=29
		_out="$_out$(answer "$_l" "${_b}$((_seq + 1))" | cut -c7-$((6 + _n * 2)))"
		_seq=$((_seq + 1))
	done
	printf '%s' "$_out"
}

# pad <hex> <bytes>: the hex padded with zeros to that many bytes.
pad() {
	_p=$1
	while [ "${#_p}" -lt $(($2 * 2)) ]; do _p="${_p}0"; done
	printf '%s' "$_p"
}

hexof() { printf '%s' "$1" | od -An -tx1 | tr -d ' \n'; }

# part <hex> <n>: the n-th 25-byte part of a string, padded.
part() {
	_s=$(printf '%s' "$1" | cut -c$(( ($2 - 1) * 50 + 1 ))-$(( $2 * 50 )))
	pad "$_s" 25
}

# leaks <file> <value> <hex of it>: lines carrying it as text, hex or spaced
# hex (the log prints raw contents a byte at a time).
leaks() {
	grep -c -i -e "$2" -e "$3" -e "$(printf '%s' "$3" | sed 's/../& /g;s/ $//')" "$1" || true
}

# upload <log>: the record count a MIB upload announces.
upload() {
	_n=$(grep -c "upload records" "$1")
	$Q cli/build/omcli --inject-file /tmp/voip-upload.txt > /dev/null 2>&1
	wait_for "$1" "upload records" 10 $((_n + 1))
	sed -n 's/^-> \([0-9]*\) upload records.*/\1/p' "$1" | tail -1
}
printf '0d014d0a00020000%064d00000028%08d\n' 0 0 > /tmp/voip-upload.txt

mkdir -p /var/config /var/run /var/log
rm -f /var/config/odi.conf /var/config/lastgood.xml /var/config/lastgood_hs.xml \
      /var/config/omci-identity.on /var/run/omcid-mib.snap /var/run/omcid-resume-decision
printf 'state 5 (O5)\nonu_id 1\nsn 4f44495400000001\nalloc_ids 0\n' > /tmp/voip-gpon

# --------------------------------------------------------------- key off
# Every answer is the one a class without a model gets: result 4, unknown
# managed entity, and nothing else in the frame; the Test of a class the ONU
# does not hold keeps its old answer, success with no result after it.
UNKNOWN=04$(printf '%062d' 0)
log=/tmp/omcid-voip-off.log
$Q respond/build/omcid -d -g /tmp/voip-gpon > "$log" 2>&1 &
pid=$!
omcid_up "$log"
send "$log" 0001 4f 0002 0000 ""                         # MIB reset
send "$log" 0002 49 008a 0000 ff00                       # Get 138
check "key off: a Get of the VoIP config data is unknown entity" \
      "$(answer "$log" 0002)" "$UNKNOWN"
send "$log" 0003 44 008b 0001 "00010201000101"           # Create 139
check "key off: a Create of a voice CTP is unknown entity" \
      "$(answer "$log" 0003)" "$UNKNOWN"
send "$log" 0004 48 0099 0001 "2000$(pad 41 25)"          # Set 153
check "key off: a Set of a SIP user is unknown entity" \
      "$(answer "$log" 0004)" "$UNKNOWN"
send "$log" 0005 52 0035 0201 ""                         # Test 53
check "key off: a Test of a POTS UNI keeps the old answer for an unknown class" \
      "$(answer "$log" 0005)" "00$(printf '%062d' 0)"
check "key off: the log names the class as unknown" \
      "$(grep -c 'class 139 *(unknown) ' "$log")" "1"
check "key off: and records it as an unknown entity" \
      "$(grep -c '^event=unknown_me class=139 op=create ' "$log")" "1"
check "key off: the MIB upload announces 399 records, as the stock ONU" \
      "$(upload "$log")" "399"
check "key off: no phone line in the startup log" "$(grep -c '^phone:' "$log")" "0"
omcid_stop "$pid"

# ---------------------------------------------------------------- key on
printf 'OMCI_POTS_PORTS=1\n' > /var/config/odi.conf
log=/tmp/omcid-voip.log
$Q respond/build/omcid -d -g /tmp/voip-gpon > "$log" 2>&1 &
pid=$!
omcid_up "$log"
check "key on: the startup log says so" \
      "$(grep -c '^phone: 1 POTS port(s), the VoIP entities accepted' "$log")" "1"
send "$log" 0010 4f 0002 0000 ""                         # MIB reset
check "key on: nothing is advertised yet, the upload is the same 399 records" \
      "$(upload "$log")" "399"

# Large strings: the proxy (60 bytes, three parts), the registrar and the
# number. A Set carries the number of parts and part 1, later parts one by
# one.
PROXY='sip:proxy.voice.example.net:5060;transport=udp;lr;maddr=10.0'
PROXYX=$(hexof "$PROXY")
REG=registrar.example.net;  REGX=$(hexof "$REG")
AOR=5511912345678;          AORX=$(hexof "$AOR")
check "the proxy string is 60 bytes" "${#PROXY}" "60"
send "$log" 0020 44 009d 0001 ""
send "$log" 0021 48 009d 0001 "c00003$(part "$PROXYX" 1)"
send "$log" 0022 48 009d 0001 "2000$(part "$PROXYX" 2)"
send "$log" 0023 48 009d 0001 "1000$(part "$PROXYX" 3)"
send "$log" 0024 44 009d 0002 ""
send "$log" 0025 48 009d 0002 "c00001$(part "$REGX" 1)"
send "$log" 0026 44 009d 0003 ""
send "$log" 0027 48 009d 0003 "c00001$(part "$AORX" 1)"
bad=0
for k in 1 2 3; do
	m=$(printf '%04x' $((1 << (15 - k))))
	send "$log" "003$k" 49 009d 0001 "$m"
	[ "$(answer "$log" "003$k" | cut -c1-56)" = "00$m$(part "$PROXYX" "$k")" ] || bad=$k
done
check "the three parts of the proxy read back, one per Get" "$bad" "0"

# The SIP credentials: an authentication method, scheme and username 1 in one
# Set (26 bytes), then the password and the realm.
USR=5511912345678@voip;  USRX=$(hexof "$USR")
PW=Sip-Pa55word;         PWX=$(hexof "$PW")
send "$log" 0040 44 0094 0003 ""
send "$log" 0041 48 0094 0003 "c00001$(pad "$USRX" 25)"
send "$log" 0042 48 0094 0003 "2000$(pad "$PWX" 25)"
send "$log" 0043 48 0094 0003 "1000$(pad "$(hexof example.net)" 25)"
check "the password Set is answered ok" "$(answer "$log" 0042 | cut -c1-2)" "00"

# The registrar's network address (8001): security pointer to the
# authentication method, address pointer to its large string.
send "$log" 0050 44 0089 8001 "00030002"
check "a network address for the registrar is created" "$(answer "$log" 0050 | cut -c1-2)" "00"
# TCP/UDP config data: port 5060, UDP, no TOS, IP host 0.
send "$log" 0051 44 0088 0001 "13c411000000"
check "the TCP/UDP config data is created" "$(answer "$log" 0051 | cut -c1-2)" "00"

# ------------------------------------------------- SIP agent config data
# The Create carries every set-by-create attribute in order, 24 bytes:
# proxy, outbound proxy, the two DNS, host part, registrar, softswitch,
# option transmit, URI format, redundant agent.
send "$log" 0060 44 0096 0001 "0001ffff0000000000000000ffff8001000000000001ffff"
check "the SIP agent is created from its 24-byte Create" "$(answer "$log" 0060 | cut -c1-2)" "00"
send "$log" 0061 49 0096 0001 f000
check "its proxy, outbound proxy and DNS read back" \
      "$(answer "$log" 0061 | cut -c1-30)" "00f0000001ffff0000000000000000"
send "$log" 0062 49 0096 0001 0060
check "its registrar and softswitch read back" \
      "$(answer "$log" 0062 | cut -c1-18)" "000060800100000000"
send "$log" 0063 49 0096 0001 0e00
check "what the Create does not carry is G.988's default: no TCP/UDP, 3600 s, 360 s" \
      "$(answer "$log" 0063 | cut -c1-26)" "000e00ffff00000e1000000168"
send "$log" 0064 48 0096 0001 08000001
send "$log" 0065 49 0096 0001 0800
check "the TCP/UDP pointer is then set" "$(answer "$log" 0065 | cut -c1-10)" "0008000001"
send "$log" 0066 49 0096 0001 0080
check "the SIP status is 0, ok/initial: the stick registers nothing" \
      "$(answer "$log" 0066 | cut -c1-8)" "00008000"
# The SIP response table: rows keyed by the response code, code 0 clears.
S486=01e601ffff; S404=019402ffff; S486B=01e603ffff
send "$log" 0067 48 0096 0001 "0010$S486"
send "$log" 0068 48 0096 0001 "0010$S404"
send "$log" 0069 48 0096 0001 "0010$S486B"
check "SIP responses: keyed by code, a second 486 replaces the first" \
      "$(table "$log" b01 0096 0001 0010)" "$S486B$S404"
send "$log" 006a 48 0096 0001 "00100000000000"
check "SIP responses: code 0 clears the table" "$(table "$log" b02 0096 0001 0010)" ""
send "$log" 006b 48 0096 0001 "0010$S404"

# ------------------------------------------------------ dial plan table
# Create: the table's maximum size, the critical and partial timeouts, the
# format. Rows are 30 bytes: id, action (1 add, 0 remove), the token.
send "$log" 0070 44 0091 0001 "00100fa03e8001"
D1=$(pad "0101$(hexof 0T)" 30)
D2=$(pad "0201$(hexof '*xx')" 30)
D3=$(pad "0301$(hexof '[2-9]xxxxxxx')" 30)
send "$log" 0071 48 0091 0001 "0400$D1"
send "$log" 0072 48 0091 0001 "0400$D2"
send "$log" 0073 48 0091 0001 "0400$D3"
check "dial plan: three rows, newest first" \
      "$(table "$log" b03 0091 0001 0400)" "$D3$D2$D1"
send "$log" 0074 48 0091 0001 "0400$(pad 0100 30)"
check "dial plan: action 0 removes that id" \
      "$(table "$log" b04 0091 0001 0400)" "$D3$D2"
send "$log" 0075 49 0091 0001 8000
check "dial plan: the dial plan number is the rows held" \
      "$(answer "$log" 0075 | cut -c1-10)" "0080000002"

# ------------------------------------------------ voice service profile
# Create: announcement, jitter target and maximum, echo cancel, PSTN
# variant, DTMF level and duration, hook flash minimum and maximum, and the
# network specific extensions pointer (18 bytes). Then the four tables.
send "$log" 0080 44 003a 0001 "02000000000100008000000000000000ffff"
check "the voice service profile is created" "$(answer "$log" 0080 | cut -c1-2)" "00"
T1=$(pad 0101015e00 20); T2=$(pad 0201019000 20); T1B=$(pad 0101028000 20)
send "$log" 0081 48 003a 0001 "0040$T1"
send "$log" 0082 48 003a 0001 "0040$T2"
send "$log" 0083 48 003a 0001 "0040$T1B"
check "tone patterns: keyed by index, the second index 1 replaces the first" \
      "$(table "$log" b05 003a 0001 0040)" "$T1B$T2"
send "$log" 0084 49 003a 0001 fffc
check "a Get of the whole mask is one answer: the first table's size, 40 bytes" \
      "$(answer "$log" 0084 | cut -c1-14)" "00004000000028"
E1=0301ffff000000
send "$log" 0085 48 003a 0001 "0020$E1"
check "tone events: one row" "$(table "$log" b06 003a 0001 0020)" "$E1"
send "$log" 0086 48 003a 0001 "002000000000000000"
check "tone events: event 0 clears the table" "$(table "$log" b07 003a 0001 0020)" ""
R1=010103e800
send "$log" 0087 48 003a 0001 "0010$R1"
check "ringing patterns: one row" "$(table "$log" b08 003a 0001 0010)" "$R1"
RE1=0101ffff00ffff
send "$log" 0088 48 003a 0001 "0008$RE1"
check "ringing events: one row" "$(table "$log" b09 003a 0001 0008)" "$RE1"
check "tone patterns: untouched by the other three" \
      "$(table "$log" b0a 003a 0001 0040)" "$T1B$T2"

# ----------------------------------------- the profiles and the line
# RTP profile: ports 50000-50100, DSCP EF, DTMF events. Its IP host
# pointer, not in the Create, is G.988's null.
send "$log" 0090 44 008f 0001 "c350c3b42e00000100"
send "$log" 0091 49 008f 0001 0100
check "the RTP profile's IP host pointer is null until set" \
      "$(answer "$log" 0091 | cut -c1-10)" "000100ffff"
# Media profile: passthrough fax, the voice service profile, four codec
# triples (codec, 20 ms, no silence suppression), out-of-band DTMF, the RTP
# profile: 18 bytes.
send "$log" 0092 44 008e 0001 "000001001400081400121400001400010001"
check "the media profile is created" "$(answer "$log" 0092 | cut -c1-2)" "00"
# Application service profile: no features, no URIs.
send "$log" 0093 44 0092 0001 "00000000000000ffffffffffff"
send "$log" 0094 49 0092 0001 0040
check "the application service profile's IP host pointer is null until set" \
      "$(answer "$log" 0094 | cut -c1-10)" "000040ffff"
# Feature access codes: nothing set by the Create; the emergency number.
send "$log" 0095 44 0093 0001 ""
send "$log" 0096 48 0093 0001 "0080$(pad "$(hexof 190)" 5)"
send "$log" 0097 49 0093 0001 0080
check "a feature access code reads back" \
      "$(answer "$log" 0097 | cut -c1-16)" "0000803139300000"

# The SIP user: agent, number, authentication, no voicemail, 3600 s, dial
# plan, application profile, feature codes, POTS UNI 0x0201 (20 bytes).
send "$log" 00a0 44 0099 0001 "000100030003ffff00000e100001000100010201"
check "the SIP user is created" "$(answer "$log" 00a0 | cut -c1-2)" "00"
send "$log" 00a1 49 0099 0001 0030
check "its release and receiver off hook timers are G.988's 10 s and 15 s" \
      "$(answer "$log" 00a1 | cut -c1-10)" "0000300a0f"
NAME='Alice Example';      NAMEX=$(hexof "$NAME")
send "$log" 00a2 48 0099 0001 "2000$(pad "$NAMEX" 25)"
check "the display name Set is answered ok" "$(answer "$log" 00a2 | cut -c1-2)" "00"
# The voice CTP ties them together: SIP user, POTS UNI, media profile, loop
# start.
send "$log" 00a3 44 008b 0001 "00010201000101"
send "$log" 00a4 49 008b 0001 f000
check "the voice CTP reads back" "$(answer "$log" 00a4 | cut -c1-20)" "00f00000010201000101"

# A PM entity: the threshold data pointer is its one set-by-create
# attribute; the counters are zero, the stick sees no calls.
send "$log" 00b0 44 008c 0201 "0000"
send "$log" 00b1 49 008c 0201 a000
check "call control PM: interval end time and the first counter are 0" \
      "$(answer "$log" 00b1 | cut -c1-16)" "00a0000000000000"
send "$log" 00b2 5c 008c 0201 1000
check "call control PM: Get current data is answered too" \
      "$(answer "$log" 00b2 | cut -c1-14)" "00100000000000"

# --------------------------------------------- what the OLT may not do
# The POTS UNI, its line status and the VoIP config data are the ONU's own:
# no Create, no Delete, and the line status takes no Set.
for t in "00c0 44 0035 0201 Create-of-a-POTS-UNI" "00c1 46 0035 0201 Delete-of-a-POTS-UNI" \
         "00c2 44 008d 0201 Create-of-a-line-status" "00c3 48 008d 0201 Set-of-a-line-status" \
         "00c4 44 008a 0000 Create-of-the-VoIP-config-data"; do
	set -- $t
	send "$log" "$1" "$2" "$3" "$4" "4000"
	check "$(echo "$5" | tr - ' ') is refused, result 2" "$(answer "$log" "$1" | cut -c1-2)" "02"
done
# Test of a POTS UNI: there is no line to test; result 2, and no Test result.
send "$log" 00c5 52 0035 0201 ""
check "a Test of a POTS UNI is refused, result 2" "$(answer "$log" 00c5 | cut -c1-2)" "02"
check "and no Test result follows" "$(grep -c '^-> test result' "$log")" "0"

# ----------------------------------------------------------- secrets
check "the SIP password is in no log line" "$(leaks "$log" "$PW" "$PWX")" "0"
check "the display name is in no log line" "$(leaks "$log" "$NAME" "$NAMEX")" "0"
check "the Set of the display name is logged as withheld" \
      "$(grep -A1 '^<- set  *tci 162 ' "$log" | grep -c 'contents withheld (credentials)')" "1"
$Q cli/build/omcli mib get 153 > /tmp/voip-dump 2>&1
check "omcli mib 153 masks the display name" \
      "$(leaks /tmp/voip-dump "$NAME" "$NAMEX")$([ "$(grep -c '\*\*' /tmp/voip-dump)" -gt 0 ] && echo masked)" "0masked"
$Q cli/build/omcli mib get 153 --secrets > /tmp/voip-dump 2>&1
check "and shows it with --secrets" "$(leaks /tmp/voip-dump "$NAME" "$NAMEX")" "1"
$Q cli/build/omcli mib get 148 > /tmp/voip-dump 2>&1
check "omcli mib 148 masks the SIP password" "$(leaks /tmp/voip-dump "$PW" "$PWX")" "0"
for c in 53 58 138 139 141 142 143 145 146 147 150 153; do
	check "diag-bundle.sh does not dump class $c" \
	      "$(sed -n 's/^OMCI_CLASSES="\(.*\)"/\1/p' ../../rootfs/skeleton/etc/scripts/diag-bundle.sh | tr ' ' '\n' | grep -cx "$c")" "0"
done

# ------------------------------------------------ respawn and resume
send "$log" 00d0 49 0002 0000 8000
mds=$(answer "$log" 00d0 | cut -c7-8)
kill -9 "$pid" 2>/dev/null
wait "$pid" 2>/dev/null
log2=/tmp/omcid-voip2.log
$Q respond/build/omcid -d -s 5 -g /tmp/voip-gpon > "$log2" 2>&1 &
pid=$!
omcid_up "$log2"
check "the respawn resumed from the snapshot" "$(grep -c 'snapshot loaded' "$log2")" "1"
send "$log2" 00e0 49 0002 0000 8000
check "with the same MIB data sync" "$(answer "$log2" 00e0 | cut -c7-8)" "$mds"
check "the dial plan rows are back" "$(table "$log2" b11 0091 0001 0400)" "$D3$D2"
check "the tone patterns are back" "$(table "$log2" b12 003a 0001 0040)" "$T1B$T2"
check "the SIP response row is back" "$(table "$log2" b13 0096 0001 0010)" "$S404"
send "$log2" 00e1 49 0096 0001 0e00
check "the SIP agent's set TCP/UDP pointer is back" \
      "$(answer "$log2" 00e1 | cut -c1-26)" "000e00000100000e1000000168"
$Q cli/build/omcli mib get 153 --secrets > /tmp/voip-dump 2>&1
check "the display name is back" "$(leaks /tmp/voip-dump "$NAME" "$NAMEX")" "1"

# A MIB reset drops every one of them, and the snapshot.
send "$log2" 00f0 4f 0002 0000 ""
for c in 58 139 145 150 153; do
	check "after a MIB reset class $c holds nothing" \
	      "$($Q cli/build/omcli mib get "$c" 2>&1 | tail -1)" "0 rows"
done
check "and the snapshot is gone" "$([ -e /var/run/omcid-mib.snap ] && echo there)" ""
omcid_stop "$pid"

[ "$fail" -eq 0 ] || { keep_log "$log" voip-test.log; keep_log "${log2:-$log}" voip-test2.log; }
if [ "$fail" -eq 0 ]; then echo "voip-test.sh: all checks passed"; else echo "FAILED ($fail)"; fi
suite_exit
