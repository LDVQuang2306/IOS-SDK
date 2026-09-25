#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <functional>
#include <tuple>
#include <cmath>
#include <type_traits>

#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/getsect.h>
#include <dlfcn.h>
#include "Settings.h"

inline std::string str_tolower(std::string S)
{
    std::transform(S.begin(), S.end(), S.begin(), [](unsigned char C) { return std::tolower(C); });
    return S;
}

template<typename CharType>
inline int32_t StrlenHelper(const CharType* Str)
{
    if constexpr (std::is_same<CharType, char>())
    {
        return strlen(Str);
    }
    else if constexpr (std::is_same<CharType, char16_t>())
    {
        return std::char_traits<char16_t>::length(Str);
    }
    else
    {
        return wcslen(Str);
    }
}

template<typename CharType>
inline bool StrnCmpHelper(const CharType* Left, const CharType* Right, size_t NumCharsToCompare)
{
    if constexpr (std::is_same<CharType, char>())
    {
        return strncmp(Left, Right, NumCharsToCompare) == 0;
    }
    else if constexpr (std::is_same<CharType, char16_t>())
    {
        return std::char_traits<char16_t>::compare(Left, Right, NumCharsToCompare) == 0;
    }
    else
    {
        return wcsncmp(Left, Right, NumCharsToCompare) == 0;
    }
}

namespace ASMUtils
{
    // Check if the instruction is a B/BL/B.cond (i.e., relative branch)
    inline bool IsBranchInstruction(uint32_t instruction)
    {
        // Check top 6 bits for 0b000101 (B) or 0b100101 (BL)
        return (instruction & 0xFC000000) == 0x14000000 || (instruction & 0xFC000000) == 0x94000000;
    }

    // Resolves a 26-bit immediate branch (B/BL) to its absolute address
    inline uintptr_t ResolveBranchTarget(uintptr_t Address)
    {
        uint32_t instr = *reinterpret_cast<uint32_t*>(Address);

        // Instruction format: B/BL <label>
        // Offset is bits[25:0] << 2 (sign-extended)
        int32_t imm26 = (instr & 0x03FFFFFF);
        int64_t offset = (int64_t)(imm26 << 6) >> 4; // sign extend to 64 bits

        return Address + offset;
    }

    // Check for ADRP (used for PC-relative loads to registers)
    inline bool IsADRP(uint32_t instruction)
    {
        return (instruction & 0x9F000000) == 0x90000000;
    }

    inline uintptr_t ResolveADRP_LDR(uintptr_t address)
    {
        uint32_t* instrs = reinterpret_cast<uint32_t*>(address);
        uint32_t adrp = instrs[0];
        uint32_t ldr  = instrs[1];
        
        // 1. Resolve ADRP (Page Base)
        // Check if actually ADRP (0x90...)
        if ((adrp & 0x9F000000) != 0x90000000) return 0;
        
        uint64_t pc_page = address & ~0xFFFULL;
        uint64_t immhi = (adrp >> 5) & 0x7FFFF;
        uint64_t immlo = (adrp >> 29) & 0x3;
        int64_t adrpImm = ((int64_t)((immhi << 2) | immlo) << 43) >> 31;
        uintptr_t adrpBase = pc_page + adrpImm;
        
        // 2. Resolve LDR Immediate Offset
        // LDR (Immediate) - 64-bit: 0xF9400000 | 32-bit: 0xB9400000
        // We assume 64-bit LDR (0xF94) for pointers
        if ((ldr & 0xFFC00000) != 0xF9400000) return 0;
        
        // Extract 12-bit scaled immediate (bits 21-10)
        uint32_t imm12 = (ldr >> 10) & 0xFFF;
        // Scale by 8 for 64-bit LDR
        uint32_t offset = imm12 << 3;
        
        // 3. Combine
        // Note: This points to the *location* of the pointer in memory (e.g., GOT entry or VTable)
        return adrpBase + offset;
    }

    inline uintptr_t ResolveADRP(uintptr_t Address)
    {
        uint32_t instr = *reinterpret_cast<uint32_t*>(Address);
        uint64_t pc_page = Address & ~0xFFFULL;

        // Extract immhi and immlo
        uint64_t immhi = (instr >> 5) & 0x7FFFF;
        uint64_t immlo = (instr >> 29) & 0x3;

        // Sign-extend 21-bit immediate
        int64_t imm = ((int64_t)((immhi << 2) | immlo) << 43) >> 31;

        return pc_page + imm;
    }

    // Check for LDR literal (PC-relative loads)
    inline bool IsLDRLiteral(uint32_t instruction)
    {
        // LDR (literal) has opcode 0b0001xx (depending on size)
        return (instruction & 0x3B000000) == 0x18000000;
    }

