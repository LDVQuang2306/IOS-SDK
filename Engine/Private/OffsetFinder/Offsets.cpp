#include <format>

#include "../../../Utils/Utils.h"

#include "../../Public/OffsetFinder/Offsets.h"
#include "../../Public/OffsetFinder/OffsetFinder.h"

#include "../../Public/Unreal/ObjectArray.h"
#include "../../Public/Unreal/NameArray.h"
#include "../../../Menu/Logger.h"

// @@TODO: Fix this shit for now init it manually
void Off::InSDK::ProcessEvent::InitPE()
{
	void** Vft = *(void***)ObjectArray::GetByIndex(0).GetAddress();

	/* Primary, and more reliable, check for ProcessEvent */
	auto IsProcessEvent = [](const uint8_t* FuncAddress, [[maybe_unused]] int32_t Index) -> bool
	{
		return FindPatternInRange({ 0xF7, -0x1, Off::UFunction::FunctionFlags, 0x0, 0x0, 0x0, 0x0, 0x04, 0x0, 0x0 }, FuncAddress, 0x400)
			&& FindPatternInRange({ 0xF7, -0x1, Off::UFunction::FunctionFlags, 0x0, 0x0, 0x0, 0x0, 0x0, 0x40, 0x0 }, FuncAddress, 0xF00);
	};

	const void* ProcessEventAddr = nullptr;
	int32_t ProcessEventIdx = 0;

	auto [FuncPtr, FuncIdx] = IterateVTableFunctions(Vft, IsProcessEvent);

	ProcessEventAddr = FuncPtr;
	ProcessEventIdx = FuncIdx;

	if (!FuncPtr)
	{
		/* ProcessEvent is sometimes located right after a func with the string L"Accessed None. Might as well check for it, because else we're going to crash anyways. */
		void* PossiblePEAddr = (void*)FindByWStringInAllSections(TEXT("Accessed None")).FindNextFunctionStart();

		auto IsSameAddr = [PossiblePEAddr](const uint8_t* FuncAddress, [[maybe_unused]] int32_t Index) -> bool
		{
			return FuncAddress == PossiblePEAddr;
		};

		auto [FuncPtr2, FuncIdx2] = IterateVTableFunctions(Vft, IsSameAddr);
		ProcessEventAddr = FuncPtr2;
		ProcessEventIdx = FuncIdx2;
	}

	if (ProcessEventAddr)
	{
		Off::InSDK::ProcessEvent::PEIndex = ProcessEventIdx;
		Off::InSDK::ProcessEvent::PEOffset = GetOffset(ProcessEventAddr);

		LogSuccess("ProcessEvent found - Offset: 0x%X, Index: 0x%X", Off::InSDK::ProcessEvent::PEOffset, ProcessEventIdx);
		return;
	}

	LogError("Couldn't find ProcessEvent!");
}

void Off::InSDK::ProcessEvent::InitPE(int32 Index, const char* const ModuleName)
{
	Off::InSDK::ProcessEvent::PEIndex = Index;

	void** VFT = *reinterpret_cast<void***>(ObjectArray::GetByIndex(0).GetAddress());

	uintptr_t Imagebase = GetModuleBase(ModuleName);

	Off::InSDK::ProcessEvent::PEOffset = uintptr_t(VFT[Off::InSDK::ProcessEvent::PEIndex]) - Imagebase;

	LogInfo("Manual ProcessEvent Override - VFT-Offset: 0x%X", uintptr_t(VFT) - Imagebase);
}

/* UWorld */
void Off::InSDK::World::InitGWorld()
{
	UEClass UWorld = ObjectArray::FindClassFast("World");

	for (UEObject Obj : ObjectArray())
	{
		if (Obj.HasAnyFlags(EObjectFlags::ClassDefaultObject) || !Obj.IsA(UWorld))
			continue;

		/* Try to find a pointer to the word, aka UWorld** GWorld */
		void* Result = FindAlignedValueInProcess(Obj.GetAddress());

		/* Pointer to UWorld* couldn't be found */
		if (Result)
		{
			Off::InSDK::World::GWorld = GetOffset(Result);
			LogSuccess("GWorld found - Offset: 0x%X", Off::InSDK::World::GWorld);
			break;
		}
	}

	if (Off::InSDK::World::GWorld == 0x0)
		LogError("GWorld WAS NOT FOUND!");
}


