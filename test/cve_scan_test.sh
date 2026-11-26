#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later
# tools/cve-scan.sh against a made-up kernel CVE feed (a local git repository
# in the shape of vulns.git) and saved NVD answers: which kernel CVEs it
# lists for a version and a source list, the report it writes, the fetch of
# a feed it already has, and the cases where the scan cannot run. No network.
set -u
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok() { # name command...
	n=$1; shift
	if "$@"; then echo "ok    $n"; pass=$((pass + 1))
	else echo "FAIL  $n"; sed 's/^/        /' "$T/out"; fail=$((fail + 1)); fi
}
g() { git -C "$T/src" -c user.name=t -c user.email=t@t -c commit.gpgsign=false "$@"; }

# rec ID STATE FILES VERSIONS [ADP-SEVERITIES]: a record of the feed. FILES
# is a JSON list, VERSIONS the version ranges of the Linux product, and
# ADP-SEVERITIES the scores CISA gives it, space-separated.
rec() {
	mkdir -p "$T/src/cve/published/2026"
	adp=
	if [ -n "${5:-}" ]; then
		# shellcheck disable=SC2086 # one printf argument per score
		adp=$(printf '{"cvssV3_1": {"baseSeverity": "%s"}},' $5)
		adp=', "adp": [{"metrics": ['"${adp%,}"']}]'
	fi
	printf '{"cveMetadata": {"cveId": "%s", "state": "%s"}, "containers": {"cna": {"affected": [
	  {"product": "Linux", "defaultStatus": "unaffected", "programFiles": %s,
	   "versions": [{"version": "abc", "lessThan": "def", "status": "affected", "versionType": "git"}]},
	  {"product": "Linux", "defaultStatus": "affected", "programFiles": %s, "versions": [%s]}]}%s}}\n' \
		"$1" "$2" "$3" "$3" "$4" "$adp" > "$T/src/cve/published/2026/$1.json"
}
intro='{"version": "6.15", "status": "affected"}, {"version": "0", "lessThan": "6.15", "status": "unaffected", "versionType": "semver"}'
mainline='{"version": "7.1", "lessThanOrEqual": "*", "status": "unaffected", "versionType": "original_commit_for_fix"}'

git init -q -b master "$T/src"
# fixed on the 6.18 branch before 6.18.65: not listed
rec CVE-2026-0001 PUBLISHED '["net/core/dev.c"]' "$intro, "'{"version": "6.18.57", "lessThanOrEqual": "6.18.*", "status": "unaffected", "versionType": "semver"}'", $mainline"
# affects 6.18.65 in a built file, fixed later on the branch: listed
rec CVE-2026-0002 PUBLISHED '["net/core/dev.c"]' "$intro, "'{"version": "6.18.70", "lessThanOrEqual": "6.18.*", "status": "unaffected", "versionType": "semver"}'", $mainline" HIGH
# only in a file this kernel does not build: left out, counted
rec CVE-2026-0003 PUBLISHED '["drivers/net/x/unbuilt.c"]' "$intro, $mainline"
# a .c that is not compiled, and a header that is: left out, counted (the fix
# edits a struct the header shares, the code at fault is not built)
rec CVE-2026-0004 PUBLISHED '["drivers/net/x/unbuilt.c", "include/linux/x.h"]' "$intro, $mainline"
# a header and a .c, neither compiled in: left out, counted
rec CVE-2026-0009 PUBLISHED '["drivers/net/x/unbuilt.c", "include/linux/unused.h"]' "$intro, $mainline"
# headers only, one compiled in: listed
rec CVE-2026-0010 PUBLISHED '["include/linux/x.h"]' "$intro, $mainline"
# scored twice by CISA: the worse of the two
rec CVE-2026-0011 PUBLISHED '["net/core/dev.c"]' "$intro, $mainline" "MEDIUM CRITICAL"
# rejected: not listed
rec CVE-2026-0005 REJECTED '["net/core/dev.c"]' "$intro, $mainline"
# fixed on the 6.12 branch only (a number above 6.18.65 there): still listed
rec CVE-2026-0008 PUBLISHED '["net/core/dev.c"]' "$intro, "'{"version": "6.12.100", "lessThanOrEqual": "6.12.*", "status": "unaffected", "versionType": "semver"}'", $mainline"
# from 6.19 on only: not listed
rec CVE-2026-0006 PUBLISHED '["net/core/dev.c"]' '{"version": "6.19", "status": "affected"}, {"version": "0", "lessThan": "6.19", "status": "unaffected", "versionType": "semver"}'", $mainline"
g add -A && g commit -q -m feed

