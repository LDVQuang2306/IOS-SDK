
#include <fstream>

#include "Generators/Generator.h"
#include "Managers/StructManager.h"
#include "Managers/EnumManager.h"
#include "Managers/MemberManager.h"
#include "Managers/PackageManager.h"

#include "HashStringTable.h"
#include "Utils.h"
#include "Menu/Logger.h"
#include "Unreal/NameArray.h"

inline void InitSettings()
{
	Settings::InitWeakObjectPtrSettings();
	Settings::InitLargeWorldCoordinateSettings();

	Settings::InitObjectPtrPropertySettings();
	Settings::InitArrayDimSizeSettings();
}

/* Scan __TEXT for the first IDA pattern that hits, ADRL-decode at hit+step,
 * and return the result as an imagebase-relative offset (the form
 * ObjectArray::Init / FName::Init expect). Returns 0 on failure.
 *
 * Lifted straight from Mj0x's UEGameProfile GetNamesPtr / GetGUObjectArrayPtr
 * idiom — paste a vector of {ida_pattern, step} pairs and call. */
static int32 FindOffsetByIDAPatterns(std::initializer_list<std::pair<const char*, int>> Patterns, const char* Label, const char* ModuleName = nullptr)
{
	const auto [ImageBase, ImageSize, Header, Slide] = GetImageBaseAndSize(ModuleName);
	if (!ImageBase) { LogError("FindOffsetByIDAPatterns[%s]: module not found", Label); return 0; }

	int Idx = 0;
	for (const auto& [Pattern, Step] : Patterns)
	{
		++Idx;
		uintptr_t Hit = reinterpret_cast<uintptr_t>(FindPattern(Pattern, "__TEXT", 0));
		if (!Hit) continue;

		uintptr_t Resolved = ASMUtils::ResolveADRL(Hit + Step);
		if (!Resolved || Resolved < ImageBase || Resolved >= (ImageBase + ImageSize)) continue;

		const int32 Offset = static_cast<int32>(Resolved - ImageBase);
		LogInfo("FindOffsetByIDAPatterns[%s]: pattern #%d hit @ +0x%lX -> 0x%X", Label, Idx, Hit - ImageBase, Offset);
		return Offset;
	}
	LogError("FindOffsetByIDAPatterns[%s]: no pattern matched", Label);
	return 0;
}


void Generator::InitEngineCore()
{
	LogInfo("Initializing Engine Core...");
	ObjectArray::Init();
	/* Per-game FName decryption hooks. Install BEFORE FName::Init.
	 * Pick whichever match the target game; unused hooks default to identity.
	 *
	 *   (1) Raw FNameEntry-bytes transform — header itself is encrypted.
	 *       Returns a (possibly scratch) entry pointer the reader uses instead.
	 * InitNameEntryDecryption([](uint8_t* Entry) -> uint8_t* {
	 *     // ... bytes-in / bytes-out ...
	 *     return Entry;
	 * });
	 *
	 *   (2) Output std::string transform — DeltaForce. Header is plaintext,
	 *       chars are XOR'd. Runs at the tail of FNameEntry::GetString on the
	 *       already-decoded std::string.
	 * InitNameStringDecryption([](std::string Decoded) -> std::string {
	 *     // ... XOR Decoded chars in-place, see DeltaForce example in ReadMe.md ...
	 *     return Decoded;
	 * });
	 *
	 *   (3) TNameEntryArray pointer chain — PUBG (UE 4.17, bIsNamePool == false):
	 * InitNameArrayDecryption([](uintptr_t Start) -> uintptr_t {
	 *     // ... pointer-chain walk, see PUBG example in ReadMe.md ...
	 *     return Start;
	 * });
	 *
	 *   (4) FNamePool pointer indirection — Valorant (UE 4.23+, bIsNamePool == true):
	 * InitNamePoolDecryption([](uintptr_t Start) -> uintptr_t {
	 *     // ... reassemble pointer from scattered bytes ...
	 *     return Start;
	 * });
	 */
	int32 GNamesOff = FindOffsetByIDAPatterns({
		{"? ? ? ? 29 01 15 91 28 21 08 8B", 0}, // HOK: World
	}, "GNames");
	FName::Init(GNamesOff, FName::EOffsetOverrideType::GNames, true, "NGR");
	Off::Init();
	PropertySizes::Init();
	Off::InSDK::ProcessEvent::InitPE(); //Must be at this position, relies on offsets initialized in Off::Init()

	Off::InSDK::World::InitGWorld(); //Must be at this position, relies on offsets initialized in Off::Init()

	Off::InSDK::Text::InitTextOffsets(); //Must be at this position, relies on offsets initialized in Off::InitPE()

	InitSettings();
	LogSuccess("Engine Core initialized successfully");
}

