#!/bin/sh
# The SIGHUP reload: what omcid does when apply.sh omci tells it to reread
# the config store (respond/reload.c, docs/SETTINGS.md "How apply works").
#
# Three things are proved here, against omcid-drvtrace (every driver call
# logged and answered with success, drv-test.sh) running under qemu-user:
#
#   1. A VLAN-only change is rebuilt in place, and the rules are the ones a
#      fresh omcid builds. For each ISP session and two new VLAN settings
#      (another tag, and transparent mode) the daemon is run on the old
#      settings, sent SIGHUP with the store changed, and its bridge-rule
#      driver calls (command 51) and connection dump after the reload are
#      compared byte for byte with a second daemon that ran the same session
#      with the new settings from its first frame. The MIB is compared
#      before and after too: a reload does not touch it.
#   2. An identity change re-registers without a restart: the driver verbs
#      arrive in the order rcS uses (gpondeact, gponsn, gponpw, gponact,
#      written to a file with -i), the MIB is cleared, the loop is still
#      served during the hold, the reload finishes when the ONU is back in O5
#      with its services, and the pid never changes. The failure paths (a
#      verb that fails, O5 never reached, O5 without services) and a burst
#      of SIGHUPs during a re-registration are covered.
#   3. Which key lands in which class, one key at a time, in a dry-run
#      instance (no driver, no waiting), including the rule that the report
#      keys count only while the identity switch is on.
#
# Run from src/omci, inside the toolchain image, after drv-test.sh:
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci "$(../../toolchain/image.sh diag)" \
#          sh reload-test.sh
set -u
Q=qemu-mips-static
FIX=../../test/fixtures
fail=0

CAPS=020000ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00ff00000000000000010000000300000002ffffffff000000000000001000000040000000800000000800000000000000100000000f00000440

CS=/var/config/lastgood.xml
HS=/var/config/lastgood_hs.xml
ODI=/var/config/odi.conf
SW=/var/config/omci-identity.on
STATUS=/var/run/omcid-reload
INITLOG=/tmp/rl-init

mkdir -p /var/config /var/run
clean_state() {
	rm -f "$HS" "$ODI" "$SW" /var/run/omcid-mib.snap /var/run/omcid-mib.snap.tmp \
	      /var/run/omcid-resume-decision "$STATUS" "$INITLOG"
	: > "$INITLOG"
}

ok()  { echo "ok    $*"; }
bad() { echo "FAIL  $*"; fail=$((fail + 1)); }
check() {   # check <label> <want> <got>
	if [ "$2" = "$3" ]; then ok "$1"; else bad "$1: want '$2', got '$3'"; fi
}
check_grep() {   # check_grep <label> <pattern> <file>
	if grep -q -- "$2" "$3"; then ok "$1"; else bad "$1: no '$2' in $3"; sed -n '1,3p' "$3"; fi
}

# The config store: the four VLAN keys, then whatever extra <Value> lines.
store() {   # store <mode> <vid> <pri> [extra lines]
	cat > "$CS" <<EOF
<Config Name="ROOT">
	<Dir Name="MIB_TABLE">
		<Value Name="VLAN_CFG_TYPE" Value="1"/>
		<Value Name="VLAN_MANU_MODE" Value="$1"/>
		<Value Name="VLAN_MANU_TAG_VID" Value="$2"/>
$(if [ -n "$3" ]; then printf '\t\t<Value Name="VLAN_MANU_TAG_PRI" Value="%s"/>\n' "$3"; fi)
${4:-}	</Dir>
</Config>
EOF
}
hs_sn() {   # hs_sn <label>
	cat > "$HS" <<EOF
<Config Name="ROOT">
	<Dir Name="HW_MIB_TABLE">
		<Value Name="GPON_SN" Value="$1"/>
	</Dir>
</Config>
EOF
}
val() { printf '\t\t<Value Name="%s" Value="%s"/>\n' "$1" "$2"; }

gpon() {   # gpon <file> <state> <alloc_ids line value>
	printf 'state %s (O%s)\nonu_id 26\nsn 4f44495400000001\neqd multiframe 0 inframe 0\nploam ds_rx 0 us_tx 0\nalloc_ids %s\n' \
		"$2" "$2" "$3" > "$1"
}
gpon /tmp/rl-gpon-isp1 5 "5 282 794 1050 1306 538"
gpon /tmp/rl-gpon-isp2 5 "1 348"

