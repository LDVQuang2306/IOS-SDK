#pragma once

#include "CollisionManager.h"


class EnumInfoHandle;

class EnumManager;

struct EnumCollisionInfo
{
private:
	friend class EnumManager;

private:
	HashStringTableIndex MemberName;
	uint64 MemberValue;

	uint8 CollisionCount = 0;

public:
	std::string GetUniqueName() const;
	std::string GetRawName() const;
	uint64 GetValue() const;

	uint8 GetCollisionCount() const;
};

struct EnumInfo
{
private:
	friend class EnumInfoHandle;
	friend class EnumManager;

private:
	/* Name of this Enum*/
	HashStringTableIndex Name;

	/* sizeof(UnderlayingType) */
	uint8 UnderlyingTypeSize = 0x1;

	/* Wether an occurence of this enum was found, if not guess the type by the enums' max value */
	bool bWasInstanceFound = false;

	/* Whether this enums' size was initialized before */
	bool bWasEnumSizeInitialized = false;

	/* Whether the enum itself was seen (Name and MemberInfos are set), not just a property that uses it */
	bool bWasNameInitialized = false;

	/* Infos on all members and if there are any collisions between member-names */
	std::vector<EnumCollisionInfo> MemberInfos;
};

struct CollisionInfoIterator
{
private:
	const std::vector<EnumCollisionInfo>& CollisionInfos;

public:
	CollisionInfoIterator(const std::vector<EnumCollisionInfo>& Infos)
		: CollisionInfos(Infos)
	{
	}

public:
	auto begin() const { return CollisionInfos.cbegin(); }
	auto end() const { return CollisionInfos.end(); }
};

class EnumInfoHandle
{
private:
	const EnumInfo* Info;

public:
	EnumInfoHandle() = default;
	EnumInfoHandle(const EnumInfo& InInfo);

public:
	uint8 GetUnderlyingTypeSize() const;
	const StringEntry& GetName() const;

	int32 GetNumMembers() const;

	CollisionInfoIterator GetMemberCollisionInfoIterator() const;
};


class EnumManager
{
private:
	friend class EnumCollisionInfo;
	friend class EnumInfoHandle;
	friend class EnumManagerTest;

public:
	using OverrideMaptType = std::unordered_map<int32 /* EnumIndex */, EnumInfo>;
	using IllegalNameContaierType = std::vector<HashStringTableIndex>;

private:
	/* NameTable containing names of all enums as well as information on name-collisions */
	static inline HashStringTable UniqueEnumNameTable;

	/* Map containing infos on all enums. Implemented due to information missing in the Unreal's reflection system (EnumSize). */
	static inline OverrideMaptType EnumInfoOverrides;

	/* NameTable containing names of all enum-values as well as information on name-collisions */
	static inline HashStringTable UniqueEnumValueNames;

	/* List containing names-indices which contain illegal enum names such as 'PF_MAX' */
	static inline IllegalNameContaierType IllegalNames;

	static inline bool bIsInitialized = false;

	/* Number of enums that had to be added after Init(), only the first ones are logged */
	static inline int32 NumMissingEnums = 0;

private:
	static void InitInternal();
	static void InitIllegalNames();

	/* Name, values and (if no property uses the enum) size of an enum */
	static EnumInfo& InitEnum(const UEEnum Enum);

	/* Info for an enum that Init() didn't see (it wasn't in the object list) */
	static EnumInfo& AddMissingEnum(const UEEnum Enum);

public:
	static void Init();

private:
	static inline const StringEntry& GetEnumName(const EnumInfo& Info)
	{
		return UniqueEnumNameTable[Info.Name];
	}

	static inline const StringEntry& GetValueName(const EnumCollisionInfo& Info)
	{
		return UniqueEnumValueNames[Info.MemberName];
	}

public:
	static inline const OverrideMaptType& GetEnumInfos()
	{
		return EnumInfoOverrides;
	}

	static inline bool IsEnumNameUnique(const EnumInfo& Info)
	{
		return UniqueEnumNameTable[Info.Name].IsUnique();
	}

	static inline EnumInfoHandle GetInfo(const UEEnum Enum)
	{
		if (!Enum)
			return {};

		/* Used to be EnumInfoOverrides.at(), an enum Init() didn't see aborted the whole dump ("unordered_map::at: key not found") */
		auto It = EnumInfoOverrides.find(Enum.GetIndex());
		if (It == EnumInfoOverrides.end() || !It->second.bWasNameInitialized) [[unlikely]]
			return AddMissingEnum(Enum);

		return It->second;
	}
};

