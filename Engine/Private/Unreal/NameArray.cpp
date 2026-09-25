#include <iostream>
#include <string>
#include <vector>
#include <cstdio> 

#include "Unreal/ObjectArray.h"
#include "Unreal/NameArray.h"
#include "Utils/Utils.h"
#include "Utils/Encoding/UtfN.hpp"
#include "Menu/Logger.h"

uint8* NameArray::GNames = nullptr;

void NameArray::InitEntryDecryption(uint8_t* (*DecryptionFunction)(uint8_t* RawEntryPtr), const char* DecryptionLambdaAsStr)
{
    LogInfo("Initializing FNameEntry decryption: %s", DecryptionLambdaAsStr);
    DecryptNameEntry = DecryptionFunction;
    NameEntryDecryptionLambdaStr = DecryptionLambdaAsStr;
    LogSuccess("FNameEntry decryption initialized");
}

void NameArray::InitStringDecryption(std::string (*DecryptionFunction)(std::string Decoded), const char* DecryptionLambdaAsStr)
{
    LogInfo("Initializing FName output-string decryption: %s", DecryptionLambdaAsStr);
    DecryptNameString = DecryptionFunction;
    NameStringDecryptionLambdaStr = DecryptionLambdaAsStr;
    LogSuccess("FName output-string decryption initialized");
}

void NameArray::InitArrayDecryption(uintptr_t (*DecryptionFunction)(uintptr_t EncryptedNameArrayData), const char* DecryptionLambdaAsStr)
{
    LogInfo("Initializing TNameEntryArray pointer decryption: %s", DecryptionLambdaAsStr);
    DecryptNameArray = DecryptionFunction;
    NameArrayDecryptionLambdaStr = DecryptionLambdaAsStr;
    LogSuccess("TNameEntryArray pointer decryption initialized");
}

void NameArray::InitPoolDecryption(uintptr_t (*DecryptionFunction)(uintptr_t EncryptedNamePoolData), const char* DecryptionLambdaAsStr)
{
    LogInfo("Initializing FNamePool pointer decryption: %s", DecryptionLambdaAsStr);
    DecryptNamePool = DecryptionFunction;
    NamePoolDecryptionLambdaStr = DecryptionLambdaAsStr;
    LogSuccess("FNamePool pointer decryption initialized");
}

FNameEntry::FNameEntry(void* Ptr)
    : Address((uint8*)Ptr)
{
}

UnrealString FNameEntry::GetWString()
{
    if (!Address)
        return TEXT("");

    return GetStr(Address);
}

std::string FNameEntry::GetString()
{
    if (!Address)
        return "";

    // DecryptNameString is applied inside NameArray::GetStr on the raw narrow
    // bytes, so GetWString() already returns decrypted wide chars here.
    auto Wide = GetWString();
    return std::string(Wide.begin(), Wide.end());
}

void* FNameEntry::GetAddress()
{
    return Address;
}

