# odi-oss — firmware for the ODI DFP-34X-2C2, built from source.
#
# Read README.md and docs/FLASHING.md before expecting a working image.

.PHONY: help kernel-tree toolchain toolchain-audit kernel packages busybox \
	src releases image image-all clean distclean lint test test-host test-omci \
	test-diag test-rcs test-qemu

help:
	@echo "targets:"
	@echo "  kernel-tree     fetch Linux 6.18 and pack it for the kernel build"
	@echo "  toolchain       pull the pinned toolchain images (toolchain/images.env)"
	@echo "  toolchain-audit  prove the target libraries carry no illegal instruction"
	@echo "  kernel          Linux 6.18 for the RTL9602C (kernel/build.sh)"
	@echo "  busybox         upstream busybox, ISA-audited"
	@echo "  packages        every userland package"
	@echo "  src             our own binaries, freestanding, into out/bin"
	@echo "  releases        metricsd and confd from their own releases"
	@echo "  image           assemble a flashable tarball into out/image/"
	@echo "  image-all       everything above from a clean clone, in order (about 25 min native)"
	@echo "  test            lint + the host-side tests + diag + the OMCI daemon under qemu (~2 min)"
	@echo "  lint            shellcheck every script here"
	@echo "  test-rcs        the rcS action trace against its goldens (needs out/busybox, docker)"
	@echo "  test-qemu       boot the real rootfs under qemu-system-mips, ssh/web/exporter + resilience scenarios"
	@echo
	@echo "Nothing here flashes anything. See docs/FLASHING.md."
	@echo "An image is INERT when flashed; nv setenv sw_tryactive <slot> boots it once."

# The pristine kernel tree, fetched and packed into build/kernel-618/tree.tar.
# The kernel build reaches into it directly (kernel/tree.sh, kernel/build.sh);
# the toolchain is prebuilt now and no longer needs it.
kernel-tree:
	./kernel/tree.sh

# Both toolchains are prebuilt container images from the odi-toolchain
# repository, pinned by digest in toolchain/images.env: gcc 16.2.0, binutils
# 2.47 and uClibc-ng 1.0.59 for the kernel and packages, and Debian
# gcc-mips-linux-gnu for src/. This pulls them; every build step pulls what
# it needs on its own too. toolchain/README.md has how to build them from
# source instead (OSS_IMAGE=, DIAG_IMAGE=).
include toolchain/images.env
OSS_IMAGE ?= $(OSS_IMAGE_PINNED)
DIAG_IMAGE ?= $(DIAG_IMAGE_PINNED)
QEMU_KERNEL_IMAGE ?= $(QEMU_KERNEL_IMAGE_PINNED)
export OSS_IMAGE DIAG_IMAGE QEMU_KERNEL_IMAGE

toolchain:
	./toolchain/image.sh oss
	./toolchain/image.sh diag

# The image build already refused any trapping instruction in libc.a and
# libgcc.a; this runs the same audit again against the image in use.
toolchain-audit: toolchain
	docker run --rm "$(OSS_IMAGE)" odi-audit-libs

# The kernel is built with the toolchain above (our binutils carries the three
# Lexra opcodes it needs): Linux 6.18, pristine tree fetched by
# kernel/618/fetch.sh into kernel/618/mainline, patched from kernel/618/patches,
# config seeded from kernel/618/config. kernel/README.md has the rest.
kernel: toolchain
	./kernel/build.sh

busybox:
	./packages/busybox/build.sh

packages: busybox
	./packages/dropbear/build.sh
	./packages/iproute2/build.sh

src:
	./src/build.sh

releases:
	./src/fetch-releases.sh

image: src
	./image/build.sh

# From a clean clone. Each step is its own target above and skips work it has
# already done (docker layers, pinned tarballs, the .built-with stamps). One
# sub-make per step rather than a prerequisite list, so the order is the one
# written here even under make -j: the kernel tree before the toolchain, the
# toolchain before anything compiled with it.
image-all:
	$(MAKE) kernel-tree
	$(MAKE) toolchain
	$(MAKE) toolchain-audit
	$(MAKE) kernel
	$(MAKE) packages
	$(MAKE) src
	$(MAKE) releases
	$(MAKE) image

