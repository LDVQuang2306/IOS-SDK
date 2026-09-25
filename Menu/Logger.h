//
//  Logger.h
//  Dumper

#pragma once

// Global logging functions - safe to call from any thread. printf-style, the format is checked by the compiler.
void LogInfo(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void LogError(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void LogSuccess(const char* fmt, ...) __attribute__((format(printf, 1, 2))); // Green text for success messages
