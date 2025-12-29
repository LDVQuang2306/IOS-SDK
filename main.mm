#include <iostream>
#include <chrono>
#include <fstream>
#include <thread>
#include <cstdio>

#include "Generator/Public/Generators/CppGenerator.h"
#include "Generator/Public/Generators/MappingGenerator.h"
#include "Generator/Public/Generators/IDAMappingGenerator.h"
#include "Generator/Public/Generators/DumpspaceGenerator.h"

#include "Generator/Public/Generators/Generator.h"

#import <Foundation/Foundation.h>

#include "main.h"
#include "Menu/Logger.h"

#include "Engine/Public/Unreal/NameArray.h"

using namespace std::chrono_literals;

void StartDump()
{
    std::this_thread::sleep_for(2s);
    
    auto t_1 = std::chrono::high_resolution_clock::now();

    LogSuccess("Started Generation [Dumper-7]!\n");
    Generator::InitEngineCore();
    Generator::InitInternal();

    if (Settings::Generator::GameName.empty() && Settings::Generator::GameVersion.empty())
    {
        FString Name;
        FString Version;
        UEClass Kismet = ObjectArray::FindClassFast("KismetSystemLibrary");
        UEFunction GetGameName = Kismet.GetFunction("KismetSystemLibrary", "GetGameName");
        UEFunction GetEngineVersion = Kismet.GetFunction("KismetSystemLibrary", "GetEngineVersion");

        Kismet.ProcessEvent(GetGameName, &Name);
        Kismet.ProcessEvent(GetEngineVersion, &Version);

        Settings::Generator::GameName = Name.ToString();
        Settings::Generator::GameVersion = Version.ToString();
    }

    LogInfo("GameName: %s\n", Settings::Generator::GameName.c_str());
    LogInfo("GameVersion: %s\n\n", Settings::Generator::GameVersion.c_str());


    Generator::Generate<CppGenerator>();
    Generator::Generate<MappingGenerator>();
    Generator::Generate<IDAMappingGenerator>();
    Generator::Generate<DumpspaceGenerator>();


    auto t_C = std::chrono::high_resolution_clock::now();

    auto ms_int_ = std::chrono::duration_cast<std::chrono::milliseconds>(t_C - t_1);
    std::chrono::duration<double, std::milli> ms_double_ = t_C - t_1;

    LogInfo("\n\nGenerating SDK took (%fms)\n\n\n", ms_double_.count());
}
