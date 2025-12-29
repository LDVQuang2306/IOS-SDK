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

inline MachImageInfo GetImageBaseAndSize(const char* ImageName = nullptr)
{
    uint32_t Count = _dyld_image_count();
    for (uint32_t i = 0; i < Count; i++)
    {
        const char* Name = _dyld_get_image_name(i);
        if (!ImageName || strstr(Name, ImageName))
        {
            const auto* Header = (const struct mach_header_64*)_dyld_get_image_header(i);
            
            // Calculate real image size by iterating segments
            uintptr_t MinAddr = ~0UL;
            uintptr_t MaxAddr = 0;
            auto* Cmd = (const struct load_command*)(Header + 1);
            
            for (uint32_t c = 0; c < Header->ncmds; c++)
            {
                if (Cmd->cmd == LC_SEGMENT_64) {
                    auto* Seg = (const struct segment_command_64*)Cmd;
                    if (Seg->vmaddr < MinAddr) MinAddr = Seg->vmaddr;
                    if ((Seg->vmaddr + Seg->vmsize) > MaxAddr) MaxAddr = Seg->vmaddr + Seg->vmsize;
                }
                Cmd = (const struct load_command*)((uintptr_t)Cmd + Cmd->cmdsize);
            }
            return { (uintptr_t)Header, (size_t)(MaxAddr - MinAddr), Header };
        }
    }
    return { 0, 0, nullptr };
}

inline uintptr_t GetModuleBase(const char* SearchModuleName = nullptr) {
    if (SearchModuleName == nullptr)
        return (uintptr_t)_dyld_get_image_header(0);

    return GetImageBaseAndSize(SearchModuleName).Base;
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

inline bool IsBadReadPtr(const void* Ptr)
{
    uint8_t Data = 0;
    size_t Size = 0;
    
    kern_return_t KR = vm_read_overwrite(mach_task_self(), (vm_address_t)Ptr, 1, (vm_address_t)&Data, &Size);
    return (KR == KERN_INVALID_ADDRESS ||
            KR == KERN_MEMORY_FAILURE  ||
            KR == KERN_MEMORY_ERROR    ||
            KR == KERN_PROTECTION_FAILURE);
};

inline bool IsBadReadPtr(const uintptr_t Ptr)
{
    return IsBadReadPtr(reinterpret_cast<const void*>(Ptr));
}

inline bool IsValidVirtualAddress(const uintptr_t Address)
{
    return !IsBadReadPtr(Address);
}

inline bool IsInProcessRange(const uintptr_t Address)
{
    const auto [Base, Size, Header] = GetImageBaseAndSize();
    if (Address >= Base && Address < (Base + Size))
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
inline T* FindAlignedValueInProcessInRange(T Value, int32_t Alignment, uintptr_t StartAddress, uint32_t Range)
{
    for (uint32_t i = 0x0; i < Range; i += Alignment)
    {
        T* TypedPtr = reinterpret_cast<T*>(StartAddress + i);
        if (*TypedPtr == Value)
            return TypedPtr;
    }
    return nullptr;
}

template<typename T>
inline T* FindAlignedValueInProcess(T Value, const std::string& Sectionname = "__DATA", int32_t Alignment = alignof(T), bool bSearchAllSections = false)
{
    const auto [ImageBase, ImageSize, Header] = GetImageBaseAndSize();
    uintptr_t SearchStart = ImageBase;
    uintptr_t SearchRange = ImageSize;

    if (!bSearchAllSections)
    {
        const auto [SectionStart, SectionSize] = GetSegmentByName(Header, Sectionname.c_str());
        if (SectionStart != 0x0 && SectionSize != 0x0)
        {
            SearchStart = SectionStart;
            SearchRange = SectionSize;
        }
        else
        {
            bSearchAllSections = true;
        }
    }

    T* Result = FindAlignedValueInProcessInRange(Value, Alignment, SearchStart, SearchRange);
    if (!Result && SearchStart != ImageBase)
        return FindAlignedValueInProcess(Value, Sectionname, Alignment, true);
    return Result;
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

    /* Start searching a bit ahead if StartAddress is provided to avoid immediate self-find */
    uint32_t* SearchStart = StartAddress ? (reinterpret_cast<uint32_t*>(StartAddress) + 2) : reinterpret_cast<uint32_t*>(ImageBase);
    int32_t SearchRange = StartAddress ? ImageEnd - StartAddress : ImageSize;

    if (Range != 0x0)
        SearchRange = fmin(Range, SearchRange);

    const int32_t RefStrLen = StrlenHelper(RefStr);

    for (uintptr_t i = 0; i < SearchRange; i += 4)
    {
        /* Check for ADRP+ADD (ADRL) sequence which loads a pointer relative to PC */
        if (ASMUtils::IsADRL(reinterpret_cast<uintptr_t>(SearchStart) + i))
        {
            const uintptr_t StrPtr = ASMUtils::ResolveADRL(reinterpret_cast<uintptr_t>(SearchStart) + i);
            
            if (!IsInProcessRange(StrPtr))
                continue;

            /* Check if the string at the resolved address matches our target */
            if (StrnCmpHelper(RefStr, reinterpret_cast<const CharType*>(StrPtr), RefStrLen))
                return { reinterpret_cast<uintptr_t>(SearchStart) + i };
        }
    }

    return nullptr;
}

template<typename Type = const char*>
inline MemAddress FindUnrealExecFunctionByString(Type RefStr, void* StartAddress = nullptr)
{
    const auto [ImageBase, ImageSize, Header] = GetImageBaseAndSize();
    uint8_t* SearchStart = StartAddress ? reinterpret_cast<uint8_t*>(StartAddress) : reinterpret_cast<uint8_t*>(ImageBase);
    int32_t SearchRange = ImageSize;

    const int32_t RefStrLen = StrlenHelper(RefStr);

    static auto IsValidExecFunctionNotSetupFunc = [](uintptr_t Address) -> bool
    {
        if (!IsInProcessRange(Address)) return false;
        return true;
    };

    for (uintptr_t i = 0; i < (SearchRange - 0x8); i += sizeof(void*))
    {
        const uintptr_t PossibleStringAddress = *reinterpret_cast<uintptr_t*>(SearchStart + i);
        const uintptr_t PossibleExecFuncAddress = *reinterpret_cast<uintptr_t*>(SearchStart + i + sizeof(void*));

        if (PossibleStringAddress == PossibleExecFuncAddress) continue;
        if (!IsInProcessRange(PossibleStringAddress) || !IsInProcessRange(PossibleExecFuncAddress)) continue;

        if constexpr (std::is_same<Type, const char*>())
        {
            if (strncmp(reinterpret_cast<const char*>(RefStr), reinterpret_cast<const char*>(PossibleStringAddress), RefStrLen) == 0 && IsValidExecFunctionNotSetupFunc(PossibleExecFuncAddress))
            {
                return { PossibleExecFuncAddress };
            }
        }
        else
        {
            if (wcsncmp(reinterpret_cast<const wchar_t*>(RefStr), reinterpret_cast<const wchar_t*>(PossibleStringAddress), RefStrLen) == 0 && IsValidExecFunctionNotSetupFunc(PossibleExecFuncAddress))
            {
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
