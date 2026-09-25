# regdump

Dump every switch-core register the register table defines (minus GPON,
the counter/MIB block, and a listed set of read-sensitive registers) on the
stick, under two images, and diff the two captures: the way to find a
register one image sets and the other never does. It was built for the
bring-up of our own switch driver, when the image carrying the stock kernel
modules and the one carrying ours differed in registers our driver did not
know about yet; the diff it produced became the parity table below. The
image today carries no stock module at all, so the tool is a debugging aid,
not part of any build or boot.

Physical addresses only, one 32-bit word per read, through
`tools/memprobe/memprobe reg <addr>` -- never a block/range read, and never
an address this tool did not put on the list. This is why: a write past
the mapped window stalled the whole SoC with no timeout, and
some registers in this map are read-sensitive (drain a FIFO, clear on
read) even for a plain load. The exclusion table mklist.py applies below is
what keeps the address list conservative; read it before trusting a capture
against a stick you care about.

## Files

- `mklist.py` -- parses the register listing into the address list
  (`<addr_hex> <name>`), applying the packing model and exclusion table
  described in its own docstring.
- `dump.sh` -- runs ON THE STICK: reads the list, calls `memprobe reg` on
  every address in order, prints the `<addr> = <value>` output memprobe
  already produces, nothing else. POSIX sh, no busybox utility beyond `sh`
  itself and the `memprobe` binary given on the command line.
- `diff.py` -- compares two dump.sh captures against the address list,
  prints the addresses that differ (name, both values, XOR), then a count
  of differences per register family.
- `mkparity.py` -- turns a `diff.py` capture into a parity load table:
  one `<offset> <value> <mask> <name>` line per differing register, minus
  a mechanical exclusion list (debug latches, the thermal sensor, SerDes
  windows, PTP/EPON state), in the format
  `rootfs/skeleton/etc/scripts/parity-load.sh` feeds to the kernel through
  `/proc/odi_omci` (`parity_add`, then `init_parity`).
- `parity-v6.table` -- the table generated that way from the bring-up
  diff (39 entries), and `parity-v6-untried.table`, a curated subset of
  it. Both are test fixtures (`test/parity_load_test.sh`,
  `test/odi_omci_parity_parse_test.sh`) and a reference; `rcS` loads a
  parity table only when one is placed at `/var/config/parity.table`.
- `test/regdump_test.sh` (repo root, wired into `make test-host`) --
  exercises all three against fixtures, plus an opportunistic sanity check
  against the real listing when `ODI_REAL_MAP` points at a local copy.

## Procedure

The register listing is generated from the stock firmware binary, not kept
in this repository -- same as the map argument of
`tools/regtrace/decode.py`:

    python3 src/diag/tools/regmap-extract.py <rootfs>/lib/librtk.so rtl9602c \
        -o /tmp/regmap-gen -t /tmp/rtl9602c-listing.txt

1. Build the address list once (it does not depend on which image is
   running; regenerate only if the map changes):

       python3 tools/regdump/mklist.py \
           /tmp/rtl9602c-listing.txt \
           /tmp/reglist.txt

   Read the count and exclusion line it prints on stderr. If it ever says
   `CAPPED`, registers with a curated name (the ones our code touches)
   were kept first and the rest filled in map order -- check the
   `# CAPPED` comment line mklist.py writes in the list file for which
   addresses that run kept.

2. Build memprobe if `/tmp/memprobe` is not already on the stick from
   an earlier session (`tools/memprobe/README.md` has the exact build
   command); push it, the list, and dump.sh:

       scp -O /tmp/reglist.txt tools/regdump/dump.sh \
           tools/memprobe/memprobe root@<stick>:/tmp/

3. On EACH image (the working one and the failing one), same boot moment
   as far as that is meaningful for a static register file (right after
   the OLT provisioning settles, matching the A/B this tool was built
   for):

       ssh root@<stick> 'chmod +x /tmp/dump.sh /tmp/memprobe; sh /tmp/dump.sh /tmp/reglist.txt /tmp/memprobe' > dump-<label>.txt

   Pull with the redirect above, not a separate `scp` of a file written
   on the stick -- the stick has little free flash/RAM to spare for a
   ~2900-line capture file (the warning in `tools/memprobe/README.md` about
   pushing too much into a running image ramfs applies here too, in
   reverse: do not leave dump output sitting in `/tmp` on the stick any
   longer than the one `ssh` invocation needs).

4. Diff:

       python3 tools/regdump/diff.py /tmp/reglist.txt dump-working.txt dump-failing.txt

   Read the "missing from" lines first, if any -- a capture that stopped
   early (stick rebooted mid-dump, ssh dropped) must not be read as a
   clean diff of a partial set. Exit code is 0 only when both captures
   are complete; a nonzero exit with no diff lines above it still means
   look at the missing-address report before trusting anything below it.

## Output format

`dump.sh` output, one line per address, list order, verbatim from
`memprobe reg`:

    <addr_hex> = <value_hex>

`diff.py` output: one line per differing address, sorted by address,

    <addr_hex> <name>: a=<value_hex> b=<value_hex> xor=<value_hex>

`name` is `|`-joined when mklist.py found more than one register at that
word address (a real overlap in the table itself -- see the mklist.py
docstring); treat a joined name as "one of these changed", not as both.
After the diff lines, a blank line and a per-family count, most-differing
family first.

## Exclusion policy (owned by mklist.py, summarised here)

Dropped whole, never partially:

| rule | why |
| --- | --- |
| offset >= `0x700000` | GPON block -- different subsystem, 202 of 1530 registers |
| offset >= `0xF00000` | counter/MIB block -- clear-on-read is common here |
| base address in `EXCLUDED_REGISTERS` (66 registers) | not idempotent to read: indirect-access read-data latches, FIFOs, status and interrupt-status words, counters and counter controls, acknowledge/clear handshakes -- by address, because most carry only a default name |

The module docstring in `mklist.py` has the full reasoning and the packing
model (narrow vs wide vs wide-multi-word) that decides how many words a
surviving register actually contributes.
