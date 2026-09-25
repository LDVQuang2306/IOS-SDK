
#include <iostream>
#include <fstream>
#include <format.h>
#include <filesystem>
#include <unistd.h>

#include "Unreal/ObjectArray.h"
#include "OffsetFinder/Offsets.h"
#include "Utils.h"
#include "Menu/Logger.h"


namespace fs = std::filesystem;

constexpr inline std::array FFixedUObjectArrayLayouts =
{
	FFixedUObjectArrayLayout // Default UE4.11 - UE4.20
	{
		.ObjectsOffset = 0x0,
		.MaxObjectsOffset = 0x8,
		.NumObjectsOffset = 0xC
	}
};

constexpr inline std::array FChunkedFixedUObjectArrayLayouts =
{
	FChunkedFixedUObjectArrayLayout // Default UE4.21 and above
	{
		.ObjectsOffset = 0x00,
		.MaxElementsOffset = 0x10,
		.NumElementsOffset = 0x14,
		.MaxChunksOffset = 0x18,
		.NumChunksOffset = 0x1C,
	},
	FChunkedFixedUObjectArrayLayout // Back4Blood
	{
		.ObjectsOffset = 0x10, // last
		.MaxElementsOffset = 0x00,
		.NumElementsOffset = 0x04,
		.MaxChunksOffset = 0x08,
		.NumChunksOffset = 0x0C,
	},
	FChunkedFixedUObjectArrayLayout // DeltaForce (layout from upstream iOS-Dumper-7)
	{
		.ObjectsOffset = 0x20,
		.MaxElementsOffset = 0x10,
		.NumElementsOffset = 0x04,
		.MaxChunksOffset = 0x00,
		.NumChunksOffset = 0x14,
	},
	FChunkedFixedUObjectArrayLayout // DeltaForce (layout previously configured in this repository)
	{
		.ObjectsOffset = 0x20,
		.MaxElementsOffset = 0x10,
		.NumElementsOffset = 0x14,
		.MaxChunksOffset = 0x00,
		.NumChunksOffset = 0x18,
	},
	FChunkedFixedUObjectArrayLayout // Mutliversus
	{
		.ObjectsOffset = 0x18,
		.MaxElementsOffset = 0x10,
		.NumElementsOffset = 0x00, // first
		.MaxChunksOffset = 0x14,
		.NumChunksOffset = 0x20,
	},
};

/* Bytes read around a candidate address while validating a layout (largest field offset + slack). */
constexpr int32 GObjectsCandidateReadSize = 0x50;

/* Upper bounds, generous enough for huge games but small enough to reject random data. */
constexpr int32 MaxPlausibleNumElements = 0x4000000;
constexpr int32 MaxPlausibleNumChunks = 0x800;

namespace
{
	inline bool IsPlausibleHeapPointer(uintptr_t Value)
	{
		return Value != 0 && (Value & 0x7) == 0 && Value >= GetLowestMappableAddress() && (Value >> 47) == 0;
	}

	/* A UObject starts with a vtable pointer that points to readable, 8-byte aligned memory. */
	inline bool IsPlausibleUObject(const void* Obj)
	{
		const uintptr_t Address = reinterpret_cast<uintptr_t>(Obj);

		if (!IsPlausibleHeapPointer(Address) || IsBadReadPtr(Obj))
			return false;

		const uintptr_t Vft = *reinterpret_cast<const uintptr_t*>(Address);
		return IsPlausibleHeapPointer(Vft) && !IsBadReadPtr(Vft);
	}

	struct FItemLayout
	{
		int32 ObjectOffset = -1;
		int32 Stride = -1;
		int32 IndexOffset = -1;
		int32 Score = 0;
	};

	/*
	* Detects sizeof(FUObjectItem), the offset of FUObjectItem::Object and the offset of UObjectBase::InternalIndex by looking at
	* the first items of the array: item k must hold a UObject whose InternalIndex is k. Games that add members to FUObjectItem
	* (larger stride) or shuffle UObjectBase are handled as long as the index is stored as a plain int32.
	*/
	FItemLayout DetectItemLayout(const uint8* FirstItem)
	{
		constexpr int32 NumItemsToCheck = 24;
		constexpr int32 PossibleStrides[] = { 0x10, 0x14, 0x18, 0x1C, 0x20, 0x28, 0x30, 0x38, 0x40 };
		constexpr int32 PossibleObjectOffsets[] = { 0x0, 0x4, 0x8, 0xC, 0x10 };

		FItemLayout Best;

		if (!FirstItem || IsBadReadRange(FirstItem, 0x40 * (NumItemsToCheck + 1)))
			return Best;

		for (const int32 Stride : PossibleStrides)
		{
			for (const int32 ObjectOffset : PossibleObjectOffsets)
			{
				if ((ObjectOffset + static_cast<int32>(sizeof(void*))) > Stride)
					continue;

				int32 NumValidObjects = 0;
				int32 IndexVotes[0x20 / 4] = { 0 }; // InternalIndex candidates 0x08 ... 0x24

				for (int32 k = 1; k <= NumItemsToCheck; k++)
				{
					const uint8* Obj = *reinterpret_cast<uint8* const*>(FirstItem + ObjectOffset + (k * Stride));

					if (!IsPlausibleUObject(Obj) || IsBadReadRange(Obj, 0x28))
						continue;

					NumValidObjects++;

					for (int32 i = 0; i < static_cast<int32>(sizeof(IndexVotes) / sizeof(IndexVotes[0])); i++)
					{
						if (*reinterpret_cast<const int32*>(Obj + 0x8 + (i * 4)) == k)
							IndexVotes[i]++;
					}
				}

				int32 BestVoteIdx = 0;
				for (int32 i = 1; i < static_cast<int32>(sizeof(IndexVotes) / sizeof(IndexVotes[0])); i++)
				{
					if (IndexVotes[i] > IndexVotes[BestVoteIdx])
						BestVoteIdx = i;
				}

				/* The InternalIndex match is a strong hint but not required, some games obfuscate it. */
				const bool bHasIndex = IndexVotes[BestVoteIdx] >= (NumItemsToCheck / 2);
				const int32 Score = (NumValidObjects * 2) + (bHasIndex ? IndexVotes[BestVoteIdx] : 0);

				if (NumValidObjects >= (NumItemsToCheck * 2 / 3) && Score > Best.Score)
				{
					Best.ObjectOffset = ObjectOffset;
					Best.Stride = Stride;
					Best.IndexOffset = bHasIndex ? 0x8 + (BestVoteIdx * 4) : -1;
					Best.Score = Score;
				}
			}
		}

		return Best;
	}

