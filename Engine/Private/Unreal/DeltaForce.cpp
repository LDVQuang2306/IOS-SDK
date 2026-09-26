#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <format.h>

#include "Unreal/DeltaForce.h"
#include "Unreal/ObjectArray.h"
#include "Unreal/NameArray.h"
#include "OffsetFinder/Offsets.h"
#include "OffsetFinder/OffsetFinder.h"
#include "Utils.h"
#include "Menu/Logger.h"

#include "DeltaForceDiscovery.h"


namespace DeltaForce
{
namespace
{
	FProfile GProfile;

	constexpr uint32 RF_ClassDefaultObject = 0x10;
	constexpr uint32 FUNC_Native = 0x400;

	int32 FindExactImageIndex(const char* Name)
	{
		const uint32 Count = _dyld_image_count();

		for (uint32 i = 0; i < Count; i++)
		{
			if (strcmp(GetImageFileName(_dyld_get_image_name(i)), Name) == 0)
				return static_cast<int32>(i);
		}

		return -1;
	}

	/* ------------------------------------------------------------------ */
	/* Kernel-read helpers. Used until the deeper reflection layout is    */
	/* verified; afterwards the generator may dereference directly.       */
	/* ------------------------------------------------------------------ */

	std::string SafeNameAt(uintptr_t FNameAddress)
	{
		uint32 NameData[2] = { 0x0, 0x0 };

		if (!ReadMemory(FNameAddress, NameData, sizeof(NameData)))
			return {};

		/* FName wraps the local copy, name lookups themselves only use kernel reads */
		return FName(NameData).ToString();
	}

	bool IsRealUObject(uintptr_t Ptr)
	{
		if (!IsPlausiblePointer(Ptr))
			return false;

		const int32 Index = SafeRead<int32>(Ptr + Off::UObject::Index, -1);

		if (Index < 0 || Index >= ObjectArray::Num())
			return false;

		return reinterpret_cast<uintptr_t>(ObjectArray::GetByIndex(Index).GetAddress()) == Ptr;
	}

	bool IsObjectOfClass(uintptr_t Object, const char* ClassName)
	{
		if (!IsRealUObject(Object))
			return false;

		uintptr_t Class = SafeRead<uintptr_t>(Object + Off::UObject::Class);

		for (int Depth = 0; Depth < 64 && Class; Depth++)
		{
			if (!IsRealUObject(Class))
				return false;

			if (SafeNameAt(Class + Off::UObject::Name) == ClassName)
				return true;

			Class = SafeRead<uintptr_t>(Class + Off::UStruct::SuperStruct);
		}

		return false;
	}

	std::string FieldClassNameOf(uintptr_t Field)
	{
		if (!IsPlausiblePointer(Field))
			return {};

		const uintptr_t FieldClass = SafeRead<uintptr_t>(Field + Off::FField::Class);

		if (!IsPlausiblePointer(FieldClass))
			return {};

		return SafeNameAt(FieldClass + Off::FFieldClass::Name);
	}

	bool IsPropertyField(uintptr_t Field)
	{
		const std::string Name = FieldClassNameOf(Field);

		return Name.size() > 8 && Name.ends_with("Property");
	}

	/* ------------------------------------------------------------------ */
	/* Profile -> Dumper-7                                                */
	/* ------------------------------------------------------------------ */

