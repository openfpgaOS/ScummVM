# ScummVM port to openfpgaOS (Analogue Pocket)
#
# Cross-compiles ScummVM (SCUMM engine only) for RISC-V rv32imafc.
# Statically links upstream musl (bundled in the SDK at $(SDK_DIR)/musl/).
# Uses the openfpgaOS SDK for video, audio, input, and file I/O.

# ── Toolchain ───────────────────────────────────────────────────────
CROSS ?= $(shell which riscv64-unknown-elf-gcc >/dev/null 2>&1 && echo riscv64-unknown-elf- || echo riscv64-elf-)
CC      = $(CROSS)gcc
CXX     = $(CROSS)g++
LD      = $(CROSS)gcc
AS      = $(CROSS)gcc
OBJDUMP = $(CROSS)objdump
SIZE    = $(CROSS)size
STRIP   = $(CROSS)strip

# ── Architecture ────────────────────────────────────────────────────
ARCH = rv32imafc
ABI  = ilp32f

# ── Paths ───────────────────────────────────────────────────────────
SDK_DIR   = ../sdk
MUSL_DIR  = $(SDK_DIR)/musl
SCUMMVM   = scummvm
APP_LD    = $(SDK_DIR)/app.ld

# musl crt objects: crt1.o brings in _start and __libc_start_main,
# crti/crtn bracket .init/.fini for static constructors.
CRT_OBJS = $(MUSL_DIR)/lib/crt1.o $(MUSL_DIR)/lib/crti.o $(MUSL_DIR)/lib/crtn.o

# ── Compiler flags ──────────────────────────────────────────────────
COMMON_FLAGS  = -march=$(ARCH) -mabi=$(ABI) -Os -Wall
COMMON_FLAGS += -ffunction-sections -fdata-sections
COMMON_FLAGS += -fno-tree-loop-distribute-patterns

# Includes: openfpgaOS headers + bundled musl headers, in that order so
# `#include "of.h"` and `#include <stdio.h>` both resolve. -nostdinc keeps
# the host system headers out of the search path entirely.
COMMON_FLAGS += -nostdinc -I$(SDK_DIR)/include -isystem $(MUSL_DIR)/include -I.

# C flags
CFLAGS = $(COMMON_FLAGS)

# C++ flags
CXXFLAGS  = $(COMMON_FLAGS)
# gnu++11 (not c++11): leaves musl's _BSD_SOURCE/_XOPEN_SOURCE fallback
# enabled so POSIX functions like usleep() remain declared.
CXXFLAGS += -std=gnu++11
CXXFLAGS += -fno-exceptions -fno-rtti -fno-threadsafe-statics
# Leave -fuse-cxa-atexit (the GCC default) on: the SDK's of_cxxabi.cpp
# provides __cxa_atexit/__cxa_finalize/__dso_handle. -fno-use-cxa-atexit
# would emit atexit() calls instead, pulling in musl's atexit.o which
# also defines __cxa_atexit and triggers a multiple-definition link error.
CXXFLAGS += -Icompat/cxx_includes

# ScummVM-specific defines
CXXFLAGS += -DNONSTANDARD_PORT
CXXFLAGS += -DHAVE_CONFIG_H
CXXFLAGS += -DDISABLE_COMMAND_LINE
CXXFLAGS += -Wno-reorder -Wno-unused-parameter -Wno-sign-compare
CXXFLAGS += -Wno-missing-field-initializers -Wno-unused-variable

# Include paths for ScummVM sources
CXXFLAGS += -I$(SCUMMVM) -I$(SCUMMVM)/engines -I$(SCUMMVM)/backends

# Assembly flags
ASFLAGS = -march=$(ARCH)_zicsr -mabi=$(ABI)

# Linker flags
# -nostdlib: don't pull in host libc; we link musl explicitly.
# -static:   no dynamic linking.
LDFLAGS  = -march=$(ARCH) -mabi=$(ABI)
LDFLAGS += -nostdlib -static
LDFLAGS += -T $(APP_LD) -L$(MUSL_DIR)/lib
LDFLAGS += -Wl,--gc-sections -Wl,--no-warn-rwx-segments

LIBGCC = $(shell $(CC) -march=$(ARCH) -mabi=$(ABI) -print-libgcc-file-name)

