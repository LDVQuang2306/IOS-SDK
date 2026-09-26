#pragma once

#include <climits>
#include <unordered_map>
#include <unordered_set>

#include "../../../Engine/Public/Unreal/UnrealObjects.h"
#include "../../../Engine/Public/Unreal/ObjectArray.h"
#include "../HashStringTable.h"


/*
struct alignas(0x08) Parent
{
	uint8 DeltaFlags;                              // Offset: 0x0, Size: 0x1
	//uint8 CompilerGeneratedTrailingPadding[0x7]; // 0x7 byte padding to achieve 0x8 alignment (size is always a multiple of alignment)
};

struct Child : Parent
{
	// This EXPLICITE padding replaces the implicite padding of the Parent class. However, this isn't done concistently by the compiler.
	uint8 Pad_idk[0x7];          // Offset: 0x1, Size: 0x7
};
// static_assert(sizeof(Parent) == 0x8); // true
// static_assert(sizeof(Child) == 0x8);  // true
*/

struct StructInfo
{
	HashStringTableIndex Name;

	/* End of the last member-variable of this struct, used to calculate implicit trailing padding */
	int32 LastMemberEnd = 0x0;

	/* Unaligned size of this struct */
	int32 Size = INT_MAX;

	/* Alignment of this struct for alignas(Alignment), might be implicit */
	int32 Alignment = 0x1;


	/* Whether alignment should be explicitly specified with 'alignas(Alignment)' */
	bool bUseExplicitAlignment;

	/* Whether this struct has child-structs for which the compiler places members in this structs' trailing padding, see example above (line 9) */
	bool bHasReusedTrailingPadding = false;

	/* Wheter this class is ever inherited from. Set to false when this struct is found to be another structs super */
	bool bIsFinal = true;

	/* Whether this struct is in a package that has cyclic dependencies. Actual index of cyclic package is stored in StructManager::CyclicStructsAndPackages */
	bool bIsPartOfCyclicPackage;
};

class StructManager;

class StructInfoHandle
{
private:
	const StructInfo* Info;

public:
	StructInfoHandle() = default;
	StructInfoHandle(const StructInfo& InInfo);

public:
	int32 GetLastMemberEnd() const;
	int32 GetSize() const;
	int32 GetUnalignedSize() const;
	int32 GetAlignment() const;
	bool ShouldUseExplicitAlignment() const;
	const StringEntry& GetName() const;
	bool IsFinal() const;
	bool HasReusedTrailingPadding() const;

	bool IsPartOfCyclicPackage() const;
};

class StructManager
{
private:
	friend StructInfoHandle;
	friend class StructManagerTest;

public:
	using OverrideMapType = std::unordered_map<int32 /*StructIdx*/, StructInfo>;
	using CycleInfoListType = std::unordered_map<int32 /*StructIdx*/, std::unordered_set<int32 /* Packages cyclic with this structs' package */>>;

private:
	/* NameTable containing names of all structs/classes as well as information on name-collisions */
	static inline HashStringTable UniqueNameTable;

	/* Map containing infos on all structs/classes. Implemented due to bugs/inconsistencies in Unreal's reflection system */
	static inline OverrideMapType StructInfoOverrides;

	/* Map containing infos on all structs/classes that are within a packages that has cyclic dependencies */
	static inline CycleInfoListType CyclicStructsAndPackages;

	static inline bool bIsInitialized = false;

	/* Number of structs that had to be added after Init(), only the first ones are logged */
	static inline int32 NumMissingStructs = 0;

private:
	static void InitAlignmentsAndNames();
	static void InitSizesAndIsFinal();

	/* Info for a struct that Init() didn't see (it wasn't in the object list), computed like Init() does for a single struct */
	static StructInfo& AddMissingStruct(const UEStruct Struct);

public:
	static void Init();

private:
	static inline const StringEntry& GetName(const StructInfo& Info)
	{
		return UniqueNameTable[Info.Name];
	}

public:
	static inline const OverrideMapType& GetStructInfos()
	{
		return StructInfoOverrides;
	}

	static inline bool IsStructNameUnique(HashStringTableIndex NameIndex)
	{
		return UniqueNameTable[NameIndex].IsUnique();
	}

	// debug function
	static inline std::string GetName(HashStringTableIndex NameIndex)
	{
		return UniqueNameTable[NameIndex].GetName();
	}

	static inline StructInfoHandle GetInfo(const UEStruct Struct)
	{
		if (!Struct)
			return {};

		/* Lookups used to be StructInfoOverrides.at(), a struct Init() didn't see aborted the whole dump ("unordered_map::at: key not found") */
		auto It = StructInfoOverrides.find(Struct.GetIndex());
		if (It == StructInfoOverrides.end()) [[unlikely]]
			return AddMissingStruct(Struct);

		return It->second;
	}

	static inline bool IsStructCyclicWithPackage(int32 StructIndex, int32 PackageIndex)
	{
		auto It = CyclicStructsAndPackages.find(StructIndex);
		if (It != CyclicStructsAndPackages.end())
			return It->second.find(PackageIndex) != It->second.end();

		return false;
	}

	/* 
	* Utility function for PackageManager::PostInit to handle the initialization of our list of cyclic structs and their respective packages
	* 
	* Marks StructInfo as 'bIsPartOfCyclicPackage = true' and adds struct to 'CyclicStructsAndPackages'
	*/
	static inline void PackageManagerSetCycleForStruct(int32 StructIndex, int32 PackageIndex)
	{
		auto It = StructInfoOverrides.find(StructIndex);
		StructInfo& Info = It != StructInfoOverrides.end() ? It->second : AddMissingStruct(ObjectArray::GetByIndex<UEStruct>(StructIndex));

		Info.bIsPartOfCyclicPackage = true;


		CyclicStructsAndPackages[StructIndex].insert(PackageIndex);
	}
};