void Generator::InitInternal()
{
	LogInfo("Initializing Internal Generator...");

	// Initialize PackageManager with all packages, their names, structs, classes enums, functions and dependencies
	PackageManager::Init();

	// Initialize StructManager with all structs and their names
	StructManager::Init();

	// Initialize EnumManager with all enums and their names
	EnumManager::Init();

	// Initialized all Member-Name collisions
	MemberManager::Init();

	// Post-Initialize PackageManager after StructManager has been initialized. 'PostInit()' handles Cyclic-Dependencies detection
	PackageManager::PostInit();

	LogSuccess("Internal Generator initialized successfully");
}

bool Generator::SetupDumperFolder()
{
	try
	{
		std::string FolderName = (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName);

		FileNameHelper::MakeValidFileName(FolderName);

		DumperFolder = fs::path(Settings::Generator::SDKGenerationPath) / "Documents" / FolderName;

		if (fs::exists(DumperFolder))
		{
			fs::path Old = DumperFolder.generic_string() + "_OLD";

			fs::remove_all(Old);

			fs::rename(DumperFolder, Old);
		}

		fs::create_directories(DumperFolder);
	}
	catch (const std::filesystem::filesystem_error& fe)
	{
		LogError("Could not create required folders! Info: %s", fe.what());
		return false;
	}

	LogSuccess("Dumper folder created: %s", DumperFolder.string().c_str());
	return true;
}

bool Generator::SetupFolders(std::string& FolderName, fs::path& OutFolder)
{
	fs::path Dummy;
	std::string EmptyName = "";
	return SetupFolders(FolderName, OutFolder, EmptyName, Dummy);
}

bool Generator::SetupFolders(std::string& FolderName, fs::path& OutFolder, std::string& SubfolderName, fs::path& OutSubFolder)
{
	FileNameHelper::MakeValidFileName(FolderName);
	FileNameHelper::MakeValidFileName(SubfolderName);

	try
	{
		OutFolder = DumperFolder / FolderName;
		OutSubFolder = OutFolder / SubfolderName;

		if (fs::exists(OutFolder))
		{
			fs::path Old = OutFolder.generic_string() + "_OLD";

			fs::remove_all(Old);

			fs::rename(OutFolder, Old);
		}

		fs::create_directories(OutFolder);

		if (!SubfolderName.empty())
			fs::create_directories(OutSubFolder);
	}
	catch (const std::filesystem::filesystem_error& fe)
	{
		LogError("Could not create required folders! Info: %s", fe.what());
		return false;
	}

	return true;
}


// iOS port: stub. Upstream emits a Metadata.json describing editor-only properties.
// The iOS port doesn't yet expose Off::FField::EditorOnlyMetadata; skipping for KISS.
void DumpEditorOnlyMetadata(const fs::path& /*DumperFolder*/)
{
}

/* Emit a single-header summary of every offset Dumper-7 discovered + the runtime
 * pointers that drive the SDK. Consumed by external tooling (iOS_UEDumper-style
 * profiles, debug helpers) that wants Dumper-7's results without parsing the full
 * Cpp SDK. Mirrors the layout of iOS_UEDumper's UE_Offsets struct. */
