#pragma once

#include <iostream>
#include <vector>
#include <string>
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
#include "TmpUtils.h"

/* Credits: https://en.cppreference.com/w/cpp/string/byte/tolower */
inline std::string str_tolower(std::string S)
{
    std::transform(S.begin(), S.end(), S.begin(), [](unsigned char C) { return std::tolower(C); });
    return S;
}

// StrlenHelper / StrnCmpHelper: provided by TmpUtils.h (upstream-shared). iOS adds char16_t specialization below.
template<> inline int32_t StrlenHelper<char16_t>(const char16_t* Str) { return std::char_traits<char16_t>::length(Str); }
template<> inline bool StrnCmpHelper<char16_t>(const char16_t* Left, const char16_t* Right, size_t N) { return std::char_traits<char16_t>::compare(Left, Right, N) == 0; }

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
    intptr_t Slide;
};

/* dyld image index of a module; nullptr/"" selects the main executable (image 0).
 * An exact match on the file name (e.g. "DeltaForceClient") wins over a substring match on the full path. */
inline int32_t FindImageIndex(const char* ImageName)
{
    const uint32_t Count = _dyld_image_count();

    if (!ImageName || !ImageName[0])
        return Count > 0 ? 0 : -1;

    for (uint32_t i = 0; i < Count; i++)
    {
        const char* Name = _dyld_get_image_name(i);
        if (!Name)
            continue;

        const char* LastSlash = strrchr(Name, '/');
        if (strcmp(LastSlash ? LastSlash + 1 : Name, ImageName) == 0)
            return static_cast<int32_t>(i);
    }

    for (uint32_t i = 0; i < Count; i++)
    {
        const char* Name = _dyld_get_image_name(i);
        if (Name && strstr(Name, ImageName))
            return static_cast<int32_t>(i);
    }

    return -1;
}

/* A segment maps memory only if it is readable. __PAGEZERO (4GB, no protection) must never be treated as part of the image. */
inline bool IsMappedSegment(const struct segment_command_64* Seg)
{
    return Seg->vmsize != 0 && (Seg->initprot & VM_PROT_READ) != 0;
}

inline MachImageInfo GetImageBaseAndSize(const char* ImageName = nullptr)
{
    const int32_t Index = FindImageIndex(ImageName);
    if (Index < 0)
        return { 0, 0, nullptr, 0 };

    const auto* Header = reinterpret_cast<const struct mach_header_64*>(_dyld_get_image_header(Index));
    if (!Header)
        return { 0, 0, nullptr, 0 };

    const intptr_t Slide = _dyld_get_image_vmaddr_slide(Index);

    /* End of the highest mapped segment. The header is the start of __TEXT, so [Header, End) covers the whole mapped image. */
    uintptr_t MaxAddr = 0;
    auto* Cmd = reinterpret_cast<const struct load_command*>(Header + 1);

    for (uint32_t c = 0; c < Header->ncmds; c++)
    {
        if (Cmd->cmd == LC_SEGMENT_64)
        {
            const auto* Seg = reinterpret_cast<const struct segment_command_64*>(Cmd);

            if (IsMappedSegment(Seg) && (Seg->vmaddr + Seg->vmsize) > MaxAddr)
                MaxAddr = Seg->vmaddr + Seg->vmsize;
        }
        Cmd = reinterpret_cast<const struct load_command*>(reinterpret_cast<uintptr_t>(Cmd) + Cmd->cmdsize);
    }

    const uintptr_t Base = reinterpret_cast<uintptr_t>(Header);
    const uintptr_t End = MaxAddr + Slide;

    return { Base, static_cast<size_t>(End > Base ? End - Base : 0), Header, Slide };
}

inline uintptr_t GetModuleBase(const char* SearchModuleName = nullptr) {
    if (SearchModuleName == nullptr)
        return (uintptr_t)_dyld_get_image_header(0);

    return GetImageBaseAndSize(SearchModuleName).Base;
}

struct MachSegmentInfo
{
    uintptr_t Start;
    uintptr_t Size;
    vm_prot_t InitProt;
    char Name[17];
};

