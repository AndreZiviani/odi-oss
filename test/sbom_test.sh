#!/usr/bin/env bash
# tools/generate-sbom.sh and tools/verify-sbom.sh: a SBOM generated from a
# synthetic build tree passes the check, and each way of breaking it (a
# build tool pulled into the runtime graph, a lost license, a missing hash, a
# dangling dependency, a changed file, a stale checksum line) fails it.
# No network and no image: the "build" is a few small files in a scratch dir.
# Needs bash and python3, like the tools.
set -u
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok() { # name expected-rc command...
	n=$1 want=$2; shift 2
	"$@" >"$T/out" 2>&1; got=$?
	if [ "$got" = "$want" ]; then echo "ok    $n"; pass=$((pass + 1))
	else echo "FAIL  $n (rc $got, want $want)"; sed 's/^/        /' "$T/out"; fail=$((fail + 1)); fi
}

# The scratch build, laid out as image/build.sh would install it.
R=$T/rootfs
mkdir -p "$T/bin" "$T/assets" "$T/pkg" "$R/bin" "$R/sbin" "$R/etc/confd" "$R/lib/firmware/odi"
for n in diag omcid omcli omciprobe omcicap nv igmpd confd metricsd; do
	echo "$n bytes" > "$T/bin/$n"; cp "$T/bin/$n" "$R/bin/$n"
done
for n in index.html app.js keys.tsv; do
	echo "$n bytes" > "$T/assets/$n"; cp "$T/assets/$n" "$R/etc/confd/$n"
done
for n in dropbearmulti ip bridge; do echo "$n bytes" > "$T/pkg/$n"; cp "$T/pkg/$n" "$R/sbin/$n"; done
echo busybox bytes > "$T/busybox"; cp "$T/busybox" "$R/bin/busybox"
echo uimage bytes > "$T/uImage"
cp rootfs/skeleton/lib/firmware/odi/*.bin "$R/lib/firmware/odi/"
# a local build of confd and metricsd: no tag, so no committed pin to match
printf 'METRICSD_TAG=local\nCONFD_TAG=\n' > "$T/releases.env"

export BIN_DIR=$T/bin ASSET_DIR=$T/assets PKG_DIR=$T/pkg BUSYBOX=$T/busybox KERNEL_IMAGE=$T/uImage
G=tools/generate-sbom.sh V=tools/verify-sbom.sh
SBOM=$T/sbom.cdx.json
SBOM_STRICT=1 SBOM_IMAGE=$T/uImage $G v9.9.9 "$T/releases.env" > "$SBOM" 2>"$T/err"
ok "generates in strict mode" 0 test -s "$SBOM"
ok "passes the check" 0 $V "$SBOM"
ok "every file re-hashes in the unpacked image" 0 $V "$SBOM" --rootfs "$R"

( cd "$T" && sha256sum sbom.cdx.json 2>/dev/null || shasum -a 256 sbom.cdx.json ) > "$T/SHA256SUMS"
ok "listed in SHA256SUMS with its hash" 0 $V "$SBOM" --sums "$T/SHA256SUMS"
echo "$(printf 'f%.0s' $(seq 64))  sbom.cdx.json" > "$T/SHA256SUMS.bad"
ok "stale line in SHA256SUMS fails" 1 $V "$SBOM" --sums "$T/SHA256SUMS.bad"
echo "$(printf 'f%.0s' $(seq 64))  other.tar" > "$T/SHA256SUMS.none"
ok "SBOM missing from SHA256SUMS fails" 1 $V "$SBOM" --sums "$T/SHA256SUMS.none"

# shape facts the acceptance criteria name, asserted on the document itself
py() { python3 -I -c "$1" "$SBOM"; }
ok "build tools are excluded, runtime ones are not" 0 py '
import json, sys
b = json.load(open(sys.argv[1]))
s = {c["name"]: c["scope"] for c in b["components"]}
assert s["gcc"] == s["binutils"] == "excluded" and s["linux"] == s["busybox"] == "required", s'
ok "confd, metricsd, assets and replay tables are present" 0 py '
import json, sys
b = json.load(open(sys.argv[1]))
files = {f["name"] for c in b["components"] for f in c.get("components", [])}
for want in ("/bin/confd", "/bin/metricsd", "/etc/confd/index.html", "/lib/firmware/odi/sdkinit.bin"):
    assert want in files, want
assert "NOASSERTION" not in json.dumps(b)'

# mutate <name> <python statement on b>: a copy of the SBOM with one defect
bad() {
	n=$1 code=$2
	python3 -I -c '
import json, sys
b = json.load(open(sys.argv[1]))
exec(sys.argv[3])
json.dump(b, open(sys.argv[2], "w"))' "$SBOM" "$T/bad.json" "$code"
	ok "$n" 1 $V "$T/bad.json"
}
bad "a build tool in the runtime graph fails" \
	'b["dependencies"][0]["dependsOn"].append("gcc")'
bad "a runtime component without a license fails" \
	'[c.pop("licenses") for c in b["components"] if c["name"] == "busybox"]'
bad "NOASSERTION as a license fails" \
	'[c.update(licenses=[{"license": {"id": "NOASSERTION"}}]) for c in b["components"] if c["name"] == "confd"]'
bad "a file without a hash fails" \
	'[f.pop("hashes") for c in b["components"] if c["name"] == "metricsd" for f in c["components"]]'
bad "a dependency on an unknown ref fails" \
	'b["dependencies"][0]["dependsOn"].append("nothing@0")'
bad "a runtime component outside the graph fails" \
	'b["dependencies"][0]["dependsOn"].remove("busybox")'
bad "a missing required component fails" \
	'b["components"] = [c for c in b["components"] if c["name"] != "confd-assets"]'
bad "a missing build/runtime distinction fails" \
	'[c.update(scope="required") for c in b["components"]]'
bad "a duplicate bom-ref fails" \
	'b["components"][1]["bom-ref"] = b["components"][0]["bom-ref"]'

echo tampered >> "$R/etc/confd/index.html"
ok "a changed file in the image fails" 1 $V "$SBOM" --rootfs "$R"
cp "$T/assets/index.html" "$R/etc/confd/index.html"
rm "$R/lib/firmware/odi/sdkinit.bin"
ok "a missing file in the image fails" 1 $V "$SBOM" --rootfs "$R"

# generator guards
rm "$T/pkg/ip"
ok "strict mode refuses a missing artifact" 1 env SBOM_STRICT=1 $G v9.9.9 "$T/releases.env"
ok "non-strict mode warns and goes on" 0 $G v9.9.9 "$T/releases.env"
ok "a SBOM with a hole does not pass the check" 1 sh -c "$G v9.9.9 $T/releases.env > $T/hole.json && $V $T/hole.json"
cp "$T/busybox" "$T/pkg/ip"
printf 'METRICSD_TAG=v1.8.0\nCONFD_TAG=\n' > "$T/pinned.env"
ok "a metricsd that is not the committed pin fails" 1 $G v9.9.9 "$T/pinned.env"

echo "sbom_test: $pass ok, $fail failed"
[ "$fail" = 0 ]