	void ApplyProfile(const FProfile& P)
	{
		using namespace Settings::Internal;

		bUseFProperty = true;
		bUseNamePool = true;
		bUseCasePreservingName = false;
		bUseOutlineNumberName = false;
		bUseMaskForFieldOwner = false;
		bIsEnumNameOnly = false;
		bIsSmallEnumValue = false;
		bIsObjectNameBeforeClass = P.ObjectName < P.ObjectClass;
		bUseNameBasedCastFlags = true;
		bIsDeltaForce = true;

		Off::UObject::Vft = 0x0;
		Off::UObject::Class = P.ObjectClass;
		Off::UObject::Outer = P.ObjectOuter;
		Off::UObject::Name = P.ObjectName;
		Off::UObject::Index = P.ObjectIndex;
		Off::UObject::Flags = OffsetFinder::OffsetNotFound; // Detected later

		Off::FName::CompIdx = 0x0;
		Off::FName::Number = 0x4;
		Off::InSDK::Name::FNameSize = 0x8;

		Off::UField::Next = P.FieldNext;

		Off::UStruct::SuperStruct = P.StructSuper;
		Off::UStruct::Children = P.StructChildren;
		Off::UStruct::ChildProperties = P.StructProperties;
		Off::UStruct::Size = P.StructSize;
		Off::UStruct::MinAlignment = P.StructAlignment;

		Off::UFunction::FunctionFlags = P.FunctionFlags;
		Off::UFunction::ExecFunction = P.FunctionNative;

		Off::UEnum::Names = P.EnumNames;

		Off::UClass::CastFlags = OffsetFinder::OffsetNotFound;
		Off::UClass::ClassDefaultObject = OffsetFinder::OffsetNotFound;
		Off::UClass::ImplementedInterfaces = OffsetFinder::OffsetNotFound;

		Off::FField::Vft = 0x0;
		Off::FField::Owner = P.FFieldOwner;
		Off::FField::Next = P.FFieldNext;
		Off::FField::Class = P.FFieldClass;
		Off::FField::Name = P.FFieldName;
		Off::FField::Flags = P.FFieldName + Off::InSDK::Name::FNameSize;

		Off::FFieldClass::Name = P.FFieldClassName;
		Off::FFieldClass::Id = P.FFieldClassName + 0x08;
		Off::FFieldClass::CastFlags = P.FFieldClassName + 0x10;
		Off::FFieldClass::ClassFlags = P.FFieldClassName + 0x18;
		Off::FFieldClass::SuperClass = P.FFieldClassName + 0x20;

		Off::Property::ArrayDim = P.PropertyDim;
		Off::Property::ElementSize = P.PropertySize;
		Off::Property::PropertyFlags = P.PropertyFlags;
		Off::Property::Offset_Internal = P.PropertyOffset;

		Off::InSDK::Properties::PropertySize = P.PropertyBaseSize;

		/* Defaults of the verified profile, re-checked against live properties by ProbePropertyOffsets() */
		Off::ByteProperty::Enum = P.PropertyPayload;
		Off::BoolProperty::Base = P.BoolPayload;
		Off::ObjectProperty::PropertyClass = P.PropertyPayload;
		Off::ClassProperty::MetaClass = P.PropertyPayload + 0x8;
		Off::StructProperty::Struct = P.PropertyPayload;
		Off::ArrayProperty::Inner = P.PropertyPayload;
		Off::DelegateProperty::SignatureFunction = P.PropertyPayload;
		Off::MapProperty::Base = P.MapKey;
		Off::SetProperty::ElementProp = P.SetElement;
		Off::EnumProperty::Base = P.EnumPropertyUnderlying;
		Off::FieldPathProperty::FieldClass = P.PropertyPayload;
		Off::OptionalProperty::ValueProperty = P.PropertyBaseSize;

		Off::InSDK::ULevel::Actors = OffsetFinder::OffsetNotFound;
	}

	/* ------------------------------------------------------------------ */
	/* Verification of the deeper reflection layout (kernel reads only)   */
	/* ------------------------------------------------------------------ */

	struct FCoreObjects
	{
		uintptr_t ClassClass = 0x0;
		uintptr_t StructClass = 0x0;
		uintptr_t FieldClass = 0x0;
		uintptr_t ObjectClass = 0x0;
		uintptr_t Guid = 0x0;
		uintptr_t Actor = 0x0;
		uintptr_t NetRole = 0x0;
	};

	bool FindCoreObjects(FCoreObjects& Out, std::string& Error)
	{
		struct FWanted
		{
			const char* Class;
			const char* Package;
			const char* Name;
			uintptr_t* Result;
			bool bRequired;
		};

		FWanted Wanted[] =
		{
			{ "Class", "CoreUObject", "Class", &Out.ClassClass, true },
			{ "Class", "CoreUObject", "Struct", &Out.StructClass, true },
			{ "Class", "CoreUObject", "Field", &Out.FieldClass, true },
			{ "Class", "CoreUObject", "Object", &Out.ObjectClass, true },
			{ "ScriptStruct", "CoreUObject", "Guid", &Out.Guid, true },
			{ "Class", "Engine", "Actor", &Out.Actor, true },
			{ "Enum", "Engine", "ENetRole", &Out.NetRole, false },
		};

		size_t Remaining = std::size(Wanted);
		const int32 NumObjects = ObjectArray::Num();

		for (int32 i = 0; i < NumObjects && Remaining > 0; i++)
		{
			UEObject Obj = ObjectArray::GetByIndex(i);

			if (!Obj)
				continue;

			const std::string Name = Obj.GetName();

			for (FWanted& Entry : Wanted)
			{
				if (*Entry.Result || Name != Entry.Name)
					continue;

				if (Obj.GetClass().GetName() != Entry.Class || Obj.GetOutermost().GetName() != Entry.Package)
					continue;

				*Entry.Result = reinterpret_cast<uintptr_t>(Obj.GetAddress());
				Remaining--;
			}
		}

		for (const FWanted& Entry : Wanted)
		{
			if (!*Entry.Result && Entry.bRequired)
			{
				Error = fmt::format("core object '{} {}.{}' not found", Entry.Class, Entry.Package, Entry.Name);
				return false;
			}
		}

		return true;
	}