	/* Default FUObjectItem layout detection (upstream heuristic), used when DetectItemLayout finds nothing. */
	bool LegacyDetectItemLayout(const uint8* FirstItemPtr, uint32& OutObjectOffset, uint32& OutStride)
	{
		if (!FirstItemPtr || IsBadReadRange(FirstItemPtr, 0x78))
			return false;

		bool bFoundOffset = false;
		for (int i = 0x0; i < 0x10; i += 4)
		{
			if (!IsBadReadPtr(*reinterpret_cast<uint8* const*>(FirstItemPtr + i)))
			{
				OutObjectOffset = i;
				bFoundOffset = true;
				break;
			}
		}

		if (!bFoundOffset)
			return false;

		for (int i = OutObjectOffset + 0x8; i <= 0x38; i += 4)
		{
			void* SecondObject = *reinterpret_cast<uint8* const*>(FirstItemPtr + i);
			void* ThirdObject  = *reinterpret_cast<uint8* const*>(FirstItemPtr + (i * 2) - OutObjectOffset);

			if (IsPlausibleUObject(SecondObject) && IsPlausibleUObject(ThirdObject))
			{
				OutStride = i - OutObjectOffset;
				return true;
			}
		}

		return false;
	}
}

bool IsAddressValidGObjects(const uintptr Address, const FFixedUObjectArrayLayout& Layout)
{
	void* Objects = *reinterpret_cast<void**>(Address + Layout.ObjectsOffset);
	const int32 MaxElements = *reinterpret_cast<const int32*>(Address + Layout.MaxObjectsOffset);
	const int32 NumElements = *reinterpret_cast<const int32*>(Address + Layout.NumObjectsOffset);

	if (NumElements > MaxElements)
		return false;

	if (MaxElements > 0x400000)
		return false;

	if (NumElements < 0x1000)
		return false;

	uint8* ObjectsButDecrypted = ObjectArray::DecryptPtr(Objects);

	if (!IsPlausibleHeapPointer(reinterpret_cast<uintptr_t>(ObjectsButDecrypted)) || IsBadReadPtr(ObjectsButDecrypted))
		return false;

	/* It is assumed that the FUObjectItem layout is { UObject*, int32, int32, int32 } for games using FFixedUObjectArray. */
	constexpr int32 AssumedItemSize = 0x18;

	if (IsBadReadRange(ObjectsButDecrypted, AssumedItemSize * 6))
		return false;

	const uint8* FifthObject = *reinterpret_cast<const uint8* const*>(ObjectsButDecrypted + (AssumedItemSize * 5));

	if (!IsPlausibleUObject(FifthObject) || IsBadReadRange(FifthObject, 0x10))
		return false;

	if (*reinterpret_cast<const int32*>(FifthObject + 0xC) != 0x5)
		return false;

	LogInfo("FFixedUObjectArray candidate at 0x%lX", static_cast<unsigned long>(Address));
	return true;
}

bool IsAddressValidGObjects(const uintptr Address, const FChunkedFixedUObjectArrayLayout& Layout)
{
	void* Objects = *reinterpret_cast<void**>(Address + Layout.ObjectsOffset);
	const int32 MaxElements = *reinterpret_cast<const int32*>(Address + Layout.MaxElementsOffset);
	const int32 NumElements = *reinterpret_cast<const int32*>(Address + Layout.NumElementsOffset);
	const int32 MaxChunks   = *reinterpret_cast<const int32*>(Address + Layout.MaxChunksOffset);
	const int32 NumChunks   = *reinterpret_cast<const int32*>(Address + Layout.NumChunksOffset);

	if (NumChunks > 0x40 || NumChunks < 0x1)
		return false;

	if (MaxChunks > MaxPlausibleNumChunks || MaxChunks < 0x1)
		return false;

	if (NumElements < 0x1000 || NumElements > MaxElements || NumChunks > MaxChunks)
		return false;

	/* NumChunks is ceil(NumElements / ElementsPerChunk), or MaxChunks if all chunks were pre-allocated. Two chunk-sizes (0x10000, 0x10400) exist. */
	auto NumChunksFits = [&](int32 PerChunk) -> bool
	{
		return NumChunks == ((NumElements + PerChunk - 1) / PerChunk) || NumChunks == ((NumElements / PerChunk) + 1) || NumChunks == MaxChunks;
	};

	if (!NumChunksFits(0x10000) && !NumChunksFits(0x10400))
		return false;

	/* MaxElements is MaxChunks * ElementsPerChunk */
	const bool bMaxChunksFitsMaxElements = (MaxElements / 0x10000) == MaxChunks || (MaxElements / 0x10400) == MaxChunks;

	if (!bMaxChunksFitsMaxElements)
		return false;

	void** ObjectsPtrButDecrypted = reinterpret_cast<void**>(ObjectArray::DecryptPtr(Objects));

	/* The chunk-pointer must always be valid (especially because it's already decrypted [if it was encrypted at all]) */
	if (!IsPlausibleHeapPointer(reinterpret_cast<uintptr_t>(ObjectsPtrButDecrypted)) || IsBadReadRange(ObjectsPtrButDecrypted, NumChunks * sizeof(void*)))
		return false;

	/* Check if every chunk-pointer is valid. */
	for (int i = 0; i < NumChunks; i++)
	{
		if (!IsPlausibleHeapPointer(reinterpret_cast<uintptr_t>(ObjectsPtrButDecrypted[i])) || IsBadReadPtr(ObjectsPtrButDecrypted[i]))
			return false;
	}

	LogInfo("FChunkedFixedUObjectArray candidate at 0x%lX", static_cast<unsigned long>(Address));
	return true;
}

