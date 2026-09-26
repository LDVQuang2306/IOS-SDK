# Builds a generated Dumper-7 CppSDK for arm64 iOS with theos, to check that the SDK compiles
# (every struct/class has static_asserts for its size and member offsets, so a wrong layout is a compile error).
#
#   make SDK_DIR=/path/to/1.203.37117_65-DeltaForce/CppSDK             SDK core: Basic.cpp + CoreUObject/Engine functions
#   make SDK_DIR=/path/to/1.203.37117_65-DeltaForce/CppSDK SDK_ALL=1   every <Package>_functions.cpp of the SDK (takes a while)
#
# SDK_DIR has to contain SDK.hpp and the SDK/ folder. Add -j8 to build in parallel.

ARCHS = arm64
TARGET = iphone:clang:latest:14.0
DEBUG = 0
FINALPACKAGE = 1

# The SDK has thousands of harmless warnings (offsetof on non standard-layout types, ...), don't turn them into errors
GO_EASY_ON_ME = 1

include $(THEOS)/makefiles/common.mk

SDK_DIR ?= CppSDK

ifeq ($(filter clean,$(MAKECMDGOALS)),)
ifeq ($(wildcard $(SDK_DIR)/SDK/Basic.cpp),)
$(error SDK_DIR ('$(SDK_DIR)') must point to the CppSDK folder of a dump, e.g. make SDK_DIR=~/Downloads/1.203.37117_65-DeltaForce/CppSDK)
endif
endif

ifeq ($(SDK_ALL),1)
SDK_FILES = $(SDK_DIR)/SDK/Basic.cpp $(wildcard $(SDK_DIR)/SDK/*_functions.cpp)
else
SDK_FILES = $(SDK_DIR)/SDK/Basic.cpp $(SDK_DIR)/SDK/CoreUObject_functions.cpp $(wildcard $(SDK_DIR)/SDK/Engine_functions.cpp)
endif

LIBRARY_NAME = DFSDKTest

DFSDKTest_FILES = Tweak.mm $(SDK_FILES)
DFSDKTest_CFLAGS = -fobjc-arc -w -I$(SDK_DIR) -I$(SDK_DIR)/SDK
# -fno-modules (has to be in CCFLAGS, theos adds -fmodules after CFLAGS): with clang modules the whole libc++ 'std' module is built,
# which fails for an iOS 14 target (<format> is unavailable)
DFSDKTest_CCFLAGS = -std=gnu++20 -fno-modules
DFSDKTest_FRAMEWORKS = Foundation
DFSDKTest_INSTALL_PATH = /usr/lib

include $(THEOS_MAKE_PATH)/library.mk
