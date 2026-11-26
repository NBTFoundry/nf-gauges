# SPDX-License-Identifier: MPL-2.0
# Gauges for NBT Manager. Targets:
#   host              build/nbt-gauges-host (X11), the unit tests, the screenshot tool
#   test              unit tests, then the Python tests (fake gateway, fake bridge)
#   test-asan         the same under AddressSanitizer and UBSan
#   check-static-init no static constructors in our objects
#   shots             every page as a PNG (build/shots), with the simulated drive
#   anims             every page as a looping animated WebP (docs/screens), ffmpeg
#   qnx               build/nbt-gauges-qnx for the head unit (NBT toolchain, no QNX SDP)
#   qnx-check         its imports exist on the car (QNX 6.5.0 SP1)
#   vm-test           its health check in the QNX 6.5 SP1 VM (LVGL, one frame)
#   package           the signed package (APP_KEY, APP_VERSION, APP_SEQ)
SHELL := /bin/bash

BUILD_DIR ?= build
OBJ_DIR := $(BUILD_DIR)/obj
GEN_DIR := $(BUILD_DIR)/gen

APP_VERSION ?=
APP_SEQ ?=
APP_KEY ?=
NBT_SDK_DIR ?= ../sdk
NBT_MGR_DIR ?= ../manager
CXX ?= g++
CC ?= gcc
PYTHON ?= python3

NBT_SDK_BUILD_DIR := $(abspath $(BUILD_DIR))/sdk
include $(NBT_SDK_DIR)/nbt-sdk.mk
ifeq ($(value nbt_stamp),)
$(error $(NBT_SDK_ROOT) has no nbt-stamp.mk: update that SDK checkout)
endif
stamp = $(OBJ_DIR)/$(1).stamp

# --- LVGL (third_party/lvgl, MIT): its own source list, without its demos and
# examples. Its C++ sources (ThorVG) and assembly are for features left off in
# src/lv_conf.h. ---
LVGL_PATH := $(abspath third_party/lvgl)
lvgl_saved_cflags := $(CFLAGS)
lvgl_saved_cxxflags := $(CXXFLAGS)
include $(LVGL_PATH)/lvgl.mk
LVGL_SRC := $(sort $(filter $(LVGL_PATH)/src/%,$(CSRCS)))
CFLAGS := $(lvgl_saved_cflags)
CXXFLAGS := $(lvgl_saved_cxxflags)
lvgl_objs = $(patsubst $(LVGL_PATH)/%.c,$(OBJ_DIR)/$(1)/lvgl/%.o,$(LVGL_SRC))
LVGL_DEFS := -DLV_CONF_INCLUDE_SIMPLE -Isrc -I$(LVGL_PATH)
# Montserrat Medium as LVGL ships it (SIL OFL 1.1), for Tiny TTF.
FONT_TTF := $(LVGL_PATH)/scripts/generators/built_in_font/Montserrat-Medium.ttf

# --- Our sources ---
CORE_SRC := src/channels.cpp src/units.cpp src/obd.cpp src/hsfz.cpp src/hsfz_link.cpp src/defs.cpp \
	src/sources.cpp src/dme.cpp src/car_art.cpp src/plot.cpp
SCREEN_SRC := src/ui.cpp src/display_gl.cpp
APP_SRC := $(CORE_SRC) $(SCREEN_SRC) src/main.cpp
GEN_SRC := $(GEN_DIR)/font.c $(GEN_DIR)/version.c
objs = $(patsubst %.cpp,$(OBJ_DIR)/$(1)/%.o,$(2))
gen_objs = $(patsubst $(GEN_DIR)/%.c,$(OBJ_DIR)/$(1)/gen/%.o,$(GEN_SRC))