    // Resolves the target of a PC-relative LDR instruction
    inline uintptr_t ResolveLDRLiteral(uintptr_t Address)
    {
        uint32_t instr = *reinterpret_cast<uint32_t*>(Address);

        // 19-bit signed offset, shifted by scale (size)
        int32_t imm19 = (instr >> 5) & 0x7FFFF;
        int32_t offset = (imm19 << 13) >> 11; // sign-extend

        return Address + offset;
    }

    inline bool IsADRL(uint32_t* address)
    {
        uint32_t adrp = address[0];
        uint32_t add  = address[1];

        bool isAdrp = (adrp & 0x9F000000) == 0x90000000; // ADRP opcode
        bool isAdd  = (add  & 0xFFC00000) == 0x91000000; // ADD (immediate)

        uint32_t adrpReg = adrp & 0x1F;          // destination register of ADRP
        uint32_t addBase = (add >> 5) & 0x1F;    // base register of ADD
        uint32_t addDest = add & 0x1F;           // destination register of ADD

        return isAdrp && isAdd && (adrpReg == addBase) && (addDest == adrpReg);
    }

    inline bool IsADRL(uintptr_t address)
    {
        return IsADRL(reinterpret_cast<uint32_t*>(address));
    }

    inline uintptr_t ResolveADRL(uintptr_t address)
    {
        uint32_t* instrs = reinterpret_cast<uint32_t*>(address);

        uint32_t adrp = instrs[0];
        uint32_t add  = instrs[1];

        // Resolve ADRP
        uint64_t pc_page = address & ~0xFFFULL;
        uint64_t immhi = (adrp >> 5) & 0x7FFFF;
        uint64_t immlo = (adrp >> 29) & 0x3;
        int64_t adrpImm = ((int64_t)((immhi << 2) | immlo) << 43) >> 31;

        uintptr_t adrpResult = pc_page + adrpImm;

        // Resolve ADD immediate
        uint32_t imm12 = (add >> 10) & 0xFFF;
        uint32_t shift = (add >> 22) & 0x1; // If set, shift imm12 by 12

        uintptr_t addResult = adrpResult + (imm12 << (shift ? 12 : 0));

        return addResult;
    }
}

struct MachImageInfo {
    uintptr_t Base;
    size_t Size;
    const struct mach_header_64* Header;
};

/* A loaded segment of a Mach-O image, already relocated by the ASLR slide. [Begin, End) */
struct MachSegment {
    char Name[17];
    uintptr_t Begin;
    uintptr_t End;
    vm_prot_t InitProt;
};

inline const char* GetImageFileName(const char* Path)
{
    if (!Path)
        return "";

    const char* LastSlash = strrchr(Path, '/');
    return LastSlash ? LastSlash + 1 : Path;
}

/* Index of the image in dyld's list, or -1. nullptr selects the main executable (MH_EXECUTE). */
inline int32_t FindImageIndex(const char* ImageName = nullptr)
{
    const uint32_t Count = _dyld_image_count();

    if (!ImageName)
    {
        for (uint32_t i = 0; i < Count; i++)
        {
            const auto* Header = reinterpret_cast<const struct mach_header_64*>(_dyld_get_image_header(i));
            if (Header && Header->magic == MH_MAGIC_64 && Header->filetype == MH_EXECUTE)
                return static_cast<int32_t>(i);
        }
        return Count > 0 ? 0 : -1;
    }

    /* Exact file-name match first. A substring match on the full path would also match every framework inside "<Name>.app/". */
    for (uint32_t i = 0; i < Count; i++)
    {
        if (strcmp(GetImageFileName(_dyld_get_image_name(i)), ImageName) == 0)
            return static_cast<int32_t>(i);
    }

    for (uint32_t i = 0; i < Count; i++)
    {
        if (strstr(GetImageFileName(_dyld_get_image_name(i)), ImageName))
            return static_cast<int32_t>(i);
    }

    return -1;
}

inline std::vector<MachSegment> GetImageSegments(const struct mach_header_64* Header, intptr_t Slide)
{
    std::vector<MachSegment> Segments;

    if (!Header || Header->magic != MH_MAGIC_64)
        return Segments;

    auto* Cmd = reinterpret_cast<const struct load_command*>(Header + 1);

    for (uint32_t c = 0; c < Header->ncmds; c++)
    {
        if (Cmd->cmdsize < sizeof(struct load_command))
            break;

        if (Cmd->cmd == LC_SEGMENT_64)
        {
            auto* Seg = reinterpret_cast<const struct segment_command_64*>(Cmd);

            /* __PAGEZERO (and anything else without protection) is not mapped. Including it made the "image size" > 4GB. */
            if (Seg->vmsize != 0 && Seg->initprot != VM_PROT_NONE)
            {
                MachSegment Segment{};
                memcpy(Segment.Name, Seg->segname, 16);
                Segment.Begin = static_cast<uintptr_t>(Seg->vmaddr + Slide);
                Segment.End = Segment.Begin + static_cast<uintptr_t>(Seg->vmsize);
                Segment.InitProt = Seg->initprot;
                Segments.push_back(Segment);
            }
        }

        Cmd = reinterpret_cast<const struct load_command*>(reinterpret_cast<uintptr_t>(Cmd) + Cmd->cmdsize);
    }

    return Segments;
}

