
#include <iostream>
#include <fstream>
#include <format.h> // fmt: std::format is unavailable for the iOS 14 deployment target
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
	FChunkedFixedUObjectArrayLayout // DeltaForce (only used if the Delta Force profile is bypassed)
	{
		.ObjectsOffset = 0x20,
		.MaxElementsOffset = 0x10,
		.NumElementsOffset = 0x4,
		.MaxChunksOffset = 0x0,
		.NumChunksOffset = 0x14,
	},
	FChunkedFixedUObjectArrayLayout // DeltaForce (current season, GUObjectArray::ObjObjects @0x10). Normally the Delta Force profile is used instead.
	{
		.ObjectsOffset = 0x20,
		.MaxElementsOffset = 0x10,
		.NumElementsOffset = 0x14,
		.MaxChunksOffset = 0x1C,
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

bool IsAddressValidGObjects(const uintptr Address, const FFixedUObjectArrayLayout& Layout)
{
	/* It is assumed that the FUObjectItem layout is constant amongst all games using FFixedUObjectArray for ObjObjects. */
	struct FUObjectItem
	{
		void* Object;
		uint8 Pad[0x10];
	};

	void* Objects = *reinterpret_cast<void**>(Address + Layout.ObjectsOffset);
	const int32 MaxElements = *reinterpret_cast<const int32*>(Address + Layout.MaxObjectsOffset);
	const int32 NumElements = *reinterpret_cast<const int32*>(Address + Layout.NumObjectsOffset);


	FUObjectItem* ObjectsButDecrypted = (FUObjectItem*)ObjectArray::DecryptPtr(Objects);

	if (NumElements > MaxElements)
        return false;

	if (MaxElements > 0x400000)
        return false;

	if (NumElements < 0x1000)
		return false;

	if (!IsPlausiblePointer(ObjectsButDecrypted))
        return false;

	/* Candidates are arbitrary data, read through the kernel */
	const uintptr FithObject = SafeRead<uintptr>(reinterpret_cast<uintptr>(&ObjectsButDecrypted[0x5].Object));

	if (!IsPlausiblePointer(FithObject))
        return false;

	const int32 IndexOfFithobject = SafeRead<int32>(FithObject + 0xC, -1);

	if (IndexOfFithobject != 0x5)
		return false;

	LogInfo("FFixedUObjectArray validation successful");
	return true;
}

bool IsAddressValidGObjects(const uintptr Address, const FChunkedFixedUObjectArrayLayout& Layout)
{
	void* Objects = *reinterpret_cast<void**>(Address + Layout.ObjectsOffset);
	const int32 MaxElements = *reinterpret_cast<const int32*>(Address + Layout.MaxElementsOffset);
	const int32 NumElements = *reinterpret_cast<const int32*>(Address + Layout.NumElementsOffset);
	const int32 MaxChunks   = *reinterpret_cast<const int32*>(Address + Layout.MaxChunksOffset);
	const int32 NumChunks   = *reinterpret_cast<const int32*>(Address + Layout.NumChunksOffset);

	void** ObjectsPtrButDecrypted = reinterpret_cast<void**>(ObjectArray::DecryptPtr(Objects));

	if (NumChunks > 0x14 || NumChunks < 0x1)
		return false;

	if (MaxChunks > 0x22F || MaxChunks < 0x6)
        return false;
	
	if (NumElements > MaxElements || NumChunks > MaxChunks)
		return false;
	
	/* There are never too many or too few chunks for all elements. Two different chunk-sizes (0x10000, 0x10400) occure on different UE versions and are checked for.*/
	const bool bNumChunksFitsNumElements = ((NumElements / 0x10000) + 1) == NumChunks || ((NumElements / 0x10400) + 1) == NumChunks;

	if (!bNumChunksFitsNumElements)
		return false;
	
	/* Same as above for the max number of elements/chunks. */
	const bool bMaxChunksFitsMaxElements = (MaxElements / 0x10000) == MaxChunks || (MaxElements / 0x10400) == MaxChunks;

	if (!bMaxChunksFitsMaxElements)
		return false;
	

	/* The chunk-pointer must always be valid (especially because it's already decrypted [if it was encrypted at all]) */
	if (!IsPlausiblePointer(ObjectsPtrButDecrypted) || IsBadReadPtr(ObjectsPtrButDecrypted))
		return false;
	

	/* Check if every chunk-pointer is valid. */
	for (int i = 0; i < NumChunks; i++)
	{
		void* Chunk = SafeRead<void*>(reinterpret_cast<uintptr>(ObjectsPtrButDecrypted + i));

		if (!IsPlausiblePointer(Chunk) || IsBadReadPtr(Chunk))
			return false;
	}
	
	LogInfo("FChunkedFixedUObjectArray validation successful");
	return true;
}

static void* ChunkedByIndexImpl(void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk, uint8_t* (*DecryptPtr)(void*), uint8* GObjects)
{
	if (Index < 0 || PerChunk == 0)
		return nullptr;

	const int32 ChunkIndex = Index / PerChunk;
	const int32 InChunkIdx = Index % PerChunk;

	/* Never index past the committed chunk table */
	if (Off::FUObjectArray::ChunkedFixedLayout.NumChunksOffset >= 0)
	{
		const int32 NumChunks = *reinterpret_cast<int32*>(GObjects + Off::FUObjectArray::ChunkedFixedLayout.NumChunksOffset);

		if (ChunkIndex >= NumChunks)
			return nullptr;
	}

	uint8** Chunks = reinterpret_cast<uint8**>(DecryptPtr(*reinterpret_cast<uint8**>(ObjectsArray)));
	if (!Chunks)
		return nullptr;

	uint8* Chunk = Chunks[ChunkIndex];
	if (!Chunk)
		return nullptr;

	return *reinterpret_cast<void**>(Chunk + (InChunkIdx * FUObjectItemSize) + FUObjectItemOffset);
}

void ObjectArray::InitializeFUObjectItem(uint8* FirstItemPtr)
{
    LogInfo("Initializing FUObjectItem...");
    
    for (int i = 0x0; i < 0x10; i += 4)
    {
        if (!IsBadReadPtr(*reinterpret_cast<uint8**>(FirstItemPtr + i)))
        {
            FUObjectItemInitialOffset = i;
            LogInfo("Found FUObjectItemInitialOffset: 0x%X", i);
            break;
        }
    }

    for (int i = FUObjectItemInitialOffset + 0x8; i <= 0x38; i += 4)
    {
        void* SecondObject = *reinterpret_cast<uint8**>(FirstItemPtr + i);
        void* ThirdObject  = *reinterpret_cast<uint8**>(FirstItemPtr + (i * 2) - FUObjectItemInitialOffset);

        if (!IsBadReadPtr(SecondObject) && !IsBadReadPtr(*reinterpret_cast<void**>(SecondObject)) && !IsBadReadPtr(ThirdObject) && !IsBadReadPtr(*reinterpret_cast<void**>(ThirdObject)))
        {
            SizeOfFUObjectItem = i - FUObjectItemInitialOffset;
            LogInfo("Found SizeOfFUObjectItem: 0x%X", SizeOfFUObjectItem);
            break;
        }
    }

    Off::InSDK::ObjArray::FUObjectItemInitialOffset = FUObjectItemInitialOffset;
    Off::InSDK::ObjArray::FUObjectItemSize = SizeOfFUObjectItem;
    LogSuccess("FUObjectItem initialized successfully (Offset: 0x%X, Size: 0x%X)", FUObjectItemInitialOffset, SizeOfFUObjectItem);
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
    
    int IndexOffset = 0x0;
    uint8* ObjAtIdx374 = (uint8*)ByIndex(ChunksPtr, 0x374, SizeOfFUObjectItem, FUObjectItemInitialOffset, 0x10000);
    uint8* ObjAtIdx106 = (uint8*)ByIndex(ChunksPtr, 0x106, SizeOfFUObjectItem, FUObjectItemInitialOffset, 0x10000);

    for (int i = 0x8; i < 0x20; i++)
    {
        if (*reinterpret_cast<int32*>(ObjAtIdx374 + i) == 0x374 && *reinterpret_cast<int32*>(ObjAtIdx106 + i) == 0x106)
        {
            IndexOffset = i;
            LogInfo("Found IndexOffset: 0x%X", i);
            break;
        }
    }

    int IndexToCheck = 0x10400;
    while (ObjectArray::Num() > IndexToCheck)
    {
        if (void* Obj = ByIndex(ChunksPtr, IndexToCheck, SizeOfFUObjectItem, FUObjectItemInitialOffset, 0x10000))
        {
            const bool bHasBiggerChunkSize = (*reinterpret_cast<int32*>((uint8*)Obj + IndexOffset) != IndexToCheck);
            NumElementsPerChunk = bHasBiggerChunkSize ? 0x10400 : 0x10000;
            LogInfo("Determined chunk size: 0x%X", NumElementsPerChunk);
            break;
        }
        IndexToCheck += 0x10400;
    }

    Off::InSDK::ObjArray::ChunkSize = NumElementsPerChunk;
    LogSuccess("Chunk size initialized: 0x%X", NumElementsPerChunk);
}

/* We don't speak about this function... */
void ObjectArray::Init(bool bScanAllMemory, const char* const ModuleName)
{
    LogInfo("\nDumper-7 by me, you & him\n\n\n");

    const auto [ImageBase, ImageSize, Header, Slide] = GetImageBaseAndSize(ModuleName);

    if (!ImageBase)
    {
        LogError("ObjectArray: module '%s' not found", ModuleName ? ModuleName : "<main executable>");
        return;
    }

    LogInfo("Searching for GObjects...\n\n");

    auto MatchesAnyLayout = []<typename ArrayLayoutType, size_t Size>(const std::array<ArrayLayoutType, Size>& ObjectArrayLayouts, uintptr Address)
    {
        for (const ArrayLayoutType& Layout : ObjectArrayLayouts)
        {
            if (!IsAddressValidGObjects(Address, Layout))
                continue;

            if constexpr (std::is_same_v<ArrayLayoutType, FFixedUObjectArrayLayout>)
            {
                Off::FUObjectArray::bIsChunked = false;
                Off::FUObjectArray::FixedLayout = Layout;
            }
            else
            {
                Off::FUObjectArray::bIsChunked = true;
                Off::FUObjectArray::ChunkedFixedLayout = Layout;
            }

            return true;
        }
        
        return false;
    };

    /* GObjects is a global variable: only data segments can contain it. (The old code scanned __TEXT, then the whole "image" incl. unmapped memory.) */
    for (const MachSegment& Segment : GetImageSegments(ModuleName))
    {
        if (!(Segment.InitProt & VM_PROT_READ) || (Segment.InitProt & VM_PROT_EXECUTE) || strncmp(Segment.Name, "__LINKEDIT", 16) == 0)
            continue;

        /* Stop 0x50 early so we don't read out of bounds when checking FixedArray->IsValid() or ChunkedArray->IsValid() */
        if (Segment.End - Segment.Begin <= 0x50)
            continue;

        for (uintptr CurrentAddress = Segment.Begin; CurrentAddress < (Segment.End - 0x50); CurrentAddress += 0x4)
        {
            if (MatchesAnyLayout(FFixedUObjectArrayLayouts, CurrentAddress))
            {
                GObjects = reinterpret_cast<uint8*>(CurrentAddress);
                NumElementsPerChunk = -1;

                Off::InSDK::ObjArray::GObjects = CurrentAddress - ImageBase;

                LogSuccess("Found FFixedUObjectArray GObjects at offset 0x%X", Off::InSDK::ObjArray::GObjects);

                ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
                {
                    if (Index < 0 || Index >= Num())
                        return nullptr;

                    uint8* ChunkPtr = DecryptPtr(*reinterpret_cast<uint8**>(ObjectsArray));

                    return *reinterpret_cast<void**>(ChunkPtr + FUObjectItemOffset + (Index * FUObjectItemSize));
                };

                uint8* ChunksPtr = DecryptPtr(*reinterpret_cast<uint8**>(GObjects + Off::FUObjectArray::GetObjectsOffset()));

                ObjectArray::InitializeFUObjectItem(*reinterpret_cast<uint8**>(ChunksPtr));

                return;
            }
            else if (MatchesAnyLayout(FChunkedFixedUObjectArrayLayouts, CurrentAddress))
            {
                GObjects = reinterpret_cast<uint8*>(CurrentAddress);
                NumElementsPerChunk = 0x10000;
                SizeOfFUObjectItem = 0x18;
                FUObjectItemInitialOffset = 0x0;

                Off::InSDK::ObjArray::GObjects = CurrentAddress - ImageBase;

                LogSuccess("Found FChunkedFixedUObjectArray GObjects at offset 0x%X", Off::InSDK::ObjArray::GObjects);

                ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
                {
                    if (Index >= Num())
                        return nullptr;

                    return ChunkedByIndexImpl(ObjectsArray, Index, FUObjectItemSize, FUObjectItemOffset, PerChunk, DecryptPtr, GObjects);
                };
                
                uint8* ChunksPtr = DecryptPtr(*reinterpret_cast<uint8**>(GObjects + Off::FUObjectArray::GetObjectsOffset()));

                ObjectArray::InitializeFUObjectItem(*reinterpret_cast<uint8**>(ChunksPtr));

                ObjectArray::InitializeChunkSize(GObjects + Off::FUObjectArray::GetObjectsOffset());

                return;
            }
        }
    }

    /* Don't kill the game (was 'exit(1)'), let the caller abort the dump. */
    GObjects = nullptr;
    ByIndex = nullptr;
    LogError("GObjects couldn't be found!");
}

void ObjectArray::Init(int32 GObjectsOffset, const FFixedUObjectArrayLayout& ObjectArrayLayout, const char* const ModuleName)
{
	LogInfo("Initializing ObjectArray with FFixedUObjectArray at offset 0x%X", GObjectsOffset);
	
	GObjects = reinterpret_cast<uint8*>(GetModuleBase(ModuleName) + GObjectsOffset);
	Off::InSDK::ObjArray::GObjects = GObjectsOffset;

	LogInfo("GObjects: 0x%p", (void*)GObjects);

	Off::FUObjectArray::bIsChunked = false;
	Off::FUObjectArray::FixedLayout = ObjectArrayLayout.IsValid() ? ObjectArrayLayout : FFixedUObjectArrayLayouts[0];

	ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
	{
		if (Index < 0 || Index >= Num())
			return nullptr;

		uint8* ItemPtr = *reinterpret_cast<uint8**>(ObjectsArray) + (Index * FUObjectItemSize);

		return *reinterpret_cast<void**>(ItemPtr + FUObjectItemOffset);
	};

	uint8* ChunksPtr = DecryptPtr(*reinterpret_cast<uint8**>(GObjects + Off::FUObjectArray::GetObjectsOffset()));

	ObjectArray::InitializeFUObjectItem(*reinterpret_cast<uint8**>(ChunksPtr));
	LogSuccess("FFixedUObjectArray initialized successfully");
}

void ObjectArray::Init(int32 GObjectsOffset, int32 ElementsPerChunk, const FChunkedFixedUObjectArrayLayout& ObjectArrayLayout, const char* const ModuleName)
{
	LogInfo("Initializing ObjectArray with FChunkedFixedUObjectArray at offset 0x%X", GObjectsOffset);
	GObjects = reinterpret_cast<uint8*>(GetModuleBase(ModuleName) + GObjectsOffset);
	Off::InSDK::ObjArray::GObjects = GObjectsOffset;

	Off::FUObjectArray::bIsChunked = true;
	Off::FUObjectArray::ChunkedFixedLayout = ObjectArrayLayout.IsValid() ? ObjectArrayLayout : FChunkedFixedUObjectArrayLayouts[0];

	NumElementsPerChunk = ElementsPerChunk;
	Off::InSDK::ObjArray::ChunkSize = ElementsPerChunk;

	ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
	{
		if (Index >= Num())
			return nullptr;

		return ChunkedByIndexImpl(ObjectsArray, Index, FUObjectItemSize, FUObjectItemOffset, PerChunk, DecryptPtr, GObjects);
	};

	uint8* ChunksPtr = DecryptPtr(*reinterpret_cast<uint8**>(GObjects + Off::FUObjectArray::GetObjectsOffset()));

	ObjectArray::InitializeFUObjectItem(*reinterpret_cast<uint8**>(ChunksPtr));
	LogSuccess("FChunkedFixedUObjectArray initialized successfully");
}

void ObjectArray::InitWithKnownLayout(uint8* GObjectsAddress, int32 GObjectsOffset, int32 ElementsPerChunk, const FChunkedFixedUObjectArrayLayout& ObjectArrayLayout, uint32 ItemSize, uint32 ItemObjectOffset)
{
	GObjects = GObjectsAddress;
	NumElementsPerChunk = ElementsPerChunk;
	SizeOfFUObjectItem = ItemSize;
	FUObjectItemInitialOffset = ItemObjectOffset;

	Off::FUObjectArray::bIsChunked = true;
	Off::FUObjectArray::ChunkedFixedLayout = ObjectArrayLayout;

	Off::InSDK::ObjArray::GObjects = GObjectsOffset;
	Off::InSDK::ObjArray::ChunkSize = ElementsPerChunk;
	Off::InSDK::ObjArray::FUObjectItemSize = ItemSize;
	Off::InSDK::ObjArray::FUObjectItemInitialOffset = ItemObjectOffset;

	ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
	{
		if (Index >= Num())
			return nullptr;

		return ChunkedByIndexImpl(ObjectsArray, Index, FUObjectItemSize, FUObjectItemOffset, PerChunk, DecryptPtr, GObjects);
	};

	LogSuccess("ObjectArray: GObjects at offset 0x%X (Objects 0x%X, NumElements 0x%X, NumChunks 0x%X, ItemSize 0x%X, ItemObject 0x%X), %d objects",
		GObjectsOffset, ObjectArrayLayout.ObjectsOffset, ObjectArrayLayout.NumElementsOffset, ObjectArrayLayout.NumChunksOffset, ItemSize, ItemObjectOffset, Num());
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

	return *reinterpret_cast<int32*>(GObjects + Off::FUObjectArray::GetNumElementsOffset());
}

template<typename UEType>
UEType ObjectArray::GetByIndex(int32 Index)
{
	if (!GObjects || !ByIndex)
		return UEType();

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
	auto ObjArray = ObjectArray();

	const int32 Total = ObjectArray::Num();
	int32 i = 0;

	for (UEObject Object : ObjArray)
	{
		// Progress heartbeat so a slow / stuck scan is visible in the console
		if ((i & 0xFFFF) == 0 && i > 0)
			LogInfo("FindObjectFast(\"%s\"): scanned %d / %d", Name.c_str(), i, Total);
		i++;

		// Skip objects whose UObject* pointer or class read would fault
		const void* Addr = Object.GetAddress();
		if (!Addr || IsBadReadPtr(Addr))
			continue;

		if (Object.IsA(RequiredType) && Object.GetName() == Name)
		{
			LogSuccess("FindObjectFast(\"%s\"): found at index %d", Name.c_str(), i - 1);
			return Object.Cast<UEType>();
		}
	}

	LogError("FindObjectFast(\"%s\"): not found after scanning %d objects", Name.c_str(), Total);
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
	: CurrentIndex(StartIndex), CurrentObject(ObjectArray::GetByIndex(StartIndex))
{
}

UEObject ObjectArray::ObjectsIterator::operator*()
{
	return CurrentObject;
}

ObjectArray::ObjectsIterator& ObjectArray::ObjectsIterator::operator++()
{
	CurrentObject = ObjectArray::GetByIndex(++CurrentIndex);

	while (!CurrentObject && CurrentIndex < (ObjectArray::Num() - 1))
	{
		CurrentObject = ObjectArray::GetByIndex(++CurrentIndex);
	}

	if (!CurrentObject && CurrentIndex == (ObjectArray::Num() - 1)) [[unlikely]]
		CurrentIndex++;

	return *this;
}

bool ObjectArray::ObjectsIterator::operator!=(const ObjectsIterator& Other) const
{
	return CurrentIndex != Other.CurrentIndex;
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
[[maybe_unused]] void TemplateTypeCreationForObjectArray(void)
{
	ObjectArray::FindObject<UEObject>("");
	ObjectArray::FindObject<UEField>("");
	ObjectArray::FindObject<UEEnum>("");
	ObjectArray::FindObject<UEStruct>("");
	ObjectArray::FindObject<UEClass>("");
	ObjectArray::FindObject<UEFunction>("");
	ObjectArray::FindObject<UEProperty>("");
	ObjectArray::FindObject<UEByteProperty>("");
	ObjectArray::FindObject<UEBoolProperty>("");
	ObjectArray::FindObject<UEObjectProperty>("");
	ObjectArray::FindObject<UEClassProperty>("");
	ObjectArray::FindObject<UEStructProperty>("");
	ObjectArray::FindObject<UEArrayProperty>("");
	ObjectArray::FindObject<UEMapProperty>("");
	ObjectArray::FindObject<UESetProperty>("");
	ObjectArray::FindObject<UEEnumProperty>("");

	ObjectArray::FindObjectFast<UEObject>("");
	ObjectArray::FindObjectFast<UEField>("");
	ObjectArray::FindObjectFast<UEEnum>("");
	ObjectArray::FindObjectFast<UEStruct>("");
	ObjectArray::FindObjectFast<UEClass>("");
	ObjectArray::FindObjectFast<UEFunction>("");
	ObjectArray::FindObjectFast<UEProperty>("");
	ObjectArray::FindObjectFast<UEByteProperty>("");
	ObjectArray::FindObjectFast<UEBoolProperty>("");
	ObjectArray::FindObjectFast<UEObjectProperty>("");
	ObjectArray::FindObjectFast<UEClassProperty>("");
	ObjectArray::FindObjectFast<UEStructProperty>("");
	ObjectArray::FindObjectFast<UEArrayProperty>("");
	ObjectArray::FindObjectFast<UEMapProperty>("");
	ObjectArray::FindObjectFast<UESetProperty>("");
	ObjectArray::FindObjectFast<UEEnumProperty>("");

	ObjectArray::FindObjectFastInOuter<UEObject>("", "");
	ObjectArray::FindObjectFastInOuter<UEField>("", "");
	ObjectArray::FindObjectFastInOuter<UEEnum>("", "");
	ObjectArray::FindObjectFastInOuter<UEStruct>("", "");
	ObjectArray::FindObjectFastInOuter<UEClass>("", "");
	ObjectArray::FindObjectFastInOuter<UEFunction>("", "");
	ObjectArray::FindObjectFastInOuter<UEProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEByteProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEBoolProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEObjectProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEClassProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEStructProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEArrayProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEMapProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UESetProperty>("", "");
	ObjectArray::FindObjectFastInOuter<UEEnumProperty>("", "");

	ObjectArray::GetByIndex<UEObject>(-1);
	ObjectArray::GetByIndex<UEField>(-1);
	ObjectArray::GetByIndex<UEEnum>(-1);
	ObjectArray::GetByIndex<UEStruct>(-1);
	ObjectArray::GetByIndex<UEClass>(-1);
	ObjectArray::GetByIndex<UEFunction>(-1);
	ObjectArray::GetByIndex<UEProperty>(-1);
	ObjectArray::GetByIndex<UEByteProperty>(-1);
	ObjectArray::GetByIndex<UEBoolProperty>(-1);
	ObjectArray::GetByIndex<UEObjectProperty>(-1);
	ObjectArray::GetByIndex<UEClassProperty>(-1);
	ObjectArray::GetByIndex<UEStructProperty>(-1);
	ObjectArray::GetByIndex<UEArrayProperty>(-1);
	ObjectArray::GetByIndex<UEMapProperty>(-1);
	ObjectArray::GetByIndex<UESetProperty>(-1);
	ObjectArray::GetByIndex<UEEnumProperty>(-1);
}


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