st() { sed -n "s/^$1=//p" "$STATUS" 2>/dev/null | head -n 1; }

# wait_count <file> <pattern> <count> <seconds>: the pattern shows at least
# <count> times within the time.
wait_count() {
	i=0
	while [ "$i" -lt $(( $4 * 10 )) ]; do
		[ "$(grep -c -- "$2" "$1")" -ge "$3" ] && return 0
		sleep 0.1
		i=$((i + 1))
	done
	return 1
}
# wait_done <previous id> <seconds>: the status file names a finished run
# other than the previous one.
wait_done() {
	i=0
	while [ "$i" -lt $(( $2 * 10 )) ]; do
		[ "$(st state)" = "done" ] && [ "$(st id)" != "$1" ] && return 0
		sleep 0.1
		i=$((i + 1))
	done
	return 1
}

# start_omcid <log> [extra args]: the traced daemon, in the background;
# sets pid. -d: it runs until told to stop.
start_omcid() {
	log=$1
	shift
	$Q respond/build/omcid-drvtrace -a -d -c "$CAPS" -i "$INITLOG" "$@" > "$log" 2>&1 &
	pid=$!
	wait_count "$log" 'answering as' 1 30 || bad "$log: omcid did not come up"
}
stop_omcid() {
	kill "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
}
inject() {   # inject <isp>
	$Q cli/build/omcli --inject-file "$FIX/omci-session-$1.txt" > /tmp/rl-inject.txt 2>&1
	frames=$(grep -c '^[0-9a-f]' "$FIX/omci-session-$1.txt")
	grep -q "injected $frames frames" /tmp/rl-inject.txt || bad "$1: omcli did not inject all $frames frames"
}
# settle <log> <rebuilds>: the quiet second after the last frame has passed
# and the bridge connections have been rebuilt <rebuilds> times in all.
settle() {
	wait_count "$1" 'bridge connections rebuilt' "$2" 15 || bad "$1: no bridge rebuild after the session"
	sleep 0.5
}
rules_in_place() {   # the bridge rules of the rebuild a reload made
	awk '/event=reload changed=vlan/{on=1} on&&/^drv 51 /{print} on&&/bridge connections rebuilt/{exit}' "$1"
}
rules_last() {       # the bridge rules of the last rebuild of a run
	awk '/^drv 51 /{b=b $0 "\n"} /bridge connections rebuilt/{last=b; b=""} END{printf "%s", last}' "$1"
}