inline std::vector<MachSegment> GetImageSegments(const char* ImageName = nullptr)
{
    const int32_t Index = FindImageIndex(ImageName);

    if (Index < 0)
        return {};

    return GetImageSegments(reinterpret_cast<const struct mach_header_64*>(_dyld_get_image_header(Index)), _dyld_get_image_vmaddr_slide(Index));
}

inline MachImageInfo GetImageBaseAndSize(const char* ImageName = nullptr)
{
    const int32_t Index = FindImageIndex(ImageName);

    if (Index < 0)
        return { 0, 0, nullptr };

    const auto* Header = reinterpret_cast<const struct mach_header_64*>(_dyld_get_image_header(Index));
    const uintptr_t Base = reinterpret_cast<uintptr_t>(Header);

    uintptr_t MaxAddr = Base;
    for (const MachSegment& Segment : GetImageSegments(Header, _dyld_get_image_vmaddr_slide(Index)))
    {
        if (Segment.End > MaxAddr)
            MaxAddr = Segment.End;
    }

    return { Base, static_cast<size_t>(MaxAddr - Base), Header };
}

inline uintptr_t GetModuleBase(const char* SearchModuleName = nullptr)
{
    const int32_t Index = FindImageIndex(SearchModuleName);

    return Index < 0 ? 0x0 : reinterpret_cast<uintptr_t>(_dyld_get_image_header(Index));
}

inline std::pair<uintptr_t, size_t> GetSegmentByName(const struct mach_header_64* Header, const char* SegmentName)
{
    if (!Header || !SegmentName) return { 0, 0 };

    uintptr_t CommandPtr = (uintptr_t)(Header + 1);

    for (uint32_t i = 0; i < Header->ncmds; ++i)
    {
        const struct load_command* LC = (const struct load_command*)CommandPtr;

        if (LC->cmd == LC_SEGMENT_64)
        {
            const struct segment_command_64* Seg = (const struct segment_command_64*)LC;

            // Check segname (Segment Name) instead of sectname
            if (strncmp(Seg->segname, SegmentName, 16) == 0)
            {
                intptr_t Slide = 0;
                // Calculate ASLR Slide
                uint32_t Count = _dyld_image_count();
                for (uint32_t j = 0; j < Count; j++) {
                    if ((const struct mach_header_64*)_dyld_get_image_header(j) == Header) {
                        Slide = _dyld_get_image_vmaddr_slide(j);
                        break;
                    }
                }
                return { Seg->vmaddr + Slide, Seg->vmsize };
            }
        }
        CommandPtr += LC->cmdsize;
    }
    return { 0, 0 };
}

inline uintptr_t GetOffset(const uintptr_t Address)
{
    static uintptr_t ImageBase = 0x0;

    if (ImageBase == 0x0)
        ImageBase = GetModuleBase();

    return Address > ImageBase ? (Address - ImageBase) : 0x0;
}

inline uintptr_t GetOffset(const void* Address)
{
    return GetOffset(reinterpret_cast<const uintptr_t>(Address));
}

inline bool IsInAnyModules(const uintptr_t Address) {
    // Basic check to see if address is inside the header of any loaded image
    uint32_t Count = _dyld_image_count();
    for (uint32_t i = 0; i < Count; i++) {
        if ((uintptr_t)_dyld_get_image_header(i) == Address) return true;
    }
    return false;
}

/* Reads memory through the kernel, so an unmapped/protected address returns false instead of crashing the game. */
inline bool ReadMemory(uintptr_t Address, void* Out, size_t Size)
{
    if (Size == 0)
        return true;

    if (!Out || Address < 0x1000 || Address > (UINTPTR_MAX - Size))
        return false;

    uint8_t* Destination = static_cast<uint8_t*>(Out);

    for (size_t Done = 0; Done < Size;)
    {
        const size_t Amount = std::min<size_t>(Size - Done, 0x10000);

        vm_size_t Read = 0;
        const kern_return_t KR = vm_read_overwrite(mach_task_self(), static_cast<vm_address_t>(Address + Done), static_cast<vm_size_t>(Amount), reinterpret_cast<vm_address_t>(Destination + Done), &Read);

        if (KR != KERN_SUCCESS || Read != Amount)
            return false;

        Done += Amount;
    }

    return true;
}

