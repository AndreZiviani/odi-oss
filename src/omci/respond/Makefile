# omcid — phase 0 of the omci_app replacement.
#
# Reuses diag's freestanding runtime: the same _start, the same syscall
# wrappers, the same output layer. Only main.c is new, so the two stay in step
# rather than drifting into two copies of the same o32 plumbing.

IMAGE := odi-diag-toolchain
CROSS := mips-linux-gnu-
BUILD := build
BIN   := $(BUILD)/omcid
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

# omcid's own translation units. Split out of one 3122-line main.c: the
# division is by what a piece talks to, not by what it is about -- the store,
# the driver, the renderers, and one file per transport. omcid.h carries
# everything that crosses.
SRCS := main mibstore apply show clisrv vqsrv omcimsg cfgstore

OBJS := $(BUILD)/start.o $(BUILD)/io.o $(SRCS:%=$(BUILD)/%.o) \
        $(BUILD)/extvlan.o $(BUILD)/omci_mib.o \
        $(BUILD)/omci_autonomous.o $(BUILD)/omci_defaults.o $(BUILD)/omci_drv.o $(BUILD)/omci_bdgconn.o

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
	$(STRIP) $@

$(BUILD)/start.o: $(DIAG)/src/start.S | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/io.o: $(DIAG)/src/io.c $(DIAG)/src/io.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

OMCID_HDRS := omcid.h $(DIAG)/src/sys.h $(DIAG)/src/io.h \
              ../generated/omci_mib.h ../nl.h ../omci.h ../omci_gemflow.h \
              ../omci_caps.h ../omci_bdgconn.h ../omci_cli_proto.h \
              ../omci_msgq.h ../omci_tmpfile.h ../extvlan.h \
              ../procparse.h ../redirect_guard.h

$(BUILD)/%.o: %.c $(OMCID_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/extvlan.o: ../extvlan.c ../extvlan.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# The class 171 entry decode, on its own. No message queue, no store, no
# device -- which is why it is a separate binary rather than another case in
# qemu-test.sh.
$(BUILD)/extvlan_test: ../test/extvlan_test.c ../extvlan.h \
                       $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/extvlan.o
	$(CC) $(CFLAGS) $(LDFLAGS) -I.. -o $@ ../test/extvlan_test.c \
	      $(BUILD)/start.o $(BUILD)/io.o $(BUILD)/extvlan.o

$(BUILD)/omci_mib.o: ../generated/omci_mib.c ../generated/omci_mib.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/omci_autonomous.o: ../generated/omci_autonomous.c \
                           ../generated/omci_autonomous.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/omci_defaults.o: ../generated/omci_defaults.c \
                           ../generated/omci_defaults.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/omci_drv.o: ../generated/omci_drv.c ../generated/omci_drv.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/omci_bdgconn.o: ../generated/omci_bdgconn.c ../omci_bdgconn.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

else

RUN := docker run --rm --user $$(id -u):$$(id -g) -e HOME=/tmp -v $(CURDIR)/../../..:/src -w /src/src/omci/respond $(IMAGE)

.PHONY: all image cap run size test clean
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

test: image
	$(RUN) make IN_CONTAINER=1 $(BUILD)/extvlan_test
	@$(RUN) qemu-mips-static $(BUILD)/extvlan_test

clean:
	rm -rf $(BUILD)

endif

# Exercise the responder with no stick: omcid under qemu, serving its message
# queues with no line side, checked against output captured from a real one.
# See ../qemu-test.sh for why netlink cannot work there.
# `cap`, not $(BIN): outside the container there is no rule to build $(BIN), so
# depending on the file made an existing one look up to date and the tests ran
# against a stale binary. That is a silent pass, which is the worst kind.
.PHONY: qemu-test
qemu-test: cap
	$(MAKE) -C ../cli
	docker run --rm --user $$(id -u):$$(id -g) -e HOME=/tmp -v $(CURDIR)/../../..:/src -w /src/src/omci \
	        $(IMAGE) sh qemu-test.sh
