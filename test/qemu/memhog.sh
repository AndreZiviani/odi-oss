#!/bin/sh
# memhog.sh -- a busybox-only memory hog for the OOM scenario test (make
# test-qemu): doubles a shell variable until ash itself cannot grow it any
# further or the OOM killer takes it first. No compiled tool needed -- ash
# keeps the string on its own heap, so this alone is enough to exhaust a
# qemu VM sized to the stick own ~15 MB of usable memory.
x=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
i=0
while [ "$i" -lt 40 ]; do
	x="$x$x"
	i=$((i + 1))
done
sleep 30