template <typename T>
inline T SafeRead(uintptr_t Address, T Default = {})
{
    T Buffer = Default;

    if (!ReadMemory(Address, &Buffer, sizeof(T)))
        return Default;

    return Buffer;
}

template <typename T>
inline T SafeRead(const void* Address, T Default = {})
{
    return SafeRead<T>(reinterpret_cast<uintptr_t>(Address), Default);
}

inline bool IsBadReadPtr(const void* Ptr)
{
    /* Any failure (including KERN_INVALID_ARGUMENT for tiny/odd addresses) means we must not dereference it. */
    uint8_t Data = 0;
    return !ReadMemory(reinterpret_cast<uintptr_t>(Ptr), &Data, 1);
};

inline bool IsBadReadPtr(const uintptr_t Ptr)
{
    return IsBadReadPtr(reinterpret_cast<const void*>(Ptr));
}

inline bool IsValidVirtualAddress(const uintptr_t Address)
{
    return !IsBadReadPtr(Address);
}

/* Userspace pointer that is 8-byte aligned (arm64 iOS). Cheap pre-check before any read. */
inline bool IsPlausiblePointer(uintptr_t Ptr)
{
    return Ptr >= 0x1000 && Ptr < 0x0000800000000000ULL && (Ptr & 0x7) == 0;
}

inline bool IsPlausiblePointer(const void* Ptr)
{
    return IsPlausiblePointer(reinterpret_cast<uintptr_t>(Ptr));
}

inline bool IsInProcessRange(const uintptr_t Address)
{
    /* Main image range doesn't change after launch, cache it (this is called in hot loops). */
    static const MachImageInfo MainImage = GetImageBaseAndSize();

    if (Address >= MainImage.Base && Address < (MainImage.Base + MainImage.Size))
        return true;

    return IsInAnyModules(Address);
}

inline bool IsInProcessRange(const void* Address)
{
    return IsInProcessRange(reinterpret_cast<const uintptr_t>(Address));
}

inline void* GetModuleAddress(const char* SearchModuleName)
{
    void* Entry = (void*)GetModuleBase(SearchModuleName);

    if (Entry)
        return Entry; // _dyld_get_image_header

    return nullptr;
}

inline void* FindPatternInRange(const void* Pattern, size_t PatternLen, const uint8_t* Start, uintptr_t Range)
{
    const uint8_t* PatBytes = static_cast<const uint8_t*>(Pattern);
    const uint8_t* End = Start + Range;
    const uint8_t* Curr = Start;

    while (Curr <= (End - PatternLen))
    {
        vm_size_t VmSize = 0;
        vm_address_t VmAddr = (vm_address_t)Curr;
        vm_region_basic_info_data_64_t Info;
        mach_msg_type_number_t Count = VM_REGION_BASIC_INFO_COUNT_64;
        memory_object_name_t Obj;
        
        // Safety Check: Ask Kernel if this address is readable
        kern_return_t Kr = vm_region_64(mach_task_self(), &VmAddr, &VmSize, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&Info, &Count, &Obj);
        
        if (Kr != KERN_SUCCESS || !(Info.protection & VM_PROT_READ)) {
            // Bad memory: Skip this whole block
            Curr = (const uint8_t*)(VmAddr + VmSize);
            continue;
        }

        // Calculate the safe end for this specific valid block
        uintptr_t BlockEndPtr = (uintptr_t)VmAddr + VmSize;
        if (BlockEndPtr > (uintptr_t)End) BlockEndPtr = (uintptr_t)End;
        const uint8_t* BlockEnd = (const uint8_t*)BlockEndPtr;

        // Fast Loop: Scan inside the valid block
        for (; Curr <= (BlockEnd - PatternLen); ++Curr)
        {
            if (memcmp(Curr, PatBytes, PatternLen) == 0)
            {
                return const_cast<void*>(static_cast<const void*>(Curr));
            }
        }
    }
    return nullptr;
}

