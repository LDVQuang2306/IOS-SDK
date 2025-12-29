//
//  Logger.h
//  Dumper

#pragma once

// Global logging functions - safe to call from any thread
void LogInfo(const char* fmt, ...);
void LogError(const char* fmt, ...);
void LogSuccess(const char* fmt, ...); // Green text for success messages
