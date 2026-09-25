#include <iostream>
#include <chrono>
#include <fstream>
#include <thread>
#include <atomic>
#include <cstdio>

#include "Generators/CppGenerator.h"
#include "Generators/MappingGenerator.h"
#include "Generators/IDAMappingGenerator.h"
#include "Generators/DumpspaceGenerator.h"

#include "Generators/Generator.h"

#import <Foundation/Foundation.h>

#include "main.h"
#include "Menu/Logger.h"

#include "Unreal/NameArray.h"

using namespace std::chrono_literals;

namespace
{
    enum class EDumpState : int
    {
        Idle,
        Running,
        Finished
    };

    std::atomic<EDumpState> GDumpState{ EDumpState::Idle };

    std::string ToStdString(NSString* String)
    {
        return String ? std::string([String UTF8String] ?: "") : std::string();
    }

    /* Game name/version from the app bundle: no engine code has to be executed for it. */
    void InitGameInfoFromBundle()
    {
        @autoreleasepool
        {
            NSDictionary* Info = [[NSBundle mainBundle] infoDictionary];

            NSString* Name = Info[@"CFBundleDisplayName"] ?: Info[@"CFBundleName"] ?: Info[@"CFBundleExecutable"];
            NSString* Version = Info[@"CFBundleShortVersionString"];
            NSString* Build = Info[@"CFBundleVersion"];

            std::string GameName = ToStdString(Name);
            std::string GameVersion = ToStdString(Version);

            if (Build && (!Version || ![Build isEqualToString:Version]))
                GameVersion += (GameVersion.empty() ? "" : "_") + ToStdString(Build);

            Settings::Generator::GameName = GameName.empty() ? "UnrealGame" : GameName;
            Settings::Generator::GameVersion = GameVersion.empty() ? "0" : GameVersion;
        }
    }

    /* Purely informational, only done if ProcessEvent was found with high confidence. */
    void LogEngineVersion()
    {
        if (!Off::InSDK::ProcessEvent::bIsValid || !Settings::Config::bCallProcessEvent)
            return;

        UEClass Kismet = ObjectArray::FindClassFast("KismetSystemLibrary");
        UEFunction GetEngineVersion = Kismet ? Kismet.GetFunction("KismetSystemLibrary", "GetEngineVersion") : UEFunction(nullptr);

        if (!Kismet || !GetEngineVersion || !Kismet.GetDefaultObject())
            return;

        FString Version;
        if (Kismet.GetDefaultObject().ProcessEvent(GetEngineVersion, &Version) && Version.IsValid())
            LogInfo("EngineVersion: %s", Version.ToString().c_str());
    }
}

bool IsDumpRunning()
{
    return GDumpState == EDumpState::Running;
}

void StartDump()
{
    EDumpState Expected = EDumpState::Idle;
    if (!GDumpState.compare_exchange_strong(Expected, EDumpState::Running))
    {
        LogError(Expected == EDumpState::Running ? "A dump is already running, please wait." : "The SDK was already dumped in this session. Restart the game to dump again.");
        return;
    }

    /* The generator keeps global state once it ran, so only a failed engine-core initialization may be retried. */
    bool bGeneratorStarted = false;

    try
    {
        std::this_thread::sleep_for(2s);

        Settings::Config::DelayDumperStart();

        auto t_1 = std::chrono::high_resolution_clock::now();

        LogSuccess("Started Generation [Dumper-7]!\n");

        if (!Generator::InitEngineCore())
        {
            LogError("Dump aborted, nothing was generated. You can press 'Start Dump' again once the game is fully loaded.");
            GDumpState = EDumpState::Idle;
            return;
        }

        if (Settings::Generator::GameName.empty() && Settings::Generator::GameVersion.empty())
            InitGameInfoFromBundle();

        LogInfo("GameName: %s\n", Settings::Generator::GameName.c_str());
        LogInfo("GameVersion: %s\n\n", Settings::Generator::GameVersion.c_str());
        LogEngineVersion();

        bGeneratorStarted = true;

        Generator::InitInternal();

        Generator::Generate<CppGenerator>();
        Generator::Generate<MappingGenerator>();
        Generator::Generate<IDAMappingGenerator>();
        Generator::Generate<DumpspaceGenerator>();


        auto t_C = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> ms_double_ = t_C - t_1;

        LogSuccess("\n\nGenerating SDK took (%fms)\n\n\n", ms_double_.count());
    }
    catch (const std::exception& Exception)
    {
        /* An exception escaping this thread would std::terminate() the whole game. */
        LogError("Dump failed with an exception: %s", Exception.what());
    }
    catch (...)
    {
        LogError("Dump failed with an unknown exception");
    }

    GDumpState = bGeneratorStarted ? EDumpState::Finished : EDumpState::Idle;
}