APP_INC := $(NBT_SDK_INC) -Isrc -isystem $(LVGL_PATH) -DLV_CONF_INCLUDE_SIMPLE
APP_CXXFLAGS := -std=c++11 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti $(CXXFLAGS)
ASAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
HOST_COMPILE = $(CXX) $(APP_INC) $(APP_CXXFLAGS)
ASAN_COMPILE = $(HOST_COMPILE) $(ASAN_FLAGS)
HOST_C_COMPILE = $(CC) -std=gnu99 -O2 -w $(LVGL_DEFS) $(CFLAGS)
QNX_COMPILE = $(NBT_QNX_CXX) $(NBT_QNX_CXXFLAGS) $(NBT_QNX_SDK_INC) $(APP_INC) $(APP_CXXFLAGS)
QNX_C_COMPILE = $(NBT_QNX_CC) $(NBT_QNX_CFLAGS) -std=gnu99 -O2 -w $(LVGL_DEFS)

HOST_APP := $(BUILD_DIR)/nbt-gauges-host
ASAN_APP := $(BUILD_DIR)/nbt-gauges-asan
UNIT_TEST := $(BUILD_DIR)/gauges-test
ASAN_TEST := $(BUILD_DIR)/gauges-test-asan
SHOT := $(BUILD_DIR)/gauges-shot
QNX_APP := $(BUILD_DIR)/nbt-gauges-qnx
HOST_LDLIBS = $(NBT_SDK_HOST_LDLIBS) -lm -lpthread

.PHONY: all host test test-asan check-static-init shots anims format qnx qnx-check vm-test package \
	package-check notice clean

all: host test

# --- Generated sources ---
$(GEN_DIR)/font.c: $(FONT_TTF) tools/embed.py
	$(PYTHON) tools/embed.py bytes $< gauges_font_ttf $@

GIT_VERSION := $(shell git describe --always --dirty --tags 2>/dev/null || echo unknown)
$(GEN_DIR)/version.c: FORCE
	@$(PYTHON) tools/embed.py text "$(if $(APP_VERSION),$(APP_VERSION) ($(GIT_VERSION)),$(GIT_VERSION))" \
		gauges_version $@
FORCE:

# --- Object rules, per kind ---
$(OBJ_DIR)/host/%.o: %.cpp $(call stamp,host)
	@mkdir -p $(dir $@)
	$(HOST_COMPILE) -MMD -MP -c $< -o $@

$(OBJ_DIR)/asan/%.o: %.cpp $(call stamp,asan)
	@mkdir -p $(dir $@)
	$(ASAN_COMPILE) -MMD -MP -c $< -o $@

$(OBJ_DIR)/qnx/%.o: %.cpp $(call stamp,qnx)
	@mkdir -p $(dir $@)
	$(QNX_COMPILE) -MMD -MP -c $< -o $@

$(OBJ_DIR)/host/lvgl/%.o: $(LVGL_PATH)/%.c $(call stamp,host-c)
	@mkdir -p $(dir $@)
	@$(HOST_C_COMPILE) -MMD -MP -c $< -o $@

$(OBJ_DIR)/qnx/lvgl/%.o: $(LVGL_PATH)/%.c $(call stamp,qnx-c)
	@mkdir -p $(dir $@)
	@$(QNX_C_COMPILE) -MMD -MP -c $< -o $@

$(OBJ_DIR)/host/gen/%.o: $(GEN_DIR)/%.c $(call stamp,host-c)
	@mkdir -p $(dir $@)
	$(HOST_C_COMPILE) -c $< -o $@

$(OBJ_DIR)/qnx/gen/%.o: $(GEN_DIR)/%.c $(call stamp,qnx-c)
	@mkdir -p $(dir $@)
	$(QNX_C_COMPILE) -c $< -o $@

# LVGL as a library per kind (the ASan build links the host one).
$(BUILD_DIR)/liblvgl-host.a: $(call lvgl_objs,host)
	@rm -f $@
	@ar rcs $@ $^
	@echo "liblvgl-host.a: $(words $^) objects"

$(BUILD_DIR)/liblvgl-qnx.a: $(call lvgl_objs,qnx)
	@rm -f $@
	@$(NBT_LLVM_BIN)/llvm-ar rcs $@ $^
	@echo "liblvgl-qnx.a: $(words $^) objects"

