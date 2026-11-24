#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later
# tools/cve-scan.sh turns grype's matches into the report: counts per
# component and severity, every match once, the CVEs a patch here fixes
# apart, and no failure on a match. A stub stands in for grype, so there is
# no network and no database.
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

echo '{"metadata": {"component": {"version": "v9.9.9"}}}' > "$T/sbom.json"
cat > "$T/grype" <<'EOF'
#!/bin/sh
[ "$1" = "sbom:$SBOM" ] || { echo "stub grype: got $1" >&2; exit 9; }
cat "$MATCHES"
EOF
chmod +x "$T/grype"
m() { # name version id severity fixed
	printf '{"artifact": {"name": "%s", "version": "%s"}, "vulnerability": {"id": "%s", "severity": "%s", "fix": {"versions": [%s]}}}' "$@"
}
{
	printf '{"matches": ['
	m busybox 1.37.0 CVE-2025-0001 High '"1.37.1"'; printf ,
	m busybox 1.37.0 CVE-2025-0002 Low ''; printf ,
	m linux 6.18.65 CVE-2026-1000 Medium ''; printf ,
	# grype can list one CVE twice, once per way it matched
	m linux 6.18.65 CVE-2026-1000 Medium ''
	printf ']}\n'
} > "$T/matches.json"
export GRYPE=$T/grype SBOM=$T/sbom.json MATCHES=$T/matches.json

bash tools/cve-scan.sh "$T/sbom.json" > "$T/out" 2>&1; rc=$?
ok "a match does not fail the scan" [ "$rc" = 0 ]
ok "the report names the release" grep -q '^## Known vulnerabilities in the v9.9.9 SBOM$' "$T/out"
ok "counts per component and severity" grep -qx '| busybox | 0 | 1 | 0 | 1 | 0 | 0 |' "$T/out"
ok "a match listed twice counts once" grep -qx '| linux | 0 | 0 | 1 | 0 | 0 | 0 |' "$T/out"
ok "the fixed version is shown" grep -qx '| busybox | 1.37.0 | CVE-2025-0001 | High | 1.37.1 |' "$T/out"
ok "the worst severity comes first" sh -c "grep -n 'CVE-' '$T/out' | head -1 | grep -q CVE-2025-0001"

# busybox carries a patch for CVE-2025-0001 (tools/sbom.py pedigree)
echo '{"metadata": {"component": {"version": "v9.9.9"}}, "components": [{"name": "busybox",
  "pedigree": {"patches": [{"type": "unofficial", "resolves": [{"type": "security", "id": "CVE-2025-0001"}]}]}}]}' > "$T/sbom.json"
bash tools/cve-scan.sh "$T/sbom.json" > "$T/out" 2>&1
ok "a CVE a patch here fixes is named apart" grep -qx 'Fixed by a patch in this tree, not counted: CVE-2025-0001 busybox (1.37.0).' "$T/out"
ok "and not counted" grep -qx '| busybox | 0 | 0 | 0 | 1 | 0 | 0 |' "$T/out"
ok "nor listed as a match" sh -c "! grep -q '| CVE-2025-0001 |' '$T/out'"
echo '{"metadata": {"component": {"version": "v9.9.9"}}}' > "$T/sbom.json"

echo '{"matches": []}' > "$T/matches.json"
bash tools/cve-scan.sh "$T/sbom.json" > "$T/out" 2>&1; rc=$?
ok "no match says so" sh -c "[ $rc = 0 ] && grep -qx 'No match.' '$T/out'"

bash tools/cve-scan.sh "$T/none.json" > "$T/out" 2>&1; rc=$?
ok "a missing SBOM fails" [ "$rc" = 1 ]
printf '#!/bin/sh\nexit 1\n' > "$T/grype"
bash tools/cve-scan.sh "$T/sbom.json" > "$T/out" 2>&1; rc=$?
ok "a scan that does not run fails" [ "$rc" != 0 ]

echo "cve_scan_test: $pass ok, $fail failed"
[ "$fail" = 0 ]
