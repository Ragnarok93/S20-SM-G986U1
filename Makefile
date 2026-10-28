# Evidence-backed Samsung profile metadata. This is NOT an executable payload profile.
SAMSUNG_PROJECT := samsung-g986u1-4.19.113
SAMSUNG_DEVICE := Samsung Galaxy S20+
SAMSUNG_MODEL := SM-G986U1
SAMSUNG_FIRMWARE := G986U1UESCHXL1
SAMSUNG_KERNEL_RELEASE := 4.19.113-27095354
SAMSUNG_ARCH := aarch64
SAMSUNG_ANDROID_API := 33
SAMSUNG_VA_BITS := 39
SAMSUNG_BOOT_IMAGE ?= $(HOME)/stock-boot.img
SAMSUNG_RAW_IMAGE ?= /tmp/g986u1-stock-kernel.Image
SAMSUNG_VENDOR_ELF ?= $(HOME)/firmware-check/vmlinux.elf
SAMSUNG_KERNEL_CONFIG ?= $(HOME)/firmware-check/kernel.config
SAMSUNG_KERNEL_SOURCE ?= $(HOME)/s20_kernel_chxl1
SAMSUNG_SOURCE_CONFIG ?= $(SAMSUNG_KERNEL_SOURCE)/out-stock-layout/.config
SAMSUNG_CONFIG_STRICT ?= 0
SAMSUNG_DEVICE_PROFILE := src/targets/$(SAMSUNG_PROJECT)/device-profile.json
ADB ?= adb
ADB_SERIAL ?=
UPSTREAM_GENERATOR_ROOT ?= /tmp/ghostlock-profile-gen

API ?= $(SAMSUNG_ANDROID_API)
PROJECT ?= $(SAMSUNG_PROJECT)
OUTDIR ?= build/$(PROJECT)/bin
EMBEDDIR ?= build/embed

TARGET_DIR := src/targets/$(PROJECT)
TARGET_HEADER := $(TARGET_DIR)/target.h
REQUESTED_GOALS := $(if $(strip $(MAKECMDGOALS)),$(MAKECMDGOALS),preload)
PAYLOAD_GOALS := all preload s2 s3 rootchain closed-loop hello-dso
UTILITY_GOALS := list-projects list-device-profiles clean info samsung-info samsung-inspect samsung-verify samsung-source-check samsung-config-compare samsung-test-tools samsung-generator-audit samsung-generator-verify samsung-generator-legacy-patch-check samsung-generator-legacy-patch samsung-generator-pe-stub-patch-check samsung-generator-pe-stub-patch samsung-generator-patches-check samsung-generator-patches samsung-debug-metadata samsung-device-check

ifeq ($(strip $(PROJECT)),)
ifneq ($(filter $(UTILITY_GOALS),$(MAKECMDGOALS)),)
# Utility goals do not select a target profile.
else
$(error PROJECT is required; run 'make list-projects' and then 'make PROJECT=<profile>')
endif
else ifeq ($(PROJECT),$(SAMSUNG_PROJECT))
ifeq ($(wildcard $(TARGET_HEADER)),)
ifneq ($(filter $(PAYLOAD_GOALS),$(REQUESTED_GOALS)),)
$(error $(SAMSUNG_PROJECT) is evidence-only: no verified target.h exists; refusing to build a device-specific payload. Use 'make samsung-info' or 'make samsung-verify'.)
endif
endif
else
ifeq ($(wildcard $(TARGET_HEADER)),)
$(error unknown PROJECT=$(PROJECT), missing $(TARGET_HEADER))
endif
endif


define pick_src
$(if $(wildcard $(TARGET_DIR)/$(1)),$(TARGET_DIR)/$(1),src/$(1))
endef

EMBED_SU := $(EMBEDDIR)/su_daemon_aarch64_pie
PRELOAD := $(OUTDIR)/preload.so
WALLPAPER := assets/wallpaper.webp

CORE_SRCS := \
  $(call pick_src,main.c) \
  $(call pick_src,util.c) \
  $(call pick_src,slide.c) \
  src/slide_s2.c \
  $(call pick_src,fops.c) \
  $(call pick_src,pipe.c) \
  src/root.c