/* FText */
void Off::InSDK::Text::InitTextOffsets()
{
	if (!Off::InSDK::ProcessEvent::PEIndex)
	{
        LogError("\nDumper-7: Error, 'InitInSDKTextOffsets' was called before ProcessEvent was initialized!\n");
		return;
	}

	auto IsValidPtr = [](void* a) -> bool
	{
		return !IsBadReadPtr(a) && (uintptr_t(a) & 0x1) == 0; // realistically, there wont be any pointers to unaligned memory
	};


	UEFunction Conv_StringToText = ObjectArray::FindObjectFast<UEFunction>("Conv_StringToText", EClassCastFlags::Function);

	UEProperty InStringProp = nullptr;
	UEProperty ReturnProp = nullptr;

	for (UEProperty Prop : Conv_StringToText.GetProperties())
	{
		/* Func has 2 params, if the param is the return value assign to ReturnProp, else InStringProp*/
		(Prop.HasPropertyFlags(EPropertyFlags::ReturnParm) ? ReturnProp : InStringProp) = Prop;
	}

	const int32 ParamSize = Conv_StringToText.GetStructSize();

	const int32 FTextSize = ReturnProp.GetSize();

	const int32 StringOffset = InStringProp.GetOffset();
	const int32 ReturnValueOffset = ReturnProp.GetOffset();

	Off::InSDK::Text::TextSize = FTextSize;


	/* Allocate and zero-initialize ParamStruct */
#pragma warning(disable: 6255)
	uint8_t* ParamPtr = static_cast<uint8_t*>(__builtin_alloca(ParamSize));
	memset(ParamPtr, 0, ParamSize);

	/* Choose a, fairly random, string to later search for in FTextData */
	constexpr const TCHAR* StringText = TEXT("ThisIsAGoodString!");
	constexpr int32 StringLength = (sizeof(TEXT("ThisIsAGoodString!")) / sizeof(TCHAR));
	constexpr int32 StringLengthBytes = (sizeof(TEXT("ThisIsAGoodString!")));

	/* Initialize 'InString' in the ParamStruct */
	*reinterpret_cast<FString*>(ParamPtr + StringOffset) = StringText;

	/* This function is 'static' so the object on which we call it doesn't matter */
	ObjectArray::GetByIndex(0).ProcessEvent(Conv_StringToText, ParamPtr);

	uint8_t* FTextDataPtr = nullptr;

	/* Search for the first valid pointer inside of the FText and make the offset our 'TextDatOffset' */
	for (int32 i = 0; i < (FTextSize - sizeof(void*)); i += sizeof(void*))
	{
		void* PossibleTextDataPtr = *reinterpret_cast<void**>(ParamPtr + ReturnValueOffset + i);

		if (IsValidPtr(PossibleTextDataPtr))
		{
			FTextDataPtr = static_cast<uint8_t*>(PossibleTextDataPtr);

			Off::InSDK::Text::TextDatOffset = i;
			break;
		}
	}

	if (!FTextDataPtr)
	{
        LogError("\nDumper-7: Error, 'FTextDataPtr' could not be found!\n");
		return;
	}

	constexpr int32 MaxOffset = 0x50;
	constexpr int32 StartOffset = sizeof(void*); // FString::NumElements offset

	/* Search for a pointer pointing to a int32 Value (FString::NumElements) equal to StringLength */
	for (int32 i = StartOffset; i < MaxOffset; i += sizeof(int32))
	{
		TCHAR* PosibleStringPtr = *reinterpret_cast<TCHAR**>((FTextDataPtr + i) - 0x8);
		const int32 PossibleLength = *reinterpret_cast<int32*>(FTextDataPtr + i);

		/* Check if our length matches and see if the data before the length is a pointer to our StringText */
		if (PossibleLength == StringLength && PosibleStringPtr && IsValidPtr(PosibleStringPtr) && memcmp(StringText, PosibleStringPtr, StringLengthBytes) == 0)
		{
			Off::InSDK::Text::InTextDataStringOffset = (i - 0x8);
			break;
		}
	}

	LogSuccess("Off::InSDK::Text::TextSize: 0x%lx\n", Off::InSDK::Text::TextSize);
	LogSuccess("Off::InSDK::Text::TextDatOffset: 0x%lx\n", Off::InSDK::Text::TextDatOffset);
	LogSuccess("Off::InSDK::Text::InTextDataStringOffset: 0x%lx\n\n", Off::InSDK::Text::InTextDataStringOffset);
}

