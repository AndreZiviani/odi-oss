# odi-oss — firmware for the ODI DFP-34X-2C2, built from source.
#
# Read README.md and docs/FLASHING.md before expecting a working image.

.PHONY: help toolchain toolchain-audit kernel packages busybox \
	src releases image image-all clean distclean lint test test-host test-omci \
	test-diag

help:
	@echo "targets:"
	@echo "  toolchain       build gcc 16.2.0 + binutils 2.47 + uClibc-ng 1.0.59"
	@echo "  toolchain-audit  prove the target libraries carry no illegal instruction"
	@echo "  kernel          Linux 6.18 for the RTL9602C (kernel/build.sh)"
	@echo "  busybox         upstream busybox, ISA-audited"
	@echo "  packages        every userland package"
	@echo "  src             our own binaries, freestanding, into out/bin"
	@echo "  releases        metricsd and confd from their own releases"
	@echo "  image           assemble a flashable tarball into out/image/"
	@echo "  image-all       everything above from a clean clone, in order"
	@echo "  test            lint + the host-side tests + diag + the OMCI daemon under qemu (~2 min)"
	@echo "  lint            shellcheck every script here"
	@echo
	@echo "Nothing here flashes anything. See docs/FLASHING.md."
	@echo "An image is INERT when flashed; nv setenv sw_tryactive <slot> boots it once."

# Our own, from current source. Lives in the Docker volume odi-oss-toolchain-318,
# not in the working tree: the build writes hundreds of thousands of small
# files and a macOS bind mount is both slow and case-insensitive.
toolchain:
	./toolchain/build-oss-toolchain.sh

toolchain-audit:
	./toolchain/audit-toolchain.sh

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
# already done (docker layers, pinned tarballs, the .built-with stamps).
image-all: toolchain kernel packages src releases image

# What runs without a stick. test-host needs only bash and the repo (the flash
# accessor against a sample store, the fwu.sh guards against captured /proc
# files); test-omci builds our binaries and drives omcid under qemu-user in the
# diag toolchain container (src/omci/qemu-test.sh, about 130 checks). The
# QEMU system harnesses under test/ that need a staged rootfs or a built kernel
# stay manual; each says so in its header.
test: lint test-host test-diag test-omci
	@echo "ok"

test-host:
	bash test/flash_test.sh
	bash test/fwu_guard_test.sh
	bash test/fwu_starter_test.sh
	bash test/network_addr_test.sh
	bash test/apply_test.sh
	bash test/regtrace_decode_test.sh
	bash test/regtrace_sequence_test.sh
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
	bash test/odi_switch_parity_test.sh
	bash test/odi_omci_test.sh
	bash test/regdump_test.sh
	bash test/mkparity_test.sh
	bash test/parity_load_test.sh
	bash test/odi_omci_parity_parse_test.sh
	bash test/mkmodload_test.sh
	bash test/odi_switch_modload_replay_test.sh
	bash test/mksdkinit_test.sh
	bash test/odi_switch_sdkinit_replay_test.sh
	bash test/odi_replay_blob_test.sh
	bash test/odi_switch_ds_encrypt_test.sh
	bash test/odi_switch_gpon_ploam_test.sh
	bash test/omci_ageing_time_test.sh
	bash test/odi_gpon_test.sh
	bash test/odi_gpon_replay_test.sh
	bash test/odi_intr_test.sh
	bash test/odi_board_test.sh
	bash test/odi_i2c_test.sh
	bash test/odi_ddm_test.sh
	bash test/odi_rtk_init_test.sh
	bash test/odi_wdt_test.sh
	bash test/odi_ramlog_test.sh
	bash test/procparse_test.sh
	bash test/odi_switch_flows_test.sh
	python3 test/regnames_kernel_test.py

# diag's parser and conversion tests, natively, then under qemu in the diag
# toolchain container: the conversion vectors and the exporter contract (the
# batch metricsd sends, byte for byte against test/exporter.golden).
test-diag:
	$(MAKE) -C src/diag test selftest

test-omci: src
	docker run --rm --user $$(id -u):$$(id -g) -e HOME=/tmp -v "$(CURDIR)":/src -w /src/src/omci odi-diag-toolchain sh qemu-test.sh

lint:
	shellcheck -S warning toolchain/*.sh kernel/*.sh packages/*.sh packages/*/*.sh \
	          image/*.sh tools/*.sh src/*.sh test/*.sh
	@# The scripts that actually run ON the device, checked as POSIX sh
	@# because busybox ash is what interprets them -- not bash. These were
	@# outside the lint entirely until 2026-09-14, which is backwards: they
	@# are the only ones whose failure costs a stick rather than a build.
	@# tools/regdump/dump.sh joins them here for the same reason: it is
	@# pushed to and run on the stick, against a minimal busybox, not
	@# built or run on the host.
	shellcheck -S warning -s sh rootfs/skeleton/etc/init.d/* rootfs/skeleton/etc/scripts/*.sh rootfs/skeleton/etc/scripts/regreplay rootfs/skeleton/etc/scripts/flash tools/regdump/dump.sh
	@echo "shellcheck: clean"
	@./tools/check-inline-quotes.sh toolchain/*.sh kernel/*.sh packages/*.sh \
	          packages/*/*.sh image/*.sh src/*.sh tools/*.sh test/*.sh
	@# This repo is public; the private investigation workspace it was
	@# developed alongside is not, and none of its paths or document names
	@# may leak into tracked files here. Excludes this Makefile itself,
	@# since the pattern below necessarily contains the strings it looks for.
	@if git grep -nE 'investigations/|odi-sfp-re|SPEC-(NIC|GPON)|NIC-ABI|PLAN-[A-Z]|INVENTORY-|ANALYSIS-|RESEARCH-|REVIEW-|NOTES\.md|KB-DRAFTS|vendor-src/|regtrace/(omci|gpon|sdkinit|nic)/|kb/systems/|kb/decisions/|kernel/patches/|kernel/vendor/|~/git/' -- ':!Makefile'; then \
		echo "lint: found a reference to the private investigation workspace above -- restate the point in this repo own words instead" >&2; \
		exit 1; \
	fi
	@echo "no private-workspace references: clean"
	@# AGENTS.md: describing how the stock (OEM) firmware behaves, as an
	@# observed black box (a shipped binary own exported symbols, librtk.so
	@# and the like), is fine; naming a file or function read out of the
	@# vendor own SOURCE is not. This catches known vendor SOURCE citations
	@# specifically -- it is intentionally narrow (not e.g. a bare "rtk_" or
	@# "bsp_" prefix) because those prefixes are also how this tree spells
	@# sockopt ABI names and its own driver symbols.
	@if git grep -nE 'prom\.c|re8686|c-rlx\.c|apollo|bsp_[a-z_]+\(' -- . ':!Makefile'; then \
		echo "lint: found a vendor SOURCE file/function reference above -- describe the observed (black-box) behavior instead, and put vendor attribution in commit history/docs, per AGENTS.md" >&2; \
		exit 1; \
	fi
	@echo "no vendor source citations: clean"

clean:
	rm -rf build out

# Also drops the kernel trees and the download cache. The Docker volumes the
# build uses -- odi-oss-toolchain-318 (the toolchain we build ourselves) and
# odi-kbuild-618 (the kernel build workdir) -- survive this on purpose: they
# cost real time to rebuild and nothing in the tree depends on their
# contents. Remove them deliberately:
#
#     docker volume rm odi-oss-toolchain-318 odi-kbuild-618
distclean: clean
	rm -rf kernel/618/mainline dl
