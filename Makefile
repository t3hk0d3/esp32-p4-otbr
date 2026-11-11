# Entry point for building, flashing, updating and testing. `make help` lists the targets.
# The ESP-IDF environment is activated per command when idf.py is not on PATH already (IDF_EXPORT).

SHELL := bash
.SHELLFLAGS := -euo pipefail -c
.DEFAULT_GOAL := help

IDF_VERSION := $(shell cat IDF_VERSION)
IDF_EXPORT ?= $(HOME)/esp/esp-idf-$(IDF_VERSION)/export.sh
PORT ?= /dev/ttyACM0
C6_PORT ?= /dev/ttyUSB0
BOARD ?=
BOARD_ID ?=
CHIP_ID ?=
DIST ?= dist
PYTEST ?= uvx --with requests --with prometheus_client pytest
TARGET_PYTEST ?= .venv/bin/pytest
TEST ?=
MARK ?=
TIMEOUT ?= 900
SIZES ?= build/sizes.json
SIZES_BASE ?=
SIZES_BASE_LABEL ?=
MKDOCS ?= uvx --with mkdocs-material mkdocs
SITE ?= site

IDF := $(if $(shell command -v idf.py),,. "$(IDF_EXPORT)" >/dev/null &&)
# The firmware target (BOARD_ID and CHIP_ID, both or neither for the default one) as key=value words from tools/targets.py.
target_info := $(shell tools/targets.py resolve $(BOARD_ID) $(CHIP_ID) 2>&1)
$(if $(filter 0,$(.SHELLSTATUS)),,$(error $(target_info)))
target_var = $(patsubst $(1)=%,%,$(filter $(1)=%,$(target_info)))
target_build_dir = $(CURDIR)/$(call target_var,build_dir)
target_bundle = $(DIST)/$(if $(filter $(call target_var,build_dir),p4/build),otbr.bundle,$(call target_var,board_id)_$(call target_var,chip_id).ota)
# Every P4 idf.py call names the target's build dir; its sdkconfig is in that build dir's cache.
IDF_P4 = idf.py -C p4 -B $(target_build_dir)
target_option = $(if $(BOARD_ID),--target $(BOARD_ID)/$(CHIP_ID))
board_url = $(if $(BOARD),http://$(BOARD),$(error set BOARD=<board address>, e.g. make $@ BOARD=192.168.1.50))

.PHONY: help build build-test build-release reconfigure clean flash monitor erase c6-flash-base bundle package sizes sizes-report ota radio recover \
	status zigbee-test-device docs docs-serve docs-firmware test test-host test-vendored test-tools test-target lan-ping-up lan-ping-down lint routing-vectors

help: ## List the targets
	@awk -F':.*## ' '/^[a-z0-9-]+:.*## / {printf "  %-20s %s\n", $$1, $$2}' $(MAKEFILE_LIST)
	@echo
	@echo "Variables: PORT=$(PORT) C6_PORT=$(C6_PORT) BOARD=<address> BOARD_ID=<board_id> CHIP_ID=<chip_id> DIST=$(DIST) TEST=<test file> MARK=<marker>"

# --- build ---

build: ## Release build of c6_updater, c6_rcp, c6_installer and p4
	$(IDF) tools/build_all.sh $(target_option)

build-release: ## Release build of the C6 projects and p4 for every firmware target, with one version stamp
	$(IDF) tools/build_all.sh --all-targets

build-test: ## Test build (test-only console commands, Debug/Verbose logging compiled in; never ship it)
	$(IDF) tools/build_all.sh --test $(target_option)

reconfigure: ## Refresh the P4 version stamp (the -dirty flag is as of the last configure)
	$(IDF) $(IDF_P4) -DOTBR_BUILD_STAMP= reconfigure

clean: ## Remove all build directories and dist/
	rm -rf c6_updater/build c6_rcp/build c6_installer/build p4/build p4/build_* tools/zigbee_test_device/build $(DIST) $(SIZES)
	find . -path ./.venv -prune -o -type d -name build -path '*/test_host/build' -print0 | xargs -0 rm -rf

zigbee-test-device: ## Build the rig's Zigbee test device (on/off light router for the aux C6, tools/zigbee_test_device)
	$(IDF) idf.py -C tools/zigbee_test_device build

# --- flash over USB ---

flash: ## Flash the P4 over USB (bootloader, partition table, app, web UI); the C6 is not touched
	$(IDF) $(IDF_P4) -p $(PORT) flash

monitor: ## Open the P4 console (115200 baud)
	$(IDF) $(IDF_P4) -p $(PORT) monitor

erase: ## Erase the whole P4 flash, including the Thread dataset and network config
	$(IDF) idf.py -C p4 -p $(PORT) erase-flash

c6-flash-base: ## USB-UART install: erase the C6 and write its base images over a USB-UART on H7 (C6_PORT)
	cd c6_updater/build && $(IDF) esptool --chip esp32c6 -p $(C6_PORT) --before no-reset --after no-reset erase-flash
	cd c6_updater/build && $(IDF) esptool --chip esp32c6 -p $(C6_PORT) --before no-reset --after no-reset write-flash \
		$$(head -n 1 flash_args) 0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin \
		$$(awk -F', *' '$$1 == "test" {print $$4}' ../../common/c6_board/partitions_c6.csv) c6_updater.bin

# --- release files and updates over the network ---

bundle: ## Build the P4 OTA bundle from the target's build: dist/otbr.bundle, or dist/<board_id>_<chip_id>.ota (BOARD_ID, CHIP_ID)
	mkdir -p $(DIST)
	tools/make_bundle.py --web $(target_build_dir)/web_0.bin --app $(target_build_dir)/p4.bin --out $(target_bundle)

package: ## Collect all release files into dist/ (what CI publishes; RELEASE_TAG=<tag> adds the release line to versions.txt)
	$(IDF) tools/package_release.sh $(DIST)

sizes: ## Image sizes and flash headroom of the current build into SIZES (default build/sizes.json), then sizes-report
	tools/image_sizes.py collect --out $(SIZES)
	@$(MAKE) -s --no-print-directory sizes-report

sizes-report: ## Markdown table of SIZES; SIZES_BASE=<sizes.json of another build> adds the deltas (no ESP-IDF needed)
	tools/image_sizes.py report $(SIZES) $(if $(SIZES_BASE),--base $(SIZES_BASE)) \
		$(if $(SIZES_BASE_LABEL),--base-label "$(SIZES_BASE_LABEL)")

ota: bundle ## Update the P4 app and web UI over the network (BOARD, BOARD_ID, CHIP_ID)
	curl --fail-with-body --data-binary @$(target_bundle) $(board_url)/device/ota
	@echo

radio: ## Upload the C6 radio firmware c6_rcp/build/c6_rcp.bin (BOARD)
	curl --fail-with-body --data-binary @c6_rcp/build/c6_rcp.bin $(board_url)/device/radio/firmware
	@echo

recover: ## Maintenance jumper: rewrite the C6 base image (BOARD)
	curl --fail-with-body -X POST $(board_url)/device/radio/recover
	@echo

status: ## Show /device/status (BOARD)
	curl --fail-with-body -s $(board_url)/device/status
	@echo

# --- tests ---

# --- documentation site ---

docs: ## Build the documentation site into SITE (default site/); MKDOCS=<mkdocs command>
	$(MKDOCS) build --strict --site-dir $(SITE)

docs-serve: ## Preview the documentation site at http://127.0.0.1:8000 (the browser flasher has no firmware there)
	$(MKDOCS) serve

docs-firmware: ## Put the latest release's P4 images and the browser flasher manifest into SITE/firmware (needs gh)
	mkdir -p $(SITE)/firmware
	gh release download --repo t3hk0d3/esp32-p4-otbr --pattern p4-flash.zip --dir $(SITE)/firmware --clobber
	cd $(SITE)/firmware && unzip -o -q p4-flash.zip && rm -f p4-flash.zip
	tools/flash_manifest.py $(SITE)/firmware "$$(gh release view --repo t3hk0d3/esp32-p4-otbr --json tagName --jq .tagName)"

test: test-host test-tools test-vendored ## All host-side tests (no board)

test-host: ## Component host tests on the ESP-IDF linux target
	$(IDF) tools/run_host_tests.sh

test-tools: ## Tests of the tools and the device page script (needs node)
	$(IDF) $(PYTEST) tools/tests -q

test-vendored: ## Upstream esp_ot_br_server REST tests (needs clang++ and a p4 build for its managed components)
	$(IDF) tools/run_vendored_rest_tests.sh

test-target: ## On-board tests against a build-test image: TEST=<file or node id>, MARK=<marker> (slow)
	$(if $(TEST),,$(error set TEST=<test file or node id>, e.g. make test-target TEST=p4/pytest/test_br_boot.py))
	$(IDF) timeout $(TIMEOUT) $(TARGET_PYTEST) $(TEST) --port $(PORT) -p no:cacheprovider $(if $(MARK),-m $(MARK))

lan-ping-up: ## Disposable LAN container that reaches Thread through the board, for test_latency (LAN_IF, default mgmt0)
	tools/lan_ping.sh up

lan-ping-down: ## Remove the latency tests' LAN container
	tools/lan_ping.sh down

lint: ## Format and lint checks (pre-commit)
	uvx pre-commit run --all-files

routing-vectors: ## Regenerate the routing host-test vectors (vectors.h) with scapy
	uv run --no-project --with scapy==2.6.1 python tools/routing_vectors.py \
		p4/components/routing/test_host/main/vectors.h
