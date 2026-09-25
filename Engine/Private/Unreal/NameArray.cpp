#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <cstdio>
#include <cstring>

#include "Unreal/ObjectArray.h"
#include "Unreal/NameArray.h"
#include "Utils/Utils.h"
#include "Utils/Encoding/UtfN.hpp"
#include "Menu/Logger.h"

uint8* NameArray::GNames = nullptr;

/* UE source constants */
constexpr int32 FNameMaxBlocks = 0x2000;
constexpr int32 TNameEntryArrayElementsPerChunk = 0x4000;

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
    bHasUserStringDecryption = true;
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

/* Delta Force (iOS-Dumper-7 upstream): the header is plaintext, every character is XOR'd with a key derived from the length. */
std::string NameArray::DecryptDeltaForceNameString(std::string Decoded)
{
    const uint32_t Len = static_cast<uint32_t>(Decoded.size());
    if (Len == 0)
        return Decoded;

    uint32_t Key = 0;
    switch (Len % 9)
    {
        case 0: Key = (Len & 0x1F) + Len; break;
        case 1: Key = (Len ^ 0xDF) + Len; break;
        case 2: Key = (Len | 0xCF) + Len; break;
        case 3: Key = 33 * Len;           break;
        case 4: Key = Len + (Len >> 2);   break;
        case 5: Key = 3 * Len + 5;        break;
        case 6: Key = ((4 * Len) | 5) + Len; break;
        case 7: Key = ((Len >> 4) | 7) + Len; break;
        case 8: Key = (Len ^ 0xC) + Len;  break;
        default: Key = (Len ^ 0x40) + Len; break;
    }

    for (uint32_t i = 0; i < Len; ++i)
        Decoded[i] = static_cast<char>((Key & 0x80) ^ ~static_cast<uint8_t>(Decoded[i]));

    return Decoded;
}

/* Same function as source text, emitted into the generated SDK (FNameEntry::DecryptString). */
const char* const NameArray::DeltaForceNameStringDecryptionSrc =
    "[](std::string Decoded) -> std::string { const uint32_t Len = (uint32_t)Decoded.size(); if (Len == 0) return Decoded; "
    "uint32_t Key = 0; switch (Len % 9) { case 0: Key = (Len & 0x1F) + Len; break; case 1: Key = (Len ^ 0xDF) + Len; break; "
    "case 2: Key = (Len | 0xCF) + Len; break; case 3: Key = 33 * Len; break; case 4: Key = Len + (Len >> 2); break; "
    "case 5: Key = 3 * Len + 5; break; case 6: Key = ((4 * Len) | 5) + Len; break; case 7: Key = ((Len >> 4) | 7) + Len; break; "
    "case 8: Key = (Len ^ 0xC) + Len; break; default: Key = (Len ^ 0x40) + Len; break; } "
    "for (uint32_t i = 0; i < Len; ++i) Decoded[i] = (char)((Key & 0x80) ^ ~(uint8_t)Decoded[i]); return Decoded; }";

namespace
{
    struct FNamePoolEntryLayout
    {
        int32 HeaderOffset = 0x0;
        int32 StringOffset = 0x2;
        int32 Stride = 0x2;
        int32 LenShift = 6;
    };

    thread_local int32 NumberedNameRecursionDepth = 0;

    inline bool IsPlausibleHeapPointer(uintptr_t Value)
    {
        return Value != 0 && (Value & 0x7) == 0 && Value >= GetLowestMappableAddress() && (Value >> 47) == 0;
    }

    inline int32 AlignUp(int32 Value, int32 Alignment)
    {
        return (Value + Alignment - 1) & ~(Alignment - 1);
    }

    /* FNameEntryHeader: { bIsWide : 1, LowercaseProbeHash : 5, Len : 10 } or, with case preserving names, { bIsWide : 1, Len : 15 } */
    inline int32 GetNameLen(uint16 Header, int32 LenShift)
    {
        return LenShift == 1 ? ((Header >> 1) & 0x7FFF) : ((Header >> LenShift) & 0x3FF);
    }

    /* ANSI name entries only ever contain 7-bit characters, widen them 1:1 (also well-defined for garbage bytes). */
    UnrealString WidenAnsi(const std::string& Ansi)
    {
        UnrealString Out;
        Out.reserve(Ansi.size());

        for (const char C : Ansi)
            Out.push_back(static_cast<TCHAR>(static_cast<uint8>(C)));

        return Out;
    }

    /* Converts WIDECHAR text of the game (2 or 4 bytes per code unit) to the dumper's UnrealString. */
    UnrealString WideCharsToUnrealString(const uint8* Chars, int32 Len, int32 CharSize)
    {
        if (!Chars || Len <= 0)
            return UnrealString();

        if (CharSize == static_cast<int32>(sizeof(TCHAR)))
            return UnrealString(reinterpret_cast<const TCHAR*>(Chars), Len);

        std::u32string CodePoints;
        CodePoints.reserve(Len);

        if (CharSize == 2)
        {
            const char16_t* Src = reinterpret_cast<const char16_t*>(Chars);
            for (int32 i = 0; i < Len; i++)
            {
                char32_t C = Src[i];
                if (C >= 0xD800 && C <= 0xDBFF && (i + 1) < Len && Src[i + 1] >= 0xDC00 && Src[i + 1] <= 0xDFFF)
                {
                    C = 0x10000 + ((C - 0xD800) << 10) + (Src[i + 1] - 0xDC00);
                    i++;
                }
                CodePoints.push_back(C);
            }
        }
        else
        {
            const char32_t* Src = reinterpret_cast<const char32_t*>(Chars);
            CodePoints.assign(Src, Src + Len);
        }

        UnrealString Out;
        Out.reserve(CodePoints.size());

        for (char32_t C : CodePoints)
        {
            if constexpr (sizeof(TCHAR) == 2)
            {
                if (C > 0xFFFF && C <= 0x10FFFF)
                {
                    C -= 0x10000;
                    Out.push_back(static_cast<TCHAR>(0xD800 + (C >> 10)));
                    Out.push_back(static_cast<TCHAR>(0xDC00 + (C & 0x3FF)));
                    continue;
                }
            }
            Out.push_back(static_cast<TCHAR>(C));
        }

        return Out;
    }

