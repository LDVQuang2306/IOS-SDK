#pragma once

#include <vector>

#include "../Unreal/ObjectArray.h"
#include "../../../Menu/Logger.h"

namespace OffsetFinder
{
    constexpr int32 OffsetNotFound = -1;
    template<int Alignement = 4, typename T>
    inline int32_t FindOffset(const std::vector<std::pair<void*, T>>& ObjectValuePair, int MinOffset = 0x28, int MaxOffset = 0x1A0)
    {

        int32_t HighestFoundOffset = MinOffset;
        for (int i = 0; i < ObjectValuePair.size(); i++)
        {
            if (ObjectValuePair[i].first == nullptr)
                continue;

            for (int j = HighestFoundOffset; j < MaxOffset; j += Alignement)
            {
                uintptr_t Address = reinterpret_cast<uintptr_t>(ObjectValuePair[i].first) + j;
                if (IsBadReadPtr((void*)Address))
                    continue;
                
                const T TypedValueAtOffset = *reinterpret_cast<T*>(Address);
                if (TypedValueAtOffset == ObjectValuePair[i].second && j >= HighestFoundOffset)
                {
                    if (j > HighestFoundOffset)
                    {
                        HighestFoundOffset = j;
                        i = -1;
                    }
                    j = MaxOffset; // Break inner loop to move to next object
                }
            }
        }

        if (HighestFoundOffset != MinOffset)
            return HighestFoundOffset;
        
        return OffsetNotFound;
    }

    template<bool bCheckForVft = true>
    inline int32_t GetValidPointerOffset(const uint8_t* ObjA, const uint8_t* ObjB, int32_t StartingOffset, int32_t MaxOffset)
    {
        
        if (IsBadReadPtr(ObjA) || IsBadReadPtr(ObjB))
            return OffsetNotFound;
        
        for (int j = StartingOffset; j <= MaxOffset; j += sizeof(void*))
        {
            // Calculate the address of the member variable
            uintptr_t MemberAddrA = (uintptr_t)(ObjA + j);
            uintptr_t MemberAddrB = (uintptr_t)(ObjB + j);

            if (IsBadReadPtr((void*)MemberAddrA) || IsBadReadPtr((void*)MemberAddrB))
                continue;

            // We use a void* here because we are looking for pointers
            void* PtrA = *reinterpret_cast<void**>(MemberAddrA);
            void* PtrB = *reinterpret_cast<void**>(MemberAddrB);


            if (IsBadReadPtr(PtrA) || IsBadReadPtr(PtrB))
                continue;

            // Check for VTable (Dereference the pointer we just found)
            if constexpr (bCheckForVft)
            {
                // Validate if PtrA/PtrB points to readable memory (VTable pointer)
                // Note: We already checked IsBadReadPtr(PtrA) above, so we can safely read *PtrA
                void* VTableA = *reinterpret_cast<void**>(PtrA);
                void* VTableB = *reinterpret_cast<void**>(PtrB);

                if (IsBadReadPtr(VTableA) || IsBadReadPtr(VTableB))
                    continue;
            }
            
            return j;
        }
        return OffsetNotFound;
    };

	/* UObject */
	int32_t FindUObjectFlagsOffset();
	int32_t FindUObjectIndexOffset();
	int32_t FindUObjectClassOffset();
	int32_t FindUObjectNameOffset();
	int32_t FindUObjectOuterOffset();

	void FixupHardcodedOffsets();
	void InitFNameSettings();
	void PostInitFNameSettings();

	/* UField */
	int32_t FindUFieldNextOffset();

	/* FField */
	int32_t FindFFieldNextOffset();

	int32_t FindFFieldNameOffset();

	/* UEnum */
	int32_t FindEnumNamesOffset();

	/* UStruct */
	int32_t FindSuperOffset();
	int32_t FindChildOffset();
	int32_t FindChildPropertiesOffset();
	int32_t FindStructSizeOffset();
	int32_t FindMinAlignmentOffset();

	/* UFunction */
	int32_t FindFunctionFlagsOffset();
	int32_t FindFunctionNativeFuncOffset();

	/* UClass */
	int32_t FindCastFlagsOffset();
	int32_t FindDefaultObjectOffset();
	int32_t FindImplementedInterfacesOffset();

	/* Property */
	int32_t FindElementSizeOffset();
	int32_t FindArrayDimOffset();
	int32_t FindPropertyFlagsOffset();
	int32_t FindOffsetInternalOffset();

	/* BoolProperty */
	int32_t FindBoolPropertyBaseOffset();

	/* ArrayProperty */
	int32_t FindInnerTypeOffset(const int32 PropertySize);

	/* SetProperty */
	int32_t FindSetPropertyBaseOffset(const int32 PropertySize);

	/* MapProperty */
	int32_t FindMapPropertyBaseOffset(const int32 PropertySize);

	/* InSDK -> ULevel */
	int32_t FindLevelActorsOffset();

	/* InSDK -> UDataTable */
	int32_t FindDatatableRowMapOffset();
}