void Off::Init()
{
    auto OverwriteIfInvalidOffset = [](int32& Offset, int32 DefaultValue)
    {
        if (Offset == OffsetFinder::OffsetNotFound)
        {
            LogInfo("Defaulting to offset: 0x%X", DefaultValue);
            Offset = DefaultValue;
        }
    };

    LogInfo("Initializing Offsets...");

    Off::UObject::Flags = OffsetFinder::FindUObjectFlagsOffset();
    OverwriteIfInvalidOffset(Off::UObject::Flags, sizeof(void*)); // Default to right after VTable
    LogInfo("Off::UObject::Flags: 0x%X", Off::UObject::Flags);

    Off::UObject::Index = OffsetFinder::FindUObjectIndexOffset();
    OverwriteIfInvalidOffset(Off::UObject::Index, (Off::UObject::Flags + sizeof(int32))); // Default to right after Flags
    LogInfo("Off::UObject::Index: 0x%X", Off::UObject::Index);

    Off::UObject::Class = OffsetFinder::FindUObjectClassOffset();
    OverwriteIfInvalidOffset(Off::UObject::Class, (Off::UObject::Index + sizeof(int32))); // Default to right after Index
    LogInfo("Off::UObject::Class: 0x%X", Off::UObject::Class);

    Off::UObject::Outer = OffsetFinder::FindUObjectOuterOffset();
    LogInfo("Off::UObject::Outer: 0x%X", Off::UObject::Outer);

    Off::UObject::Name = OffsetFinder::FindUObjectNameOffset();
    OverwriteIfInvalidOffset(Off::UObject::Name, (Off::UObject::Class + sizeof(void*))); // Default to right after Class
    LogInfo("Off::UObject::Name: 0x%X", Off::UObject::Name);

    OverwriteIfInvalidOffset(Off::UObject::Outer, (Off::UObject::Name + sizeof(int32) + sizeof(int32)));  // Default to right after Name

    LogInfo("\nInitializing FName Settings...");
    OffsetFinder::InitFNameSettings();

    ::NameArray::PostInit();

    Off::UStruct::Children = OffsetFinder::FindChildOffset();
    LogInfo("Off::UStruct::Children: 0x%X", Off::UStruct::Children);

    Off::UField::Next = OffsetFinder::FindUFieldNextOffset();
    LogInfo("Off::UField::Next: 0x%X", Off::UField::Next);

    Off::UStruct::SuperStruct = OffsetFinder::FindSuperOffset();
    LogInfo("Off::UStruct::SuperStruct: 0x%X", Off::UStruct::SuperStruct);

    Off::UStruct::Size = OffsetFinder::FindStructSizeOffset();
    LogInfo("Off::UStruct::Size: 0x%X", Off::UStruct::Size);

    Off::UStruct::MinAlignemnt = OffsetFinder::FindMinAlignmentOffset();
    LogInfo("Off::UStruct::MinAlignemnt: 0x%X", Off::UStruct::MinAlignemnt);

    Off::UClass::CastFlags = OffsetFinder::FindCastFlagsOffset();
    LogInfo("Off::UClass::CastFlags: 0x%X", Off::UClass::CastFlags);

    // Castflags become available for use after this point

    if (Settings::Internal::bUseFProperty)
    {
        LogInfo("\nGame uses FProperty system\n");

        Off::UStruct::ChildProperties = OffsetFinder::FindChildPropertiesOffset();
        LogInfo("Off::UStruct::ChildProperties: 0x%X", Off::UStruct::ChildProperties);

        OffsetFinder::FixupHardcodedOffsets(); // must be called after FindChildPropertiesOffset

        Off::FField::Next = OffsetFinder::FindFFieldNextOffset();
        LogInfo("Off::FField::Next: 0x%X", Off::FField::Next);
        
        Off::FField::Name = OffsetFinder::FindFFieldNameOffset();
        LogInfo("Off::FField::Name: 0x%X", Off::FField::Name);

        /*
        * FNameSize might be wrong at this point of execution.
        * FField::Flags is not critical so a fix is only applied later in OffsetFinder::PostInitFNameSettings().
        */
        Off::FField::Flags = Off::FField::Name + Off::InSDK::Name::FNameSize;
        LogInfo("Off::FField::Flags: 0x%X", Off::FField::Flags);
    }

    Off::UClass::ClassDefaultObject = OffsetFinder::FindDefaultObjectOffset();
    LogInfo("Off::UClass::ClassDefaultObject: 0x%X", Off::UClass::ClassDefaultObject);

    Off::UClass::ImplementedInterfaces = OffsetFinder::FindImplementedInterfacesOffset();
    LogInfo("Off::UClass::ImplementedInterfaces: 0x%X", Off::UClass::ImplementedInterfaces);

    Off::UEnum::Names = OffsetFinder::FindEnumNamesOffset();
    LogInfo("Off::UEnum::Names: 0x%X", Off::UEnum::Names);

    Off::UFunction::FunctionFlags = OffsetFinder::FindFunctionFlagsOffset();
    LogInfo("Off::UFunction::FunctionFlags: 0x%X", Off::UFunction::FunctionFlags);

    Off::UFunction::ExecFunction = OffsetFinder::FindFunctionNativeFuncOffset();
    LogInfo("Off::UFunction::ExecFunction: 0x%X", Off::UFunction::ExecFunction);

    Off::Property::ElementSize = OffsetFinder::FindElementSizeOffset();
    LogInfo("Off::Property::ElementSize: 0x%X", Off::Property::ElementSize);

    Off::Property::ArrayDim = OffsetFinder::FindArrayDimOffset();
    LogInfo("Off::Property::ArrayDim: 0x%X", Off::Property::ArrayDim);

    Off::Property::Offset_Internal = OffsetFinder::FindOffsetInternalOffset();
    LogInfo("Off::Property::Offset_Internal: 0x%X", Off::Property::Offset_Internal);

    Off::Property::PropertyFlags = OffsetFinder::FindPropertyFlagsOffset();
    LogInfo("Off::Property::PropertyFlags: 0x%X", Off::Property::PropertyFlags);

    Off::InSDK::Properties::PropertySize = OffsetFinder::FindBoolPropertyBaseOffset();
    LogInfo("UPropertySize: 0x%X", Off::InSDK::Properties::PropertySize);

    Off::ArrayProperty::Inner = OffsetFinder::FindInnerTypeOffset(Off::InSDK::Properties::PropertySize);
    LogInfo("Off::ArrayProperty::Inner: 0x%X", Off::ArrayProperty::Inner);

    Off::SetProperty::ElementProp = OffsetFinder::FindSetPropertyBaseOffset(Off::InSDK::Properties::PropertySize);
    LogInfo("Off::SetProperty::ElementProp: 0x%X", Off::SetProperty::ElementProp);

    Off::MapProperty::Base = OffsetFinder::FindMapPropertyBaseOffset(Off::InSDK::Properties::PropertySize);
    LogInfo("Off::MapProperty::Base: 0x%X", Off::MapProperty::Base);

    Off::InSDK::ULevel::Actors = OffsetFinder::FindLevelActorsOffset();
    LogInfo("Off::InSDK::ULevel::Actors: 0x%X", Off::InSDK::ULevel::Actors);

    Off::InSDK::UDataTable::RowMap = OffsetFinder::FindDatatableRowMapOffset();
    LogInfo("Off::InSDK::UDataTable::RowMap: 0x%X", Off::InSDK::UDataTable::RowMap);

    OffsetFinder::PostInitFNameSettings();

    // Default property inheritance offsets
    Off::ByteProperty::Enum = Off::InSDK::Properties::PropertySize;
    Off::BoolProperty::Base = Off::InSDK::Properties::PropertySize;
    Off::ObjectProperty::PropertyClass = Off::InSDK::Properties::PropertySize;
    Off::StructProperty::Struct = Off::InSDK::Properties::PropertySize;
    Off::EnumProperty::Base = Off::InSDK::Properties::PropertySize;
    Off::DelegateProperty::SignatureFunction = Off::InSDK::Properties::PropertySize;
    Off::FieldPathProperty::FieldClass = Off::InSDK::Properties::PropertySize;
    Off::OptionalProperty::ValueProperty = Off::InSDK::Properties::PropertySize;

    Off::ClassProperty::MetaClass = Off::InSDK::Properties::PropertySize + 0x8; //0x8 inheritance from ObjectProperty
    
    LogSuccess("Offsets initialized successfully.");
}
void PropertySizes::Init()
{
	InitTDelegateSize();
	InitFFieldPathSize();
}

