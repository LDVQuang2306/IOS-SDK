#include <cstdarg>
#include <cstdio>
static void Log(const char* Prefix, const char* Fmt, va_list Args) { char Buffer[4096]; vsnprintf(Buffer, sizeof(Buffer), Fmt, Args); printf("%s%s\n", Prefix, Buffer); fflush(stdout); }
void LogInfo(const char* Fmt, ...) { va_list A; va_start(A, Fmt); Log("[I] ", Fmt, A); va_end(A); }
void LogError(const char* Fmt, ...) { va_list A; va_start(A, Fmt); Log("[E] ", Fmt, A); va_end(A); }
void LogSuccess(const char* Fmt, ...) { va_list A; va_start(A, Fmt); Log("[S] ", Fmt, A); va_end(A); }