/* All mapped segments of an image with their slid start address. */
inline std::vector<MachSegmentInfo> GetImageSegments(const char* ImageName = nullptr)
{
    std::vector<MachSegmentInfo> Segments;

    const auto [ImageBase, ImageSize, Header, Slide] = GetImageBaseAndSize(ImageName);
    if (!Header)
        return Segments;

    auto* Cmd = reinterpret_cast<const struct load_command*>(Header + 1);
    for (uint32_t c = 0; c < Header->ncmds; c++)
    {
        if (Cmd->cmd == LC_SEGMENT_64)
        {
            const auto* Seg = reinterpret_cast<const struct segment_command_64*>(Cmd);

            if (IsMappedSegment(Seg))
            {
                MachSegmentInfo Info{};
                Info.Start = static_cast<uintptr_t>(Seg->vmaddr + Slide);
                Info.Size = static_cast<uintptr_t>(Seg->vmsize);
                Info.InitProt = Seg->initprot;
                memcpy(Info.Name, Seg->segname, 16);
                Segments.push_back(Info);
            }
        }
        Cmd = reinterpret_cast<const struct load_command*>(reinterpret_cast<uintptr_t>(Cmd) + Cmd->cmdsize);
    }

    return Segments;
}

/* Writable segments (__DATA, __DATA_DIRTY, __DATA_CONST, __AUTH, ...). Global engine objects such as GUObjectArray,
 * the FNamePool and GWorld live in one of these, never in __TEXT. */
