#pragma once

// 1. IMPORT C++ HEADERS TRƯỚC
#ifdef __cplusplus
    #include <stdio.h>
    #include <vector>
    #include <map>
    #include <unistd.h>
    #include <string.h>
    #include <functional>
    #include <iostream>
    #include <queue>
    #include <codecvt>
    #include <mutex>
    #include <unordered_set>
    #include <concepts>
    #include <unordered_map>
    #include <regex>
    #include <array>
    #include <cmath>
#endif

// 2. SAU ĐÓ MỚI ĐẾN SYSTEM FRAMEWORKS
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <Security/Security.h>
#import <os/log.h>
#import <dlfcn.h>
#import <mach-o/dyld.h>
#import <mach/mach.h>
#import <pthread/pthread.h>

#include "../ImGui/imgui.h"
#include "../ImGui/imgui_internal.h"
#include "../ImGui/imgui_impl_metal.h"

#include "../Utils/Singleton.h"
#include "../Utils/Macros.h"

#include "ImGuiDrawView.h"
#include "MenuLoad.h"


inline UIButton* GInvisibleMenuButton = nullptr;
inline UIButton* GVisibleMenuButton = nullptr;
inline MenuInteraction* GMenuTouchView = nullptr;
inline UITextField* GHideRecordTextField = nullptr;
inline UIView* GHideRecordView = nullptr;

inline ImFont* GIconFont = nullptr;
inline ImFont* Font = nullptr;


template<int32_t Len>
struct StringLiteral
{
    char Chars[Len];

    consteval StringLiteral(const char(&String)[Len])
    {
        std::copy_n(String, Len, Chars);
    }

    operator std::string() const
    {
        return static_cast<const char*>(Chars);
    }
};

FORCEINLINE void CrashSafe()
{
    *(volatile int*)0 = 1;
    return;
}

template<typename To>
FORCEINLINE To* Cast(void* Src)
{
    return static_cast<To*>(Src);
}

template<typename To>
FORCEINLINE const To* Cast(const void* Src)
{
    return static_cast<const To*>(Src);
}

struct
{
    int Width;
    int Height;
    int HalfWidth;
    int HalfHeight;
    int Scale;
    
    void Init()
    {
        this->Height = [UIScreen mainScreen].bounds.size.height;
        this->Width = [UIScreen mainScreen].bounds.size.width;
        this->HalfHeight = this->Height / 2;
        this->HalfWidth = this->Width / 2;
        this->Scale = [UIScreen mainScreen].scale;
    }
    
}inline ScreenRect;
