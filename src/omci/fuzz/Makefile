# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# omcid on the build host, for fuzzing: the daemon as shipped, linked against
# a model of the kernel instead of the kernel (README.md here).
#
#   make replay            every seed and saved input through each target,
#                          built with cc; ODI_SAN_CFLAGS adds the sanitizers
#                          (make test-host and make test-host-asan run this)
#   make fuzz              the targets side by side under libFuzzer with ASan
#                          and UBSan for FUZZ_SECONDS (default 60), FUZZ_JOBS
#                          processes each
#   make fuzz TARGETS=olt  one target
#
# Needs clang with its libFuzzer runtime for `fuzz` (Debian and
# Ubuntu: clang and libclang-rt-dev); `replay` needs only cc, binutils and
# python3. No Docker, no toolchain image: nothing here is built for the
# target, and nothing here ends up in the image.

TOP     := ../../..
RESPOND := ../respond
DIAG    := ../../diag
UAPI    := $(TOP)/kernel/extra/drivers/net/ethernet/odi/uapi
BUILD   := build

TARGETS      ?= olt local
FUZZ_CC      ?= clang
FUZZ_SECONDS ?= 60
FUZZ_JOBS    ?= 1
# The longest input: 320 records of fuzz_olt.c. A session fixture is 269.
FUZZ_MAX_LEN ?= 15360
OBJCOPY      ?= objcopy

# The translation units of omcid: SRCS of ../respond/Makefile, kept in step
# with it by `check-srcs` below, and the objects that Makefile adds to them.
SRCS := main mibstore drv apply apply_qos apply_uni apply_bridge show clisrv vqsrv omcimsg cfgstore snapshot events swimage reload rereg alarm mcast pm voip frames
OMCID := $(SRCS) io extvlan mcastvlan omci_mib omci_autonomous omci_defaults omci_drv omci_bdgconn
HARNESS := fakeos stick state

VPATH := $(RESPOND) .. ../generated $(DIAG)/src

# How omcid is compiled here. The sources and the include paths are those of
# ../respond/Makefile; what differs is what a host needs:
#   -include sys_host.h   the system call layer (sys_host.h says how)
#   -fno-strict-overflow  from src/harden.mk: signed overflow wraps in the
#                         shipped binary, so it is not reported here either
#   -ftrivial-auto-var-init=zero  also from harden.mk: a local nobody wrote
#                         is zero on the stick, so it is zero here too
#   -O2 -g                fast enough to fuzz, and stack traces with lines
#   -fno-common -fno-lto  every variable in a section of its object file. A
#                         common symbol is in none, and the state reset is
#                         by section (check-sections.sh looks for one too)
# No -Wall here: the warnings of omcid are those of the target build, which
# has the compiler that counts. A host compiler, clang above all, warns
# about other things, and a test run is not the place to print them.
OMCID_CFLAGS := -std=c11 -O2 -g -fno-strict-overflow -ftrivial-auto-var-init=zero \
                -fno-common -fno-lto \
                -fno-omit-frame-pointer -include sys_host.h \
                -I$(DIAG)/src -I../generated -I$(UAPI) -I$(RESPOND)
HARNESS_CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer \
                  -I$(DIAG)/src -I../generated -I$(UAPI)

SAN := -fsanitize=address,undefined -fno-sanitize-recover=undefined

# Two builds of the same objects, each in a directory of its own:
#   fuzz    clang, ASan, UBSan, libFuzzer coverage on omcid only
#   replay  cc and whatever ODI_SAN_CFLAGS holds; a second directory when it
#           holds something, so the plain and the sanitized objects never mix
FLAVOURS := fuzz replay replay-san
REPLAY   := $(if $(strip $(ODI_SAN_CFLAGS)),replay-san,replay)

CC_fuzz        := $(FUZZ_CC)
CC_replay      := cc
CC_replay-san  := cc
# What every object of the build gets, and what only omcid gets on top.
ALL_fuzz        := $(SAN)
ALL_replay      :=
ALL_replay-san  := $(ODI_SAN_CFLAGS)
# -fno-sanitize-address-globals-dead-stripping: with it clang gives every
# global a section of its own, and the rename below is by section name.
INSTR_fuzz       := -fsanitize=fuzzer-no-link -fno-sanitize-address-globals-dead-stripping \
                    -fsanitize-ignorelist=sanitize-ignore.txt \
                    -fsanitize-coverage-ignorelist=sanitize-ignore.txt
