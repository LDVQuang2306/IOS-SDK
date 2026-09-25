#include <format.h>
#include <thread>
#include <chrono>

#include "Utils.h"

#include "OffsetFinder/Offsets.h"
#include "OffsetFinder/OffsetFinder.h"

#include "Unreal/ObjectArray.h"
#include "Unreal/NameArray.h"

#include "Platform.h"
#include "Architecture.h"


#include "Menu/Logger.h"
void Off::InSDK::ProcessEvent::InitPE()
{
	UEObject FirstObj = ObjectArray::GetByIndex(0);
	if (!FirstObj.GetAddress())
	{
		LogError("InitPE: ObjectArray::GetByIndex(0) returned a null UObject");
		return;
	}

	void** Vft = *reinterpret_cast<void***>(FirstObj.GetAddress());
	if (!Vft || IsBadReadPtr(Vft))
	{
		LogError("InitPE: invalid vtable on UObject 0");
		return;
	}

	const int32_t FoundIdx = Architecture_x86_64::FindProcessEventIndex(Vft);
	const void*   FoundPtr = (FoundIdx >= 0) ? Vft[FoundIdx] : nullptr;

	if (!FoundPtr || FoundIdx < 0)
	{
		LogError("InitPE: ProcessEvent scorer found no candidate");
		return;
	}

	Off::InSDK::ProcessEvent::PEIndex  = FoundIdx;
	Off::InSDK::ProcessEvent::PEOffset = Platform::GetOffset(FoundPtr);

	LogInfo("PE-Index (auto): 0x%X", FoundIdx);
	LogInfo("PE-Offset: 0x%X", Off::InSDK::ProcessEvent::PEOffset);
}

void Off::InSDK::ProcessEvent::InitPE(const int32 Index, const char* const ModuleName)
{
	Off::InSDK::ProcessEvent::PEIndex = Index;

	void** VFT = *reinterpret_cast<void***>(ObjectArray::GetByIndex(0).GetAddress());

	Off::InSDK::ProcessEvent::PEOffset = Platform::GetOffset(VFT[Off::InSDK::ProcessEvent::PEIndex], ModuleName);

	LogInfo("PE-Index (manual): 0x%X", Index);
	LogInfo("PE-Offset: 0x%X", Off::InSDK::ProcessEvent::PEOffset);
}

/* UWorld */
void Off::InSDK::World::InitGWorld()
{
	LogInfo("InitGWorld: searching for UWorld** GWorld via in-process scan...");

	UEClass UWorld = ObjectArray::FindClassFast("World");
	if (!UWorld)
	{
		LogError("InitGWorld: UClass 'World' not found in ObjectArray");
		return;
	}

	for (UEObject Obj : ObjectArray())
	{
		if (Obj.HasAnyFlags(EObjectFlags::ClassDefaultObject) || !Obj.IsA(UWorld))
			continue;

		/* Try to find a pointer to the word, aka UWorld** GWorld */
		auto Results = Platform::FindAllAlignedValuesInProcess(Obj.GetAddress());

		void* Result = nullptr;
		if (Results.size())
		{
			if (Results.size() == 1)
			{
				Result = Results[0];
			}
			else if (Results.size() == 2)
			{
				auto ObjAddress = reinterpret_cast<uintptr_t>(Obj.GetAddress());
				auto PossibleGWorld = reinterpret_cast<volatile uintptr_t*>(Results[0]);
				auto CurrentValue = *PossibleGWorld;

				for (int i = 0; CurrentValue == ObjAddress && i < 50; ++i)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					CurrentValue = *PossibleGWorld;
				}
				if (CurrentValue == ObjAddress)
				{
					Result = Results[0];
				}
				else
				{
					Result = Results[1];
					LogInfo("InitGWorld: filtered out GActiveLogWorld at 0x%lX", reinterpret_cast<uintptr_t>(PossibleGWorld));
				}
			}
			else
			{
				LogError("InitGWorld: ambiguous - found %zu candidate GWorld pointers", Results.size());
			}
		}

		/* Pointer to UWorld* couldn't be found */
		if (Result)
		{
			Off::InSDK::World::GWorld = Platform::GetOffset(Result);
			LogSuccess("InitGWorld: GWorld-Offset = 0x%X", Off::InSDK::World::GWorld);
			break;
		}
	}

	if (Off::InSDK::World::GWorld == 0x0)
		LogError("InitGWorld: GWorld not found");
}