inline std::vector<MachSegmentInfo> GetWritableImageSegments(const char* ImageName = nullptr)
{
    std::vector<MachSegmentInfo> Segments = GetImageSegments(ImageName);

    std::erase_if(Segments, [](const MachSegmentInfo& Seg) { return (Seg.InitProt & VM_PROT_WRITE) == 0; });

    return Segments;
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
            if (strncmp(Seg->segname, SegmentName, 16) == 0 && IsMappedSegment(Seg))
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

template <typename T>
inline T SafeRead(uintptr_t Address, T Default = {})
{
    T Buffer = Default;
    vm_size_t Size = 0;
    // Uses the same API as your IsBadReadPtr
    kern_return_t KR = vm_read_overwrite(mach_task_self(), (vm_address_t)Address, sizeof(T), (vm_address_t)&Buffer, &Size);
    if (KR != KERN_SUCCESS || Size != sizeof(T)) return Default;
    return Buffer;
}

/* Lowest address that can possibly be mapped: everything inside the main executable's __PAGEZERO is reserved. */
inline uintptr_t GetLowestMappableAddress()
{
    static const uintptr_t LowestAddress = []() -> uintptr_t
    {
        const auto* Header = reinterpret_cast<const struct mach_header_64*>(_dyld_get_image_header(0));
        if (!Header)
            return 0x1000;

        auto* Cmd = reinterpret_cast<const struct load_command*>(Header + 1);
        for (uint32_t c = 0; c < Header->ncmds; c++)
        {
            if (Cmd->cmd == LC_SEGMENT_64)
            {
                const auto* Seg = reinterpret_cast<const struct segment_command_64*>(Cmd);

                if (Seg->vmaddr == 0 && Seg->initprot == VM_PROT_NONE && Seg->vmsize >= 0x1000)
                    return static_cast<uintptr_t>(Seg->vmsize);
            }
            Cmd = reinterpret_cast<const struct load_command*>(reinterpret_cast<uintptr_t>(Cmd) + Cmd->cmdsize);
        }
        return 0x1000;
    }();

    return LowestAddress;
}

inline bool IsBadReadPtr(const void* Ptr)
{
    const uintptr_t Address = reinterpret_cast<uintptr_t>(Ptr);

    /* Nothing is mapped inside __PAGEZERO, and user-space addresses never use the top 17 bits (tagged/PAC/garbage values). */
    if (Address < GetLowestMappableAddress() || (Address >> 47) != 0)
        return true;

    uint8_t Data = 0;
    vm_size_t Size = 0;

    /* Anything but KERN_SUCCESS means the byte can't be read (KERN_INVALID_ARGUMENT, KERN_NO_SPACE, ... included). */
    const kern_return_t KR = vm_read_overwrite(mach_task_self(), (vm_address_t)Address, 1, (vm_address_t)&Data, &Size);
    return KR != KERN_SUCCESS || Size != 1;
}

inline bool IsBadReadPtr(const uintptr_t Ptr)
{
    return IsBadReadPtr(reinterpret_cast<const void*>(Ptr));
}

/* Checks that every page touched by [Ptr, Ptr + Size) is readable. */
inline bool IsBadReadRange(const void* Ptr, uintptr_t Size)
{
    if (Size == 0)
        return IsBadReadPtr(Ptr);

    constexpr uintptr_t PageSize = 0x1000;

    const uintptr_t Start = reinterpret_cast<uintptr_t>(Ptr);
    const uintptr_t Last = Start + Size - 1;
    if (Last < Start)
        return true;

    for (uintptr_t Page = Start & ~(PageSize - 1); Page <= Last; Page += PageSize)
    {
        if (IsBadReadPtr(Page < Start ? Start : Page))
            return true;
    }

    return false;
}

inline bool IsValidVirtualAddress(const uintptr_t Address)
{
    return !IsBadReadPtr(Address);
}

inline bool IsInProcessRange(const uintptr_t Address)
{
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
    const auto [ImageBase, ImageSize, Header, Slide] = GetImageBaseAndSize();
    
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

    for (uintptr_t i = 0x0; i <= (Range - sizeof(T)); i += Alignment)
    {
        T* TypedPtr = reinterpret_cast<T*>(StartAddress + i);
        if (*TypedPtr == Value)
            return TypedPtr;
    }
    return nullptr;
}

/* Searches the named segment first, then every other writable segment of the main image. Never touches unmapped memory. */
template<typename T>
inline T* FindAlignedValueInProcess(T Value, const std::string& Sectionname = "__DATA", int32_t Alignment = alignof(T), bool bSearchAllSections = false)
{
    const std::vector<MachSegmentInfo> Segments = GetWritableImageSegments();

    if (!bSearchAllSections)
    {
        for (const MachSegmentInfo& Seg : Segments)
        {
            if (strncmp(Seg.Name, Sectionname.c_str(), 16) != 0)
                continue;

            if (T* Result = FindAlignedValueInProcessInRange(Value, Alignment, Seg.Start, Seg.Size))
                return Result;
        }
    }

    for (const MachSegmentInfo& Seg : Segments)
    {
        if (!bSearchAllSections && strncmp(Seg.Name, Sectionname.c_str(), 16) == 0)
            continue;

        if (T* Result = FindAlignedValueInProcessInRange(Value, Alignment, Seg.Start, Seg.Size))
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

    if (!VTable || IsBadReadPtr(VTable))
        return { nullptr, -1 };

    for (int i = 0; i < NumFunctions; i++)
    {
        /* The vtable may end right before an unmapped page, only read entries that are readable. */
        if ((reinterpret_cast<uintptr_t>(&VTable[i]) & 0xFFF) < sizeof(void*) && IsBadReadPtr(&VTable[i]))
            break;

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
    const auto [ImageBase, ImageSize, Header, Slide] = GetImageBaseAndSize();
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

    for (uintptr_t i = 0; (i + 8) <= TextSize; i += 4)
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

    const auto [ImageBase, ImageSize, Header, Slide] = GetImageBaseAndSize();
    const uintptr_t ImageEnd = ImageBase + ImageSize;

    if (!Header || !RefStr || (StartAddress != 0x0 && (StartAddress < ImageBase || StartAddress >= ImageEnd)))
        return nullptr;

    const int32_t RefStrLen = StrlenHelper(RefStr);
    const uintptr_t RefStrByteLen = static_cast<uintptr_t>(RefStrLen) * sizeof(CharType);

    /* ADRP+ADD pairs only exist in executable segments (__TEXT). Scanning data or __LINKEDIT would only produce false positives. */
    for (const MachSegmentInfo& Seg : GetImageSegments())
    {
        if ((Seg.InitProt & VM_PROT_EXECUTE) == 0)
            continue;

        uintptr_t ScanStart = Seg.Start;
        uintptr_t ScanEnd = Seg.Start + Seg.Size;

        if (StartAddress != 0x0)
        {
            /* Start searching a bit ahead if StartAddress is provided to avoid immediate self-find */
            const uintptr_t AlignedStart = (StartAddress & ~static_cast<uintptr_t>(3)) + 8;

            if (AlignedStart >= ScanEnd)
                continue;

            ScanStart = AlignedStart > ScanStart ? AlignedStart : ScanStart;
        }

        if (Range > 0 && (ScanStart + static_cast<uintptr_t>(Range)) < ScanEnd)
            ScanEnd = ScanStart + static_cast<uintptr_t>(Range);

        for (uintptr_t Address = ScanStart; (Address + 8) <= ScanEnd; Address += 4)
        {
            /* Check for ADRP+ADD (ADRL) sequence which loads a pointer relative to PC */
            if (!ASMUtils::IsADRL(Address))
                continue;

            const uintptr_t StrPtr = ASMUtils::ResolveADRL(Address);

            if (StrPtr < ImageBase || (StrPtr + RefStrByteLen) > ImageEnd)
                continue;

            /* Check if the string at the resolved address matches our target */
            if (StrnCmpHelper(RefStr, reinterpret_cast<const CharType*>(StrPtr), RefStrLen))
                return { Address };
        }

        if (StartAddress != 0x0 || Range > 0)
            break;
    }

    return nullptr;
}

/* Finds the native function registered for an exec function name (static { const char* Name; FNativeFuncPtr Ptr; } tables in data segments). */
template<typename Type = const char*>
inline MemAddress FindUnrealExecFunctionByString(Type RefStr, void* StartAddress = nullptr)
{
    const auto [ImageBase, ImageSize, Header, Slide] = GetImageBaseAndSize();
    const uintptr_t ImageEnd = ImageBase + ImageSize;

    if (!Header || !RefStr)
        return nullptr;

    const int32_t RefStrLen = StrlenHelper(RefStr);
    const uintptr_t RefStrByteLen = static_cast<uintptr_t>(RefStrLen + 1) * sizeof(*RefStr);

    auto IsInImage = [ImageBase, ImageEnd](uintptr_t Address, uintptr_t Size) -> bool
    {
        return Address >= ImageBase && (Address + Size) <= ImageEnd;
    };

    for (const MachSegmentInfo& Seg : GetImageSegments())
    {
        /* The name/function pointer pairs are data, never code. */
        if ((Seg.InitProt & VM_PROT_EXECUTE) != 0 || strncmp(Seg.Name, "__LINKEDIT", 16) == 0)
            continue;

        uintptr_t ScanStart = Seg.Start;
        const uintptr_t ScanEnd = Seg.Start + Seg.Size;

        if (StartAddress)
        {
            const uintptr_t Requested = reinterpret_cast<uintptr_t>(StartAddress) & ~static_cast<uintptr_t>(7);
            if (Requested >= ScanEnd)
                continue;

            ScanStart = Requested > ScanStart ? Requested : ScanStart;
        }

        for (uintptr_t Address = ScanStart; (Address + 2 * sizeof(void*)) <= ScanEnd; Address += sizeof(void*))
        {
            const uintptr_t PossibleStringAddress = *reinterpret_cast<uintptr_t*>(Address);
            const uintptr_t PossibleExecFuncAddress = *reinterpret_cast<uintptr_t*>(Address + sizeof(void*));

            if (PossibleStringAddress == PossibleExecFuncAddress) continue;
            if (!IsInImage(PossibleStringAddress, RefStrByteLen) || !IsInImage(PossibleExecFuncAddress, 4)) continue;

            if constexpr (std::is_same<Type, const char*>())
            {
                if (strncmp(reinterpret_cast<const char*>(RefStr), reinterpret_cast<const char*>(PossibleStringAddress), RefStrLen) == 0)
                    return { PossibleExecFuncAddress };
            }
            else
            {
                if (StrnCmpHelper(RefStr, reinterpret_cast<decltype(RefStr)>(PossibleStringAddress), RefStrLen))
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

namespace FileNameHelper_IOSRemoved
{
    inline void _MakeValidFileName_Removed(std::string& InOutName)
    {
        for (char& c : InOutName)
        {
            if (c == '<' || c == '>' || c == ':' || c == '\"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
                c = '_';
        }
    }
}
