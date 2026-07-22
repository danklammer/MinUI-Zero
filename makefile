# MinUI

# NOTE: this runs on the host system (eg. macOS) not in a docker image
# it has to, otherwise we'd be running a docker in a docker and oof

# prevent accidentally triggering a full build with invalid calls
ifneq (,$(PLATFORM))
ifeq (,$(MAKECMDGOALS))
$(error found PLATFORM arg but no target, did you mean "make PLATFORM=$(PLATFORM) shell"?)
endif
endif

ifeq (,$(PLATFORMS))
# This is a Brick/Smart Pro (tg5040) focused fork. Other platforms are frozen in
# workspace/_unmaintained/ (NextUI-style) and are not built or supported.
PLATFORMS = tg5040
endif

###########################################################

BUILD_HASH:=$(shell git rev-parse --short HEAD)
ZERO_VERSION=v1.5.2
RELEASE_TIME:=$(shell TZ=GMT date +%Y%m%d)
RELEASE_BETA=
RELEASE_BASE=MinUI-Zero-$(RELEASE_TIME)$(RELEASE_BETA)
# highest existing suffix + 1 — counting files breaks after any deletion (a stale count
# can re-issue an existing name and zip -r would append into the shipped artifact)
RELEASE_DOT:=$(shell find -E ./releases/. -regex ".*/${RELEASE_BASE}-[0-9]+-base\.zip" | sed -E 's/.*-([0-9]+)-base\.zip/\1/' | awk 'BEGIN{m=-1}{if($$1+0>m)m=$$1+0}END{print m+1}')
RELEASE_NAME=$(RELEASE_BASE)-$(RELEASE_DOT)
LICENSE_CORES=fceumm gambatte gpsp pcsx_rearmed picodrive snes9x2005_plus mednafen_pce_fast mednafen_vb mednafen_supafaust mgba fake-08

###########################################################

.PHONY: build

export MAKEFLAGS=--no-print-directory

all: setup $(PLATFORMS) special package done
	
shell:
	make -f makefile.toolchain PLATFORM=$(PLATFORM)

name:
	@echo $(RELEASE_NAME)

# host-side unit tests (no device, no toolchain)
.PHONY: test-governor test-telemetry test-save-io test-ff-audio test-undervolt test-reproducibility test-wakeup test-gov-memory test-dupskip test-snd-pacing check-threading-policy
test-governor:
	sh ./workspace/all/common/run-governor-tests.sh
test-telemetry:
	sh ./workspace/all/common/run-telemetry-tests.sh
test-save-io:
	sh ./workspace/all/common/run-save-io-tests.sh
test-ff-audio:
	sh ./workspace/all/common/run-ff-audio-tests.sh
test-undervolt:
	sh ./workspace/tg5040/undervolt/run-tests.sh
test-reproducibility:
	sh ./workspace/all/cores/run-source-verifier-tests.sh
test-wakeup:
	sh ./workspace/all/common/run-wakeup-tests.sh
test-gov-memory:
	sh ./workspace/all/common/run-gov-memory-tests.sh
test-dupskip:
	sh ./workspace/all/common/run-dupskip-tests.sh
test-snd-pacing:
	sh ./workspace/all/common/run-snd-pacing-tests.sh
check-threading-policy:
	sh ./workspace/all/common/check-threading-policy.sh
# threading v2 framering protocol module (host; TSan/ASan are SEPARATE builds per contract)
.PHONY: test-frame-pool test-framering test-framering-tsan test-framering-asan
test-frame-pool:
	sh ./workspace/all/common/run-frame-pool-tests.sh
test-framering:
	sh ./workspace/all/common/run-framering-tests.sh plain
test-framering-tsan:
	sh ./workspace/all/common/run-framering-tests.sh tsan
test-framering-asan:
	sh ./workspace/all/common/run-framering-tests.sh asan
