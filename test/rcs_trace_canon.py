#!/usr/bin/env python3
"""Turn an `strace -ff` capture of rcS into its canonical action trace.

Usage: rcs_trace_canon.py <trace-dir> <chroot-root>

The trace directory holds one file per process, t.<pid>. The processes are
walked depth first from the root, and each child is inlined at the point
its parent created it. The result depends on what each process did and in
what order, not on how the scheduler interleaved them, so background jobs
give the same trace on every run.

Two kinds of action are kept:
  exec  <path> <argv>        every successful execve, argv as strace prints it
  write <path> <bytes>       every write to a file under /proc
The execve of chroot and of qemu itself (the harness) are dropped.
"""
import os
import re
import sys

tdir, root = sys.argv[1], sys.argv[2].rstrip('/')

CHILD = re.compile(r'^(?:clone|clone3|fork|vfork)\((.*)\)\s+=\s+(\d+)')
EXEC = re.compile(r'^execve\("([^"]*)", (\[.*\]), .*\)\s+=\s+0$')
WRITE = re.compile(r'^write\(\d+<([^>]*)>, ("(?:[^"\\]|\\.)*")(?:\.\.\.)?, \d+\)\s+=')

events = {}
root_pid = None
for name in os.listdir(tdir):
    if not name.startswith('t.'):
        continue
    pid = int(name[2:])
    ev = []
    with open(os.path.join(tdir, name), errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            m = CHILD.match(line)
            if m:
                # A thread (qemu starts some of its own) is not a process.
                if 'CLONE_THREAD' not in m.group(1):
                    ev.append(('child', int(m.group(2))))
                continue
            m = EXEC.match(line)
            if m:
                path = m.group(1)
                if path.endswith('/chroot'):
                    root_pid = pid
                if path.endswith('/chroot') or path == '/qemu-mips-static':
                    continue
                ev.append(('exec', '%s %s' % (path, m.group(2))))
                continue
            m = WRITE.match(line)
            if m:
                path = m.group(1)
                if path.startswith(root + '/'):
                    path = path[len(root):]
                if path.startswith('/proc/'):
                    ev.append(('write', '%s %s' % (path, m.group(2))))
    events[pid] = ev

if root_pid is None:
    sys.exit('rcs_trace_canon: no process ran chroot, nothing to walk')

out = []
stack = [iter(events[root_pid])]
while stack:
    try:
        kind, val = next(stack[-1])
    except StopIteration:
        stack.pop()
        continue
    if kind == 'child':
        stack.append(iter(events.get(val, [])))
    else:
        out.append('%s %s' % (kind, val))

print('\n'.join(out))
