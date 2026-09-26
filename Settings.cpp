#include "Settings.h"

#include <chrono>
#include <thread>

#include "Unreal/UnrealObjects.h"
#include "Unreal/ObjectArray.h"
#include "Menu/Logger.h"

void Settings::InitWeakObjectPtrSettings()
{
	const UEStruct LoadAsset = ObjectArray::FindObjectFast<UEFunction>("LoadAsset", EClassCastFlags::Function);

	if (!LoadAsset)
	{
		LogError("'LoadAsset' wasn't found, could not determine value for 'bIsWeakObjectPtrWithoutTag'!");
		return;
	}

	const UEProperty Asset = LoadAsset.FindMember("Asset", EClassCastFlags::SoftObjectProperty);
	if (!Asset)
	{
		LogError("'Asset' wasn't found, could not determine value for 'bIsWeakObjectPtrWithoutTag'!");
		return;
	}

	const UEStruct SoftObjectPath = ObjectArray::FindObjectFast<UEStruct>("SoftObjectPath");

	constexpr int32 SizeOfFFWeakObjectPtr = 0x08;
	constexpr int32 OldUnrealAssetPtrSize = 0x10;
	const int32 SizeOfSoftObjectPath = SoftObjectPath ? SoftObjectPath.GetStructSize() : OldUnrealAssetPtrSize;

	Settings::Internal::bIsWeakObjectPtrWithoutTag = Asset.GetSize() <= (SizeOfSoftObjectPath + SizeOfFFWeakObjectPtr);

	LogSuccess("\nDumper-7: bIsWeakObjectPtrWithoutTag = %d\n", Settings::Internal::bIsWeakObjectPtrWithoutTag);
}

void Settings::InitLargeWorldCoordinateSettings()
{
	const UEStruct FVectorStruct = ObjectArray::FindObjectFast<UEStruct>("Vector", EClassCastFlags::Struct);

	if (!FVectorStruct) [[unlikely]]
	{
		LogError("Something went horribly wrong, FVector wasn't even found!");
		return;
	}

	const UEProperty XProperty = FVectorStruct.FindMember("X");

	if (!XProperty) [[unlikely]]
	{
		LogError("Something went horribly wrong, FVector::X wasn't even found!");
		return;
	}

	/* Check the underlaying type of FVector::X. If it's double we're on UE5.0, or higher, and using large world coordinates. */
	Settings::Internal::bUseLargeWorldCoordinates = XProperty.IsA(EClassCastFlags::DoubleProperty);

	LogSuccess("\nDumper-7: bUseLargeWorldCoordinates = %d\n", Settings::Internal::bUseLargeWorldCoordinates);
}

void Settings::InitObjectPtrPropertySettings()
{
	const UEClass ObjectPtrPropertyClass = ObjectArray::FindClassFast("ObjectPtrProperty");

	if (!ObjectPtrPropertyClass)
	{
		Settings::Internal::bIsObjPtrInsteadOfFieldPathProperty = false;
		LogInfo("\nDumper-7: bIsObjPtrInsteadOfFieldPathProperty = %d\n", Settings::Internal::bIsObjPtrInsteadOfFieldPathProperty);
		return;
	}

	Settings::Internal::bIsObjPtrInsteadOfFieldPathProperty = ObjectPtrPropertyClass.GetDefaultObject().IsA(EClassCastFlags::FieldPathProperty);

	LogInfo("\nDumper-7: bIsObjPtrInsteadOfFieldPathProperty = %d\n", Settings::Internal::bIsObjPtrInsteadOfFieldPathProperty);
}

void Settings::InitArrayDimSizeSettings()
{
	for (const UEObject Obj : ObjectArray())
	{
		if (!Obj.IsA(EClassCastFlags::Struct))
			continue;

		const UEStruct AsStruct = Obj.Cast<UEStruct>();

		for (const UEProperty Property : AsStruct.GetProperties())
		{
			if (Property.GetArrayDim() >= 0x000F0001)
			{
				Settings::Internal::bUseUint8ArrayDim = true;
				LogInfo("\nDumper-7: bUseUint8ArrayDim = %d\n", Settings::Internal::bUseUint8ArrayDim);
				return;
			}
		}
	}

	Settings::Internal::bUseUint8ArrayDim = false;
	LogInfo("\nDumper-7: bUseUint8ArrayDim = %d\n", Settings::Internal::bUseUint8ArrayDim);
}

// iOS port: no .ini config file. The user runs the dumper via the ImGui menu's
// "Start Dump" button, so there's nothing to load and no key to wait for.
void Settings::Config::Load()
{
}

void Settings::Config::DelayDumperStart()
{
	if (SleepTimeout > 0)
	{
		const int Millis = SleepTimeout < 1000 ? SleepTimeout * 1000 : SleepTimeout;
		std::this_thread::sleep_for(std::chrono::milliseconds(Millis));
	}
}
