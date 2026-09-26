#include "Arch_arm64.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "Utils.h"
#include "OffsetFinder/Offsets.h"

/*
ARM64 stubs for the Architecture_x86_64 surface. OffsetFinder calls a tiny subset
of these on ARM64; functions specific to x86 instruction encodings are no-ops.
ARM64-native decoders live in Utils.h's ASMUtils namespace and are used directly.
*/

namespace Architecture_x86_64
{
    bool IsValid64BitVirtualAddress(const uintptr_t Address)
    {
        return Address != 0 && !IsBadReadPtr(Address);
    }

    bool IsValid64BitVirtualAddress(const void* Address)
    {
        return IsValid64BitVirtualAddress(reinterpret_cast<uintptr_t>(Address));
    }

    bool Is32BitRIPRelativeJump(const uintptr_t /*Address*/) { return false; }

    uintptr_t Resolve32BitRIPRelativeJumpTarget(const uintptr_t Address) { return Address; }
    uintptr_t Resolve32BitRegisterRelativeJump(const uintptr_t Address)  { return Address; }
    uintptr_t Resolve32BitSectionRelativeCall(const uintptr_t Address)   { return Address; }
    uintptr_t Resolve32BitRelativeCall(const uintptr_t Address)          { return Address; }
    uintptr_t Resolve32BitRelativeMove(const uintptr_t Address)          { return Address; }
    uintptr_t Resolve32BitRelativeLea(const uintptr_t Address)           { return Address; }
    uintptr_t Resolve32BitRelativePush(const uintptr_t Address)          { return Address; }
    uintptr_t Resolve32bitAbsoluteCall(const uintptr_t Address)          { return Address; }
    uintptr_t Resolve32bitAbsoluteMove(const uintptr_t Address)          { return Address; }

    bool IsFunctionRet(const uintptr_t Address)
    {
        if (IsBadReadPtr(Address)) return false;
        // ARM64 RET / RET LR variants.
        const uint32_t Instr = *reinterpret_cast<const uint32_t*>(Address);
        return Instr == 0xD65F03C0 || Instr == 0xD65F03E0 || Instr == 0xD65F03E1;
    }

    uintptr_t ResolveJumpIfInstructionIsJump(const uintptr_t Address, const uintptr_t DefaultReturnValueOnFail)
    {
        if (IsBadReadPtr(Address)) return DefaultReturnValueOnFail;
        const uint32_t Instr = *reinterpret_cast<const uint32_t*>(Address);
        if (ASMUtils::IsBranchInstruction(Instr))
            return ASMUtils::ResolveBranchTarget(Address);
        return DefaultReturnValueOnFail;
    }

    uintptr_t FindNextFunctionStart(const uintptr_t Address)
    {
        if (IsBadReadPtr(Address)) return 0;
        for (uintptr_t Addr = Address; Addr < Address + 0xFFFF; Addr += 4)
        {
            if (IsFunctionRet(Addr))
                return Addr + 4;
        }
        return 0;
    }

    uintptr_t FindNextFunctionStart(const void* Address)
    {
        return FindNextFunctionStart(reinterpret_cast<uintptr_t>(Address));
    }

    uintptr_t FindFunctionEnd(const uintptr_t Address, uint32_t Range)
    {
        if (IsBadReadPtr(Address)) return 0;
        if (Range > 0xFFFF) Range = 0xFFFF;
        for (uintptr_t i = 0; i < Range; i += 4)
        {
            if (IsFunctionRet(Address + i))
                return Address + i;
        }
        return 0;
    }

    /* Minimal ARM64 decoder for the fingerprints FindProcessEventIndex needs.
     * Returns a struct with type flags and the (already-scaled) immediate. */
    namespace
    {
        struct DecodedInsn
        {
            bool      IsADRP = false;
            bool      IsLDR  = false;   // LDR Xt/Wt, [Xn, #imm12]   (immediate, scaled to bytes)
            bool      IsLDRB = false;   // LDRB Wt, [Xn, #imm12]
            bool      IsMOVZ = false;   // MOVZ Wt, #imm16
            uint32_t  Rd = 0;
            uint32_t  Rn = 0;
            int64_t   Immediate = 0;
            uintptr_t AdrpPage  = 0;    // ADRP page-base (PC & ~0xFFF + sign-extended imm21<<12)
        };

        DecodedInsn DecodeInsn(uint32_t Insn, uintptr_t PC)
        {
            DecodedInsn d{};

            if ((Insn & 0x9F000000) == 0x90000000)        // ADRP
            {
                d.IsADRP = true;
                d.Rd = Insn & 0x1F;
                uint64_t immhi = (Insn >> 5) & 0x7FFFF;
                uint64_t immlo = (Insn >> 29) & 0x3;
                int64_t  imm21 = ((int64_t)((immhi << 2) | immlo) << 43) >> 31;  // sign-extend
                d.AdrpPage = (PC & ~uintptr_t(0xFFF)) + imm21;
                d.Immediate = (int64_t)d.AdrpPage;
            }
            else if ((Insn & 0xFFC00000) == 0xF9400000)   // LDR (immediate, unsigned, 64-bit)
            {
                d.IsLDR = true;
                d.Rd = Insn & 0x1F;
                d.Rn = (Insn >> 5) & 0x1F;
                d.Immediate = (int64_t)(((Insn >> 10) & 0xFFF) * 8u);
            }
            else if ((Insn & 0xFFC00000) == 0xB9400000)   // LDR (immediate, 32-bit)
            {
                d.IsLDR = true;
                d.Rd = Insn & 0x1F;
                d.Rn = (Insn >> 5) & 0x1F;
                d.Immediate = (int64_t)(((Insn >> 10) & 0xFFF) * 4u);
            }
            else if ((Insn & 0xFFC00000) == 0x39400000)   // LDRB (immediate, unsigned)
            {
                d.IsLDRB = true;
                d.Rd = Insn & 0x1F;
                d.Rn = (Insn >> 5) & 0x1F;
                d.Immediate = (int64_t)((Insn >> 10) & 0xFFF);
            }
            else if ((Insn & 0xFF800000) == 0x52800000)   // MOVZ Wd, #imm16
            {
                d.IsMOVZ = true;
                d.Rd = Insn & 0x1F;
                d.Immediate = (int64_t)((Insn >> 5) & 0xFFFF);
            }

            return d;
        }
    }

