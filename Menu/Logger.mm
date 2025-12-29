//
//  Logger.mm
//  Dumper
//
//  Created by Euclid Jan Guillermo on 12/8/25.
//

#include "Logger.h"
#include "Console.h" // Your Console class
#include <cstdarg>
#include <cstdio>
#include <string>

// Helper to format string and send to console
static void LogToConsole(int type, const char* fmt, va_list args) {
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    std::string msg(buffer);

    // Assuming Console follows Singleton pattern or global instance
    // Type 0 = Info/Default, 1 = Error, 2 = Success/Special
    if (type == 1) {
        Console::Get().logError(msg);
    } else if (type == 2) {
        Console::Get().logInfo(msg); // Reusing logInfo for "Success" style if available, or just log
    } else {
        Console::Get().log(msg);
    }
}

void LogInfo(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogToConsole(0, fmt, args);
    va_end(args);
}

void LogError(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogToConsole(1, fmt, args);
    va_end(args);
}

void LogSuccess(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogToConsole(2, fmt, args);
    va_end(args);
}
