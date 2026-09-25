#pragma once

#include "UnrealTypes.h"

class FNameEntry
{
private:
	friend class NameArray;

private:
	static constexpr int32 NameWideMask = 0x1;

private:
	static inline int32 FNameEntryLengthShiftCount = 0x0;

	static inline UnrealString(*GetStr)(uint8* NameEntry) = nullptr;

private:
	uint8* Address = nullptr;

public:
	FNameEntry() = default;

	FNameEntry(void* Ptr);

public:
	UnrealString GetWString();
	std::string GetString();
	void* GetAddress();

private:
	//Optional to avoid code duplication for FNamePool
	static void Init(const uint8_t* FirstChunkPtr = nullptr, int64 NameEntryStringOffset = 0x0);
};

class NameArray
{
private:
	static inline uint32 FNameBlockOffsetBits = 0x10;
private:
	static uint8* GNames;

	static inline int64 NameEntryStride = 0x0;

	/* Delta Force obfuscates the characters of every FNameEntry */
	static inline bool bIsNameEncrypted = false;

	static inline void* (*ByIndex)(void* NamesArray, int32 ComparisonIndex, int32 NamePoolBlockOffsetBits) = nullptr;

private:
	static bool InitializeNameArray(uint8_t* NameArray);
	static bool InitializeNamePool(uint8_t* NamePool);

public:
	/* Should be changed later and combined */
	static bool TryFindNameArray();
	static bool TryFindNamePool();

	static bool TryInit(bool bIsTestOnly = false);
	static bool TryInit(int32 OffsetOverride, bool bIsNamePool, const char* const ModuleName = nullptr);

	/* Initializes the GNames offset, but doesn't call NameArray::InitializeNameArray() or NameArray::InitializedNamePool() */
	static bool SetGNamesWithoutCommiting();

	/*
	* FNamePool with a known (already validated) layout, e.g. Delta Force: Blocks[] @0xC8, CurrentByteCursor @0x100C8, CurrentBlock @0x100CC.
	* Every read of the pool goes through the kernel, invalid indices return an empty name instead of crashing.
	*/
	static bool InitWithKnownNamePoolLayout(uint8* NamePool, int32 GNamesOffset, int32 BlocksOffset, int32 CursorOffset, int32 CurrentBlockOffset,
		uint32 BlockOffsetBits, uint32 EntryStride, uint32 LengthShift, bool bEncryptedNames);

	/* Thread-safe, cached name of a ComparisonIndex (names in the pool are immutable once allocated). */
	static UnrealString GetCachedName(int32 ComparisonIndex);

	static void PostInit();
	
public:
	static int32 GetNumChunks();

	static int32 GetNumElements();
	static int32 GetByteCursor();

	static inline bool IsNameEncrypted() { return bIsNameEncrypted; }

	static FNameEntry GetNameEntry(const void* Name);
	static FNameEntry GetNameEntry(int32 Idx);
};
