#pragma once

#include "Generators/Generator.h"
#include "OffsetFinder/Offsets.h"
#include "Utils.h"
#include "Menu/Logger.h"

#include <vector>
#include <algorithm>
#include <format.h>
#include <fstream>

class CoreGenerator
{
public:
    static inline PredefinedMemberLookupMapType PredefinedMembers;
    static inline std::string MainFolderName = "SDK";
    static inline std::string SubfolderName = "Core";
    static inline fs::path MainFolder;
    static inline fs::path Subfolder;

    struct MemberInfo {
        int32 Offset;
        int32 Size;
        std::string Decl;
    };


    static void Generate()
    {
        fs::path FilePath = MainFolder / "CoreStructures.h";
        std::ofstream File(FilePath);

        if (!File.is_open())
        {
            LogError("Failed to open file: %s", FilePath.string().c_str());
            return;
        }

        File << "#pragma once\n";
        File << "#include <string>\n";
        File << "#include <vector>\n\n";

        // Forward declarations
        File << "class UObject;\n";
        File << "class UClass;\n";
        File << "class UFunction;\n";
        File << "class UStruct;\n";
        File << "class UScriptStruct;\n";
        File << "class FField;\n";
        File << "class FProperty;\n";
        File << "struct FName;\n";
        File << "struct FWrappedName;\n";
        File << "struct FUObjectArray;\n";
        File << "struct FUObjectItem;\n\n";

        WriteFStructBaseChain(File);
        WriteUObject(File);
        WriteUField(File);
        WriteUProperty(File);
        WriteUStruct(File);
        WriteUFunction(File);
        WriteUClass(File);
        WriteUScriptStruct(File);
        WriteWeakObjectPtr(File);

        File.close();
        LogSuccess("Core structures generated at: %s", FilePath.string().c_str());
    }

    static void InitPredefinedMembers() {}
    static void InitPredefinedFunctions() {}

private:
   
    static std::string GeneratePadding(int32 Offset, int32 Size, std::string Reason)
    {
        if (Size <= 0) return "";
        return fmt::format("\tuint8 Pad_{:X}[0x{:X}]; // 0x{:X} ({})\n", Offset, Size, Offset, Reason);
    }

    static std::string GenerateMember(std::string Type, std::string Name, int32 Offset, int32 Size, std::string Comment = "")
    {
        return fmt::format("\t{:{}} {:{}} // 0x{:X} {}\n", Type, 40, Name + ";", 40, Offset, Comment);
    }

    static void WriteStructEx(std::ofstream& File, std::string Name, std::string Base, const std::vector<MemberInfo>& Members, int32 TotalSize = -1)
    {
        File << "class " << Name;
        if (!Base.empty()) File << " : public " << Base;
        File << "\n{\npublic:\n";

        int32 CurrentOffset = 0;
        
        int32 BaseEnd = 0; 
        
        if (Name == "UObject") BaseEnd = 0;
        else if (Name == "UField") BaseEnd = 0x28; // UObject
        else if (Name == "UProperty") BaseEnd = 0x30; // UField
        else if (Name == "UStruct") BaseEnd = 0x30; // UField
        else if (Name == "UFunction") BaseEnd = 0xB0; // UStruct end estimate
        else if (Name == "UClass") BaseEnd = 0x88; // UStruct end estimate
        else if (Name == "UScriptStruct") BaseEnd = 0x88;
        
        // Dynamic calc best effort
        // We know UObject End is Off::UObject::Index + 4
        if (Name == "UField") BaseEnd = Off::UObject::Index + 4;
        
        CurrentOffset = BaseEnd;
        
        // If sorting needed
        std::vector<MemberInfo> SortedMembers = Members;
        std::sort(SortedMembers.begin(), SortedMembers.end(), [](const MemberInfo& a, const MemberInfo& b) {
            return a.Offset < b.Offset;
        });

        for (const auto& Mem : SortedMembers)
        {
            if (Mem.Offset < CurrentOffset) continue; 

            if (Mem.Offset > CurrentOffset)
            {
                File << CoreGenerator::GeneratePadding(CurrentOffset, Mem.Offset - CurrentOffset, "Gap");
                CurrentOffset = Mem.Offset;
            }

            File << Mem.Decl;
            CurrentOffset += Mem.Size;
        }
        
        if (TotalSize > CurrentOffset)
        {
             File << CoreGenerator::GeneratePadding(CurrentOffset, TotalSize - CurrentOffset, "Struct Padding");
        }

        File << "};\n\n";
    }

    static void WriteFStructBaseChain(std::ofstream& File)
    {
        File << "class FStructBaseChain\n"
             << "{\n"
             << "protected:\n"
             << "\t// Helper functions not dumped\n"
             << "private:\n"
             << "\tvoid** StructBaseChainArray;\n"
             << "\tint32 NumStructBasesInChainMinusOne;\n"
             << "\tfriend class UStruct;\n"
             << "};\n\n";
    }