    /* Length of a null-terminated string of CharSize-byte characters, at most MaxLen characters. */
    int32 BoundedStrLen(const uint8* Str, int32 CharSize, int32 MaxLen)
    {
        for (int32 i = 0; i < MaxLen; i++)
        {
            bool bIsNull = true;
            for (int32 b = 0; b < CharSize; b++)
                bIsNull = bIsNull && Str[(i * CharSize) + b] == 0;

            if (bIsNull)
                return i;
        }
        return MaxLen;
    }

    /* 'Entry' must be readable for at least Layout.StringOffset + Expected.size() bytes. */
    bool IsNarrowEntry(const uint8* Entry, const FNamePoolEntryLayout& Layout, std::string_view Expected, std::string (*Decrypt)(std::string))
    {
        const uint16 Header = *reinterpret_cast<const uint16*>(Entry + Layout.HeaderOffset);

        if ((Header & 0x1) != 0 || GetNameLen(Header, Layout.LenShift) != static_cast<int32>(Expected.size()))
            return false;

        std::string Chars(reinterpret_cast<const char*>(Entry + Layout.StringOffset), Expected.size());
        return Decrypt(std::move(Chars)) == Expected;
    }

    /* Blocks[0] of every FNamePool starts with the "None" entry followed by "ByteProperty" (hardcoded EName order). */
    bool DetectNamePoolEntryLayout(const uint8* Block0, std::string (*Decrypt)(std::string), FNamePoolEntryLayout& OutLayout)
    {
        constexpr FNamePoolEntryLayout Candidates[] =
        {
            { 0x0, 0x2, 0x2, 6 }, // UE4.23+ (WIDECHAR is 2 bytes)
            { 0x0, 0x4, 0x4, 6 }, // WIDECHAR is a 4-byte wchar_t -> 4-byte aligned entries
            { 0x4, 0x6, 0x4, 1 }, // WITH_CASE_PRESERVING_NAME: FNameEntryId ComparisonId, then { bIsWide : 1, Len : 15 }
            { 0x4, 0x6, 0x4, 6 },
            { 0x0, 0x2, 0x2, 1 },
            { 0x0, 0x2, 0x2, 5 },
        };

        /* Covers the two entries of every candidate layout (<= 0x6 + 4 + 2 + 0x6 + 12 bytes) */
        if (!IsPlausibleHeapPointer(reinterpret_cast<uintptr_t>(Block0)) || IsBadReadRange(Block0, 0x40))
            return false;

        for (const FNamePoolEntryLayout& Candidate : Candidates)
        {
            if (!IsNarrowEntry(Block0, Candidate, "None", Decrypt))
                continue;

            const int32 SecondEntryOffset = AlignUp(Candidate.StringOffset + 4, Candidate.Stride);

            if (!IsNarrowEntry(Block0 + SecondEntryOffset, Candidate, "ByteProperty", Decrypt))
                continue;

            OutLayout = Candidate;
            return true;
        }

        return false;
    }

    enum class ENameDecryption { None, Installed, DeltaForce };

    /* Tries the currently installed string decryption and, unless the user installed one themselves, Delta Force's. */
    ENameDecryption DetectEntryLayoutAndDecryption(const uint8* Block0, FNamePoolEntryLayout& OutLayout)
    {
        if (DetectNamePoolEntryLayout(Block0, NameArray::DecryptNameString, OutLayout))
            return ENameDecryption::Installed;

        if (!NameArray::bHasUserStringDecryption && DetectNamePoolEntryLayout(Block0, &NameArray::DecryptDeltaForceNameString, OutLayout))
            return ENameDecryption::DeltaForce;

        return ENameDecryption::None;
    }

    /* Byte offset behind the last entry of a FNamePool block (FNameEntryAllocator::CurrentByteCursor for the current block). */
    int32 WalkBlockCursor(const uint8* Block, const FNamePoolEntryLayout& Layout, int32 MaxBlockBytes)
    {
        if (!Block)
            return -1;

        uintptr_t LastCheckedPage = 0;
        int32 Cursor = 0;

        while ((Cursor + Layout.StringOffset + 0x10) < MaxBlockBytes)
        {
            const uintptr_t EntryAddress = reinterpret_cast<uintptr_t>(Block) + Cursor;
            const uintptr_t EntryPage = (EntryAddress + Layout.StringOffset + 0x10) & ~static_cast<uintptr_t>(0xFFF);

            if (EntryPage != LastCheckedPage)
            {
                if (IsBadReadPtr(EntryPage < EntryAddress ? EntryAddress : EntryPage))
                    return -1;

                LastCheckedPage = EntryPage;
            }

            const uint16 Header = *reinterpret_cast<const uint16*>(EntryAddress + Layout.HeaderOffset);
            const int32 Len = GetNameLen(Header, Layout.LenShift);

            /* An all-zero header marks unused memory. Len == 0 with a non-zero header is a numbered entry (FNAME_OUTLINE_NUMBER). */
            if (Header == 0)
                break;

            const int32 WideCharSize = (Layout.Stride == 4 && (Layout.StringOffset % 4) == 0) ? 4 : 2;
            const int32 DataSize = Len == 0 ? 0x8 : ((Header & 0x1) ? Len * WideCharSize : Len);
            Cursor += AlignUp(Layout.StringOffset + DataSize, Layout.Stride);
        }

        return Cursor;
    }
}