PRELOAD_SRCS := $(CORE_SRCS) src/preload.c src/su_blob.S src/wallpaper_blob.S
S2_SRCS := src/s2_main.c src/slide_s2.c src/util.c src/s2_support.c \
	src/dso_start.S
S2_OUTDIR := build/alioth/bin
S2_PRELOAD := $(S2_OUTDIR)/preload_slide_h1a_s2.so
S2_EXPORT_MAP := src/s2_export.map
S2_NDK_ROOT ?= D:/codeTool/ndk/android-ndk-r26b/toolchains/llvm/prebuilt/windows-x86_64
S2_CC ?= $(S2_NDK_ROOT)/bin/aarch64-linux-android34-clang.cmd
S3_SRCS := src/s3_main.c src/slide_s3.c src/util.c src/s2_support.c \
	src/dso_start.S
S3_PRELOAD := $(S2_OUTDIR)/preload_slide_h1a_s3.so
ROOTCHAIN_SRCS := src/rootchain_bridge.c src/slide_s3.c src/util.c \
	src/s2_support.c src/fops.c src/pipe.c src/root.c src/preload.c \
	src/su_blob.S src/wallpaper_blob.S src/dso_start.S
ROOTCHAIN_PRELOAD := $(S2_OUTDIR)/preload_rootchain_bridge.so
CLOSED_LOOP_SRCS := src/closed_loop/phase_controller.c \
	src/closed_loop/binding_renderer.c src/closed_loop/bucket_answer.c \
	src/closed_loop/util.c src/rootchain_bridge.c src/slide_s3.c \
	src/s2_support.c src/fops.c src/pipe.c src/root.c src/preload.c \
	src/su_blob.S src/wallpaper_blob.S src/dso_start.S
CLOSED_LOOP_PRELOAD := $(S2_OUTDIR)/preload_closed_loop.so
HELLO_DSO := $(S2_OUTDIR)/hello_dso.so
HELLO_SRCS := src/hello_dso.c src/dso_start.S

.DEFAULT_GOAL := preload

DEFAULT_NDK_ROOT := $(HOME)/ndk/android-ndk-r27c
SDK_NDK_ROOT := $(HOME)/Android/Sdk/ndk/27.2.12479018
LEGACY_NDK_ROOT := $(HOME)/android-ndk-cache/android-ndk-r29
NDK_ROOT ?= $(or $(ANDROID_NDK_HOME),$(ANDROID_NDK_ROOT),$(wildcard $(DEFAULT_NDK_ROOT)),$(wildcard $(SDK_NDK_ROOT)),$(wildcard $(LEGACY_NDK_ROOT)))
NDK_TOOLCHAIN ?= $(if $(NDK_ROOT),$(NDK_ROOT)/toolchains/llvm/prebuilt/linux-x86_64)
NDK_CC := $(NDK_TOOLCHAIN)/bin/aarch64-linux-android$(API)-clang
HOST_CLANG ?= clang
SYSROOT ?= $(if $(NDK_TOOLCHAIN),$(NDK_TOOLCHAIN)/sysroot)
RESOURCE_DIR ?= $(if $(NDK_TOOLCHAIN),$(NDK_TOOLCHAIN)/lib/clang/21)

HOST_TARGET_FLAGS := \
  --target=aarch64-linux-android$(API) \
  --sysroot=$(SYSROOT) \
  -resource-dir $(RESOURCE_DIR) \
  --rtlib=compiler-rt \
  --unwindlib=none
HOST_COMMON_LDFLAGS := \
  -fuse-ld=lld \
  -Wl,-rpath-link,$(SYSROOT)/usr/lib/aarch64-linux-android/$(API) \
  -L$(SYSROOT)/usr/lib/aarch64-linux-android/$(API) \
  -L$(SYSROOT)/usr/lib/aarch64-linux-android
HOST_PIE_LDFLAGS := \
  $(HOST_COMMON_LDFLAGS) \
  -Wl,-dynamic-linker,/system/bin/linker64

ifneq ($(origin CC),default)
  TARGET_CC := $(CC)
  TARGET_FLAGS :=
  TARGET_COMMON_LDFLAGS :=
  TARGET_PIE_LDFLAGS :=
