#!/usr/bin/env bash
#
# Compare two builds of vmlinux function by function.
#
#   tools/objdiff.sh [--exact] <old-vmlinux> <new-vmlinux>
#
# Default: every function is disassembled, and the parts of each
# instruction that depend on where things landed are replaced by what they
# name: branch and call targets by symbol, lui/addiu (and lui plus a load
# or store) pairs by the symbol they address, or by the string itself when
# the address falls in a string literal. The code is then compared function
# by function, by name, so a function that moved to another file or
# another address compares equal. Printed: the functions only in one side,
# and a diff of every function whose code differs (a static function
# inlined differently shows here). Exit 0 when nothing differs.
#
# --exact: the whole `objdump -d` text must be identical (the check for a
# change that is meant to leave vmlinux as it was: comments, an unused
# header, a Kconfig arm that was compiled out).
#
# objdump comes from $OBJDUMP, else mips-linux-uclibc-objdump on PATH,
# else the pinned uclibc toolchain image (toolchain/images.env) through
# docker, read only.
set -euo pipefail

EXACT=0
if [ "${1:-}" = --exact ]; then
	EXACT=1
	shift
fi
[ $# -eq 2 ] || { echo "usage: $0 [--exact] <old-vmlinux> <new-vmlinux>" >&2; exit 2; }
OLD=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
NEW=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
[ -f "$OLD" ] && [ -f "$NEW" ] || { echo "objdiff: no such file" >&2; exit 2; }

TMP=$(mktemp -d -t objdiff.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

# run_objdump <args...> <file>: the file is the last argument.
run_objdump() {
	if [ -n "${OBJDUMP:-}" ]; then
		"$OBJDUMP" "$@"
	elif command -v mips-linux-uclibc-objdump >/dev/null 2>&1; then
		mips-linux-uclibc-objdump "$@"
	else
		local f=${!#} n=$(($# - 1))
		local root; root=$(cd "$(dirname "$0")/.." && pwd)
		# shellcheck source=../toolchain/images.env
		. "$root/toolchain/images.env"
		docker run --rm -v "$(dirname "$f"):/in:ro" \
			"${OSS_IMAGE:-$OSS_IMAGE_PINNED}" /opt/oss/bin/mips-linux-uclibc-objdump "${@:1:$n}" "/in/$(basename "$f")"
	fi
}

for side in old new; do
	f=$OLD
	[ "$side" = new ] && f=$NEW
	run_objdump -d "$f" | sed 1,2d > "$TMP/$side.dis"
	run_objdump -t "$f" > "$TMP/$side.sym"
	run_objdump -h "$f" > "$TMP/$side.sec"
done

if [ "$EXACT" = 1 ]; then
	if cmp -s "$TMP/old.dis" "$TMP/new.dis"; then
		echo "objdiff: text identical ($(grep -c '^[0-9a-f]*:' "$TMP/new.dis" || true) lines of disassembly)"
		exit 0
	fi
	echo "objdiff: text differs"
	diff "$TMP/old.dis" "$TMP/new.dis" | head -n 80
	exit 1
fi

python3 - "$TMP" "$OLD" "$NEW" <<'PY'
import bisect
import difflib
import re
import sys

tmp, old_elf, new_elf = sys.argv[1:4]

# Instructions that write no register named in their first operand.
NO_DEST = re.compile(r'^(s[bhw]|sw[lr]|sc|b[a-z]*|j|jr|jalr?|nop|sync|break|syscall|'
                     r'eret|mt[a-z0-9]*|cache|pref|teq|tne|wait|ssnop|ehb|deret)$')


def load_syms(path):
    syms = []
    for line in open(path):
        m = re.match(r'^([0-9a-f]{8}) (.{7}) (\S+)\s+([0-9a-f]{8})\s+(\S+)$', line)
        if not m or m.group(3) in ('*ABS*', '*UND*'):
            continue
        name = m.group(5)
        if name.startswith('$') or name.startswith('.L'):
            continue
        syms.append((int(m.group(1), 16), int(m.group(4), 16), name, m.group(3)))
    syms.sort()
    return syms


def load_secs(path, elf):
    secs = []
    for line in open(path):
        m = re.match(r'^\s*\d+\s+(\S+)\s+([0-9a-f]{8})\s+([0-9a-f]{8})\s+[0-9a-f]{8}\s+([0-9a-f]{8})', line)
        if m:
            secs.append((m.group(1), int(m.group(3), 16), int(m.group(2), 16), int(m.group(4), 16)))
    data = open(elf, 'rb').read()
    return secs, data


class Side:
    def __init__(self, tag, elf):
        self.syms = load_syms('%s/%s.sym' % (tmp, tag))
        self.addrs = [s[0] for s in self.syms]
        self.secs, self.data = load_secs('%s/%s.sec' % (tmp, tag), elf)

    def section(self, addr):
        for name, vma, size, off in self.secs:
            if vma <= addr < vma + size:
                return name, vma, off
        return None

    def string_at(self, addr):
        s = self.section(addr)
        if not s or not s[0].startswith('.rodata'):
            return None
        off = s[2] + addr - s[1]
        end = self.data.find(b'\0', off, off + 256)
        if end <= off:
            return None
        raw = self.data[off:end]
        if all(32 <= b < 127 or b in (9, 10) for b in raw) and len(raw) >= 2:
            # A string starts at addr only if the byte before ends another.
            if off > 0 and self.data[off - 1] != 0:
                return None
            return '"%s"' % raw.decode().replace('\n', '\\n')
        return None

    def name(self, addr):
        s = self.string_at(addr)
        if s:
            return s
        i = bisect.bisect_right(self.addrs, addr) - 1
        while i >= 0:
            a, size, name, sec = self.syms[i]
            if a <= addr < a + max(size, 1):
                return name if addr == a else '%s+%#x' % (name, addr - a)
            if a < addr and size == 0:
                i -= 1
                continue
            break
        sec = self.section(addr)
        if sec:
            return '%s+%#x' % (sec[0], addr - sec[1])
        return '%#x' % addr


INSN = re.compile(r'^\s*([0-9a-f]+):\s+(?:[0-9a-f]{8}\s+)?(\S+)\s*(.*)$')
FUNC = re.compile(r'^([0-9a-f]{8}) <(.+)>:$')


def read_funcs(tag):
    funcs, cur, name, seen = {}, None, None, {}
    for line in open('%s/%s.dis' % (tmp, tag)):
        line = line.rstrip('\n')
        m = FUNC.match(line)
        if m:
            name = m.group(2)
            seen[name] = seen.get(name, 0) + 1
            if seen[name] > 1:
                name = '%s#%d' % (name, seen[name])
            cur = funcs[name] = (int(m.group(1), 16), [])
            continue
        m = INSN.match(line)
        if m and cur is not None:
            cur[1].append((int(m.group(1), 16), m.group(2), m.group(3)))
    return funcs


def sext16(v):
    v &= 0xffff
    return v - 0x10000 if v & 0x8000 else v


def normalize(side, start, insns):
    end = insns[-1][0] + 4 if insns else start
    out = []
    lui = {}          # reg -> (index in out, hi value)
    for addr, mnem, ops in insns:
        ops = re.sub(r'\s+#.*$', '', ops)
        parts = [p.strip() for p in ops.split(',')] if ops else []
        text = None
        # Branch and call targets: within the function, relative; else by name.
        m = re.search(r'([0-9a-f]+) <([^>]+)>$', ops)
        if m:
            tgt = int(m.group(1), 16)
            rel = ('.+%#x' % (tgt - start)) if start <= tgt < end else '<%s>' % m.group(2)
            text = '%s %s' % (mnem, re.sub(r'[0-9a-f]+ <[^>]+>$', rel, ops))
        elif mnem == 'lui' and len(parts) == 2:
            lui[parts[0]] = (len(out), int(parts[1], 0) << 16)
            # Stays a number unless a later instruction pairs it with a
            # low half: then it is an address and gets the name instead.
            out.append('lui %s' % ops)
            continue
        elif mnem in ('addiu', 'ori') and len(parts) == 3 and parts[1] in lui:
            idx, hi = lui[parts[1]]
            lo = sext16(int(parts[2], 0)) if mnem == 'addiu' else int(parts[2], 0)
            sym = side.name((hi + lo) & 0xffffffff)
            out[idx] = 'lui %s,%%hi(%s)' % (parts[1], sym)
            text = '%s %s,%s,%%lo(%s)' % (mnem, parts[0], parts[1], sym)
        else:
            mm = re.match(r'^(-?(?:0x)?[0-9a-f]+)\((\w+)\)$', parts[-1]) if parts else None
            if mm and mm.group(2) in lui:
                idx, hi = lui[mm.group(2)]
                sym = side.name((hi + int(mm.group(1), 0)) & 0xffffffff)
                out[idx] = 'lui %s,%%hi(%s)' % (mm.group(2), sym)
                text = '%s %s,%%lo(%s)(%s)' % (mnem, ','.join(parts[:-1]), sym, mm.group(2))
        if text is None:
            text = ('%s %s' % (mnem, ops)).strip()
        if parts and not NO_DEST.match(mnem) and parts[0] in lui:
            del lui[parts[0]]
        out.append(text)
    return out


def body(side, funcs, name):
    start, insns = funcs[name]
    return normalize(side, start, insns)


old, new = Side('old', old_elf), Side('new', new_elf)
fo, fn = read_funcs('old'), read_funcs('new')
only_old = sorted(set(fo) - set(fn))
only_new = sorted(set(fn) - set(fo))
changed = []
for name in sorted(set(fo) & set(fn)):
    a, b = body(old, fo, name), body(new, fn, name)
    if a != b:
        changed.append((name, a, b))

for name in only_old:
    print('only in old: %s (%d insns)' % (name, len(fo[name][1])))
for name in only_new:
    print('only in new: %s (%d insns)' % (name, len(fn[name][1])))
for name, a, b in changed:
    print('changed: %s (%d -> %d insns)' % (name, len(a), len(b)))
    for line in list(difflib.unified_diff(a, b, 'old', 'new', lineterm='', n=2))[2:40]:
        print('    ' + line)
print('objdiff: %d functions compared, %d only in old, %d only in new, %d changed'
      % (len(set(fo) & set(fn)), len(only_old), len(only_new), len(changed)))
sys.exit(1 if only_old or only_new or changed else 0)
PY
