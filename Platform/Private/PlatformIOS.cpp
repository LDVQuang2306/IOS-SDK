#include "PlatformIOS.h"

#include <cstring>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>

#include "Utils.h"

/*
SectionInfo opaque layout:
    [0x00] uintptr_t Start
    [0x08] uintptr_t Size
*/
struct SectionInfoLayout
{
    uintptr_t Start;
    uintptr_t Size;
    uint64_t  Padding;
};
static_assert(sizeof(SectionInfoLayout) <= sizeof(SectionInfo), "SectionInfo storage too small");

namespace
{
    inline SectionInfoLayout& Lay(SectionInfo& Info) { return *reinterpret_cast<SectionInfoLayout*>(&Info); }
    inline const SectionInfoLayout& Lay(const SectionInfo& Info) { return *reinterpret_cast<const SectionInfoLayout*>(&Info); }

    // PE-style section names (.text/.rdata/.data) map to Mach-O segments.
    inline const char* TranslateSegmentName(const char* Name)
    {
        if (!Name) return "__TEXT";
        if (Name[0] == '_') return Name;
        if (strcmp(Name, ".text") == 0)   return "__TEXT";
        if (strcmp(Name, ".rdata") == 0)  return "__DATA_CONST";
        if (strcmp(Name, ".data") == 0)   return "__DATA";
        if (strcmp(Name, ".rodata") == 0) return "__DATA_CONST";
        return Name;
    }
}

namespace Platform
{
    uintptr_t GetModuleBase(const char* const ModuleName)
    {
        return ::GetModuleBase(ModuleName);
    }

    uintptr_t GetOffset(const uintptr_t Address, const char* const /*ModuleName*/)
    {
        return ::GetOffset(Address);
    }

    uintptr_t GetOffset(const void* Address, const char* const /*ModuleName*/)
    {
        return ::GetOffset(Address);
    }

    SectionInfo GetSectionInfo(const std::string& SectionName, const char* const ModuleName)
    {
        SectionInfo Info{};
        const auto [ImageBase, ImageSize, Header, Slide] = ::GetImageBaseAndSize(ModuleName);
        if (!Header) return Info;

        const auto [SegStart, SegSize] = ::GetSegmentByName(Header, TranslateSegmentName(SectionName.c_str()));
        Lay(Info).Start = SegStart;
        Lay(Info).Size = SegSize;
        return Info;
    }

    void* IterateSectionWithCallback(const SectionInfo& Info, const std::function<bool(void* Address)>& Callback, uint32_t Granularity, uint32_t OffsetFromEnd)
    {
        if (!Info.IsValid() || !Callback) return nullptr;

        const uintptr_t Start = Lay(Info).Start;
        const uintptr_t End = Start + Lay(Info).Size - OffsetFromEnd;
        if (Granularity == 0) Granularity = 0x4;

        for (uintptr_t Addr = Start; Addr + Granularity <= End; Addr += Granularity)
        {
            void* P = reinterpret_cast<void*>(Addr);
            if (Callback(P)) return P;
        }
        return nullptr;
    }

    void* IterateAllSectionsWithCallback(const std::function<bool(void* Address)>& Callback, uint32_t Granularity, uint32_t OffsetFromEnd, const char* const ModuleName)
    {
        if (!Callback) return nullptr;

        const auto [ImageBase, ImageSize, Header, Slide] = ::GetImageBaseAndSize(ModuleName);
        if (!Header) return nullptr;

        if (Granularity == 0) Granularity = 0x4;

        uintptr_t CommandPtr = reinterpret_cast<uintptr_t>(Header + 1);
        for (uint32_t i = 0; i < Header->ncmds; ++i)
        {
            const auto* LC = reinterpret_cast<const struct load_command*>(CommandPtr);
            if (LC->cmd == LC_SEGMENT_64)
            {
                const auto* Seg = reinterpret_cast<const struct segment_command_64*>(LC);
                const uintptr_t Start = Seg->vmaddr + Slide;
                const uintptr_t End = Start + Seg->vmsize - OffsetFromEnd;

                for (uintptr_t Addr = Start; Addr + Granularity <= End; Addr += Granularity)
                {
                    void* P = reinterpret_cast<void*>(Addr);
                    if (Callback(P)) return P;
                }
            }
            CommandPtr += LC->cmdsize;
        }
        return nullptr;
    }