	bool VerifyReflectionLayout(const FCoreObjects& Core, std::string& Error)
	{
		/* UStruct::SuperStruct: Class -> Struct -> Field -> Object -> null */
		auto Super = [](uintptr_t Struct) { return SafeRead<uintptr_t>(Struct + Off::UStruct::SuperStruct, ~uintptr_t(0)); };

		if (Super(Core.ClassClass) != Core.StructClass || Super(Core.StructClass) != Core.FieldClass || Super(Core.FieldClass) != Core.ObjectClass || Super(Core.ObjectClass) != 0x0)
		{
			Error = fmt::format("UStruct::SuperStruct (0x{:X}) doesn't link Class->Struct->Field->Object", Off::UStruct::SuperStruct);
			return false;
		}

		/* UStruct::Size / MinAlignment */
		const int32 GuidSize = SafeRead<int32>(Core.Guid + Off::UStruct::Size, -1);
		const int32 GuidAlignment = SafeRead<int32>(Core.Guid + Off::UStruct::MinAlignment, -1);

		if (GuidSize != 0x10 || GuidAlignment != 0x4)
		{
			Error = fmt::format("UStruct::Size (0x{:X}) / MinAlignment (0x{:X}) wrong: FGuid size={} alignment={}", Off::UStruct::Size, Off::UStruct::MinAlignment, GuidSize, GuidAlignment);
			return false;
		}

		/* UStruct::ChildProperties, FField::Next/Name/Class, FProperty::ArrayDim/ElementSize/Offset */
		const uintptr_t FirstGuidProperty = SafeRead<uintptr_t>(Core.Guid + Off::UStruct::ChildProperties);
		uintptr_t Field = FirstGuidProperty;

		static constexpr const char* GuidMembers[] = { "A", "B", "C", "D" };

		for (int32 i = 0; i < 4; i++)
		{
			const std::string Name = IsPlausiblePointer(Field) ? SafeNameAt(Field + Off::FField::Name) : "";
			const std::string Kind = FieldClassNameOf(Field);
			const int32 ArrayDim = SafeRead<int32>(Field + Off::Property::ArrayDim, -1);
			const int32 ElementSize = SafeRead<int32>(Field + Off::Property::ElementSize, -1);
			const int32 Offset = SafeRead<int32>(Field + Off::Property::Offset_Internal, -1);

			if (Name != GuidMembers[i] || !Kind.ends_with("Property") || ArrayDim != 1 || ElementSize != 4 || Offset != i * 4)
			{
				Error = fmt::format("FField/FProperty layout wrong at FGuid member {}: name='{}' kind='{}' dim={} size={} offset={}", i, Name, Kind, ArrayDim, ElementSize, Offset);
				return false;
			}

			Field = SafeRead<uintptr_t>(Field + Off::FField::Next);
		}

		/* FField::Owner is a FFieldVariant: { UObject* or FField*, bool bIsUObject } or a tagged pointer */
		const uintptr_t OwnerValue = SafeRead<uintptr_t>(FirstGuidProperty + Off::FField::Owner);

		if (OwnerValue == Core.Guid && SafeRead<uint8>(FirstGuidProperty + Off::FField::Owner + 0x8) == 1)
		{
			Settings::Internal::bUseMaskForFieldOwner = false;
		}
		else if (OwnerValue == (Core.Guid | 0x1))
		{
			Settings::Internal::bUseMaskForFieldOwner = true;
		}
		else
		{
			LogError("[DeltaForce] FField::Owner (0x%X) not verified (value 0x%llX), owner lookups are unreliable", Off::FField::Owner, static_cast<unsigned long long>(OwnerValue));
		}

		/* UStruct::Children, UField::Next, UFunction::FunctionFlags */
		uintptr_t Child = SafeRead<uintptr_t>(Core.Actor + Off::UStruct::Children);
		int32 NumFunctions = 0;
		int32 NumNative = 0;

		for (int32 Steps = 0; Child && Steps < 0x4000; Steps++)
		{
			if (!IsRealUObject(Child))
			{
				Error = fmt::format("UStruct::Children (0x{:X}) / UField::Next (0x{:X}) don't point to UObjects", Off::UStruct::Children, Off::UField::Next);
				return false;
			}

			if (IsObjectOfClass(Child, "Function"))
			{
				NumFunctions++;

				if (SafeRead<uint32>(Child + Off::UFunction::FunctionFlags) & FUNC_Native)
					NumNative++;
			}

			Child = SafeRead<uintptr_t>(Child + Off::UField::Next);
		}

		if (NumFunctions < 5 || NumNative * 2 < NumFunctions)
		{
			Error = fmt::format("AActor functions not found or UFunction::FunctionFlags (0x{:X}) wrong: {} functions, {} native", Off::UFunction::FunctionFlags, NumFunctions, NumNative);
			return false;
		}

		/* UEnum::Names: TArray<TPair<FName, int64>> */
		if (Core.NetRole)
		{
			const uintptr_t Data = SafeRead<uintptr_t>(Core.NetRole + Off::UEnum::Names);
			const int32 Num = SafeRead<int32>(Core.NetRole + Off::UEnum::Names + 0x8, -1);
			const int32 Max = SafeRead<int32>(Core.NetRole + Off::UEnum::Names + 0xC, -1);

			const std::string FirstName = IsPlausiblePointer(Data) ? SafeNameAt(Data) : "";

			if (!IsPlausiblePointer(Data) || Num < 3 || Num > 64 || Max < Num || FirstName.find("ROLE_None") == std::string::npos ||
				SafeRead<int64>(Data + 0x8, -1) != 0 || SafeRead<int64>(Data + 0x18, -1) != 1)
			{
				Error = fmt::format("UEnum::Names (0x{:X}) wrong: ENetRole Num={} Max={} First='{}'", Off::UEnum::Names, Num, Max, FirstName);
				return false;
			}
		}

		return true;
	}