void FNameEntry::Init(const uint8* FirstChunkPtr, int64 NameEntryStringOffset)
{
    LogInfo("Dumper-7: [FNameEntry] Initializing...");
    if (Settings::Internal::bUseNamePool)
    {

        Off::FNameEntry::NamePool::StringOffset = (int32)NameEntryStringOffset;
        Off::FNameEntry::NamePool::HeaderOffset = (int32)NameEntryStringOffset == 6 ? 4 : 0;

        LogSuccess("Dumper-7: [FNameEntry] NamePool initialized (Stride: %d)", Off::InSDK::NameArray::FNameEntryStride);

        /* Standard UE 4.23+ FNamePool entry reader (upstream Dumper-7 layout).
         * Header is 2 bytes at HeaderOffset; top 10 bits = name length, bit 0 = wide flag.
         * NameLen == 0 indicates a numbered entry referencing a base by index. */
        GetStr = [](uint8* NameEntry) -> UnrealString
        {
            // Per-game FNameEntry content decryption (no-op by default).
            NameEntry = NameArray::DecryptNameEntry(NameEntry);

            const uint16 HeaderWithoutNumber = *reinterpret_cast<uint16*>(NameEntry + Off::FNameEntry::NamePool::HeaderOffset);
            const int32 NameLen = HeaderWithoutNumber >> 6;

            if (NameLen == 0)
            {
                const int32 EntryIdOffset = Off::FNameEntry::NamePool::StringOffset + ((Off::FNameEntry::NamePool::StringOffset == 6) * 2);

                const int32 NextEntryIndex = *reinterpret_cast<int32*>(NameEntry + EntryIdOffset);
                const int32 Number = *reinterpret_cast<int32*>(NameEntry + EntryIdOffset + sizeof(int32));

                if (Number > 0)
                    return NameArray::GetNameEntry(NextEntryIndex).GetWString() + TEXT('_') + ToUEString(Number - 1);

                return NameArray::GetNameEntry(NextEntryIndex).GetWString();
            }

            // Wide-char path: no DecryptNameString applied. The hook signature is for
            // narrow byte XOR; wide encryption (no known shipping game does this) would
            // need UTF-16-aware math, not a byte-level hook.
            if (HeaderWithoutNumber & NameWideMask)
                return UnrealString(reinterpret_cast<const TCHAR*>(NameEntry + Off::FNameEntry::NamePool::StringOffset), NameLen);

            /* Decrypt at the raw-bytes level BEFORE wide conversion. UTF-8 inflation
             * of XOR-encrypted bytes (e.g. DeltaForce: 0xB1 -> 2-byte UTF-8) would
             * change the string Len and break per-Len key derivation in the hook. */
            std::string RawAnsi(reinterpret_cast<const char*>(NameEntry + Off::FNameEntry::NamePool::StringOffset), NameLen);
            RawAnsi = NameArray::DecryptNameString(std::move(RawAnsi));
            return UtfN::StringToWString(RawAnsi);
        };
    }
    else
    {
        // GNames (NameArray) Logic
        LogInfo("Dumper-7: [FNameEntry] Mode: NameArray");
        
        uint8* FNameEntryNone = (uint8*)NameArray::GetNameEntry(0x0).GetAddress();
        uint8* FNameEntryIdxThree = (uint8*)NameArray::GetNameEntry(0x3).GetAddress();
        uint8* FNameEntryIdxEight = (uint8*)NameArray::GetNameEntry(0x8).GetAddress();

        if (IsBadReadPtr(FNameEntryNone) || IsBadReadPtr(FNameEntryIdxThree) || IsBadReadPtr(FNameEntryIdxEight)) {
            LogError("Dumper-7: Invalid FNameEntry pointers (None: %p)", FNameEntryNone);
            return;
        }

        LogInfo("Dumper-7: Analyzing FNameEntry structure...");

        // Scan for String Offset
        for (int i = 0; i < 0x20; i++)
        {
            if (*reinterpret_cast<uint32*>(FNameEntryNone + i) == 'enoN') // None
            {
                Off::FNameEntry::NameArray::StringOffset = i;
                LogInfo("Dumper-7: Found StringOffset at 0x%X", i);
                break;
            }
        }

        // Scan for Index Offset
        for (int i = 0; i < 0x20; i++)
        {
            if ((*reinterpret_cast<uint32*>(FNameEntryIdxThree + i) >> 1) == 0x3 &&
                (*reinterpret_cast<uint32*>(FNameEntryIdxEight + i) >> 1) == 0x8)
            {
                Off::FNameEntry::NameArray::IndexOffset = i;
                LogInfo("Dumper-7: Found IndexOffset at 0x%X", i);
                break;
            }
        }

        LogSuccess("Dumper-7: [FNameEntry] NameArray initialized (StringOffset: 0x%X, IndexOffset: 0x%X)",
                   Off::FNameEntry::NameArray::StringOffset, Off::FNameEntry::NameArray::IndexOffset);

        GetStr = [](uint8* NameEntry) -> UnrealString
        {
            // Per-game FNameEntry content decryption (no-op by default).
            NameEntry = NameArray::DecryptNameEntry(NameEntry);

            const int32 NameIdx = *reinterpret_cast<int32*>(NameEntry + Off::FNameEntry::NameArray::IndexOffset);
            const void* NameString = reinterpret_cast<void*>(NameEntry + Off::FNameEntry::NameArray::StringOffset);

            // Wide-char path: no per-string decryption hook applied. The DecryptNameString
            // signature is for narrow byte XOR; wide encryption (if any future game does it)
            // would need UTF-16-aware math, not a byte-level hook.
            if (NameIdx & NameWideMask)
                return UnrealString(reinterpret_cast<const TCHAR*>(NameString));

            // Decrypt at the raw-bytes level BEFORE wide conversion. Legacy entries are
            // null-terminated, so length comes from strlen — encryption schemes whose output
            // never contains 0x00 mid-string (the common case) work cleanly.
            const char* RawChars = reinterpret_cast<const char*>(NameString);
            std::string RawAnsi(RawChars, strlen(RawChars));
            RawAnsi = NameArray::DecryptNameString(std::move(RawAnsi));
            return UtfN::StringToWString<std::string>(RawAnsi);
        };
    }
}

bool NameArray::InitializeNameArray(uint8* NameArray)
{
    int32 ValidPtrCount = 0x0;
    int32 ZeroQWordCount = 0x0;

    if (!NameArray || IsBadReadPtr(NameArray))
        return false;

    for (int i = 0; i < 0x800; i += 0x8)
    {
        uint8* SomePtr = *reinterpret_cast<uint8**>(NameArray + i);

        if (SomePtr == 0)
        {
            ZeroQWordCount++;
        }
        else if (ZeroQWordCount == 0x0 && SomePtr != nullptr)
        {
            ValidPtrCount++;
        }
        else if (ZeroQWordCount > 0 && SomePtr != 0)
        {
            int32 NumElements = *reinterpret_cast<int32_t*>(NameArray + i);
            int32 NumChunks = *reinterpret_cast<int32_t*>(NameArray + i + 4);

            if (NumChunks == ValidPtrCount)
            {
                Off::NameArray::NumElements = i;
                Off::NameArray::MaxChunkIndex = i + 4;

                ByIndex = [](void* NamesArray, int32 ComparisonIndex, int32 NamePoolBlockOffsetBits) -> void*
                {
                    const int32 ChunkIdx = ComparisonIndex / 0x4000;
                    const int32 InChunk = ComparisonIndex % 0x4000;

                    if (ComparisonIndex > NameArray::GetNumElements())
                        return nullptr;

                    return reinterpret_cast<void***>(NamesArray)[ChunkIdx][InChunk];
                };

                LogSuccess("TNameEntryArray initialized successfully");
                return true;
            }
        }
    }

    LogError("Failed to initialize TNameEntryArray");
    return false;
}


/* "None" FNameEntry fingerprint at Blocks[0]: header must be a narrow Len=4
 * entry (in either standard `Header >> 6` or case-preserving
 * `(Header >> 1) & 0x7FFF` layouts), and the 4 chars after the header — once
 * passed through the installed DecryptNameString hook — must spell "None".
 *
 * Routing the chars through the hook makes this work for games like DeltaForce
 * where Blocks[0]+2 holds XOR-encrypted bytes instead of literal "None". For
 * games without encryption the default identity hook returns the chars
 * unchanged, so the check reduces to a literal "None" compare. */
