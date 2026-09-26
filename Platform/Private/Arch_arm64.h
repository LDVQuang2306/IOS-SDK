#pragma once
#include <cstdint>

/*
iOS/ARM64 placeholder mirroring the Architecture_x86_64 surface. The dumper's OffsetFinder
calls a small subset of these; iOS-specific decoders (ADRP/ADD/ADRL/LDR) live in
Utils.h's ASMUtils namespace and are used directly by NameArray.cpp / UnrealTypes.cpp.
*/

namespace Architecture_x86_64
{
	bool IsValid64BitVirtualAddress(const uintptr_t Address);
	bool IsValid64BitVirtualAddress(const void* Address);

	bool Is32BitRIPRelativeJump(const uintptr_t Address);

	uintptr_t Resolve32BitRIPRelativeJumpTarget(const uintptr_t Address);
	uintptr_t Resolve32BitRegisterRelativeJump(const uintptr_t Address);
	uintptr_t Resolve32BitSectionRelativeCall(const uintptr_t Address);
	uintptr_t Resolve32BitRelativeCall(const uintptr_t Address);
	uintptr_t Resolve32BitRelativeMove(const uintptr_t Address);
	uintptr_t Resolve32BitRelativeLea(const uintptr_t Address);
	uintptr_t Resolve32BitRelativePush(const uintptr_t Address);
	uintptr_t Resolve32bitAbsoluteCall(const uintptr_t Address);
	uintptr_t Resolve32bitAbsoluteMove(const uintptr_t Address);

	bool IsFunctionRet(const uintptr_t Address);
	uintptr_t ResolveJumpIfInstructionIsJump(const uintptr_t Address, const uintptr_t DefaultReturnValueOnFail = 0);

	uintptr_t FindNextFunctionStart(const uintptr_t Address);
	uintptr_t FindNextFunctionStart(const void* Address);

	uintptr_t FindFunctionEnd(const uintptr_t Address, uint32_t Range = 0xFFFF);

	uintptr_t GetRipRelativeCalledFunction(const uintptr_t Address, const int32_t OneBasedFuncIndex, bool(*IsWantedTarget)(const uintptr_t CalledAddr) = nullptr);

	/* Port of iOS_UEDumper's IGameProfile::findProcessEvent — scores each vtable
	 * slot against UE-source-level fingerprints (UObject.Index load, FUObjectItem
	 * stride MOV, UFunction.FunctionFlags+1/+2 LDRB, UStruct.Size LDR, ChildProperties
	 * LDR) and returns the highest-scoring slot.
	 * Caller must have populated Off::UObject::Index, Off::InSDK::ObjArray::FUObjectItemSize,
	 * Off::UFunction::FunctionFlags, Off::UStruct::Size, Off::UStruct::ChildProperties
	 * (or .Children), and Off::InSDK::ObjArray::GObjects before invoking.
	 * Returns -1 on failure. */
	int32_t FindProcessEventIndex(void** UObjectVTable);
}