# ------------------------------------------------ 1. VLAN, in place vs fresh
#
# gpon file per ISP as drv-test.sh gives them: ISP1 binds five PLOAM Alloc-IDs.
for isp in isp1 isp2; do
	case $isp in
	isp1) vid0=11 ;;
	*)    vid0=10 ;;
	esac
	for variant in tag transparent; do
		case $variant in
		tag)         mode=1; vid=20; pri=3; keys=VLAN_MANU_TAG_VID,VLAN_MANU_TAG_PRI
			     [ "$vid0" = 20 ] && vid=21 ;;
		transparent) mode=0; vid=$vid0; pri=0; keys=VLAN_MANU_MODE ;;
		esac
		label="$isp -> $variant"

		# In place: the old settings, the session, then the change.
		clean_state
		store 1 "$vid0" 0
		log=/tmp/rl-inplace.log
		start_omcid "$log" -g /tmp/rl-gpon-$isp
		inject "$isp"
		settle "$log" 1
		rules_last "$log" > /tmp/rl-rules-before.txt
		mib_before=$($Q cli/build/omcli mib get all 2>&1)
		conn_before=$($Q cli/build/omcli conn 2>&1)
		store "$mode" "$vid" "$pri"
		old_id=$(st id)
		kill -HUP "$pid"
		if ! wait_done "$old_id" 15; then
			bad "$label: the reload did not finish"
			cat "$STATUS" 2>/dev/null
		fi
		mib_after=$($Q cli/build/omcli mib get all 2>&1)
		conn_after=$($Q cli/build/omcli conn 2>&1)
		rules_in_place "$log" > /tmp/rl-rules-inplace.txt
		alive=$(kill -0 "$pid" 2>/dev/null && echo yes || echo no)
		starts=$(grep -c 'event=start ' "$log")
		stop_omcid

		check_grep "$label: reload logged as a rebuild" \
			"event=reload changed=vlan action=rebuild keys=$keys\$" "$log"
		check_grep "$label: reload logged as done" \
			"event=reload_done changed=vlan action=rebuild result=ok" "$log"
		check "$label: the process is the same one, never restarted" "yes:1" "$alive:$starts"
		check "$label: the status file says done, ok, vlan" "done ok vlan rebuild" \
			"$(st state) $(st result) $(st changed) $(st action)"
		check "$label: the MIB is untouched by the reload" "$mib_before" "$mib_after"
		if [ -s /tmp/rl-rules-inplace.txt ]; then ok "$label: the reload sent bridge rules ($(wc -l < /tmp/rl-rules-inplace.txt))"
		else bad "$label: the reload sent no bridge rules"; fi
		if cmp -s /tmp/rl-rules-before.txt /tmp/rl-rules-inplace.txt; then
			bad "$label: the rules did not change with the setting"
		else ok "$label: the rules differ from the ones before the reload"; fi

		# Fresh: the new settings from the first frame.
		clean_state
		store "$mode" "$vid" "$pri"
		flog=/tmp/rl-fresh.log
		start_omcid "$flog" -g /tmp/rl-gpon-$isp
		inject "$isp"
		settle "$flog" 1
		rules_last "$flog" > /tmp/rl-rules-fresh.txt
		conn_fresh=$($Q cli/build/omcli conn 2>&1)
		stop_omcid

		if cmp -s /tmp/rl-rules-inplace.txt /tmp/rl-rules-fresh.txt; then
			ok "$label: the rebuilt rules are byte for byte a fresh daemon's"
		else
			bad "$label: rebuilt rules differ from a fresh daemon's"
			diff /tmp/rl-rules-inplace.txt /tmp/rl-rules-fresh.txt | head -20
		fi
		check "$label: the connection dump equals a fresh daemon's" "$conn_fresh" "$conn_after"
		if [ "$conn_before" = "$conn_after" ] && [ "$variant" = tag ]; then
			bad "$label: the connection dump did not change with the VID"
		fi
	done
done

# ------------------------------------------------ unchanged, and an old store
clean_state
store 1 11 0
log=/tmp/rl-none.log
start_omcid "$log" -g /tmp/rl-gpon-isp1
inject isp1
settle "$log" 1
: > "$INITLOG"
drv_before=$(grep -c '^drv ' "$log")
kill -HUP "$pid"
wait_count "$log" 'event=reload_done' 1 10 || bad "unchanged: no reload_done"
drv_after=$(grep -c '^drv ' "$log")
check_grep "unchanged: logged as changed=none" "event=reload changed=none action=none\$" "$log"
check "unchanged: no driver call" "$drv_before" "$drv_after"
check "unchanged: no driver verb" "0" "$(wc -c < "$INITLOG" | tr -d ' ')"
check "unchanged: status done none" "done none ok" "$(st state) $(st changed) $(st result)"
stop_omcid

# ------------------------------------------------ 2. identity: re-register
clean_state
store 1 11 0 "$(val GPON_PLOAM_PASSWD 3132333435)
"
hs_sn ODIT00000001
gpon /tmp/rl-gpon-isp1 5 "5 282 794 1050 1306 538"
log=/tmp/rl-ident.log
start_omcid "$log" -g /tmp/rl-gpon-isp1
inject isp1
settle "$log" 1
mib_before=$($Q cli/build/omcli mib get all 2>&1)
conn_before=$($Q cli/build/omcli conn 2>&1)
: > "$INITLOG"
old_id=$(st id)

# The new serial number, and the ONU never sees O5 until the test says so.
hs_sn ODIT00000002
gpon /tmp/rl-gpon-isp1 1 "0"
kill -HUP "$pid"
wait_count "$INITLOG" gpondeact 1 5 || bad "identity: no gpondeact"
sleep 1
check "identity: still held down a second in (only gpondeact so far)" "gpondeact" \
	"$(tr '\n' ' ' < "$INITLOG" | sed 's/ $//')"
