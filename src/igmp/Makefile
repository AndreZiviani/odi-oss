# igmpd — IGMP snooping, milestone 1: the receive path.
#
# Same arrangement as omci/capture: diag owns the freestanding runtime (_start,
# the syscall wrappers, the output layer) and only what is new lives here, so
# the two do not drift into two copies of the same o32 plumbing.

IMAGE := odi-diag-toolchain
CROSS := mips-linux-gnu-
BUILD := build
BIN   := $(BUILD)/igmpd
TEST  := $(BUILD)/frame_test
GTEST := $(BUILD)/group_test
ETEST := $(BUILD)/entry_test
DIAG  := ../diag

ifeq ($(IN_CONTAINER),1)

CC    := $(CROSS)gcc
STRIP := $(CROSS)strip

CFLAGS := -std=c11 -Os -Wall -Wextra \
          -march=mips1 -mabi=32 -EB -msoft-float -G0 \
          -fno-pic -mno-abicalls -ffreestanding -fno-builtin -fno-stack-protector \
          -ffunction-sections -fdata-sections \
          -I. -I$(DIAG)/src
LDFLAGS := -nostdlib -nostartfiles -static -Wl,-e,_start -Wl,--build-id=none \
           -Wl,--gc-sections

# switch_opt.c holds the three /dev/odi_sw ioctls this daemon programs the
# switch with; its header lists them.
OBJS := $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/frame.o $(BUILD)/group.o \
        $(BUILD)/entry.o $(BUILD)/program.o $(BUILD)/switch_opt.o \
        $(BUILD)/main.o

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	$(STRIP) $@

$(TEST): test/frame_test.c igmpd.h $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/frame.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ test/frame_test.c \
	      $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/frame.o

$(GTEST): test/group_test.c group.h igmpd.h $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/group.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ test/group_test.c \
	      $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/group.o

$(ETEST): test/entry_test.c entry.h $(DIAG)/src/odi_sw_ioctl.h $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/entry.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ test/entry_test.c \
	      $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/entry.o

$(BUILD)/start.o: $(DIAG)/src/start.S | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/io.o: $(DIAG)/src/io.c $(DIAG)/src/io.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/frame.o: frame.c igmpd.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/group.o: group.c group.h igmpd.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/entry.o: entry.c entry.h $(DIAG)/src/odi_sw_ioctl.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/program.o: program.c program.h entry.h switch_opt.h $(DIAG)/src/odi_sw_ioctl.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/switch_opt.o: switch_opt.c switch_opt.h $(DIAG)/src/odi_sw_ioctl.h $(DIAG)/src/sys.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/main.o: main.c igmpd.h group.h program.h entry.h ../omci/nl.h ../omci/redirect_guard.h ../omci/procparse.h $(DIAG)/src/sys.h $(DIAG)/src/io.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

else

RUN := docker run --rm --user $$(id -u):$$(id -g) -e HOME=/tmp -v $(CURDIR)/../..:/src -w /src/src/igmp $(IMAGE)

.PHONY: all image igmpd test size clean
all: igmpd

image:
	@docker image inspect $(IMAGE) >/dev/null 2>&1 || \
	  docker build -t $(IMAGE) $(CURDIR)/$(DIAG)

igmpd: image
	$(RUN) make IN_CONTAINER=1 $(BIN)
	@ls -l $(BIN)

# The parse is pure arithmetic over a buffer, so the whole of it runs under
# qemu with no device and no netlink. That is the point of keeping frame.c
# free of syscalls.
test: image
	$(RUN) make IN_CONTAINER=1 $(TEST) $(GTEST) $(ETEST)
	@$(RUN) qemu-mips-static $(TEST)
	@$(RUN) qemu-mips-static $(GTEST)
	@$(RUN) qemu-mips-static $(ETEST)

size: igmpd
	@$(RUN) sh -c 'ls -l $(BIN); $(CROSS)size $(BIN)'

clean:
	rm -rf $(BUILD)

endif
