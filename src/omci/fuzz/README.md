# Fuzzing omcid on the build host

omcid is the one program on the stick that reads input from outside it:
OMCI frames from the OLT. This directory builds omcid for the build host,
with AddressSanitizer and UndefinedBehaviorSanitizer, and runs it under
libFuzzer. No stick, no Docker, no toolchain image.

    make fuzz-omci                    # from the top: 60 s per target
    make fuzz-omci FUZZ_SECONDS=600 FUZZ_JOBS=4
    make fuzz-omci-replay             # every seed and saved input, once

`make fuzz-omci` needs clang with the libFuzzer runtime (Debian and Ubuntu:
`clang` and `libclang-rt-dev`). The replay needs only `cc`, binutils and
`python3`; `make test-host` runs it plain and `make test-host-asan` with the
sanitizers.

## What runs

The whole daemon, as shipped. `respond/*.c`, `extvlan.c`, `mcastvlan.c`,
the generated tables and `diag/src/io.c` are compiled from the same files
with the same include paths as for the stick, and `main()` runs from its
first line to the signal handler that ends it. Three things differ, all in
the Makefile here and none in the sources:

- **The system call layer.** `src/diag/src/sys.h` is o32 MIPS assembly.
  The build passes `-include sys_host.h`, which declares the same `sys_*()`
  functions and defines the include guard of `sys.h`, so the real one
  contributes nothing. `fakeos.c` has the bodies.
- **`main` is `omcid_main`**, so the harness can call it.
- **`memcpy`, `memset`, `memmove`, `memcmp` of `io.c`** get other names: on
  the host libc has them, the sanitizer checks the calls, and libFuzzer
  sees the comparisons omcid makes with `memcmp`.

A build for the stick sees none of this: these are compiler flags of
this Makefile, and `sys_host.h` refuses to compile for MIPS.

## The kernel model (`fakeos.c`)

omcid fed made-up frames removes files, sends signals and arms alarms, so
it must not be anywhere near the real kernel. `fakeos.c` makes no system
call, and before `make replay` runs anything it checks that the binary it
built imports nothing outside a short list (`IMPORTS` in the Makefile:
memory, strings, `setjmp`, `abort`, and the stdio `replay.c` reads its
inputs with), so a `sys_*()` that someone gives a real body fails the test
the day it is written. It keeps:

| what | how |
|---|---|
| files | names and contents in memory; `/var`, `/proc`, `/tmp`, sysfs alike |
| message queues | by key and id; a request comes from the fuzz input, a reply is counted and dropped |
| netlink | registration, frames both ways, driver commands answered at once (every one succeeds, as in the trace build of `respond/drv.c`) |
| clock | starts at a fixed time, moves only when omcid waits |
| signals | `sys_signal` stores the handler, `sys_alarm` a deadline on that clock, `sys_kill` writes down what it was asked and sends nothing |

When the input is used up the model lets eight seconds pass, so the rebuild
after the last frame and the periodic work run, and then delivers SIGTERM.

An input can also kill omcid where it waits and have it started again, as
init does on a stick: the statics are put back (below), the files and
queues stay. The new omcid finds the snapshot of the old one and resumes
from it, which is the only way a snapshot with real contents gets read
here. `fuzz_local` can patch the snapshot in between.

The model stops a run with `abort()` for defects no sanitizer sees: a frame
put on the line that is not 48 bytes with a correct CRC, a signal to a
process group, a receive or send that would block for good. `stick.c` adds
two after the run: a file descriptor left open, and any signal other than
the SIGTERM to pid 1 of an OLT Reboot the configuration allowed.

## State between two inputs (`state.c`)

omcid keeps its state in statics and has nothing to reset them with: on a
stick a new state is a new process. Here the writable data of every omcid
object is put back before each input. The Makefile renames `.data` and
`.bss` of those objects to `omcid_data` and `omcid_bss`, the linker brackets
each with `__start_`/`__stop_` symbols, and `state.c` restores one from a
copy taken before the first run and zeroes the other. `check-sections.sh`
fails the build if an omcid object has writable data anywhere else, or a
common symbol, which is in no section (the build passes `-fno-common`).