INSTR_replay     :=
INSTR_replay-san :=
LINK_fuzz       := $(SAN) -fsanitize=fuzzer
LINK_replay     :=
LINK_replay-san := $(ODI_SAN_CFLAGS)
DRIVER_fuzz       :=
DRIVER_replay     := replay
DRIVER_replay-san := replay
DEFS_fuzz       := -DFUZZ_LIBFUZZER
DEFS_replay     :=
DEFS_replay-san :=

OMCID_HDRS := sys_host.h sanitize-ignore.txt $(wildcard $(RESPOND)/*.h ../*.h ../generated/*.h \
                $(DIAG)/src/io.h $(UAPI)/*.h)
HARNESS_HDRS := sys_host.h fakeos.h harness.h

# An omcid object: compiled, then its .data and .bss renamed, so that state.c
# can put all the variables of omcid back between two runs and none of the
# harness. main() becomes omcid_main() (stick.c calls it), and the
# memcpy/memset/memmove/memcmp io.c supplies for the freestanding link get other
# names: here libc has them, and the sanitizer watches those.
EXTRA_main := -Dmain=omcid_main
EXTRA_io   := -Dmemcpy=odi_io_memcpy -Dmemset=odi_io_memset -Dmemmove=odi_io_memmove \
              -Dmemcmp=odi_io_memcmp

define flavour
$(BUILD)/$(1)/omcid/%.o: %.c $$(OMCID_HDRS) Makefile check-sections.sh | $(BUILD)/$(1)/omcid
	$$(CC_$(1)) $$(OMCID_CFLAGS) $$(ALL_$(1)) $$(INSTR_$(1)) $$(EXTRA_$$*) -c -o $$@.tmp.o $$<
	$$(OBJCOPY) --rename-section .data=omcid_data --rename-section .bss=omcid_bss \
	            --rename-section .data.rel.local=omcid_data \
	            --rename-section .data.rel=omcid_data $$@.tmp.o $$@.ren.o
	@rm -f $$@.tmp.o
	@sh check-sections.sh $$@.ren.o || { rm -f $$@.ren.o; exit 1; }
	@mv $$@.ren.o $$@

# state.c takes no sanitizer: it copies across the red zones on purpose.
$(BUILD)/$(1)/state.o: state.c $$(HARNESS_HDRS) Makefile | $(BUILD)/$(1)/omcid
	$$(CC_$(1)) $$(HARNESS_CFLAGS) -c -o $$@ $$<

$(BUILD)/$(1)/%.o: %.c $$(HARNESS_HDRS) $$(OMCID_HDRS) Makefile | $(BUILD)/$(1)/omcid
	$$(CC_$(1)) $$(HARNESS_CFLAGS) $$(ALL_$(1)) $$(DEFS_$(1)) -c -o $$@ $$<

$(BUILD)/$(1)/fuzz_%: $(BUILD)/$(1)/fuzz_%.o $$(HARNESS:%=$(BUILD)/$(1)/%.o) \
                      $$(DRIVER_$(1):%=$(BUILD)/$(1)/%.o) \
                      $$(OMCID:%=$(BUILD)/$(1)/omcid/%.o)
	$$(CC_$(1)) $$(LINK_$(1)) -o $$@ $$^

$(BUILD)/$(1)/omcid:
	mkdir -p $$@
endef
$(foreach f,$(FLAVOURS),$(eval $(call flavour,$(f))))

.SECONDARY:
.PHONY: all replay fuzz minimize seeds check-srcs clean

all: replay

# A translation unit added to omcid and not to this list would only show as
# an undefined symbol, or not at all; say it in words.
check-srcs:
	@want=$$(sed -n 's/^SRCS := //p' $(RESPOND)/Makefile); \
	if [ "$$want" != "$(SRCS)" ]; then \
		echo "fuzz: SRCS here and in $(RESPOND)/Makefile differ; copy the list over" >&2; \
		exit 1; \
	fi

# The seed corpus of each target, rebuilt from the text it comes from on
# every run (seeds.py says what that is). Cheap, and never stale.
seeds:
	@for t in $(TARGETS); do \
		python3 -I seeds.py $$t $(BUILD)/seeds/$$t || exit 1; \
	done

# Every seed through every target. This is the regression test: the inputs
# that once broke omcid are in corpus/, and they are seeds. replay.c runs
# each input twice and compares what omcid let out, and holds an input to
# the `# expect:` lines of its corpus file.
#
# Before anything runs, the binary is asked what it imports. omcid fed
# made-up frames sends signals, arms alarms and removes files; all of that
# has to end in fakeos.c, and a sys_*() that someone gives a real body would
# end in the kernel instead. A replay binary is the harness, omcid and libc
# (the fuzz build also holds libFuzzer, which has signal handlers of its own
# and cannot be asked this way). IMPORTS is what it has a reason to import:
# memory and strings, setjmp for sys_exit(), abort, the environment, and
# the stdio, stat and scandir with which replay.c reads its inputs. A name
# that is not on it fails here and has to be looked at; add it only when it
# cannot reach the kernel on behalf of omcid. The stdio that could (fopen)
# is on it for replay.c alone, so no other file of the harness may hold it.
#
# `timeout`: the model ends a run in which omcid waits for good, but not
# one in which it loops without waiting, and a test that never comes back
# is the one failure `make test-host` cannot report.
#
# The state reset needs ELF: objcopy to rename sections, and the start and
# stop symbols the GNU and LLVM linkers make for them. On any other host
# the replay says so and `make test-host` goes on.
IMPORTS := mem(cpy|move|set|cmp|chr)|str(len|n?cmp|n?cpy|r?chr|c?spn|str)|malloc|calloc|realloc|free|abort|exit|_?setjmp|longjmp|getenv|setenv|f(open|close|read|write|gets|putc|puts|printf|flush)|v?snprintf|put(c|s|char)|printf|perror|stderr|stdout|stat|scandir|__(asan|ubsan|sanitizer|sancov)_[a-z0-9_]*|__[a-z0-9_]*_chk|__isoc[0-9]+_[a-z]*|__stack_chk_(fail|guard)|__libc_start_main|__cxa_(finalize|atexit)|__gmon_start__|_ITM_[A-Za-z]*|__errno_location
REPLAY_TIMEOUT ?= 300
ifneq ($(shell uname -s),Linux)
replay:
	@echo "skip  fuzz replay: the host build of omcid needs an ELF host (objcopy, section start symbols)"
else
replay: check-srcs seeds $(TARGETS:%=$(BUILD)/$(REPLAY)/fuzz_%)
	@if grep -nE 'fopen|fdopen|freopen|FILE \*|popen|tmpfile *\(' fakeos.c stick.c state.c $(TARGETS:%=fuzz_%.c); then \
		echo "FAIL  fuzz replay: only replay.c opens files of the host (the lines above)"; exit 1; \
	fi
	@for t in $(TARGETS); do \
		new=$$(nm -u $(BUILD)/$(REPLAY)/fuzz_$$t | sed 's/@.*//' | awk '{ print $$NF }' | \
			grep -vxE '$(IMPORTS)' || true); \
		if [ -n "$$new" ]; then \
			echo "FAIL  fuzz replay: fuzz_$$t imports" $$new "and IMPORTS in the Makefile does not have it (fakeos.c has to be the only kernel omcid sees)"; \
			exit 1; \
		fi; \
	done
	@for t in $(TARGETS); do \
		timeout $(REPLAY_TIMEOUT) $(BUILD)/$(REPLAY)/fuzz_$$t $(BUILD)/seeds/$$t 2>$(BUILD)/replay-$$t.log || { \
			tail -n 60 $(BUILD)/replay-$$t.log; \
			echo "FAIL  fuzz replay: $$t (the last input named above)"; exit 1; }; \
		echo "ok    fuzz replay: $$t, $$(tail -n 1 $(BUILD)/replay-$$t.log)"; \
	done
endif

# libFuzzer, for a bounded time. The working corpus is $(BUILD)/corpus/<t>
# and is kept between runs, so a second run goes on where the first stopped
# (CI keeps it in a cache for the same reason); the seeds are read in each
# time. An input that breaks omcid is saved under $(BUILD)/artifacts and
# printed as hex, which is the form it goes into corpus/ in.
#
# FUZZ_JOBS processes of a target share its corpus directory and pick up
# what the others find. Not libFuzzer -fork: that starts a new process every
# few seconds and each one runs the whole corpus first, which with a target
# this slow (a whole daemon life per input) leaves little time to fuzz.
# The targets run side by side, FUZZ_JOBS processes each: FUZZ_SECONDS is
# the time the whole run takes, not the time per target.
# -timeout: one input, the longest session included, takes well under a
# second; twenty mean omcid no longer comes back. -report_slow_units is
# above it, so the only files libFuzzer writes under artifacts are inputs
# that broke omcid: a slow-unit file is written for a run that passed, on a
# busy machine, and would fail this target with nothing behind it.
# What an earlier run left under artifacts is removed first, or one finding
# would fail every run after it, fixed or not.
fuzz: check-srcs seeds $(TARGETS:%=$(BUILD)/fuzz/fuzz_%)
	@mkdir -p $(BUILD)/artifacts
	@rm -f $(BUILD)/artifacts/*
	@rc=0; pids=; for t in $(TARGETS); do \
		mkdir -p $(BUILD)/corpus/$$t; \
		dict=; [ -f $$t.dict ] && dict=-dict=$$t.dict; \
		echo "== fuzz_$$t for $(FUZZ_SECONDS) s, $(FUZZ_JOBS) process(es)"; \
		j=1; while [ $$j -le $(FUZZ_JOBS) ]; do \
			ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:symbolize=1 \
			UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
			$(BUILD)/fuzz/fuzz_$$t -max_total_time=$(FUZZ_SECONDS) \
				-max_len=$(FUZZ_MAX_LEN) -timeout=20 -report_slow_units=25 \
				-rss_limit_mb=2048 $$dict -print_final_stats=1 \
				-artifact_prefix=$(BUILD)/artifacts/$$t- \
				$(BUILD)/corpus/$$t $(BUILD)/seeds/$$t \
				> $(BUILD)/fuzz-$$t-$$j.log 2>&1 & \
			pids="$$pids $$!"; j=$$((j + 1)); \
		done; \
	done; \
	for p in $$pids; do wait $$p || rc=1; done; \
	for t in $(TARGETS); do \
		echo "== fuzz_$$t"; \
		j=1; while [ $$j -le $(FUZZ_JOBS) ]; do \
			grep -E '^#[0-9]+[[:space:]]+DONE|^stat::(number_of_executed_units|average_exec_per_sec)' \
				$(BUILD)/fuzz-$$t-$$j.log | sed "s/^/    [$$j] /"; \
			j=$$((j + 1)); \
		done; \
		echo "    corpus: $$(ls $(BUILD)/corpus/$$t | wc -l) input(s)"; \
	done; \
	found=$$(ls $(BUILD)/artifacts 2>/dev/null); \
	if [ -n "$$found" ]; then \
		echo "FAIL  fuzz: inputs that broke omcid, in $(BUILD)/artifacts:"; \
		for a in $(BUILD)/artifacts/*; do \
			python3 -I seeds.py --hex $$a; \
			grep -l "$$(basename $$a)" $(BUILD)/fuzz-*.log | head -n 1 | \
				xargs -r grep -m 1 -A 12 -E 'ERROR: |runtime error|^fakeos: |^stick: '; \
		done; \
		exit 1; \
	fi; \
	[ $$rc = 0 ] || { \
		for l in $(BUILD)/fuzz-*.log; do echo "== $$l (last 60 lines)"; tail -n 60 $$l; done; \
		echo "FAIL  fuzz: a target stopped with an error, logs above"; exit 1; }; \
	echo "ok    fuzz: $(TARGETS), $(FUZZ_SECONDS) s, nothing found"

# The working corpus cut down to the inputs that add coverage (libFuzzer
# -merge). A corpus only grows while it
# is fuzzed, and every run starts by running all of it; CI trims it before
# it goes back into the cache. The trimmed set is built beside the corpus
# and takes its place only when the merge succeeded.
minimize: $(TARGETS:%=$(BUILD)/fuzz/fuzz_%)
	@for t in $(TARGETS); do \
		[ -d $(BUILD)/corpus/$$t ] || continue; \
		was=$$(ls $(BUILD)/corpus/$$t | wc -l); \
		rm -rf $(BUILD)/corpus-min; \
		mkdir -p $(BUILD)/corpus-min; \
		$(BUILD)/fuzz/fuzz_$$t -merge=1 -max_len=$(FUZZ_MAX_LEN) \
			$(BUILD)/corpus-min $(BUILD)/corpus/$$t \
			> $(BUILD)/minimize-$$t.log 2>&1 || { \
			tail -n 20 $(BUILD)/minimize-$$t.log; \
			echo "FAIL  fuzz minimize: $$t"; exit 1; }; \
		mv $(BUILD)/corpus/$$t $(BUILD)/corpus-old; \
		mv $(BUILD)/corpus-min $(BUILD)/corpus/$$t; \
		rm -rf $(BUILD)/corpus-old; \
		echo "ok    fuzz minimize: $$t, $$was -> $$(ls $(BUILD)/corpus/$$t | wc -l) input(s)"; \
	done

clean:
	rm -rf $(BUILD)