    static void WriteUObject(std::ofstream& File)
    {
        std::vector<MemberInfo> Members;

        Members.push_back({0x0, (int32)sizeof(void*), CoreGenerator::GenerateMember("void**", "VTable", 0x0, sizeof(void*))});
        Members.push_back({Off::UObject::Flags, 4, CoreGenerator::GenerateMember("EObjectFlags", "ObjectFlags", Off::UObject::Flags, 4)});
        Members.push_back({Off::UObject::Index, 4, CoreGenerator::GenerateMember("int32", "InternalIndex", Off::UObject::Index, 4)});
        Members.push_back({Off::UObject::Class, (int32)sizeof(void*), CoreGenerator::GenerateMember("class UClass*", "ClassPrivate", Off::UObject::Class, sizeof(void*))});
        Members.push_back({Off::UObject::Name, (int32)sizeof(int32)*2, CoreGenerator::GenerateMember("struct FName", "NamePrivate", Off::UObject::Name, sizeof(int32)*2)});
        Members.push_back({Off::UObject::Outer, (int32)sizeof(void*), CoreGenerator::GenerateMember("class UObject*", "OuterPrivate", Off::UObject::Outer, sizeof(void*))});
        
        // Add static GUObjectArray
        File << "// GUObjectArray static member not dumped in definition (needs external def)\n";
        
        WriteStructEx(File, "UObject", "", Members);
    }

    static void WriteUField(std::ofstream& File)
    {
        std::vector<MemberInfo> Members;
        Members.push_back({Off::UField::Next, (int32)sizeof(void*), CoreGenerator::GenerateMember("class UField*", "Next", Off::UField::Next, sizeof(void*))});
        WriteStructEx(File, "UField", "UObject", Members);
    }

    static void WriteUProperty(std::ofstream& File)
    {
        File << "class UProperty : public UField\n{\npublic:\n";
        File << "\t// Legacy UProperty support or minimal dump\n";
        File << "};\n\n";
    }

    static void WriteUStruct(std::ofstream& File)
    {
        std::vector<MemberInfo> Members;
        
        Members.push_back({Off::UStruct::SuperStruct, (int32)sizeof(void*), CoreGenerator::GenerateMember("class UStruct*", "SuperStruct", Off::UStruct::SuperStruct, sizeof(void*))});
        Members.push_back({Off::UStruct::Children, (int32)sizeof(void*), CoreGenerator::GenerateMember("class UField*", "Children", Off::UStruct::Children, sizeof(void*))});
        Members.push_back({Off::UStruct::ChildProperties, (int32)sizeof(void*), CoreGenerator::GenerateMember("class FField*", "ChildProperties", Off::UStruct::ChildProperties, sizeof(void*))});
        Members.push_back({Off::UStruct::Size, 4, CoreGenerator::GenerateMember("int32", "Size", Off::UStruct::Size, 4)});
        Members.push_back({Off::UStruct::MinAlignemnt, 4, CoreGenerator::GenerateMember("int32", "MinAlignemnt", Off::UStruct::MinAlignemnt, 4)});

        WriteStructEx(File, "UStruct", "UField", Members); 
    }

    static void WriteUFunction(std::ofstream& File)
    {
         std::vector<MemberInfo> Members;
         Members.push_back({Off::UFunction::FunctionFlags, 4, CoreGenerator::GenerateMember("EFunctionFlags", "FunctionFlags", Off::UFunction::FunctionFlags, 4)});
         Members.push_back({Off::UFunction::ExecFunction, (int32)sizeof(void*), CoreGenerator::GenerateMember("Native", "Func", Off::UFunction::ExecFunction, sizeof(void*))});
         
         WriteStructEx(File, "UFunction", "UStruct", Members);
    }

    static void WriteUClass(std::ofstream& File)
    {
        std::vector<MemberInfo> Members;
        Members.push_back({Off::UClass::CastFlags, 8, CoreGenerator::GenerateMember("EClassCastFlags", "ClassCastFlags", Off::UClass::CastFlags, 8)});
        Members.push_back({Off::UClass::ClassDefaultObject, (int32)sizeof(void*), CoreGenerator::GenerateMember("class UObject*", "ClassDefaultObject", Off::UClass::ClassDefaultObject, sizeof(void*))});
        Members.push_back({Off::UClass::ImplementedInterfaces, (int32)sizeof(void*)*2, CoreGenerator::GenerateMember("TArray<FImplementedInterface>", "Interfaces", Off::UClass::ImplementedInterfaces, sizeof(void*)*2)});

        WriteStructEx(File, "UClass", "UStruct", Members);
    }

    static void WriteUScriptStruct(std::ofstream& File)
    {
        WriteStructEx(File, "UScriptStruct", "UStruct", {});
    }

    static void WriteWeakObjectPtr(std::ofstream& File)
    {
        File << "struct FWeakObjectPtr\n{\n";
        File << "\tint32 ObjectIndex;\n";
        File << "\tint32 ObjectSerialNumber;\n";
        File << "};\n\n";

        File << "template<typename T>\nclass TWeakObjectPtr : public FWeakObjectPtr\n{\npublic:\n\tT* Get() const;\n};\n\n";
    }
};
