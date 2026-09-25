#pragma once

#include "Unreal/UnrealTypes.h"

class FNameEntry
{
private:
	friend class NameArray;

private:
	static constexpr int32 NameWideMask = 0x1;

private:
	/* Right-shift that yields FNameEntryHeader::Len (6 for regular builds, 1 for case-preserving names). Detected, not assumed. */
	static inline int32 FNameEntryLengthShiftCount = 0x6;

	/* sizeof(WIDECHAR) inside the game. Can differ from the dumper's TCHAR, wide names are converted. */
	static inline int32 GameWideCharSize = 0x2;

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
	/* Installs the entry reader for the current mode (FNamePool / TNameEntryArray). Returns false if the entry layout couldn't be determined. */
	static bool Init(const uint8_t* FirstChunkPtr = nullptr, int64 NameEntryStringOffset = 0x0);
};

class NameArray
{
private:
	static inline uint32 FNameBlockOffsetBits = 0x10;
private:
	static uint8* GNames;

	static inline int64 NameEntryStride = 0x0;

	static inline void* (*ByIndex)(void* NamesArray, int32 ComparisonIndex, int32 NamePoolBlockOffsetBits) = nullptr;

	/* Whether every Blocks[0 ... FNameMaxBlocks - 1] slot is readable, so name lookups don't need a syscall per access. */
	static inline bool bAllBlockSlotsReadable = false;

	/* Number of FNamePool blocks verified so far. Blocks are allocated in order, higher indices are verified on first use. */
	static inline int32 NumKnownBlocks = 0;

private:
	static bool InitializeNameArray(uint8_t* NameArray);
	static bool InitializeNamePool(uint8_t* NamePool);

	/* Commits an FNamePool whose Blocks[] array starts at BlocksSlot. Offsets of the other members are derived from the data. */
	static bool InitializeNamePoolFromBlocks(uint8_t* BlocksSlot, uintptr_t SearchLowerBound, uintptr_t ImageBase, uint8_t* ForcedPoolBase = nullptr);

public:
	/* (1) Per-FNameEntry content decryption (default: identity).
	 *
	 * Called on every raw FNameEntry* immediately before the header / chars
	 * are read. Use it for games that XOR or otherwise mangle entry bytes.
	 * Example: DeltaForce — per-Len char XOR inside ToString.
	 *
	 * The function may either:
	 *   - return its input unchanged (no decryption)
	 *   - return a pointer to a transformed copy in a scratch buffer
	 *   - mutate the entry in place and return the same pointer
	 *
	 * Captured-source-string analogue to ObjectArray::DecryptionLambdaStr so
	 * the SDK emitter can inline the same logic into generated code. */
	static inline std::string NameEntryDecryptionLambdaStr;

	static inline uint8_t* (*DecryptNameEntry)(uint8_t* RawEntryPtr) = [](uint8_t* Ptr) -> uint8_t* { return Ptr; };

	static void InitEntryDecryption(uint8_t* (*DecryptionFunction)(uint8_t* RawEntryPtr), const char* DecryptionLambdaAsStr);

	/* (2) Output std::string decryption (default: identity).
	 *
	 * Runs at the tail of FNameEntry::GetString — after the entry's header
	 * has been parsed and the chars copied into a std::string. The hook
	 * receives that string by value and returns the decrypted form.
	 *
	 * Use this when the header is plaintext but the chars are XOR'd, and
	 * you'd rather not write byte-level entry math. Matches Mj0x's
	 * IGameProfile::GetNameEntryString override style.
	 *
	 * Mutually compatible with DecryptNameEntry — if both are installed,
	 * DecryptNameEntry runs first (on raw bytes), then DecryptNameString
	 * runs on the resulting std::string. */
	static inline std::string NameStringDecryptionLambdaStr;

	static inline std::string (*DecryptNameString)(std::string Decoded) = [](std::string s) -> std::string { return s; };

	static void InitStringDecryption(std::string (*DecryptionFunction)(std::string Decoded), const char* DecryptionLambdaAsStr);

