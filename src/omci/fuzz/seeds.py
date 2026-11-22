#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

"""Build the seed corpus of the fuzz targets from what the repo already holds.

    seeds.py <target> <out-dir>

Nothing binary is kept in the tree. The inputs of a fuzz target are bytes,
but everything they are made from is text that can be read and reviewed:

  corpus/<target>/*.txt   inputs written by hand, and every input that once
                          broke omcid (the regression corpus). Hex, any
                          spacing, `#` starts a comment. A comment line
                          `# expect: <text>` is something omcid has to
                          print when the input runs (replay.c checks it;
                          it goes to <outdir>.expect/<name>).
  test/fixtures/omci-session-*.txt
                          the OLT side of whole provisioning sessions, one
                          frame a line, already scrubbed of identities
                          (their headers say how).
  src/omci/*-test.sh      every literal 48-byte frame a suite injects.

For the olt target a line of a session is a record as it stands (fuzz_olt.c).
The voice session only means something behind the ISP1 one and with a phone
port configured, so it is written as that: a stick record, then both.

The local target has a record format of its own (fuzz_local.c); its seeds
are built here from the config stores of src/omci/test and from the command
names of the two CLI protocols.

`seeds.py --hex <file>...` goes the other way: it prints an input the fuzzer
saved as hex, 48 bytes a line, ready to go into corpus/ with a comment
above it.
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOP = HERE.parent.parent.parent
REC = 48


def unhex(text):
    """The bytes of a hex text: comments and all white space dropped."""
    digits = "".join(line.split("#", 1)[0] for line in text.splitlines())
    return bytes.fromhex("".join(digits.split()))


def session(path):
    return unhex(path.read_text())


def stick(b0=0, b1=0, b2=0):
    """The record that configures the stick (fuzz_olt.c, device id 0xc0)."""
    return bytes([b0, b1, b2, 0xC0]) + bytes(REC - 4)


def pause(seconds):
    return bytes([seconds >> 8, seconds & 0xFF, 0, 0]) + bytes(REC - 4)


def olt_seeds():
    out = {}
    fix = TOP / "test" / "fixtures"
    sessions = {p.stem.replace("omci-session-", ""): session(p)
                for p in sorted(fix.glob("omci-session-*.txt"))}
    for name, data in sessions.items():
        if not name.startswith("voice"):
            out["session-" + name] = data
    if "isp1" in sessions and "voice" in sessions:
        # OMCI_POTS_PORTS=1, the ISP1 session, the voice one, its second IP
        # host, and an hour for the PM intervals to turn over.
        out["session-voice"] = (stick(0x02) + sessions["isp1"]
                                + sessions["voice"]
                                + sessions.get("voice-mgmt", b"")
                                + pause(1024))
    if "isp2" in sessions:
        # omcid killed behind a session and started again: the new one
        # resumes from the snapshot, is asked for the MIB data sync and a
        # MIB upload, gets on with the session it resumed, and is killed
        # once more before the snapshot of that is a second old.
        respawn = bytes([0, 0, 0, 0x52]) + bytes(44)
        ask = (bytes.fromhex("7001490a00020000") + bytes(32)
               + bytes.fromhex("00000028") + bytes(4)
               + bytes.fromhex("70024d0a00020000") + bytes(32)
               + bytes.fromhex("00000028") + bytes(4))
        out["session-respawn"] = (sessions["isp2"] + pause(3) + respawn + ask
                                  + sessions["isp2"][-48 * 12:] + respawn
                                  + ask + pause(2))
    frame = re.compile(r"(?<![0-9a-f])[0-9a-f]{96}(?![0-9a-f])")
    for sh in sorted((TOP / "src" / "omci").glob("*-test.sh")):
        frames = frame.findall(sh.read_text())
        if frames:
            out["suite-" + sh.stem] = bytes.fromhex("".join(frames))
    return out


def local_record(kind, data):
    return bytes([kind, len(data) & 0xFF, len(data) >> 8]) + data


def local_seeds():
    """fuzz_local.c: records of kind, length (little-endian), data."""
    out = {}
    # One store per seed, as the file it would be on the stick: the _hs
    # fixtures are the hardware store, the others the config store.
    for xml in sorted((TOP / "src" / "omci" / "test").glob("cfg_*.xml")):
        idx = 1 if xml.stem.endswith("_hs") else 0
        out["store-" + xml.stem] = local_record(0, bytes([idx]) + xml.read_bytes())
    for xml in sorted((TOP / "test" / "fixtures").glob("omci-store-*.xml")):
        out["store-" + xml.stem] = local_record(0, bytes([0]) + xml.read_bytes())
    # The omcli verbs (respond/clisrv.c), each on its own and all in a row
    # behind a small session, so the dumps have something to print.
    fix = TOP / "test" / "fixtures"
    verbs = ["mib", "flows", "caps", "tcont", "check", "conn", "bridge",
             "state", "provision", "voip", "ident", "vlan", "cfgset",
             "frames", "help"]

    def frames_of(data):
        return b"".join(local_record(3, data[i:i + REC])
                        for i in range(0, len(data), REC))

    def omcli(*args):
        return local_record(2, bytes([1, len(args)])
                            + b"".join(a.encode() + b"\0" for a in args))

    def secs(n):
        return local_record(4, bytes([n]))

    sighup = local_record(5, b"")
    cs, hs, odi = (b"/var/config/lastgood.xml", b"/var/config/lastgood_hs.xml",
                   b"/var/config/odi.conf")
    isp2 = frames_of(session(fix / "omci-session-isp2.txt"))
    out["omcli-verbs"] = isp2 + secs(2) + b"".join(omcli(v) for v in verbs)
    out["omcli-args"] = isp2 + secs(2) + b"".join(omcli(*a) for a in (
        ["mib", "all"], ["mib", "84"], ["mib", "256", "0"], ["mib", "0x101"],
        ["bridge", "any", "1", "0"], ["bridge", "0", "1", "1", "transparent"],
        ["ident", cs.decode(), hs.decode(), odi.decode()],
        ["frames", "--full"], ["frames", "--pcap", "/tmp/omci.pcap"],
        ["nothing"], []))
    # A reload that finds something changed: a store with a manual VLAN, a
    # session, the VID rewritten through cfgset, SIGHUP, and time for the
    # reload and the rebuild behind it; then the LOID, and again.
    store = (fix / "omci-store-isp2.xml").read_bytes()
    out["reload"] = (local_record(0, bytes([0]) + store) + isp2 + secs(2)
                     + omcli("cfgset", cs.decode(), "MIB_TABLE",
                             "VLAN_MANU_TAG_VID", "12")
                     + sighup + secs(6)
                     + omcli("cfgset", cs.decode(), "MIB_TABLE", "LOID", "user-two")
                     + omcli("cfgset", odi.decode(), "", "OMCI_UNI_LOCK", "1")
                     + sighup + secs(6) + omcli("state") + omcli("conn"))
    # A stick with a phone port (record 6), the ISP1 session with the voice
    # one behind it, and what omcli says about the lines.
    voice = (session(fix / "omci-session-isp1.txt")
             + session(fix / "omci-session-voice.txt")
             + session(fix / "omci-session-voice-mgmt.txt"))
    out["voice"] = (local_record(6, bytes([0x02, 0, 0, 0])) + frames_of(voice)
                    + secs(2) + omcli("voip") + omcli("voip", "--secrets")
                    + omcli("voip", "--secrets-list") + omcli("provision")
                    + omcli("mib", "all") + omcli("check") + omcli("conn")
                    + omcli("flows") + omcli("tcont") + omcli("state"))
    # omcid killed and started again behind a session, and what the new one
    # says it resumed; then the same with the snapshot patched in between:
    # its version (offset 4), a byte of the serial it was taken for (16),
    # and bytes further in, where the tables are.
    out["respawn"] = (isp2 + secs(3) + local_record(7, b"") + omcli("state")
                      + omcli("mib", "all") + omcli("flows") + secs(2))
    for name, patch in (("version", bytes([4, 0, 0, 0, 6])),
                        ("device", bytes([16, 0, 0, 0x55])),
                        ("tables", bytes([0x40, 0x01, 0]) + bytes([0xff] * 12)),
                        ("tail", bytes([0x00, 0x30, 0]) + bytes([0x7f] * 24))):
        out["respawn-patch-" + name] = (
            isp2 + secs(3) + local_record(7, patch) + omcli("state")
            + omcli("mib", "all") + omcli("flows") + omcli("check") + secs(2))
    # With no netlink, as under qemu: answers are printed, not sent.
    out["no-netlink"] = (local_record(6, bytes([0, 0, 0, 1])) + isp2 + secs(2)
                         + omcli("state") + omcli("mib", "all"))
    # The 45 ids of the omcicli protocol (respond/vqsrv.c): id in word 0 of
    # the payload, a class name where one is read.
    vq = b""
    for cmd in range(46):
        payload = bytearray(240)
        payload[0:4] = cmd.to_bytes(4, "big")
        payload[4:8] = (0xFFFFFFFF).to_bytes(4, "big")
        payload[35:35 + 3] = b"256"
        vq += local_record(1, bytes([3]) + bytes(payload[:48]))
    out["omcicli-ids"] = isp2 + secs(2) + vq
    return out


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "--hex":
        for name in sys.argv[2:]:
            data = Path(name).read_bytes()
            print("# " + Path(name).name)
            for i in range(0, len(data), REC):
                print(data[i:i + REC].hex())
        return 0
    if len(sys.argv) != 3 or sys.argv[1] not in ("olt", "local"):
        sys.stderr.write(__doc__)
        return 2
    target, outdir = sys.argv[1], Path(sys.argv[2])
    outdir.mkdir(parents=True, exist_ok=True)
    seeds = olt_seeds() if target == "olt" else local_seeds()
    expect = {}
    for txt in sorted((HERE / "corpus" / target).glob("*.txt")):
        text = txt.read_text()
        seeds["corpus-" + txt.stem] = unhex(text)
        want = re.findall(r"^# expect: (.+)$", text, re.M)
        if want:
            expect["corpus-" + txt.stem] = "\n".join(want) + "\n"
    # What replay.c holds an input to, beside the seeds and not among them.
    expdir = outdir.with_name(outdir.name + ".expect")
    expdir.mkdir(parents=True, exist_ok=True)
    for d in (outdir, expdir):
        for old in d.iterdir():
            old.unlink()
    for name, data in seeds.items():
        (outdir / name).write_bytes(data)
    for name, text in expect.items():
        (expdir / name).write_text(text)
    print(f"seeds: {len(seeds)} input(s) for {target} in {outdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
