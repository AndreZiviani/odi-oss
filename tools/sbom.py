#!/usr/bin/env python3
"""CycloneDX 1.5 SBOM of a release image: generate it, and check it.

    tools/sbom.py generate <version> [--releases-env FILE] [--strict] [--image TAR]
    tools/sbom.py verify <sbom.cdx.json> [--rootfs DIR] [--sums SHA256SUMS]

tools/generate-sbom.sh and tools/verify-sbom.sh are the entry points the
release job and docs/FLASHING.md name; both call this file.

Why python3 and not shell: the SBOM now carries hashes, nested components and
a dependency graph, and the check walks that graph. Neither is something to
build with printf. python3 and nothing but its standard library, so a host
that can run `make test-host` can run this, and nothing is downloaded.

What the SBOM says (docs/LICENSING.md has the prose):

  runtime   everything on the flashed rootfs: the kernel and our drivers, the
            statically linked uClibc-ng, busybox, dropbear, iproute2, our own
            binaries, confd and metricsd, the confd web assets, and the
            register replay tables the kernel loads at boot. scope "required".
  build     gcc and binutils: they build the image and do not ship. scope
            "excluded" and property odi:role=build; nothing at runtime
            depends on them, and verify refuses an SBOM that says otherwise.

Every file that lands on the rootfs is a nested component of type "file" with
its SHA-256 and odi:path, the path it has in the image, so
`verify --rootfs <unpacked image>` re-hashes the lot. A pinned upstream
source tarball is an externalReference with its hash. The versions come from
the pins already in the tree (kernel/618/fetch.sh, packages/*/build.sh,
toolchain/images.env, src/fetch-releases.sh, src/release-sha256.txt): this
script reads them and carries no copy.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys
import uuid

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT_REF = "odi-oss-firmware"
OWN = "AndreZiviani/odi-oss"


def die(msg):
    print("sbom.py: " + msg, file=sys.stderr)
    sys.exit(1)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def pin(rel, key, pattern=r"[0-9][0-9.]*"):
    m = re.search(r"^%s=(%s)\s*$" % (re.escape(key), pattern), read(rel), re.M)
    if not m:
        die("no %s= pin in %s" % (key, rel))
    return m.group(1)


def package_pins(name):
    rel = "packages/%s/build.sh" % name
    ver = pin(rel, "VERSION")
    url = pin(rel, "URL", r"\S+").replace("$VERSION", ver)
    return ver, url, pin(rel, "SHA256", r"[0-9a-f]{64}")


def release_pins():
    """(repo, tag, asset) -> sha256 from src/release-sha256.txt."""
    pins = {}
    for line in read("src/release-sha256.txt").splitlines():
        f = line.split()
        if len(f) == 4 and not f[0].startswith("#"):
            pins[(f[0], f[1], f[2])] = f[3]
    return pins


def default_tag(var):
    m = re.search(r"^%s=\$\{%s:?-(v[0-9.]+)\}" % (var, var), read("src/fetch-releases.sh"), re.M)
    if not m:
        die("no default %s in src/fetch-releases.sh" % var)
    return m.group(1)


def env_file(path):
    out = {}
    if path and os.path.isfile(path):
        for line in open(path, encoding="utf-8"):
            k, sep, v = line.strip().partition("=")
            if sep:
                out[k] = v
    return out


class Builder:
    def __init__(self, strict):
        self.strict = strict
        self.deps = {}  # bom-ref -> [bom-ref]

    def missing(self, what):
        # Strict (the release job): an artifact that should be hashed and is
        # not there fails the build rather than ship an SBOM with a hole.
        if self.strict:
            die("%s is missing: strict mode refuses an SBOM without its hash" % what)
        print("sbom.py: warning: %s is missing, no hash recorded" % what, file=sys.stderr)

    def file_comp(self, path, installed, sha=None):
        """A file component: installed is its path in the image."""
        c = {
            "type": "file",
            "bom-ref": "file:" + installed,
            "name": installed,
            "scope": "required",
            "properties": [{"name": "odi:path", "value": installed}],
        }
        if sha is None and path and os.path.isfile(path):
            sha = sha256_file(path)
        if sha is None:
            self.missing(installed)
        else:
            c["hashes"] = [{"alg": "SHA-256", "content": sha}]
        return c

    def edge(self, ref, *to):
        self.deps.setdefault(ref, [])
        for t in to:
            if t not in self.deps[ref]:
                self.deps[ref].append(t)


def lic(spdx):
    if " " in spdx:
        return [{"expression": spdx}]
    return [{"license": {"id": spdx}}]


def comp(name, ver, spdx, supplier, purl=None, ref=None, scope="required", role="runtime", ctype="library"):
    c = {
        "type": ctype,
        "bom-ref": ref or ("%s@%s" % (name, ver)),
        "name": name,
        "version": ver,
        "scope": scope,
        "supplier": {"name": supplier},
        "licenses": lic(spdx),
        "properties": [{"name": "odi:role", "value": role}],
    }
    if purl:
        c["purl"] = purl
    return c


def source_ref(url, sha):
    return {"type": "source-distribution", "url": url, "hashes": [{"alg": "SHA-256", "content": sha}]}


def generate(args):
    version = args.version
    env = env_file(args.releases_env or os.path.join(ROOT, "out/bin/releases.env"))
    bin_dir = os.environ.get("BIN_DIR", os.path.join(ROOT, "out/bin"))
    asset_dir = os.environ.get("ASSET_DIR", os.path.join(ROOT, "out/confd-assets"))
    pkg_dir = os.environ.get("PKG_DIR", os.path.join(ROOT, "out"))
    busybox = os.environ.get("BUSYBOX", os.path.join(ROOT, "out/busybox"))
    b = Builder(args.strict)

    # --- pins -------------------------------------------------------------
    kernel_ver = pin("kernel/618/fetch.sh", "VERSION")
    kernel_url = "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-%s.tar.xz" % kernel_ver
    kernel_sha = pin("kernel/618/fetch.sh", "SHA256_XZ", r"[0-9a-f]{64}")
    tl = re.search(r"uClibc-ng toolchain \(binutils ([0-9.]+), gcc ([0-9.]+), uClibc-ng ([0-9.]+)",
                   read("toolchain/images.env"))
    if not tl:
        die("no uClibc-ng toolchain line in toolchain/images.env")
    binutils_ver, gcc_ver, uclibc_ver = tl.groups()
    pins = release_pins()
    metricsd_tag = env.get("METRICSD_TAG") or default_tag("METRICSD_TAG")
    # CONFD_TAG may be set and empty (a local build, src/fetch-releases.sh).
    confd_tag = env["CONFD_TAG"] if "CONFD_TAG" in env else default_tag("CONFD_TAG")
    confd_tag = confd_tag or "local"
    exporter_repo, ui_repo = "AndreZiviani/odi-sfp-exporter", "AndreZiviani/odi-ui"

    comps = []

    # --- kernel and our drivers, the replay tables --------------------------
    kernel = comp("linux", kernel_ver, "GPL-2.0-only", "The Linux kernel community (kernel.org)",
                  "pkg:generic/linux@" + kernel_ver, ref="linux")
    kernel["externalReferences"] = [source_ref(kernel_url, kernel_sha)]
    kimage = os.environ.get("KERNEL_IMAGE", os.path.join(ROOT, "build/kernel-618/uImage"))
    if os.path.isfile(kimage):
        kernel["hashes"] = [{"alg": "SHA-256", "content": sha256_file(kimage)}]
        kernel["properties"].append({"name": "odi:path", "value": "uImage (in the image tarball)"})
    else:
        b.missing("kernel image " + os.path.basename(kimage))
    drivers = comp("odi-oss-kernel-drivers", version, "GPL-2.0-or-later", OWN,
                   "pkg:github/%s@%s" % (OWN, version), ref="odi-oss-kernel-drivers")
    drivers["description"] = "our own drivers and patches, built into the kernel (CONFIG_ODI_*=y); no modules"
    drivers["properties"].append({"name": "odi:embedded", "value": "built-into-kernel"})

    tables = comp("odi-register-replay-tables", version, "GPL-2.0-or-later", OWN,
                  "pkg:github/%s@%s#rootfs/skeleton/lib/firmware/odi" % (OWN, version),
                  ref="odi-register-replay-tables", ctype="data")
    tables["description"] = "register write lists the kernel loads with request_firmware() (docs/LICENSING.md)"
    tables["components"] = []
    fw = os.path.join(ROOT, "rootfs/skeleton/lib/firmware/odi")
    for n in sorted(os.listdir(fw)):
        if n.endswith(".bin"):
            tables["components"].append(b.file_comp(os.path.join(fw, n), "/lib/firmware/odi/" + n))
    if not tables["components"]:
        die("no replay tables under rootfs/skeleton/lib/firmware/odi")

    # --- the libc and the upstream packages ---------------------------------
    uclibc = comp("uClibc-ng", uclibc_ver, "LGPL-2.1-only", "uClibc-ng project",
                  "pkg:generic/uclibc-ng@" + uclibc_ver, ref="uClibc-ng")
    uclibc["properties"].append({"name": "odi:embedded", "value": "static-link"})
    uclibc["description"] = "linked statically into every target binary, so it has no file of its own"

    pkgs = []
    bb_ver, bb_url, bb_sha = package_pins("busybox")
    bb = comp("busybox", bb_ver, "GPL-2.0-only", "busybox.net", "pkg:generic/busybox@" + bb_ver, ref="busybox")
    bb["externalReferences"] = [source_ref(bb_url, bb_sha)]
    bb["components"] = [b.file_comp(busybox, "/bin/busybox")]
    pkgs.append(bb)
    db_ver, db_url, db_sha = package_pins("dropbear")
    db = comp("dropbear", db_ver, "MIT", "Matt Johnston", "pkg:generic/dropbear@" + db_ver, ref="dropbear")
    db["externalReferences"] = [source_ref(db_url, db_sha)]
    db["components"] = [b.file_comp(os.path.join(pkg_dir, "dropbearmulti"), "/sbin/dropbearmulti")]
    pkgs.append(db)
    ip_ver, ip_url, ip_sha = package_pins("iproute2")
    ip = comp("iproute2", ip_ver, "GPL-2.0-only", "kernel.org", "pkg:generic/iproute2@" + ip_ver, ref="iproute2")
    ip["externalReferences"] = [source_ref(ip_url, ip_sha)]
    ip["components"] = [b.file_comp(os.path.join(pkg_dir, n), "/sbin/" + n) for n in ("ip", "bridge")]
    pkgs.append(ip)

    # --- our own userland ---------------------------------------------------
    userland = comp("odi-oss-userland", version, "GPL-2.0-or-later", OWN,
                    "pkg:github/%s@%s" % (OWN, version), ref="odi-oss-userland")
    userland["description"] = "diag, omcid, omcli, omciprobe, omcicap, nv, igmpd, and the rootfs skeleton"
    userland["components"] = [b.file_comp(os.path.join(bin_dir, n), "/bin/" + n)
                              for n in ("diag", "omcid", "omcli", "omciprobe", "omcicap", "nv", "igmpd")]

    # --- confd and metricsd: separate releases, hashes from the committed pins
    def release_comp(name, repo, tag, supplier):
        c = comp(name, tag, "GPL-2.0-or-later", supplier, "pkg:github/%s@%s" % (repo, tag), ref=name)
        asset = os.path.join(bin_dir, name)
        sha = pins.get((repo, tag, name))
        have = sha256_file(asset) if os.path.isfile(asset) else None
        if sha and have and sha != have:
            die("%s in %s is %s but src/release-sha256.txt pins %s" % (name, bin_dir, have, sha))
        # A local build has no pin: the file is the only evidence there is.
        sha = sha or have
        c["components"] = [b.file_comp(asset, "/bin/" + name, sha)]
        if tag != "local":
            c["externalReferences"] = [{"type": "distribution",
                                        "url": "https://github.com/%s/releases/tag/%s" % (repo, tag)}]
        return c

    metricsd = release_comp("metricsd", exporter_repo, metricsd_tag, exporter_repo)
    confd = release_comp("confd", ui_repo, confd_tag, ui_repo)
    assets = comp("confd-assets", confd_tag, "GPL-2.0-or-later", ui_repo,
                  "pkg:github/%s@%s#web" % (ui_repo, confd_tag), ref="confd-assets", ctype="data")
    assets["description"] = "the web UI and the .tsv tables confd reads from /etc/confd"
    tgz_sha = pins.get((ui_repo, confd_tag, "confd-assets.tar.gz"))
    if tgz_sha:
        assets["externalReferences"] = [{"type": "distribution",
                                         "url": "https://github.com/%s/releases/download/%s/confd-assets.tar.gz"
                                                % (ui_repo, confd_tag),
                                         "hashes": [{"alg": "SHA-256", "content": tgz_sha}]}]
    assets["components"] = []
    if os.path.isdir(asset_dir):
        for n in sorted(os.listdir(asset_dir)):
            if os.path.isfile(os.path.join(asset_dir, n)):
                assets["components"].append(b.file_comp(os.path.join(asset_dir, n), "/etc/confd/" + n))
    if not assets["components"]:
        b.missing("confd assets in " + asset_dir)

    # --- build tools: named, excluded, referenced by nothing at runtime -----
    gcc = comp("gcc", gcc_ver, "GPL-3.0-only WITH GCC-exception-3.1", "GNU Project",
               "pkg:generic/gcc@" + gcc_ver, ref="gcc", scope="excluded", role="build")
    binutils = comp("binutils", binutils_ver, "GPL-3.0-only", "GNU Project",
                    "pkg:generic/binutils@" + binutils_ver, ref="binutils", scope="excluded", role="build")

    runtime = [kernel, drivers, tables, uclibc] + pkgs + [userland, metricsd, confd, assets]
    comps = runtime + [gcc, binutils]

    # --- the dependency graph ----------------------------------------------
    b.edge(ROOT_REF, *[c["bom-ref"] for c in runtime if c["bom-ref"] not in ("odi-oss-kernel-drivers",
                                                                             "odi-register-replay-tables",
                                                                             "confd-assets")])
    b.edge("linux", "odi-oss-kernel-drivers")
    b.edge("odi-oss-kernel-drivers", "odi-register-replay-tables")
    # every target binary is linked statically against uClibc-ng
    for r in ("busybox", "dropbear", "iproute2", "odi-oss-userland", "metricsd", "confd"):
        b.edge(r, "uClibc-ng")
    b.edge("confd", "confd-assets")
    b.edge("uClibc-ng")
    b.edge("confd-assets")
    b.edge("odi-register-replay-tables")
    for c in comps:
        b.deps.setdefault(c["bom-ref"], [])

    root = {
        "type": "firmware",
        "bom-ref": ROOT_REF,
        "name": "odi-oss",
        "version": version,
        "purl": "pkg:github/%s@%s" % (OWN, version),
    }
    if args.image:
        if os.path.isfile(args.image):
            root["hashes"] = [{"alg": "SHA-256", "content": sha256_file(args.image)}]
        else:
            b.missing("image tarball " + args.image)

    # serialNumber from the version and the commit, not random: the same
    # source gives the same SBOM, apart from the timestamp.
    try:
        src = subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD"], capture_output=True, text=True,
                             timeout=30, check=True).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        src = "nogit"
    serial = uuid.uuid5(uuid.NAMESPACE_URL, "odi-oss %s %s" % (version, src))

    bom = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "serialNumber": "urn:uuid:%s" % serial,
        "version": 1,
        "metadata": {
            "timestamp": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "component": root,
            "properties": [{"name": "odi:commit", "value": src}],
        },
        "components": comps,
        "dependencies": [{"ref": r, "dependsOn": d} for r, d in b.deps.items()],
    }
    json.dump(bom, sys.stdout, indent=2)
    sys.stdout.write("\n")


# ------------------------------------------------------------------ verify

REQUIRED = ["linux", "uClibc-ng", "busybox", "dropbear", "iproute2", "metricsd", "confd", "confd-assets",
            "odi-oss-kernel-drivers", "odi-register-replay-tables", "odi-oss-userland"]


def walk(components, parent=None):
    for c in components or []:
        yield c, parent
        yield from walk(c.get("components"), c)


def prop(c, name):
    for p in c.get("properties", []):
        if p.get("name") == name:
            return p.get("value")
    return None


def sha_of(c):
    for h in c.get("hashes", []):
        if h.get("alg") == "SHA-256" and re.fullmatch(r"[0-9a-f]{64}", h.get("content", "")):
            return h["content"]
    return None


def verify(args):
    errs = []

    def bad(msg):
        errs.append(msg)

    try:
        with open(args.sbom, encoding="utf-8") as f:
            bom = json.load(f)
    except (OSError, ValueError) as e:
        die("cannot read %s: %s" % (args.sbom, e))
    if bom.get("bomFormat") != "CycloneDX" or bom.get("specVersion") != "1.5":
        bad("not a CycloneDX 1.5 document")
    root = bom.get("metadata", {}).get("component", {})
    rref = root.get("bom-ref")
    if not rref:
        bad("metadata.component has no bom-ref")

    allc = list(walk(bom.get("components")))
    refs = {}
    for c, _ in allc:
        r = c.get("bom-ref")
        if not r:
            bad("component %s has no bom-ref" % c.get("name"))
        elif r in refs:
            bad("duplicate bom-ref %s" % r)
        else:
            refs[r] = c
    if rref:
        refs.setdefault(rref, root)

    graph = {}
    for d in bom.get("dependencies", []):
        if d.get("ref") not in refs:
            bad("dependency entry for unknown ref %s" % d.get("ref"))
        graph[d.get("ref")] = d.get("dependsOn", [])
        for t in graph[d.get("ref")]:
            if t not in refs:
                bad("%s depends on unknown ref %s" % (d.get("ref"), t))
    reach, todo = set(), [rref]
    while todo:
        r = todo.pop()
        if r in reach:
            continue
        reach.add(r)
        todo.extend(graph.get(r, []))

    names = {c.get("name") for c, _ in allc}
    for n in REQUIRED:
        if n not in names:
            bad("required component %s is missing" % n)

    runtime = build = 0
    for c, parent in allc:
        n = c.get("name")
        if c.get("scope") == "excluded":
            build += 1
            if prop(c, "odi:role") != "build":
                bad("%s is excluded but not marked odi:role=build" % n)
            if c.get("bom-ref") in reach:
                bad("build component %s is reachable from the firmware: it would be a runtime dependency" % n)
            continue
        if c.get("type") == "file":
            if parent is None:
                bad("file %s is not nested in a component" % n)
            if not sha_of(c):
                bad("file %s has no SHA-256" % n)
            if not (prop(c, "odi:path") or "").startswith("/"):
                bad("file %s has no odi:path" % n)
            continue
        runtime += 1
        if not c.get("version"):
            bad("%s has no version" % n)
        if not c.get("purl"):
            bad("%s has no purl" % n)
        ls = c.get("licenses") or []
        if not ls or "NOASSERTION" in json.dumps(ls):
            bad("%s has no license (or NOASSERTION)" % n)
        if c.get("bom-ref") not in reach:
            bad("runtime component %s is not reachable from the firmware in the dependency graph" % n)
        files = [x for x in c.get("components", []) if x.get("type") == "file"]
        if not (sha_of(c) or files or prop(c, "odi:embedded") in ("static-link", "built-into-kernel")):
            bad("%s has no hash: no SHA-256, no file, not marked as embedded in another artifact" % n)
    if not runtime:
        bad("no runtime component")
    if not build:
        bad("no build component: runtime and build dependencies are not told apart")

    if args.rootfs:
        n = 0
        for c, _ in allc:
            p = prop(c, "odi:path")
            if c.get("type") != "file" or not p or not sha_of(c):
                continue
            f = os.path.join(args.rootfs, p.lstrip("/"))
            if not os.path.isfile(f):
                bad("%s is not in %s" % (p, args.rootfs))
            elif sha256_file(f) != sha_of(c):
                bad("%s differs from the SBOM" % p)
            n += 1
        if not n:
            bad("no file to check under %s" % args.rootfs)

    if args.sums:
        want = sha256_file(args.sbom)
        base = os.path.basename(args.sbom)
        found = False
        try:
            lines = open(args.sums, encoding="utf-8").read().splitlines()
        except OSError as e:
            die("cannot read %s: %s" % (args.sums, e))
        for line in lines:
            f = line.split(None, 1)
            if len(f) == 2 and os.path.basename(f[1].lstrip("*").strip()) == base:
                found = True
                if f[0] != want:
                    bad("%s does not match its line in %s" % (base, args.sums))
        if not found:
            bad("%s has no line in %s" % (base, args.sums))

    if errs:
        for e in errs:
            print("verify-sbom: " + e, file=sys.stderr)
        sys.exit(1)
    print("verify-sbom: %s OK (%d runtime, %d build, %d files)" % (
        args.sbom, runtime, build, sum(1 for c, _ in allc if c.get("type") == "file")))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("generate")
    g.add_argument("version")
    g.add_argument("--releases-env")
    g.add_argument("--strict", action="store_true")
    g.add_argument("--image")
    v = sub.add_parser("verify")
    v.add_argument("sbom")
    v.add_argument("--rootfs")
    v.add_argument("--sums")
    args = ap.parse_args()
    {"generate": generate, "verify": verify}[args.cmd](args)


if __name__ == "__main__":
    main()
