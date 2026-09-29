# nv — the /bin/nv replacement, without libmib.
#
# Same freestanding runtime as diag, omcid and omcli: one _start, one syscall
# layer, one output layer.

# The freestanding toolchain image, pinned by digest in toolchain/images.env
# and pulled on first use; DIAG_IMAGE overrides it (toolchain/image.sh).
TOP := $(CURDIR)/../..
-include $(TOP)/toolchain/images.env
DIAG_IMAGE ?= $(DIAG_IMAGE_PINNED)
IMAGE := $(DIAG_IMAGE)
CROSS := mips-linux-gnu-
BUILD := build
BIN   := $(BUILD)/nv
DIAG  := ../diag

ifeq ($(IN_CONTAINER),1)

CC    := $(CROSS)gcc
STRIP := $(CROSS)strip

CFLAGS := -std=c11 -Os -Wall -Wextra \
          -march=mips1 -mabi=32 -EB -msoft-float -G0 \
          -fno-pic -mno-abicalls -ffreestanding -fno-builtin -fno-stack-protector \
          -ffunction-sections -fdata-sections \
          -I$(DIAG)/src -I.
LDFLAGS := -nostdlib -nostartfiles -static -Wl,-e,_start -Wl,--build-id=none \
           -Wl,--gc-sections

OBJS := $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/env.o $(BUILD)/commit.o $(BUILD)/main.o

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	$(STRIP) $@

$(BUILD)/start.o: $(DIAG)/src/start.S | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/io.o: $(DIAG)/src/io.c $(DIAG)/src/io.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/env.o: env.c env.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/commit.o: commit.c commit.h env.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/main.o: main.c env.h commit.h $(DIAG)/src/sys.h $(DIAG)/src/io.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

else

RUN := docker run --rm --user $$(id -u):$$(id -g) -e HOME=/tmp -v $(CURDIR)/../..:/src -w /src/src/nv $(IMAGE)

.PHONY: all image nv size test clean
all: nv

image:
	@DIAG_IMAGE='$(IMAGE)' $(TOP)/toolchain/image.sh diag >/dev/null

nv: image
	$(RUN) make IN_CONTAINER=1 $(BIN)
	@ls -l $(BIN)

size: nv
	@$(RUN) sh -c 'ls -l $(BIN); $(CROSS)size $(BIN)'

# The block logic and the commit sequence have no syscalls in them, so they
# are tested on the host: the block against one built to the device layout,
# `nv commit` against a fake pair of partitions that can fail a write or a
# read-back at any step (test/test_commit.c). The partition I/O itself
# (MEMERASE, write(2) on /dev/mtdN) is not covered -- see the header of main.c.
test:
	@mkdir -p $(BUILD)
	@cc -std=c11 -Wall -Wextra -Werror -o $(BUILD)/test_env test/test_env.c env.c && \
	 $(BUILD)/test_env
	@cc -std=c11 -Wall -Wextra -Werror -o $(BUILD)/test_commit test/test_commit.c commit.c env.c && \
	 $(BUILD)/test_commit

clean:
	rm -rf $(BUILD)

endif