	/* RF_ClassDefaultObject must be set on every "Default__" object and (almost) nowhere else */
	int32 FindObjectFlagsOffset()
	{
		const int32 NumObjects = std::min(ObjectArray::Num(), 0x10000);

		std::vector<std::pair<uint8*, bool>> Objects;
		Objects.reserve(NumObjects);

		for (int32 i = 0; i < NumObjects; i++)
		{
			UEObject Obj = ObjectArray::GetByIndex(i);

			if (Obj)
				Objects.emplace_back(static_cast<uint8*>(Obj.GetAddress()), Obj.GetName().starts_with("Default__"));
		}

		const int32 HeaderEnd = std::max({ Off::UObject::Class + 0x8, Off::UObject::Outer + 0x8, Off::UObject::Name + 0x8, Off::UObject::Index + 0x4 });

		for (int32 Offset = 0x8; Offset + 0x4 <= HeaderEnd; Offset += 0x4)
		{
			auto Overlaps = [Offset](int32 Start, int32 Size) { return Offset < Start + Size && Start < Offset + 0x4; };

			if (Overlaps(Off::UObject::Class, 0x8) || Overlaps(Off::UObject::Outer, 0x8) || Overlaps(Off::UObject::Name, 0x8) || Overlaps(Off::UObject::Index, 0x4))
				continue;

			int32 NumCDOs = 0, NumCDOsWithFlag = 0, NumOthers = 0, NumOthersWithFlag = 0;

			for (const auto& [Address, bIsCDO] : Objects)
			{
				/* Inside of the UObject header, a direct read is safe */
				const bool bHasFlag = *reinterpret_cast<uint32*>(Address + Offset) & RF_ClassDefaultObject;

				(bIsCDO ? NumCDOs : NumOthers)++;
				(bIsCDO ? NumCDOsWithFlag : NumOthersWithFlag) += bHasFlag;
			}

			if (NumCDOs >= 16 && NumCDOsWithFlag == NumCDOs && NumOthersWithFlag * 200 <= NumOthers)
				return Offset;
		}

		return OffsetFinder::OffsetNotFound;
	}

	/* ------------------------------------------------------------------ */
	/* FProperty payload offsets                                          */
	/* ------------------------------------------------------------------ */

	using FSamples = std::unordered_map<std::string, std::vector<uintptr_t>>;

	FSamples CollectPropertySamples(size_t MaxPerKind)
	{
		FSamples Samples;

		const int32 NumObjects = ObjectArray::Num();

		for (int32 i = 0; i < NumObjects; i++)
		{
			UEObject Obj = ObjectArray::GetByIndex(i);

			if (!Obj || !Obj.IsA(EClassCastFlags::Struct))
				continue;

			int32 Steps = 0;
			for (UEFField Field = Obj.Cast<UEStruct>().GetChildProperties(); Field && Steps < 0x4000; Field = Field.GetNext(), Steps++)
			{
				std::vector<uintptr_t>& KindSamples = Samples[Field.GetClass().GetName()];

				if (KindSamples.size() < MaxPerKind)
					KindSamples.push_back(reinterpret_cast<uintptr_t>(Field.GetAddress()));
			}
		}

		return Samples;
	}

	/* Check returns 1 = evidence for the offset, -1 = contradiction, 0 = no evidence (e.g. nullptr) */
	int32 ChooseOffset(const char* What, const std::vector<uintptr_t>& Samples, int32 Default, const std::function<int32(uintptr_t, int32)>& Check)
	{
		if (Samples.empty())
		{
			LogInfo("[DeltaForce] %s: no samples, using 0x%X", What, Default);
			return Default;
		}

		std::vector<int32> Candidates = { Default };
		for (int32 Offset = 0x78; Offset <= 0x98; Offset += 0x8)
		{
			if (Offset != Default)
				Candidates.push_back(Offset);
		}

		for (const int32 Offset : Candidates)
		{
			int32 NumGood = 0;
			int32 NumBad = 0;

			for (const uintptr_t Sample : Samples)
			{
				const int32 Result = Check(Sample, Offset);

				NumGood += Result > 0;
				NumBad += Result < 0;
			}

			if (NumBad == 0 && NumGood >= std::min<int32>(3, static_cast<int32>(Samples.size())) && NumGood > 0)
			{
				if (Offset != Default)
					LogInfo("[DeltaForce] %s: live data contradicts 0x%X, using 0x%X (%d/%zu samples)", What, Default, Offset, NumGood, Samples.size());
				else
					LogInfo("[DeltaForce] %s: 0x%X verified (%d/%zu samples)", What, Offset, NumGood, Samples.size());

				return Offset;
			}
		}

		LogError("[DeltaForce] %s: no offset could be verified (%zu samples), keeping 0x%X", What, Samples.size(), Default);
		return Default;
	}

	std::vector<uintptr_t> Merge(const FSamples& Samples, std::initializer_list<const char*> Kinds)
	{
		std::vector<uintptr_t> Result;

		for (const char* Kind : Kinds)
		{
			auto It = Samples.find(Kind);

			if (It != Samples.end())
				Result.insert(Result.end(), It->second.begin(), It->second.end());
		}

		return Result;
	}