static bool IsNoneFNameEntryAt(uint8* B0)
{
    if (!B0 || IsBadReadPtr(B0)) return false;

    const uint16 Header = *reinterpret_cast<uint16*>(B0);
    if (Header == 0) return false;

    const int32 LenStd = Header >> 6;
    const int32 LenCP  = (Header >> 1) & 0x7FFF;
    const bool  bIsWide = (Header & 1) != 0;
    if (bIsWide) return false;
    if (LenStd != 4 && LenCP != 4) return false;

    std::string Raw(reinterpret_cast<const char*>(B0 + 2), 4);
    std::string Decoded = NameArray::DecryptNameString(std::move(Raw));
    return Decoded == "None";
}

/* (Pass 1) Upstream Dumper-7 algorithm — int32 value scan.
 *
 * Walk int32 slots in [0, 0x200). For each, treat the value as a candidate
 * CurrentBlock; walk pointer slots after it, count non-null pointers, and
 * accept if value == count-1. Assumes ByteCursor sits immediately after
 * MaxChunkIndex (the standard UE 4.23-5.x adjacency).
 *
 * Works for: most stock UE games (DeltaForce, ArenaBreakout, etc.). */
static bool TryUpstreamNamePoolScan(uint8* NamePool)
{
    constexpr int32 OuterRange = 0x200;
    constexpr int32 InnerRange = 0x10000;
    constexpr int32 MaxAllowedNumInvalidPtrs = 0x1000;

    for (int32 i = 0; i < OuterRange; i += 4)
    {
        if (IsBadReadPtr(NamePool + i)) continue;

        const int32 PossibleMaxChunkIdx = *reinterpret_cast<int32*>(NamePool + i);
        if (PossibleMaxChunkIdx < 0 || PossibleMaxChunkIdx > 0x2000) continue;

        int32 NotNullptrCount = 0;
        bool bFoundFirstPtr = false;
        int32 ChunksStartCandidate = -1;
        int32 NumPtrsSinceLastValid = 0;

        for (int32 j = 0; j < InnerRange; j += 8)
        {
            const int32 ChunkOffset = i + 8 + j + (i % 8);
            if (IsBadReadPtr(NamePool + ChunkOffset)) break;

            uint8* ChunkPtr = *reinterpret_cast<uint8**>(NamePool + ChunkOffset);
            if (ChunkPtr != nullptr)
            {
                NotNullptrCount++;
                NumPtrsSinceLastValid = 0;
                if (!bFoundFirstPtr) { bFoundFirstPtr = true; ChunksStartCandidate = ChunkOffset; }
            }
            else
            {
                NumPtrsSinceLastValid++;
                if (NumPtrsSinceLastValid == MaxAllowedNumInvalidPtrs) break;
            }
        }

        if (!bFoundFirstPtr || PossibleMaxChunkIdx != NotNullptrCount - 1)
            continue;

        /* Validate by data anchor: Blocks[0] must point to the "None" FNameEntry.
         * Rejects false positives where the int32 + run-of-pointers happens to
         * line up midstream inside the real Blocks[] array — e.g. DeltaForce,
         * where i=0x104 looks valid but ChunksStart=0x110 actually points to
         * Blocks[8] of the real array that starts at 0xC8. */
        uint8** Blocks = reinterpret_cast<uint8**>(NamePool + ChunksStartCandidate);
        if (IsBadReadPtr(Blocks)) continue;
        if (!IsNoneFNameEntryAt(Blocks[0])) continue;

        Off::NameArray::MaxChunkIndex = i;
        Off::NameArray::ByteCursor    = i + 4;
        Off::NameArray::ChunksStart   = ChunksStartCandidate;
        LogInfo("FNamePool (upstream scan): MaxChunkIndex=0x%X, ByteCursor=0x%X, ChunksStart=0x%X, NumBlocks=%d (Blocks[0] -> 'None' confirmed)",
            i, i + 4, ChunksStartCandidate, NotNullptrCount);
        return true;
    }
    return false;
}

/* (Pass 2) "None" data anchor fallback.
 *
 * Some games (e.g. NGR / HOK: World) keep CurrentBlock and CurrentByteCursor
 * inside an inner FNamePoolPart sub-struct, so they're not adjacent to Blocks[]
 * in the outer FNamePool — upstream's scan can't match them.
 *
 * Anchors on `Blocks[0]` containing the "None" FNameEntry (always the first
 * entry in any FNamePool, in both standard and case-preserving header
 * layouts), then independently searches the int32 prefix for matching
 * CurrentBlock and CurrentByteCursor values. */
