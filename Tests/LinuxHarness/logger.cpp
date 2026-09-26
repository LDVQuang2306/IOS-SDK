#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* fake_world.cpp: simulates the game garbage collecting objects while the dump runs (DF_HARNESS_UNLOAD=<every Nth object>) */
void HarnessUnloadObjects(int EveryNth);

static void Log(const char* Prefix, const char* Fmt, va_list Args)
{
	char Buffer[4096];
	vsnprintf(Buffer, sizeof(Buffer), Fmt, Args);
	printf("%s%s\n", Prefix, Buffer);
	fflush(stdout);

	/* Right after the generator took its snapshot of the object list, or (_LATE) while the SDK files are written */
	if (const char* Every = getenv("DF_HARNESS_UNLOAD"); Every && strncmp(Buffer, "ObjectArray: snapshot", 21) == 0)
		HarnessUnloadObjects(atoi(Every));

	if (const char* Every = getenv("DF_HARNESS_UNLOAD_LATE"); Every && strncmp(Buffer, "Internal Generator initialized", 30) == 0)
		HarnessUnloadObjects(atoi(Every));
}

void LogInfo(const char* Fmt, ...) { va_list A; va_start(A, Fmt); Log("[I] ", Fmt, A); va_end(A); }
void LogError(const char* Fmt, ...) { va_list A; va_start(A, Fmt); Log("[E] ", Fmt, A); va_end(A); }
void LogSuccess(const char* Fmt, ...) { va_list A; va_start(A, Fmt); Log("[S] ", Fmt, A); va_end(A); }