check "identity: running, MIB cleared, no services" "running 0" "$(st state) $(st services)"
served=$($Q cli/build/omcli state 2>&1 | wc -l | tr -d ' ')
[ "$served" -gt 0 ] && ok "identity: the loop still answers the CLI during the hold" \
	|| bad "identity: the CLI got no answer during the hold"
wait_count "$INITLOG" gponact 1 8 || bad "identity: no gponact"
check "identity: the verbs, in rcS order" "gpondeact gponsn ODIT00000002 gponpw 3132333435 gponact" \
	"$(tr '\n' ' ' < "$INITLOG" | sed 's/ $//')"
check_grep "identity: the MIB reset is logged" "event=mib_reset side=local" "$log"
check "identity: activated, not yet done" "running activated" "$(st state) $(st result)"

# The OLT: O5 and the session again.
gpon /tmp/rl-gpon-isp1 5 "5 282 794 1050 1306 538"
inject isp1
if wait_done "$old_id" 20; then ok "identity: finished once O5 and the services were back"
else bad "identity: never finished"; cat "$STATUS"; fi
check "identity: result ok" "done ok identity reregister" \
	"$(st state) $(st result) $(st changed) $(st action)"
[ -n "$(st o5_ms)" ] && ok "identity: the time to O5 is reported ($(st o5_ms) ms)" || bad "identity: no o5_ms"
[ "$(st services)" -gt 0 ] && ok "identity: services are back ($(st services))" || bad "identity: no services"
check_grep "identity: logged" "event=reload changed=identity action=reregister keys=GPON_SN\$" "$log"
check_grep "identity: done logged" "event=reload_done changed=identity action=reregister result=ok" "$log"
check "identity: the process was never restarted" "1" "$(grep -c 'event=start ' "$log")"
kill -0 "$pid" 2>/dev/null && ok "identity: the process is still the same one" || bad "identity: the process died"
check "identity: the serial number was read from the kernel again" "2" \
	"$(grep -c 'serial number: from the kernel' "$log")"
sleep 1
check "identity: the MIB is what the session builds" "$mib_before" "$($Q cli/build/omcli mib get all 2>&1)"
check "identity: the connections are what the session builds" "$conn_before" "$($Q cli/build/omcli conn 2>&1)"
stop_omcid

# ------------------------------------------------ the failure paths
# -j 4: give up on O5 after four seconds instead of 150.
clean_state
store 1 11 0 "$(val LOID_OLD user1)
$(val LOID user1)
"
gpon /tmp/rl-gpon-isp1 1 "0"
log=/tmp/rl-fail.log
start_omcid "$log" -g /tmp/rl-gpon-isp1 -j 4
old_id=$(st id)
store 1 11 0 "$(val LOID_OLD user2)
$(val LOID user2)
"
kill -HUP "$pid"
if wait_done "$old_id" 20; then
	check "O5 never reached: result timeout" "done timeout" "$(st state) $(st result)"
else bad "O5 never reached: never gave up"; fi
check_grep "O5 never reached: logged" "event=reload_done changed=identity action=reregister result=timeout" "$log"
check "O5 never reached: no gponsn (serial unchanged)" "gpondeact gponact" \
	"$(tr '\n' ' ' < "$INITLOG" | sed 's/ $//')"

# O5 without services, and a burst of SIGHUPs during the wait: exactly one
# more reload follows, and it finds nothing to do.
gpon /tmp/rl-gpon-isp1 5 "0"
old_id=$(st id)
store 1 11 0 "$(val LOID_OLD user3)
$(val LOID user3)
"
before=$(grep -c 'event=reload ' "$log")
kill -HUP "$pid"
wait_count "$INITLOG" gponact 2 10 || bad "no second gponact"
kill -HUP "$pid"; kill -HUP "$pid"; kill -HUP "$pid"
# The coalesced reload finishes within milliseconds of this one and takes the
# status file, so the outcome is read from the log line.
wait_count "$log" 'event=reload_done .*result=no_services' 1 20 || bad "O5 without services: never gave up"
check_grep "O5 without services: result no_services" \
	"event=reload_done changed=identity action=reregister result=no_services" "$log"
wait_count "$log" 'event=reload changed=none' 1 10 || bad "the coalesced SIGHUPs led to no reload"
sleep 1
after=$(grep -c 'event=reload ' "$log")
check "three SIGHUPs during a reload: one reload on top of the running one" "2" "$((after - before))"
stop_omcid