static bool TryNoneFingerprintScan(uint8* NamePool)
{
    constexpr int32 ScanStart = 0x40;
    constexpr int32 ScanEnd   = 0x100;
    constexpr int32 MaxBlocks = 0x2000;
    constexpr int32 BlockSize = 0x20000;

    int32 FoundChunksStart = -1;
    int32 NumValidBlocks = 0;
    uint8* LastBlock = nullptr;

    for (int32 O = ScanStart; O <= ScanEnd; O += 0x8)
    {
        uint8_t** Blocks = reinterpret_cast<uint8_t**>(NamePool + O);
        if (IsBadReadPtr(Blocks)) continue;
        if (!IsNoneFNameEntryAt(Blocks[0])) continue;

        FoundChunksStart = O;
        for (int32 i = 0; i < MaxBlocks; ++i)
        {
            if (IsBadReadPtr(&Blocks[i])) break;
            uint8* B = Blocks[i];
            if (!B || IsBadReadPtr(B)) break;
            LastBlock = B;
            NumValidBlocks = i + 1;
        }
        break;
    }

    if (FoundChunksStart < 0) return false;

    Off::NameArray::ChunksStart = FoundChunksStart;

    const int32 ExpectedCurrentBlock = NumValidBlocks - 1;
    int32 MaxChunkIndexOff = -1;
    for (int32 I = FoundChunksStart - 4; I >= 0; I -= 4)
    {
        if (*reinterpret_cast<int32*>(NamePool + I) == ExpectedCurrentBlock) { MaxChunkIndexOff = I; break; }
    }

    auto WalkCursor = [&](int32 Stride) -> int32
    {
        if (!LastBlock || Stride <= 0) return -1;
        int32 Cursor = 0;
        while (Cursor + 2 < BlockSize)
        {
            const uint16 H = *reinterpret_cast<uint16*>(LastBlock + Cursor);
            if (H == 0) break;
            const int32 Len = H >> 6;
            if (Len == 0) break;
            const bool bWide = (H & 1) != 0;
            int32 EntryBytes = 2 + (bWide ? Len * 2 : Len);
            EntryBytes = (EntryBytes + (Stride - 1)) & ~(Stride - 1);
            Cursor += EntryBytes;
        }
        return Cursor;
    };
    const int32 Cursor2 = WalkCursor(2);
    const int32 Cursor4 = WalkCursor(4);

    int32 ByteCursorOff = -1;
    for (int32 I = FoundChunksStart - 4; I >= 0; I -= 4)
    {
        if (I == MaxChunkIndexOff) continue;
        const int32 v = *reinterpret_cast<int32*>(NamePool + I);
        if (v == Cursor2 || v == Cursor4) { ByteCursorOff = I; break; }
    }

    Off::NameArray::MaxChunkIndex = (MaxChunkIndexOff >= 0) ? MaxChunkIndexOff : (FoundChunksStart - 0x14);
    Off::NameArray::ByteCursor    = (ByteCursorOff   >= 0) ? ByteCursorOff   : (FoundChunksStart - 0x10);

    LogInfo("FNamePool (None-anchor): ChunksStart=0x%X, MaxChunkIndex=0x%X (CurrentBlock=%d %s), ByteCursor=0x%X (%s)",
        FoundChunksStart,
        Off::NameArray::MaxChunkIndex, ExpectedCurrentBlock, MaxChunkIndexOff >= 0 ? "match" : "default",
        Off::NameArray::ByteCursor, ByteCursorOff >= 0 ? "match" : "default");
    return true;
}

/* Two-pass detection: upstream int32 scan first (covers standard UE 4.23-5.x
 * layouts), "None" data fingerprint fallback for exotic variants. */
static bool DetectFNamePoolOffsets(uint8* NamePool)
{
    if (TryUpstreamNamePoolScan(NamePool)) return true;
    LogInfo("FNamePool: upstream scan didn't match; trying 'None' data anchor fallback");
    if (TryNoneFingerprintScan(NamePool))  return true;
    LogError("FNamePool: both detection passes failed; using hardcoded defaults (0xC4/0xC8/0xD8)");
    Off::NameArray::MaxChunkIndex = 0xC4;
    Off::NameArray::ByteCursor    = 0xC8;
    Off::NameArray::ChunksStart   = 0xD0;
    return false;
}

bool NameArray::InitializeNamePool(uint8* NamePool)
{
    LogInfo("Initializing FNamePool...");

    // Basic pointer check
    if (IsBadReadPtr(NamePool)) {
        LogError("Invalid NamePool pointer");
        return false;
    }

    // Data-anchored offset discovery (replaces hardcoded MaxChunkIndex/ByteCursor/ChunksStart).
    DetectFNamePoolOffsets(NamePool);

    uint8_t** ChunkPtr = reinterpret_cast<uint8_t**>(NamePool + Off::NameArray::ChunksStart);
    if (IsBadReadPtr(ChunkPtr)) {
        LogError("Invalid ChunkPtr in NamePool");
        return false;
    }

    uint8* FirstChunk = *ChunkPtr;
    if (IsBadReadPtr(FirstChunk)) {
        LogError("Invalid FirstChunk (is bad read). Decryption or Offset failed.");
        return false;
    }

    int FNameEntryHeaderSize = 2;
    LogInfo("Found CoreUObject, HeaderSize: %lld", FNameEntryHeaderSize);
    NameEntryStride = FNameEntryHeaderSize == 2 ? 2 : 4;
    Off::InSDK::NameArray::FNameEntryStride = (int32)NameEntryStride;

    ByIndex = [](void* NamesArray, int32 ComparisonIndex, int32 NamePoolBlockOffsetBits) -> void*
    {
        uintptr_t BlockBit = NamePoolBlockOffsetBits;
        uintptr_t BlocksOffset = Off::NameArray::ChunksStart;
        uintptr_t ChunkMask = (1 << BlockBit) - 1;
        uintptr_t Stride = (uintptr_t)NameEntryStride;

        uintptr_t BlockOffset = ((ComparisonIndex >> BlockBit) * sizeof(void*));
        uintptr_t ChunkOffset = ((ComparisonIndex & ChunkMask) * Stride);

        uintptr_t PoolBase = reinterpret_cast<uintptr_t>(NamesArray);

        uint8_t** ChunkPtrLocation = reinterpret_cast<uint8_t**>(PoolBase + BlocksOffset + BlockOffset);
        if (IsBadReadPtr(ChunkPtrLocation)) return nullptr;

        uint8_t* Chunk = *ChunkPtrLocation;
        if (!Chunk || IsBadReadPtr(Chunk)) return nullptr;

        return (Chunk + ChunkOffset);
    };

    Settings::Internal::bUseNamePool = true;
    FNameEntry::Init(reinterpret_cast<uint8*>(ChunkPtr), FNameEntryHeaderSize);

    LogSuccess("FNamePool initialized successfully");
    return true;
}