inline void* FindPatternInRange(const std::vector<int>& Signature, const uint8_t* Start, uintptr_t Range, bool bRelative = false, uint32_t Offset = 0, int SkipCount = 0)
{
    const auto PatternLength = Signature.size();
    const auto PatternBytes = Signature.data();
    const uint8_t* End = Start + Range;
    const uint8_t* Curr = Start;

    // Outer Loop: Jump between valid memory regions
    while (Curr <= (End - PatternLength))
    {
        vm_size_t VmSize = 0;
        vm_address_t VmAddr = (vm_address_t)Curr;
        vm_region_basic_info_data_64_t Info;
        mach_msg_type_number_t Count = VM_REGION_BASIC_INFO_COUNT_64;
        memory_object_name_t Obj;
        
        // Safety Check
        kern_return_t Kr = vm_region_64(mach_task_self(), &VmAddr, &VmSize, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&Info, &Count, &Obj);
        
        if (Kr != KERN_SUCCESS || !(Info.protection & VM_PROT_READ)) {
            Curr = (const uint8_t*)(VmAddr + VmSize);
            continue;
        }

        uintptr_t BlockEndPtr = (uintptr_t)VmAddr + VmSize;
        if (BlockEndPtr > (uintptr_t)End) BlockEndPtr = (uintptr_t)End;
        const uint8_t* BlockEnd = (const uint8_t*)BlockEndPtr;

        // Fast Loop
        for (; Curr <= (BlockEnd - PatternLength); ++Curr)
        {
            bool bFound = true;
            for (size_t j = 0; j < PatternLength; ++j)
            {
                if (Curr[j] != PatternBytes[j] && PatternBytes[j] != -1)
                {
                    bFound = false;
                    break;
                }
            }

            if (bFound)
            {
                if (SkipCount > 0) {
                    SkipCount--;
                    continue;
                }

                uintptr_t Address = reinterpret_cast<uintptr_t>(Curr);
                if (bRelative) Address = Address + Offset;
                return reinterpret_cast<void*>(Address);
            }
        }
    }
    return nullptr;
}

inline void* FindPatternInRange(const char* Signature, const uint8_t* Start, uintptr_t Range, bool bRelative = false, uint32_t Offset = 0)
{
    static auto patternToByte = [](const char* pattern) -> std::vector<int>
    {
        auto Bytes = std::vector<int>{};
        const char* Current = pattern;

        while (*Current)
        {
            if (*Current == '?') {
                ++Current;
                if (*Current == '?') ++Current;
                Bytes.push_back(-1);
            } else if (isxdigit(*Current)) {
                Bytes.push_back(strtoul(Current, const_cast<char**>(&Current), 16));
            } else {
                ++Current;
            }
        }
        return Bytes;
    };
    
    std::vector<int> Bytes = patternToByte(Signature);
    return FindPatternInRange(Bytes, Start, Range, bRelative, Offset);
}

inline void* FindPattern(const char* Signature, const char* SegmentName = "__TEXT", uint32_t Offset = 0, uintptr_t StartAddress = 0x0)
{
    const auto [ImageBase, ImageSize, Header] = GetImageBaseAndSize();
    
    // Default to ImageBase (Scan All)
    uintptr_t SearchStart = ImageBase;
    uintptr_t SearchRange = ImageSize;


    if (SegmentName != nullptr)
    {
        const auto [SegStart, SegSize] = GetSegmentByName(Header, SegmentName);
        if (SegStart != 0 && SegSize != 0) {
            SearchStart = SegStart;
            SearchRange = SegSize;
        } else {
            return nullptr;
        }
    }

    const uintptr_t SearchEnd = SearchStart + SearchRange;

    // Handle optional StartAddress override
    if (StartAddress != 0x0)
    {
        if (StartAddress < SearchStart || StartAddress >= SearchEnd) return nullptr;
        SearchStart = StartAddress + 1;
        if (SearchStart >= SearchEnd) return nullptr;
        SearchRange = SearchEnd - SearchStart;
    }

    return FindPatternInRange(Signature, reinterpret_cast<uint8_t*>(SearchStart), SearchRange, Offset != 0, Offset);
}

template<typename T>
inline T* FindAlignedValueInProcessInRange(T Value, int32_t Alignment, uintptr_t StartAddress, uintptr_t Range)
{
    if (Alignment <= 0 || Range < sizeof(T))
        return nullptr;

    /* Copy the range in chunks through the kernel. Segments can contain unmapped holes (e.g. lazily bound pages). */
    constexpr uintptr_t ChunkSize = 0x10000;
    std::vector<uint8_t> Buffer(ChunkSize + sizeof(T));

    for (uintptr_t ChunkStart = 0x0; ChunkStart + sizeof(T) <= Range; ChunkStart += ChunkSize)
    {
        const uintptr_t ToRead = std::min<uintptr_t>(ChunkSize + sizeof(T), Range - ChunkStart);

        if (!ReadMemory(StartAddress + ChunkStart, Buffer.data(), ToRead))
            continue;

        for (uintptr_t i = 0x0; i + sizeof(T) <= ToRead && i < ChunkSize; i += Alignment)
        {
            T Current;
            memcpy(&Current, Buffer.data() + i, sizeof(T));

            if (Current == Value)
                return reinterpret_cast<T*>(StartAddress + ChunkStart + i);
        }
    }

    return nullptr;
}

