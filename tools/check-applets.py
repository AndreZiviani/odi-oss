#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later
"""Refuse an on-device script that runs a command the image does not have.

    tools/check-applets.py [--list] [--applets FILE] SCRIPT...

busybox is built from packages/busybox/applets.txt, and an applet that is not
on it is not "applet not found" at build time: it is that message on stderr
and an empty pipeline, at run time, on the stick (config.fragment). So this
reads every script that runs on the stick and checks each command it runs
against what the image ships:

  - a shell builtin or keyword, or a function defined in one of the scripts;
  - an applet in packages/busybox/applets.txt;
  - one of OTHERS below, our binaries and the rest of image/build.sh;
  - a script in rootfs/skeleton, or a path under /tmp or /var, where a
    tool pushed to the stick lives (tools/regdump, tools/mcast-probe).

Anything else is reported with its file and line, and the exit status is 1.
--list prints every command found instead, with where. --applets reads
another list than packages/busybox/applets.txt (test/check_applets_test.sh).

/etc/inittab is read too: the process field of each line is a command.

What counts as running a command: the first word of each simple command,
inside $( ), backticks, subshells, pipelines, $(( )) and here-documents
that expand too; the command behind exec, nohup, nice, setsid, busybox,
command, env, xargs, chroot, timeout and respawn.sh; the script of `sh -c`
(or -ec) when it is a literal; a word that is just
"$VAR", by the defaults the scripts give VAR (VAR=${VAR:-sleep}); and the
command behind a function of ours that runs "$@" (diag-bundle.sh step), past
the arguments it shifts off first.

What it cannot see: a command built at run time ("$@" of a script, a
variable with no default here), and text a script hands to something else to
run. test-qemu and the chroot test run the real image and catch those.
It reads shell the way these scripts are written, not every way sh allows:
a variable default or a wrapper function counts in every script, a
function in every script sourced together with its own, and a << inside a
string that spans lines starts a here-document. Each can hide a missing
applet in a script written to provoke it; none does in the scripts here.
Nor the web UI (odi-ui) and the exporter (odi-sfp-exporter), which live in
their own repositories: OTHER_REPOS below lists what they run, by hand, and
is checked against the image the same way.

python3 and its standard library only, as tools/sbom.py.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APPLETS = os.path.join(ROOT, "packages", "busybox", "applets.txt")
SKELETON = os.path.join(ROOT, "rootfs", "skeleton")

# What the image ships besides busybox (image/build.sh): our binaries,
# dropbear and its links, iproute2 ip.
OTHERS = {
    "diag", "omcid", "omcli", "omcicli", "omciprobe", "omcicap", "nv",
    "igmpd", "metricsd", "confd",
    "dropbearmulti", "dropbear", "dropbearkey", "scp", "ip", "busybox",
}

# ash builtins and keywords. echo, printf, test, [ and kill are builtins of
# ash as well as applets; they are on applets.txt regardless.
BUILTINS = {
    ".", ":", "[", "[[", "alias", "bg", "break", "cd", "chdir", "command",
    "continue", "echo", "eval", "exec", "exit", "export", "false", "fg",
    "getopts", "hash", "jobs", "kill", "let", "local", "printf", "pwd",
    "read", "readonly", "return", "set", "shift", "source", "test", "times",
    "trap", "true", "type", "ulimit", "umask", "unalias", "unset", "wait",
}

# xargs options that take the next word as their argument.
XARGS_ARG = {"-n", "-s", "-I", "-P", "-E", "-d", "-L", "-a"}

# What odi-ui (confd) and the exporter (metricsd) run, from their own
# repositories: execve by path, and the shell snippets of confd. They are
# not read here, so this list stands in for them and every name on it must
# be in the image.
OTHER_REPOS = {
    "sh", "mkdir", "md5sum", "ping", "reboot", "kill", "pidof", "sleep",
    "nv", "diag", "omcicli",
}

START_KW = {"then", "do", "else", "elif", "if", "while", "until", "!", "{", "time"}
END_KW = {"fi", "done", "esac", "}"}
NAME = r"[A-Za-z_][A-Za-z0-9_]*"
VAR_RE = re.compile(r'^"?\$\{?(' + NAME + r'|[0-9@*])\}?"?$')
ASSIGN_RE = re.compile(r"^(" + NAME + r")=(.*)$", re.S)
DEFAULT_RE = re.compile(r'^"?\$\{(?:' + NAME + r'|[0-9]):?-([^}$`"\']*)\}"?$')
PLAIN_RE = re.compile(r"^[A-Za-z0-9_./-]+$")
HEREDOC_RE = re.compile(r"<<(-?)\s*(['\"]?)(" + NAME + r")\2")


def heredoc_ops(line):
    """The << operators of one line that the shell would see: not inside
    quotes, not in a comment."""
    ops, q, i = [], None, 0
    while i < len(line):
        c = line[i]
        if q:
            if c == "\\" and q == '"':
                i += 1
            elif c == q:
                q = None
        elif c == "\\":
            i += 1
        elif c in "'\"":
            q = c
        elif c == "#" and (i == 0 or line[i - 1] in " \t;&|("):
            break
        elif line.startswith("<<", i):
            m = HEREDOC_RE.match(line, i)
            if m:
                ops.append(m)
                i = m.end()
                continue
        i += 1
    return ops


def expanding(body):
    """A line of an expanding here-document as `: "LINE"`: the double quotes
    of the text escaped, those inside a $( ) or backticks kept."""
    out, depth, bq = [], 0, False
    i = 0
    while i < len(body):
        c = body[i]
        if c == "\\":
            out.append(body[i:i + 2])
            i += 2
            continue
        if body.startswith("$(", i):
            depth += 1
            out.append("$(")
            i += 2
            continue
        if c == ")" and depth:
            depth -= 1
        elif c == "`":
            bq = not bq
        elif c == '"' and not depth and not bq:
            out.append('\\"')
            i += 1
            continue
        out.append(c)
        i += 1
    return ': "' + "".join(out) + '"'


def blank_heredocs(text):
    """The text with every here-document body taken out, line numbers kept.
    A body with an unquoted delimiter is expanded by the shell, so each of
    its lines stays as `: "LINE"`: its $( ) and backticks are still read."""
    out, lines, i = [], text.split("\n"), 0
    while i < len(lines):
        out.append(lines[i])
        for m in heredoc_ops(lines[i]):
            strip, quoted, delim = m.group(1), m.group(2), m.group(3)
            while i + 1 < len(lines):
                i += 1
                body = lines[i].lstrip("\t") if strip else lines[i]
                if body == delim:
                    out.append("")
                    break
                out.append("" if quoted else expanding(body))
        i += 1
    return "\n".join(out)


class Word:
    """One shell word: its text with quotes removed, as written, and whether
    it holds no expansion at all."""

    def __init__(self, text, raw, literal):
        self.text, self.raw, self.literal = text, raw, literal


class Script:
    """The simple commands of one script, with the function each is in."""

    def __init__(self, text, path="-"):
        self.path = path
        self.t = blank_heredocs(text)
        self.n = len(self.t)
        self.cmds = []          # (words, line, function or None)
        self.funcs = set()
        self.brace = 0          # depth of { } groups
        self.infunc = []        # (name, brace depth its body opened at)
        self.pending = None     # a name() seen, its body not yet opened
        self.in_bq = False      # inside backticks: a backtick ends a word
        self.commands(0, None)

    def line(self, i):
        return self.t.count("\n", 0, i) + 1

    def squote(self, i):
        j = self.t.find("'", i)
        return self.n if j < 0 else j + 1

    def dquote(self, i):
        while i < self.n:
            c = self.t[i]
            if c == "\\":
                i += 2
            elif c == '"':
                return i + 1
            elif c == "`":
                i = self.commands(i + 1, "`")
            elif c == "$":
                i = self.dollar(i)
            else:
                i += 1
        return i

    def parens(self, i, depth):
        """Past the end of (( )) or $(( )); a $( ) or `` ` `` in it is read."""
        while i < self.n and depth:
            c = self.t[i]
            if c == "$" and self.t.startswith("$(", i) and not self.t.startswith("$((", i):
                i = self.commands(i + 2, ")")
                continue
            if c == "`":
                i = self.commands(i + 1, "`")
                continue
            depth += {"(": 1, ")": -1}.get(c, 0)
            i += 1
        return i

    def dollar(self, i):
        if self.t.startswith("$((", i):
            return self.parens(i + 3, 2)
        if self.t.startswith("$(", i):
            return self.commands(i + 2, ")")
        if self.t.startswith("${", i):
            depth, i = 1, i + 2
            while i < self.n and depth:
                c = self.t[i]
                if c == "\\":
                    i += 2
                elif c == "'":
                    i = self.squote(i + 1)
                elif c == '"':
                    i = self.dquote(i + 1)
                elif c == "$":
                    i = self.dollar(i)
                elif c == "`":
                    i = self.commands(i + 1, "`")
                else:
                    depth += {"{": 1, "}": -1}.get(c, 0)
                    i += 1
            return i
        i += 1
        if i < self.n and self.t[i] in "?#@*!$-0123456789":
            return i + 1
        while i < self.n and (self.t[i].isalnum() or self.t[i] == "_"):
            i += 1
        return i

    def word(self, i):
        start, buf, literal = i, [], True
        while i < self.n:
            c = self.t[i]
            if c in " \t\n;&|<>()" or (c == "`" and self.in_bq):
                break
            if c == "\\":
                buf.append(self.t[i + 1:i + 2])
                i += 2
            elif c == "'":
                j = self.squote(i + 1)
                buf.append(self.t[i + 1:j - 1])
                i = j
            elif c == '"':
                j = self.dquote(i + 1)
                seg = self.t[i + 1:j - 1]
                literal = literal and "$" not in seg and "`" not in seg
                buf.append(seg)
                i = j
            elif c == "$":
                i = self.dollar(i)
                literal = False
                buf.append("$")
            elif c == "`":
                i = self.commands(i + 1, "`")
                literal = False
            else:
                buf.append(c)
                i += 1
        return Word("".join(buf), self.t[start:i], literal), i

    def keyword(self, w):
        """Track { } so each command knows the function it is in."""
        if w == "{":
            self.brace += 1
            if self.pending:
                self.infunc.append((self.pending, self.brace))
                self.pending = None
        elif w == "}":
            if self.infunc and self.infunc[-1][1] == self.brace:
                self.infunc.pop()
            self.brace -= 1

    def commands(self, i, end):
        """Scan a command list from i to `end`; return the index after it."""
        outer, self.in_bq = self.in_bq, end == "`" or (self.in_bq and end is None)
        try:
            return self._commands(i, end)
        finally:
            self.in_bq = outer

    def _commands(self, i, end):
        words, at = [], 0
        mode = None  # None, "for", "case-word", "case-pat"

        def flush():
            nonlocal words
            if words:
                fn = self.infunc[-1][0] if self.infunc else None
                self.cmds.append((words, at, fn))
            words = []

        while i < self.n:
            c = self.t[i]
            if end and c == end and mode != "case-pat":
                flush()
                return i + 1
            if c in " \t":
                i += 1
            elif self.t.startswith("\\\n", i):
                i += 2
            elif c == "#" and (i == 0 or self.t[i - 1] in " \t\n;&|()"):
                j = self.t.find("\n", i)
                i = self.n if j < 0 else j
            elif c in "\n;&|":
                flush()
                if self.t.startswith(";;", i):
                    mode = "case-pat"
                    i += 2
                else:
                    i += 2 if self.t[i:i + 2] in ("&&", "||") else 1
            elif c == "(":
                if mode == "case-pat":
                    i += 1
                elif self.t.startswith("((", i):
                    i = self.parens(i + 2, 2)
                elif words and self.t.startswith("()", i):
                    self.funcs.add(words[0].text)
                    self.pending = words[0].text
                    words = []
                    i += 2
                else:
                    flush()
                    i = self.commands(i + 1, ")")
            elif c == ")":
                if mode == "case-pat":
                    mode = None
                i += 1
            elif c in "<>":
                j = i
                while j < self.n and self.t[j] in "<>&-|":
                    j += 1
                while j < self.n and self.t[j] in " \t":
                    j += 1
                _, i = self.word(j)
            else:
                s = i
                w, i = self.word(i)
                if not w.raw:
                    i += 1
                elif w.raw.isdigit() and i < self.n and self.t[i] in "<>":
                    pass  # the fd of a redirection: 2>, 9>>
                elif mode == "case-pat":
                    if w.literal and w.text == "esac":
                        mode = None
                elif mode == "case-word":
                    if w.text == "in":
                        mode = "case-pat"
                elif mode == "for":
                    if w.text == "do":
                        mode = None
                elif not words and w.literal and w.text in START_KW | END_KW:
                    self.keyword(w.text)
                elif not words and w.literal and w.text == "for":
                    mode = "for"
                elif not words and w.literal and w.text == "case":
                    mode = "case-word"
                else:
                    if not words:
                        at = self.line(s)
                    words.append(w)
        flush()
        return i