/*
* Layout-agnostic detection of FChunkedFixedUObjectArray.
*
* Tencent titles (Delta Force) reorder the members of FChunkedFixedUObjectArray between client builds, so a list of
* hard-coded layouts goes stale. Starting at a pointer to the chunk table, look for the four int32 members around it
* by their relationships instead:
*   MaxElements == MaxChunks * ElementsPerChunk
*   NumElements <= MaxElements
*   NumChunks   == ceil(NumElements / ElementsPerChunk)   (or MaxChunks if all chunks are pre-allocated)
*/
static bool TryDeriveChunkedLayout(uintptr_t PtrSlot, uintptr_t SegStart, uintptr_t SegEnd, uintptr_t& OutBase, FChunkedFixedUObjectArrayLayout& OutLayout)
{
	constexpr uintptr_t Window = 0x28;

	struct FIntField { uintptr_t Address; int32 Value; };

	FIntField Fields[40];
	int32 NumFields = 0;

	const uintptr_t WindowStart = (PtrSlot - SegStart) > Window ? (PtrSlot - Window) : SegStart;
	const uintptr_t WindowEnd = (SegEnd - (PtrSlot + 8)) > Window ? (PtrSlot + 8 + Window) : SegEnd;

	for (uintptr_t Address = WindowStart; (Address + 4) <= WindowEnd && NumFields < 40; Address += 4)
	{
		if (Address >= PtrSlot && Address < (PtrSlot + 8))
			continue;

		Fields[NumFields++] = { Address, *reinterpret_cast<const int32*>(Address) };
	}

	for (int32 PerChunk : { 0x10000, 0x10400 })
	{
		for (int32 m = 0; m < NumFields; m++)
		{
			const int32 MaxElements = Fields[m].Value;

			if (MaxElements < PerChunk || (MaxElements % PerChunk) != 0 || MaxElements > (MaxPlausibleNumChunks * PerChunk))
				continue;

			const int32 MaxChunks = MaxElements / PerChunk;

			for (int32 mc = 0; mc < NumFields; mc++)
			{
				if (mc == m || Fields[mc].Value != MaxChunks)
					continue;

				for (int32 n = 0; n < NumFields; n++)
				{
					const int32 NumElements = Fields[n].Value;

					if (n == m || n == mc || NumElements < 0x1000 || NumElements > MaxElements)
						continue;

					for (int32 nc = 0; nc < NumFields; nc++)
					{
						if (nc == m || nc == mc || nc == n)
							continue;

						const int32 NumChunks = Fields[nc].Value;
						const bool bNumChunksFits = NumChunks == ((NumElements + PerChunk - 1) / PerChunk) || NumChunks == ((NumElements / PerChunk) + 1) || NumChunks == MaxChunks;

						if (NumChunks < 1 || NumChunks > MaxChunks || !bNumChunksFits)
							continue;

						const uintptr_t Addresses[] = { PtrSlot, Fields[m].Address, Fields[mc].Address, Fields[n].Address, Fields[nc].Address };
						uintptr_t Base = PtrSlot;
						for (const uintptr_t A : Addresses)
							Base = A < Base ? A : Base;

						OutBase = Base;
						OutLayout = FChunkedFixedUObjectArrayLayout{
							.ObjectsOffset = static_cast<int32>(PtrSlot - Base),
							.MaxElementsOffset = static_cast<int32>(Fields[m].Address - Base),
							.NumElementsOffset = static_cast<int32>(Fields[n].Address - Base),
							.MaxChunksOffset = static_cast<int32>(Fields[mc].Address - Base),
							.NumChunksOffset = static_cast<int32>(Fields[nc].Address - Base),
						};

						if (IsAddressValidGObjects(Base, OutLayout))
							return true;
					}
				}
			}
		}
	}

	return false;
}

