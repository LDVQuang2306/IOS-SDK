#include <cstdio>
#include <cstdlib>
#include <exception>
#include "Generator/Public/Generators/CppGenerator.h"
#include "Generator/Public/Generators/MappingGenerator.h"
#include "Generator/Public/Generators/IDAMappingGenerator.h"
#include "Generator/Public/Generators/DumpspaceGenerator.h"
#include "Generator/Public/Generators/Generator.h"
#include "Engine/Public/Unreal/DeltaForce.h"

void BuildFakeDeltaForce();

/* Same sequence as RunDump()/DumpThreadEntry() in main.mm */
int main()
{
    BuildFakeDeltaForce();

    try
    {
        if (!Generator::InitEngineCore()) { printf("InitEngineCore FAILED\n"); return 1; }

        Settings::Generator::GameName = "DeltaForce";
        Settings::Generator::GameVersion = "1.0.0_Test";

        Generator::InitInternal();
        Generator::Generate<CppGenerator>();
        Generator::Generate<MappingGenerator>();
        Generator::Generate<IDAMappingGenerator>();
        Generator::Generate<DumpspaceGenerator>();
    }
    catch (const std::exception& Exception)
    {
        printf("[E] Dump failed with an exception: %s\n", Exception.what());
        return 2;
    }

    printf("HARNESS DONE\n");
    return 0;
}
