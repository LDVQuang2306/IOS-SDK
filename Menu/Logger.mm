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
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>

/*
* Every log line is also appended to <App>/Documents/Dumper-7.log and flushed immediately,
* so the log survives if the game gets killed during a dump.
*/
static void LogToFile(int type, const char* msg) {
    static std::mutex FileMutex;
    static FILE* LogFile = nullptr;
    static bool bTriedOpening = false;

    std::lock_guard<std::mutex> lock(FileMutex);

    if (!bTriedOpening) {
        bTriedOpening = true;

        if (const char* Home = getenv("HOME")) {
            const std::string Path = std::string(Home) + "/Documents/Dumper-7.log";
            LogFile = fopen(Path.c_str(), "w");
        }
    }

    if (!LogFile)
        return;

    char TimeBuffer[32];
    time_t Now = time(nullptr);
    struct tm LocalTime;
    localtime_r(&Now, &LocalTime);
    strftime(TimeBuffer, sizeof(TimeBuffer), "%H:%M:%S", &LocalTime);

    fprintf(LogFile, "[%s] %s %s\n", TimeBuffer, type == 1 ? "[ERROR]" : (type == 2 ? "[OK]   " : "[INFO] "), msg);
    fflush(LogFile);
}

// Helper to format string and send to console
static void LogToConsole(int type, const char* fmt, va_list args) {
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    std::string msg(buffer);

    LogToFile(type, buffer);

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