else ifneq ($(wildcard $(NDK_CC)),)
  NDK_CC_WORKS := $(shell $(NDK_CC) --version >/dev/null 2>&1 && echo yes)
  ifeq ($(NDK_CC_WORKS),yes)
    TARGET_CC := $(NDK_CC)
    TARGET_FLAGS :=
    TARGET_COMMON_LDFLAGS :=
    TARGET_PIE_LDFLAGS :=
  else
    TARGET_CC := $(HOST_CLANG)
    TARGET_FLAGS := $(HOST_TARGET_FLAGS)
    TARGET_COMMON_LDFLAGS := $(HOST_COMMON_LDFLAGS)
    TARGET_PIE_LDFLAGS := $(HOST_PIE_LDFLAGS)
  endif
else
  TARGET_CC := $(HOST_CLANG)
  TARGET_FLAGS := $(HOST_TARGET_FLAGS)
  TARGET_COMMON_LDFLAGS := $(HOST_COMMON_LDFLAGS)
  TARGET_PIE_LDFLAGS := $(HOST_PIE_LDFLAGS)
endif

COMMON_CFLAGS := -O2 -g0 -Wall -Wextra -Isrc
PIE_CFLAGS := -fPIE -pie $(COMMON_CFLAGS)
SO_CFLAGS := -fPIC $(COMMON_CFLAGS)
WARN_CFLAGS := -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function
TARGET_CFLAGS := -DTARGET_CONFIG_H=\"targets/$(PROJECT)/target.h\"

.PHONY: all preload s2 s3 rootchain closed-loop hello-dso clean info list-projects list-device-profiles samsung-info samsung-inspect samsung-verify samsung-config-compare samsung-generator-audit samsung-generator-verify samsung-generator-legacy-patch-check samsung-generator-legacy-patch samsung-generator-pe-stub-patch-check samsung-generator-pe-stub-patch samsung-generator-patches-check samsung-generator-patches samsung-debug-metadata samsung-device-check samsung-source-check samsung-test-tools

all: preload

preload: $(PRELOAD)

s2: $(S2_PRELOAD)

s3: $(S3_PRELOAD)

rootchain: $(ROOTCHAIN_PRELOAD)

closed-loop: $(CLOSED_LOOP_PRELOAD)

hello-dso: $(HELLO_DSO)

$(OUTDIR):
	mkdir -p $@

$(S2_OUTDIR):
	mkdir -p $@

$(EMBEDDIR):
	mkdir -p $@

$(EMBED_SU): src/su_daemon.c | $(EMBEDDIR)
	$(TARGET_CC) $(TARGET_FLAGS) $(PIE_CFLAGS) $(TARGET_CFLAGS) \
	  $< $(TARGET_PIE_LDFLAGS) -o $@

