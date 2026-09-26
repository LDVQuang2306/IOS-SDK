#include <iostream>
#include <atomic>
#include <chrono>
#include <exception>
#include <fstream>
#include <cstdio>
#include <pthread.h>

#include "Generator/Public/Generators/CppGenerator.h"
#include "Generator/Public/Generators/MappingGenerator.h"
#include "Generator/Public/Generators/IDAMappingGenerator.h"
#include "Generator/Public/Generators/DumpspaceGenerator.h"

#include "Generator/Public/Generators/Generator.h"

#import <Foundation/Foundation.h>

#include "main.h"
#include "Menu/Logger.h"

#include "Engine/Public/Unreal/NameArray.h"
#include "Engine/Public/Unreal/DeltaForce.h"

namespace
{
    enum class EDumpState : int
    {
        Idle,       // Nothing started yet, or the engine core could not be initialized (safe to retry)
        Running,
        Finished,   // Generator state is global, a second run in the same process is not supported
    };

    std::atomic<EDumpState> DumpState = EDumpState::Idle;

    /* Dumper-7 recurses deeply while generating. Secondary threads on iOS only get 512KB of stack, which overflows. */
    constexpr size_t DumpThreadStackSize = 64 * 1024 * 1024;

    std::string ToStdString(NSString* String)
    {
        return String ? std::string([String UTF8String]) : std::string();
    }

    /*
    * Game name/version from Info.plist. The old code called KismetSystemLibrary::GetGameName/GetEngineVersion through
    * ProcessEvent with a guessed VTable index, which crashes the game if the index is wrong.
    */
    void InitGameNameAndVersion()
    {
        if (!Settings::Generator::GameName.empty() && !Settings::Generator::GameVersion.empty())
            return;

        @autoreleasepool
        {
            NSBundle* MainBundle = [NSBundle mainBundle];

            NSString* Name = [MainBundle objectForInfoDictionaryKey:@"CFBundleDisplayName"];
            if (![Name isKindOfClass:[NSString class]] || Name.length == 0)
                Name = [MainBundle objectForInfoDictionaryKey:@"CFBundleName"];

            NSString* Version = [MainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
            NSString* Build = [MainBundle objectForInfoDictionaryKey:@"CFBundleVersion"];

            if (Settings::Generator::GameName.empty())
            {
                if (Settings::Internal::bIsDeltaForce)
                    Settings::Generator::GameName = "DeltaForce";
                else
                    Settings::Generator::GameName = [Name isKindOfClass:[NSString class]] && Name.length > 0 ? ToStdString(Name) : "UnrealGame";
            }

            if (Settings::Generator::GameVersion.empty())
            {
                std::string VersionString = [Version isKindOfClass:[NSString class]] ? ToStdString(Version) : "";

                if ([Build isKindOfClass:[NSString class]] && Build.length > 0 && ToStdString(Build) != VersionString)
                    VersionString += (VersionString.empty() ? "" : "_") + ToStdString(Build);

                Settings::Generator::GameVersion = VersionString.empty() ? "Unknown" : VersionString;
            }
        }
    }

    void RunDump()
    {
        auto StartTime = std::chrono::high_resolution_clock::now();

        LogSuccess("Started Generation [Dumper-7]!\n");

        if (!Generator::InitEngineCore())
        {
            LogError("Dump aborted before anything was written. You can press 'Start Dump' again once the game finished loading.");
            DumpState = EDumpState::Idle;
            return;
        }

        /* From here on the generator's global state is modified, don't allow a second run in this process. */
        DumpState = EDumpState::Finished;

        InitGameNameAndVersion();

        LogInfo("GameName: %s\n", Settings::Generator::GameName.c_str());
        LogInfo("GameVersion: %s\n\n", Settings::Generator::GameVersion.c_str());

        Generator::InitInternal();

        Generator::Generate<CppGenerator>();
        Generator::Generate<MappingGenerator>();
        Generator::Generate<IDAMappingGenerator>();
        Generator::Generate<DumpspaceGenerator>();

        const std::chrono::duration<double, std::milli> Elapsed = std::chrono::high_resolution_clock::now() - StartTime;

        LogSuccess("\n\nGenerating SDK took (%fms)\n", Elapsed.count());
        LogSuccess("Output: %s/Documents/%s-%s/", Settings::Generator::SDKGenerationPath ? Settings::Generator::SDKGenerationPath : "~",
            Settings::Generator::GameVersion.c_str(), Settings::Generator::GameName.c_str());
    }

    void* DumpThreadEntry(void*)
    {
        @autoreleasepool
        {
            try
            {
                RunDump();
            }
            catch (const std::exception& Exception)
            {
                LogError("Dump failed with an exception: %s", Exception.what());
            }
            catch (...)
            {
                LogError("Dump failed with an unknown exception");
            }
        }

        if (DumpState == EDumpState::Running)
            DumpState = EDumpState::Idle;

        return nullptr;
    }
}

bool IsDumpRunning()
{
    return DumpState == EDumpState::Running;
}

void StartDump()
{
    EDumpState Expected = EDumpState::Idle;

    if (!DumpState.compare_exchange_strong(Expected, EDumpState::Running))
    {
        LogError(Expected == EDumpState::Running ? "A dump is already running." : "The SDK was already generated in this session. Restart the game to dump again.");
        return;
    }

    pthread_attr_t Attributes;
    pthread_attr_init(&Attributes);
    pthread_attr_setstacksize(&Attributes, DumpThreadStackSize);
    pthread_attr_setdetachstate(&Attributes, PTHREAD_CREATE_DETACHED);

    pthread_t Thread;
    const int Result = pthread_create(&Thread, &Attributes, DumpThreadEntry, nullptr);

    pthread_attr_destroy(&Attributes);

    if (Result != 0)
    {
        DumpState = EDumpState::Idle;
        LogError("Could not create the dump thread (error %d)", Result);
    }
}
