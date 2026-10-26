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
# resumes it all from the snapshot; a MIB reset that drops it. And what the
# ONU creates for the port the key asks for: the POTS circuit pack, the PPTP
# POTS UNI and its line status ("port not configured" until a SIP line names
# the port, then "config done", never "registered"), the VoIP config data
# (SIP over OMCI only; another method reads as failed), a Test refused, no
# alarm.
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
check "key off: omcicli voip says there is no voice, in one line" \
      "$($Q cli/build/omcli voip 2>&1)" "no voice provisioned"
omcid_stop "$pid"

# ---------------------------------------------------------------- key on
printf 'OMCI_POTS_PORTS=1\n' > /var/config/odi.conf
log=/tmp/omcid-voip.log
$Q respond/build/omcid -d -g /tmp/voip-gpon > "$log" 2>&1 &
pid=$!
omcid_up "$log"
check "key on: the startup log says so" \
      "$(grep -c '^phone: 1 POTS port(s) in slot 2, the VoIP entities accepted' "$log")" "1"
send "$log" 0010 4f 0002 0000 ""                         # MIB reset
# One port: the cardholder (2 records) and circuit pack (4) of the POTS
# slot, the POTS UNI (1), its UNI-G (1) and line status (4), and the VoIP
# config data (2), after the stock 399.
check "key on: the upload carries the 14 records of one phone port" \
      "$(upload "$log")" "413"
ga() { $Q cli/build/omcli mib getattr "$@" 2>&1; }
check "key on, nothing provisioned: omcicli voip says so in one line" \
      "$($Q cli/build/omcli voip 2>&1)" "no voice provisioned"
check "and its secret list is empty" "$($Q cli/build/omcli voip --secrets-list 2>&1)" ""
check "the POTS circuit pack is in slot 2, type 32 (POTS)" \
      "$(ga 6 258 1)" "class 6 entity 258 attr 1 = 20"
check "with one port" "$(ga 6 258 2)" "class 6 entity 258 attr 2 = 01"
check "its cardholder expects a POTS card with one port" \
      "$(ga 5 258 2)$(ga 5 258 3)" "class 5 entity 258 attr 2 = 20class 5 entity 258 attr 3 = 01"
check "the POTS UNI 0x0201 is unlocked and enabled, on hook" \
      "$(ga 53 513 1)$(ga 53 513 9)$(ga 53 513 10)" \
      "class 53 entity 513 attr 1 = 00class 53 entity 513 attr 9 = 00class 53 entity 513 attr 10 = 00"
check "the VoIP config data offers SIP, over OMCI only" \
      "$(ga 138 0 1)$(ga 138 0 3)" "class 138 entity 0 attr 1 = 01class 138 entity 0 attr 3 = 00000001"
check "the line status says port not configured (13)" \
      "$(ga 141 513 2)" "class 141 entity 513 attr 2 = 0d"

# The VoIP config data: the OLT picks SIP and OMCI (attributes 2 and 4).
send "$log" 0011 48 008a 0000 "50000101"
send "$log" 0012 49 008a 0000 5400
check "SIP over OMCI picked: stored, the state inactive (0)" \
      "$(answer "$log" 0012 | cut -c1-12)" "005400010100"
send "$log" 0013 48 008a 0000 "100002"
send "$log" 0014 49 008a 0000 0400
check "a method that fetches a configuration reads as failed (3)" \
      "$(answer "$log" 0014 | cut -c1-8)" "00040003"
send "$log" 0015 48 008a 0000 "100001"
send "$log" 0016 49 008a 0000 0400
check "back to OMCI, the state is inactive again" \
      "$(answer "$log" 0016 | cut -c1-8)" "00040000"
