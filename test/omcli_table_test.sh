#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# The command table of omcli (src/omci/cli/main.c) and vq_dispatch() in
# src/omci/respond/vqsrv.c agree on which stock setters omcid carries out. A
# row that writes or has no answer must have F_NOOMCID exactly when omcid has
# no case for its id, or one whose whole body is `return;` (a no-op): without
# the flag a command omcid drops is sent and prints nothing, and with it a
# command omcid handles is refused. Host-only: reads the two sources, builds
# nothing.
#
# The sources are parsed, not grepped: comments are stripped and whitespace
# normalised first, and anything the parser cannot classify (a table line that
# is not a row, a case it cannot delimit, no rows, no cases) fails the test
# instead of being skipped. The parser checks itself on small synthetic
# snippets before it reads the tree, so a formatting variant that would fool
# it is caught here and not by a reviewer.
set -u
cd "$(dirname "$0")/.." || exit 1
exec python3 -I - src/omci/cli/main.c src/omci/respond/vqsrv.c <<'PY'
import re, sys


class Unparsable(Exception):
    pass


def strip_comments(src):
    """Remove /* */ and // comments, leaving string and char literals alone."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            if j >= n:
                raise Unparsable("unterminated literal")
            out.append(src[i:j + 1])
            i = j + 1
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            if j < 0:
                raise Unparsable("unterminated comment")
            out.append(" ")
            i = j + 2
        elif src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
            out.append(" ")
        else:
            out.append(c)
            i += 1
    return "".join(out)


def function_body(src, header):
    """The text between the braces of the function whose header starts a line."""
    m = re.search(r"^" + re.escape(header) + r"\b[^;{]*\{", src, re.M)
    if not m:
        raise Unparsable("no function " + header)
    depth, i = 1, m.end()
    while i < len(src) and depth:
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        i += 1
    if depth:
        raise Unparsable("unbalanced braces in " + header)
    return src[m.end():i - 1]


LABEL = re.compile(r"\b(case\s+(\d+)\s*:|default\s*:)")


def dispatch_cases(func_src):
    """{id: 'noop' | 'live'} for the case labels of a function's switches.

    A body is a no-op only when, comments and whitespace aside, it is exactly
    `return;`; labels that fall through to one body share its class."""
    body = strip_comments(function_body(strip_comments(func_src), "static void vq_dispatch"))
    # Brace depth at every position, to tell a switch label from anything else.
    depth_at, d = [], 0
    for ch in body:
        d += {"{": 1, "}": -1}.get(ch, 0)
        depth_at.append(d)
    labels = [m for m in LABEL.finditer(body)]
    if not labels:
        raise Unparsable("no case labels")
    for m in labels:
        if depth_at[m.start()] != 1:
            raise Unparsable("case label inside nested braces at offset %d" % m.start())
    result, k = {}, 0
    while k < len(labels):
        group = [labels[k]]
        while k + 1 < len(labels) and not body[group[-1].end():labels[k + 1].start()].strip():
            k += 1
            group.append(labels[k])
        k += 1
        start = group[-1].end()
        d, i = 0, start
        while i < len(body):
            ch = body[i]
            if d == 0 and ch == "}":
                break
            if d == 0 and k < len(labels) and i >= labels[k].start():
                break
            d += {"{": 1, "}": -1}.get(ch, 0)
            i += 1
        text = " ".join(body[start:i].split())
        if not text:
            raise Unparsable("empty body after case at offset %d" % start)
        kind = "noop" if text == "return;" else "live"
        for m in group:
            if m.group(2) is not None:
                result[int(m.group(2))] = kind
    if not result:
        raise Unparsable("no numbered case")
    return result


def split_top(s, sep):
    parts, cur, d, i = [], [], 0, 0
    while i < len(s):
        c = s[i]
        if c in "\"'":
            j = i + 1
            while j < len(s) and s[j] != c:
                j += 2 if s[j] == "\\" else 1
            cur.append(s[i:j + 1])
            i = j + 1
            continue
        d += {"{": 1, "}": -1, "(": 1, ")": -1}.get(c, 0)
        if c == sep and d == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(c)
        i += 1
    parts.append("".join(cur))
    return parts


FLAGS = {"0", "F_WRITE", "F_NOOMCID"}


def table_rows(src):
    """[(group, name, id, answer, flags)] for the cmds[] initialiser."""
    src = strip_comments(src)
    m = re.search(r"\bstruct\s+cmd\s+cmds\s*\[\s*\]\s*=\s*\{", src)
    if not m:
        raise Unparsable("no cmds[] table")
    depth, i = 1, m.end()
    while i < len(src) and depth:
        c = src[i]
        if c in "\"'":
            i += 1
            while i < len(src) and src[i] != c:
                i += 2 if src[i] == "\\" else 1
        else:
            depth += {"{": 1, "}": -1}.get(c, 0)
        i += 1
    if depth:
        raise Unparsable("unbalanced cmds[]")
    inner = src[m.end():i - 1]
    rows = []
    for item in split_top(inner, ","):
        item = item.strip()
        if not item:
            continue
        if not (item.startswith("{") and item.endswith("}")):
            raise Unparsable("not a table row: " + item[:60])
        f = [x.strip() for x in split_top(item[1:-1], ",")]
        if f and f[-1] == "":
            f.pop()
        if len(f) != 7:
            raise Unparsable("row with %d fields: %s" % (len(f), item[:60]))
        grp, name, cid, ans, _arg, flags, _help = f
        if not (grp.startswith('"') and name.startswith('"') and cid.isdigit()
                and ans in ("ANS_NONE", "ANS_QUEUE", "ANS_FILE")):
            raise Unparsable("row does not parse: " + item[:60])
        fl = [x.strip() for x in flags.split("|")]
        if any(x not in FLAGS for x in fl):
            raise Unparsable("unknown flag in: " + item[:60])
        rows.append((grp.strip('"'), name.strip('"'), int(cid), ans, set(fl)))
    if not rows:
        raise Unparsable("no rows")
    return rows


def wrap(body):
    return "static void vq_dispatch(void)\n{\n\tswitch (id) {\n" + body + "\t}\n}\n"


def selfcheck():
    ok = bad = 0

    def t(name, got, want):
        nonlocal ok, bad
        if got == want:
            print("ok    parser: " + name)
            ok += 1
        else:
            print("FAIL  parser: %s: got %r, wanted %r" % (name, got, want))
            bad += 1

    def raises(name, fn):
        try:
            fn()
        except Unparsable:
            t(name, True, True)
        else:
            t(name, "parsed", "Unparsable")

    c = lambda body: dispatch_cases(wrap(body))
    t("bare return is a no-op", c("case 4:\n\treturn;\n"), {4: "noop"})
    t("blank line before return", c("case 4:\n\n\n\treturn;\n"), {4: "noop"})
    t("comment between label and return", c("case 4: /* x\n y */\n\t// z\n\treturn;\n"), {4: "noop"})
    t("statement after the label on its line", c("case 4: log_config();\n\treturn;\n"), {4: "live"})
    t("statement on the next line", c("case 4:\n\tlog_config();\n\treturn;\n"), {4: "live"})
    t("return with a value is not a bare return", c("case 4:\n\treturn 1;\n"), {4: "live"})
    t("fall-through labels share a no-op body",
      c("case 4: /* a */\ncase 41: // b\n\n\treturn;\n"), {4: "noop", 41: "noop"})
    t("fall-through labels share a live body",
      c("case 4:\ncase 41:\n\tdo_it();\n\tbreak;\n"), {4: "live", 41: "live"})
    t("a no-op followed by a live case", c("case 4:\n\treturn;\ncase 5:\n\tgo();\n\tbreak;\n"),
      {4: "noop", 5: "live"})
    t("a braced body is live", c("case 33: {\n\tint k;\n\treturn;\n\t}\ncase 32:\n\treturn;\n"),
      {33: "live", 32: "noop"})
    t("default is not a numbered case",
      c("case 7:\n\treturn;\ndefault:\n\tout(\"x\");\n\tbreak;\n"), {7: "noop"})
    raises("no case at all", lambda: c("foo();\n"))
    raises("a label with no body", lambda: c("case 4:\n"))
    raises("a label inside nested braces", lambda: c("case 4: {\n\tcase 5:\n\t\treturn;\n}\n"))
    raises("an unterminated comment", lambda: c("case 4: /* x\n\treturn;\n"))

    row = '{ "set", "x", %d, ANS_NONE, A_NONE, %s, "help, with a comma and a } brace" }'
    t("a row", table_rows("static const struct cmd cmds[] = {\n" + (row % (4, "F_NOOMCID")) + ",\n};\n"),
      [("set", "x", 4, "ANS_NONE", {"F_NOOMCID"})])
    t("indented rows, comments, a combined flag, several on a line",
      table_rows("static const struct cmd cmds[] = {\n  /* g */\n\t   " + (row % (4, "F_WRITE | F_NOOMCID")) +
                 ", // c\n   " + (row % (5, "0")) + ", " + (row % (6, "F_WRITE")) + ",\n};\n"),
      [("set", "x", 4, "ANS_NONE", {"F_WRITE", "F_NOOMCID"}), ("set", "x", 5, "ANS_NONE", {"0"}),
       ("set", "x", 6, "ANS_NONE", {"F_WRITE"})])
    raises("a line that is not a row",
           lambda: table_rows("static const struct cmd cmds[] = {\n" + (row % (4, "0")) + ",\nfoo bar,\n};\n"))
    raises("a row with a missing field",
           lambda: table_rows('static const struct cmd cmds[] = {\n{ "set", "x", 4, ANS_NONE, "h" },\n};\n'))
    raises("an unknown flag",
           lambda: table_rows("static const struct cmd cmds[] = {\n" + (row % (4, "F_BOGUS")) + ",\n};\n"))
    raises("an empty table", lambda: table_rows("static const struct cmd cmds[] = {\n};\n"))
    raises("no table", lambda: table_rows("int x;\n"))
    return ok, bad


def main():
    ok, bad = selfcheck()
    try:
        rows = table_rows(open(sys.argv[1]).read())
        cases = dispatch_cases(open(sys.argv[2]).read())
    except Unparsable as e:
        print("FAIL  cannot classify the sources: %s" % e)
        return 1
    for grp, name, cid, ans, flags in rows:
        marked = "F_NOOMCID" in flags
        writes = "F_WRITE" in flags or ans == "ANS_NONE"
        if not writes:
            continue
        live = cases.get(cid) == "live"
        if marked and live:
            print("FAIL  %s %s (id %d) is marked F_NOOMCID but vq_dispatch() handles it" % (grp, name, cid))
            bad += 1
        elif not marked and not live:
            print("FAIL  %s %s (id %d) writes, has no handler in vq_dispatch() and lacks F_NOOMCID" % (grp, name, cid))
            bad += 1
        else:
            print("ok    %s %s (id %d): %s" % (grp, name, cid, "F_NOOMCID, no handler" if marked else "handled"))
            ok += 1
    print("\n%d passed, %d failed" % (ok, bad))
    return 1 if bad or not ok else 0


sys.exit(main())
PY