FNameEntry::FNameEntry(void* Ptr)
    : Address((uint8*)Ptr)
{
}

UnrealString FNameEntry::GetWString()
{
    if (!Address || !GetStr)
        return UnrealString();

    return GetStr(Address);
}

std::string FNameEntry::GetString()
{
    if (!Address)
        return "";

    // DecryptNameString is applied inside GetStr on the raw narrow bytes, so GetWString() already returns decrypted chars here.
    return UtfN::WStringToString(GetWString());
}

void* FNameEntry::GetAddress()
{
    return Address;
}

bool FNameEntry::Init(const uint8* FirstChunkPtr, int64 NameEntryStringOffset)
{
    LogInfo("Dumper-7: [FNameEntry] Initializing...");

    if (Settings::Internal::bUseNamePool)
    {
        /* Off::FNameEntry::NamePool::HeaderOffset/StringOffset, the stride and the Len-shift were detected in InitializeNamePoolFromBlocks. */
        LogSuccess("Dumper-7: [FNameEntry] NamePool (Header: 0x%X, String: 0x%X, Stride: %d, LenShift: %d, WideChar: %d bytes)",
            Off::FNameEntry::NamePool::HeaderOffset, Off::FNameEntry::NamePool::StringOffset, Off::InSDK::NameArray::FNameEntryStride, FNameEntryLengthShiftCount, GameWideCharSize);

        GetStr = [](uint8* NameEntry) -> UnrealString
        {
            // Per-game FNameEntry content decryption (no-op by default).
            NameEntry = NameArray::DecryptNameEntry(NameEntry);

            if (!NameEntry)
                return UnrealString();

            const uint16 HeaderWithoutNumber = *reinterpret_cast<uint16*>(NameEntry + Off::FNameEntry::NamePool::HeaderOffset);
            const int32 NameLen = GetNameLen(HeaderWithoutNumber, FNameEntry::FNameEntryLengthShiftCount);

            if (NameLen == 0)
            {
                /* FNAME_OUTLINE_NUMBER: { FNameEntryId Id; uint32 Number; } referencing the base entry. Guard against self-references. */
                if (HeaderWithoutNumber == 0 || NumberedNameRecursionDepth >= 2)
                    return UnrealString();

                const int32 EntryIdOffset = Off::FNameEntry::NamePool::StringOffset + ((Off::FNameEntry::NamePool::StringOffset == 6) * 2);

                const int32 NextEntryIndex = *reinterpret_cast<int32*>(NameEntry + EntryIdOffset);
                const int32 Number = *reinterpret_cast<int32*>(NameEntry + EntryIdOffset + sizeof(int32));

                NumberedNameRecursionDepth++;
                UnrealString BaseName = NameArray::GetNameEntry(NextEntryIndex).GetWString();
                NumberedNameRecursionDepth--;

                if (Number > 0)
                    return BaseName + TEXT('_') + ToUEString(Number - 1);

                return BaseName;
            }

            /* A garbage ComparisonIndex can point near the end of a block. Entries never cross into unmapped memory, garbage might. */
            const uintptr_t CharsStart = reinterpret_cast<uintptr_t>(NameEntry + Off::FNameEntry::NamePool::StringOffset);
            const uintptr_t CharsSize = static_cast<uintptr_t>(NameLen) * ((HeaderWithoutNumber & NameWideMask) ? FNameEntry::GameWideCharSize : 1);

            if (((CharsStart ^ (CharsStart + CharsSize - 1)) & ~static_cast<uintptr_t>(0xFFF)) != 0 && IsBadReadRange(reinterpret_cast<void*>(CharsStart), CharsSize))
                return UnrealString();

            // Wide-char path: no DecryptNameString applied (no known game encrypts wide names).
            if (HeaderWithoutNumber & NameWideMask)
                return WideCharsToUnrealString(NameEntry + Off::FNameEntry::NamePool::StringOffset, NameLen, FNameEntry::GameWideCharSize);

            /* Decrypt at the raw-bytes level BEFORE widening. The per-Len key of Delta Force needs the undecoded length. */
            std::string RawAnsi(reinterpret_cast<const char*>(NameEntry + Off::FNameEntry::NamePool::StringOffset), NameLen);
            RawAnsi = NameArray::DecryptNameString(std::move(RawAnsi));
            return WidenAnsi(RawAnsi);
        };

        return true;
    }

    // GNames (NameArray) Logic
    LogInfo("Dumper-7: [FNameEntry] Mode: NameArray");

    uint8* FNameEntryNone = (uint8*)NameArray::GetNameEntry(0x0).GetAddress();
    uint8* FNameEntryIdxThree = (uint8*)NameArray::GetNameEntry(0x3).GetAddress();
    uint8* FNameEntryIdxEight = (uint8*)NameArray::GetNameEntry(0x8).GetAddress();

    if (IsBadReadRange(FNameEntryNone, 0x30) || IsBadReadRange(FNameEntryIdxThree, 0x30) || IsBadReadRange(FNameEntryIdxEight, 0x30))
    {
        LogError("Dumper-7: Invalid FNameEntry pointers (None: %p)", FNameEntryNone);
        return false;
    }

    LogInfo("Dumper-7: Analyzing FNameEntry structure...");

    int32 StringOffset = -1;
    int32 IndexOffset = -1;

    // Scan for String Offset
    for (int i = 0; i < 0x20; i++)
    {
        if (*reinterpret_cast<uint32*>(FNameEntryNone + i) == 'enoN') // None
        {
            StringOffset = i;
            break;
        }
    }

    // Scan for Index Offset
    for (int i = 0; i < 0x20; i += 4)
    {
        if ((*reinterpret_cast<uint32*>(FNameEntryIdxThree + i) >> 1) == 0x3 &&
            (*reinterpret_cast<uint32*>(FNameEntryIdxEight + i) >> 1) == 0x8)
        {
            IndexOffset = i;
            break;
        }
    }

    if (StringOffset < 0 || IndexOffset < 0)
    {
        LogError("Dumper-7: [FNameEntry] NameArray entry layout not found (StringOffset: %d, IndexOffset: %d)", StringOffset, IndexOffset);
        return false;
    }

    Off::FNameEntry::NameArray::StringOffset = StringOffset;
    Off::FNameEntry::NameArray::IndexOffset = IndexOffset;

    /* UE < 4.21 on iOS uses a 4-byte wchar_t TCHAR for these legacy name tables. */
    GameWideCharSize = 4;

    LogSuccess("Dumper-7: [FNameEntry] NameArray initialized (StringOffset: 0x%X, IndexOffset: 0x%X)",
               Off::FNameEntry::NameArray::StringOffset, Off::FNameEntry::NameArray::IndexOffset);

    GetStr = [](uint8* NameEntry) -> UnrealString
    {
        // Per-game FNameEntry content decryption (no-op by default).
        NameEntry = NameArray::DecryptNameEntry(NameEntry);

        if (!NameEntry)
            return UnrealString();

        constexpr int32 NameSize = 1024; // NAME_SIZE

        const int32 NameIdx = *reinterpret_cast<int32*>(NameEntry + Off::FNameEntry::NameArray::IndexOffset);
        const uint8* NameString = NameEntry + Off::FNameEntry::NameArray::StringOffset;

        if (NameIdx & NameWideMask)
            return WideCharsToUnrealString(NameString, BoundedStrLen(NameString, FNameEntry::GameWideCharSize, NameSize), FNameEntry::GameWideCharSize);

        // Decrypt at the raw-bytes level BEFORE widening. Legacy entries are null-terminated.
        const char* RawChars = reinterpret_cast<const char*>(NameString);
        std::string RawAnsi(RawChars, strnlen(RawChars, NameSize));
        RawAnsi = NameArray::DecryptNameString(std::move(RawAnsi));
        return WidenAnsi(RawAnsi);
    };

    return true;
}