void DumpUEOffsetsHeader(const fs::path& DumperFolder)
{
	const fs::path OutPath = DumperFolder / "UEOffsets.hpp";
	std::ofstream Out(OutPath);
	if (!Out)
	{
		LogError("DumpUEOffsetsHeader: failed to open %s", OutPath.string().c_str());
		return;
	}

	const uintptr_t ImageBase = GetModuleBase();
	auto Addr = [ImageBase](uintptr_t off) { return ImageBase + off; };

	Out << "#pragma once\n";
	Out << "// Auto-generated by Dumper-7 (iOS port).\n";
	Out << "// All offsets discovered for the target game by OffsetFinder + InitPE + InitGWorld.\n\n";
	Out << "#include <cstdint>\n\n";
	Out << "namespace UEOffsets\n{\n\n";

	Out << "namespace Config\n{\n";
	Out << "    constexpr bool isUsingCasePreservingName = " << (Settings::Internal::bUseCasePreservingName ? "true" : "false") << ";\n";
	Out << "    constexpr bool IsUsingFNamePool          = " << (Settings::Internal::bUseNamePool ? "true" : "false") << ";\n";
	Out << "    constexpr bool isUsingOutlineNumberName  = " << (Settings::Internal::bUseOutlineNumberName ? "true" : "false") << ";\n";
	Out << "}\n\n";

	Out << std::hex << std::showbase;

	Out << "namespace FName\n{\n";
	Out << "    constexpr uintptr_t ComparisonIndex = " << Off::FName::CompIdx << ";\n";
	Out << "    constexpr uintptr_t Number          = " << Off::FName::Number << ";\n";
	Out << "    constexpr uintptr_t DisplayIndex    = " << (Settings::Internal::bUseCasePreservingName ? Off::FName::CompIdx + 4 : 0) << ";\n";
	Out << "    constexpr uintptr_t Size            = " << Off::InSDK::Name::FNameSize << ";\n";
	Out << "}\n\n";

	Out << "namespace FNamePool\n{\n";
	Out << std::dec;
	Out << "    constexpr int32_t  Stride       = " << Off::InSDK::NameArray::FNameEntryStride << ";\n";
	Out << "    constexpr int32_t  BlocksBit    = " << Off::InSDK::NameArray::FNamePoolBlockOffsetBits << ";\n";
	Out << std::hex << std::showbase;
	Out << "    constexpr uintptr_t BlocksOff   = " << Off::NameArray::ChunksStart << ";\n";
	Out << "}\n\n";

	Out << "namespace FNamePoolEntry\n{\n";
	Out << "    constexpr uintptr_t Header = " << Off::FNameEntry::NamePool::HeaderOffset << ";\n";
	Out << "    constexpr uintptr_t Name   = " << Off::FNameEntry::NamePool::StringOffset << ";\n";
	Out << "}\n\n";

	Out << "namespace FUObjectArray\n{\n";
	Out << "    // Off::InSDK::ObjArray::GObjects already points at the ObjObjects sub-struct.\n";
	Out << "    constexpr uintptr_t ObjObjects = " << 0 << ";\n";
	Out << "}\n\n";

	Out << "namespace TUObjectArray\n{\n";
	Out << "    constexpr uintptr_t Objects             = " << Off::FUObjectArray::ChunkedFixedLayout.ObjectsOffset << ";\n";
	Out << "    constexpr uintptr_t NumElements         = " << Off::FUObjectArray::ChunkedFixedLayout.NumElementsOffset << ";\n";
	Out << "    constexpr uintptr_t NumElementsPerChunk = " << Off::InSDK::ObjArray::ChunkSize << ";\n";
	Out << "}\n\n";

	Out << "namespace FUObjectItem\n{\n";
	Out << "    constexpr uintptr_t Object = " << 0 << ";\n";
	Out << "    constexpr uintptr_t Size   = " << Off::InSDK::ObjArray::FUObjectItemSize << ";\n";
	Out << "}\n\n";

	Out << "namespace UObject\n{\n";
	Out << "    constexpr uintptr_t Vft           = " << Off::UObject::Vft << ";\n";
	Out << "    constexpr uintptr_t ObjectFlags   = " << Off::UObject::Flags << ";\n";
	Out << "    constexpr uintptr_t InternalIndex = " << Off::UObject::Index << ";\n";
	Out << "    constexpr uintptr_t ClassPrivate  = " << Off::UObject::Class << ";\n";
	Out << "    constexpr uintptr_t NamePrivate   = " << Off::UObject::Name << ";\n";
	Out << "    constexpr uintptr_t OuterPrivate  = " << Off::UObject::Outer << ";\n";
	Out << "}\n\n";

	Out << "namespace UField\n{\n";
	Out << "    constexpr uintptr_t Next = " << Off::UField::Next << ";\n";
	Out << "}\n\n";

	Out << "namespace UEnum\n{\n";
	Out << "    constexpr uintptr_t Names = " << Off::UEnum::Names << ";\n";
	Out << "}\n\n";

	Out << "namespace UStruct\n{\n";
	Out << "    constexpr uintptr_t SuperStruct     = " << Off::UStruct::SuperStruct << ";\n";
	Out << "    constexpr uintptr_t Children        = " << Off::UStruct::Children << ";\n";
	Out << "    constexpr uintptr_t ChildProperties = " << Off::UStruct::ChildProperties << ";\n";
	Out << "    constexpr uintptr_t PropertiesSize  = " << Off::UStruct::Size << ";\n";
	Out << "    constexpr uintptr_t MinAlignment    = " << Off::UStruct::MinAlignment << ";\n";
	Out << "    // -1 if not present in this build (StructBaseChain enables a faster IsA path).\n";
	Out << "    constexpr int32_t  StructBaseChain  = " << std::dec << Off::UStruct::StructBaseChain << std::hex << std::showbase << ";\n";
	Out << "}\n\n";

	Out << "namespace UFunction\n{\n";
	Out << "    constexpr uintptr_t EFunctionFlags = " << Off::UFunction::FunctionFlags << ";\n";
	Out << "    constexpr uintptr_t Func           = " << Off::UFunction::ExecFunction << ";\n";
	Out << "    // NumParams / ParamSize: not exposed by Dumper-7 (derived by walking Children).\n";
	Out << "}\n\n";

	Out << "namespace UClass\n{\n";
	Out << "    constexpr uintptr_t CastFlags             = " << Off::UClass::CastFlags << ";\n";
	Out << "    constexpr uintptr_t ClassDefaultObject    = " << Off::UClass::ClassDefaultObject << ";\n";
	Out << "    constexpr uintptr_t ImplementedInterfaces = " << Off::UClass::ImplementedInterfaces << ";\n";
	Out << "}\n\n";

	if (Settings::Internal::bUseFProperty)
	{
		Out << "namespace FField\n{\n";
		Out << "    constexpr uintptr_t Vft          = " << Off::FField::Vft << ";\n";
		Out << "    constexpr uintptr_t ClassPrivate = " << Off::FField::Class << ";\n";
		Out << "    constexpr uintptr_t Owner        = " << Off::FField::Owner << ";\n";
		Out << "    constexpr uintptr_t Next         = " << Off::FField::Next << ";\n";
		Out << "    constexpr uintptr_t NamePrivate  = " << Off::FField::Name << ";\n";
		Out << "    constexpr uintptr_t FlagsPrivate = " << Off::FField::Flags << ";\n";
		Out << "    // -1 if not present (editor-only builds expose extra metadata).\n";
		Out << "    constexpr int32_t  EditorOnlyMetadata = " << std::dec << Off::FField::EditorOnlyMetadata << std::hex << std::showbase << ";\n";
		Out << "}\n\n";

		Out << "namespace FFieldClass\n{\n";
		Out << "    constexpr uintptr_t Name       = " << Off::FFieldClass::Name << ";\n";
		Out << "    constexpr uintptr_t Id         = " << Off::FFieldClass::Id << ";\n";
		Out << "    constexpr uintptr_t CastFlags  = " << Off::FFieldClass::CastFlags << ";\n";
		Out << "    constexpr uintptr_t ClassFlags = " << Off::FFieldClass::ClassFlags << ";\n";
		Out << "    constexpr uintptr_t SuperClass = " << Off::FFieldClass::SuperClass << ";\n";
		Out << "}\n\n";

		Out << "namespace FProperty\n{\n";
		Out << "    constexpr uintptr_t ArrayDim        = " << Off::Property::ArrayDim << ";\n";
		Out << "    constexpr uintptr_t ElementSize     = " << Off::Property::ElementSize << ";\n";
		Out << "    constexpr uintptr_t PropertyFlags   = " << Off::Property::PropertyFlags << ";\n";
		Out << "    constexpr uintptr_t Offset_Internal = " << Off::Property::Offset_Internal << ";\n";
		Out << "    constexpr uintptr_t Size            = " << Off::InSDK::Properties::PropertySize << ";\n";
		Out << "}\n\n";
	}
	else
	{
		Out << "namespace UProperty\n{\n";
		Out << "    constexpr uintptr_t ArrayDim        = " << Off::Property::ArrayDim << ";\n";
		Out << "    constexpr uintptr_t ElementSize     = " << Off::Property::ElementSize << ";\n";
		Out << "    constexpr uintptr_t PropertyFlags   = " << Off::Property::PropertyFlags << ";\n";
		Out << "    constexpr uintptr_t Offset_Internal = " << Off::Property::Offset_Internal << ";\n";
		Out << "    constexpr uintptr_t Size            = " << Off::InSDK::Properties::PropertySize << ";\n";
		Out << "}\n\n";
	}

	/* Per-property-type subclass offsets. All sub-offsets are relative to the property
	 * base (UProperty or FProperty depending on UE version). */
	Out << "namespace ByteProperty       { constexpr uintptr_t Enum               = " << Off::ByteProperty::Enum << "; }\n";
	Out << "namespace BoolProperty       { constexpr uintptr_t Base               = " << Off::BoolProperty::Base << "; }\n";
	Out << "namespace ObjectProperty     { constexpr uintptr_t PropertyClass      = " << Off::ObjectProperty::PropertyClass << "; }\n";
	Out << "namespace ClassProperty      { constexpr uintptr_t MetaClass          = " << Off::ClassProperty::MetaClass << "; }\n";
	Out << "namespace StructProperty     { constexpr uintptr_t Struct             = " << Off::StructProperty::Struct << "; }\n";
	Out << "namespace ArrayProperty      { constexpr uintptr_t Inner              = " << Off::ArrayProperty::Inner << "; }\n";
	Out << "namespace DelegateProperty   { constexpr uintptr_t SignatureFunction  = " << Off::DelegateProperty::SignatureFunction << "; }\n";
	Out << "namespace MapProperty        { constexpr uintptr_t Base               = " << Off::MapProperty::Base << "; }\n";
	Out << "namespace SetProperty        { constexpr uintptr_t ElementProp        = " << Off::SetProperty::ElementProp << "; }\n";
	Out << "namespace EnumProperty       { constexpr uintptr_t Base               = " << Off::EnumProperty::Base << "; }\n";
	Out << "namespace FieldPathProperty  { constexpr uintptr_t FieldClass         = " << Off::FieldPathProperty::FieldClass << "; }\n";
	Out << "namespace OptionalProperty   { constexpr uintptr_t ValueProperty      = " << Off::OptionalProperty::ValueProperty << "; }\n\n";

	/* Property sizes that vary by UE version / compiler. */
	Out << "namespace PropertySizes\n{\n";
	Out << "    constexpr int32_t DelegateProperty               = " << std::dec << PropertySizes::DelegateProperty << ";\n";
	Out << "    constexpr int32_t FieldPathProperty              = " << PropertySizes::FieldPathProperty << ";\n";
	Out << "    constexpr int32_t MulticastInlineDelegateProperty = " << PropertySizes::MulticastInlineDelegateProperty << ";\n";
	Out << "}\n\n";
	Out << std::hex << std::showbase;

	/* FName resolution helpers (used by SDK's runtime to call AppendString / GetNameEntryFromName). */
	Out << "namespace FNameResolver\n{\n";
	Out << "    constexpr bool      bUsesAppendString          = " << (Off::InSDK::Name::bIsUsingAppendStringOverToString ? "true" : "false") << ";\n";
	Out << "    constexpr bool      bAppendStringInlined       = " << (Off::InSDK::Name::bIsAppendStringInlinedAndUsed ? "true" : "false") << ";\n";
	Out << "    constexpr uintptr_t AppendNameToString_Offset  = " << Off::InSDK::Name::AppendNameToString << ";\n";
	Out << "    constexpr uintptr_t GetNameEntryFromName_Offset = " << Off::InSDK::Name::GetNameEntryFromName << ";\n";
	Out << "}\n\n";

	/* FText layout (used for FText::ToString in the SDK). */
	Out << "namespace FText\n{\n";
	Out << "    constexpr uintptr_t TextSize                = " << Off::InSDK::Text::TextSize << ";\n";
	Out << "    constexpr uintptr_t TextDatOffset           = " << Off::InSDK::Text::TextDatOffset << ";\n";
	Out << "    constexpr uintptr_t InTextDataStringOffset  = " << Off::InSDK::Text::InTextDataStringOffset << ";\n";
	Out << "}\n\n";

	/* Misc UE class field offsets that the SDK references. */
	Out << "namespace ULevel    { constexpr uintptr_t Actors = " << Off::InSDK::ULevel::Actors << "; }\n";
	Out << "namespace UDataTable { constexpr uintptr_t RowMap = " << Off::InSDK::UDataTable::RowMap << "; }\n\n";

	Out << "}  // namespace UEOffsets\n\n";

	Out << "namespace UEPointers\n{\n";
	Out << "    constexpr uintptr_t Names             = " << Addr(Off::InSDK::NameArray::GNames) << ";\n";
	Out << "    constexpr uintptr_t UObjectArray      = " << Addr(Off::InSDK::ObjArray::GObjects) << ";\n";
	Out << "    constexpr uintptr_t ObjObjects        = " << Addr(Off::InSDK::ObjArray::GObjects) << ";\n";
	Out << "    constexpr uintptr_t World             = " << Addr(Off::InSDK::World::GWorld) << ";\n";
	Out << "    constexpr uintptr_t ProcessEvent      = " << Addr(Off::InSDK::ProcessEvent::PEOffset) << ";\n";
	Out << std::dec;
	Out << "    constexpr int32_t  ProcessEventIndex = " << Off::InSDK::ProcessEvent::PEIndex << ";\n";
	Out << "}\n";

	Out.close();
	LogSuccess("DumpUEOffsetsHeader: wrote %s", OutPath.string().c_str());
}