template<typename T>
inline T* FindAlignedValueInProcess(T Value, const std::string& Sectionname = "__DATA", int32_t Alignment = alignof(T), bool bSearchAllSections = false)
{
    const std::vector<MachSegment> Segments = GetImageSegments();

    /* Named segment first */
    if (!bSearchAllSections)
    {
        for (const MachSegment& Segment : Segments)
        {
            if (strncmp(Segment.Name, Sectionname.c_str(), 16) != 0)
                continue;

            if (T* Result = FindAlignedValueInProcessInRange(Value, Alignment, Segment.Begin, Segment.End - Segment.Begin))
                return Result;
        }
    }

    /* Then every other readable, non-executable segment of the image (never __PAGEZERO or unmapped memory) */
    for (const MachSegment& Segment : Segments)
    {
        if (!(Segment.InitProt & VM_PROT_READ) || (Segment.InitProt & VM_PROT_EXECUTE) || strncmp(Segment.Name, "__LINKEDIT", 16) == 0)
            continue;

        if (!bSearchAllSections && strncmp(Segment.Name, Sectionname.c_str(), 16) == 0)
            continue;

        if (T* Result = FindAlignedValueInProcessInRange(Value, Alignment, Segment.Begin, Segment.End - Segment.Begin))
            return Result;
    }

    return nullptr;
}

enum class InstType {
    ADRL,       // ADRP + ADD
    ADRP_LDR,   // ADRP + LDR
    ADRP_STR    // ADRP + STR (Functionally similar to LDR for address calculation)
};

template<typename PointerType>
inline __attribute__((always_inline))
void InitializePointer(PointerType*& Pointer, const char* Pattern, InstType Inst, int Step = 0)
{
    // 'Step' is passed as the Offset to FindPattern, so 'Found' points exactly
    // to the start of the instruction sequence (ADRP)
    uintptr_t Found = (uintptr_t)FindPattern(Pattern, "__TEXT", Step);

    if (!Found) {
        Pointer = nullptr;
        return;
    }

    // 2. Resolve based on Instruction Type
    switch (Inst)
    {
        case InstType::ADRL:
            // Uses your existing ASMUtils::ResolveADRL
            Pointer = reinterpret_cast<PointerType*>(ASMUtils::ResolveADRL(Found));
            break;

        case InstType::ADRP_LDR:
        case InstType::ADRP_STR:
            // Uses the new helper provided above.
            // Note: For LDR, this calculates the address being accessed.
            // If you need the *value* at that address, you must dereference it later.
            Pointer = reinterpret_cast<PointerType*>(ASMUtils::ResolveADRP_LDR(Found));
            break;

        default:
            Pointer = nullptr;
            break;
    }
}

template<bool bShouldResolve32BitJumps = true>
inline std::pair<const void*, int32_t> IterateVTableFunctions(void** VTable, const std::function<bool(const uint8_t* Addr, int32_t Index)>& CallBackForEachFunc, int32_t NumFunctions = 0x1000, int32_t OffsetFromStart = 0x0)
{
    [[maybe_unused]] auto Resolve32BitRelativeJump = [](const void* FunctionPtr) -> const uint8_t*
    {
        return reinterpret_cast<const uint8_t*>(FunctionPtr);
    };

    if (!CallBackForEachFunc)
        return { nullptr, -1 };

    for (int i = 0; i < NumFunctions; i++)
    {
        const uintptr_t CurrentFuncAddress = reinterpret_cast<uintptr_t>(VTable[i]);
        if (CurrentFuncAddress == NULL || !IsInProcessRange(CurrentFuncAddress))
            break;

        const uint8_t* ResolvedAddress = Resolve32BitRelativeJump(reinterpret_cast<const uint8_t*>(CurrentFuncAddress));

        if (CallBackForEachFunc(ResolvedAddress, i))
            return { ResolvedAddress, i };
    }
    return { nullptr, -1 };
}

struct MemAddress
{
public:
    uintptr_t Address;

private:
    static bool IsFunctionRet(const uint8_t* Address)
    {
        if (!Address) return false;
        uint32_t instr = *reinterpret_cast<const uint32_t*>(Address);
        return (instr == 0xD65F03C0 || instr == 0xD65F03E0 || instr == 0xD65F03E1);
    }

public:
    inline MemAddress(std::nullptr_t) : Address(NULL) {}
    inline MemAddress(void* Addr) : Address(reinterpret_cast<uintptr_t>(Addr)) {}
    inline MemAddress(uintptr_t Addr) : Address(Addr) {}

    explicit operator bool() { return Address != NULL; }
    template<typename T> explicit operator T*() { return reinterpret_cast<T*>(Address); }
    operator uintptr_t() { return Address; }
    inline bool operator==(MemAddress Other) const { return Address == Other.Address; }
    inline MemAddress operator+(int Value) const { return Address + Value; }
    inline MemAddress operator-(int Value) const { return Address - Value; }
    template<typename T = void> inline T* Get() { return reinterpret_cast<T*>(Address); }
    template<typename T = void> inline const T* Get() const { return reinterpret_cast<const T*>(Address); }