bool NameArray::InitializeNameArray(uint8* NameArray)
{
    int32 ValidPtrCount = 0x0;
    int32 ZeroQWordCount = 0x0;

    if (!IsPlausibleHeapPointer(reinterpret_cast<uintptr_t>(NameArray)) || IsBadReadRange(NameArray, 0x808))
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

            if (NumChunks == ValidPtrCount && NumElements > 0 && NumElements <= (NumChunks * TNameEntryArrayElementsPerChunk))
            {
                Off::NameArray::NumElements = i;
                Off::NameArray::MaxChunkIndex = i + 4;

                ByIndex = [](void* NamesArray, int32 ComparisonIndex, int32 NamePoolBlockOffsetBits) -> void*
                {
                    if (ComparisonIndex < 0 || ComparisonIndex >= NameArray::GetNumElements())
                        return nullptr;

                    const int32 ChunkIdx = ComparisonIndex / TNameEntryArrayElementsPerChunk;
                    const int32 InChunk = ComparisonIndex % TNameEntryArrayElementsPerChunk;

                    void** Chunk = reinterpret_cast<void***>(NamesArray)[ChunkIdx];
                    if (!Chunk)
                        return nullptr;

                    return Chunk[InChunk];
                };

                LogSuccess("TNameEntryArray initialized successfully");
                return true;
            }

            break;
        }
    }

    LogError("Failed to initialize TNameEntryArray");
    return false;
}