    int32_t FindProcessEventIndex(void** UObjectVTable)
    {
        if (!UObjectVTable || IsBadReadPtr(UObjectVTable)) return -1;

        constexpr int MaxSlots  = 100;
        constexpr int InsnCount = 0x200 / 4;  // 128 instructions per function
        
        const int64_t   IndexOff   = Off::UObject::Index;
        const int64_t   ItemSize   = Off::InSDK::ObjArray::FUObjectItemSize;
        const int64_t   FuncFlags  = Off::UFunction::FunctionFlags;
        const int64_t   PropSize   = Off::UStruct::Size;
        const int64_t   ChildProps = Off::UStruct::ChildProperties ? Off::UStruct::ChildProperties
                                                                   : Off::UStruct::Children;
        const uintptr_t GObjects   = GetModuleBase() + (uintptr_t)Off::InSDK::ObjArray::GObjects;

        int32_t BestScore = 0;
        int32_t BestIdx   = -1;

        for (int slot = 0; slot < MaxSlots; ++slot)
        {
            void* FuncPtr = nullptr;
            if (IsBadReadPtr(&UObjectVTable[slot])) break;
            FuncPtr = UObjectVTable[slot];
            if (!FuncPtr || IsBadReadPtr(FuncPtr) || IsBadReadPtr((uint8_t*)FuncPtr + (InsnCount * 4) - 1))
                continue;

            const uint32_t* Insns = reinterpret_cast<const uint32_t*>(FuncPtr);
            std::array<bool, 7> Hits{};

            for (int j = 0; j < InsnCount; ++j)
            {
                const uintptr_t PC = reinterpret_cast<uintptr_t>(FuncPtr) + (j * 4);
                DecodedInsn d = DecodeInsn(Insns[j], PC);

                /* [0] ADRP (+ following ADD-imm or LDR-imm on same register) resolves
                 * to GUObjectArray address (either directly or as a pointer-to-pointer). */
                if (!Hits[0] && d.IsADRP)
                {
                    uintptr_t Resolved = d.AdrpPage;
                    for (int k = 1; k < 4 && (j + k) < InsnCount; ++k)
                    {
                        DecodedInsn d2 = DecodeInsn(Insns[j + k], PC + (k * 4));
                        if ((d2.IsLDR || d2.IsLDRB) && d2.Rn == d.Rd && d2.Immediate != 0)
                        {
                            Resolved += d2.Immediate;
                            break;
                        }
                    }
                    if (Resolved == GObjects) { Hits[0] = true; }
                    else if (!IsBadReadPtr((void*)Resolved))
                    {
                        if (*(uintptr_t*)Resolved == GObjects) Hits[0] = true;
                    }
                }

                if (!Hits[1] && d.IsLDR  && d.Immediate == IndexOff)        Hits[1] = true;
                if (!Hits[2] && d.IsMOVZ && d.Immediate == ItemSize)        Hits[2] = true;
                if (!Hits[3] && d.IsLDRB && d.Immediate == FuncFlags + 1)   Hits[3] = true;
                if (!Hits[4] && d.IsLDR  && d.Immediate == PropSize)        Hits[4] = true;
                if (!Hits[5] && d.IsLDR  && d.Immediate == ChildProps)      Hits[5] = true;
                if (!Hits[6] && d.IsLDRB && d.Immediate == FuncFlags + 2)   Hits[6] = true;
            }

            int32_t Score = 0;
            for (bool h : Hits) if (h) ++Score;

            if (Score > BestScore)
            {
                BestScore = Score;
                BestIdx   = slot;
            }
        }

        return BestIdx;
    }

    uintptr_t GetRipRelativeCalledFunction(const uintptr_t Address, const int32_t OneBasedFuncIndex, bool(*IsWantedTarget)(const uintptr_t CalledAddr))
    {
        if (!Address || OneBasedFuncIndex == 0) return 0;
        const int32_t Multiply = OneBasedFuncIndex > 0 ? 1 : -1;
        const int32_t Target = OneBasedFuncIndex * Multiply;

        int32_t NumCalls = 0;
        for (int i = 0; i < 0xFFF; i += 4)
        {
            const int32_t ByteOffset = i * Multiply;
            const uintptr_t CurrentPC = Address + ByteOffset;
            if (IsBadReadPtr(CurrentPC)) break;

            const uint32_t Instr = *reinterpret_cast<const uint32_t*>(CurrentPC);
            if (!ASMUtils::IsBranchInstruction(Instr)) continue;

            const uintptr_t CallTarget = ASMUtils::ResolveBranchTarget(CurrentPC);
            if (!IsInProcessRange(CallTarget)) continue;

            if (++NumCalls == Target)
            {
                if (IsWantedTarget && !IsWantedTarget(CallTarget))
                {
                    --NumCalls;
                    continue;
                }
                return CallTarget;
            }
        }
        return 0;
    }
}
