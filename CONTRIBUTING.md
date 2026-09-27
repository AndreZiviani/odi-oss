# Contributing

## Scope

odi-oss is firmware for exactly one board, the ODI DFP-34X-2C2 GPON SFP
ONU (RTL9602C, Lexra RLX5281): a mainline Linux 6.18 kernel with our own
drivers, our own userland, and our own toolchain. Changes that fit:
drivers and fixes for this board, OMCI behaviour against real OLTs, tools
and tests, documentation, field notes on the hardware. Out of scope: other
boards or SoCs, anything derived from a vendor SDK or firmware source, and
anything that would ship or link proprietary code. The web UI (`confd`) and
the exporter (`metricsd`) are separate projects; report their issues there.

## Reporting an issue

Open an issue with:

- the image version (`/etc/odi-build`, or the name of the tarball), and
  which slot it ran from (`cat /proc/cmdline`);
- what you did and what happened, including whether it was a trial boot
  and where the stick came back up;
- the evidence you have: `/proc/odi_ramlog_prev` or a `tools/memprobe`
  read of the ramlog after a failed trial, `/etc/config/breadcrumbs`,
  `dmesg`, `/var/log/omcid.log`, `omcli state`.

Refer to lines and sticks by a neutral name (ISP1, "my line"), and remove
serial numbers, MAC addresses, LOIDs and passwords before you post a log.
Corrections to the field notes in `docs/kb/` are welcome, especially data
from other units, OLTs or firmware revisions.

## Changing the code

Read [`docs/HACKING.md`](docs/HACKING.md) first: how the build works, how
the trial boot keeps a stick recoverable, how to test, and the traps that
have already cost time. [`AGENTS.md`](AGENTS.md) is the short list of
house rules and applies to humans too.

The gates, before you open a pull request:

    make test          # lint + host tests + diag and the exporter contract + omcid under qemu

and, for a kernel change, a kernel build with no new warning, `packages/isa-audit.sh
build/kernel-618/vmlinux` clean and `tools/kernel-footprint.sh` not grown
without a reason; for anything that ships, `make image`. CI runs `make
test`'s four parts on every pull request; it does not build the kernel or
the image.

Keep commits small and each about one change, with a subject like
`area: what changed` and a body that says why and what you measured. Say
whether you trial-booted it, on which kind of line, and what you saw.
Never ask anyone to `sw_commit` an image that has not been through a
trial.

If your change affects users, the build, or the docs, add an entry under
`## Unreleased` in [`CHANGELOG.md`](CHANGELOG.md) in the same commit; a
release moves `Unreleased` into a version section named after the tag. CI
checks this on every pull request unless it is labelled `no-changelog`.