bool NameArray::InitializeNamePoolFromBlocks(uint8* BlocksSlot, uintptr_t SearchLowerBound, uintptr_t ImageBase, uint8* ForcedPoolBase)
{
    uint8** Blocks = reinterpret_cast<uint8**>(BlocksSlot);

    FNamePoolEntryLayout EntryLayout;
    const ENameDecryption Decryption = DetectEntryLayoutAndDecryption(Blocks[0], EntryLayout);

    if (Decryption == ENameDecryption::None)
        return false;

    if (Decryption == ENameDecryption::DeltaForce)
    {
        LogSuccess("FNamePool entries are encrypted with Delta Force's per-length XOR, installing the name decryption");
        DecryptNameString = &NameArray::DecryptDeltaForceNameString;
        NameStringDecryptionLambdaStr = DeltaForceNameStringDecryptionSrc;
    }

    /* FNameEntryAllocator::Blocks: consecutive block pointers up to CurrentBlock, zero afterwards. */
    int32 NumBlocks = 0;
    for (int32 i = 0; i < FNameMaxBlocks; i++)
    {
        if (i > 0 && (reinterpret_cast<uintptr_t>(&Blocks[i]) & 0xFFF) == 0 && IsBadReadPtr(&Blocks[i]))
            break;

        uint8* Block = Blocks[i];
        if (!IsPlausibleHeapPointer(reinterpret_cast<uintptr_t>(Block)) || IsBadReadPtr(Block))
            break;

        NumBlocks++;
    }

    if (NumBlocks <= 0)
        return false;

    const int32 CurrentBlock = NumBlocks - 1;
    const int32 MaxBlockBytes = EntryLayout.Stride << 16; // FNameBlockOffsetBits is 16 in stock UE
    const int32 Cursor = WalkBlockCursor(Blocks[CurrentBlock], EntryLayout, MaxBlockBytes);

    /* CurrentBlock and CurrentByteCursor are stored right before Blocks[] in stock UE, but some games move them. Search by value. */
    uintptr_t CurrentBlockAddr = 0x0;
    uintptr_t ByteCursorAddr = 0x0;

    const uintptr_t BlocksAddr = reinterpret_cast<uintptr_t>(BlocksSlot);

    for (uintptr_t Address = BlocksAddr - 4; (Address + 0x40) >= BlocksAddr && Address >= SearchLowerBound; Address -= 4)
    {
        const int32 Value = *reinterpret_cast<const int32*>(Address);

        if (!CurrentBlockAddr && Value == CurrentBlock)
        {
            CurrentBlockAddr = Address;
            continue;
        }

        if (!ByteCursorAddr && Cursor > 0 && Value > 0 && Value <= MaxBlockBytes && (Value > Cursor ? Value - Cursor : Cursor - Value) <= 0x200)
            ByteCursorAddr = Address;
    }

    if (!CurrentBlockAddr)
        CurrentBlockAddr = BlocksAddr - 0x8;

    if (!ByteCursorAddr)
        ByteCursorAddr = (CurrentBlockAddr + 4) != BlocksAddr ? (CurrentBlockAddr + 4) : (BlocksAddr - 0x4);

    uintptr_t Lowest = BlocksAddr;
    Lowest = CurrentBlockAddr < Lowest ? CurrentBlockAddr : Lowest;
    Lowest = ByteCursorAddr < Lowest ? ByteCursorAddr : Lowest;

    uintptr_t PoolBase = Lowest;

    if (ForcedPoolBase && reinterpret_cast<uintptr_t>(ForcedPoolBase) <= Lowest)
    {
        PoolBase = reinterpret_cast<uintptr_t>(ForcedPoolBase);
    }
    else
    {
        /* FNamePool starts with FNameEntryAllocator::Lock, an FRWLock (pthread_rwlock_t) whose signature is 0x2DA8B3B4 on Apple platforms. */
        constexpr uint64 PthreadRWLockSignature = 0x2DA8B3B4;

        for (uintptr_t Address = (Lowest - 8) & ~static_cast<uintptr_t>(7); (Address + 0x100) >= Lowest && Address >= SearchLowerBound; Address -= 8)
        {
            if (*reinterpret_cast<const uint64*>(Address) == PthreadRWLockSignature)
            {
                PoolBase = Address;
                break;
            }
        }
    }

    GNames = reinterpret_cast<uint8*>(PoolBase);
    Off::InSDK::NameArray::GNames = static_cast<int32>(PoolBase - ImageBase);
    Off::NameArray::ChunksStart = static_cast<int32>(BlocksAddr - PoolBase);
    Off::NameArray::MaxChunkIndex = static_cast<int32>(CurrentBlockAddr - PoolBase);
    Off::NameArray::ByteCursor = static_cast<int32>(ByteCursorAddr - PoolBase);

    Off::FNameEntry::NamePool::HeaderOffset = EntryLayout.HeaderOffset;
    Off::FNameEntry::NamePool::StringOffset = EntryLayout.StringOffset;

    NameEntryStride = EntryLayout.Stride;
    Off::InSDK::NameArray::FNameEntryStride = EntryLayout.Stride;

    FNameEntry::FNameEntryLengthShiftCount = EntryLayout.LenShift;
    FNameEntry::GameWideCharSize = (EntryLayout.Stride == 4 && (EntryLayout.StringOffset % 4) == 0) ? 4 : 2;

    bAllBlockSlotsReadable = !IsBadReadRange(Blocks, FNameMaxBlocks * sizeof(void*));
    NumKnownBlocks = NumBlocks;

    ByIndex = [](void* NamesArray, int32 ComparisonIndex, int32 NamePoolBlockOffsetBits) -> void*
    {
        if (ComparisonIndex < 0 || NamePoolBlockOffsetBits <= 0 || NamePoolBlockOffsetBits > 0x18)
            return nullptr;

        const int32 BlockIdx = ComparisonIndex >> NamePoolBlockOffsetBits;
        if (BlockIdx >= FNameMaxBlocks)
            return nullptr;

        const uintptr_t InBlockOffset = static_cast<uintptr_t>(ComparisonIndex & ((1 << NamePoolBlockOffsetBits) - 1)) * static_cast<uintptr_t>(NameEntryStride);

        uint8** BlockPtrs = reinterpret_cast<uint8**>(reinterpret_cast<uint8*>(NamesArray) + Off::NameArray::ChunksStart);

        /* Blocks are allocated in order and never freed. A block the game allocated after initialization is accepted once
         * every slot up to it holds a valid block, anything else (e.g. from a garbage ComparisonIndex) is rejected. */
        if (BlockIdx >= NumKnownBlocks)
        {
            for (int32 i = NumKnownBlocks; i <= BlockIdx; i++)
            {
                if (!bAllBlockSlotsReadable && IsBadReadPtr(&BlockPtrs[i]))
                    return nullptr;

                uint8* NewBlock = BlockPtrs[i];
                if (!IsPlausibleHeapPointer(reinterpret_cast<uintptr_t>(NewBlock)) || IsBadReadPtr(NewBlock))
                    return nullptr;
            }

            NumKnownBlocks = BlockIdx + 1;
        }

        uint8* Block = BlockPtrs[BlockIdx];
        if (!Block)
            return nullptr;

        return Block + InBlockOffset;
    };

    Settings::Internal::bUseNamePool = true;

    if (!FNameEntry::Init())
        return false;

    LogSuccess("FNamePool: GNames offset 0x%X { CurrentBlock=0x%X, ByteCursor=0x%X, Blocks=0x%X }, %d blocks, name decryption: %s",
        Off::InSDK::NameArray::GNames, Off::NameArray::MaxChunkIndex, Off::NameArray::ByteCursor, Off::NameArray::ChunksStart, NumBlocks,
        Decryption == ENameDecryption::DeltaForce ? "Delta Force" : (NameStringDecryptionLambdaStr.empty() ? "none" : "custom"));

    return true;
}