/* * Finds a call to FName::GetNames, OR a reference to GNames directly.
 * * [iOS Port Note]: The original x64 instruction parsing logic has been replaced with
 * a generic search. On ARM64, direct pointer discovery via "ByteProperty" string reference
 * is usually reliable.
 */
inline std::pair<uintptr, bool> FindFNameGetNamesOrGNames(uintptr StartAddress)
{
    /* Range from "ByteProperty" which we want to search upwards */
    constexpr int32 SearchRange = 0x200;

    /* Find a reference to the string "ByteProperty" */
    /* Note: On iOS/Mach-O, FindByString works by searching for ADRL sequences pointing to the string */
    MemAddress BytePropertyStringRef = FindByStringInAllSections(TEXT("ByteProperty"), StartAddress);
    
    if (!BytePropertyStringRef) {
        // Fallback for ascii
        BytePropertyStringRef = FindByStringInAllSections("ByteProperty", StartAddress);
    }

    if (!BytePropertyStringRef)
        return { 0x0, false };

    /* On ARM64, accessing GNames usually looks like:
       ADRP X0, #Page_GNames
       ADD  X0, X0, #PageOff_GNames
       
       Or LDR X0, [GNames] if it's a pointer.
       
       We search near the string reference for potential global variables.
    */
    
    uintptr StringRefAddr = BytePropertyStringRef;
    
    // Simple Heuristic:
    // If we are in FName::StaticInit (or similar), GNames is likely initialized near here.
    // We scan for instructions referencing the .bss or .data section (where GNames would live).
    // This is a "blind" scan for pointers to valid memory that look like GNames.
    
    // For specific iOS games, pattern scanning is preferred.
    // Here we return the StringRefAddr as a starting point for debugging if manual offset is needed.
    
    // Note: The logic below is a placeholder. On iOS without x64 disassembly,
    // it's safer to rely on explicit patterns or the assumption that GNames is relative to this string.
    
    return { StringRefAddr, false };
};

bool NameArray::TryFindNameArray()
{
    LogInfo("Searching for TNameEntryArray GNames...");
    
    // iOS: We skip the Windows-specific "kernel32.dll" and "EnterCriticalSection" check.
    // Instead, we try to locate based on string references.
    
    auto [Address, bIsGNamesDirectly] = FindFNameGetNamesOrGNames(GetModuleBase());

    if (Address == 0x0)
    {
        LogError("ByteProperty string reference not found");
        return false;
    }

    // IOS TODO: Implement proper ARM64 instruction analysis here.
    // For now, we assume if we found the string, we might need manual verification or
    // we scan nearby for a pointer that looks like the NameArray.
    
    // Heuristic: Scan nearby memory for a pointer that points to a valid NameArray structure
    // (NumElements, MaxChunkIndex)
    
    uintptr ScanStart = Address - 0x200;
    uintptr ScanEnd = Address + 0x200;
    
    // Safety clamp
    if (ScanStart < GetModuleBase()) ScanStart = GetModuleBase();
    
    for (uintptr Ptr = ScanStart; Ptr < ScanEnd; Ptr += 8) {
         if (IsBadReadPtr(Ptr)) continue;
         
         // Candidate for GNames pointer?
         void* Candidate = *reinterpret_cast<void**>(Ptr);
         if (IsBadReadPtr(Candidate)) continue;
         
         // Check if it looks like a NameArray
         if (NameArray::InitializeNameArray((uint8*)Candidate)) {
             Off::InSDK::NameArray::GNames = (int32)GetOffset(Ptr);
             return true;
         }
    }
    
    return false;
}

