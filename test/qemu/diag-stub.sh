#!/bin/sh
# diag-stub.sh -- the qemu test harness stand-in for /bin/diag
# (docs/HACKING.md, make test-qemu). Installed ONLY in the harness
# initramfs (test/qemu/build-initramfs.sh), never in rootfs/skeleton or a
# flashed image: the real /bin/diag talks to hardware qemu does not
# emulate, and stubbing it here is what "make test-qemu does not cover our
# own kernel and drivers" (docs/HACKING.md) means concretely for the
# exporter.
#
# diag is interactive, batched on stdin (AGENTS.md); metricsd sends the
# fixed command batch in src/diag/test/exporter.txt and expects the
# byte-for-byte contract in src/diag/test/exporter.golden back
# (src/diag Makefile, test-diag). This stub ignores what it is actually
# asked -- there is no register file or GPON state behind it to read
# back honestly -- and prints that golden output, so the exporter has
# complete data to serve under qemu instead of the "unreadable" a real
# diag gives when a register read fails (network.sh already tolerates
# that, harmlessly, for its own SerDes probe).
cat /etc/qemu-test/exporter.golden
exit 0