# The POTS UNI: locked, it is disabled; unlocked, enabled again.
send "$log" 0017 48 0035 0201 "800001"
send "$log" 0018 49 0035 0201 0080
check "a locked POTS UNI is disabled (1)" "$(answer "$log" 0018 | cut -c1-8)" "00008001"
send "$log" 0019 48 0035 0201 "800000"
send "$log" 001a 49 0035 0201 0080
check "unlocked, enabled again (0)" "$(answer "$log" 001a | cut -c1-8)" "00008000"

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
DIAL1=$(pad "0101$(hexof 0T)" 30)
DIAL2=$(pad "0201$(hexof '*xx')" 30)
DIAL3=$(pad "0301$(hexof '[2-9]xxxxxxx')" 30)
send "$log" 0071 48 0091 0001 "0400$DIAL1"
send "$log" 0072 48 0091 0001 "0400$DIAL2"
send "$log" 0073 48 0091 0001 "0400$DIAL3"
check "dial plan: three rows, newest first" \
      "$(table "$log" b03 0091 0001 0400)" "$DIAL3$DIAL2$DIAL1"
send "$log" 0074 48 0091 0001 "0400$(pad 0100 30)"
check "dial plan: action 0 removes that id" \
      "$(table "$log" b04 0091 0001 0400)" "$DIAL3$DIAL2"
send "$log" 0075 49 0091 0001 8000
check "dial plan: the dial plan number is the rows held" \
      "$(answer "$log" 0075 | cut -c1-10)" "0080000002"

# ------------------------------------------------ voice service profile
# Create: announcement, jitter target and maximum, echo cancel, PSTN
# variant, DTMF level and duration, hook flash minimum and maximum, and the
# network specific extensions pointer (18 bytes). Then the four tables.
send "$log" 0080 44 003a 0001 "02000000000100008000000000000000ffff"
check "the voice service profile is created" "$(answer "$log" 0080 | cut -c1-2)" "00"
TONE1=$(pad 0101015e00 20); TONE2=$(pad 0201019000 20); TONE1B=$(pad 0101028000 20)
send "$log" 0081 48 003a 0001 "0040$TONE1"
send "$log" 0082 48 003a 0001 "0040$TONE2"
send "$log" 0083 48 003a 0001 "0040$TONE1B"
check "tone patterns: keyed by index, the second index 1 replaces the first" \
      "$(table "$log" b05 003a 0001 0040)" "$TONE1B$TONE2"
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
      "$(table "$log" b0a 003a 0001 0040)" "$TONE1B$TONE2"

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
# start. Only then is the line configured.
send "$log" 00a5 49 008d 0201 4000
check "line status: port not configured while no voice CTP names the port" \
      "$(answer "$log" 00a5 | cut -c1-8)" "0040000d"
send "$log" 00a3 44 008b 0001 "00010201000101"
send "$log" 00a6 49 008d 0201 4000
check "line status: config done (14) once one does; never registered" \
      "$(answer "$log" 00a6 | cut -c1-8)" "0040000e"
send "$log" 00a7 49 008d 0201 f980
check "line status: idle, no codec, no packet period, line idle, no emergency call" \
      "$(answer "$log" 00a7)" "00f98000000e00000000000000$(printf '%038d' 0)"
send "$log" 00ab 49 008d 0201 0400
check "line status: no called address" \
      "$(answer "$log" 00ab)" "000400$(printf '%058d' 0)"
send "$log" 00a8 46 008b 0001 ""
send "$log" 00a9 49 008d 0201 4000
check "line status: not configured again when the voice CTP goes" \
      "$(answer "$log" 00a9 | cut -c1-8)" "0040000d"
send "$log" 00aa 44 008b 0001 "00010201000101"
send "$log" 00a4 49 008b 0001 f000
check "the voice CTP reads back" "$(answer "$log" 00a4 | cut -c1-20)" "00f00000010201000101"

# The router's report (OMCI_VOIP_LINE_REPORT): fresh facts only, from the file
# confd writes; the seconds-since-boot stamp is the /proc/uptime clock.
up() { cut -d. -f1 /proc/uptime; }
status_of() {
	send "$log" "$1" 49 008d 0201 4000
	answer "$log" "$1" | cut -c1-8
}
REP=/var/run/voip-line-status
rm -f $REP
printf 'LINE1=registered %s\n' "$(up)" > $REP
check "report: a fresh registered report is ignored while the key is off" \
      "$(status_of 0c01)" "0040000e"