bool NameArray::TryFindNamePool()
{
    LogInfo("Searching for FNamePool GNames...");
    auto GetARM64Reg = [](uint32 Instruction) -> int { return Instruction & 0x1F; };
    auto ResolveARM64Adr = [](uintptr AdrpAddr, uint32 AdrpInst, uint32 AddInst) -> uintptr {
        // Decode ADRP (Page Address)
        int32_t immlo = (AdrpInst >> 29) & 0x3;
        int32_t immhi = (AdrpInst >> 5) & 0x7FFFF;
        int64_t imm = (immhi << 2) | immlo;
        // Sign extend 21-bit immediate to 64-bit
        if (imm & 0x100000) imm |= ~0xFFFFF;
        // ADRP calculates relative to the 4KB page of the PC
        uintptr_t PageBase = AdrpAddr & ~0xFFF;
        uintptr_t PageOffset = imm << 12;
        uintptr_t BaseAddr = PageBase + PageOffset;
        // Decode ADD (Page Offset)
        // imm12: bits 10-21
        uint32_t imm12 = (AddInst >> 10) & 0xFFF;
        return BaseAddr + imm12;
    };
    
    uintptr_t StringRefAddr = FindByStringInAllSections(TEXT("ERROR_NAME_SIZE_EXCEEDED"));
    if (!StringRefAddr)
        StringRefAddr = FindByStringInAllSections("ERROR_NAME_SIZE_EXCEEDED");
    if (!StringRefAddr) {
        LogError("Could not find 'ERROR_NAME_SIZE_EXCEEDED' string reference.");
        return false;
    }
    
    LogInfo("[NameArray] Found Error String Ref at 0x%p", (void*)StringRefAddr);
    constexpr int32_t ScanRange   = 0x80;       constexpr uint32_t MaskADRP = 0x9F000000;
    constexpr uint32_t OpcodeADRP = 0x90000000; constexpr uint32_t MaskADD  = 0xFFC00000;
    constexpr uint32_t OpcodeADD  = 0x91000000;
    
    for (int i = 4; i < ScanRange; i += 4)
    {
        uintptr_t CurrentAddr = StringRefAddr + i;
        if (IsBadReadPtr(CurrentAddr))
            continue;
        uint32_t Inst = *reinterpret_cast<uint32_t*>(CurrentAddr);
        // Is this an ADRP instruction?
        if ((Inst & MaskADRP) != OpcodeADRP)
            continue;
        //  Does it target register X0? (Rd == 0)
        if (GetARM64Reg(Inst) != 0)
            continue;

        // Found potential ADRP. Now check the next 1-2 instructions for the pairing ADD.
        for (int k = 4; k <= 8; k += 4)
        {
            uintptr_t NextAddr = CurrentAddr + k;
            if (IsBadReadPtr(NextAddr))
                continue;

            uint32_t NextInst = *reinterpret_cast<uint32_t*>(NextAddr);
            // Is this an ADD (immediate) instruction?
            if ((NextInst & MaskADD) != OpcodeADD)
                continue;
            // Are both Destination (Rd) and Source (Rn) X0?
            int AddRd = GetARM64Reg(NextInst);
            int AddRn = (NextInst >> 5) & 0x1F;
            if (AddRd != 0 || AddRn != 0)
                continue;

            // Sequence Matched: Resolve the address
            uintptr_t ResolvedGNamesAddr = ResolveARM64Adr(CurrentAddr, Inst, NextInst);
            LogSuccess("Resolved GNames Address: 0x%p (from instruction at 0x%p)", (void*)ResolvedGNamesAddr, (void*)CurrentAddr);
            Off::InSDK::NameArray::GNames = (int32)GetOffset(ResolvedGNamesAddr);
            LogSuccess("Found NamePool at 0x%p", (void*)ResolvedGNamesAddr);
            return true;
        }
    }
    LogError("TryFindNamePool failed - instruction pattern not found.");
    return false;
}

bool NameArray::TryInit(bool bIsTestOnly)
{
    uintptr ImageBase = GetModuleBase();

    uint8* GNamesAddress = nullptr;

    bool bFoundNameArray = false;
    bool bFoundnamePool = false;

    if (NameArray::TryFindNameArray())
    {
        LogSuccess("Found 'TNameEntryArray GNames' at offset 0x%lX", Off::InSDK::NameArray::GNames);

        // Pass the raw ImageBase + offset; DecryptNameArray (default: deref once) yields the array pointer.
        GNamesAddress = reinterpret_cast<uint8*>(ImageBase + Off::InSDK::NameArray::GNames);
        Settings::Internal::bUseNamePool = false;
        bFoundNameArray = true;
    }
    else if (NameArray::TryFindNamePool())
    {
        LogSuccess("Found 'FNamePool GNames' at offset 0x%lX", Off::InSDK::NameArray::GNames);
        
        GNamesAddress = reinterpret_cast<uint8*>(ImageBase + Off::InSDK::NameArray::GNames); // No derefernce
        Settings::Internal::bUseNamePool = true;
        bFoundnamePool = true;
    }

    if (!bFoundNameArray && !bFoundnamePool)
    {
        LogError("\n\nCould not find GNames!\n\n");
        return false;
    }

    if (bIsTestOnly)
        return false;

    if (bFoundNameArray)
    {
        // Apply per-game TNameEntryArray pointer decryption (PUBG-style indirection, UE <= 4.22).
        // Hook returns TNameEntryArray** (address of global pointer) per Mj0x semantics;
        // runtime derefs once to get the live array pointer.
        const uintptr_t PtrToPtr = DecryptNameArray(reinterpret_cast<uintptr_t>(GNamesAddress));
        if (!PtrToPtr || IsBadReadPtr((void*)PtrToPtr))
        {
            LogError("DecryptNameArray returned invalid TNameEntryArray** (%p)", (void*)PtrToPtr);
            return false;
        }
        uint8* DecryptedAddr = *reinterpret_cast<uint8**>(PtrToPtr);
        LogInfo("DecryptNameArray: raw %p -> ** %p -> * %p", (void*)GNamesAddress, (void*)PtrToPtr, (void*)DecryptedAddr);

        if (NameArray::InitializeNameArray(DecryptedAddr))
        {
            GNames = DecryptedAddr;
            Settings::Internal::bUseNamePool = false;
            FNameEntry::Init();
            return true;
        }
    }
    else if (bFoundnamePool)
    {
        // Apply per-game FNamePool pointer decryption (Valorant-style indirection, UE 4.23+).
        // Default is identity, so no-op for games that don't need it.
        uint8* DecryptedAddr = reinterpret_cast<uint8*>(DecryptNamePool(reinterpret_cast<uintptr_t>(GNamesAddress)));
        if (DecryptedAddr != GNamesAddress)
            LogInfo("DecryptNamePool: %p -> %p", (void*)GNamesAddress, (void*)DecryptedAddr);

        if (NameArray::InitializeNamePool(DecryptedAddr))
        {
            GNames = DecryptedAddr;
            Settings::Internal::bUseNamePool = true;
            /* FNameEntry::Init() was moved into NameArray::InitializeNamePool to avoid duplicated logic */
            return true;
        }
    }

    //GNames = nullptr;
    //Off::InSDK::NameArray::GNames = 0x0;
    //Settings::Internal::bUseNamePool = false;

    LogError("The address that was found couldn't be used by the generator, this might be due to GNames-encryption");

    return false;
}

