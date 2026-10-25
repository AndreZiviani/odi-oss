# igmpd — IGMP snooping, milestone 1: the receive path.
#
# Same arrangement as omci/capture: diag owns the freestanding runtime (_start,
# the syscall wrappers, the output layer) and only what is new lives here, so
# the two do not drift into two copies of the same o32 plumbing.

# The freestanding toolchain image, pinned by digest in toolchain/images.env
# and pulled on first use; DIAG_IMAGE overrides it (toolchain/image.sh).
TOP := $(CURDIR)/../..
-include $(TOP)/toolchain/images.env
DIAG_IMAGE ?= $(DIAG_IMAGE_PINNED)
IMAGE := $(DIAG_IMAGE)
CROSS := mips-linux-gnu-
BUILD := build
BIN   := $(BUILD)/igmpd
TEST  := $(BUILD)/frame_test
GTEST := $(BUILD)/group_test
PTEST := $(BUILD)/policy_test
ETEST := $(BUILD)/entry_test
UTEST := $(BUILD)/uplink_test
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

# switch_opt.c holds the /dev/odi_sw ioctls this daemon programs the switch
# with; its header lists them. uplink.c is the way up for a taken report.
OBJS := $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/frame.o $(BUILD)/group.o \
        $(BUILD)/policy.o $(BUILD)/state.o $(BUILD)/status.o $(BUILD)/entry.o $(BUILD)/program.o $(BUILD)/switch_opt.o \
        $(BUILD)/uplink.o $(BUILD)/replay.o $(BUILD)/main.o

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	$(STRIP) $@

$(TEST): test/frame_test.c igmpd.h $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/frame.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ test/frame_test.c \
	      $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/frame.o

$(GTEST): test/group_test.c group.h policy.h igmpd.h $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/group.o $(BUILD)/policy.o $(BUILD)/entry.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ test/group_test.c \
	      $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/group.o $(BUILD)/policy.o $(BUILD)/entry.o

$(PTEST): test/policy_test.c group.h policy.h state.h entry.h igmpd.h $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/group.o $(BUILD)/policy.o $(BUILD)/state.o $(BUILD)/entry.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ test/policy_test.c \
	      $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/group.o $(BUILD)/policy.o $(BUILD)/state.o $(BUILD)/entry.o

$(UTEST): test/uplink_test.c uplink.h policy.h group.h igmpd.h $(DIAG)/src/odi_sw_ioctl.h $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/uplink.o $(BUILD)/policy.o $(BUILD)/group.o $(BUILD)/frame.o $(BUILD)/entry.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ test/uplink_test.c \
	      $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/uplink.o $(BUILD)/policy.o $(BUILD)/group.o $(BUILD)/frame.o $(BUILD)/entry.o

$(ETEST): test/entry_test.c entry.h $(DIAG)/src/odi_sw_ioctl.h $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/entry.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ test/entry_test.c \
	      $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/entry.o

$(BUILD)/start.o: $(DIAG)/src/start.S | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/io.o: $(DIAG)/src/io.c $(DIAG)/src/io.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/frame.o: frame.c igmpd.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/group.o: group.c group.h policy.h igmpd.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/state.o: state.c state.h group.h policy.h igmpd.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/status.o: status.c status.h state.h group.h policy.h $(DIAG)/src/odi_sw_ioctl.h $(DIAG)/src/sys.h $(DIAG)/src/io.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/policy.o: policy.c policy.h entry.h $(DIAG)/src/odi_sw_ioctl.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/entry.o: entry.c entry.h $(DIAG)/src/odi_sw_ioctl.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/program.o: program.c program.h entry.h switch_opt.h $(DIAG)/src/odi_sw_ioctl.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/switch_opt.o: switch_opt.c switch_opt.h $(DIAG)/src/odi_sw_ioctl.h $(DIAG)/src/sys.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/uplink.o: uplink.c uplink.h policy.h igmpd.h $(DIAG)/src/odi_sw_ioctl.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/replay.o: replay.c replay.h igmpd.h group.h policy.h entry.h $(DIAG)/src/sys.h $(DIAG)/src/io.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/main.o: main.c uplink.h status.h replay.h igmpd.h group.h policy.h state.h program.h entry.h ../omci/nl.h ../omci/redirect_guard.h ../omci/procparse.h $(DIAG)/src/sys.h $(DIAG)/src/io.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

else

RUN := docker run --rm --user $$(id -u):$$(id -g) -e HOME=/tmp -v $(CURDIR)/../..:/src -w /src/src/igmp $(IMAGE)

.PHONY: all image igmpd test size clean
all: igmpd

image:
	@DIAG_IMAGE='$(IMAGE)' $(TOP)/toolchain/image.sh diag >/dev/null

igmpd: image
	$(RUN) make IN_CONTAINER=1 $(BIN)
	@ls -l $(BIN)

# The parse is pure arithmetic over a buffer, so the whole of it runs under
# qemu with no device and no netlink. That is the point of keeping frame.c
# free of syscalls.
test: image
	$(RUN) make IN_CONTAINER=1 $(TEST) $(GTEST) $(ETEST) $(PTEST) $(UTEST) $(BIN)
	@$(RUN) qemu-mips-static $(TEST)
	@$(RUN) qemu-mips-static $(GTEST)
	@$(RUN) qemu-mips-static $(ETEST)
	@$(RUN) qemu-mips-static $(PTEST)
	@$(RUN) qemu-mips-static $(UTEST)
	@$(RUN) sh test/replay_test.sh

size: igmpd
	@$(RUN) sh -c 'ls -l $(BIN); $(CROSS)size $(BIN)'

clean:
	rm -rf $(BUILD)

endif