bool NameArray::InitializeNamePool(uint8* NamePool)
{
    LogInfo("Initializing FNamePool...");

    if (!NamePool || IsBadReadRange(NamePool, 0x10))
    {
        LogError("Invalid NamePool pointer");
        return false;
    }

    const uintptr_t ImageBase = GetModuleBase(Settings::General::DefaultModuleName);

    /* Locate FNameEntryAllocator::Blocks[] inside the pool: the first slot whose block starts with the "None" entry. */
    for (int32 Offset = 0x0; Offset <= 0x200; Offset += 0x8)
    {
        uint8** Slot = reinterpret_cast<uint8**>(NamePool + Offset);

        if (((reinterpret_cast<uintptr_t>(Slot) & 0xFFF) < 0x8 || Offset == 0) && IsBadReadRange(Slot, sizeof(void*)))
            break;

        if (!IsPlausibleHeapPointer(reinterpret_cast<uintptr_t>(*Slot)))
            continue;

        FNamePoolEntryLayout EntryLayout;
        if (DetectEntryLayoutAndDecryption(*Slot, EntryLayout) == ENameDecryption::None)
            continue;

        return InitializeNamePoolFromBlocks(reinterpret_cast<uint8*>(Slot), reinterpret_cast<uintptr_t>(NamePool), ImageBase, NamePool);
    }

    LogError("FNamePool: no Blocks[] array with the 'None' entry found at %p. Decryption or offset is wrong.", NamePool);
    return false;
}

bool NameArray::TryFindNamePoolByData(const char* const ModuleName)
{
    LogInfo("Searching for FNamePool in the data segments...");

    const auto [ImageBase, ImageSize, Header, Slide] = GetImageBaseAndSize(ModuleName);
    if (!Header)
        return false;

    for (const MachSegmentInfo& Seg : GetWritableImageSegments(ModuleName))
    {
        const uintptr_t SegEnd = Seg.Start + Seg.Size;

        for (uintptr_t Slot = Seg.Start; (Slot + sizeof(void*)) <= SegEnd; Slot += sizeof(void*))
        {
            const uintptr_t Block0 = *reinterpret_cast<const uintptr_t*>(Slot);

            /* Blocks are heap allocations, never part of the image */
            if (!IsPlausibleHeapPointer(Block0) || (Block0 >= ImageBase && Block0 < (ImageBase + ImageSize)))
                continue;

            FNamePoolEntryLayout EntryLayout;
            if (DetectEntryLayoutAndDecryption(reinterpret_cast<const uint8*>(Block0), EntryLayout) == ENameDecryption::None)
                continue;

            LogInfo("FNamePool Blocks[0] found at offset 0x%lX", static_cast<unsigned long>(Slot - ImageBase));

            const uintptr_t SearchLowerBound = (Slot - Seg.Start) > 0x200 ? (Slot - 0x200) : Seg.Start;

            if (InitializeNamePoolFromBlocks(reinterpret_cast<uint8*>(Slot), SearchLowerBound, ImageBase))
                return true;
        }
    }

    LogInfo("FNamePool wasn't found in the data segments");
    return false;
}