/* FText */
void Off::InSDK::Text::InitTextOffsets()
{
	LogInfo("InitTextOffsets: probing FText layout via Conv_StringToText...");

	if (!Off::InSDK::ProcessEvent::PEIndex)
	{
		LogError("InitTextOffsets: called before ProcessEvent was initialized (PEIndex == 0)");
		return;
	}

	auto IsValidPtr = [](void* a) -> bool
	{
		return !Platform::IsBadReadPtr(a) /* && (uintptr_t(a) & 0x1) == 0*/; // realistically, there wont be any pointers to unaligned memory
	};


	const UEFunction Conv_StringToText = ObjectArray::FindObjectFast<UEFunction>("Conv_StringToText", EClassCastFlags::Function);

	UEProperty InStringProp = nullptr;
	UEProperty ReturnProp = nullptr;

	if (!Conv_StringToText)
	{
		LogError("InitTextOffsets: Conv_StringToText UFunction not found");
		return;
	}

	for (UEProperty Prop : Conv_StringToText.GetProperties())
	{
		/* Func has 2 params, if the param is the return value assign to ReturnProp, else InStringProp*/
		if (Prop.HasPropertyFlags(EPropertyFlags::ReturnParm))
		{
			ReturnProp = Prop;
		}
		else
		{
			InStringProp = Prop;
		}
	}

	const int32 ParamSize = Conv_StringToText.GetStructSize();
	const int32 FTextSize = ReturnProp.GetSize();

	const int32 StringOffset = InStringProp.GetOffset();
	const int32 ReturnValueOffset = ReturnProp.GetOffset();

	Off::InSDK::Text::TextSize = FTextSize;


	/* Allocate and zero-initialize ParamStruct */
#pragma warning(disable: 6255)
	uint8_t* ParamPtr = static_cast<uint8_t*>(alloca(ParamSize));
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
		LogError("InitTextOffsets: FTextDataPtr not found inside FText return value");
		return;
	}

	constexpr int32 MaxOffset = 0x50;
	constexpr int32 StartOffset = sizeof(void*); // FString::NumElements offset

	/* Search for a pointer pointing to a int32 Value (FString::NumElements) equal to StringLength */
	for (int32 i = StartOffset; i < MaxOffset; i += sizeof(int32))
	{
		TCHAR* PosibleStringPtr = *reinterpret_cast<TCHAR**>((FTextDataPtr + i) - sizeof(void*));
		const int32 PossibleLength = *reinterpret_cast<int32*>(FTextDataPtr + i);

		if (PossibleLength == StringLength && PosibleStringPtr && IsValidPtr(PosibleStringPtr) && memcmp(StringText, PosibleStringPtr, StringLengthBytes) == 0)
		{
			Off::InSDK::Text::InTextDataStringOffset = (i - sizeof(void*));
			break;
		}
	}

	LogInfo("Off::InSDK::Text::TextSize: 0x%X", Off::InSDK::Text::TextSize);
	LogInfo("Off::InSDK::Text::TextDatOffset: 0x%X", Off::InSDK::Text::TextDatOffset);
	LogInfo("Off::InSDK::Text::InTextDataStringOffset: 0x%X", Off::InSDK::Text::InTextDataStringOffset);
	LogSuccess("InitTextOffsets: done");
}