That is a check of where the compiler put things. The check of the result
is in the replay: every input runs twice in a row, and what omcid let out
(every byte it printed, every frame, the number of every driver command)
has to be the same both times. State that gets from one run into the next
by any way at all shows there.

So every input, and every omcid started within one, begins as the omcid
of a fresh process, and a saved input reproduces on its own. What does persist across inputs is outside omcid and
outside what it can see: the heap of the process (the model frees its files
at each reset), the coverage counters, and the sanitizer runtimes.

## The targets

| target | input | what it is for |
|---|---|---|
| `fuzz_olt` | 48-byte records, each an OMCI frame on the line | the OLT, the only remote input. Byte 3 (device id) other than 0x0a turns a record into a pause, a SIGHUP, a respawn, a raw-CRC frame or the configuration of the stick; `fuzz_olt.c` has the list. The CRC is computed by the target |
| `fuzz_local` | records of kind, length, bytes | what root on the stick hands omcid: the config stores and files under `/var/run` and `/proc`, both CLI protocols, frames injected on the queue. Not a trust boundary; fuzzed for robustness |

Both run the same daemon; they differ in what the input can express.
`fuzz_olt` mutates whole records (a custom mutator), because a byte
inserted in a session shifts every later frame out of place.

## The corpus

Nothing binary is in the tree. `seeds.py` builds the seeds on every run
from text: the session fixtures of `test/fixtures` (already scrubbed of
identities), the literal frames in the suites of `src/omci`, the config
stores of `src/omci/test`, and `corpus/<target>/*.txt`, which holds inputs
written by hand, the frames of the suites that build theirs in shell
(`recorded-*.txt`, taken from a run of the suite), and **every input that
once broke omcid**. Those are hex with comments; the comment says what the
input is and what it broke.

A comment line `# expect: <text>` in such a file is something omcid has to
print when the input runs, and the replay fails when it does not. A
regression input should have one: the line omcid prints when it refuses or
survives what the input does. The inputs written by hand and the recorded
ones have one or more too: a line only the code they were written for
prints (an `event=` line where there is one). Without it the input only fails where a
sanitizer sees the damage, which `make test-host` has none of, and it goes
on passing when the layout it was aimed at has moved and it no longer
reaches the code it was written for.

When the fuzzer finds something it saves the input under
`build/artifacts/` and `make fuzz-omci` prints it as hex. To keep it:

    python3 src/omci/fuzz/seeds.py --hex src/omci/fuzz/build/artifacts/olt-crash-... \
        > src/omci/fuzz/corpus/olt/<what-it-broke>.txt

then say in a comment at the top what it is, fix omcid, and check with
`make fuzz-omci-replay` (`build/replay*/fuzz_olt -v <file>` shows the run:
what omcid printed and sent). Each input of the replay runs in its own
process under `timeout` (`REPLAY_TIMEOUT`, 10 s; the longest takes under
half a second), so an input that makes omcid loop fails the test within
seconds, by name, instead of holding it. The working corpus the fuzzer grows
(`build/corpus/`) is not kept in the tree; CI keeps it in a cache.

## What this does not cover

- **Byte order and word size.** The stick is big-endian with 32-bit `long`;
  the host is neither. omcid reads frames a byte at a time, so the OLT path
  is the same code, but a defect that needs a 32-bit `long` to overflow, or
  a byte order, is not found here.
- **The driver.** Every command succeeds, or (a bit of the stick record)
  one fails or none is answered. What the kernel does with a command is
  the host tests of `test/`.
- **Signals at any instant.** A handler runs where omcid waits for input,
  not between two arbitrary instructions.
- **Signed overflow.** The build keeps `-fno-strict-overflow` of
  `src/harden.mk`, under which it wraps, so it is not reported.