# threading v2 frontend_core lifecycle engine (host; F31 cleanup oracle + adversarial runtime)
.PHONY: test-frontend-core test-frontend-core-tsan test-frontend-core-asan
test-frontend-core:
	sh ./workspace/all/common/run-frontend-core-tests.sh plain
test-frontend-core-tsan:
	sh ./workspace/all/common/run-frontend-core-tests.sh tsan
test-frontend-core-asan:
	sh ./workspace/all/common/run-frontend-core-tests.sh asan
.PHONY: check-forbidden-globals
check-forbidden-globals:
	sh ./workspace/all/common/check-forbidden-globals.sh

build:
	# ----------------------------------------------------
	make build -f makefile.toolchain PLATFORM=$(PLATFORM)
	# ----------------------------------------------------

system:
	make -f ./workspace/$(PLATFORM)/platform/makefile.copy PLATFORM=$(PLATFORM)
	
	# populate system
	cp ./workspace/$(PLATFORM)/keymon/keymon.elf ./build/SYSTEM/$(PLATFORM)/bin/
	cp ./workspace/$(PLATFORM)/libmsettings/libmsettings.so ./build/SYSTEM/$(PLATFORM)/lib
	cp ./workspace/all/minui/build/$(PLATFORM)/minui.elf ./build/SYSTEM/$(PLATFORM)/bin/
	cp ./workspace/all/minarch/build/$(PLATFORM)/minarch.elf ./build/SYSTEM/$(PLATFORM)/bin/
	cp ./workspace/all/syncsettings/build/$(PLATFORM)/syncsettings.elf ./build/SYSTEM/$(PLATFORM)/bin/
	cp ./workspace/all/say/build/$(PLATFORM)/say.elf ./build/SYSTEM/$(PLATFORM)/bin/
	cp ./workspace/all/confirm/build/$(PLATFORM)/confirm.elf ./build/SYSTEM/$(PLATFORM)/bin/
	cp ./workspace/all/clock/build/$(PLATFORM)/clock.elf ./build/EXTRAS/Tools/$(PLATFORM)/Clock.pak/
	cp ./workspace/all/minput/build/$(PLATFORM)/minput.elf ./build/EXTRAS/Tools/$(PLATFORM)/Input.pak/
	# Tune Voltage harness binaries -> the pak (tg5040 only)
	if [ "$(PLATFORM)" = "tg5040" ]; then \
		mkdir -p "./build/EXTRAS/Tools/tg5040/Optimize CPU.pak/bin"; \
		cp ./workspace/tg5040/undervolt/build/uvtool "./build/EXTRAS/Tools/tg5040/Optimize CPU.pak/bin/"; \
		cp ./workspace/tg5040/undervolt/build/stress "./build/EXTRAS/Tools/tg5040/Optimize CPU.pak/bin/"; \
		cp ./workspace/tg5040/undervolt/build/deadman "./build/EXTRAS/Tools/tg5040/Optimize CPU.pak/bin/"; \
		cp ./workspace/tg5040/undervolt/uvmap.sh "./build/EXTRAS/Tools/tg5040/Optimize CPU.pak/bin/"; \
	fi

