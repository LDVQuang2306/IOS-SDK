#pragma once

#import <UIKit/UIKit.h>
#import <MetalKit/MetalKit.h>
#import <Foundation/Foundation.h>

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

// // // // // // // // // // // // // // // // // // //

#define isIPad (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad)
#define isIPhone (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone)

// // // // // // // // // // // // // // // // // // //

#define SCREEN_WIDTH [UIScreen mainScreen].bounds.size.width
#define SCREEN_HEIGHT [UIScreen mainScreen].bounds.size.height
#define SCREEN_SCALE [UIScreen mainScreen].scale

// // // // // // // // // // // // // // // // // // //

#define CallAfterSeconds(sec) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, sec * NSEC_PER_SEC), dispatch_get_main_queue(), ^

#define once(block) \
 static dispatch_once_t onceToken; \
 dispatch_once(&onceToken, ^{ block });

// // // // // // // // // // // // // // // // // // //

// #define PI 3.14159265358979323846
#define DEGTORAD 3.14159265358979323846f / 180.0f
#define RADTODEG 180.0f / 3.14159265358979323846f

#define FORCEINLINE inline __attribute__((always_inline))
#define FORCENOINLINE __attribute__((noinline))
#define RESTRICT __restrict

#define ENTRY_POINT __attribute__((constructor(101)))

#define INVOKE(fn) [&] { fn; }

// // // // // // // // // // // // // // // // // // //

#define ul unsigned long
#define uchar_t unsigned char
#define GUI_TITLE "menu"

// // // // // // // // // // // // // // // // // // //

#define RUN_ONCE(...) \
    do { \
        static bool __has_run_once__ = false; \
        if (!__has_run_once__) { \
            __VA_ARGS__; \
            __has_run_once__ = true; \
        } \
    } while (false)