class Checker:
    def __init__(self, scripts):
        self.scripts = scripts
        self.funcs = set()
        self.defaults = {}
        self.rawvals = {}       # every value a variable is given, as written
        for sc in scripts:
            self.funcs |= sc.funcs
            for words, _, _ in sc.cmds:
                for w in words:
                    m = ASSIGN_RE.match(w.raw)
                    if not m:
                        break
                    self.rawvals.setdefault(m.group(1), []).append(m.group(2))
                    d = DEFAULT_RE.match(m.group(2))
                    if d and d.group(1):
                        self.defaults.setdefault(m.group(1), set()).add(d.group(1))
                    elif PLAIN_RE.match(m.group(2)):
                        self.defaults.setdefault(m.group(1), set()).add(m.group(2))
        # A function of ours that runs "$@": how many arguments it shifts
        # off before that, so its callers are read past them.
        self.wrappers = {}
        for sc in scripts:
            shifts = {}
            for words, _, fn in sc.cmds:
                if fn is None or fn in self.wrappers:
                    continue
                if words[0].literal and words[0].text == "shift":
                    n = words[1].text if len(words) > 1 else "1"
                    shifts[fn] = shifts.get(fn, 0) + (int(n) if n.isdigit() else 1)
                    continue
                found = []
                self.run(words, found, "", 0, wrappers=False)
                if any(name == '"$@"' for name, _ in found):
                    self.wrappers[fn] = shifts.get(fn, 0)

    def resolve(self, w):
        """The command names a word can be; [] when it is built at run time."""
        if w.literal:
            return [w.text]
        m = VAR_RE.match(w.raw)
        if m and m.group(1) in self.defaults:
            return sorted(self.defaults[m.group(1)])
        if w.raw in ('"$@"', "$@"):
            return ['"$@"']
        return []

    def run(self, words, out, where, depth, wrappers=True):
        """Append (name, where) for each command one simple command runs."""
        k = 0
        while k < len(words) and ASSIGN_RE.match(words[k].raw):
            k += 1
        while k < len(words):
            names = self.resolve(words[k])
            out += [(n, where) for n in names]
            if not names:
                return
            name = os.path.basename(names[0])
            rest = [w.text for w in words[k + 1:]]
            k += 1
            if name in ("exec", "nohup", "nice", "setsid", "busybox", "command"):
                if name == "command" and rest[:1] in (["-v"], ["-V"]):
                    return
                while k < len(words) and words[k].text.startswith("-"):
                    k += 2 if name == "nice" and words[k].text == "-n" else 1
            elif name == "timeout":
                while k < len(words) and words[k].text.startswith("-"):
                    k += 2 if words[k].text in ("-s", "-k") else 1
                k += 1
            elif name in ("env", "xargs"):
                while k < len(words) and (words[k].text.startswith("-")
                                          or ASSIGN_RE.match(words[k].raw)):
                    takes = name == "xargs" and words[k].text in XARGS_ARG
                    k += 2 if takes else 1
            elif name == "chroot":
                k += 1
            elif name == "respawn.sh":
                k += 3
            elif name in ("sh", "ash") and any(re.match(r"^-[a-z]*c[a-z]*$", r)
                                               for r in rest[:3]):
                while k < len(words) and words[k].text.startswith("-"):
                    k += 1
                if k < len(words) and words[k].literal and depth < 3:
                    sub = Script(words[k].text)
                    for ws, _, _ in sub.cmds:
                        self.run(ws, out, where, depth + 1, wrappers)
                return
            elif wrappers and name in self.wrappers:
                k += self.wrappers[name]
            else:
                return

    def groups(self):
        """The scripts that share functions: one that sources another
        (. FILE) sees its functions, and the one sourced sees those of the
        one sourcing it (rcS.dev runs inside rcS). Joined both ways."""
        names = {os.path.basename(s.path): s.path for s in self.scripts}
        group = {s.path: {s.path} for s in self.scripts}
        for sc in self.scripts:
            for words, _, _ in sc.cmds:
                if not (words[0].literal and words[0].text in (".", "source")
                        and len(words) > 1):
                    continue
                m = VAR_RE.match(words[1].raw)
                cands = [words[1].raw] + (self.rawvals.get(m.group(1), [])
                                          if m else [])
                for c in cands:
                    for base in re.findall(r"[A-Za-z0-9_.-]+", c):
                        other = names.get(base)
                        if other and group[other] is not group[sc.path]:
                            merged = group[other] | group[sc.path]
                            for p in merged:
                                group[p] = merged
        return group

    def commands(self):
        """(name, where, the functions visible there) for every command:
        a function counts in its own script and its group (groups())."""
        out, group = [], self.groups()
        byname = {s.path: s for s in self.scripts}
        for sc in self.scripts:
            funcs = set()
            for p in group[sc.path]:
                funcs |= byname[p].funcs
            found = []
            for words, line, _ in sc.cmds:
                self.run(words, found, f"{sc.path}:{line}", 0)
            out += [(n, w, funcs) for n, w in found]
        return out


