"""The southbound driver API of libomci_mib.so.

Every omci_wrapper_* that touches hardware funnels into one helper:

    omci_drv_call(cmd, buf, len)   /* len <= 256 */
        req = { u32 cmd; u32 len; u8 data[256] };
        getsockopt(ctrlFd, 0, 0x310a, &req, &optlen=264);
        memcpy(buf, req.data, len);

with ctrlFd from socket(AF_INET, SOCK_RAW, 0xff) -- the same option ABI librtk
uses for the switch core, one option number with a command word inside. This
prints the command each wrapper sends, which is the whole surface a
reimplementation has to speak.
"""
import os, sys, struct
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'diag', 'tools'))
from rtkelf import Image

argv = [a for a in sys.argv[1:] if not a.startswith('--')]
img = Image(argv[0] if argv else os.path.expanduser('~/tmp/odi/rootfs/lib/libomci_mib.so'))
d, vals = img.d, img.got_values()
HELPER = int(argv[1], 0) if len(argv) > 1 else 0x17de4


def s16(x):
    x &= 0xffff
    return x - 0x10000 if x & 0x8000 else x


def owner(a):
    for fa, (n, sz) in img.funcs.items():
        if fa <= a < fa + sz:
            return n
    return '?'


# Five commands are only ever sent from static helpers -- functions with no
# dynamic symbol, sitting between two exported ones -- so owner() cannot name
# them and the header used to carry "?". What is recorded here is the
# command's identity, established separately from the payload and call path.
#
# 23 is omci_wrapper_setPriQueue's command: that wrapper sends
# getsockopt(0x17, buf, 0x1c) from its downstream branch, and the buffer is
# built from the class 277 row. See ../omci_gemflow.h. The call site the scan
# below finds is a *different* one, inside omci_wrapper_updateUsGemFlow -- the
# same command reached from the upstream flow path.
#
# The other four stay "?" until something establishes them the same way.
STATIC_OWNER = {
    # Sent from omci_wrapper_setPriQueue's downstream branch, and ALSO once per
    # queue from a helper at the tail of omci_CreatePriQByTcontId -- which is
    # the call site the scan below actually finds. Named for the wrapper,
    # because that is the one a reader will look for.
    (23, 28): 'omci_wrapper_setPriQueue',
    # Allocates a T-CONT able to carry N queues and returns the chosen index in
    # word 0. Only sender is omci_CreatePriQByTcontId, which is static -- found
    # by the immediate that loads its own name string.
    (22, 8): 'omci_CreatePriQByTcontId',
}

# Commands whose libomci_mib sender the scan could not attribute, given a
# descriptive name from what they do: the delete twin of the priority queue
# command, and the VEIP GEM flow set and delete (see ../omci_gemflow.h).
OUR_NAMES = {
    (24, 28): 'priQueueDelete',     # priority queue delete
    (65, 72): 'veipFlowSet',        # VEIP GEM flow set
    (66, 72): 'veipFlowDelete',     # VEIP GEM flow delete
}


rows = {}
for va, fsz, fo in img.loads:
    gp = hi = None
    for o in range(fo, fo + fsz - 4, 4):
        x = struct.unpack_from('>I', d, o)[0]
        a = va + (o - fo)
        if (x >> 16) == 0x3c1c:
            hi = (x & 0xffff) << 16
        elif (x >> 16) == 0x279c and hi is not None:
            gp = hi + s16(x) + a - 4
        elif (x >> 26) == 0x23 and ((x >> 21) & 31) == 28 and ((x >> 16) & 31) == 25 and gp:
            base = vals.get(gp + s16(x))
            if base is None:
                continue
            # `lw t9, anchor(gp)` then, within a few instructions, `addiu t9, t9, K`
            for k in range(o + 4, min(o + 24, fo + fsz - 4), 4):
                y = struct.unpack_from('>I', d, k)[0]
                if (y >> 26) == 9 and ((y >> 21) & 31) == 25 and ((y >> 16) & 31) == 25:
                    if base + s16(y) != HELPER:
                        break
                    # cmd is a0 and the payload length a2, each the last `li`
                    # into that register up to and including the delay slot.
                    val, j = {}, None
                    for m in range(max(fo, o - 48), min(o + 40, fo + fsz - 4), 4):
                        z = struct.unpack_from('>I', d, m)[0]
                        if (z >> 26) == 9 and ((z >> 21) & 31) == 0 and ((z >> 16) & 31) in (4, 6):
                            if j is None:
                                val[(z >> 16) & 31] = s16(z)
                        if z == 0x0320f809 and m > o:
                            j = m
                            zz = struct.unpack_from('>I', d, m + 4)[0]
                            if (zz >> 26) == 9 and ((zz >> 21) & 31) == 0 and ((zz >> 16) & 31) in (4, 6):
                                val[(zz >> 16) & 31] = s16(zz)
                            break
                    rows.setdefault((val.get(4), val.get(6)), set()).add(owner(a))
                    break

print('%d wrappers reach omci_drv_call(%#x)' % (sum(len(v) for v in rows.values()), HELPER))
for c, n in sorted(rows, key=lambda x: (x[0] is None, x[0])):
    print('  cmd %-5s len %-5s %s' % (c, n, ', '.join(sorted(rows[(c, n)]))))

if '--header' in sys.argv:
    out = ['/* The driver commands behind libomci_mib.so\'s omci_wrapper_* calls.',
           ' *',
           ' * Generated by tools/omci-drv-api.py. Every one goes through',
           ' * getsockopt(ctrlFd, 0, 0x310a, &{cmd, len, data[256]}, &264).',
           ' * `safe` marks the wrappers whose name begins with "get": those only',
           ' * read, which is what makes them usable beside a running omci_app.',
           ' */',
           '#ifndef OMCI_DRV_CMDS_H',
           '#define OMCI_DRV_CMDS_H',
           '',
           '#define OMCI_DRV_OPT   0x310au',
           '#define OMCI_DRV_MAX   256',
           '',
           'struct omci_drv_cmd {',
           '\tunsigned short cmd;',
           '\tunsigned short len;',
           '\tunsigned char safe;',
           '\tconst char *name;',
           '};',
           '',
           'static const struct omci_drv_cmd omci_drv_cmds[] = {']
    for c, n in sorted(rows, key=lambda x: (x[0] is None, x[0])):
        if c is None or n is None:
            continue
        owners = sorted(rows[(c, n)])
        # The NAME may skip an unattributed call site -- '?' sorts before every
        # letter and would otherwise win. `safe` may NOT: it gates whether
        # omciprobe sends a command without -f, so it has to consider every
        # site, including the ones owner() could not name.
        named = [x for x in owners if x != '?'] or owners
        # Strip either prefix: the wrappers are omci_wrapper_*, and the one
        # static sender named here is a plain omci_* function.
        nm = STATIC_OWNER.get((c, n), named[0])
        nm = nm.replace('omci_wrapper_', '').replace('omci_', '', 1)
        if nm == '?':
            nm = OUR_NAMES.get((c, n), nm)
        safe = 1 if all(x.startswith('omci_wrapper_get') for x in owners) else 0
        out.append('\t{ %3d, %4d, %d, "%s" },' % (c, n, safe, nm))
    out += ['};', '',
            '#define OMCI_DRV_CMD_COUNT '
            '((int)(sizeof omci_drv_cmds / sizeof omci_drv_cmds[0]))', '', '#endif']
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        '..', 'generated', 'omci_drv_cmds.h')
    with open(path, 'w') as fh:
        fh.write('\n'.join(out) + '\n')
    print('wrote %s' % os.path.normpath(path))