bool NameArray::TryFindNameArrayByData(const char* const ModuleName)
{
    LogInfo("Searching for TNameEntryArray in the data segments...");

    if (!NameArrayDecryptionLambdaStr.empty())
    {
        LogInfo("A TNameEntryArray pointer decryption is installed, skipping the data search");
        return false;
    }

    const auto [ImageBase, ImageSize, Header, Slide] = GetImageBaseAndSize(ModuleName);
    if (!Header)
        return false;

    auto EntryHasName = [](const uint8* Entry, int32 StringOffset, const char* Name, size_t NameLen) -> bool
    {
        return IsPlausibleHeapPointer(reinterpret_cast<uintptr_t>(Entry)) && !IsBadReadRange(Entry, StringOffset + NameLen + 1)
            && memcmp(Entry + StringOffset, Name, NameLen + 1) == 0;
    };

    for (const MachSegmentInfo& Seg : GetWritableImageSegments(ModuleName))
    {
        const uintptr_t SegEnd = Seg.Start + Seg.Size;

        for (uintptr_t Slot = Seg.Start; (Slot + sizeof(void*)) <= SegEnd; Slot += sizeof(void*))
        {
            /* GNames is a 'TNameEntryArray*' -> the array, whose first member is the chunk table */
            const uintptr_t ArrayPtr = *reinterpret_cast<const uintptr_t*>(Slot);

            if (!IsPlausibleHeapPointer(ArrayPtr) || (ArrayPtr >= ImageBase && ArrayPtr < (ImageBase + ImageSize)) || IsBadReadPtr(ArrayPtr))
                continue;

            const uintptr_t Chunk0 = *reinterpret_cast<const uintptr_t*>(ArrayPtr);
            if (!IsPlausibleHeapPointer(Chunk0) || IsBadReadRange(reinterpret_cast<void*>(Chunk0), 2 * sizeof(void*)))
                continue;

            const uint8* Entry0 = reinterpret_cast<const uint8* const*>(Chunk0)[0];
            const uint8* Entry1 = reinterpret_cast<const uint8* const*>(Chunk0)[1];

            bool bFound = false;
            for (int32 StringOffset = 0x8; StringOffset <= 0x18 && !bFound; StringOffset += 0x4)
                bFound = EntryHasName(Entry0, StringOffset, "None", 4) && EntryHasName(Entry1, StringOffset, "ByteProperty", 12);

            if (!bFound || !InitializeNameArray(reinterpret_cast<uint8*>(ArrayPtr)))
                continue;

            GNames = reinterpret_cast<uint8*>(ArrayPtr);
            Off::InSDK::NameArray::GNames = static_cast<int32>(Slot - ImageBase);
            Settings::Internal::bUseNamePool = false;

            if (!FNameEntry::Init())
            {
                GNames = nullptr;
                continue;
            }

            LogSuccess("Found 'TNameEntryArray GNames' at offset 0x%X", Off::InSDK::NameArray::GNames);
            return true;
        }
    }

    LogInfo("TNameEntryArray wasn't found in the data segments");
    return false;
}

/*
* Code-pattern fallback for TNameEntryArray. On ARM64 there's no reliable instruction pattern (yet), the data-anchored
* search (TryFindNameArrayByData) is used instead.
*/
bool NameArray::TryFindNameArray()
{
    return false;
}

bool NameArray::TryFindNamePool()
{
    LogInfo("Searching for FNamePool GNames via code references...");
    auto GetARM64Reg = [](uint32 Instruction) -> int { return Instruction & 0x1F; };
    auto ResolveARM64Adr = [](uintptr AdrpAddr, uint32 AdrpInst, uint32 AddInst) -> uintptr {
        // Decode ADRP (Page Address)
        int64_t immlo = (AdrpInst >> 29) & 0x3;
        int64_t immhi = (AdrpInst >> 5) & 0x7FFFF;
        int64_t imm = (immhi << 2) | immlo;
        // Sign extend 21-bit immediate to 64-bit
        if (imm & 0x100000) imm |= ~0xFFFFFll;
        // ADRP calculates relative to the 4KB page of the PC
        uintptr_t PageBase = AdrpAddr & ~0xFFFull;
        uintptr_t PageOffset = static_cast<uintptr_t>(imm << 12);
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
            Off::InSDK::NameArray::GNames = (int32)GetOffset(ResolvedGNamesAddr);
            LogInfo("Possible NamePool at 0x%p (from instruction at 0x%p)", (void*)ResolvedGNamesAddr, (void*)CurrentAddr);
            return true;
        }
    }
    LogError("TryFindNamePool failed - instruction pattern not found.");
    return false;
}

bool NameArray::TryInit(bool bIsTestOnly)
{
    const char* const ModuleName = Settings::General::DefaultModuleName;

    /* 1. Data anchored: works for stock games and Delta Force (encrypted names), independent of code patterns. */
    if (NameArray::TryFindNamePoolByData(ModuleName))
        return true;

    if (NameArray::TryFindNameArrayByData(ModuleName))
        return true;

    /* 2. Code references (upstream heuristic), validated by the "None"-entry check in InitializeNamePool. */
    if (NameArray::TryFindNamePool())
    {
        uint8* GNamesAddress = reinterpret_cast<uint8*>(GetModuleBase(ModuleName) + Off::InSDK::NameArray::GNames);
        const int32 RawOffset = Off::InSDK::NameArray::GNames;

        // Apply per-game FNamePool pointer decryption (Valorant-style indirection, UE 4.23+).
        uint8* DecryptedAddr = reinterpret_cast<uint8*>(DecryptNamePool(reinterpret_cast<uintptr_t>(GNamesAddress)));
        if (DecryptedAddr != GNamesAddress)
            LogInfo("DecryptNamePool: %p -> %p", (void*)GNamesAddress, (void*)DecryptedAddr);

        if (NameArray::InitializeNamePool(DecryptedAddr))
        {
            if (DecryptedAddr != GNamesAddress)
                Off::InSDK::NameArray::GNames = RawOffset;

            return true;
        }
    }

    Off::InSDK::NameArray::GNames = 0x0;
    LogError("Could not find GNames! If the game encrypts names differently, install a decryption hook in Generator::InitEngineCore().");
    return false;
}

