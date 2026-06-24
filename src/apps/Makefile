#------------------------------------------------------------------------------
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
#------------------------------------------------------------------------------

# openfpgaOS SDK — SDK apps
#
# This Makefile drives the "SDK app" build path: every app under
# src/apps/<name>/ is compiled and bundled into the shared SDK demo
# core (dist/sdk/ → build/pocket/sdk/), which packages to a single ZIP.
#
# For the other path — a CUSTOM CORE that ships its own standalone
# openFPGA core wrapping a single app — use `make core` from the
# SDK root instead.
#
# Targets:
#   make                  Build every SDK app → build/pocket/sdk/
#   make new APP=demo     Scaffold a new SDK app under src/apps/demo/
#   make debug APP=demo   Push a single SDK app's ELF over UART
#   make test APP=demo    Build a single SDK app for desktop (SDL2)
#   make copy             Copy build/pocket/sdk/ to the Pocket SD card
#   make package          Package build/pocket/sdk/ as a single ZIP
#   make clean            Remove build artifacts
#

SDK_ROOT  = $(realpath $(CURDIR)/../..)
RUNTIME   = $(SDK_ROOT)/runtime
# Pocket FPGA/kernel artifacts live under runtime/pocket/ (uniform with
# runtime/mister/); bank.ofsf is target-agnostic and stays at runtime/.
RUNTIME_POCKET = $(RUNTIME)/pocket
DIST      = $(SDK_ROOT)/dist/sdk
CORE_ID   = ThinkElastic.openfpgaOS
PLATFORM  = openfpgaos
# The assembled APF tree is Pocket-specific -> build/pocket/sdk.  MiSTer
# images are per-app: `make build APP=<name> TARGET=mister` from the SDK
# root (one openfpgaOS.vhd per app under build/mister/<name>/).
BUILD     = $(SDK_ROOT)/build/pocket/sdk

# Discover SDK apps: every src/apps/<name>/ that has its own Makefile.
APPS = $(sort $(patsubst %/,%,$(dir $(wildcard */Makefile))))

all: build-apps release

build-apps:
	@for app in $(APPS); do \
		echo "Building SDK app: $$app..."; \
		$(MAKE) -C $$app || exit 1; \
	done

# Assemble: dist/ (static) + runtime + ELFs + data files = deployable
release:
	@echo "Assembling build/pocket/sdk/..."
	@rm -rf $(BUILD)
	@mkdir -p $(SDK_ROOT)/build/pocket
	@cp -r $(DIST)/ $(BUILD)/
	@# Runtime binaries (pocket-specific → runtime/pocket/).  Copy EVERY
	@# variant bitstream the demo core's core.json names (os25.rbf_r + os30.rbf_r)
	@# plus the loader VM.  The bitstream is chosen per-instance at load time:
	@# the chip32 loader probes the "Variant Select" data slot (data.json id 9,
	@# stamped onto os30 apps by apply-variants.sh) and loads os25 or os30.
	@for rbf in $$(grep -o '"[^"]*\.rbf_r"' $(BUILD)/Cores/$(CORE_ID)/core.json | tr -d '"' | sort -u); do \
		cp $(RUNTIME_POCKET)/$$rbf $(BUILD)/Cores/$(CORE_ID)/ || exit 1; \
	done
	@cp $(RUNTIME_POCKET)/loader.bin $(BUILD)/Cores/$(CORE_ID)/
	@mkdir -p $(BUILD)/Assets/$(PLATFORM)/common
	@cp $(RUNTIME_POCKET)/os.bin $(BUILD)/Assets/$(PLATFORM)/common/
	@# Sound bank (SC-55 GM, target-agnostic → runtime/)
	@cp $(RUNTIME)/bank.ofsf $(BUILD)/Assets/$(PLATFORM)/common/ 2>/dev/null || true
	@# Demo ELFs + data files
	@for app in $(APPS); do \
		[ -f "$(SDK_ROOT)/.obj/sdk/$$app/app.elf" ] && \
			cp "$(SDK_ROOT)/.obj/sdk/$$app/app.elf" "$(BUILD)/Assets/$(PLATFORM)/common/$$app.elf"; \
		for f in $$app/*.mid $$app/*.mod $$app/*.wav $$app/*.dat $$app/*.png \
		         $$app/*.bin $$app/*.cfg $$app/*.iso; do \
			[ -f "$$f" ] && cp "$$f" "$(BUILD)/Assets/$(PLATFORM)/common/"; \
		done; \
	done 2>/dev/null; true
	@echo "Ready: build/pocket/sdk/"

new:
	@test -n "$(APP)" || { echo "Usage: make new APP=myapp"; exit 1; }
	@$(SDK_ROOT)/scripts/new.sh --sdk-app "$(APP)"

# Single-app debug / test. Both delegate to the per-app Makefile so
# they pick up that app's exact SRCS / CFLAGS, including the special
# cases like mididemo (pulls of_midi.c) and cxxdemo (C++ ABI shim).
debug:
	@test -n "$(APP)" || { echo "Usage: make debug APP=<sdk-app>"; exit 1; }
	@test -d "$(APP)" || { echo "No such SDK app: src/apps/$(APP)"; exit 1; }
	$(MAKE) -C $(APP) debug

test:
	@test -n "$(APP)" || { echo "Usage: make test APP=<sdk-app>"; exit 1; }
	@test -d "$(APP)" || { echo "No such SDK app: src/apps/$(APP)"; exit 1; }
	$(MAKE) -C $(APP) test

copy: all
	@$(SDK_ROOT)/scripts/copy.sh

package: all
	@$(SDK_ROOT)/scripts/package.sh

clean:
	@for app in $(APPS); do \
		$(MAKE) -C $$app clean; \
	done
	rm -rf $(BUILD)

.PHONY: all build-apps release new debug test copy package clean
