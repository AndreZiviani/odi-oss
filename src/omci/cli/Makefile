# omcli — the omcicli replacement.
#
# Same freestanding runtime as diag and omcid: one _start, one syscall layer,
# one output layer, so the three stay in step rather than drifting into three
# copies of the same o32 plumbing.

# The freestanding toolchain image, pinned by digest in toolchain/images.env
# and pulled on first use; DIAG_IMAGE overrides it (toolchain/image.sh).
TOP := $(CURDIR)/../../..
-include $(TOP)/toolchain/images.env
DIAG_IMAGE ?= $(DIAG_IMAGE_PINNED)
IMAGE := $(DIAG_IMAGE)
CROSS := mips-linux-gnu-
BUILD := build
BIN   := $(BUILD)/omcli
DIAG  := ../../diag

ifeq ($(IN_CONTAINER),1)

CC    := $(CROSS)gcc
STRIP := $(CROSS)strip

CFLAGS := -std=c11 -Os -Wall -Wextra \
          -march=mips1 -mabi=32 -EB -msoft-float -G0 \
          -fno-pic -mno-abicalls -ffreestanding -fno-builtin -fno-stack-protector \
          -ffunction-sections -fdata-sections \
          -I$(DIAG)/src -I../generated
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

$(BUILD)/main.o: main.c $(DIAG)/src/sys.h $(DIAG)/src/io.h ../omci_msgq.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

else

RUN := docker run --rm --user $$(id -u):$$(id -g) -e HOME=/tmp -v $(CURDIR)/../../..:/src -w /src/src/omci/cli $(IMAGE)

.PHONY: all image cli run size clean
all: cli

image:
	@DIAG_IMAGE='$(IMAGE)' $(TOP)/toolchain/image.sh diag >/dev/null

cli: image
	$(RUN) make IN_CONTAINER=1 $(BIN)
	@ls -l $(BIN)

run: cli
	@$(RUN) qemu-mips-static $(BIN) $(ARGS)

size: cli
	@$(RUN) sh -c 'ls -l $(BIN); $(CROSS)size $(BIN)'

clean:
	rm -rf $(BUILD)

endif