void ObjectArray::InitializeFUObjectItem(uint8* FirstItemPtr)
{
	LogInfo("Initializing FUObjectItem...");

	const FItemLayout Layout = DetectItemLayout(FirstItemPtr);

	if (Layout.Stride > 0)
	{
		FUObjectItemInitialOffset = Layout.ObjectOffset;
		SizeOfFUObjectItem = Layout.Stride;
		ObjectIndexOffset = Layout.IndexOffset;
	}
	else
	{
		uint32 ObjectOffset = 0x0;
		uint32 Stride = 0x18;

		if (LegacyDetectItemLayout(FirstItemPtr, ObjectOffset, Stride))
		{
			FUObjectItemInitialOffset = ObjectOffset;
			SizeOfFUObjectItem = Stride;
		}
		else
		{
			LogError("FUObjectItem layout couldn't be detected, assuming { UObject*, int32, int32, int32 }");
			FUObjectItemInitialOffset = 0x0;
			SizeOfFUObjectItem = 0x18;
		}
		ObjectIndexOffset = -1;
	}

	Off::InSDK::ObjArray::FUObjectItemInitialOffset = FUObjectItemInitialOffset;
	Off::InSDK::ObjArray::FUObjectItemSize = SizeOfFUObjectItem;
	LogSuccess("FUObjectItem initialized (Offset: 0x%X, Size: 0x%X, UObject::Index at %d)", FUObjectItemInitialOffset, SizeOfFUObjectItem, ObjectIndexOffset);
}

void ObjectArray::InitDecryption(uint8* (*DecryptionFunction)(void* ObjPtr), const char* DecryptionLambdaAsStr)
{
	LogInfo("Initializing decryption: %s", DecryptionLambdaAsStr);
	DecryptPtr = DecryptionFunction;
	DecryptionLambdaStr = DecryptionLambdaAsStr;
	LogSuccess("Decryption initialized");
}

void ObjectArray::InitializeChunkSize(uint8* ChunksPtr)
{
	LogInfo("Initializing chunk size...");

	/* The chunk size is only observable once there are more elements than fit into one 0x10000-chunk. */
	const int32 IndexToCheck = 0x10400;

	/* MaxElements is MaxChunks * ElementsPerChunk */
	const int32 MaxElements = *reinterpret_cast<int32*>(GObjects + Off::FUObjectArray::ChunkedFixedLayout.MaxElementsOffset);
	if ((MaxElements % 0x10000) != 0 && (MaxElements % 0x10400) == 0)
		NumElementsPerChunk = 0x10400;

	if (ObjectArray::Num() > IndexToCheck && ObjectIndexOffset >= 0)
	{
		const uint8* Obj = static_cast<const uint8*>(ByIndex(ChunksPtr, IndexToCheck, SizeOfFUObjectItem, FUObjectItemInitialOffset, 0x10000));

		if (IsPlausibleUObject(Obj) && !IsBadReadRange(Obj, ObjectIndexOffset + sizeof(int32)))
		{
			const bool bHasBiggerChunkSize = (*reinterpret_cast<const int32*>(Obj + ObjectIndexOffset) != IndexToCheck);
			NumElementsPerChunk = bHasBiggerChunkSize ? 0x10400 : 0x10000;
		}
	}

	Off::InSDK::ObjArray::ChunkSize = NumElementsPerChunk;
	LogSuccess("Chunk size initialized: 0x%X", NumElementsPerChunk);
}

/* UObject::ClassPrivate chains end in UClass, whose class is itself. Finds such a member offset for the given objects. */
static bool HasSelfReferencingClassChain(const std::vector<const uint8*>& Objects)
{
	for (int32 ClassOffset = 0x8; ClassOffset <= 0x28; ClassOffset += 0x8)
	{
		int32 NumValid = 0;

		for (const uint8* Obj : Objects)
		{
			const uint8* Current = Obj;

			for (int32 Hop = 0; Hop < 4 && Current; Hop++)
			{
				if (IsBadReadRange(Current, ClassOffset + sizeof(void*)))
				{
					Current = nullptr;
					break;
				}

				const uint8* Next = *reinterpret_cast<const uint8* const*>(Current + ClassOffset);

				if (Next == Current)
				{
					NumValid++;
					break;
				}

				Current = IsPlausibleUObject(Next) ? Next : nullptr;
			}
		}

		if (NumValid == static_cast<int32>(Objects.size()))
			return true;
	}

	return false;
}

/* Samples objects over the whole array. They must be UObjects whose InternalIndex matches (if known) and whose class chain is sane. */
bool ObjectArray::ValidateObjects()
{
	const int32 NumElements = Num();

	if (NumElements < 0x100)
	{
		LogError("ObjectArray validation failed: only %d elements", NumElements);
		return false;
	}

	constexpr int32 NumSamples = 64;

	int32 NumNonNull = 0;
	int32 NumPlausible = 0;
	int32 NumMatches = 0;
	std::vector<const uint8*> ChainSamples;

	for (int32 s = 0; s < NumSamples; s++)
	{
		const int32 Index = static_cast<int32>((static_cast<int64>(NumElements - 1) * s) / (NumSamples - 1));

		const uint8* Obj = static_cast<const uint8*>(GetByIndex(Index).GetAddress());
		if (!Obj)
			continue;

		NumNonNull++;

		if (!IsPlausibleUObject(Obj) || IsBadReadRange(Obj, 0x30))
			continue;

		NumPlausible++;

		if (ChainSamples.size() < 8)
			ChainSamples.push_back(Obj);

		if (ObjectIndexOffset >= 0 && *reinterpret_cast<const int32*>(Obj + ObjectIndexOffset) == Index)
			NumMatches++;
	}

	if (NumNonNull < (NumSamples / 4) || NumPlausible < ((NumNonNull * 8) / 10))
	{
		LogError("ObjectArray validation failed: %d/%d sampled objects look like UObjects", NumPlausible, NumNonNull);
		return false;
	}

	if (ObjectIndexOffset >= 0 && NumMatches >= ((NumNonNull * 8) / 10))
		return true;

	/* No (matching) InternalIndex: fall back to the structural check */
	if (HasSelfReferencingClassChain(ChainSamples))
	{
		LogInfo("ObjectArray validated by the UObject class chain (%d/%d InternalIndex matches)", NumMatches, NumNonNull);
		return true;
	}

	LogError("ObjectArray validation failed: %d/%d InternalIndex matches and no valid class chain", NumMatches, NumNonNull);
	return false;
}