# musl + math + libgcc, in the order the linker wants. We deliberately
# don't pull in the toolchain's libsupc++ -- it was built against newlib
# and references newlib-only symbols that musl doesn't ship. C++ apps
# link the SDK's own minimal C++ ABI shim ($(OF_CXXABI_SRC)) instead.
LIBS = -Wl,--start-group -lc -lm -lgcc -Wl,--end-group

# ── Source files ────────────────────────────────────────────────────

# C++ runtime (SDK-provided minimal C++ ABI shim)
CXX_RT_SRCS = $(SDK_DIR)/of_cxxabi.cpp

# Backend.  ISO 9660 reads now go through the kernel mount API
# (of_iso_mount + POSIX fopen/opendir under /cd) -- no vendored
# lib9660, no app-side sector cache.
BACKEND_SRCS = \
	backend/openfpga_osystem.cpp \
	backend/openfpga_mixer.cpp \
	backend/openfpga_midi.cpp \
	backend/openfpga_save.cpp \
	backend/openfpga_fs.cpp

# Compat stubs
COMPAT_SRCS = \
	compat/zlib_stub.cpp

# Main entry
MAIN_SRCS = main.cpp

# All local sources
LOCAL_SRCS = $(CXX_RT_SRCS) $(BACKEND_SRCS) $(COMPAT_SRCS) $(MAIN_SRCS)

# SDK init shim runs as a constructor before main (e.g. unbuffered stdout).
OF_INIT_SRC = $(SDK_DIR)/of_init.c
OF_INIT_OBJ = $(SDK_DIR)/of_init.o

# SDK MIDI / sample-voice synth: the .ofsf bank loader, voice engine,
# and Standard MIDI File parser. We use of_midi_init() + the smp_voice_*
# event API directly from openfpga_midi.cpp; the SMF parser in of_midi.c
# gets linked too (small enough to not bother stripping).
OF_SDK_C_SRCS = \
	$(SDK_DIR)/of_midi.c \
	$(SDK_DIR)/of_smp_bank.c \
	$(SDK_DIR)/of_smp_tables.c \
	$(SDK_DIR)/of_smp_voice.c
OF_SDK_C_OBJS = $(OF_SDK_C_SRCS:.c=.o)