# What runs without a stick. test-host needs only bash and the repo (the flash
# accessor against a sample store, the fwu.sh guards against captured /proc
# files); test-omci builds our binaries and drives omcid under qemu-user in the
# diag toolchain container (src/omci/qemu-test.sh, about 130 checks). The
# QEMU system harnesses under test/ that need a staged rootfs or a built kernel
# stay manual; each says so in its header.
test: lint test-host test-diag test-omci
	@echo "ok"

test-host:
	bash test/root_pw_test.sh
	bash test/flash_test.sh
	bash test/slot_state_test.sh
	$(MAKE) -C src/nv test
	bash test/fwu_guard_test.sh
	bash test/fwu_starter_test.sh
	bash test/network_addr_test.sh
	bash test/apply_test.sh
	bash test/omci_respawn_test.sh
	bash test/regtrace_decode_test.sh
	bash test/regtrace_compare_test.sh
	bash test/regtrace_compare_stream_test.sh
	bash test/odi_nic_hw_test.sh
	bash test/odi_switch_test.sh
	bash test/odi_switch_mmio_bounds_test.sh
	bash test/odi_switch_tbl_desc_test.sh
	bash test/odi_switch_l2_test.sh
	bash test/odi_switch_dal_test.sh
	bash test/odi_reg_test.sh
	bash test/odi_switch_cmd_test.sh
	bash test/odi_switch_isp2_test.sh
	bash test/odi_switch_init_platform_test.sh
	bash test/odi_omci_test.sh
	bash test/regdump_test.sh
	bash test/mkmodload_test.sh
	bash test/odi_switch_modload_replay_test.sh
	bash test/mksdkinit_test.sh
	bash test/odi_switch_sdkinit_replay_test.sh
	bash test/odi_replay_blob_test.sh
	bash test/odi_switch_ds_encrypt_test.sh
	bash test/omci_ageing_time_test.sh
	bash test/odi_gpon_test.sh
	bash test/odi_gpon_replay_test.sh
	bash test/odi_gpon_irq_test.sh
	bash test/odi_board_test.sh
	bash test/odi_i2c_test.sh
	bash test/odi_ddm_test.sh
	bash test/odi_optics_model_test.sh
	bash test/odi_init_test.sh
	bash test/odi_wdt_test.sh
	bash test/odi_ramlog_test.sh
	bash test/procparse_test.sh
	bash test/odi_switch_flows_test.sh
	bash test/boot_golden_test.sh
	python3 test/regnames_kernel_test.py

# diag's parser and conversion tests, natively, then under qemu in the diag
# toolchain container: the conversion vectors and the exporter contract (the
# batch metricsd sends, byte for byte against test/exporter.golden).
test-diag:
	$(MAKE) -C src/diag test selftest

# Runs as root in the container: the harness creates /var/config and other
# system paths inside it, and writes nothing into the mounted tree.
# drv-test.sh is the driver-call golden, against a test build of omcid.
# resume-test.sh replays that same golden across a kill -9 and respawn:
# the resumed instance's MIB and switch bookkeeping must come back
# identical, with zero driver calls.
test-omci: src
	$(MAKE) -C src/omci/respond drvtrace
	docker run --rm -v "$(CURDIR)":/src -w /src/src/omci "$(DIAG_IMAGE)" \
		sh -c 'sh qemu-test.sh && sh drv-test.sh && sh resume-test.sh'

# The rcS action trace: what rcS executes and writes under /proc, for
# three flag sets, against test/fixtures/rcs-trace-*.txt. Not part of `test`:
# it needs a built busybox and a container allowed to ptrace.
test-rcs:
	bash test/rcs_trace_test.sh