bool ObjectArray::CommitAndValidate(uint8* Address, uintptr_t ImageBase, bool bIsChunked, int32 ElementsPerChunk, bool bDetectItemLayout)
{
	GObjects = Address;
	Off::InSDK::ObjArray::GObjects = static_cast<int32>(reinterpret_cast<uintptr_t>(Address) - ImageBase);

	if (!bIsChunked)
	{
		NumElementsPerChunk = -1;

		ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
		{
			if (Index < 0 || Index >= Num())
				return nullptr;

			uint8* ItemsPtr = DecryptPtr(*reinterpret_cast<uint8**>(ObjectsArray));
			if (!ItemsPtr)
				return nullptr;

			return *reinterpret_cast<void**>(ItemsPtr + FUObjectItemOffset + (Index * FUObjectItemSize));
		};

		/* For FFixedUObjectArray 'Objects' points directly at the first FUObjectItem. */
		uint8* ItemsPtr = DecryptPtr(*reinterpret_cast<uint8**>(GObjects + Off::FUObjectArray::GetObjectsOffset()));

		if (bDetectItemLayout)
			ObjectArray::InitializeFUObjectItem(ItemsPtr);
	}
	else
	{
		NumElementsPerChunk = ElementsPerChunk > 0 ? ElementsPerChunk : 0x10000;

		ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
		{
			if (Index < 0 || Index >= Num() || PerChunk == 0)
				return nullptr;

			const int32 ChunkIndex = Index / PerChunk;
			const int32 InChunkIdx = Index % PerChunk;

			/* Never index past the allocated chunk-pointers, even if NumElements is momentarily out of sync. */
			if (ChunkIndex >= *reinterpret_cast<int32*>(GObjects + Off::FUObjectArray::ChunkedFixedLayout.NumChunksOffset))
				return nullptr;

			uint8** ChunkTable = reinterpret_cast<uint8**>(DecryptPtr(*reinterpret_cast<uint8**>(ObjectsArray)));
			if (!ChunkTable)
				return nullptr;

			uint8* Chunk = ChunkTable[ChunkIndex];
			if (!Chunk)
				return nullptr;

			return *reinterpret_cast<void**>(Chunk + FUObjectItemOffset + (InChunkIdx * FUObjectItemSize));
		};

		uint8** ChunkTable = reinterpret_cast<uint8**>(DecryptPtr(*reinterpret_cast<uint8**>(GObjects + Off::FUObjectArray::GetObjectsOffset())));

		if (bDetectItemLayout)
			ObjectArray::InitializeFUObjectItem(ChunkTable ? ChunkTable[0] : nullptr);

		if (ElementsPerChunk <= 0)
			ObjectArray::InitializeChunkSize(GObjects + Off::FUObjectArray::GetObjectsOffset());

		Off::InSDK::ObjArray::ChunkSize = NumElementsPerChunk;
	}

	if (ValidateObjects())
		return true;

	GObjects = nullptr;
	ByIndex = nullptr;
	Off::InSDK::ObjArray::GObjects = 0x0;
	return false;
}