    inline MemAddress FindFunctionEnd(uint32_t Range = 0xFFFF) const
    {
        if (!Address) return nullptr;
        if (Range > 0xFFFF) Range = 0xFFFF;
        for (int i = 0; i < Range; i += 4)
        {
            if (IsFunctionRet(Get<uint8_t>() + i))
                return Address + i;
        }
        return nullptr;
    }

    inline MemAddress RelativePattern(const char* Pattern, int32_t Range, int32_t Relative = 0) const
    {
        if (!Address) return nullptr;
        return FindPatternInRange(Pattern, Get<uint8_t>(), Range, Relative != 0, Relative);
    }

    inline MemAddress GetRelativeCalledFunction(int32_t OneBasedFuncIndex, bool(*IsWantedTarget)(MemAddress CalledAddr) = nullptr) const
    {
        if (!Address || OneBasedFuncIndex == 0) return nullptr;
        const int32_t Multiply = OneBasedFuncIndex > 0 ? 1 : -1;
        auto GetIndex = [=](int32_t Index) -> int32_t { return Index * Multiply; };

        int32_t NumCalls = 0;
        for (int i = 0; i < 0xFFF; i += 4)
        {
            const int32_t ByteOffset = GetIndex(i);
            const uint32_t Instr = *reinterpret_cast<uint32_t*>(Address + ByteOffset);

            uintptr_t CurrentPC = Address + ByteOffset;
            MemAddress RelativeCallTarget = ASMUtils::ResolveBranchTarget(CurrentPC);
            if (!IsInProcessRange(RelativeCallTarget)) continue;

            if (++NumCalls == abs(OneBasedFuncIndex))
            {
                if (IsWantedTarget && !IsWantedTarget(RelativeCallTarget))
                {
                    --NumCalls;
                    continue;
                }
                return RelativeCallTarget;
            }
        }
        return nullptr;
    }

    inline MemAddress FindNextFunctionStart() const
    {
        if (!Address) return MemAddress(nullptr);
        uintptr_t FuncEnd = (uintptr_t)FindFunctionEnd();
        if (!FuncEnd) return nullptr;
        FuncEnd += 4;
        return FuncEnd % 4 != 0 ? FuncEnd + (4 - (FuncEnd % 4)) : FuncEnd;
    }
};

template<typename Type = const char*>
inline MemAddress FindByString(Type RefStr)
{
    const auto [ImageBase, ImageSize, Header] = GetImageBaseAndSize();
    const auto [TextSection, TextSize] = GetSegmentByName(Header, "__TEXT");
    
    if (!TextSection) return nullptr;

    uintptr_t StringAddress = NULL;
    const auto RetfStrLength = StrlenHelper(RefStr);
    
    // Calculate total byte length based on character type (char vs char16_t)
    using CharT = std::remove_pointer_t<Type>;
    const size_t StrByteLen = RetfStrLength * sizeof(CharT);

    // Call the new raw byte overload
    uint8_t* FoundPtr = (uint8_t*)FindPatternInRange(RefStr, StrByteLen, (uint8_t*)ImageBase, ImageSize);
    
    if (FoundPtr) StringAddress = (uintptr_t)FoundPtr;
    if (!StringAddress) return nullptr;

    for (int i = 0; i < TextSize; i += 4)
    {
        if (ASMUtils::IsADRL(TextSection + i))
        {
            const uintptr_t StrPtr = ASMUtils::ResolveADRL(TextSection + i);
            if (StrPtr == StringAddress)
                return { TextSection + i };
        }
    }
    return nullptr;
}

inline MemAddress FindByWString(const wchar_t* RefStr)
{
    return FindByString<const wchar_t*>(RefStr);
}