printf 'OMCI_POTS_PORTS=1\nOMCI_VOIP_LINE_REPORT=1\n' > /var/config/odi.conf
check "report: key on, fresh registered: line status 1" "$(status_of 0c02)" "00400001"
for w in 'in_session 2' 'failed_icmp 3' 'failed_tcp 4' 'failed_auth 5' \
         'failed_timeout 6' 'failed_server 7' 'failed_invite_icmp 8' \
         'failed_invite_tcp 9' 'failed_invite_auth 10' 'failed_invite_timeout 11' \
         'failed_invite_server 12' 'none 0'; do
	printf 'LINE1=%s %s\n' "${w% *}" "$(up)" > $REP
	check "report: ${w% *} reads as ${w#* }" "$(status_of 0c03)" "004000$(printf %02x "${w#* }")"
done
printf 'LINE1=registered %s\n' "$(( $(up) - 400 ))" > $REP
check "report: older than the TTL falls back to config done" "$(status_of 0c04)" "0040000e"
printf 'LINE1=registered %s\n' "$(( $(up) + 1000 ))" > $REP
check "report: a stamp from the future is no report" "$(status_of 0c05)" "0040000e"
printf 'LINE1=ringing %s\n' "$(up)" > $REP
check "report: an unknown state word is no report" "$(status_of 0c06)" "0040000e"
printf 'LINE1=registered\n' > $REP
check "report: no stamp is no report" "$(status_of 0c07)" "0040000e"
printf 'LINE2=registered %s\n' "$(up)" > $REP
check "report: another port's report does not count" "$(status_of 0c08)" "0040000e"
printf 'LINE1=registered %s\n' "$(up)" > $REP
send "$log" 0c09 46 008b 0001 ""
check "report: no provisioned line, no reported state: port not configured" \
      "$(status_of 0c0a)" "0040000d"
send "$log" 0c0b 44 008b 0001 "00010201000101"
check "report: the line is back, the report still counts" "$(status_of 0c0c)" "00400001"
check "report: omcicli voip shows the reported state" \
      "$($Q cli/build/omcli voip 2>&1 | grep -c 'line status registered (1)')" "1"
rm -f $REP
check "report: no file is no report" "$(status_of 0c0d)" "0040000e"
printf 'OMCI_POTS_PORTS=1\n' > /var/config/odi.conf

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
# Get all alarms: nothing of the phone port has an alarm to report.
send "$log" 00c6 4b 0002 0000 ""
na=$((0x$(answer "$log" 00c6 | cut -c1-4)))
s=0; voice=0
while [ "$s" -lt "$na" ]; do
	t=$(printf '%04x' $((0xd00 + s)))
	send "$log" "$t" 4c 0002 0000 "$(printf '%04x' "$s")"
	case $(answer "$log" "$t" | cut -c1-8) in
	00050102|00060102|0035*|008a*|008d*|01080201) voice=$((voice + 1)) ;;
	esac
	s=$((s + 1))
done
check "Get all alarms lists none of the phone port's entities" "$voice" "0"

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

# ------------------------------------------------------- omcicli voip
# The resolved view. More of the provisioning first: a second server URL
# with a login in it (the outbound proxy), a TCP/UDP config naming IP host 1,
# the tag the OLT puts on that IP host, and a TR-069 login that must not
# show up, a 148 reached by no SIP entity.
OUTB='sip:user1:Url-Pw-9@proxy2.example.net:5070'; OUTBX=$(hexof "$OUTB")
send "$log" 0100 44 009d 0004 ""
send "$log" 0101 48 009d 0004 "c00002$(part "$OUTBX" 1)"
send "$log" 0102 48 009d 0004 "2000$(part "$OUTBX" 2)"
send "$log" 0103 48 0096 0001 "40000004"                  # outbound proxy -> 4
send "$log" 0104 48 0088 0001 "10000001"                  # TCP/UDP -> IP host 1
send "$log" 0105 44 00ab 0001 ""
send "$log" 0106 48 00ab 0001 "800003"                    # association: IP host
send "$log" 0107 48 00ab 0001 "02000001"                  #   of IP host 1
send "$log" 0108 48 00ab 0001 "0400f8000000f800000000050324000f8000"  # untagged -> VID 100, PRI 5
ACSPW=Acs-Pw-77; ACSPWX=$(hexof "$ACSPW"); ACSU=acs-user-x; ACSUX=$(hexof "$ACSU")
ACSURL='https://acs-u:acs-p@acs.example.net/cwmp'; ACSURLX=$(hexof "$ACSURL")
send "$log" 0110 44 0094 0009 ""
send "$log" 0111 48 0094 0009 "c00001$(pad "$ACSUX" 25)"
send "$log" 0112 48 0094 0009 "2000$(pad "$ACSPWX" 25)"
send "$log" 0113 44 009d 0005 ""
send "$log" 0114 48 009d 0005 "c00002$(part "$ACSURLX" 1)"
send "$log" 0115 48 009d 0005 "2000$(part "$ACSURLX" 2)"
send "$log" 0116 44 0089 8002 "00090005"
$Q cli/build/omcli mib get 148 --secrets > /tmp/voip-dump 2>&1
check "the TR-069 login is in the MIB, a 148 no SIP entity points at" \
      "$(leaks /tmp/voip-dump "$ACSPW" "$ACSPWX")" "1"