	void ProbePropertyOffsets()
	{
		const FSamples Samples = CollectPropertySamples(48);

		auto PointerTo = [](const char* ClassName, bool bAllowNull)
		{
			return [ClassName, bAllowNull](uintptr_t Property, int32 Offset) -> int32
			{
				const uintptr_t Target = SafeRead<uintptr_t>(Property + Offset, ~uintptr_t(0));

				if (Target == 0x0)
					return bAllowNull ? 0 : -1;

				return IsObjectOfClass(Target, ClassName) ? 1 : -1;
			};
		};

		auto PropertyAt = [](uintptr_t Property, int32 Offset) -> int32
		{
			return IsPropertyField(SafeRead<uintptr_t>(Property + Offset)) ? 1 : -1;
		};

		Off::StructProperty::Struct = ChooseOffset("FStructProperty::Struct", Merge(Samples, { "StructProperty" }), Off::StructProperty::Struct,
			[](uintptr_t Property, int32 Offset) -> int32
			{
				const uintptr_t Struct = SafeRead<uintptr_t>(Property + Offset);

				if (!IsObjectOfClass(Struct, "ScriptStruct"))
					return -1;

				return SafeRead<int32>(Struct + Off::UStruct::Size, -1) == SafeRead<int32>(Property + Off::Property::ElementSize, -2) ? 1 : -1;
			});

		Off::ObjectProperty::PropertyClass = ChooseOffset("FObjectPropertyBase::PropertyClass",
			Merge(Samples, { "ObjectProperty", "ObjectPtrProperty", "ClassProperty", "WeakObjectProperty", "LazyObjectProperty", "SoftObjectProperty", "SoftClassProperty", "InterfaceProperty" }),
			Off::ObjectProperty::PropertyClass, PointerTo("Class", true));

		Off::ClassProperty::MetaClass = ChooseOffset("FClassProperty::MetaClass", Merge(Samples, { "ClassProperty", "ClassPtrProperty", "SoftClassProperty" }),
			Off::ObjectProperty::PropertyClass + 0x8, PointerTo("Class", true));

		/* PropertyClass of a class property is always a UClass too, it must not be mistaken for MetaClass */
		if (Off::ClassProperty::MetaClass == Off::ObjectProperty::PropertyClass)
			Off::ClassProperty::MetaClass = Off::ObjectProperty::PropertyClass + 0x8;

		Off::ByteProperty::Enum = ChooseOffset("FByteProperty::Enum", Merge(Samples, { "ByteProperty" }), Off::ByteProperty::Enum, PointerTo("Enum", true));

		Off::ArrayProperty::Inner = ChooseOffset("FArrayProperty::Inner", Merge(Samples, { "ArrayProperty" }), Off::ArrayProperty::Inner, PropertyAt);

		Off::SetProperty::ElementProp = ChooseOffset("FSetProperty::ElementProp", Merge(Samples, { "SetProperty" }), Off::SetProperty::ElementProp, PropertyAt);

		Off::MapProperty::Base = ChooseOffset("FMapProperty::KeyProp/ValueProp", Merge(Samples, { "MapProperty" }), Off::MapProperty::Base,
			[&](uintptr_t Property, int32 Offset) -> int32
			{
				return PropertyAt(Property, Offset) > 0 && PropertyAt(Property, Offset + 0x8) > 0 ? 1 : -1;
			});

		Off::EnumProperty::Base = ChooseOffset("FEnumProperty::UnderlyingProp/Enum", Merge(Samples, { "EnumProperty" }), Off::EnumProperty::Base,
			[&](uintptr_t Property, int32 Offset) -> int32
			{
				const std::string Underlying = FieldClassNameOf(SafeRead<uintptr_t>(Property + Offset));

				if (!Underlying.ends_with("Property"))
					return -1;

				return IsObjectOfClass(SafeRead<uintptr_t>(Property + Offset + 0x8), "Enum") ? 1 : -1;
			});

		Off::DelegateProperty::SignatureFunction = ChooseOffset("FDelegateProperty::SignatureFunction",
			Merge(Samples, { "DelegateProperty", "MulticastInlineDelegateProperty", "MulticastSparseDelegateProperty", "MulticastDelegateProperty" }),
			Off::DelegateProperty::SignatureFunction, PointerTo("Function", true));

		Off::FieldPathProperty::FieldClass = ChooseOffset("FFieldPathProperty::PropertyClass", Merge(Samples, { "FieldPathProperty" }), Off::FieldPathProperty::FieldClass,
			[](uintptr_t Property, int32 Offset) -> int32
			{
				const uintptr_t FieldClass = SafeRead<uintptr_t>(Property + Offset, ~uintptr_t(0));

				if (FieldClass == 0x0)
					return 0;

				if (!IsPlausiblePointer(FieldClass))
					return -1;

				const std::string Name = SafeNameAt(FieldClass + Off::FFieldClass::Name);
				return Name.ends_with("Property") || Name == "Field" ? 1 : -1;
			});

		/* FBoolProperty { uint8 FieldSize; uint8 ByteOffset; uint8 ByteMask; uint8 FieldMask; } can start at an odd offset */
		const std::vector<uintptr_t> BoolSamples = Merge(Samples, { "BoolProperty" });
		const int32 DefaultBool = Off::BoolProperty::Base;

		auto IsValidBool = [](uintptr_t Property, int32 Offset) -> bool
		{
			const uint32 Packed = SafeRead<uint32>(Property + Offset, 0x0);

			const uint8 FieldSize = Packed & 0xFF;
			const uint8 ByteOffset = (Packed >> 8) & 0xFF;
			const uint8 ByteMask = (Packed >> 16) & 0xFF;
			const uint8 FieldMask = (Packed >> 24) & 0xFF;

			const bool bIsPow2Byte = ByteMask != 0 && (ByteMask & (ByteMask - 1)) == 0;
			const bool bIsValidFieldMask = FieldMask == 0xFF || (FieldMask != 0 && (FieldMask & (FieldMask - 1)) == 0);

			return FieldSize >= 1 && FieldSize <= 8 && ByteOffset < FieldSize && bIsPow2Byte && bIsValidFieldMask &&
				SafeRead<int32>(Property + Off::Property::ElementSize, -1) == FieldSize;
		};

		if (!BoolSamples.empty())
		{
			std::vector<int32> Candidates = { DefaultBool };
			for (int32 Offset = Off::InSDK::Properties::PropertySize - 0x8; Offset <= Off::InSDK::Properties::PropertySize + 0x10; Offset++)
			{
				if (Offset != DefaultBool)
					Candidates.push_back(Offset);
			}

			bool bFound = false;
			for (const int32 Offset : Candidates)
			{
				if (std::all_of(BoolSamples.begin(), BoolSamples.end(), [&](uintptr_t Property) { return IsValidBool(Property, Offset); }))
				{
					Off::BoolProperty::Base = Offset;
					bFound = true;
					break;
				}
			}

			if (bFound)
				LogInfo("[DeltaForce] FBoolProperty base: 0x%X (%zu samples)", Off::BoolProperty::Base, BoolSamples.size());
			else
				LogError("[DeltaForce] FBoolProperty base could not be verified, keeping 0x%X", DefaultBool);
		}

		/* Sizes of property types that the generator needs */
		PropertySizes::Init();
	}

