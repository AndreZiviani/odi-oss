#!/usr/bin/env python3
"""Rebuild the OLT side of an OMCI session from an omcid log.

omcid logs every request it handles as

    <- <type>  tci N  class C  <name>  inst I
       type T contents <16 hex bytes>        (set, create, test, unnamed)
       (no ack requested)                    (when the AR bit is clear)

and the answer on the lines after it. That is enough to rebuild each request
as a baseline frame: the header, the first 16 bytes of the contents (all the
log keeps; 20 for a GemPortCtp or T-CONT create, whose raw line is longer;
the rest are zero), and the trailer (length 0x28, CRC zero --
omcid does not check it on the injection path). A get carries the attribute
mask of its answer, the closest the log comes to the request mask.

Scrubbing, so a rebuilt session can be committed:
  - classes whose contents are line credentials or addresses (AuthSecMethod,
    LargeString, TR069ManageServer, IpHostConfigData) are already withheld by
    omcid itself and come out as zeros;
  - OltG contents (the OLT vendor and equipment ids) are zeroed as well;
  - any other contents with a run of three or more printable ASCII bytes stop
    the conversion, so a new identity field is looked at, not committed.

Restoring what the log cut off. Where an attribute the session needs lies
past the logged bytes and its value is known from elsewhere (a MIB readback
of the same stick, or the rest of the same message), it can be put back:

  --fill CLASS/MT@OFF=HEX    every message of that class and type
  --fill tTCI@OFF=HEX        the one message with that tci

OFF is the byte offset in the 32-byte contents. Each rule used is echoed as a
comment line ahead of the frames, so the fixture says what was not logged.

Usage: session-from-log.py [--fill RULE]... <omcid.log>
       (frames on stdout, one per line)
"""
import re
import sys

MT = {
    'create': 4, 'delete': 6, 'set': 8, 'get': 9, 'get-all-alarms': 11,
    'get-all-alarms-next': 12, 'mib-upload': 13, 'mib-upload-next': 14,
    'mib-reset': 15, 'test': 18, 'start-sw-download': 19,
    'end-sw-download': 21, 'activate-image': 22, 'commit-image': 23,
    'sync-time': 24, 'reboot': 25, 'get-next': 26,
}
ZEROED = {131}   # OltG
HDR = re.compile(r'^<- (\S+)\s+tci (\d+)\s+class (\d+)\s+.*\binst (\d+)\s*$')
CONTENTS = re.compile(r'^\s+type (\d+) contents(.*)$')
GET_MASK = re.compile(r'^-> ok, mask ([0-9a-f]{4})')
UPLOAD_NEXT = re.compile(r'^-> \[(\d+)\]')
RAW = re.compile(r'^\s+create mask [0-9a-f]{4} raw(.*)$')


def ascii_run(b, n=3):
    run = 0
    for x in b:
        run = run + 1 if 0x20 <= x < 0x7f else 0
        if run >= n:
            return True
    return False


def frames(lines, fills=()):
    msgs = []
    cur = None
    for line in lines:
        m = HDR.match(line)
        if m:
            cur = {'name': m.group(1), 'tci': int(m.group(2)),
                   'cls': int(m.group(3)), 'inst': int(m.group(4)),
                   'mt': MT.get(m.group(1)), 'ar': True, 'body': bytes(32)}
            msgs.append(cur)
            continue
        if cur is None:
            continue
        m = CONTENTS.match(line)
        if m:
            cur['mt'] = int(m.group(1))
            rest = m.group(2).split()
            if rest and rest[0] != 'withheld':
                cur['body'] = bytes(int(x, 16) for x in rest[:16]) + bytes(16)
            continue
        m = RAW.match(line)
        if m:
            raw = bytes(int(x, 16) for x in m.group(1).split()[:20])
            cur['body'] = raw + cur['body'][len(raw):]
            continue
        if line.strip() == '(no ack requested)':
            cur['ar'] = False
            continue
        m = GET_MASK.match(line)
        if m and cur['name'] == 'get':
            cur['body'] = bytes.fromhex(m.group(1)) + bytes(30)
            continue
        m = UPLOAD_NEXT.match(line)
        if m and cur['name'] == 'mib-upload-next':
            cur['body'] = int(m.group(1)).to_bytes(2, 'big') + bytes(30)
    for c in msgs:
        if c['mt'] is None:
            raise SystemExit('unknown message type %r (tci %d)' % (c['name'], c['tci']))
    for rule, n in apply_fills(msgs, fills).items():
        yield '# restored: %s (%d message%s)' % (rule, n, '' if n == 1 else 's')
    for c in msgs:
        body = c['body']
        if c['cls'] in ZEROED:
            body = bytes(32)
        elif ascii_run(body):
            raise SystemExit('printable ASCII in class %d inst %d tci %d: %s -- '
                             'decide whether it is an identity value before committing'
                             % (c['cls'], c['inst'], c['tci'], body.hex()))
        f = (c['tci'].to_bytes(2, 'big') +
             bytes([c['mt'] | (0x40 if c['ar'] else 0), 0x0a]) +
             c['cls'].to_bytes(2, 'big') + c['inst'].to_bytes(2, 'big') +
             body + bytes([0, 0, 0, 0x28, 0, 0, 0, 0]))
        yield f.hex()


def parse_fill(rule):
    where, _, what = rule.partition('@')
    off, _, val = what.partition('=')
    if where.startswith('t'):
        key = ('tci', int(where[1:]))
    else:
        cls, _, mt = where.partition('/')
        key = ('cls', int(cls), int(mt))
    return key, int(off), bytes.fromhex(val), rule


def apply_fills(msgs, fills):
    used = {}
    for c in msgs:
        for key, off, val, rule in fills:
            hit = (key == ('tci', c['tci']) if key[0] == 'tci'
                   else key == ('cls', c['cls'], c['mt']))
            if hit:
                b = bytearray(c['body'])
                b[off:off + len(val)] = val
                c['body'] = bytes(b)
                used[rule] = used.get(rule, 0) + 1
    for _, _, _, rule in fills:
        if rule not in used:
            raise SystemExit('fill rule %s matched no message' % rule)
    return used


def main():
    args = sys.argv[1:]
    fills = []
    while len(args) > 1 and args[0] == '--fill':
        fills.append(parse_fill(args[1]))
        args = args[2:]
    with open(args[0]) as fh:
        out = list(frames(fh, fills))
    for hexframe in out:
        print(hexframe)


if __name__ == '__main__':
    main()