    bool IsAddressInAnyModule(const uintptr_t Address)
    {
        return ::IsInProcessRange(Address);
    }

    bool IsAddressInAnyModule(const void* Address)
    {
        return ::IsInProcessRange(Address);
    }

    bool IsAddressInProcessRange(const uintptr_t Address)
    {
        return ::IsInProcessRange(Address);
    }

    bool IsAddressInProcessRange(const void* Address)
    {
        return ::IsInProcessRange(Address);
    }

    bool IsBadReadPtr(const uintptr_t Address)
    {
        return ::IsBadReadPtr(Address);
    }

    bool IsBadReadPtr(const void* Address)
    {
        return ::IsBadReadPtr(Address);
    }

    const void* GetAddressOfImportedFunction(const char* /*SearchModuleName*/, const char* ModuleToImportFrom, const char* SearchFunctionName)
    {
        if (!SearchFunctionName) return nullptr;
        void* Handle = ModuleToImportFrom ? dlopen(ModuleToImportFrom, RTLD_NOW | RTLD_NOLOAD) : RTLD_DEFAULT;
        if (!Handle) return nullptr;
        void* Sym = dlsym(Handle, SearchFunctionName);
        if (Handle != RTLD_DEFAULT) dlclose(Handle);
        return Sym;
    }

    const void* GetAddressOfImportedFunctionFromAnyModule(const char* /*ModuleToImportFrom*/, const char* SearchFunctionName)
    {
        if (!SearchFunctionName) return nullptr;
        return dlsym(RTLD_DEFAULT, SearchFunctionName);
    }

    const void* GetAddressOfExportedFunction(const char* /*SearchModuleName*/, const char* SearchFunctionName)
    {
        if (!SearchFunctionName) return nullptr;
        return dlsym(RTLD_DEFAULT, SearchFunctionName);
    }

    void* FindPattern(const char* Signature, const uint32_t Offset, const bool /*bSearchAllSections*/, const uintptr_t StartAddress, const char* const /*ModuleName*/)
    {
        return ::FindPattern(Signature, "__TEXT", Offset, StartAddress);
    }

    void* FindPatternInRange(const char* Signature, const void* Start, const uintptr_t Range, const bool bRelative, const uint32_t Offset)
    {
        return ::FindPatternInRange(Signature, static_cast<const uint8_t*>(Start), Range, bRelative, Offset);
    }

    void* FindPatternInRange(const char* Signature, const uintptr_t Start, const uintptr_t Range, const bool bRelative, const uint32_t Offset)
    {
        return ::FindPatternInRange(Signature, reinterpret_cast<const uint8_t*>(Start), Range, bRelative, Offset);
    }

    void* FindPatternInRange(std::vector<int>&& Signature, const void* Start, const uintptr_t Range, const bool bRelative, uint32_t Offset, const uint32_t SkipCount)
    {
        return ::FindPatternInRange(Signature, static_cast<const uint8_t*>(Start), Range, bRelative, Offset, static_cast<int>(SkipCount));
    }

    template<typename CharType>
    void* FindByStringInAllSections(const CharType* RefStr, const uintptr_t StartAddress, int32_t Range, const bool /*bSearchOnlyExecutableSections*/, const char* const /*ModuleName*/)
    {
        MemAddress Result = ::FindByStringInAllSections<false, CharType>(RefStr, StartAddress, Range);
        return reinterpret_cast<void*>(static_cast<uintptr_t>(Result));
    }