	/* ------------------------------------------------------------------ */
	/* FText layout without calling ProcessEvent                          */
	/* ------------------------------------------------------------------ */

	void InitTextOffsetsFromLiveTexts()
	{
		/* UE4.26: FText { TSharedRef<ITextData> TextData (0x10); uint32 Flags; } -> TextData object @0x0, TTextData::History::SourceString @0x28 */
		Off::InSDK::Text::TextSize = 0x18;
		Off::InSDK::Text::TextDatOffset = 0x0;
		Off::InSDK::Text::InTextDataStringOffset = 0x28;

		std::map<int32, int32> Votes;
		int32 NumVotes = 0;
		bool bFoundTextSize = false;

		const int32 NumObjects = ObjectArray::Num();

		for (int32 i = 0; i < NumObjects && NumVotes < 64; i++)
		{
			UEObject Obj = ObjectArray::GetByIndex(i);

			if (!Obj || !Obj.HasAnyFlags(EObjectFlags::ClassDefaultObject))
				continue;

			int32 Depth = 0;
			for (UEStruct Struct = Obj.GetClass(); Struct && Depth < 8; Struct = Struct.GetSuper(), Depth++)
			{
				for (UEProperty Property : Struct.GetProperties())
				{
					if (!Property.IsA(EClassCastFlags::TextProperty) || Property.GetArrayDim() != 1)
						continue;

					if (!bFoundTextSize)
					{
						Off::InSDK::Text::TextSize = Property.GetSize();
						bFoundTextSize = true;
					}

					const uintptr_t Text = reinterpret_cast<uintptr_t>(Obj.GetAddress()) + Property.GetOffset();
					const uintptr_t TextData = SafeRead<uintptr_t>(Text + Off::InSDK::Text::TextDatOffset);

					if (!IsPlausiblePointer(TextData))
						continue;

					for (int32 Offset = 0x8; Offset <= 0x50; Offset += 0x8)
					{
						struct { uintptr_t Data; int32 Num; int32 Max; } String{};

						if (!ReadMemory(TextData + Offset, &String, sizeof(String)) || !IsPlausiblePointer(String.Data) || String.Num < 2 || String.Num > 256 || String.Max < String.Num)
							continue;

						std::vector<uint16> Chars(String.Num);
						if (!ReadMemory(String.Data, Chars.data(), Chars.size() * sizeof(uint16)) || Chars.back() != 0)
							continue;

						if (std::all_of(Chars.begin(), Chars.end() - 1, [](uint16 C) { return C >= 0x20; }))
						{
							Votes[Offset]++;
							NumVotes++;
						}
					}
				}
			}
		}

		auto Best = std::max_element(Votes.begin(), Votes.end(), [](const auto& A, const auto& B) { return A.second < B.second; });

		if (Best != Votes.end() && Best->second >= 3)
			Off::InSDK::Text::InTextDataStringOffset = Best->first;
		else
			LogError("[DeltaForce] FText string offset not verified from live texts, using 0x%X", Off::InSDK::Text::InTextDataStringOffset);

		LogInfo("[DeltaForce] FText: Size 0x%X, TextData 0x%X, String 0x%X", Off::InSDK::Text::TextSize, Off::InSDK::Text::TextDatOffset, Off::InSDK::Text::InTextDataStringOffset);
	}