sbom() { # PROPERTIES
	printf '{"metadata": {"component": {"version": "v9.9.9"}}, "components": [
	  {"name": "linux", "version": "6.18.65", "cpe": "cpe:2.3:o:linux:linux_kernel:6.18.65:*:*:*:*:*:*:*", "properties": [%s]},
	  {"name": "busybox", "version": "1.38.0", "cpe": "cpe:2.3:a:busybox:busybox:1.38.0:*:*:*:*:*:*:*", "properties": []},
	  {"name": "dropbear", "version": "2026.94", "cpe": "cpe:2.3:a:dropbear_ssh_project:dropbear_ssh:2026.94:*:*:*:*:*:*:*", "properties": []}]}\n' "$1" > "$T/sbom.json"
}
sbom '{"name": "odi:kernel-sources", "value": "include/linux/x.h init/main.c net/core/dev.c"}'
mkdir -p "$T/nvd"
# The file name is the CPE with every character outside [A-Za-z0-9._-] as _.
nvdf=$T/nvd/$(printf %s 'cpe:2.3:a:busybox:busybox:1.38.0:*:*:*:*:*:*:*' | sed 's/[^A-Za-z0-9._-]/_/g').json
cat > "$nvdf" <<'EOF'
{"vulnerabilities": [{"cve": {"id": "CVE-2026-0100",
  "metrics": {"cvssMetricV31": [{"type": "Secondary", "cvssData": {"baseSeverity": "HIGH"}},
                                {"type": "Primary", "cvssData": {"baseSeverity": "MEDIUM"}}]},
  "configurations": [{"nodes": [{"cpeMatch": [{"vulnerable": true,
    "criteria": "cpe:2.3:a:busybox:busybox:*:*:*:*:*:*:*:*", "versionEndExcluding": "1.38.1"}]}]}]}}]}
EOF
nvdf=$T/nvd/$(printf %s 'cpe:2.3:a:dropbear_ssh_project:dropbear_ssh:2026.94:*:*:*:*:*:*:*' | sed 's/[^A-Za-z0-9._-]/_/g').json
cat > "$nvdf" <<'EOF'
{"vulnerabilities": [{"cve": {"id": "CVE-2026-0101",
  "metrics": {"cvssMetricV31": [{"type": "Secondary", "cvssData": {"baseSeverity": "MEDIUM"}},
                                {"type": "Secondary", "cvssData": {"baseSeverity": "HIGH"}}]}}}]}
EOF
export CVE_FEED_URL=$T/src CVE_FEED_DIR=$T/cache/vulns.git NVD_FILES=$T/nvd

bash tools/cve-scan.sh "$T/sbom.json" > "$T/out" 2>&1; rc=$?
ok "a match does not fail the scan" [ "$rc" = 0 ]
ok "the report names the release" grep -q '^## Known vulnerabilities in the v9.9.9 SBOM$' "$T/out"
ok "a CVE that affects a built file is listed, with the fix on its branch" \
	grep -qx '| linux | 6.18.65 | CVE-2026-0002 | High | 6.18.70 |' "$T/out"
ok "a fix on another stable branch does not count for this one" \
	grep -qx '| linux | 6.18.65 | CVE-2026-0008 | Unknown | 7.1 |' "$T/out"