$(PRELOAD): $(PRELOAD_SRCS) $(EMBED_SU) $(WALLPAPER) $(TARGET_HEADER) src/offset.h src/common.h src/kernelsnitch/*.h | $(OUTDIR)
	$(TARGET_CC) $(TARGET_FLAGS) $(SO_CFLAGS) $(WARN_CFLAGS) $(TARGET_CFLAGS) \
	  $(PRELOAD_SRCS) $(TARGET_COMMON_LDFLAGS) \
	  -shared -o $@ -pthread
	sha256sum $@

$(S2_PRELOAD): $(S2_SRCS) $(S2_EXPORT_MAP) build_s2.rsp $(TARGET_HEADER) src/offset.h src/common.h src/kernelsnitch/*.h | $(S2_OUTDIR)
	cmd /c "$(S2_CC) @build_s2.rsp"
	certutil -hashfile $@ SHA256

$(S3_PRELOAD): $(S3_SRCS) $(S2_EXPORT_MAP) build_s3.rsp $(TARGET_HEADER) src/offset.h src/common.h src/kernelsnitch/*.h | $(S2_OUTDIR)
	cmd /c "$(S2_CC) @build_s3.rsp"
	certutil -hashfile $@ SHA256

$(ROOTCHAIN_PRELOAD): $(ROOTCHAIN_SRCS) $(EMBED_SU) $(WALLPAPER) \
	$(S2_EXPORT_MAP) build_rootchain.rsp src/rootchain_bridge.h $(TARGET_HEADER) \
	src/offset.h src/common.h src/kernelsnitch/*.h | $(S2_OUTDIR)
	cmd /c "$(S2_CC) @build_rootchain.rsp"
	certutil -hashfile $@ SHA256

$(CLOSED_LOOP_PRELOAD): $(CLOSED_LOOP_SRCS) $(S2_EXPORT_MAP) \
	build_closed_loop.rsp $(TARGET_HEADER) src/offset.h src/common.h \
	src/closed_loop/kernelsnitch/*.h src/kernelsnitch/*.h | $(S2_OUTDIR)
	cmd /c "$(S2_CC) @build_closed_loop.rsp"
	certutil -hashfile $@ SHA256

$(HELLO_DSO): $(HELLO_SRCS) $(S2_EXPORT_MAP) | $(S2_OUTDIR)
	cmd /c "$(S2_CC) -fPIC -shared -pthread -O2 -g0 -Wl,-e,_start -Wl,--version-script=$(S2_EXPORT_MAP) -o $@ $(HELLO_SRCS)"
	certutil -hashfile $@ SHA256

info:
	@echo "PROJECT=$(PROJECT)"
	@echo "PROFILE_STATUS=$(if $(filter $(SAMSUNG_PROJECT),$(PROJECT)),evidence-only,executable-profile)"
	@echo "ANDROID_API=$(API)"
	@echo "TARGET_DIR=$(TARGET_DIR)"
	@echo "TARGET_CC=$(TARGET_CC)"
	@echo "TARGET_FLAGS=$(TARGET_FLAGS)"
	@echo "TARGET_COMMON_LDFLAGS=$(TARGET_COMMON_LDFLAGS)"
	@echo "TARGET_PIE_LDFLAGS=$(TARGET_PIE_LDFLAGS)"
	@echo "PRELOAD=$(PRELOAD)"
	@echo "EMBED_SU=$(EMBED_SU)"
	@echo "WALLPAPER=$(WALLPAPER)"
	@echo "CORE_SRCS=$(CORE_SRCS)"

samsung-info:
	@echo "PROFILE_STATUS=evidence-only"
	@echo "PROJECT=$(SAMSUNG_PROJECT)"
	@echo "DEVICE=$(SAMSUNG_DEVICE)"
	@echo "MODEL=$(SAMSUNG_MODEL)"
	@echo "FIRMWARE=$(SAMSUNG_FIRMWARE)"
	@echo "KERNEL_RELEASE=$(SAMSUNG_KERNEL_RELEASE)"
	@echo "ARCH=$(SAMSUNG_ARCH)"
	@echo "ANDROID_API=$(SAMSUNG_ANDROID_API)"
	@echo "CONFIG_ARM64_VA_BITS_$(SAMSUNG_VA_BITS)=y"
	@echo "PROFILE_DOC=src/targets/$(SAMSUNG_PROJECT)/PROFILE.md"
	@echo "DEVICE_PROFILE=$(SAMSUNG_DEVICE_PROFILE)"
	@echo "TARGET_HEADER_STATUS=missing; device-specific payload build is intentionally blocked"

samsung-device-check:
	python3 tools/verify_samsung_device.py --profile "$(SAMSUNG_DEVICE_PROFILE)" --adb "$(ADB)" $(if $(strip $(ADB_SERIAL)),--serial "$(ADB_SERIAL)",)

samsung-source-check:
	python3 tools/verify_samsung_kernel_source.py --profile "$(SAMSUNG_DEVICE_PROFILE)" --source-root "$(SAMSUNG_KERNEL_SOURCE)"

samsung-config-compare:
	python3 tools/compare_samsung_kernel_configs.py --profile "$(SAMSUNG_DEVICE_PROFILE)" --stock-config "$(SAMSUNG_KERNEL_CONFIG)" --candidate-config "$(SAMSUNG_SOURCE_CONFIG)" --show-all-differences $(if $(filter 1 yes true,$(SAMSUNG_CONFIG_STRICT)),--strict-all,)

samsung-inspect:
	python3 tools/inspect_android_boot.py \
	  --boot "$(SAMSUNG_BOOT_IMAGE)" \
	  --image "$(SAMSUNG_RAW_IMAGE)" \
	  --config "$(SAMSUNG_KERNEL_CONFIG)" \
	  --elf "$(SAMSUNG_VENDOR_ELF)" \
	  --scan-btf

samsung-verify:
	python3 tools/verify_samsung_firmware.py --profile "$(SAMSUNG_DEVICE_PROFILE)"
	python3 tools/inspect_android_boot.py \
	  --boot "$(SAMSUNG_BOOT_IMAGE)" \
	  --image "$(SAMSUNG_RAW_IMAGE)" \
	  --config "$(SAMSUNG_KERNEL_CONFIG)" \
	  --elf "$(SAMSUNG_VENDOR_ELF)" \
	  --scan-btf
	python3 tools/inspect_elf_debug_metadata.py "$(SAMSUNG_VENDOR_ELF)" --strict-structure
	NDK_ROOT="$(NDK_ROOT)" API="$(SAMSUNG_ANDROID_API)" bash tools/check_android_abi.sh
	python3 -m unittest discover -s tests -v

samsung-test-tools:
	python3 -m unittest discover -s tests -v

samsung-generator-audit:
	python3 tools/audit_generator_compat.py \
	  --generator-root "$(UPSTREAM_GENERATOR_ROOT)" \
	  --boot "$(SAMSUNG_BOOT_IMAGE)"

# Exercise both vendored boot parsers in the external generator tree, first
# against synthetic layouts and then against this device's actual v2 boot
# image, requiring an exact raw-Image byte match. The static audit separately
# checks that unsupported layouts/fallback paths are not silently accepted.
samsung-generator-verify:
	python3 tools/verify_generator_boot_parser.py \
	  --generator-root "$(UPSTREAM_GENERATOR_ROOT)" \
	  --boot "$(SAMSUNG_BOOT_IMAGE)" \
	  --image "$(SAMSUNG_RAW_IMAGE)" \
	  --expect-version 2
	python3 tools/audit_generator_compat.py \
	  --generator-root "$(UPSTREAM_GENERATOR_ROOT)" \
	  --boot "$(SAMSUNG_BOOT_IMAGE)"

# Check/apply parser compatibility on a pristine external generator checkout.
# These targets never generate target.h; use --check before applying a patch.
samsung-generator-legacy-patch-check:
	python3 tools/patch_legacy_boot_v2.py --root "$(UPSTREAM_GENERATOR_ROOT)" --check

samsung-generator-legacy-patch:
	python3 tools/patch_legacy_boot_v2.py --root "$(UPSTREAM_GENERATOR_ROOT)"

samsung-generator-pe-stub-patch-check:
	python3 tools/patch_optional_arm64_pe_stub.py --root "$(UPSTREAM_GENERATOR_ROOT)" --check

samsung-generator-pe-stub-patch:
	python3 tools/patch_optional_arm64_pe_stub.py --root "$(UPSTREAM_GENERATOR_ROOT)"

# Combined parser preparation; check first, then apply only if source preflight passes.
samsung-generator-patches-check:
	python3 tools/patch_legacy_boot_v2.py --root "$(UPSTREAM_GENERATOR_ROOT)" --check
	python3 tools/patch_optional_arm64_pe_stub.py --root "$(UPSTREAM_GENERATOR_ROOT)" --check

samsung-generator-patches:
	python3 tools/patch_legacy_boot_v2.py --root "$(UPSTREAM_GENERATOR_ROOT)"
	python3 tools/patch_optional_arm64_pe_stub.py --root "$(UPSTREAM_GENERATOR_ROOT)"

samsung-debug-metadata:
	python3 tools/inspect_elf_debug_metadata.py "$(SAMSUNG_VENDOR_ELF)"

list-projects:
	@find src/targets -mindepth 2 -maxdepth 2 -name target.h -printf '%h\n' | sed 's#src/targets/##' | sort

# Device identity/compatibility profiles are discoverable separately from build-ready payload profiles.
list-device-profiles:
	@find src/targets -mindepth 2 -maxdepth 2 -name device-profile.json -printf '%h\n' | sed 's#src/targets/##' | sort

clean:
	rm -rf build