	FChunkedFixedUObjectArrayLayout MakeObjectArrayLayout(const FProfile& P, uintptr_t GObjects)
	{
		const int32 Array = static_cast<int32>(P.ObjectArray);

		FChunkedFixedUObjectArrayLayout Layout;
		Layout.ObjectsOffset = Array + static_cast<int32>(P.ObjectsChunks);
		Layout.NumElementsOffset = Array + static_cast<int32>(P.ObjectsCount);
		Layout.NumChunksOffset = Array + static_cast<int32>(P.ObjectsNumChunks);

		/* Not needed for dumping, only named in the SDK. Use the remaining int32 slots of TUObjectArray when they look like maxima. */
		const int32 NumElements = SafeRead<int32>(GObjects + Layout.NumElementsOffset);
		const int32 NumChunks = SafeRead<int32>(GObjects + Layout.NumChunksOffset);

		std::vector<int32> FreeSlots;
		for (int32 Offset = Array; Offset < Layout.ObjectsOffset; Offset += 0x4)
		{
			if (Offset != Layout.NumElementsOffset && Offset != Layout.NumChunksOffset)
				FreeSlots.push_back(Offset);
		}

		Layout.MaxElementsOffset = Layout.NumElementsOffset;
		Layout.MaxChunksOffset = Layout.NumChunksOffset;

		for (const int32 Offset : FreeSlots)
		{
			const int32 Value = SafeRead<int32>(GObjects + Offset);

			if (Layout.MaxElementsOffset == Layout.NumElementsOffset && Value >= NumElements && Value > 0x10000)
				Layout.MaxElementsOffset = Offset;
			else if (Layout.MaxChunksOffset == Layout.NumChunksOffset && Value >= NumChunks && Value <= 0x400)
				Layout.MaxChunksOffset = Offset;
		}

		return Layout;
	}
}

	FProfile SeedProfile()
	{
		return FProfile{};
	}

	const FProfile& GetProfile()
	{
		return GProfile;
	}

	bool IsDeltaForceProcess()
	{
		return FindExactImageIndex(Settings::DeltaForce::ModuleName) >= 0;
	}