template<bool bCheckIfLeaIsStrPtr = false, typename CharType = char>
inline MemAddress FindByStringInAllSections(const CharType* RefStr, uintptr_t StartAddress = 0x0, int32_t Range = 0x0)
{
    static_assert(std::is_same_v<CharType, char> || std::is_same_v<CharType, wchar_t> || std::is_same_v<CharType, char16_t>, "Only char/wchar_t/char16_t supported");

    const auto [ImageBase, ImageSize, Header] = GetImageBaseAndSize();
    const uintptr_t ImageEnd = ImageBase + ImageSize;

    if (StartAddress != 0x0 && (StartAddress < ImageBase || StartAddress > ImageEnd))
        return nullptr;

    const size_t RefStrLen = static_cast<size_t>(StrlenHelper(RefStr));
    const size_t RefStrBytes = RefStrLen * sizeof(CharType);

    std::vector<CharType> Candidate(RefStrLen + 1);

    /* Only code can contain ADRP+ADD, so only executable segments are scanned (the old code walked the whole image incl. unmapped space). */
    for (const MachSegment& Segment : GetImageSegments())
    {
        if (!(Segment.InitProt & VM_PROT_EXECUTE))
            continue;

        /* Start searching a bit ahead if StartAddress is provided to avoid immediate self-find */
        uintptr_t SearchStart = Segment.Begin;
        if (StartAddress != 0x0)
        {
            if (StartAddress + 8 >= Segment.End)
                continue;

            SearchStart = std::max<uintptr_t>(SearchStart, (StartAddress + 8) & ~uintptr_t(3));
        }

        uintptr_t SearchEnd = Segment.End - 8;
        if (Range > 0 && SearchStart + static_cast<uintptr_t>(Range) < SearchEnd)
            SearchEnd = SearchStart + static_cast<uintptr_t>(Range);

        for (uintptr_t Address = SearchStart; Address < SearchEnd; Address += 4)
        {
            /* Check for ADRP+ADD (ADRL) sequence which loads a pointer relative to PC */
            if (!ASMUtils::IsADRL(Address))
                continue;

            const uintptr_t StrPtr = ASMUtils::ResolveADRL(Address);

            if (StrPtr < ImageBase || StrPtr + RefStrBytes > ImageEnd)
                continue;

            if (!ReadMemory(StrPtr, Candidate.data(), RefStrBytes))
                continue;

            /* Check if the string at the resolved address matches our target */
            if (StrnCmpHelper(RefStr, Candidate.data(), RefStrLen))
                return { Address };
        }
    }

    return nullptr;
}

template<typename Type = const char*>
inline MemAddress FindUnrealExecFunctionByString(Type RefStr, void* StartAddress = nullptr)
{
    using CharType = std::remove_const_t<std::remove_pointer_t<Type>>;

    const auto [ImageBase, ImageSize, Header] = GetImageBaseAndSize();
    const uintptr_t ImageEnd = ImageBase + ImageSize;

    const size_t RefStrLen = static_cast<size_t>(StrlenHelper(RefStr));
    std::vector<CharType> Candidate(RefStrLen + 1);

    /* FNameNativePtrPair { const char* Name; FNativeFuncPtr Func; } tables live in data segments. Read them in chunks through the kernel. */
    constexpr uintptr_t ChunkSize = 0x10000;
    std::vector<uint8_t> Buffer(ChunkSize + sizeof(void*));

    for (const MachSegment& Segment : GetImageSegments())
    {
        if (!(Segment.InitProt & VM_PROT_READ) || (Segment.InitProt & VM_PROT_EXECUTE) || strncmp(Segment.Name, "__LINKEDIT", 16) == 0)
            continue;

        uintptr_t Begin = Segment.Begin;
        if (StartAddress)
        {
            const uintptr_t Start = reinterpret_cast<uintptr_t>(StartAddress);
            if (Start >= Segment.End)
                continue;
            Begin = std::max(Begin, Start & ~uintptr_t(7));
        }

        for (uintptr_t Chunk = Begin; Chunk + 2 * sizeof(void*) <= Segment.End; Chunk += ChunkSize)
        {
            const uintptr_t ToRead = std::min<uintptr_t>(ChunkSize + sizeof(void*), Segment.End - Chunk);

            if (!ReadMemory(Chunk, Buffer.data(), ToRead))
                continue;

            for (uintptr_t i = 0; i + 2 * sizeof(void*) <= ToRead && i < ChunkSize; i += sizeof(void*))
            {
                uintptr_t PossibleStringAddress, PossibleExecFuncAddress;
                memcpy(&PossibleStringAddress, Buffer.data() + i, sizeof(void*));
                memcpy(&PossibleExecFuncAddress, Buffer.data() + i + sizeof(void*), sizeof(void*));

                if (PossibleStringAddress == PossibleExecFuncAddress)
                    continue;

                if (PossibleStringAddress < ImageBase || PossibleStringAddress >= ImageEnd || PossibleExecFuncAddress < ImageBase || PossibleExecFuncAddress >= ImageEnd)
                    continue;

                if (!ReadMemory(PossibleStringAddress, Candidate.data(), RefStrLen * sizeof(CharType)))
                    continue;

                if (StrnCmpHelper<CharType>(RefStr, Candidate.data(), RefStrLen))
                    return { PossibleExecFuncAddress };
            }
        }
    }

    return nullptr;
}

template<bool bCheckIfLeaIsStrPtr = false>
inline MemAddress FindByWStringInAllSections(const TCHAR* RefStr)
{
    return FindByStringInAllSections<bCheckIfLeaIsStrPtr, TCHAR>(RefStr);
}

namespace FileNameHelper
{
    inline void MakeValidFileName(std::string& InOutName)
    {
        for (char& c : InOutName)
        {
            if (c == '<' || c == '>' || c == ':' || c == '\"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
                c = '_';
        }
    }
}