def shipped(path):
    names = set()
    with open(path, encoding="utf-8") as f:
        for line in f:
            names.update(line.split("#", 1)[0].split())
    return names


def known(name, applets, funcs):
    if name in BUILTINS or name in funcs or name == '"$@"':
        return True
    if name.startswith("/"):
        if name.startswith(("/tmp/", "/var/")):
            return True
        if os.path.isfile(SKELETON + name):
            return True
        if os.path.dirname(name) not in ("/bin", "/sbin", "/usr/bin", "/usr/sbin"):
            return False
        name = os.path.basename(name)
    return name in applets or name in OTHERS


def main(argv):
    listing, applets_file = False, APPLETS
    while argv[:1] in (["--list"], ["--applets"]):
        if argv[0] == "--list":
            listing, argv = True, argv[1:]
        else:
            applets_file, argv = argv[1], argv[2:]
    paths = argv
    if not paths:
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    scripts = []
    for p in paths:
        with open(p, encoding="utf-8", errors="replace") as f:
            text = f.read()
        if os.path.basename(p) == "inittab":
            # id:runlevels:action:process, and the process is a command.
            text = "\n".join(line.split(":", 3)[3] if line.count(":") >= 3
                             and not line.startswith("#") else ""
                             for line in text.split("\n"))
        scripts.append(Script(text, p))
    chk = Checker(scripts)
    found = chk.commands()
    if listing:
        for name, where, _ in found:
            print(f"{name}\t{where}")
        return 0
    applets = shipped(applets_file)
    bad = [(n, w) for n, w, funcs in found if not known(n, applets, funcs)]
    bad += [(n, "odi-ui or the exporter (OTHER_REPOS)")
            for n in sorted(OTHER_REPOS) if not known(n, applets, set())]
    for name, where in bad:
        print(f"{where}: runs `{name}`, which the image does not ship "
              f"(not an applet in packages/busybox/applets.txt, not a builtin, "
              f"not one of ours)", file=sys.stderr)
    if bad:
        return 1
    print(f"check-applets: {len(paths)} files, {len(set(n for n, _, _ in found))} "
          f"distinct commands, all in the image")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
