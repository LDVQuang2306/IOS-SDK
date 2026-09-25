#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Enums.h"

/*
* Delta Force (iOS, "DeltaForceClient") engine profile.
*
* Delta Force ships a modified UE4 core that the generic Dumper-7 finders can't handle:
*  - FNameEntry strings are XOR-obfuscated and FNamePool stores Blocks[] BEFORE CurrentByteCursor/CurrentBlock
*    (Blocks @0xC8, Cursor @0x100C8, CurrentBlock @0x100CC, 18 block-offset bits, stride 2).
*  - UObject header is reordered (Class 0x08, Outer 0x10, Name 0x1C, Index 0x24), so are UStruct/UFunction/FField/FProperty.
*  - FUObjectArray keeps ObjObjects at +0x10 with NumElements/NumChunks/Objects at +0x4/+0x8/+0x10 of it.
*
* The layout below is the validated "DF-v1" profile of the reference DFSDKDumper. GNames/GObjects are never hard-coded:
* they are discovered by scanning the image's data segments and every candidate is semantically validated
* (FName[0] == "None", CoreUObject anchors, InternalIndex == slot) before anything is dereferenced.
*/
namespace DeltaForce
{
	enum class ENameCodec : uint32_t
	{
		DfV1 = 0,  // XOR obfuscated entries
		Plain = 1, // regular FNameEntry strings
	};

	struct FProfile
	{
		ENameCodec NameCodec = ENameCodec::DfV1;
		uint32_t NameLengthShift = 6;

		uint64_t NamesRVA = 0x0;
		uint64_t ObjectsRVA = 0x0;

		/* FNamePool */
		uint32_t PoolBlocks = 0xC8;
		uint32_t PoolCursor = 0x100C8;
		uint32_t PoolCurrentBlock = 0x100CC;
		uint32_t PoolBlockBits = 18;
		uint32_t NameStride = 2;

		/* FUObjectArray -> TUObjectArray (ObjObjects) */
		uint32_t ObjectArray = 0x10;
		uint32_t ObjectsChunks = 0x10;
		uint32_t ObjectsCount = 0x4;
		uint32_t ObjectsNumChunks = 0x8;
		uint32_t ChunkSize = 0x10000;
		uint32_t ItemSize = 0x18;
		uint32_t ItemObject = 0x0;

		/* UObject */
		uint32_t ObjectClass = 0x08;
		uint32_t ObjectOuter = 0x10;
		uint32_t ObjectName = 0x1C;
		uint32_t ObjectIndex = 0x24;

		/* UField / UStruct / UFunction / UEnum */
		uint32_t FieldNext = 0x28;
		uint32_t StructSize = 0x3C;
		uint32_t StructSuper = 0x40;
		uint32_t StructAlignment = 0x48;
		uint32_t StructChildren = 0x50;
		uint32_t StructProperties = 0x68;
		uint32_t FunctionNumParams = 0xB0;
		uint32_t FunctionParamsSize = 0xB2;
		uint32_t FunctionFlags = 0xB8;
		uint32_t FunctionNative = 0xD8;
		uint32_t EnumNames = 0x40;

		/* FField / FFieldClass */
		uint32_t FFieldOwner = 0x08;
		uint32_t FFieldNext = 0x18;
		uint32_t FFieldClass = 0x20;
		uint32_t FFieldName = 0x28;
		uint32_t FFieldClassName = 0x00;

		/* FProperty */
		uint32_t PropertyDim = 0x38;
		uint32_t PropertySize = 0x3C;
		uint32_t PropertyFlags = 0x40;
		uint32_t PropertyOffset = 0x4C;
		uint32_t PropertyBaseSize = 0x80;
		uint32_t PropertyPayload = 0x88;         // Struct/Object/Byte(Enum)/Array(Inner)
		uint32_t EnumPropertyUnderlying = 0x80;  // FEnumProperty::UnderlyingProp, Enum follows (+0x8)
		uint32_t BoolPayload = 0x81;             // FieldSize, ByteOffset, ByteMask, FieldMask
		uint32_t MapKey = 0x88;                  // ValueProp follows (+0x8)
		uint32_t SetElement = 0x80;

		uint32_t MaxObjects = 4000000;
		uint32_t MaxFields = 65536;
	};

	/* Layout seed of the reference dumper (globals are discovered, never taken from here). */
	FProfile SeedProfile();

	/* True if the Delta Force executable is loaded in this process. */
	bool IsDeltaForceProcess();

	/*
	* Discovers GNames/GObjects, validates the layout and initializes ObjectArray, FName/NameArray, all Off:: offsets,
	* PropertySizes and the InSDK offsets. Returns false (with an error in the console) instead of crashing.
	*/
	bool InitEngineCore();

	const FProfile& GetProfile();

	/* Delta Force FNameEntry obfuscation (in-place). No-op for empty strings or strings starting with '\0'. */
	void DecryptAnsi(uint8_t* Text, size_t Length);
	void DecryptWide(uint16_t* Text, size_t Length);

	/* C++ source of the name decryption, emitted into the generated SDK. */
	const char* GetDecryptionSource();
}