/* We don't speak about this function... */
bool ObjectArray::Init(bool bScanAllMemory, const char* const ModuleName)
{
	LogInfo("\nDumper-7 by me, you & him\n\n\n");

	/* Never keep a previous (possibly stale) result if this search fails */
	GObjects = nullptr;
	ByIndex = nullptr;

	const auto [ImageBase, ImageSize, Header, Slide] = GetImageBaseAndSize(ModuleName);

	if (!Header)
	{
		LogError("ObjectArray::Init: module '%s' is not loaded", ModuleName ? ModuleName : "<main executable>");
		return false;
	}

	/* GUObjectArray is a global variable -> it lives in a writable data segment (__DATA / __DATA_DIRTY / __common / __bss), never in __TEXT. */
	const std::vector<MachSegmentInfo> Segments = bScanAllMemory ? GetImageSegments(ModuleName) : GetWritableImageSegments(ModuleName);

	LogInfo("Searching for GObjects in %d segment(s)...", static_cast<int32>(Segments.size()));

	/* Pass 1: known layouts. */
	for (const MachSegmentInfo& Seg : Segments)
	{
		if (Seg.Size <= GObjectsCandidateReadSize)
			continue;

		const uintptr_t SegEnd = Seg.Start + Seg.Size - GObjectsCandidateReadSize;

		for (uintptr_t CurrentAddress = Seg.Start; CurrentAddress < SegEnd; CurrentAddress += 0x4)
		{
			for (const FFixedUObjectArrayLayout& Layout : FFixedUObjectArrayLayouts)
			{
				if (!IsAddressValidGObjects(CurrentAddress, Layout))
					continue;

				Off::FUObjectArray::bIsChunked = false;
				Off::FUObjectArray::FixedLayout = Layout;

				if (CommitAndValidate(reinterpret_cast<uint8*>(CurrentAddress), ImageBase, false, -1, true))
				{
					LogSuccess("Found FFixedUObjectArray GObjects at offset 0x%X", Off::InSDK::ObjArray::GObjects);
					return true;
				}
			}

			for (const FChunkedFixedUObjectArrayLayout& Layout : FChunkedFixedUObjectArrayLayouts)
			{
				if (!IsAddressValidGObjects(CurrentAddress, Layout))
					continue;

				Off::FUObjectArray::bIsChunked = true;
				Off::FUObjectArray::ChunkedFixedLayout = Layout;

				if (CommitAndValidate(reinterpret_cast<uint8*>(CurrentAddress), ImageBase, true, -1, true))
				{
					LogSuccess("Found FChunkedFixedUObjectArray GObjects at offset 0x%X", Off::InSDK::ObjArray::GObjects);
					return true;
				}
			}
		}
	}

	/* Pass 2: unknown member order (e.g. a newer Delta Force build). */
	LogInfo("No known GObjects layout matched, trying layout-agnostic detection...");

	for (const MachSegmentInfo& Seg : Segments)
	{
		if (Seg.Size <= GObjectsCandidateReadSize)
			continue;

		const uintptr_t SegEnd = Seg.Start + Seg.Size - sizeof(void*);

		for (uintptr_t PtrSlot = Seg.Start; PtrSlot < SegEnd; PtrSlot += sizeof(void*))
		{
			const uintptr_t Value = reinterpret_cast<uintptr_t>(DecryptPtr(*reinterpret_cast<void**>(PtrSlot)));

			/* The chunk table is heap memory, never part of the image itself. */
			if (!IsPlausibleHeapPointer(Value) || (Value >= ImageBase && Value < (ImageBase + ImageSize)))
				continue;

			uintptr_t Base = 0x0;
			FChunkedFixedUObjectArrayLayout Layout;

			if (!TryDeriveChunkedLayout(PtrSlot, Seg.Start, Seg.Start + Seg.Size, Base, Layout))
				continue;

			Off::FUObjectArray::bIsChunked = true;
			Off::FUObjectArray::ChunkedFixedLayout = Layout;

			if (CommitAndValidate(reinterpret_cast<uint8*>(Base), ImageBase, true, -1, true))
			{
				LogSuccess("Found FChunkedFixedUObjectArray GObjects at offset 0x%X with layout { Objects=0x%X, MaxElements=0x%X, NumElements=0x%X, MaxChunks=0x%X, NumChunks=0x%X }",
					Off::InSDK::ObjArray::GObjects, Layout.ObjectsOffset, Layout.MaxElementsOffset, Layout.NumElementsOffset, Layout.MaxChunksOffset, Layout.NumChunksOffset);
				return true;
			}
		}
	}

	if (!bScanAllMemory)
	{
		LogInfo("Retrying with all segments of the image...");
		return ObjectArray::Init(true, ModuleName);
	}

	LogError("GObjects couldn't be found! Set a manual override in Generator::InitEngineCore().");
	return false;
}

bool ObjectArray::Init(int32 GObjectsOffset, const FFixedUObjectArrayLayout& ObjectArrayLayout, const char* const ModuleName)
{
	GObjects = nullptr;
	ByIndex = nullptr;

	LogInfo("Initializing ObjectArray with FFixedUObjectArray at offset 0x%X", GObjectsOffset);

	const uintptr_t ImageBase = GetModuleBase(ModuleName);
	if (!ImageBase || GObjectsOffset <= 0)
	{
		LogError("ObjectArray override: invalid module/offset");
		return false;
	}

	uint8* Address = reinterpret_cast<uint8*>(ImageBase + GObjectsOffset);

	Off::FUObjectArray::bIsChunked = false;
	Off::FUObjectArray::FixedLayout = ObjectArrayLayout.IsValid() ? ObjectArrayLayout : FFixedUObjectArrayLayouts[0];

	if (IsBadReadRange(Address, GObjectsCandidateReadSize) || !IsAddressValidGObjects(reinterpret_cast<uintptr_t>(Address), Off::FUObjectArray::FixedLayout))
	{
		LogError("ObjectArray override at 0x%X doesn't look like a FFixedUObjectArray (outdated offset?)", GObjectsOffset);
		return false;
	}

	if (!CommitAndValidate(Address, ImageBase, false, -1, true))
		return false;

	LogSuccess("FFixedUObjectArray initialized successfully");
	return true;
}

bool ObjectArray::Init(int32 GObjectsOffset, int32 ElementsPerChunk, const FChunkedFixedUObjectArrayLayout& ObjectArrayLayout, const char* const ModuleName)
{
	GObjects = nullptr;
	ByIndex = nullptr;

	LogInfo("Initializing ObjectArray with FChunkedFixedUObjectArray at offset 0x%X", GObjectsOffset);

	const uintptr_t ImageBase = GetModuleBase(ModuleName);
	if (!ImageBase || GObjectsOffset <= 0)
	{
		LogError("ObjectArray override: invalid module/offset");
		return false;
	}

	uint8* Address = reinterpret_cast<uint8*>(ImageBase + GObjectsOffset);

	Off::FUObjectArray::bIsChunked = true;
	Off::FUObjectArray::ChunkedFixedLayout = ObjectArrayLayout.IsValid() ? ObjectArrayLayout : FChunkedFixedUObjectArrayLayouts[0];

	if (IsBadReadRange(Address, GObjectsCandidateReadSize) || !IsAddressValidGObjects(reinterpret_cast<uintptr_t>(Address), Off::FUObjectArray::ChunkedFixedLayout))
	{
		LogError("ObjectArray override at 0x%X doesn't look like a FChunkedFixedUObjectArray (outdated offset or wrong layout?)", GObjectsOffset);
		return false;
	}

	if (!CommitAndValidate(Address, ImageBase, true, ElementsPerChunk, true))
		return false;

	LogSuccess("FChunkedFixedUObjectArray initialized successfully");
	return true;
}

