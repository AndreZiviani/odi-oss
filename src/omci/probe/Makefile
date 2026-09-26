# omciprobe — phase 0 of the omci_app replacement.
#
# Reuses diag's freestanding runtime: the same _start, the same syscall
# wrappers, the same output layer. Only main.c is new, so the two stay in step
# rather than drifting into two copies of the same o32 plumbing.

# The freestanding toolchain image, pinned by digest in toolchain/images.env
# and pulled on first use; DIAG_IMAGE overrides it (toolchain/image.sh).
TOP := $(CURDIR)/../../..
-include $(TOP)/toolchain/images.env
DIAG_IMAGE ?= $(DIAG_IMAGE_PINNED)
IMAGE := $(DIAG_IMAGE)
CROSS := mips-linux-gnu-
BUILD := build
BIN   := $(BUILD)/omciprobe
DIAG  := ../../diag

STOCK_ROOTFS ?= stock-rootfs
VENDOR_LIBOMCI ?= $(STOCK_ROOTFS)/lib/libomci_mib.so

ifeq ($(IN_CONTAINER),1)

CC    := $(CROSS)gcc
STRIP := $(CROSS)strip

CFLAGS := -std=c11 -Os -Wall -Wextra \
          -march=mips1 -mabi=32 -EB -msoft-float -G0 \
          -fno-pic -mno-abicalls -ffreestanding -fno-builtin -fno-stack-protector \
          -ffunction-sections -fdata-sections \
          -I$(DIAG)/src -I../generated -I..
LDFLAGS := -nostdlib -nostartfiles -static -Wl,-e,_start -Wl,--build-id=none \
           -Wl,--gc-sections

OBJS := $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/main.o

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	$(STRIP) $@

$(BUILD)/start.o: $(DIAG)/src/start.S | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/io.o: $(DIAG)/src/io.c $(DIAG)/src/io.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/main.o: main.c $(DIAG)/src/sys.h $(DIAG)/src/io.h ../generated/omci_drv_cmds.h ../nl.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

else

RUN := docker run --rm --user $$(id -u):$$(id -g) -e HOME=/tmp -v $(CURDIR)/../../..:/src -w /src/src/omci/probe $(IMAGE)

.PHONY: all image probe generate run size clean
all: probe

image:
	@DIAG_IMAGE='$(IMAGE)' $(TOP)/toolchain/image.sh diag >/dev/null

probe: image
	$(RUN) make IN_CONTAINER=1 $(BIN)
	@ls -l $(BIN)

# Re-lift the command table from the vendor library. Committed, so a normal
# build does not need the image.
generate:
	python3 ../tools/omci-drv-api.py "$(VENDOR_LIBOMCI)" --header

run: probe
	@$(RUN) qemu-mips-static $(BIN) $(ARGS)

size: probe
	@$(RUN) sh -c 'ls -l $(BIN); $(CROSS)size $(BIN)'

clean:
	rm -rf $(BUILD)

endif