has() { grep -c -F -e "$2" "$1" || true; }
v=/tmp/voip-view
$Q cli/build/omcli voip > "$v" 2>&1
check "voip: the line is on the POTS UNI 0x201" "$(has "$v" 'port 0x201 (voice CTP 0x1)')" "1"
check "voip: the number and the display name are masked like the serial" \
      "$(has "$v" 'sip user 0x1: aor MASKED-5678, display name MASKED-mple')" "1"
check "voip: the login name is masked, the password only said to be set, the realm shown" \
      "$(has "$v" 'sip user login 0x3: username MASKED-voip, password set (12 bytes), realm example.net')" "1"
check "voip: neither the number, the name, the login nor the password is printed" \
      "$(leaks "$v" "$AOR" "$AORX")$(leaks "$v" "$NAME" "$NAMEX")$(leaks "$v" "$USR" "$USRX")$(leaks "$v" "$PW" "$PWX")" "0000"
check "voip: the proxy, as it is" "$(has "$v" "    proxy $PROXY")" "1"
check "voip: the outbound proxy URL has its user:password@ stripped" \
      "$(has "$v" '    outbound proxy sip:proxy2.example.net:5070')" "1"
check "voip: and the stripped login is nowhere in the view" \
      "$(leaks "$v" Url-Pw-9 "$(hexof Url-Pw-9)")$(grep -c 'user1' "$v" || true)" "00"
check "voip: the registrar, through its network address" "$(has "$v" '    registrar registrar.example.net')" "1"
check "voip: the agent port and protocol" "$(has "$v" '    port 5060/udp')" "1"
check "voip: the codecs and the DTMF mode" \
      "$(has "$v" 'codecs PCMU/20ms PCMA/20ms G729/20ms PCMU/20ms, dtmf out of band')" "1"
check "voip: the RTP ports and DSCP" "$(has "$v" 'rtp ports 50000-50100 dscp 46')" "1"
check "voip: the voice service profile" "$(has "$v" 'voice service profile 0x1: jitter target 0 max 0 ms, echo cancel on')" "1"
check "voip: the voice VLAN and priority of the IP host" \
      "$(has "$v" 'voice vlan 100 priority 5 (IP host 0x1)')" "1"
check "voip: the line status is config done" "$(has "$v" 'line status config done (14)')" "1"
check "voip: the TR-069 login is not shown as a SIP credential" \
      "$(leaks "$v" "$ACSPW" "$ACSPWX")$(leaks "$v" "$ACSU" "$ACSUX")$(grep -c 'acs-' "$v" || true)" "000"

$Q cli/build/omcli voip --secrets > "$v" 2>&1
check "voip --secrets: the number, the name and the login in clear" \
      "$(has "$v" "sip user 0x1: aor $AOR, display name $NAME")$(has "$v" "username $USR,")" "12"
check "voip --secrets: the password, on its login line" \
      "$(has "$v" "sip user login 0x3: username $USR, password $PW, realm example.net")" "1"
