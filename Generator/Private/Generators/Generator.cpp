
#include "../../Public/Generators/Generator.h"
#include "../../Public/Managers/StructManager.h"
#include "../../Public/Managers/EnumManager.h"
#include "../../Public/Managers/MemberManager.h"
#include "../../Public/Managers/PackageManager.h"

#include "../../Public/HashStringTable.h"
#include "../../../Utils/Utils.h"
#include "../../../Menu/Logger.h"
#include "../../../Engine/Public/Unreal/NameArray.h"

inline void InitWeakObjectPtrSettings()
{
	UEStruct LoadAsset = ObjectArray::FindObjectFast<UEFunction>("LoadAsset", EClassCastFlags::Function);

	if (!LoadAsset)
	{
		LogError("'LoadAsset' wasn't found, could not determine value for 'bIsWeakObjectPtrWithoutTag'!");
		return;
	}

	UEProperty Asset = LoadAsset.FindMember("Asset", EClassCastFlags::SoftObjectProperty);
	if (!Asset)
	{
		LogError("'Asset' wasn't found, could not determine value for 'bIsWeakObjectPtrWithoutTag'!");
		return;
	}

	UEStruct SoftObjectPath = ObjectArray::FindObjectFast<UEStruct>("SoftObjectPath");

	constexpr int32 SizeOfFFWeakObjectPtr = 0x08;
	constexpr int32 OldUnrealAssetPtrSize = 0x10;
	const int32 SizeOfSoftObjectPath = SoftObjectPath ? SoftObjectPath.GetStructSize() : OldUnrealAssetPtrSize;

	Settings::Internal::bIsWeakObjectPtrWithoutTag = Asset.GetSize() <= (SizeOfSoftObjectPath + SizeOfFFWeakObjectPtr);

    LogSuccess("\nDumper-7: bIsWeakObjectPtrWithoutTag = %d\n", Settings::Internal::bIsWeakObjectPtrWithoutTag);
}

inline void InitLargeWorldCoordinateSettings()
{
	UEStruct FVectorStruct = ObjectArray::FindObjectFast<UEStruct>("Vector", EClassCastFlags::Struct);

	if (!FVectorStruct) [[unlikely]]
	{
		LogError("Something went horribly wrong, FVector wasn't even found!");
		return;
	}

	UEProperty XProperty = FVectorStruct.FindMember("X");

	if (!XProperty) [[unlikely]]
	{
		LogError("Something went horribly wrong, FVector::X wasn't even found!");
		return;
	}

	/* Check the underlaying type of FVector::X. If it's double we're on UE5.0, or higher, and using large world coordinates. */
	Settings::Internal::bUseLargeWorldCoordinates = XProperty.IsA(EClassCastFlags::DoubleProperty);

    LogSuccess("\nDumper-7: bUseLargeWorldCoordinates = %d\n", Settings::Internal::bUseLargeWorldCoordinates);
}

inline void InitSettings()
{
	InitWeakObjectPtrSettings();
	InitLargeWorldCoordinateSettings();
}


void Generator::InitEngineCore()
{
	LogInfo("Initializing Engine Core...");
	
	/* manual override */
	// ObjectArray::Init(0x12cde518, 0x10000, FChunkedFixedUObjectArrayLayout {
	// 	.ObjectsOffset = 0x20,
	// 	.MaxElementsOffset = 0x10,
	// 	.NumElementsOffset = 0x4,
	// 	.MaxChunksOffset = 0x0,
	// 	.NumChunksOffset = 0x14,
	// }, "DeltaForceClient");

	//FName::Init(/*FName::AppendString*/);
	//FName::Init(/*FName::ToString, FName::EOffsetOverrideType::ToString*/);
	//FName::Init(/*GNames, FName::EOffsetOverrideType::GNames, true/false*/);
	//Off::InSDK::ProcessEvent::InitPE(/*PEIndex*/);

	/* Back4Blood (requires manual GNames override) */
	//InitObjectArrayDecryption([](void* ObjPtr) -> uint8* { return reinterpret_cast<uint8*>(uint64(ObjPtr) ^ 0x8375); });

	/* Multiversus [Unsupported, weird GObjects-struct] */
	//InitObjectArrayDecryption([](void* ObjPtr) -> uint8* { return reinterpret_cast<uint8*>(uint64(ObjPtr) ^ 0x1B5DEAFD6B4068C); });

	ObjectArray::Init();
//    FName::Init((int32)0x0CB41B80, FName::EOffsetOverrideType::GNames, true, "UAGame"); // ArenaBreakout
    //    FName::Init((int32)0x05E4AD40, FName::EOffsetOverrideType::GNames, true, "ShooterGame"); // ARK Revamp
    FName::Init((int32)0x1290abc0, FName::EOffsetOverrideType::GNames, false /* Not FNamePool */, "DeltaForceClient"); // ARK 2.0
//    FName::Init();
	Off::Init();
	PropertySizes::Init();
	Off::InSDK::ProcessEvent::InitPE(69); //Must be at this position, relies on offsets initialized in Off::Init()

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