# --- Host (Linux, X11) ---
$(HOST_APP): $(call objs,host,$(APP_SRC)) $(call gen_objs,host) $(BUILD_DIR)/liblvgl-host.a $(NBT_SDK_HOST_LIBS)
	$(CXX) $^ $(LDFLAGS) $(HOST_LDLIBS) -o $@

$(ASAN_APP): $(call objs,asan,$(APP_SRC)) $(call gen_objs,host) $(BUILD_DIR)/liblvgl-host.a $(NBT_SDK_ASAN_LIBS)
	$(CXX) $(ASAN_FLAGS) $^ $(LDFLAGS) $(HOST_LDLIBS) -o $@

$(UNIT_TEST): $(call objs,host,tests/gauges_test.cpp $(CORE_SRC)) $(NBT_SDK_HOST_LIBS)
	$(CXX) $^ $(LDFLAGS) $(HOST_LDLIBS) -o $@

$(ASAN_TEST): $(call objs,asan,tests/gauges_test.cpp $(CORE_SRC)) $(NBT_SDK_ASAN_LIBS)
	$(CXX) $(ASAN_FLAGS) $^ $(LDFLAGS) $(HOST_LDLIBS) -o $@

$(SHOT): $(call objs,host,src/shot.cpp $(CORE_SRC) $(SCREEN_SRC)) $(call gen_objs,host) \
		$(BUILD_DIR)/liblvgl-host.a $(NBT_SDK_HOST_LIBS)
	$(CXX) $^ $(LDFLAGS) $(HOST_LDLIBS) -o $@

host: $(HOST_APP) $(UNIT_TEST) $(SHOT)

test: host
	$(UNIT_TEST)
	NBT_GAUGES_APP=$(abspath $(HOST_APP)) NBT_MGR_DIR=$(abspath $(NBT_MGR_DIR)) \
		$(PYTHON) -m unittest discover -s tests -v

test-asan: $(ASAN_APP) $(ASAN_TEST)
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(ASAN_TEST)
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		NBT_GAUGES_APP=$(abspath $(ASAN_APP)) $(PYTHON) -m unittest discover -s tests -v -k Gateway -k Health

check-static-init: $(call objs,host,$(APP_SRC))
	@set -e; \
	for obj in $^; do \
		ctors="$$(readelf -S "$$obj" | awk '/\.init_array|\.ctors/ {print $$2}')"; \
		if test -n "$$ctors"; then echo "ERROR: static constructor section in $$obj: $$ctors"; exit 1; fi; \
	done; \
	echo "OK: no .init_array/.ctors in our objects"

shots: $(SHOT)
	@mkdir -p $(BUILD_DIR)/shots
	$(SHOT) $(BUILD_DIR)/shots

# The README's animations: each page as a seamless loop, lossless animated WebP
# (ffmpeg with libwebp).
ANIM_PAGES := engine fuel-air temperatures ignition dpf transmission more connection
ANIM_FPS ?= 15
anims: $(SHOT)
	@set -eo pipefail; for page in $(ANIM_PAGES); do \
		$(SHOT) --frames $$page 1280 480 $(ANIM_FPS) | \
		ffmpeg -hide_banner -loglevel error -y -f rawvideo -pixel_format bgr0 \
			-video_size 1280x480 -framerate $(ANIM_FPS) -i - \
			-c:v libwebp_anim -lossless 1 -compression_level 6 -loop 0 docs/screens/$$page.webp; \
	done

