# diag -- the switch, GPON and optics CLI for our own kernel.
#
# A small hand-written command table (src/table.c) over /dev/odi_sw,
# /proc/odi_gpon and the odi_omci netlink command path. README.md has the
# command list and the exporter contract.
#
# Host targets shell out to the toolchain container; the C build re-enters this
# Makefile there with IN_CONTAINER=1.

# The freestanding toolchain image, pinned by digest in toolchain/images.env
# and pulled on first use; DIAG_IMAGE overrides it (toolchain/image.sh).
TOP := $(CURDIR)/../..
-include $(TOP)/toolchain/images.env
DIAG_IMAGE ?= $(DIAG_IMAGE_PINNED)
IMAGE := $(DIAG_IMAGE)
CROSS := mips-linux-gnu-
BUILD := build
BIN   := $(BUILD)/diag

# The netlink transport is omcid's (nl.h) and the flow readback layout the
# driver ABI (odi/uapi); diag reads the same headers rather than restating
# them.
OMCI := ../omci
UAPI := ../../kernel/extra/drivers/net/ethernet/odi/uapi

ifeq ($(IN_CONTAINER),1)

# ------------------------------------------------------------- in container

CC    := $(CROSS)gcc
STRIP := $(CROSS)strip

# -march=mips1     RLX5281 is MIPS-I class; mips2 is the documented ceiling
# -EB              big-endian, per CONFIG_CPU_BIG_ENDIAN in Realtek's config
# -msoft-float     RLX cores have no FPU
# -G0              no gp-relative small data -- _start never sets up $gp
# -fno-pic
# -mno-abicalls    plain static ELF, no GOT
# -ffreestanding   no libc, and no turning our loops back into libc calls
CFLAGS := -std=c11 -Os -Wall -Wextra \
          -march=mips1 -mabi=32 -EB -msoft-float -G0 \
          -fno-pic -mno-abicalls -ffreestanding -fno-builtin -fno-stack-protector \
          -fno-tree-loop-distribute-patterns \
          -ffunction-sections -fdata-sections \
          -Isrc -I$(OMCI) -I$(UAPI)

LDFLAGS := -nostdlib -nostartfiles -static -Wl,-e,_start -Wl,--build-id=none \
           -Wl,--gc-sections

# Everything but the hardware layer, shared by diag and diag_fake.
CORE := $(BUILD)/start.o $(BUILD)/main.o $(BUILD)/shell.o $(BUILD)/parse.o \
        $(BUILD)/table.o $(BUILD)/commands.o $(BUILD)/io.o $(BUILD)/ddm.o \
        $(BUILD)/gpon_status.o $(BUILD)/l2.o $(BUILD)/l2_fmt.o

$(BIN): $(CORE) $(BUILD)/hw.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	$(STRIP) $@

# The exporter contract test: the real binary, with the accessors the
# exporter's commands reach answered by test/hw_fake.c instead.
FAKE_WRAP := hw_transceiver_get hw_gpon_status_get hw_gpon_alarms_get \
             hw_stat_port_get hw_l2_mode hw_l2_get hw_l2_next

$(BUILD)/diag_fake: $(CORE) $(BUILD)/hw.o test/hw_fake.c | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $(foreach f,$(FAKE_WRAP),-Wl,--wrap=$(f)) \
	      -o $@ test/hw_fake.c $(CORE) $(BUILD)/hw.o

