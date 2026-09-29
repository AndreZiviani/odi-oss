#!/usr/bin/env bash
# Pull the two RAM log pages off the stick (memprobe must be in /tmp there) and
# print them: page A = first 3984 bytes of printk (4016 from a format 1
# metadata block, 4080 from an image without one), page B = ring of the last
# 4080.
set -eu
CTL=${CTL:-/tmp/odi_ctl}
out=${1:-$(dirname "$0")/ramlog-$(date +%H%M%S)}
mkdir -p "$out"
ssh -S "$CTL" x '/tmp/memprobe d 017ff000 1000' > "$out/pageA.bin"
ssh -S "$CTL" x '/tmp/memprobe d 01fff000 1000' > "$out/pageB.bin"
python3 - "$out" <<'PY'
import struct,sys
d=sys.argv[1]
a=open(d+'/pageA.bin','rb').read(); b=open(d+'/pageB.bin','rb').read()
def tag(x): return x.decode('latin1') if all(32<=c<127 for c in x) else x.hex()
ma,na,h1,h2=struct.unpack('>IIII',a[:16]); mb,nb=struct.unpack('>II',b[:8])
print("page A magic %08x (%s) count %d  stamps: %s / %s"%(ma,tag(a[:4]),na,tag(a[8:12]),tag(a[12:16])))
print("page B magic %08x (%s) count %d"%(mb,tag(b[:4]),nb))
# CONFIG_ODI_EARLY_CRUMBS (mach-rtl8686/odi-early-crumb.h in kernel/extra)
# stamps a tag + step word at page B +8/+12 -- otherwise-unused on this
# page (only page A carries tag1/tag2). Only meaningful when ASCII: once
# the real ramlog console comes up, odi_ramlog_page_reset() overwrites
# both words with zero, which is expected, not an error.
crumb_tag, crumb_step = b[8:12], struct.unpack('>I', b[12:16])[0]
if all(32<=c<127 for c in crumb_tag):
    print("page B early crumb: tag=%s step=%d"%(crumb_tag.decode('latin1'), crumb_step))
# The boot metadata block (odi_ramlog.h), the last 64 bytes of page A:
# magic RLGM, boot counter, slot, format, build id, and the crumb stash.
# With it, page A text stops at 4016 bytes (format 1) or 3984 (format 2,
# which keeps the reset reason block in the 32 bytes before it: magic
# RLGR, reason code, 16-byte detail); without it (an older image), at 4080.
# count never passes the cap, so min(count, cap) is exact for all three.
REASONS={0:'unknown',1:'wdt_client',2:'wdt_mem',3:'wdt_userland',4:'reboot',
         5:'halt',6:'poweroff',7:'panic',8:'oops'}
mm,mboot,mslot,mfmt=struct.unpack('>IIII',a[4032:4048])
alen=min(na,4080)
if ma==0x524c4741 and mm==0x524c474d:
    alen=min(na,4016)
    build=a[4048:4088].split(b'\0')[0].decode('latin1')
    reason='unknown'
    rm,rcode=struct.unpack('>II',a[4000:4008])
    if mfmt>=2 and rm==0x524c4752:
        reason=REASONS.get(rcode,'unknown')
        if rcode==1:
            reason+=':'+(a[4008:4023].split(b'\0')[0].decode('latin1') or '?')
    print("boot %d slot %s build %s reason %s (stash: crumb of the boot before: %s/%d)"%(
        mboot, '?' if mslot==0xffffffff else mslot, build or '?', reason,
        tag(a[4088:4092]), struct.unpack('>I',a[4092:4096])[0]))
elif ma!=0x524c4741 and mb!=0x524c4742:
    print("no valid page magic: DRAM lost its contents (reason power)")
if ma==0x524c4741:
    print("---- page A: first %d bytes of printk ----"%alen)
    print(a[16:16+alen].decode('latin1'))
if mb==0x524c4742:
    ring=b[16:16+4080]
    if nb>4080:
        p=nb%4080; txt=ring[p:]+ring[:p]
    else: txt=ring[:nb]
    print("---- page B: last %d of %d bytes ----"%(len(txt),nb))
    print(txt.decode('latin1'))
PY
