# SPDX-License-Identifier: CC0-1.0
#
# SPDX-FileContributor: Antonio Niño Díaz, 2023

BLOCKSDS	?= /opt/blocksds/core
BLOCKSDSEXT	?= /opt/blocksds/external

export TARGET := cart_flasher
# Build provenance is shown in-app. ROM naming itself only distinguishes local
# development, nightly, and release builds.
CART_FLASHER_COMMIT ?= $(shell git describe --always --abbrev=7 2>/dev/null || echo unknown)
export CART_FLASHER_COMMIT
# Falls back to "unknown" (not a stale hardcoded version) when no tag is
# reachable, e.g. a shallow clone or a source archive with no .git.
CART_FLASHER_VERSION ?= $(shell git describe --tags --abbrev=0 2>/dev/null || echo unknown)
export CART_FLASHER_VERSION
export NDS_OUT := $(TARGET)-dev.nds
export TOPDIR := $(CURDIR)

# Build provenance shown in-app. CI overrides this on the command line.
CART_FLASHER_BUILD_KIND ?= Dev
export CART_FLASHER_BUILD_KIND

GAME_TITLE     := Cart-Flasher
GAME_SUBTITLE  := Flashcart Backup and Restore
GAME_AUTHOR    := Nimbo
GAME_ICON      := resources/icon.png

GAME_FULL_TITLE := $(GAME_TITLE);$(GAME_SUBTITLE);$(GAME_AUTHOR)

# Source code paths
ARM9DIR		:= arm9
ARM7DIR		:= arm7

# Tools
MAKE		:= make
RM		:= rm -rf

ifeq ($(VERBOSE),1)
V		:=
else
V		:= @
endif

.PHONY: all clean arm9 arm7

all: $(NDS_OUT)

clean:
	@echo "  CLEAN"
	$(V)$(MAKE) -C $(ARM9DIR) clean --no-print-directory
	$(V)$(MAKE) -C $(ARM7DIR) clean --no-print-directory
	$(V)$(RM) $(TARGET)-*.nds

arm9:
	$(V)+$(MAKE) -C $(ARM9DIR) --no-print-directory

arm7:
	$(V)+$(MAKE) -C $(ARM7DIR) --no-print-directory

$(NDS_OUT) : arm9 arm7
	@echo "  NDSTOOL $@"
	$(V)$(BLOCKSDS)/tools/ndstool/ndstool -c $@ \
		-7 $(ARM7DIR)/$(TARGET).elf -9 $(ARM9DIR)/$(TARGET).elf \
		-b $(GAME_ICON) "$(GAME_FULL_TITLE)"
	@branch=$$(git symbolic-ref --short -q HEAD \
		|| printf '%s' "$${GITHUB_REF_NAME:-detached}"); \
	printf '%s\n' \
		'=== Build summary ===' \
		'  ROM     $(NDS_OUT)' \
		'  BANNER  $(GAME_TITLE)' \
		'          $(GAME_SUBTITLE)' \
		'          $(GAME_AUTHOR)' \
		'  BUILD   kind=$(CART_FLASHER_BUILD_KIND)'; \
	printf '  SOURCE  commit=$(CART_FLASHER_COMMIT) branch=%s\n' "$$branch"
