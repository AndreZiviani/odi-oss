# Changelog

Releases of the flashable image. Trial builds between releases are not
listed here.

## Unreleased

The first public version. Everything in `docs/IMPROVEMENTS.md`: Linux 6.18,
fully open — our own switch, GPON MAC, CPU-port NIC and OMCI transport
drivers, an independent implementation built straight into the kernel, no proprietary kernel
module anywhere in the image; our own OMCI daemon (`omcid`) driving GPON
provisioning end to end; watchdog rescue for a hung kernel or a stuck boot
script; the DRAM ramlog console for reading a boot with no serial console;
devtmpfs; seeded entropy; a current dropbear with SSH keys and `scp`;
per-build root password; the web UI (`confd`) with SSH-key management and
image defaults; the Prometheus exporter (`metricsd`) with
`gpon_omci_services`; our own `diag` CLI (SFF-8472 DDM readout, GPON state,
alarm status and GEM flows, port MIB counters, register access), keeping the
stock syntax and output for the commands the exporter uses; an IGMP snooping/proxy daemon (`igmpd`,
observe-only by default); one-shot trial boots with `fwu.sh` guards; our own
toolchain (gcc 16 / binutils 2.47 / uClibc-ng), building the kernel and every
userland binary; `make image-all` from a clean clone and CI for lint and the
OMCI suite.