# A verb that fails: the daemon goes back to what it was running with.
clean_state
store 1 11 0 "$(val LOID_OLD user1)
$(val LOID user1)
"
gpon /tmp/rl-gpon-isp1 5 "5 282 794 1050 1306 538"
log=/tmp/rl-verbfail.log
start_omcid "$log" -g /tmp/rl-gpon-isp1 -i /nonexistent/odi_init
inject isp1
settle "$log" 1
mib_before=$($Q cli/build/omcli mib get all 2>&1)
old_id=$(st id)
store 1 11 0 "$(val LOID_OLD user2)
$(val LOID user2)
"
kill -HUP "$pid"
wait_done "$old_id" 10 || bad "verb failure: no outcome"
check "verb failure: result failed" "done failed" "$(st state) $(st result)"
check_grep "verb failure: the step is logged" "event=reload_step verb=gpondeact rc=2" "$log"
check "verb failure: the MIB was not touched" "$mib_before" "$($Q cli/build/omcli mib get all 2>&1)"
old_id=$(st id)
kill -HUP "$pid"
wait_done "$old_id" 10 || bad "verb failure: no outcome on the retry"
check "verb failure: the next SIGHUP still sees the change and tries again" "identity failed" \
	"$(st changed) $(st result)"
stop_omcid