check "voip --secrets: a URL is still stripped" \
      "$(has "$v" '    outbound proxy sip:proxy2.example.net:5070')$(leaks "$v" Url-Pw-9 "$(hexof Url-Pw-9)")" "10"
check "voip --secrets: the TR-069 login is still not there" \
      "$(leaks "$v" "$ACSPW" "$ACSPWX")$(grep -c 'acs-' "$v" || true)" "00"

$Q cli/build/omcli voip --secrets-list > "$v" 2>&1
check "voip --secrets-list: only the secrets, one per line" \
      "$(sort "$v" | tr '\n' ' ')" \
      "$(printf '%s\n' "$AOR" "$NAME" "$USR" "$PW" user1 Url-Pw-9 "$USR" "$PW" | sort | tr '\n' ' ')"
check "voip --secrets-list: not the TR-069 login" \
      "$(grep -c -e acs- -e Acs- "$v" || true)" "0"
$Q cli/build/omcli voip --nonsense > "$v" 2>&1
check "voip: an unknown argument is refused" "$(has "$v" 'unknown argument --nonsense')" "1"
check "the voip view leaves the password out of the log" "$(leaks "$log" "$ACSPW" "$ACSPWX")" "0"

# ------------------------------------------- the diagnostics bundle
# diag-bundle.sh, run against this omcid with the provisioning above, and a
# log file that quotes every SIP credential the way a careless log line
# would (text, hex and spaced hex): the bundle must carry the masked voip
# view and none of the strings, and still say a line is provisioned. Only
# the steps that need a device fail here, and are listed as failing.
DB=/tmp/voip-diag
rm -rf "$DB"
mkdir -p "$DB/bin" "$DB/log" "$DB/cfg"
printf '#!/bin/sh\nexec %s %s/cli/build/omcli "$@"\n' "$Q" "$PWD" > "$DB/bin/omcli"
chmod +x "$DB/bin/omcli"
spaced() { printf '%s' "$1" | sed 's/../& /g;s/ $//'; }
{
	echo "harness: aor=$AOR name=$NAME user=$USR pw=$PW url=Url-Pw-9 login=user1"
	echo "harness: hex aor=$AORX name=$NAMEX user=$USRX pw=$PWX url=$(hexof Url-Pw-9)"
	echo "harness: ph pw=$(spaced "$PWX") name=$(spaced "$NAMEX")"
} > "$DB/log/voip-harness.log"
cp "$log" "$DB/log/omcid.log"
CONFIG_DIR="$DB/cfg" LOG_DIR="$DB/log" PATH="$DB/bin:$PATH" METRICS_TIMEOUT_S=1 \
	timeout 120 sh ../../rootfs/skeleton/etc/scripts/diag-bundle.sh "$DB/out.tar.gz" > "$DB/run.out" 2>&1
check "the bundle is written" "$(tail -n 1 "$DB/run.out")" "$DB/out.tar.gz"
mkdir "$DB/x"
tar -xzf "$DB/out.tar.gz" -C "$DB/x" 2>/dev/null
D=$DB/x/odi-diag
check "the bundle carries the masked voice view" \
      "$(has "$D/pon/omcli_voip.txt" 'port 0x201 (voice CTP 0x1)')$(has "$D/pon/omcli_voip.txt" 'line status config done (14)')" "11"
check "and its step is listed with status 0" \
      "$(awk '$3 == "pon/omcli_voip.txt" && $1 == 0 { n++ } END { print n + 0 }' "$D/MANIFEST.txt")" "1"
planted=0
for x in "$AOR" "$AORX" "$NAME" "$NAMEX" "$USR" "$USRX" "$PW" "$PWX" Url-Pw-9 "$(hexof Url-Pw-9)" \
	"$(spaced "$PWX")" "$(spaced "$NAMEX")"; do
	planted=$((planted + $(grep -r -l -i -F -e "$x" "$D" | wc -l)))
done
check "no SIP number, name, login or password is in any file of the bundle" "$planted" "0"
check "the login in a server URL is not either, as a whole token" \
      "$(grep -r -l -E '(^|[^A-Za-z0-9])user1([^A-Za-z0-9]|$)' "$D" | wc -l | tr -d ' ')" "0"