	/* (3) TNameEntryArray *pointer* decryption (default: identity).
	 *
	 * Applies to UE <= 4.22 games (`bIsNamePool == false`). Called once during
	 * TryInit with the *raw* `ImageBase + GNamesOffset` address. The hook must
	 * return the address where the live `TNameEntryArray*` is stored
	 * (i.e. `TNameEntryArray**`). The runtime then dereferences once to get
	 * the live array — matches Mj0x's IGameProfile::GetNamesPtr semantics.
	 *
	 * Default behavior: identity. For standard games the offset already points
	 * to a global `TNameEntryArray*` variable, so the raw address IS the `**`.
	 *
	 * Override for games whose raw address needs work to reach the `**`, e.g.:
	 *   - PUBG (UE 4.17): the raw address is the start of an encrypted struct
	 *     (int32 header + chain pointer); walk the chain N hops to reach the
	 *     `TNameEntryArray**`. Don't do the final deref — the runtime does it. */
	static inline std::string NameArrayDecryptionLambdaStr;

	static inline uintptr_t (*DecryptNameArray)(uintptr_t RawGNamesAddr) = [](uintptr_t P) -> uintptr_t { return P; };

	static void InitArrayDecryption(uintptr_t (*DecryptionFunction)(uintptr_t RawGNamesAddr), const char* DecryptionLambdaAsStr);

	/* (4) FNamePool *pointer* decryption (default: identity).
	 *
	 * Applies to UE 4.23+ games (`bIsNamePool == true`). Called once during
	 * TryInit, right after the GNames address is resolved but before
	 * InitializeNamePool runs. Use it for games whose discovered address
	 * isn't the real FNamePool base, e.g.:
	 *   - Valorant: 8-byte fragments at +128..+156 of the encrypted pool
	 *     reassemble into a pointer that, dereferenced, yields the real pool. */
	static inline std::string NamePoolDecryptionLambdaStr;

	static inline uintptr_t (*DecryptNamePool)(uintptr_t EncryptedNamePoolData) = [](uintptr_t P) -> uintptr_t { return P; };

	static void InitPoolDecryption(uintptr_t (*DecryptionFunction)(uintptr_t EncryptedNamePoolData), const char* DecryptionLambdaAsStr);

public:
	/* Whether the name-string decryption was installed by the user (then it's never replaced by auto-detection). */
	static inline bool bHasUserStringDecryption = false;

	/* Delta Force: FNameEntry characters are XOR'd with a key derived from the name length (header stays plaintext). */
	static std::string DecryptDeltaForceNameString(std::string Decoded);
	static const char* const DeltaForceNameStringDecryptionSrc;

public:
	/* Should be changed later and combined */
	static bool TryFindNameArray();
	static bool TryFindNamePool();

	/*
	* Data-anchored searches: walk the writable segments of the image for the name table itself. FNamePool::Blocks[0] (and
	* TNameEntryArray chunk 0) always start with the "None" and "ByteProperty" entries. Delta Force's encrypted name strings
	* are recognized automatically. These don't depend on code patterns, so they survive game updates.
	*/
	static bool TryFindNamePoolByData(const char* const ModuleName = nullptr);
	static bool TryFindNameArrayByData(const char* const ModuleName = nullptr);

	static inline bool IsInitialized() { return GNames != nullptr && ByIndex != nullptr && FNameEntry::GetStr != nullptr; }

	static bool TryInit(bool bIsTestOnly = false);
	static bool TryInit(int32 OffsetOverride, bool bIsNamePool, const char* const ModuleName = nullptr);

	/* Initializes the GNames offset, but doesn't call NameArray::InitializeNameArray() or NameArray::InitializedNamePool() */
	static bool SetGNamesWithoutCommiting();

	static void PostInit();

public:
	static int32 GetNumChunks();

	static int32 GetNumElements();
	static int32 GetByteCursor();

	static FNameEntry GetNameEntry(const void* Name);
	static FNameEntry GetNameEntry(int32 Idx);
};

/* Convenience: pass a lambda and have its source captured automatically for SDK emission. */
#define InitNameEntryDecryption(DecryptionLambda)  NameArray::InitEntryDecryption(DecryptionLambda, #DecryptionLambda)
#define InitNameStringDecryption(DecryptionLambda) NameArray::InitStringDecryption(DecryptionLambda, #DecryptionLambda)
#define InitNameArrayDecryption(DecryptionLambda)  NameArray::InitArrayDecryption(DecryptionLambda, #DecryptionLambda)
#define InitNamePoolDecryption(DecryptionLambda)   NameArray::InitPoolDecryption(DecryptionLambda, #DecryptionLambda)
