# omcicap — phase 0 of the omci_app replacement.
#
# Reuses diag's freestanding runtime: the same _start, the same syscall
# wrappers, the same output layer. Only main.c is new, so the two stay in step
# rather than drifting into two copies of the same o32 plumbing.

IMAGE := odi-diag-toolchain
CROSS := mips-linux-gnu-
BUILD := build
BIN   := $(BUILD)/omcicap
DIAG  := ../../diag

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

OBJS := $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/main.o $(BUILD)/omci_mib.o $(BUILD)/omci_mib.o

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	$(STRIP) $@

$(BUILD)/start.o: $(DIAG)/src/start.S | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/io.o: $(DIAG)/src/io.c $(DIAG)/src/io.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/main.o: main.c $(DIAG)/src/sys.h $(DIAG)/src/io.h ../generated/omci_mib.h ../redirect_guard.h ../procparse.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/omci_mib.o: ../generated/omci_mib.c ../generated/omci_mib.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

else

RUN := docker run --rm --user $$(id -u):$$(id -g) -e HOME=/tmp -v $(CURDIR)/../../..:/src -w /src/src/omci/capture $(IMAGE)

.PHONY: all image cap run size clean
all: cap

image:
	@docker image inspect $(IMAGE) >/dev/null 2>&1 || \
	  docker build -t $(IMAGE) $(CURDIR)/$(DIAG)

cap: image
	$(RUN) make IN_CONTAINER=1 $(BIN)
	@ls -l $(BIN)

run: cap
	@$(RUN) qemu-mips-static $(BIN) $(ARGS)

size: cap
	@$(RUN) sh -c 'ls -l $(BIN); $(CROSS)size $(BIN)'

clean:
	rm -rf $(BUILD)

endif