format:
	clang-format -i src/*.cpp src/*.hpp tests/*.cpp

# --- The head unit: an ordinary QNX 6.5 program built with the NBT
# toolchain's QNX target (no QNX SDP; "make -C ../sdk toolchain" once). ---
$(QNX_APP)-unstripped: $(call objs,qnx,$(APP_SRC)) $(call gen_objs,qnx) $(BUILD_DIR)/liblvgl-qnx.a $(NBT_SDK_QNX_LIBS)
	$(call nbt_qnx_link,$@,$(filter %.o %.a,$^),$(NBT_SDK_QNX_LDLIBS))

$(QNX_APP): $(QNX_APP)-unstripped
	$(NBT_LLVM_BIN)/llvm-strip -o $@ $<

qnx: $(QNX_APP)

# The car runs QNX 6.5.0 SP1: the program's imports must exist there; libKD
# calls its kdMain.
QNX65_SP1_SYSROOT ?= $(HOME)/src/tools/qnx-6.5sp1-tools/sdk/target/qnx6
qnx-check: qnx
	@$(NBT_LLVM_BIN)/llvm-nm -D --defined-only $(QNX_APP)-unstripped | grep -qw kdMain \
		|| { echo "$(QNX_APP): kdMain is not exported"; exit 1; }
	@if test -d "$(QNX65_SP1_SYSROOT)"; then \
		$(PYTHON) $(NBT_SDK_ROOT)/tools/check_qnx_imports.py --sysroot "$(QNX65_SP1_SYSROOT)" \
			--readelf $(NBT_MOD_READELF) $(QNX_APP)-unstripped; \
	else echo "skipped: no QNX 6.5.0 SP1 sysroot at $(QNX65_SP1_SYSROOT)"; fi

# The health check in the QNX 6.5 SP1 VM (no display there): LVGL starts,
# the screens are built and one frame is drawn off screen, then ready.
VM_DIR := $(BUILD_DIR)/vm
vm-test: $(QNX_APP)
	@mkdir -p $(VM_DIR)
	cp $(QNX_APP) $(VM_DIR)/nbt-gauges
	QEMU_RUN_SECS=60 $(NBT_SDK_VM_RUN) \
		-c "NBT_LAUNCH=check NBT_DATA_DIR=/dev/shmem nbt-gauges &" -c "waitfor /none 10" \
		-c "slay -s TERM nbt-gauges" -c "waitfor /none 2" \
		-x 'screens ready, 1280x480' \
		$(VM_DIR)/nbt-gauges

# --- The signed package ---
PACKAGE_INPUT ?= $(QNX_APP)
PACKAGE := $(BUILD_DIR)/gauges-$(APP_VERSION).nbt
PACKAGE_ARGS := release/package.args

package: $(PACKAGE_INPUT)
	@test -f "$(APP_KEY)" || { echo "APP_KEY must name the RSA signing key"; exit 1; }
	@test -f "$(NBT_MGR_DIR)/tools/package.py" || { echo "NBT_MGR_DIR=$(NBT_MGR_DIR) has no tools/package.py"; exit 1; }
	@test -n "$(APP_VERSION)" -a -n "$(APP_SEQ)" || { echo "APP_VERSION and APP_SEQ must be given"; exit 1; }
	rm -f $(PACKAGE)
	$(PYTHON) $(NBT_MGR_DIR)/tools/package.py @$(PACKAGE_ARGS) \
		--version "$(APP_VERSION)" --sequence "$(APP_SEQ)" --key "$(APP_KEY)" \
		--output $(PACKAGE) $(PACKAGE_INPUT)

notice:
	NBT_SDK_DIR=$(NBT_SDK_ROOT) $(PYTHON) tools/make_notice.py

clean:
	rm -rf $(BUILD_DIR)

DEPS := $(shell find $(OBJ_DIR) -name '*.d' 2>/dev/null)
-include $(DEPS)

$(call nbt_stamp,$(call stamp,host),$(HOST_COMPILE),$(CXX))
$(call nbt_stamp,$(call stamp,asan),$(ASAN_COMPILE),$(CXX))
$(call nbt_stamp,$(call stamp,host-c),$(HOST_C_COMPILE),$(CC))
$(call nbt_stamp,$(call stamp,qnx),$(QNX_COMPILE),,$(NBT_TOOLCHAIN)/VERSION)
$(call nbt_stamp,$(call stamp,qnx-c),$(QNX_C_COMPILE),,$(NBT_TOOLCHAIN)/VERSION)