	bool InitEngineCore()
	{
		const int32 ImageIndex = FindExactImageIndex(Settings::DeltaForce::ModuleName);

		if (ImageIndex < 0)
		{
			LogError("[DeltaForce] '%s' is not loaded", Settings::DeltaForce::ModuleName);
			return false;
		}

		const auto* Header = reinterpret_cast<const struct mach_header_64*>(_dyld_get_image_header(ImageIndex));
		const uintptr_t ImageBase = reinterpret_cast<uintptr_t>(Header);
		const std::vector<MachSegment> Segments = GetImageSegments(Header, _dyld_get_image_vmaddr_slide(ImageIndex));

		if (!Header || Header->magic != MH_MAGIC_64 || Header->cputype != CPU_TYPE_ARM64 || Segments.empty())
		{
			LogError("[DeltaForce] invalid ARM64 Mach-O image");
			return false;
		}

		if (ImageBase != GetModuleBase())
			LogError("[DeltaForce] '%s' is not the main executable, SDK offsets are relative to 0x%llX", Settings::DeltaForce::ModuleName, static_cast<unsigned long long>(ImageBase));

		LogInfo("[DeltaForce] %s loaded at 0x%llX", Settings::DeltaForce::ModuleName, static_cast<unsigned long long>(ImageBase));

		/* 1. Globals + name codec + UObject header, all semantically validated before first use */
		FProfile Profile = SeedProfile();
		std::string Error;

		const bool bHasManualOverride = Settings::DeltaForce::GNamesRVA != 0x0 && Settings::DeltaForce::GObjectsRVA != 0x0;
		bool bResolved = false;

		if (bHasManualOverride)
		{
			Profile.NamesRVA = Settings::DeltaForce::GNamesRVA;
			Profile.ObjectsRVA = Settings::DeltaForce::GObjectsRVA;

			bResolved = Discovery::CompleteProfile(ImageBase, Profile, Error);

			if (!bResolved)
				LogError("[DeltaForce] Manual GNames 0x%llX / GObjects 0x%llX rejected: %s. Falling back to scanning.", static_cast<unsigned long long>(Profile.NamesRVA), static_cast<unsigned long long>(Profile.ObjectsRVA), Error.c_str());
		}

		if (!bResolved)
		{
			Discovery::FMemoryRanges DataRanges;

			for (const MachSegment& Segment : Segments)
			{
				if ((Segment.InitProt & VM_PROT_READ) && !(Segment.InitProt & VM_PROT_EXECUTE) && strncmp(Segment.Name, "__LINKEDIT", 16) != 0 && Segment.Begin > ImageBase)
					DataRanges.emplace_back(Segment.Begin, Segment.End);
			}

			std::sort(DataRanges.begin(), DataRanges.end());

			bResolved = Discovery::DiscoverProfile(ImageBase, DataRanges, Profile, Error);
		}

		if (!bResolved)
		{
			LogError("[DeltaForce] %s", Error.c_str());
			LogError("[DeltaForce] Wait until the lobby is fully loaded and press 'Start Dump' again.");
			return false;
		}

		GProfile = Profile;

		LogSuccess("[DeltaForce] GNames RVA 0x%llX, GObjects RVA 0x%llX", static_cast<unsigned long long>(Profile.NamesRVA), static_cast<unsigned long long>(Profile.ObjectsRVA));
		LogInfo("[DeltaForce] UObject: Class 0x%X, Outer 0x%X, Name 0x%X, Index 0x%X | FUObjectItem: Size 0x%X, Object 0x%X | Names: %s, LenShift %u",
			Profile.ObjectClass, Profile.ObjectOuter, Profile.ObjectName, Profile.ObjectIndex, Profile.ItemSize, Profile.ItemObject,
			Profile.NameCodec == ENameCodec::DfV1 ? "DF-v1 (encrypted)" : "plain", Profile.NameLengthShift);

		/* 2. Hook the profile into the generator's engine core */
		ApplyProfile(Profile);

		const uintptr_t GObjects = ImageBase + Profile.ObjectsRVA;
		const uintptr_t GNames = ImageBase + Profile.NamesRVA;

		ObjectArray::InitWithKnownLayout(reinterpret_cast<uint8*>(GObjects), static_cast<int32>(Profile.ObjectsRVA), static_cast<int32>(Profile.ChunkSize),
			MakeObjectArrayLayout(Profile, GObjects), Profile.ItemSize, Profile.ItemObject);

		if (!NameArray::InitWithKnownNamePoolLayout(reinterpret_cast<uint8*>(GNames), static_cast<int32>(Profile.NamesRVA), static_cast<int32>(Profile.PoolBlocks),
			static_cast<int32>(Profile.PoolCursor), static_cast<int32>(Profile.PoolCurrentBlock), Profile.PoolBlockBits, Profile.NameStride, Profile.NameLengthShift,
			Profile.NameCodec == ENameCodec::DfV1))
		{
			return false;
		}

		FName::InitWithNameArray();

		/* 3. Verify the deeper reflection layout with kernel reads before the generator dereferences anything */
		FCoreObjects Core;
		if (!FindCoreObjects(Core, Error) || !VerifyReflectionLayout(Core, Error))
		{
			LogError("[DeltaForce] Reflection layout check failed: %s", Error.c_str());
			LogError("[DeltaForce] This game build changed the UStruct/FField layout. Aborting instead of crashing.");
			return false;
		}

		LogSuccess("[DeltaForce] UStruct/UFunction/UEnum/FField/FProperty layout verified");

		Off::UObject::Flags = FindObjectFlagsOffset();
		if (Off::UObject::Flags == OffsetFinder::OffsetNotFound)
			LogError("[DeltaForce] UObject::Flags not found, CDOs are detected by name");
		else
			LogInfo("[DeltaForce] UObject::Flags: 0x%X", Off::UObject::Flags);

		ProbePropertyOffsets();

		/* 4. Offsets that are only written to the SDK. All finders below only dereference verified pointers or check with IsBadReadPtr. */
		Off::UClass::ClassDefaultObject = OffsetFinder::FindDefaultObjectOffset();
		LogInfo("[DeltaForce] UClass::ClassDefaultObject: 0x%X", Off::UClass::ClassDefaultObject);

		Off::UClass::CastFlags = OffsetFinder::FindCastFlagsOffset();
		LogInfo("[DeltaForce] UClass::CastFlags: 0x%X (dumper uses name based cast flags)", Off::UClass::CastFlags);

		Off::UClass::ImplementedInterfaces = OffsetFinder::FindImplementedInterfacesOffset();
		LogInfo("[DeltaForce] UClass::ImplementedInterfaces: 0x%X", Off::UClass::ImplementedInterfaces);

		Off::InSDK::ULevel::Actors = OffsetFinder::FindLevelActorsOffset();
		LogInfo("[DeltaForce] ULevel::Actors: 0x%X", Off::InSDK::ULevel::Actors);

		Off::InSDK::UDataTable::RowMap = OffsetFinder::FindDatatableRowMapOffset();
		LogInfo("[DeltaForce] UDataTable::RowMap: 0x%X", Off::InSDK::UDataTable::RowMap);

		Off::InSDK::ProcessEvent::InitPE();
		if (Off::InSDK::ProcessEvent::PEIndex <= 0)
		{
			LogError("[DeltaForce] ProcessEvent not found by the ARM64 heuristic, writing fallback index 0x%X to the SDK (unverified)", Settings::DeltaForce::FallbackProcessEventIndex);
			Off::InSDK::ProcessEvent::InitPE(Settings::DeltaForce::FallbackProcessEventIndex, Settings::DeltaForce::ModuleName);
		}

		Off::InSDK::World::InitGWorld();

		InitTextOffsetsFromLiveTexts();

		LogSuccess("[DeltaForce] Engine core ready: %d objects", ObjectArray::Num());
		return true;
	}
}