cores: # TODO: can't assume every platform will have the same stock cores (platform should be responsible for copy too)
	# stock cores
	cp ./workspace/$(PLATFORM)/cores/output/fceumm_libretro.so ./build/SYSTEM/$(PLATFORM)/cores
	cp ./workspace/$(PLATFORM)/cores/output/gambatte_libretro.so ./build/SYSTEM/$(PLATFORM)/cores
	cp ./workspace/$(PLATFORM)/cores/output/gpsp_libretro.so ./build/SYSTEM/$(PLATFORM)/cores
	cp ./workspace/$(PLATFORM)/cores/output/picodrive_libretro.so ./build/SYSTEM/$(PLATFORM)/cores
	cp ./workspace/$(PLATFORM)/cores/output/snes9x2005_plus_libretro.so ./build/SYSTEM/$(PLATFORM)/cores
	cp ./workspace/$(PLATFORM)/cores/output/pcsx_rearmed_libretro.so ./build/SYSTEM/$(PLATFORM)/cores
	cp ./workspace/$(PLATFORM)/cores/output/mednafen_supafaust_libretro.so ./build/SYSTEM/$(PLATFORM)/cores # SNES default (SUPA in base)
	
	# extras
	# Extra systems ship DORMANT in the base: cores live in their paks under .system, but no
	# Roms folder is created for them, so MinUI doesn't show them out of the box. A user who
	# wants one just makes a Roms folder with the matching tag (eg. "Virtual Boy (VB)") and it
	# lights up — the tuned core is already there. One download, clean default, no extras zip.
	cp ./workspace/$(PLATFORM)/cores/output/mgba_libretro.so ./build/SYSTEM/$(PLATFORM)/paks/Emus/MGBA.pak
	cp ./workspace/$(PLATFORM)/cores/output/mgba_libretro.so ./build/SYSTEM/$(PLATFORM)/paks/Emus/SGB.pak
	cp ./workspace/$(PLATFORM)/cores/output/mednafen_pce_fast_libretro.so ./build/SYSTEM/$(PLATFORM)/paks/Emus/PCE.pak
	cp ./workspace/$(PLATFORM)/cores/output/picodrive_libretro.so ./build/SYSTEM/$(PLATFORM)/paks/Emus/GG.pak
	cp ./workspace/$(PLATFORM)/cores/output/picodrive_libretro.so ./build/SYSTEM/$(PLATFORM)/paks/Emus/SMS.pak
	cp ./workspace/$(PLATFORM)/cores/output/mednafen_vb_libretro.so ./build/SYSTEM/$(PLATFORM)/paks/Emus/VB.pak
	cp ./workspace/$(PLATFORM)/cores/output/fake08_libretro.so ./build/SYSTEM/$(PLATFORM)/paks/Emus/P8.pak

common: build system cores
	
clean:
	rm -rf ./build

setup: name
	# ----------------------------------------------------
	# make sure we're running in an input device (non-fatal: allow headless/CI builds)
	tty -s || true
	
	# ready fresh build
	rm -rf ./build
	mkdir -p ./releases
	cp -R ./skeleton ./build
	
	# remove authoring detritus
	cd ./build && find . -type f -name '.keep' -delete
	cd ./build && find . -type f -name '*.meta' -delete
	echo $(BUILD_HASH) > ./workspace/hash.txt
	
	# copy readmes to workspace so we can use Linux fmt instead of host's
	mkdir -p ./workspace/readmes
	cp ./skeleton/BASE/README.txt ./workspace/readmes/BASE-in.txt
	cp ./skeleton/EXTRAS/README.txt ./workspace/readmes/EXTRAS-in.txt
	
done:
	say "done" 2>/dev/null || true

special:
	# tg5040 (TrimUI Brick / Smart Pro): set up the trimui .tmp_update bootstrap only
	mv ./build/BOOT/common ./build/BOOT/.tmp_update
	mv ./build/BOOT/trimui ./build/BASE/
	cp -R ./build/BOOT/.tmp_update ./build/BASE/trimui/app/

tidy:
	# ----------------------------------------------------
	# copy update from merged platform to old pre-merge platform bin so old cards update properly
ifneq (,$(findstring tg5040, $(PLATFORMS)))
	mkdir -p ./build/SYSTEM/tg3040/paks/MinUI.pak/
	cp ./build/SYSTEM/tg5040/bin/install.sh ./build/SYSTEM/tg3040/paks/MinUI.pak/launch.sh
endif