    // Explicit template instantiations for the character types the dumper uses.
    template void* FindByStringInAllSections<char>(const char*, const uintptr_t, int32_t, const bool, const char* const);
    template void* FindByStringInAllSections<wchar_t>(const wchar_t*, const uintptr_t, int32_t, const bool, const char* const);
    template void* FindByStringInAllSections<char16_t>(const char16_t*, const uintptr_t, int32_t, const bool, const char* const);

    template<bool bShouldResolve32BitJumps>
    std::pair<const void*, int32_t> IterateVTableFunctions(void** VTable, const std::function<bool(const uint8_t* Address, int32_t Index)>& CallBackForEachFunc, int32_t NumFunctions, int32_t OffsetFromStart)
    {
        return ::IterateVTableFunctions<bShouldResolve32BitJumps>(VTable, CallBackForEachFunc, NumFunctions, OffsetFromStart);
    }

    template std::pair<const void*, int32_t> IterateVTableFunctions<true>(void**, const std::function<bool(const uint8_t*, int32_t)>&, int32_t, int32_t);
    template std::pair<const void*, int32_t> IterateVTableFunctions<false>(void**, const std::function<bool(const uint8_t*, int32_t)>&, int32_t, int32_t);
}

void* PlatformPrivateImplHelper::FinAlignedValueInRangeImpl(const void* ValuePtr, ValueCompareFuncType ComparisonFunction, const int32_t ValueTypeSize, const int32_t Alignment, uintptr_t StartAddress, uint32_t Range)
{
    if (!ValuePtr || !ComparisonFunction || ValueTypeSize <= 0 || Alignment <= 0 || Range == 0)
        return nullptr;

    const uintptr_t End = StartAddress + Range;
    for (uintptr_t Addr = StartAddress; Addr + ValueTypeSize <= End; Addr += Alignment)
    {
        if (ComparisonFunction(ValuePtr, reinterpret_cast<const void*>(Addr)))
            return reinterpret_cast<void*>(Addr);
    }
    return nullptr;
}

void* PlatformPrivateImplHelper::FindAlignedValueInSectionImpl(const SectionInfo& Info, const void* ValuePtr, ValueCompareFuncType ComparisonFunction, const int32_t ValueTypeSize, const int32_t Alignment)
{
    if (!Info.IsValid()) return nullptr;
    return FinAlignedValueInRangeImpl(ValuePtr, ComparisonFunction, ValueTypeSize, Alignment, Lay(Info).Start, static_cast<uint32_t>(Lay(Info).Size));
}

void* PlatformPrivateImplHelper::FindAlignedValueInAllSectionsImpl(const void* ValuePtr, ValueCompareFuncType ComparisonFunction, const int32_t ValueTypeSize, const int32_t Alignment, const uintptr_t StartAddress, int32_t Range, const char* const ModuleName)
{
    const auto [ImageBase, ImageSize, Header, Slide] = ::GetImageBaseAndSize(ModuleName);
    if (!Header) return nullptr;

    uintptr_t CommandPtr = reinterpret_cast<uintptr_t>(Header + 1);
    for (uint32_t i = 0; i < Header->ncmds; ++i)
    {
        const auto* LC = reinterpret_cast<const struct load_command*>(CommandPtr);
        if (LC->cmd == LC_SEGMENT_64)
        {
            const auto* Seg = reinterpret_cast<const struct segment_command_64*>(LC);
            const uintptr_t SegStart = Seg->vmaddr + Slide;
            const uintptr_t SegEnd = SegStart + Seg->vmsize;

            uintptr_t Start = StartAddress > SegStart ? StartAddress : SegStart;
            uintptr_t End = Range > 0 ? std::min<uintptr_t>(SegEnd, Start + Range) : SegEnd;
            if (Start < End)
            {
                if (void* Found = FinAlignedValueInRangeImpl(ValuePtr, ComparisonFunction, ValueTypeSize, Alignment, Start, static_cast<uint32_t>(End - Start)))
                    return Found;
            }
        }
        CommandPtr += LC->cmdsize;
    }
    return nullptr;
}