void ObjectArray::DumpObjects(const fs::path& Path, bool bWithPathname)
{
	LogInfo("Dumping objects to %s...", (Path / "GObjects-Dump.txt").string().c_str());
	std::ofstream DumpStream(Path / "GObjects-Dump.txt");

	DumpStream << "Object dump by Dumper-7\n\n";
	DumpStream << (!Settings::Generator::GameVersion.empty() && !Settings::Generator::GameName.empty() ? (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName) + "\n\n" : "");
	DumpStream << "Count: " << Num() << "\n\n\n";

	for (auto Object : ObjectArray())
	{
        if (!Object.GetAddress())
            continue;
        
		if (!bWithPathname)
		{
			DumpStream << fmt::format("[{:08X}] {{{}}} {}\n", Object.GetIndex(), Object.GetAddress(), Object.GetFullName());
		}
		else
		{
			DumpStream << fmt::format("[{:08X}] {{{}}} {}\n", Object.GetIndex(), Object.GetAddress(), Object.GetPathName());
		}
	}

	DumpStream.close();
	LogSuccess("Objects dumped successfully to %s", (Path / "GObjects-Dump.txt").string().c_str());
}

void ObjectArray::DumpObjectsWithProperties(const fs::path& Path, bool bWithPathname)
{
	LogInfo("Dumping objects with properties to %s...", (Path / "GObjects-Dump-WithProperties.txt").string().c_str());
	
	std::ofstream DumpStream(Path / "GObjects-Dump-WithProperties.txt");

	DumpStream << "Object dump by Dumper-7\n\n";
	DumpStream << (!Settings::Generator::GameVersion.empty() && !Settings::Generator::GameName.empty() ? (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName) + "\n\n" : "");
	DumpStream << "Count: " << Num() << "\n\n\n";

	for (auto Object : ObjectArray())
	{
        if (!Object.GetAddress())
            continue;
		if (!bWithPathname)
		{
			DumpStream << fmt::format("[{:08X}] {{{}}} {}\n", Object.GetIndex(), Object.GetAddress(), Object.GetFullName());
		}
		else
		{
			DumpStream << fmt::format("[{:08X}] {{{}}} {}\n", Object.GetIndex(), Object.GetAddress(), Object.GetPathName());
		}

		if (Object.IsA(EClassCastFlags::Struct))
		{
			for (UEProperty Prop : Object.Cast<UEStruct>().GetProperties())
			{
				DumpStream << fmt::format("[{:08X}] {{{}}}\t{} {}\n", Prop.GetOffset(), Prop.GetAddress(), Prop.GetPropClassName(), Prop.GetName());
			}
		}
	}

	DumpStream.close();
	LogSuccess("Objects with properties dumped successfully to %s", (Path / "GObjects-Dump-WithProperties.txt").string().c_str());
}


int32 ObjectArray::Num()
{
	if (!GObjects)
		return 0;

	const int32 NumElements = *reinterpret_cast<int32*>(GObjects + Off::FUObjectArray::GetNumElementsOffset());
	return NumElements > 0 ? NumElements : 0;
}

template<typename UEType>
UEType ObjectArray::GetByIndex(int32 Index)
{
	if (!IsInitialized())
		return UEType(nullptr);

	return UEType(ByIndex(GObjects + Off::FUObjectArray::GetObjectsOffset(), Index, SizeOfFUObjectItem, FUObjectItemInitialOffset, NumElementsPerChunk));
}

template<typename UEType>
UEType ObjectArray::FindObject(const std::string& FullName, EClassCastFlags RequiredType)
{
	for (UEObject Object : ObjectArray())
	{
        if (!Object.GetAddress())
            continue;
        
		if (Object.IsA(RequiredType) && Object.GetFullName() == FullName)
		{
			return Object.Cast<UEType>();
		}
	}

	return UEType();
}

template<typename UEType>
UEType ObjectArray::FindObjectFast(const std::string& Name, EClassCastFlags RequiredType)
{
	for (UEObject Object : ObjectArray())
	{
		if (Object.IsA(RequiredType) && Object.GetName() == Name)
			return Object.Cast<UEType>();
	}

	LogInfo("FindObjectFast(\"%s\"): not found", Name.c_str());
	return UEType();
}

template<typename UEType>
UEType ObjectArray::FindObjectFastInOuter(const std::string& Name, std::string Outer)
{
	auto ObjArray = ObjectArray();

	for (UEObject Object : ObjArray)
	{
		if (Object.GetName() == Name && Object.GetOuter().GetName() == Outer)
		{
			return Object.Cast<UEType>();
		}
	}

	return UEType();
}

UEStruct ObjectArray::FindStruct(const std::string& Name)
{
	return FindObjectFast<UEClass>(Name, EClassCastFlags::Struct);
}

UEStruct ObjectArray::FindStructFast(const std::string& Name)
{
	return FindObjectFast<UEClass>(Name, EClassCastFlags::Struct);
}