# Full-system: the real rootfs (busybox, inittab, services, dropbear,
# confd, metricsd, our sysctls and tmpfs caps) booted under
# qemu-system-mips on a STOCK malta kernel (odi-toolchain-qemu-kernel-malta,
# toolchain/images.env), never our own RTL9602C kernel -- see
# test/qemu/run-qemu.sh header and docs/HACKING.md for exactly what this
# does and does not cover. Needs busybox, packages, src and releases built
# first (out/busybox, out/*), and qemu-system-mips on PATH.
test-qemu:
	./toolchain/image.sh qemu-kernel >/dev/null
	./test/qemu/build-initramfs.sh
	./test/qemu/run-qemu.sh

lint:
	shellcheck -S warning toolchain/*.sh kernel/*.sh packages/*.sh packages/*/*.sh \
	          image/*.sh tools/*.sh src/*.sh test/*.sh test/qemu/*.sh
	@# The scripts that actually run ON the device, checked as POSIX sh
	@# because busybox ash is what interprets them -- not bash. These were
	@# outside the lint entirely until 2026-09-14, which is backwards: they
	@# are the only ones whose failure costs a stick rather than a build.
	@# tools/regdump/dump.sh joins them here for the same reason: it is
	@# pushed to and run on the stick, against a minimal busybox, not
	@# built or run on the host. test/qemu/diag-stub.sh and memhog.sh are
	@# the same shape one step removed: they run under busybox ash INSIDE
	@# the qemu guest, never on the build host.
	shellcheck -S warning -s sh rootfs/skeleton/etc/init.d/* rootfs/skeleton/etc/scripts/*.sh rootfs/skeleton/etc/scripts/flash tools/regdump/dump.sh test/qemu/diag-stub.sh test/qemu/memhog.sh
	@echo "shellcheck: clean"
	@./tools/check-inline-quotes.sh toolchain/*.sh kernel/*.sh packages/*.sh \
	          packages/*/*.sh image/*.sh src/*.sh tools/*.sh test/*.sh test/qemu/*.sh
	@# This repo is public; the private investigation workspace it was
	@# developed alongside is not, and none of its paths or document names
	@# may leak into tracked files here. Excludes this Makefile itself,
	@# since the pattern below necessarily contains the strings it looks for.
	@if git grep -nE 'investigations/|odi-sfp-re|SPEC-(NIC|GPON)|NIC-ABI|PLAN-[A-Z]|INVENTORY-|ANALYSIS-|RESEARCH-|REVIEW-|NOTES\.md|KB-DRAFTS|vendor-src/|regtrace/(omci|gpon|sdkinit|nic)/|kb/systems/|kb/decisions/|kernel/patches/|kernel/vendor/|~/git/' -- ':!Makefile'; then \
		echo "lint: found a reference to the private investigation workspace above -- restate the point in this repo own words instead" >&2; \
		exit 1; \
	fi
	@echo "no private-workspace references: clean"
	@# Paths that exist on one machine only, or in private material: a home
	@# directory in any spelling, /root/ outside the documented remote
	@# build directory and the container paths of the test harnesses, the
	@# backups/ of a stick and the private stick inventory. A script that
	@# needs such a path takes it from a variable (STOCK_ROOTFS, ...).
	@if git grep -nE '~/[A-Za-z]|\$$\{?HOME\}?/|\$$\(HOME\)/|/Users/|/home/[a-z]|/root/|backups/|OUR-STICKS' -- ':!Makefile' | \
		grep -vE '/root/(odi/odi-oss|fs|stick|p[0-9]+\.img)([^A-Za-z0-9_-]|$$)|/root/fs-'; then \
		echo "lint: found a machine-local or private path above -- take it from a variable, or describe it instead" >&2; \
		exit 1; \
	fi
	@echo "no machine-local or private paths: clean"
	@# Trial history in code and scripts: trial, image and boot ids, dates,
	@# old patch numbers, "this pass". Git history is the record; a comment
	@# says what is true of the code now (docs/HACKING.md, "Repo rules").
	@# Test fixtures are exempt, their names carry dates on purpose.
	@if git grep -n -i -E 'this pass|coordinator|trial (618|[a-z]{1,2}[0-9])|(^|[^a-z0-9_.-])618[a-z]{1,2}[0-9]*([^a-z0-9_]|$$)|(^|[^a-z0-9_])[a-z][0-9]{1,2} (trial|fixes|fix)([^a-z]|$$)|20[0-9][0-9]-[01][0-9]-[0-3][0-9]|0004(.s| own| did)|(^|[^a-z0-9_])boots? [0-9]+(-[0-9]+)?,? 20[0-9][0-9]' \
		-- kernel/extra rootfs/skeleton src ':!src/omci/test' ':!src/omci/generated' ':!*.golden' ':!*fixtures*'; then \
		echo "lint: trial history in a comment above -- say what is true now; git history keeps the story" >&2; \
		exit 1; \
	fi
	@echo "no trial history in code: clean"
	@# Internal plan/task IDs (R1.1, D3, M9, U1, G-trace, T2, "batch A"):
	@# a goldens name is described by what it is, not by its gate name; a
	@# milestone by what it does, not by a trial image name. CHANGELOG.md
	@# is the one place a release name belongs.
	@if git grep -n -I -E '(^|[^A-Za-z0-9_.])R[1-4]\.[0-9]+([^A-Za-z0-9_]|$$)|(^|[^A-Za-z0-9_.])D[1-9]([^A-Za-z0-9_]|$$)|(^|[^A-Za-z0-9_.])M(1[0-3]|[1-9])([^A-Za-z0-9_]|$$)|(^|[^A-Za-z0-9_.])U1([^A-Za-z0-9_]|$$)|G-(trace|drv|rcs|host|ident|size|hw)|(^|[^A-Za-z0-9_.])T[1-4]([^A-Za-z0-9_]|$$)|\bbatch [AB]\b|since (the )?(step )?(R[1-4]\.[0-9]+|D[1-9]|M[1-9][0-3]?|U1|T[1-4])\b' \
		-- kernel/extra rootfs/skeleton src tools test docs '*.md' \
		':!Makefile' ':!CHANGELOG.md' ':!src/omci/test' ':!src/omci/generated' \
		':!*.golden' ':!*fixtures*'; then \
		echo "lint: an internal plan/task id in a comment above -- describe what the thing is, not the step that made it" >&2; \
		exit 1; \
	fi
	@echo "no internal plan/task ids in code: clean"
	@# AGENTS.md: describing how the stock (OEM) firmware behaves, as an
	@# observed black box (a shipped binary own exported symbols, librtk.so
	@# and the like), is fine; naming a file or function read out of the
	@# vendor own SOURCE is not. This catches known vendor SOURCE citations
	@# specifically -- it is intentionally narrow (not e.g. a bare "rtk_" or
	@# "bsp_" prefix) because those prefixes are also how this tree spells
	@# sockopt ABI names and its own driver symbols.
	@# The headers of the vendor SDK count too (rtk/l2.h, rtk_*.h): a
	@# header name says where a fact was read.
	@if git grep -nE 'prom\.c|re8686|c-rlx\.c|apollo|bsp_[a-z_]+\(|(^|[^A-Za-z0-9_])(rtk|rtdrv|hal|dal|osal|ioal)/[A-Za-z0-9_/]+\.h|(^|[^A-Za-z0-9_])(rtk|rtdrv|rtusr)_[a-z0-9_]+\.h' -- . ':!Makefile'; then \
		echo "lint: found a vendor SOURCE file/function reference above -- describe the observed (black-box) behavior instead, and put vendor attribution in commit history/docs, per AGENTS.md" >&2; \
		exit 1; \
	fi
	@echo "no vendor source citations: clean"

clean:
	rm -rf build out

# Also drops the kernel trees and the download cache. The Docker volume the
# build uses -- odi-kbuild-618 (the kernel build workdir) -- survives this on
# purpose: it costs real time to rebuild and nothing in the tree depends on
# its contents. The toolchain images are docker images, not files here.
# Remove the volume deliberately:
#
#     docker volume rm odi-kbuild-618
distclean: clean
	rm -rf kernel/618/mainline dl