check "the quoting log line is scrubbed in place, not dropped" \
      "$(has "$D/log/voip-harness.log" 'harness: aor=REDACTED name=REDACTED user=REDACTED pw=REDACTED url=REDACTED login=REDACTED')" "1"
rm -rf "$DB/x"
mkdir "$DB/x"
CONFIG_DIR="$DB/cfg" LOG_DIR="$DB/log" PATH="$DB/bin:$PATH" METRICS_TIMEOUT_S=1 \
	timeout 120 sh ../../rootfs/skeleton/etc/scripts/diag-bundle.sh --full "$DB/full.tar.gz" > "$DB/run.out" 2>&1
tar -xzf "$DB/full.tar.gz" -C "$DB/x" 2>/dev/null
planted=0
for x in "$AOR" "$NAME" "$USR" "$PW" Url-Pw-9; do
	planted=$((planted + $(grep -r -l -i -F -e "$x" "$DB/x/odi-diag" | wc -l)))
done
check "--full keeps the identity but never a SIP credential" "$planted" "0"
rm -rf "$DB"

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
check "the dial plan rows are back" "$(table "$log2" b11 0091 0001 0400)" "$DIAL3$DIAL2"
check "the tone patterns are back" "$(table "$log2" b12 003a 0001 0040)" "$TONE1B$TONE2"
check "the SIP response row is back" "$(table "$log2" b13 0096 0001 0010)" "$S404"
send "$log2" 00e1 49 0096 0001 0e00
check "the SIP agent's set TCP/UDP pointer is back" \
      "$(answer "$log2" 00e1 | cut -c1-26)" "000e00000100000e1000000168"
$Q cli/build/omcli mib get 153 --secrets > /tmp/voip-dump 2>&1
check "the display name is back" "$(leaks /tmp/voip-dump "$NAME" "$NAMEX")" "1"
send "$log2" 00e2 49 008d 0201 4000
check "and the line is still config done" "$(answer "$log2" 00e2 | cut -c1-8)" "0040000e"

# The resume and the saves share one buffer (snapshot.c): a Set after a
# resume is saved, together with everything the resume brought back, and a
# second respawn finds both.
send "$log2" 00e3 48 0096 0001 040000000708
kill -9 "$pid" 2>/dev/null
wait "$pid" 2>/dev/null
log3=/tmp/omcid-voip3.log
$Q respond/build/omcid -d -s 5 -g /tmp/voip-gpon > "$log3" 2>&1 &
pid=$!
omcid_up "$log3"
check "a second respawn resumed too" "$(grep -c 'snapshot loaded' "$log3")" "1"
send "$log3" 00e4 49 0096 0001 0400
check "with the Set made after the first resume" \
      "$(answer "$log3" 00e4 | cut -c1-14)" "00040000000708"
check "and the rows the first resume brought back" \
      "$(table "$log3" b14 0091 0001 0400)" "$DIAL3$DIAL2"

# A MIB reset drops every one of them, and the snapshot.
send "$log3" 00f0 4f 0002 0000 ""
for c in 58 139 145 150 153; do
	check "after a MIB reset class $c holds nothing" \
	      "$($Q cli/build/omcli mib get "$c" 2>&1 | tail -1)" "0 rows"
done
check "and the snapshot is gone" "$([ -e /var/run/omcid-mib.snap ] && echo there)" ""
check "after a MIB reset omcicli voip finds no voice" \
      "$($Q cli/build/omcli voip 2>&1)" "no voice provisioned"
send "$log3" 00f1 49 008d 0201 4000
check "the POTS UNI and its line status stay, the ONU's own: port not configured" \
      "$(answer "$log3" 00f1 | cut -c1-8)" "0040000d"
omcid_stop "$pid"

[ "$fail" -eq 0 ] || { keep_log "$log" voip-test.log; keep_log "${log2:-$log}" voip-test2.log; keep_log "${log3:-$log}" voip-test3.log; }
if [ "$fail" -eq 0 ]; then echo "voip-test.sh: all checks passed"; else echo "FAILED ($fail)"; fi
suite_exit