UEClass ObjectArray::FindClass(const std::string& FullName)
{
	return FindObject<UEClass>(FullName, EClassCastFlags::Class);
}

UEClass ObjectArray::FindClassFast(const std::string& Name)
{
	return FindObjectFast<UEClass>(Name, EClassCastFlags::Class);
}

ObjectArray::ObjectsIterator ObjectArray::begin()
{
	return ObjectsIterator();
}
ObjectArray::ObjectsIterator ObjectArray::end()
{
	return ObjectsIterator(Num());
}


ObjectArray::ObjectsIterator::ObjectsIterator(int32 StartIndex)
	: CurrentObject(nullptr), CurrentIndex(StartIndex)
{
	const int32 NumElements = ObjectArray::Num();

	if (CurrentIndex >= NumElements)
		return;

	CurrentObject = ObjectArray::GetByIndex(CurrentIndex);

	/* Never hand out an invalid (null) object from begin(), skip ahead like operator++ does */
	if (!CurrentObject)
		++(*this);
}

UEObject ObjectArray::ObjectsIterator::operator*()
{
	return CurrentObject;
}

ObjectArray::ObjectsIterator& ObjectArray::ObjectsIterator::operator++()
{
	const int32 NumElements = ObjectArray::Num();

	do
	{
		++CurrentIndex;
		CurrentObject = CurrentIndex < NumElements ? ObjectArray::GetByIndex(CurrentIndex) : UEObject(nullptr);
	}
	while (!CurrentObject && CurrentIndex < NumElements);

	return *this;
}

bool ObjectArray::ObjectsIterator::operator!=(const ObjectsIterator& Other) const
{
	/* '<' instead of '!=': objects are created while we iterate, so the end index captured by end() can be skipped over. */
	return CurrentIndex < Other.CurrentIndex;
}

int32 ObjectArray::ObjectsIterator::GetIndex() const
{
	return CurrentIndex;
}

/*
* The compiler won't generate functions for a specific template type unless it's used in the .cpp file corresponding to the
* header it was declatred in.
*
* See https://stackoverflow.com/questions/456713/why-do-i-get-unresolved-external-symbol-errors-when-using-templates
*/
/*
* Explicit instantiations: the templates are defined in this .cpp but used from other translation units.
* Implicit instantiations inside a dummy function are not guaranteed to be emitted as linkable symbols
* once the optimizer inlines them, so instantiate them explicitly.
*/
#define INSTANTIATE_OBJ_ARRAY_TEMPLATES(Type) \
	template Type ObjectArray::GetByIndex<Type>(int32); \
	template Type ObjectArray::FindObject<Type>(const std::string&, EClassCastFlags); \
	template Type ObjectArray::FindObjectFast<Type>(const std::string&, EClassCastFlags); \
	template Type ObjectArray::FindObjectFastInOuter<Type>(const std::string&, std::string);

INSTANTIATE_OBJ_ARRAY_TEMPLATES(UEObject)
INSTANTIATE_OBJ_ARRAY_TEMPLATES(UEField)
INSTANTIATE_OBJ_ARRAY_TEMPLATES(UEEnum)
INSTANTIATE_OBJ_ARRAY_TEMPLATES(UEStruct)
INSTANTIATE_OBJ_ARRAY_TEMPLATES(UEClass)
INSTANTIATE_OBJ_ARRAY_TEMPLATES(UEFunction)
INSTANTIATE_OBJ_ARRAY_TEMPLATES(UEProperty)
INSTANTIATE_OBJ_ARRAY_TEMPLATES(UEByteProperty)
INSTANTIATE_OBJ_ARRAY_TEMPLATES(UEBoolProperty)
INSTANTIATE_OBJ_ARRAY_TEMPLATES(UEObjectProperty)
INSTANTIATE_OBJ_ARRAY_TEMPLATES(UEClassProperty)
INSTANTIATE_OBJ_ARRAY_TEMPLATES(UEStructProperty)
INSTANTIATE_OBJ_ARRAY_TEMPLATES(UEArrayProperty)
INSTANTIATE_OBJ_ARRAY_TEMPLATES(UEMapProperty)
INSTANTIATE_OBJ_ARRAY_TEMPLATES(UESetProperty)
INSTANTIATE_OBJ_ARRAY_TEMPLATES(UEEnumProperty)

#undef INSTANTIATE_OBJ_ARRAY_TEMPLATES


bool AllFieldIterator::operator!=(const AllFieldIterator& Other) const
{
    return CurrentObject != Other.CurrentObject || PropertyIndex != Other.PropertyIndex;
}

AllFieldIterator& AllFieldIterator::operator++()
{
    if (CurrenStructHasMoreMembers())
    {
        PropertyIndex++;
        return *this;
    }
    IterateToNextStructWithMembers();
    return *this;
}

UEProperty AllFieldIterator::operator*() const
{
    return Fields[PropertyIndex];
}

void AllFieldIterator::IterateToNextStruct()
{
    if (IsEndIterator()) return;
    ++CurrentObject;
    while (CurrentObject != ObjectEndIterator && !IsCurrentObjectStruct())
        ++CurrentObject;
}

void AllFieldIterator::IterateToNextStructWithMembers()
{
    while (!CurrenStructHasMoreMembers())
    {
        IterateToNextStruct();
        PropertyIndex = 0;
        if (IsEndIterator()) return;
        Fields = GetCurrentStruct().GetProperties();
    }
}