bool NameArray::TryInit(int32 OffsetOverride, bool bIsNamePool, const char* const ModuleName)
{
    uintptr ImageBase = GetModuleBase(ModuleName);

    uint8* GNamesAddress = nullptr;

    const bool bIsNameArrayOverride = !bIsNamePool;
    const bool bIsNamePoolOverride = bIsNamePool;

    bool bFoundNameArray = false;
    bool bFoundnamePool = false;

    Off::InSDK::NameArray::GNames = OffsetOverride;
    
    char buffer[256];

    if (bIsNameArrayOverride)
    {
        snprintf(buffer, sizeof(buffer), "Overwrote offset: 'TNameEntryArray GNames' set as offset 0x%lX\n", Off::InSDK::NameArray::GNames);
        LogSuccess(buffer);
        // Pass the raw ImageBase + offset; DecryptNameArray (default: deref once) yields the array pointer.
        GNamesAddress = reinterpret_cast<uint8*>(ImageBase + Off::InSDK::NameArray::GNames);
        Settings::Internal::bUseNamePool = false;
        bFoundNameArray = true;
    }
    else if (bIsNamePoolOverride)
    {
        snprintf(buffer, sizeof(buffer), "Overwrote offset: 'FNamePool GNames' set as offset 0x%lX\n", Off::InSDK::NameArray::GNames);
        LogSuccess(buffer);
        GNamesAddress = reinterpret_cast<uint8*>(ImageBase + Off::InSDK::NameArray::GNames); // No derefernce
        Settings::Internal::bUseNamePool = true;
        bFoundnamePool = true;
    }

    if (!bFoundNameArray && !bFoundnamePool)
    {
        LogError("\n\nCould not find GNames!\n\n");
        return false;
    }

    if (bFoundNameArray)
    {
        // Apply per-game TNameEntryArray pointer decryption (PUBG-style indirection, UE <= 4.22).
        // Hook returns TNameEntryArray** (address of global pointer) per Mj0x semantics;
        // runtime derefs once to get the live array pointer.
        const uintptr_t PtrToPtr = DecryptNameArray(reinterpret_cast<uintptr_t>(GNamesAddress));
        if (!PtrToPtr || IsBadReadPtr((void*)PtrToPtr))
        {
            LogError("DecryptNameArray returned invalid TNameEntryArray** (%p)", (void*)PtrToPtr);
            return false;
        }
        uint8* DecryptedAddr = *reinterpret_cast<uint8**>(PtrToPtr);
        LogInfo("DecryptNameArray: raw %p -> ** %p -> * %p", (void*)GNamesAddress, (void*)PtrToPtr, (void*)DecryptedAddr);

        if (NameArray::InitializeNameArray(DecryptedAddr))
        {
            GNames = DecryptedAddr;
            Settings::Internal::bUseNamePool = false;
            FNameEntry::Init();
            return true;
        }
    }
    else if (bFoundnamePool)
    {
        // Apply per-game FNamePool pointer decryption (Valorant-style indirection, UE 4.23+).
        uint8* DecryptedAddr = reinterpret_cast<uint8*>(DecryptNamePool(reinterpret_cast<uintptr_t>(GNamesAddress)));
        if (DecryptedAddr != GNamesAddress)
            LogInfo("DecryptNamePool: %p -> %p", (void*)GNamesAddress, (void*)DecryptedAddr);

        if (NameArray::InitializeNamePool(DecryptedAddr))
        {
            GNames = DecryptedAddr;
            Settings::Internal::bUseNamePool = true;
            /* FNameEntry::Init() was moved into NameArray::InitializeNamePool to avoid duplicated logic */
            return true;
        }
    }

    LogError("The address was overwritten, but couldn't be used. This might be due to GNames-encryption.\n");
    return false;
}

bool NameArray::SetGNamesWithoutCommiting()
{
    /* GNames is already set */
    if (Off::InSDK::NameArray::GNames != 0x0)
        return false;

    char buffer[256];

    if (NameArray::TryFindNameArray())
    {
        LogSuccess("Found 'TNameEntryArray GNames' at offset 0x%lX", Off::InSDK::NameArray::GNames);
        Settings::Internal::bUseNamePool = false;
        return true;
    }
    else if (NameArray::TryFindNamePool())
    {
        LogSuccess("Found 'FNamePool GNames' at offset 0x%lX", Off::InSDK::NameArray::GNames);
        Settings::Internal::bUseNamePool = true;
        return true;
    }

    LogError("Could not find GNames (neither TNameEntryArray nor FNamePool)");
    return false;
}