bool NameArray::TryInit(int32 OffsetOverride, bool bIsNamePool, const char* const ModuleName)
{
    const uintptr_t ImageBase = GetModuleBase(ModuleName);

    if (!ImageBase || OffsetOverride <= 0)
    {
        LogError("GNames override: module not loaded or invalid offset 0x%X", OffsetOverride);
        return false;
    }

    uint8* GNamesAddress = reinterpret_cast<uint8*>(ImageBase + OffsetOverride);

    auto TryAsNamePool = [&]() -> bool
    {
        // Apply per-game FNamePool pointer decryption (Valorant-style indirection, UE 4.23+).
        uint8* DecryptedAddr = reinterpret_cast<uint8*>(DecryptNamePool(reinterpret_cast<uintptr_t>(GNamesAddress)));
        if (DecryptedAddr != GNamesAddress)
            LogInfo("DecryptNamePool: %p -> %p", (void*)GNamesAddress, (void*)DecryptedAddr);

        if (!NameArray::InitializeNamePool(DecryptedAddr))
            return false;

        if (DecryptedAddr != GNamesAddress)
            Off::InSDK::NameArray::GNames = OffsetOverride;

        return true;
    };

    auto TryAsNameArray = [&]() -> bool
    {
        // Apply per-game TNameEntryArray pointer decryption (PUBG-style indirection, UE <= 4.22).
        // Hook returns TNameEntryArray** (address of global pointer); runtime derefs once to get the live array pointer.
        const uintptr_t PtrToPtr = DecryptNameArray(reinterpret_cast<uintptr_t>(GNamesAddress));
        if (!PtrToPtr || IsBadReadRange(reinterpret_cast<void*>(PtrToPtr), sizeof(void*)))
            return false;

        uint8* DecryptedAddr = *reinterpret_cast<uint8**>(PtrToPtr);

        if (!NameArray::InitializeNameArray(DecryptedAddr))
            return false;

        GNames = DecryptedAddr;
        Off::InSDK::NameArray::GNames = OffsetOverride;
        Settings::Internal::bUseNamePool = false;

        if (!FNameEntry::Init())
        {
            GNames = nullptr;
            return false;
        }

        return true;
    };

    LogInfo("Overwrote offset: '%s GNames' set as offset 0x%X", bIsNamePool ? "FNamePool" : "TNameEntryArray", OffsetOverride);

    if (bIsNamePool ? TryAsNamePool() : TryAsNameArray())
        return true;

    /* The override might just have the wrong type (e.g. an FNamePool passed with bIsNamePool = false). */
    LogError("GNames override 0x%X is not a valid %s, trying %s...", OffsetOverride, bIsNamePool ? "FNamePool" : "TNameEntryArray", bIsNamePool ? "TNameEntryArray" : "FNamePool");

    if (bIsNamePool ? TryAsNameArray() : TryAsNamePool())
        return true;

    GNames = nullptr;
    LogError("The GNames override couldn't be used (outdated offset or GNames-encryption)");
    return false;
}

bool NameArray::SetGNamesWithoutCommiting()
{
    /* GNames is already set */
    if (Off::InSDK::NameArray::GNames != 0x0)
        return false;

    if (NameArray::TryFindNamePool())
    {
        LogSuccess("Found 'FNamePool GNames' at offset 0x%X", Off::InSDK::NameArray::GNames);
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
     * Take the maximum CompIdx seen across all objects (filtering implausibly large values that can only be garbage),
     * then find the unique bits where `MaxCompIdx >> bits == CurrentBlock`. */
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

/* For UE 4.23+ FNamePool: walk the Blocks[] array and return the index of the last valid (non-null) block pointer.
 * Layout-independent: doesn't depend on Off::NameArray::MaxChunkIndex pointing at a real CurrentBlock field.
 *
 * For UE <= 4.22 TNameEntryArray: read the NumChunks field directly via the offset discovered in InitializeNameArray. */
int32 NameArray::GetNumChunks()
{
    if (!GNames)
        return -1;

    if (!Settings::Internal::bUseNamePool)
        return *reinterpret_cast<int32*>(GNames + Off::NameArray::MaxChunkIndex);

    uint8_t** Blocks = reinterpret_cast<uint8_t**>(GNames + Off::NameArray::ChunksStart);
    if (IsBadReadPtr(Blocks))
        return -1;

    int32 LastValid = -1;
    for (int32 i = 0; i < FNameMaxBlocks; ++i)
    {
        if (!bAllBlockSlotsReadable && IsBadReadPtr(&Blocks[i]))
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
    if (!GNames)
        return 0;

    return !Settings::Internal::bUseNamePool ? *reinterpret_cast<int32*>(GNames + Off::NameArray::NumElements) : 0;
}

/* Walk entries within the latest block and return the byte cursor of the next write position. */
int32 NameArray::GetByteCursor()
{
    if (!GNames || !Settings::Internal::bUseNamePool)
        return 0;

    const int32 CurrentBlock = GetNumChunks();
    if (CurrentBlock < 0)
        return 0;

    uint8_t** Blocks = reinterpret_cast<uint8_t**>(GNames + Off::NameArray::ChunksStart);

    FNamePoolEntryLayout Layout;
    Layout.HeaderOffset = Off::FNameEntry::NamePool::HeaderOffset;
    Layout.StringOffset = Off::FNameEntry::NamePool::StringOffset;
    Layout.Stride = NameEntryStride > 0 ? static_cast<int32>(NameEntryStride) : 2;
    Layout.LenShift = FNameEntry::FNameEntryLengthShiftCount;

    const int32 Cursor = WalkBlockCursor(Blocks[CurrentBlock], Layout, Layout.Stride << FNameBlockOffsetBits);
    return Cursor > 0 ? Cursor : 0;
}

FNameEntry NameArray::GetNameEntry(const void* Name)
{
    if (!ByIndex || !Name)
        return FNameEntry(nullptr);

    return ByIndex(GNames, FName(Name).GetCompIdx(), FNameBlockOffsetBits);
}

FNameEntry NameArray::GetNameEntry(int32 Idx)
{
    if (!ByIndex)
        return FNameEntry(nullptr);

    return ByIndex(GNames, Idx, FNameBlockOffsetBits);
}
