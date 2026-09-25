# nv — the /bin/nv replacement, without libmib.
#
# Same freestanding runtime as diag, omcid and omcli: one _start, one syscall
# layer, one output layer.

IMAGE := odi-diag-toolchain
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

OBJS := $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/env.o $(BUILD)/main.o

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	$(STRIP) $@

$(BUILD)/start.o: $(DIAG)/src/start.S | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/io.o: $(DIAG)/src/io.c $(DIAG)/src/io.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/env.o: env.c env.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/main.o: main.c env.h $(DIAG)/src/sys.h $(DIAG)/src/io.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

else

RUN := docker run --rm -v $(CURDIR)/../..:/src -w /src/src/nv $(IMAGE)

.PHONY: all image nv size test clean
all: nv

image:
	@docker image inspect $(IMAGE) >/dev/null 2>&1 || \
	  docker build -t $(IMAGE) $(CURDIR)/$(DIAG)

nv: image
	$(RUN) make IN_CONTAINER=1 $(BIN)
	@ls -l $(BIN)

size: nv
	@$(RUN) sh -c 'ls -l $(BIN); $(CROSS)size $(BIN)'

# The block logic has no syscalls in it, so it is tested on the host against a
# block built to the device's layout. The WRITE path is not covered -- see the
# header of main.c.
test:
	@cc -std=c11 -Wall -Wextra -o $(BUILD)/test_env test/test_env.c env.c && \
	 $(BUILD)/test_env

clean:
	rm -rf $(BUILD)

endif