ok "a CVE touching only headers, one compiled in, is listed" grep -q '| CVE-2026-0010 | Unknown | 7.1 |' "$T/out"
ok "a kernel CVE scored twice gets the worse score" grep -q '| CVE-2026-0011 | Critical | 7.1 |' "$T/out"
ok "fixed on the branch, rejected, or newer than the version: not listed" \
	sh -c "! grep -q -e CVE-2026-0001 -e CVE-2026-0005 -e CVE-2026-0006 '$T/out'"
ok "CVEs whose code is not compiled in are left out and counted, also with a header that is" \
	sh -c "! grep -q -e CVE-2026-0003 -e CVE-2026-0004 -e CVE-2026-0009 '$T/out' && grep -q '^3 more kernel CVEs affect 6.18.65 only in files this kernel does not compile (it compiles 3)' '$T/out'"
ok "the NVD match of the other components, with the NVD score over a CNA one, and the fixed version" \
	grep -qx '| busybox | 1.38.0 | CVE-2026-0100 | Medium | 1.38.1 |' "$T/out"
ok "with only CNA and CISA scores, the worse of them" grep -qx '| dropbear | 2026.94 | CVE-2026-0101 | High | - |' "$T/out"
ok "counts per component and severity" grep -qx '| linux | 1 | 1 | 0 | 0 | 0 | 2 |' "$T/out"
ok "the worst severity comes first" sh -c "grep -n 'CVE-' '$T/out' | head -1 | grep -q CVE-2026-0011"

# A feed already there is fetched, not cloned again: a new record shows up.
rec CVE-2026-0007 PUBLISHED '["init/main.c"]' "$intro, $mainline"
g add -A && g commit -q -m more
bash tools/cve-scan.sh "$T/sbom.json" > "$T/out" 2>&1; rc=$?
ok "a later run fetches what the feed added" sh -c "[ $rc = 0 ] && grep -q CVE-2026-0007 '$T/out'"

# busybox carries a patch for CVE-2026-0100 (tools/sbom.py pedigree)
sed 's/"name": "busybox", "version": "1.38.0",/&  "pedigree": {"patches": [{"type": "unofficial", "resolves": [{"type": "security", "id": "CVE-2026-0100"}]}]},/' \
	"$T/sbom.json" > "$T/patched.json"
bash tools/cve-scan.sh "$T/patched.json" > "$T/out" 2>&1; rc=$?
ok "a CVE a patch here fixes is named apart" \
	sh -c "[ $rc = 0 ] && grep -qx 'Fixed by a patch in this tree, not counted: CVE-2026-0100 busybox (1.38.0).' '$T/out'"
ok "and neither listed nor counted" sh -c "! grep -q -e '| CVE-2026-0100 |' -e '^| busybox |' '$T/out'"

sbom ''
bash tools/cve-scan.sh "$T/sbom.json" > "$T/out" 2>&1; rc=$?
ok "with no source list every CVE of the version is listed, and the report says so" \
	sh -c "[ $rc = 0 ] && grep -q CVE-2026-0003 '$T/out' && grep -q 'does not list the kernel sources' '$T/out'"

bash tools/cve-scan.sh "$T/none.json" > "$T/out" 2>&1; rc=$?
ok "a missing SBOM fails" [ "$rc" = 1 ]
CVE_FEED_URL=$T/nowhere CVE_FEED_DIR=$T/cache2/vulns.git bash tools/cve-scan.sh "$T/sbom.json" > "$T/out" 2>&1; rc=$?
ok "a feed that cannot be fetched fails the scan" [ "$rc" = 1 ]
rm "$T/nvd/"*.json
bash tools/cve-scan.sh "$T/sbom.json" > "$T/out" 2>&1; rc=$?
ok "an NVD that does not answer fails the scan" [ "$rc" = 1 ]

echo "cve_scan_test: $pass ok, $fail failed"
[ "$fail" = 0 ]
