ARCHS = arm64
DEBUG = 0
FINALPACKAGE = 1
FOR_RELEASE = 1
IGNORE_WARNINGS = 1
TARGET = iphone:clang:latest:14.0

include $(THEOS)/makefiles/common.mk

# Macro select mode: 1 for tweak, 2 for framework
BUILD_MODE = 1

COMMON_NAME = LDVQuangDumper

$(COMMON_NAME)_FRAMEWORKS = Metal MetalKit UIKit Foundation Security QuartzCore CoreGraphics ImageIO CoreText AVFoundation Accelerate GLKit SystemConfiguration GameController IOKit AudioToolbox

$(COMMON_NAME)_CFLAGS = -fobjc-arc -Wall -Wno-deprecated-declarations -Wno-unused-variable -Wno-unused-value -Wno-unused-function

$(COMMON_NAME)_CCFLAGS = -w -std=gnu++20 -DkNO_KEYSTONE -DkNO_SUBSTRATE -DRPM_USE_MEMCPY -fno-rtti -fno-exceptions -fexceptions -DNDEBUG -fvisibility=hidden -fvisibility-inlines-hidden -fno-unwind-tables -fno-asynchronous-unwind-tables -I$(THEOS_PROJECT_DIR)/fmt -fno-modules -fcxx-exceptions

$(COMMON_NAME)_CXXFLAGS += -fexceptions -I$(THEOS_PROJECT_DIR)/fmt -fno-modules

$(COMMON_NAME)_CFLAGS   += -fexceptions -I$(THEOS_PROJECT_DIR)/fmt -fmodules -fcxx-modules -Wno-module-import-in-extern-c

IMGUI_LIB = main.mm $(wildcard Menu/*.mm) $(wildcard MenuLoad/*.mm) $(wildcard ImGui/*.cpp) $(wildcard ImGui/*.mm)
UTILS_LIB = $(wildcard Utils/Compression/*.c) $(wildcard Utils/Dumpspace/*.cpp) $(wildcard Utils/*.cpp)
GENERATOR_LIB = $(wildcard Generator/Private/*.cpp) $(wildcard Generator/Private/Generators/*.cpp) $(wildcard Generator/Private/Managers/*.cpp) $(wildcard Generator/Private/Wrappers/*.cpp)
ENGINE_LIB = $(wildcard Engine/Private/OffsetFinder/*.cpp) $(wildcard Engine/Private/Unreal/*.cpp)

$(COMMON_NAME)_FILES = $(IMGUI_LIB) $(UTILS_LIB) $(GENERATOR_LIB) $(ENGINE_LIB) fmt/format.cc

# https://t.me/quangmodmap (contact me if you have a question)
ifeq ($(BUILD_MODE),1)
    TWEAK_NAME = $(COMMON_NAME)
    include $(THEOS_MAKE_PATH)/tweak.mk
else ifeq ($(BUILD_MODE),2)
    FRAMEWORK_NAME = $(COMMON_NAME)
    include $(THEOS_MAKE_PATH)/framework.mk
else
    $(error BUILD_MODE phải là 1 (tweak) hoặc 2 (framework))
endif