void PropertySizes::InitTDelegateSize()
{
	/* If the AudioComponent class or the OnQueueSubtitles member weren't found, fallback to looping GObjects and looking for a Delegate. */
	auto OnPropertyNotFoudn = [&]() -> void
	{
		for (UEObject Obj : ObjectArray())
		{
			if (!Obj.IsA(EClassCastFlags::Struct))
				continue;

			for (UEProperty Prop : Obj.Cast<UEClass>().GetProperties())
			{
				if (Prop.IsA(EClassCastFlags::DelegateProperty))
				{
					PropertySizes::DelegateProperty = Prop.GetSize();
					return;
				}
			}
		}
	};

	const UEClass AudioComponentClass = ObjectArray::FindClassFast("AudioComponent");

	if (!AudioComponentClass)
		return OnPropertyNotFoudn();

	const UEProperty OnQueueSubtitlesProp = AudioComponentClass.FindMember("OnQueueSubtitles", EClassCastFlags::DelegateProperty);

	if (!OnQueueSubtitlesProp)
		return OnPropertyNotFoudn();

	PropertySizes::DelegateProperty = OnQueueSubtitlesProp.GetSize();
}

void PropertySizes::InitFFieldPathSize()
{
	if (!Settings::Internal::bUseFProperty)
		return;

	/* If the SetFieldPathPropertyByName function or the Value parameter weren't found, fallback to looping GObjects and looking for a Delegate. */
	auto OnPropertyNotFoudn = [&]() -> void
	{
		for (UEObject Obj : ObjectArray())
		{
			if (!Obj.IsA(EClassCastFlags::Struct))
				continue;

			for (UEProperty Prop : Obj.Cast<UEClass>().GetProperties())
			{
				if (Prop.IsA(EClassCastFlags::FieldPathProperty))
				{
					PropertySizes::FieldPathProperty = Prop.GetSize();
					return;
				}
			}
		}
	};

	const UEFunction SetFieldPathPropertyByNameFunc = ObjectArray::FindObjectFast<UEFunction>("SetFieldPathPropertyByName", EClassCastFlags::Function);

	if (!SetFieldPathPropertyByNameFunc)
		return OnPropertyNotFoudn();

	const UEProperty ValueParamProp = SetFieldPathPropertyByNameFunc.FindMember("Value", EClassCastFlags::FieldPathProperty);

	if (!ValueParamProp)
		return OnPropertyNotFoudn();

	PropertySizes::FieldPathProperty = ValueParamProp.GetSize();
}
