# SPDX-License-Identifier: Apache-2.0
SDK_DEMO_PATH := $(abspath .)
BOARD := bl618dgdk
RADIO ?= wifi
ifeq ($(BOARD),bl618dg_orion)
BL_SDK_BASE ?= $(abspath ../../../../../bouffalo_sdk)
BUILD_DIR ?= $(if $(filter 1,$(HOST_PORT)),build-orion-host-$(RADIO)$(if $(strip $(LCD_CONFIG)),-lcd),$(if $(filter 1,$(AUDIO_TEST)),build-orion-audio,$(if $(strip $(DISPLAY_CONFIG)),build-orion-display,build-orion)))
cmake_definition += -DMPX_SDK_BOARD=$(BOARD)
ifneq ($(strip $(DISPLAY_GPIO_SOURCE)),)
cmake_definition += -DMPX_DISPLAY_GPIO_SOURCE="$(abspath $(DISPLAY_GPIO_SOURCE))"
endif
else ifneq ($(strip $(DISPLAY_CONFIG) $(DISPLAY_GPIO_SOURCE)),)
$(error Display configuration requires BOARD=bl618dg_orion)
endif
ifeq ($(AUDIO_TEST),1)
ifneq ($(BOARD),bl618dg_orion)
$(error AUDIO_TEST=1 requires BOARD=bl618dg_orion)
endif
ifneq ($(strip $(DISPLAY_CONFIG)),)
$(error Select AUDIO_TEST or DISPLAY_CONFIG for this build)
endif
cmake_definition += -DMPX_AUDIO_TEST=ON
endif
ifeq ($(HOST_PORT),1)
ifeq ($(filter $(RADIO),ble wifi dual),)
$(error RADIO must be ble, wifi or dual)
endif
ifneq ($(BOARD),bl618dg_orion)
$(error HOST_PORT=1 requires BOARD=bl618dg_orion)
endif
ifneq ($(strip $(filter 1,$(AUDIO_TEST)) $(DISPLAY_CONFIG)),)
$(error Select one of HOST_PORT, AUDIO_TEST or DISPLAY_CONFIG)
endif
cmake_definition += -DMPX_HOST_PORT=ON
ORION_RUNTIME_OPT ?= -O2
ORION_INTERPRETER_OPT ?= $(ORION_RUNTIME_OPT)
ORION_GUEST_PROFILE ?= release
cmake_definition += -DMPX_ORION_RUNTIME_OPT=$(ORION_RUNTIME_OPT)
cmake_definition += -DMPX_ORION_INTERPRETER_OPT=$(ORION_INTERPRETER_OPT)
cmake_definition += -DMPX_ORION_GUEST_PROFILE=$(ORION_GUEST_PROFILE)
ifneq ($(strip $(ORION_FONT_SOURCE)),)
cmake_definition += -DMPX_ORION_FONT_SOURCE="$(abspath $(ORION_FONT_SOURCE))"
endif
endif
ifneq ($(strip $(LCD_CONFIG)),)
ifneq ($(HOST_PORT),1)
$(error LCD_CONFIG requires HOST_PORT=1 BOARD=bl618dg_orion)
endif
cmake_definition += -DMPX_LCD_CONFIG="$(abspath $(LCD_CONFIG))"
endif
ifndef BL_SDK_BASE
$(error Set BL_SDK_BASE to the SDK prepared by firmware/bouffalo/tools/prepare_sdk.py)
endif
CHIP := bl618dg
CPU_ID := ap
CPU_MODEL := b0
BUILD_DIR ?= build
CROSS_COMPILE ?= riscv64-zephyr-elf-
export BL_SDK_BASE
include $(BL_SDK_BASE)/project.build
