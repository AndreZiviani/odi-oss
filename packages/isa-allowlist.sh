#!/usr/bin/env sh
#
# The ISA gate, the other way round: refuse any instruction NOT confirmed to
# execute on this CPU.
#
#     isa-allowlist.sh <binary> [<binary>...]
#
# isa-audit.sh is a DENY list -- it names the instructions known to trap and
# counts them. That shape is not enough: an audit of exactly that form passed
# a dropbear carrying 147 `teq`, because nobody had put `teq` on the list yet.
# This one keeps the list of mnemonics CONFIRMED PRESENT and reports
# everything else; a new one appearing is not automatically wrong, but it is
# automatically unverified, and the answer is to execute it on the device and
# extend the list, never to extend the list from a datasheet.
#
# The list and the audit are isa-allowlist in the toolchain image (the
# odi-toolchain repository, isa/isa-audit). Exit 2 means unverified only,
# 1 a trap or a file that could not be audited; callers here treat both as a
# failure. Same staging as isa-audit.sh, which this runs.
ISA_TOOL=isa-allowlist; export ISA_TOOL
exec "$(dirname "$0")/isa-audit.sh" "$@"