void Off::Init()
{
	LogInfo("Off::Init: discovering UObject / UStruct / UFunction / UProperty offsets...");

	auto OverwriteIfInvalidOffset = [](int32& Offset, int32 DefaultValue)
	{
		if (Offset == OffsetFinder::OffsetNotFound)
		{
			LogInfo("  defaulting offset to 0x%X (auto-detect missed)", DefaultValue);
			Offset = DefaultValue;
		}
	};

	// --- UObject ---
	Off::UObject::Flags = OffsetFinder::FindUObjectFlagsOffset();
	OverwriteIfInvalidOffset(Off::UObject::Flags, sizeof(void*)); // right after VTable
	LogInfo("Off::UObject::Flags: 0x%X", Off::UObject::Flags);

	Off::UObject::Index = OffsetFinder::FindUObjectIndexOffset();
	OverwriteIfInvalidOffset(Off::UObject::Index, (Off::UObject::Flags + sizeof(int32))); // right after Flags
	LogInfo("Off::UObject::Index: 0x%X", Off::UObject::Index);

	Off::UObject::Class = OffsetFinder::FindUObjectClassOffset();
	OverwriteIfInvalidOffset(Off::UObject::Class, (Off::UObject::Index + sizeof(int32))); // right after Index
	LogInfo("Off::UObject::Class: 0x%X", Off::UObject::Class);

	Off::UObject::Outer = OffsetFinder::FindUObjectOuterOffset();
	LogInfo("Off::UObject::Outer: 0x%X", Off::UObject::Outer);

	Off::UObject::Name = OffsetFinder::FindUObjectNameOffset();
	OverwriteIfInvalidOffset(Off::UObject::Name, (Off::UObject::Class + sizeof(void*))); // right after Class
	LogInfo("Off::UObject::Name: 0x%X", Off::UObject::Name);

	OverwriteIfInvalidOffset(Off::UObject::Outer, (Off::UObject::Name + sizeof(int32) + sizeof(int32)));  // right after Name

	OffsetFinder::InitFNameSettings();

	::NameArray::PostInit();

	// --- UStruct / UField / UClass header ---
	// CastFlags must come first (FindChildOffset uses it).
	Off::UClass::CastFlags = OffsetFinder::FindCastFlagsOffset();
	LogInfo("Off::UClass::CastFlags: 0x%X", Off::UClass::CastFlags);

	Off::UStruct::Children = OffsetFinder::FindChildOffset();
	LogInfo("Off::UStruct::Children: 0x%X", Off::UStruct::Children);

	Off::UField::Next = OffsetFinder::FindUFieldNextOffset();
	LogInfo("Off::UField::Next: 0x%X", Off::UField::Next);

	Off::UStruct::SuperStruct = OffsetFinder::FindSuperOffset();
	LogInfo("Off::UStruct::SuperStruct: 0x%X", Off::UStruct::SuperStruct);

	Off::UStruct::Size = OffsetFinder::FindStructSizeOffset();
	LogInfo("Off::UStruct::Size: 0x%X", Off::UStruct::Size);

	Off::UStruct::MinAlignment = OffsetFinder::FindMinAlignmentOffset();
	LogInfo("Off::UStruct::MinAlignment: 0x%X", Off::UStruct::MinAlignment);

	Off::UClass::CastFlags = OffsetFinder::FindCastFlagsOffset();
	LogInfo("Off::UClass::CastFlags: 0x%X (recomputed)", Off::UClass::CastFlags);

	// CastFlags are now usable for downstream checks.

	if (Settings::Internal::bUseFProperty)
	{
		LogInfo("Game uses FProperty system (UE 4.25+)");

		Off::UStruct::ChildProperties = OffsetFinder::FindChildPropertiesOffset();
		LogInfo("Off::UStruct::ChildProperties: 0x%X", Off::UStruct::ChildProperties);

		OffsetFinder::FixupHardcodedOffsets(); // must run after FindChildPropertiesOffset

		Off::FField::Next = OffsetFinder::FindFFieldNextOffset();
		LogInfo("Off::FField::Next: 0x%X", Off::FField::Next);

		Off::FField::Class = OffsetFinder::FindFFieldClassOffset();
		LogInfo("Off::FField::Class: 0x%X", Off::FField::Class);

		// If you're crashing here, try NewFindFFieldNameOffset() instead.
		Off::FField::Name = OffsetFinder::FindFFieldNameOffset();

		if (Off::FField::Name == OffsetFinder::OffsetNotFound)
		{
			LogInfo("FindFFieldNameOffset missed - falling back to NewFindFFieldNameOffset()");
			Off::FField::Name = OffsetFinder::NewFindFFieldNameOffset();
		}

		LogInfo("Off::FField::Name: 0x%X", Off::FField::Name);

		/* FNameSize may be wrong at this point; FField::Flags isn't critical and gets
		 * fixed later in OffsetFinder::PostInitFNameSettings(). */
		Off::FField::Flags = Off::FField::Name + Off::InSDK::Name::FNameSize;
		LogInfo("Off::FField::Flags: 0x%X (provisional)", Off::FField::Flags);

		Off::FField::EditorOnlyMetadata = OffsetFinder::FindFFieldEditorOnlyMetaDataOffset();
		if (Off::FField::EditorOnlyMetadata != OffsetFinder::OffsetNotFound)
			LogInfo("Off::FField::EditorOnlyMetadata: 0x%X", Off::FField::EditorOnlyMetadata);
		else
			LogInfo("Off::FField::EditorOnlyMetadata: not present (shipping build)");

		Off::FFieldClass::CastFlags = OffsetFinder::FindFieldClassCastFlagsOffset();
		LogInfo("Off::FFieldClass::CastFlags: 0x%X", Off::FFieldClass::CastFlags);
	}
	else
	{
		LogInfo("Game uses legacy UProperty system (UE <= 4.24)");
	}

	Off::UStruct::StructBaseChain = OffsetFinder::FindStructBaseChainOffset();
	if (Off::UStruct::StructBaseChain != OffsetFinder::OffsetNotFound)
		LogInfo("Off::UStruct::StructBaseChain: 0x%X", Off::UStruct::StructBaseChain);
	else
		LogInfo("Off::UStruct::StructBaseChain: not present (using slower IsA path)");

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

	// --- Property ---
	Off::Property::ElementSize = OffsetFinder::FindElementSizeOffset();
	LogInfo("Off::Property::ElementSize: 0x%X", Off::Property::ElementSize);

	Off::Property::ArrayDim = OffsetFinder::FindArrayDimOffset();
	LogInfo("Off::Property::ArrayDim: 0x%X", Off::Property::ArrayDim);

	Off::Property::Offset_Internal = OffsetFinder::FindOffsetInternalOffset();
	LogInfo("Off::Property::Offset_Internal: 0x%X", Off::Property::Offset_Internal);

	Off::Property::PropertyFlags = OffsetFinder::FindPropertyFlagsOffset();
	LogInfo("Off::Property::PropertyFlags: 0x%X", Off::Property::PropertyFlags);

	Off::BoolProperty::Base = OffsetFinder::FindBoolPropertyBaseOffset();
	LogInfo("Off::BoolProperty::Base: 0x%X", Off::BoolProperty::Base);

	Off::EnumProperty::Base = OffsetFinder::FindEnumPropertyBaseOffset();
	LogInfo("Off::EnumProperty::Base: 0x%X", Off::EnumProperty::Base);

	if (Off::EnumProperty::Base == OffsetFinder::OffsetNotFound)
	{
		Off::InSDK::Properties::PropertySize = Off::BoolProperty::Base;
		Off::EnumProperty::Base = Off::BoolProperty::Base;
	}
	else
	{
		Off::InSDK::Properties::PropertySize = Off::EnumProperty::Base;
	}
	LogInfo("Off::InSDK::Properties::PropertySize: 0x%X", Off::InSDK::Properties::PropertySize);

	Off::ObjectProperty::PropertyClass = OffsetFinder::FindObjectPropertyClassOffset();
	OverwriteIfInvalidOffset(Off::ObjectProperty::PropertyClass, Off::InSDK::Properties::PropertySize);
	LogInfo("Off::ObjectProperty::PropertyClass: 0x%X", Off::ObjectProperty::PropertyClass);

	Off::ByteProperty::Enum = OffsetFinder::FindBytePropertyEnumOffset();
	OverwriteIfInvalidOffset(Off::ByteProperty::Enum, Off::InSDK::Properties::PropertySize);
	LogInfo("Off::ByteProperty::Enum: 0x%X", Off::ByteProperty::Enum);

	Off::StructProperty::Struct = OffsetFinder::FindStructPropertyStructOffset();
	OverwriteIfInvalidOffset(Off::StructProperty::Struct, Off::InSDK::Properties::PropertySize);
	LogInfo("Off::StructProperty::Struct: 0x%X", Off::StructProperty::Struct);

	Off::DelegateProperty::SignatureFunction = OffsetFinder::FindDelegatePropertySignatureFunctionOffset();
	OverwriteIfInvalidOffset(Off::DelegateProperty::SignatureFunction, Off::InSDK::Properties::PropertySize);
	LogInfo("Off::DelegateProperty::SignatureFunction: 0x%X", Off::DelegateProperty::SignatureFunction);

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

	Off::FieldPathProperty::FieldClass = Off::InSDK::Properties::PropertySize;
	Off::OptionalProperty::ValueProperty = Off::InSDK::Properties::PropertySize;

	Off::ClassProperty::MetaClass = Off::ObjectProperty::PropertyClass + sizeof(void*); // 0x8 inherited from ObjectProperty

	LogSuccess("Off::Init: done");
}