void NameArray::PostInit()
{
    if (!(GNames && Settings::Internal::bUseNamePool))
        return;

    LogInfo("NameArray: PostInit started. Detecting FNameBlockOffsetBits...");

    const int32 CurrentBlock = NameArray::GetNumChunks();
    if (CurrentBlock < 0)
    {
        LogError("PostInit: invalid CurrentBlock; defaulting FNameBlockOffsetBits to 0x10");
        NameArray::FNameBlockOffsetBits = 0x10;
        Off::InSDK::NameArray::FNamePoolBlockOffsetBits = 0x10;
        return;
    }

    /* Find max valid CompIdx across all UObjects.
     *
     * The previous algorithm (bump-bits-until-first-match) was fragile: a single
     * UObject with a corrupted/garbage CompIdx would push the bits value one or
     * two notches too high before any clean object equalized CurrentBlock.
     *
     * Robust replacement: take the maximum CompIdx seen across all objects
     * (filtering implausibly large values that can only be garbage), then find
     * the unique bits where `MaxCompIdx >> bits == CurrentBlock`. That equality
     * holds iff bits matches the pool's actual chunk-index width. */
    const int32 NumObjs = ObjectArray::Num();
    int32 MaxCompIdx = 0;
    for (int32 i = 0; i < NumObjs; ++i)
    {
        UEObject Obj = ObjectArray::GetByIndex(i);
        if (!Obj) continue;
        const int32 c = Obj.GetFName().GetCompIdx();
        if (c < 0 || c > 0x1000000) continue;       // filter garbage (~16M cap)
        if (c > MaxCompIdx) MaxCompIdx = c;
    }

    int32 FoundBits = -1;
    for (int32 bits = 0xE; bits <= 0x14; ++bits)
    {
        if ((MaxCompIdx >> bits) == CurrentBlock)
        {
            FoundBits = bits;
            break;
        }
    }

    if (FoundBits >= 0)
    {
        NameArray::FNameBlockOffsetBits = FoundBits;
        LogInfo("NameArray::FNameBlockOffsetBits: 0x%X (MaxCompIdx=0x%X, CurrentBlock=%d)",
            FoundBits, MaxCompIdx, CurrentBlock);
    }
    else
    {
        NameArray::FNameBlockOffsetBits = 0x10;
        LogError("PostInit: no bits in [0xE, 0x14] satisfies MaxCompIdx(0x%X) >> bits == CurrentBlock(%d); defaulting to 0x10",
            MaxCompIdx, CurrentBlock);
    }
    Off::InSDK::NameArray::FNamePoolBlockOffsetBits = NameArray::FNameBlockOffsetBits;
}

/* For UE 4.23+ FNamePool: walk the Blocks[] array and return the index of the
 * last valid (non-null) block pointer. Layout-independent: doesn't depend on
 * Off::NameArray::MaxChunkIndex pointing at a real CurrentBlock field, which is
 * unreliable on FNamePool variants that store CurrentBlock inside an inner
 * FNamePoolPart (e.g. NGR).
 *
 * For UE <= 4.22 TNameEntryArray: read the NumChunks field directly via the
 * offset discovered in InitializeNameArray (MaxChunkIndex points at the chunks
 * count adjacent to NumElements). That path doesn't have Blocks[] to walk. */
int32 NameArray::GetNumChunks()
{
    if (!Settings::Internal::bUseNamePool)
        return *reinterpret_cast<int32*>(GNames + Off::NameArray::MaxChunkIndex);

    uint8_t** Blocks = reinterpret_cast<uint8_t**>(GNames + Off::NameArray::ChunksStart);
    if (IsBadReadPtr(Blocks))
        return -1;

    constexpr int32 FNameMaxBlocks = 0x2000; // UE source constant
    int32 LastValid = -1;
    for (int32 i = 0; i < FNameMaxBlocks; ++i)
    {
        if (IsBadReadPtr(&Blocks[i]))
            break;
        uint8_t* B = Blocks[i];
        if (!B || IsBadReadPtr(B))
            break;
        LastValid = i;
    }
    return LastValid;
}

int32 NameArray::GetNumElements()
{
    return !Settings::Internal::bUseNamePool ? *reinterpret_cast<int32*>(GNames + Off::NameArray::NumElements) : 0;
}

/* Walk entries within the latest block and return the byte cursor of the next
 * write position. Same rationale as GetNumChunks — avoids depending on a stored
 * CurrentByteCursor field that's not at a reliable offset across UE variants. */
int32 NameArray::GetByteCursor()
{
    if (!Settings::Internal::bUseNamePool)
        return 0;

    const int32 CurrentBlock = GetNumChunks();
    if (CurrentBlock < 0)
        return 0;

    uint8_t** Blocks = reinterpret_cast<uint8_t**>(GNames + Off::NameArray::ChunksStart);
    uint8_t* BlockPtr = Blocks[CurrentBlock];
    if (!BlockPtr || IsBadReadPtr(BlockPtr))
        return 0;

    constexpr int32 MaxBlockBytes = 0x20000; // UE source: FNameBlockSize
    const int32 Stride = NameEntryStride > 0 ? NameEntryStride : 2;

    int32 Cursor = 0;
    while (Cursor + 2 < MaxBlockBytes)
    {
        const uint16_t Header = *reinterpret_cast<uint16_t*>(BlockPtr + Cursor);
        if (Header == 0)
            break;

        const int32 Length = Header >> 6;
        if (Length == 0)
            break;

        const bool bIsWide = (Header & 1) != 0;
        int32 EntryBytes = 2 + (bIsWide ? Length * 2 : Length);
        // Align up to NameEntryStride (2 normally, 4 for case-preserving builds).
        EntryBytes = (EntryBytes + (Stride - 1)) & ~(Stride - 1);
        Cursor += EntryBytes;
    }
    return Cursor;
}

FNameEntry NameArray::GetNameEntry(const void* Name)
{
    return ByIndex(GNames, FName(Name).GetCompIdx(), FNameBlockOffsetBits);
}

FNameEntry NameArray::GetNameEntry(int32 Idx)
{
    return ByIndex(GNames, Idx, FNameBlockOffsetBits);
}
