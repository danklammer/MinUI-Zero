###########################################################

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
	$(error please specify PLATFORM, eg. PLATFORM=trimui make)
endif

###############################

# overrides shared by all cores

# commits after this one break save states (as of 20231025)
# gpsp_HASH = 1d1c719

###############################

# this logic was broken out from picoarch's all-in-one makefile

PATCH = git apply --unidiff-zero
# `?=` so a platform can dial this down before including this file. miyoomini does: its toolchain
# image is amd64-only, so on an arm64 host it runs emulated, and at -j4 gcc 8.3 intermittently
# SEGFAULTS mid-compile (seen on picodrive's tremor/mdct.c — the identical command succeeded
# serially straight afterwards). A build that randomly dies is not reproducible.
PROCS ?= -j4

###############################

BUILD_ROOT=../../$(PLATFORM)/cores

define TEMPLATE

$1_REPO ?= https://github.com/libretro/$(1)
# ISOLATE EVERY CORE FROM THE PARENT JOBSERVER. These are third-party makefiles that recurse, and
# several mishandle an inherited jobserver: the sub-make ignores our -jN and starts linking before
# its objects exist (picodrive: undefined lprintf/rfread/rfseek; fceumm the same on miyoomini). The
# giveaway is that they build perfectly standalone and via `make <core>`, and ONLY fail inside a
# full platform build. Clearing MAKEFLAGS makes $1_PROCS the single source of truth for how parallel
# a core is allowed to be (2026-08-14).
$1_MAKE ?= env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL make $(and $($1_MAKEFILE),-f $($1_MAKEFILE)) platform=$(PLATFORM) $($(1)_FLAGS)
$1_BUILD_PATH ?= $(1)
# Per-core parallelism. Needed because a core can recurse into a sub-make that mishandles the
# jobserver: picodrive's Cyclone M68K generator dies with "write jobserver: Bad file descriptor"
# under any -jN, deterministically. Set <core>_PROCS = -j1 for those.
$1_PROCS ?= $(PROCS)

src/$(1):
	mkdir -p src
	cd src && git clone $(if $($1_HASH),,--depth 1) --recursive $$($(1)_REPO) $(1)
	$(if $($1_HASH),cd src/$$($1_BUILD_PATH) && git checkout $($1_HASH) && echo $($1_HASH),)

src/$(1)/.patched: src/$(1)
	(test ! -f patches/$(1).patch) || (test -f src/$(1)/.patched) || (cd src/$(1) && $(PATCH) -p1 < ../../patches/$(1).patch && touch .patched && true)

src/$(1)/.patched-all:
	(test ! -d ../../all/cores/patches/$(1)) || (test -f src/$(1)/.patched-all) || (cd src/$(1) && $(foreach patch, $(sort $(wildcard ../../all/cores/patches/$(1)/*.patch)), $(PATCH) -p1 < ../../$(patch) &&) touch .patched-all && true)
	

.PHONY: verify-$(1)
verify-$(1): src/$(1)/.patched src/$(1)/.patched-all
	sh ../../all/cores/verify-source.sh src/$(1) "$(if $($1_HASH),$($1_HASH),HEAD)" $(if $(wildcard patches/$(1).patch),patches/$(1).patch) $(sort $(wildcard ../../all/cores/patches/$(1)/*.patch)) $(addprefix --ignore=,$($(1)_VERIFY_IGNORE))


output/$(1)_libretro.so: src/$(1)/.patched src/$(1)/.patched-all | verify-$(1)
	mkdir -p output
	cd src/$$($1_BUILD_PATH) && $$($1_MAKE) $$($1_PROCS)
	mv src/$$($1_BUILD_PATH)/$(if $($(1)_CORE),$($(1)_CORE),$(1)_libretro.so) ./output

clone-$(1): src/$(1)

status-$(1): src/$(1)
	cd src/$(1) && git show --oneline -s

patch-$(1): src/$(1)/.patched

clean-$(1):
	test ! -d src/$(1) || cd src/$$($1_BUILD_PATH) && $$($1_MAKE) clean
	rm -rf output/$(1)_libretro.so

$(1): output/$(1)_libretro.so

endef

###############################

all: cores

$(foreach CORE,$(CORES),$(eval $(call TEMPLATE,$(CORE))))

cores: $(foreach CORE,$(CORES),$(CORE))
	@sh ../../all/cores/check-cores.sh output $(foreach C,$(CORES),$(if $($(C)_CORE),$($(C)_CORE),$(C)_libretro.so))
	
clean: $(foreach CORE,$(CORES),clean-$(CORE))

verify: $(foreach CORE,$(CORES),verify-$(CORE))
	
nuke:
	rm -rf ./output
	rm -rf ./src