void PropertySizes::Init()
{
	InitTDelegateSize();
	InitFFieldPathSize();
	InitTMulticastInlineDelegateSize();
}

void PropertySizes::InitTDelegateSize()
{
	/* If the AudioComponent class or the OnQueueSubtitles member weren't found, fallback to looping GObjects and looking for a Delegate. */
	auto OnPropertyNotFound = [&]() -> void
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
		return OnPropertyNotFound();

	const UEProperty OnQueueSubtitlesProp = AudioComponentClass.FindMember("OnQueueSubtitles", EClassCastFlags::DelegateProperty);

	if (!OnQueueSubtitlesProp)
		return OnPropertyNotFound();

	PropertySizes::DelegateProperty = OnQueueSubtitlesProp.GetSize();
}

void PropertySizes::InitFFieldPathSize()
{
	if (!Settings::Internal::bUseFProperty)
		return;

	/* If the SetFieldPathPropertyByName function or the Value parameter weren't found, fallback to looping GObjects and looking for a Delegate. */
	auto OnPropertyNotFound = [&]() -> void
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
		return OnPropertyNotFound();

	const UEProperty ValueParamProp = SetFieldPathPropertyByNameFunc.FindMember("Value", EClassCastFlags::FieldPathProperty);

	if (!ValueParamProp)
		return OnPropertyNotFound();

	PropertySizes::FieldPathProperty = ValueParamProp.GetSize();
}

void PropertySizes::InitTMulticastInlineDelegateSize()
{
	/* If the AudioComponent class or the OnQueueSubtitles member weren't found, fallback to looping GObjects and looking for a Delegate. */
	auto OnPropertyNotFound = [&]() -> void
		{
			for (UEObject Obj : ObjectArray())
			{
				if (!Obj.IsA(EClassCastFlags::Struct))
					continue;

				for (UEProperty Prop : Obj.Cast<UEClass>().GetProperties())
				{
					if (Prop.IsA(EClassCastFlags::MulticastInlineDelegateProperty))
					{
						PropertySizes::DelegateProperty = Prop.GetSize();
						return;
					}
				}
			}
		};

	const UEClass EmitterClass = ObjectArray::FindClassFast("Emitter");

	if (!EmitterClass)
		return OnPropertyNotFound();

	const UEProperty OnParticleSpawn = EmitterClass.FindMember("OnParticleSpawn", EClassCastFlags::MulticastDelegateProperty);

	if (!OnParticleSpawn)
		return OnPropertyNotFound();

	PropertySizes::MulticastInlineDelegateProperty = OnParticleSpawn.GetSize();
}