package: tidy
	# ----------------------------------------------------
	# zip up build
		
	# move formatted readmes from workspace to build
	cp ./workspace/readmes/BASE-out.txt ./build/BASE/README.txt
	cp ./workspace/readmes/EXTRAS-out.txt ./build/EXTRAS/README.txt
	rm -rf ./workspace/readmes
	
	cd ./build/SYSTEM && echo "$(ZERO_VERSION) ($(RELEASE_TIME)-$(RELEASE_DOT))\n$(BUILD_HASH)" > version.txt
	./commits.sh > ./build/SYSTEM/commits.txt
	cd ./build && find . -type f -name '.DS_Store' -delete
	mkdir -p ./build/PAYLOAD
	mv ./build/SYSTEM ./build/PAYLOAD/.system
	cp -R ./build/BOOT/.tmp_update ./build/PAYLOAD/
	# Tools ship INSIDE the updater payload too: existing users update by dropping
	# MinUI.zip alone (per README), and tool fixes must reach them (audit 2026-07-11 —
	# v1.3's Optimize CPU fixes would otherwise never reach v1.2 cards).
	cp -R ./build/EXTRAS/Tools ./build/PAYLOAD/Tools

	cd ./build/PAYLOAD && zip -r MinUI.zip .system .tmp_update Tools
	mv ./build/PAYLOAD/MinUI.zip ./build/BASE
	
	# v1: ONE download. Base is the whole product — 6 systems shown, extra systems dormant in
	# .system (add a Roms folder to unlock), and the 4 curated Tools. No extras zip to maintain.
	cp -R ./build/EXTRAS/Tools ./build/BASE/Tools
	# license compliance (audit 2026-07-11): GPL'd cores ship as binaries, so their license
	# texts, the fork's own terms, and a corresponding-source statement travel in the artifact.
	mkdir -p ./build/BASE/LICENSES
	cp LICENSE.md THIRD_PARTY_NOTICES.md ./build/BASE/LICENSES/
	for plat in $(PLATFORMS); do \
		for n in $(LICENSE_CORES); do \
			d=./workspace/$$plat/cores/src/$$n/; \
			for f in COPYING Copying COPYING.LIB copyright COPYRIGHT LICENSE LICENSE.MD LICENSE.md LICENSE.txt; do \
				if [ -f "$$d$$f" ]; then mkdir -p ./build/BASE/LICENSES/$$n && cp "$$d$$f" ./build/BASE/LICENSES/$$n/; fi; \
			done; \
		done; \
	done; true
	mkdir -p ./build/BASE/LICENSES/unzip60
	cp ./workspace/tg5040/other/unzip60/LICENSE ./build/BASE/LICENSES/unzip60/
	printf 'Corresponding source\n====================\nMinUI Zero source: https://github.com/danklammer/MinUI-Zero\nThe exact MinUI Zero commit is recorded in MinUI.zip/.system/version.txt. Emulator cores\nare built from the upstream repositories and exact commits pinned in\nworkspace/<platform>/cores/makefile at that commit; local modifications ship as patches in\nworkspace/<platform>/cores/patches/. Each core binary remains under its own license\n(texts in this folder).\n' > ./build/BASE/LICENSES/SOURCES.txt
	@if [ -e ./releases/$(RELEASE_NAME)-base.zip ]; then echo "ERROR: ./releases/$(RELEASE_NAME)-base.zip already exists — refusing to overwrite a release"; exit 1; fi
	cd ./build/BASE && zip -r ../../releases/$(RELEASE_NAME)-base.zip Bios Roms Saves Tools LICENSES trimui MinUI.zip README.txt .metadata_never_index .fseventsd
	echo "$(RELEASE_NAME)" > ./build/latest.txt
	
###########################################################

.DEFAULT:
	# ----------------------------------------------------
	# $@
	# a bare platform target (eg. `make tg5040`) runs the full release chain — without
	# setup/package it died at final packaging with nothing staged (Codex audit 2026-07-09)
	@echo "$(PLATFORMS)" | grep -q "\b$@\b" && (make setup && make common PLATFORM=$@ && make special && make package && make done) || (exit 1)
	