# ── ScummVM common/ sources (all .cpp files) ───────────────────────
SCUMMVM_COMMON_SRCS = $(filter-out %/recorderfile.cpp %/updates.cpp, $(wildcard $(SCUMMVM)/common/*.cpp)) \
	$(wildcard $(SCUMMVM)/common/encodings/*.cpp) \
	$(wildcard $(SCUMMVM)/common/formats/*.cpp) \
	$(filter-out %/zlib.cpp %/gzio.cpp, $(wildcard $(SCUMMVM)/common/compression/*.cpp)) \
	$(wildcard $(SCUMMVM)/common/std/*.cpp)

# ── ScummVM base/ sources ──────────────────────────────────────────
SCUMMVM_BASE_SRCS = \
	$(SCUMMVM)/base/main.cpp \
	$(SCUMMVM)/base/plugins.cpp \
	$(SCUMMVM)/base/version.cpp \
	$(SCUMMVM)/base/commandLine.cpp

# ── ScummVM graphics/ sources (all, excluding OpenGL/TinyGL/larry) ──
SCUMMVM_GRAPHICS_SRCS = $(filter-out %/larryScale.cpp %/larryScale_generated.cpp %/svg.cpp, \
	$(wildcard $(SCUMMVM)/graphics/*.cpp)) \
	$(filter-out %/blit-atari.cpp %/blit-neon.cpp %/blit-sse2.cpp %/blit-avx2.cpp, \
	$(wildcard $(SCUMMVM)/graphics/blit/*.cpp)) \
	$(wildcard $(SCUMMVM)/graphics/fonts/*.cpp) \
	$(wildcard $(SCUMMVM)/graphics/macgui/*.cpp) \
	$(wildcard $(SCUMMVM)/graphics/scaler/*.cpp)

# ── ScummVM audio/ sources (all, excluding libmad/vorbis/flac deps) ─
AUDIO_EXCLUDE = %/mp3.cpp %/vorbis.cpp %/flac.cpp %/aac.cpp %/ac3.cpp %/alsa_opl.cpp %/opl2lpt.cpp %/retrowave_opl3.cpp %/rwopl3.cpp %/fluidsynth.cpp %/mt32.cpp %/eas.cpp
SCUMMVM_AUDIO_SRCS = $(filter-out $(AUDIO_EXCLUDE), \
	$(wildcard $(SCUMMVM)/audio/*.cpp) \
	$(wildcard $(SCUMMVM)/audio/decoders/*.cpp) \
	$(wildcard $(SCUMMVM)/audio/softsynth/*.cpp) \
	$(wildcard $(SCUMMVM)/audio/softsynth/opl/*.cpp) \
	$(wildcard $(SCUMMVM)/audio/softsynth/fmtowns_pc98/*.cpp) \
	$(wildcard $(SCUMMVM)/audio/mods/*.cpp))

# ── ScummVM SCUMM engine sources (all subdirs, exclude HE/network) ─
SCUMM_EXCLUDE = %/dialog-createsession.cpp %/dialog-sessionselector.cpp \
	%/nut_renderer.cpp %/string_v7.cpp %/script_v8.cpp
SCUMMVM_ENGINE_SRCS = $(filter-out $(SCUMM_EXCLUDE), $(wildcard $(SCUMMVM)/engines/scumm/*.cpp)) \
	$(wildcard $(SCUMMVM)/engines/scumm/imuse/*.cpp) \
	$(wildcard $(SCUMMVM)/engines/scumm/imuse/drivers/*.cpp) \
	$(wildcard $(SCUMMVM)/engines/scumm/players/*.cpp) \
	$(wildcard $(SCUMMVM)/engines/scumm/macgui/*.cpp) \
	$(SCUMMVM)/engines/scumm/he/mixer_he.cpp \
	$(SCUMMVM)/engines/scumm/he/resource_he.cpp \
	$(SCUMMVM)/engines/scumm/he/script_v60he.cpp \
	$(SCUMMVM)/engines/scumm/he/script_v70he.cpp \
	$(SCUMMVM)/engines/scumm/he/sound_he.cpp

# ── ScummVM engine framework ───────────────────────────────────────
SCUMMVM_ENGINES_SRCS = $(wildcard $(SCUMMVM)/engines/*.cpp)

# ── ScummVM math sources (needed by WMA/WMA-Pro audio decoders) ────
# MDCT pulls in fft, cosinetables, sinetables, sinewindows.  We add
# the whole math/ tree and let --gc-sections drop the rest.
SCUMMVM_MATH_SRCS = $(wildcard $(SCUMMVM)/math/*.cpp)

# ── ScummVM backends infrastructure ─────────────────────────────────
SCUMMVM_BACKEND_SRCS = \
	$(SCUMMVM)/backends/base-backend.cpp \
	$(SCUMMVM)/backends/modular-backend.cpp \
	$(SCUMMVM)/backends/mixer/null/null-mixer.cpp \
	$(SCUMMVM)/backends/timer/default/default-timer.cpp \
	$(SCUMMVM)/backends/events/default/default-events.cpp \
	$(SCUMMVM)/backends/saves/default/default-saves.cpp \
	$(SCUMMVM)/backends/saves/savefile.cpp \
	$(SCUMMVM)/backends/audiocd/default/default-audiocd.cpp \
	$(wildcard $(SCUMMVM)/backends/keymapper/*.cpp) \
	$(SCUMMVM)/backends/fs/abstract-fs.cpp

# ── ScummVM GUI ────────────────────────────────────────────────────
GUI_EXCLUDE = %/onscreendialog.cpp %/recorderdialog.cpp %/updates-dialog.cpp \
	%/EventRecorder.cpp %/editrecorddialog.cpp %/fluidsynth-dialog.cpp \
	%/dump-all-dialogs.cpp %/integrity-dialog.cpp
SCUMMVM_GUI_SRCS = $(filter-out $(GUI_EXCLUDE), \
	$(wildcard $(SCUMMVM)/gui/*.cpp) \
	$(wildcard $(SCUMMVM)/gui/widgets/*.cpp))

# ── ScummVM image decoders (needed by GUI) ─────────────────────────
SCUMMVM_IMAGE_SRCS = $(filter-out %/gif.cpp %/mpeg.cpp, \
	$(wildcard $(SCUMMVM)/image/*.cpp) \
	$(wildcard $(SCUMMVM)/image/codecs/*.cpp))

# ── ScummVM video/ sources ─────────────────────────────────────────
VIDEO_EXCLUDE = %/theora_decoder.cpp %/bink_decoder.cpp %/hnm_decoder.cpp \
	%/mpegps_decoder.cpp %/psx_decoder.cpp %/mkv_decoder.cpp
SCUMMVM_VIDEO_SRCS = $(filter-out $(VIDEO_EXCLUDE), \
	$(wildcard $(SCUMMVM)/video/*.cpp))

# All ScummVM sources
SCUMMVM_SRCS = $(SCUMMVM_COMMON_SRCS) $(SCUMMVM_BASE_SRCS) \
               $(SCUMMVM_GRAPHICS_SRCS) $(SCUMMVM_AUDIO_SRCS) \
               $(SCUMMVM_ENGINE_SRCS) $(SCUMMVM_ENGINES_SRCS) \
               $(SCUMMVM_BACKEND_SRCS) $(SCUMMVM_GUI_SRCS) \
               $(SCUMMVM_IMAGE_SRCS) $(SCUMMVM_VIDEO_SRCS) \
               $(SCUMMVM_MATH_SRCS)

# ── Object files ────────────────────────────────────────────────────

LOCAL_OBJS    = $(LOCAL_SRCS:.cpp=.o)
SCUMMVM_OBJS  = $(SCUMMVM_SRCS:.cpp=.o)
ALL_OBJS      = $(LOCAL_OBJS) $(SCUMMVM_OBJS) $(OF_INIT_OBJ) $(OF_SDK_C_OBJS)

# ── Release / deploy paths ──────────────────────────────────────────
# Match the SDK custom-core template: dist/<app>/ is the source tree,
# build/<app>/ is the assembled SD-card image consumed by copy.sh /
# package.sh.
APP       = scummvm
TARGET    = pocket
ROOT      = $(realpath $(CURDIR)/../..)
DIST      = $(ROOT)/dist/$(APP)
OUT       = $(ROOT)/build/$(APP)
RUNTIME   = $(ROOT)/runtime

# ── Targets ─────────────────────────────────────────────────────────

all: app.elf release
	$(SIZE) $<

app.elf: $(CRT_OBJS) $(ALL_OBJS) $(APP_LD)
	$(LD) $(LDFLAGS) -o $@ $(CRT_OBJS) $(ALL_OBJS) $(LIBS) $(LIBGCC)

# SDK C sources (of_init.c, of_midi.c, of_smp_*.c)
$(SDK_DIR)/%.o: $(SDK_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# SDK C++ sources (of_cxxabi.cpp)
$(SDK_DIR)/%.o: $(SDK_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Local C++ sources
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# ScummVM C++ sources
$(SCUMMVM)/%.o: $(SCUMMVM)/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# ── SD-card image assembly ──────────────────────────────────────────
# Copies dist/scummvm/ to build/scummvm/, then layers in the runtime
# bitstream + loader (per-Core), os.bin + scummvm.elf (per-platform
# common), so the result is a tree the user can drop straight onto
# the Pocket SD card.
release: app.elf
	@rm -rf $(OUT)
	@mkdir -p $(dir $(OUT))
	@cp -r $(DIST) $(OUT)
	@for d in $(OUT)/Cores/*/; do \
		cp $(RUNTIME)/bitstream.rbf_r $(RUNTIME)/loader.bin "$$d"; \
	done
	@for d in $(OUT)/Assets/*/; do \
		mkdir -p "$$d/common"; \
		cp $(RUNTIME)/os.bin "$$d/common/"; \
		cp app.elf "$$d/common/$(APP).elf"; \
		[ -f $(RUNTIME)/bank.ofsf ] && cp $(RUNTIME)/bank.ofsf "$$d/common/" || true; \
	done
	@echo "Ready: build/$(APP)/"

# ── Deploy / package wrappers ───────────────────────────────────────
copy: release
	@$(SDK_DIR)/platforms/$(TARGET)/copy.sh "$(APP)" "$$(ls -d $(OUT)/Assets/*/common)/$(APP).elf"

package: release
	@$(ROOT)/scripts/package.sh $(APP)

# ── UART push + console (single-app debug) ──────────────────────────
debug: app.elf
	@$(ROOT)/scripts/debug.sh app.elf

clean:
	rm -f $(ALL_OBJS) app.elf
	find . -name '*.o' -delete
	rm -rf $(OUT)

.PHONY: all release copy package debug clean

.PHONY: all clean