$(BUILD)/hw.o: src/hw.c src/hw.h src/odi_sw_ioctl.h src/gpon_status.h \
               $(OMCI)/nl.h $(UAPI)/omci_flows.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: src/%.c src/*.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: src/%.S | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/ddm_selftest: $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/ddm.o \
                       test/ddm_selftest.c | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ test/ddm_selftest.c \
	      $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/ddm.o

$(BUILD)/fmt_selftest: $(BUILD)/start.o $(BUILD)/io.o \
                       test/fmt_selftest.c test/fmt_cases.h | $(BUILD)
	$(CC) $(CFLAGS) -Itest $(LDFLAGS) -o $@ test/fmt_selftest.c \
	      $(BUILD)/start.o $(BUILD)/io.o

$(BUILD)/fmt_prop_selftest: $(BUILD)/start.o $(BUILD)/io.o \
                       test/fmt_prop_selftest.c test/fmt_prop.h | $(BUILD)
	$(CC) $(CFLAGS) -Itest $(LDFLAGS) -o $@ test/fmt_prop_selftest.c \
	      $(BUILD)/start.o $(BUILD)/io.o

$(BUILD):
	mkdir -p $(BUILD)

else

# ------------------------------------------------------------------- host

# The repository root is mounted, not only this directory: hw.c includes
# the netlink transport from ../omci.
RUN := docker run --rm --user $$(id -u):$$(id -g) -e HOME=/tmp -v $(CURDIR)/../..:/src -w /src/src/diag $(IMAGE)

.PHONY: all image diag test selftest exporter-test l2-test run verify size clean help

all: diag

image:
	@DIAG_IMAGE='$(IMAGE)' $(TOP)/toolchain/image.sh diag >/dev/null

diag: image
	$(RUN) make IN_CONTAINER=1 $(BIN)

# The parser and the pure conversions are target-independent, so these build
# and run natively. Only sys.h and start.S are MIPS-only.
test:
	@mkdir -p $(BUILD)
	cc -std=c11 -Wall -Wextra -Isrc -o $(BUILD)/parse_test \
	   test/parse_test.c src/parse.c src/table.c
	@$(BUILD)/parse_test < test/cases.txt > $(BUILD)/parse_test.out
	@diff -u test/cases.expected $(BUILD)/parse_test.out \
	  && echo "parser: $$(wc -l < test/cases.txt | tr -d ' ') cases match"
	cc -std=c11 -Wall -Wextra -Isrc -o $(BUILD)/ddm_test test/ddm_test.c src/ddm.c -lm
	@$(BUILD)/ddm_test
	cc -std=c11 -Wall -Wextra -Isrc -o $(BUILD)/gpon_status_test \
	   test/gpon_status_test.c src/gpon_status.c
	@$(BUILD)/gpon_status_test
	cc -std=c11 -Wall -Wextra -Isrc -o $(BUILD)/odi_sw_ioctl_test \
	   test/odi_sw_ioctl_test.c
	@$(BUILD)/odi_sw_ioctl_test
	cc -std=c11 -Wall -Wextra -Isrc -o $(BUILD)/l2_format_test \
	   test/l2_format_test.c src/l2_fmt.c
	@$(BUILD)/l2_format_test
	@python3 test/regnames_test.py

# The exporter contract: test/exporter.txt is the batch odi-sfp-exporter sends
# (its diag_secs table, then exit), run through the real binary under qemu
# with the hardware answers fixed, and diffed byte for byte against the
# output the pre-rework diag gave for the same answers. A second run drops
# the exit and ends on end of input instead, the other way a batch ends.
exporter-test: image
	$(RUN) make IN_CONTAINER=1 $(BUILD)/diag_fake
	@$(RUN) sh -c 'qemu-mips-static $(BUILD)/diag_fake < test/exporter.txt' \
	    > $(BUILD)/exporter.out
	@cmp $(BUILD)/exporter.out test/exporter.golden \
	  || { diff -u test/exporter.golden $(BUILD)/exporter.out; exit 1; }
	@grep -v '^exit$$' test/exporter.txt > $(BUILD)/exporter_eof.txt
	@$(RUN) sh -c 'qemu-mips-static $(BUILD)/diag_fake < $(BUILD)/exporter_eof.txt' \
	    > $(BUILD)/exporter_eof.out
	@cmp $(BUILD)/exporter_eof.out test/exporter_eof.golden \
	  || { diff -u test/exporter_eof.golden $(BUILD)/exporter_eof.out; exit 1; }
	@echo "exporter contract: output byte-identical to the golden files"

# The L2 table listing: test/l2.txt through the same fake-hardware binary,
# the table answered by test/hw_fake.c, diffed against test/l2.golden. The
# first two listings must be identical: one is our spelling, the other the
# stock one odi-ui sends.
l2-test: image
	$(RUN) make IN_CONTAINER=1 $(BUILD)/diag_fake
	@$(RUN) sh -c 'qemu-mips-static $(BUILD)/diag_fake < test/l2.txt' > $(BUILD)/l2.out
	@cmp $(BUILD)/l2.out test/l2.golden \
	  || { diff -u test/l2.golden $(BUILD)/l2.out; exit 1; }
	@echo "l2-table: output byte-identical to test/l2.golden"

# The conversion vectors, run on the target under qemu rather than natively.
# fmt_selftest goes further and diffs out_fmt against the host's printf over
# every conversion the handlers' format strings use.
# Seed for the randomised out_fmt cases. Fixed so a failure is reproducible;
# `make selftest SEED=$$RANDOM` explores instead.
SEED ?= 1

selftest: image exporter-test l2-test
	$(RUN) make IN_CONTAINER=1 $(BUILD)/ddm_selftest
	@$(RUN) qemu-mips-static $(BUILD)/ddm_selftest
	$(RUN) make IN_CONTAINER=1 $(BUILD)/fmt_selftest
	@cc -std=c11 -Wall -Itest -o $(BUILD)/fmt_ref test/fmt_ref.c
	@$(BUILD)/fmt_ref > $(BUILD)/fmt_ref.out
	@$(RUN) qemu-mips-static $(BUILD)/fmt_selftest > $(BUILD)/fmt_got.out
	@diff -u $(BUILD)/fmt_ref.out $(BUILD)/fmt_got.out \
	  && echo "out_fmt matches printf on all $$(wc -l < $(BUILD)/fmt_ref.out) cases"
	$(RUN) make IN_CONTAINER=1 $(BUILD)/fmt_prop_selftest
	@cc -std=c11 -Wall -Wno-format-nonliteral -Wno-format-security -Itest \
	    -o $(BUILD)/fmt_prop_ref test/fmt_prop_ref.c
	@$(BUILD)/fmt_prop_ref $(SEED) > $(BUILD)/fmt_prop_ref.out
	@$(RUN) qemu-mips-static $(BUILD)/fmt_prop_selftest $(SEED) \
	    > $(BUILD)/fmt_prop_got.out
	@diff -u $(BUILD)/fmt_prop_ref.out $(BUILD)/fmt_prop_got.out \
	  && echo "out_fmt matches printf on $$(wc -l < $(BUILD)/fmt_prop_ref.out) random cases (seed $(SEED))"

run: diag
	@$(RUN) qemu-mips-static $(BIN) $(ARGS)

verify: diag
	@$(RUN) sh -c 'file $(BIN); \
	  $(CROSS)readelf -h $(BIN) | grep -E "Class|Data|Machine|Type"'

size: diag
	@$(RUN) sh -c 'ls -l $(BIN); $(CROSS)size $(BIN)'

clean:
	rm -rf $(BUILD)

help:
	@echo "diag -- the switch, GPON and optics CLI"
	@echo
	@echo "  make diag           build the freestanding MIPS binary"
	@echo "  make test           parser and conversion tests, natively (no container)"
	@echo "  make exporter-test  the exporter contract, under qemu"
	@echo "  make l2-test        the l2-table listing against its golden file, under qemu"
	@echo "  make selftest       exporter-test plus the conversion vectors, under qemu"
	@echo "  make run ARGS='pon get transceiver rx-power'"
	@echo "  make verify         confirm ELF32 / big-endian / MIPS / static"
	@echo "  make size           section sizes"

endif
