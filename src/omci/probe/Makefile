# omciprobe — phase 0 of the omci_app replacement.
#
# Reuses diag's freestanding runtime: the same _start, the same syscall
# wrappers, the same output layer. Only main.c is new, so the two stay in step
# rather than drifting into two copies of the same o32 plumbing.

IMAGE := odi-diag-toolchain
CROSS := mips-linux-gnu-
BUILD := build
BIN   := $(BUILD)/omciprobe
DIAG  := ../../diag

VENDOR_LIBOMCI ?= $(HOME)/tmp/odi/rootfs/lib/libomci_mib.so

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
	@docker image inspect $(IMAGE) >/dev/null 2>&1 || \
	  docker build -t $(IMAGE) $(CURDIR)/$(DIAG)

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