# ------------------------------------------------ 3. which key, which class
# A dry-run daemon (no -a, no driver): every reload is decided and logged,
# and a re-registration completes at once, so a long list of edits is quick.
clean_state
store 1 11 0
log=/tmp/rl-keys.log
$Q respond/build/omcid -d -c "$CAPS" -g /tmp/rl-gpon-isp1 > "$log" 2>&1 &
pid=$!
wait_count "$log" 'answering as' 1 30 || bad "$log: omcid did not come up"
nrel=0
# reload_is <label> <changed> <keys>: SIGHUP, then the line that came of it.
reload_is() {
	nrel=$((nrel + 1))
	kill -HUP "$pid"
	wait_count "$log" 'event=reload_done' "$nrel" 10 || { bad "$1: no reload"; tail -n 6 "$log"; return; }
	line=$(grep 'event=reload ' "$log" | sed -n "${nrel}p")
	if [ -n "${3:-}" ]; then
		check "$1" "event=reload changed=$2 action=$4 keys=$3" "$line"
	else
		check "$1" "event=reload changed=$2 action=$4" "$line"
	fi
}
reload_is "same store"                         none ""                    none
store 1 11 ""
reload_is "VLAN_MANU_TAG_PRI removed (absent reads as 0, but the tag stops applying)" vlan VLAN_MANU_TAG_PRI rebuild
store 1 11 0
reload_is "VLAN_MANU_TAG_PRI back, still 0" vlan VLAN_MANU_TAG_PRI     rebuild
store 1 11 5
reload_is "VLAN_MANU_TAG_PRI"                  vlan VLAN_MANU_TAG_PRI     rebuild
store 1 12 5
reload_is "VLAN_MANU_TAG_VID"                  vlan VLAN_MANU_TAG_VID     rebuild
store 0 12 5
reload_is "VLAN_MANU_MODE 0, transparent"      vlan VLAN_MANU_MODE        rebuild
store 0 12 5 "$(val VLAN_CFG_TYPE 2)
"
# A second VLAN_CFG_TYPE line is ignored: the first one wins, as elsewhere.
reload_is "a duplicate key is not a change"    none ""                    none
store 1 12 5 "$(val OMCI_SW_VER1 V1.2.3)
"
reload_is "VLAN_MANU_MODE back, and an SW version while the switch is off" vlan VLAN_MANU_MODE rebuild
: > "$SW"
reload_is "the identity switch turned on"      identity "omci-identity.on" reregister
store 1 12 5 "$(val OMCI_SW_VER1 V1.2.4)
"
reload_is "OMCI_SW_VER1"                       identity OMCI_SW_VER1      reregister
store 1 12 5 "$(val OMCI_SW_VER1 V1.2.4)
$(val OMCI_SW_VER2 V2.0)
"
reload_is "OMCI_SW_VER2"                       identity OMCI_SW_VER2      reregister
store 1 12 5 "$(val OMCI_SW_VER1 V1.2.4)
$(val OMCI_SW_VER2 V2.0)
$(val OMCC_VER 160)
"
reload_is "OMCC_VER"                           identity OMCC_VER          reregister
store 1 12 5 "$(val OMCI_SW_VER1 V1.2.4)
$(val OMCI_SW_VER2 V2.0)
$(val OMCC_VER 160)
$(val OMCI_VENDOR_PRODUCT_CODE 4242)
"
reload_is "OMCI_VENDOR_PRODUCT_CODE"           identity OMCI_VENDOR_PRODUCT_CODE reregister
X="$(val OMCI_SW_VER1 V1.2.4)
$(val OMCI_SW_VER2 V2.0)
$(val OMCC_VER 160)
$(val OMCI_VENDOR_PRODUCT_CODE 4242)
"
store 1 12 5 "$X$(val GPON_ONU_MODEL MODEL-X)
"
reload_is "GPON_ONU_MODEL"                     identity GPON_ONU_MODEL    reregister
X="$X$(val GPON_ONU_MODEL MODEL-X)
"
printf 'ONU_HW_VERSION=HW-2\n' > "$ODI"
reload_is "ONU_HW_VERSION"                     identity ONU_HW_VERSION    reregister
printf 'ONU_HW_VERSION=HW-2\nOMCI_UNKNOWN_ME_OK=1\n' > "$ODI"
reload_is "OMCI_UNKNOWN_ME_OK"                 identity OMCI_UNKNOWN_ME_OK reregister
printf 'ONU_HW_VERSION=HW-2\nOMCI_UNKNOWN_ME_OK=1\nOLT_SW_DOWNLOAD=reject\nSYSLOG_SERVER=10.0.0.1\n' > "$ODI"
reload_is "OLT_SW_DOWNLOAD and other odi keys are not a reload" none ""   none
store 1 12 5 "$X$(val GPON_PLOAM_PASSWD 3132333435)
"
reload_is "GPON_PLOAM_PASSWD"                  identity GPON_PLOAM_PASSWD reregister
store 1 12 5 "$X$(val GPON_PLOAM_PASSWD 3132333435)
$(val LOID_OLD u1)
$(val LOID u1)
"
reload_is "LOID"                               identity LOID              reregister
store 1 12 5 "$X$(val GPON_PLOAM_PASSWD 3132333435)
$(val LOID_OLD u1)
$(val LOID u1)
$(val LOID_PASSWD p1)
$(val LOID_PASSWD_OLD p1)
"
reload_is "LOID_PASSWD"                        identity LOID_PASSWD       reregister
hs_sn ODIT00000009
reload_is "GPON_SN"                            identity GPON_SN           reregister
hs_sn ODIT00000009
store 1 13 5 "$X$(val GPON_PLOAM_PASSWD 3132333436)
$(val LOID_OLD u1)
$(val LOID u1)
$(val LOID_PASSWD p1)
$(val LOID_PASSWD_OLD p1)
"
reload_is "an identity and a VLAN key together" identity "GPON_PLOAM_PASSWD,VLAN_MANU_TAG_VID" reregister
rm -f "$SW"
store 1 13 5 "$X$(val GPON_PLOAM_PASSWD 3132333436)
$(val LOID_OLD u1)
$(val LOID u1)
$(val LOID_PASSWD p1)
$(val LOID_PASSWD_OLD p1)
"
reload_is "the identity switch turned off"     identity "omci-identity.on" reregister
store 1 13 5 "$(val OMCI_SW_VER1 V9)
$(val GPON_PLOAM_PASSWD 3132333436)
$(val LOID_OLD u1)
$(val LOID u1)
$(val LOID_PASSWD p1)
$(val LOID_PASSWD_OLD p1)
"
reload_is "a report key while the switch is off is not a reload" none "" none
kill -0 "$pid" 2>/dev/null && ok "dry run: still the same process after $nrel reloads" || bad "dry run: died"
check "dry run: never restarted" "1" "$(grep -c 'event=start ' "$log")"
check "dry run: no driver verb was written" "0" "$(wc -c < "$INITLOG" | tr -d ' ')"
stop_omcid

echo
[ "$fail" -eq 0 ] && echo "reload-test.sh: all checks passed" || echo "reload-test.sh: $fail check(s) failed"
exit "$fail"
