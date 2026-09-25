# bridge-utils: not built, and there is no build.sh here on purpose

`brctl` is already covered twice over. A fourth binary in a 2,572,288-byte
partition needs to earn its place, and this one cannot.

## What actually calls brctl on this device

Everything in the vendor rootfs that touches a bridge, found by searching the
extracted `V1.0-220923` image for the string and for the `addbr`/`addif`
command names:

| caller | commands used |
|---|---|
| `lib/omci/mib_EthUni.so` | `brctl addif br0 eth0.%u`, `brctl delif br0 eth0.%u` |
| `bin/startup` | `addbr`, `addif` |
| `bin/xmlconfig` | `brctlAgeingTime`, `brctlStp` — config keys, which reach `brctl` as `setageing` and `stp` |
| `lib/libmib.so` | the path `/bin/brctl` |

Five verbs in total: **addbr, addif, delif, setageing, stp**. No shell script
in the image calls `brctl` at all, and neither does anything in our own
`rootfs/skeleton`.

## What we already ship

busybox's `brctl` applet, enabled in `../busybox/config.fragment`, built with
`CONFIG_FEATURE_BRCTL_FANCY=y` and `CONFIG_FEATURE_BRCTL_SHOW=y` — verified in
the produced `.config` and in the strings of `out/busybox`. That covers
`addbr`, `delbr`, `addif`, `delif`, `show`, `showstp`, `setageing`, `setfd`,
`sethello`, `setmaxage`, `setpathcost`, `setportprio`, `setbridgeprio` and
`stp`. It is a superset of the five verbs above, at zero extra bytes, because
busybox is being built either way.

`ip` and `bridge` from `../iproute2/` cover the same ground over netlink,
which is the interface the kernel actually prefers; the ioctl API `brctl` uses
has been the compatibility path since 2.6.

Note also that the vendor's own busybox 1.12.4 has **no** `brctl` applet, which
is why their image carries a separate 27,640-byte `/bin/brctl`. Ours does have
it. That is the whole difference.

## What would change this

A rootfs script or one of our own daemons needing a `brctl` verb busybox does
not implement. There is no such verb today. If one appears, the package is a
short `build.sh` away — bridge-utils 1.7.1 is small, pure ioctl, and has no
dependency this toolchain lacks.
