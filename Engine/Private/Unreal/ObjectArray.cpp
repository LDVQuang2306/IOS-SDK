
#include <iostream>
#include <fstream>
#include <format>
#include <filesystem>
#include <unistd.h>
#include <format.h>

#include "../../Public/Unreal/ObjectArray.h"
#include "../../Public/OffsetFinder/Offsets.h"
#include "../../../Utils/Utils.h"
#include "../../../Menu/Logger.h"


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
	FChunkedFixedUObjectArrayLayout // DeltaForce
	{
		.ObjectsOffset = 0x20,
		.MaxElementsOffset = 0x10,
		.NumElementsOffset = 0x4,
		.MaxChunksOffset = 0x0,
		.NumChunksOffset = 0x14,
	},
	FChunkedFixedUObjectArrayLayout // Mutliversus
	{
		.ObjectsOffset = 0x18,
		.MaxElementsOffset = 0x10,
		.NumElementsOffset = 0x00, // first
		.MaxChunksOffset = 0x14,
		.NumChunksOffset = 0x20,
	}
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

	if (IsBadReadPtr(ObjectsButDecrypted))
        return false;

	if (IsBadReadPtr(ObjectsButDecrypted[5].Object))
        return false;

	const uintptr FithObject = reinterpret_cast<uintptr>(ObjectsButDecrypted[0x5].Object);
	const int32 IndexOfFithobject = *reinterpret_cast<int32*>(FithObject + 0xC);

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
	if (!ObjectsPtrButDecrypted || IsBadReadPtr(ObjectsPtrButDecrypted))
		return false;
	

	/* Check if every chunk-pointer is valid. */
	for (int i = 0; i < NumChunks; i++)
	{
		if (!ObjectsPtrButDecrypted[i] || IsBadReadPtr(ObjectsPtrButDecrypted[i]))
			return false;
	}
	
	LogInfo("FChunkedFixedUObjectArray validation successful");
	return true;
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
    if (!bScanAllMemory)
        LogInfo("\nDumper-7 by me, you & him\n\n\n");

    const auto [ImageBase, ImageSize, Header] = GetImageBaseAndSize(ModuleName);

    uintptr SearchBase = ImageBase;
    uintptr SearchRange = ImageSize;

    if (!bScanAllMemory)
    {
        const auto [DataSection, DataSize] = GetSegmentByName(Header, "__TEXT");

        if (DataSection != 0x0 && DataSize != 0x0)
        {
            SearchBase = DataSection;
            SearchRange = DataSize;
        }
        else
        {
            LogInfo("__text section not found, scanning all memory");
            bScanAllMemory = true;
        }
    }

    /* Sub 0x50 so we don't try to read out of bounds memory when checking FixedArray->IsValid() or ChunkedArray->IsValid() */
    SearchRange -= 0x50;

    if (!bScanAllMemory)
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

    for (int i = 0; i < SearchRange; i += 0x4)
    {
        const uintptr CurrentAddress = SearchBase + i;

        if (MatchesAnyLayout(FFixedUObjectArrayLayouts, CurrentAddress))
        {
            GObjects = reinterpret_cast<uint8*>(SearchBase + i);
            NumElementsPerChunk = -1;

            Off::InSDK::ObjArray::GObjects = (SearchBase + i) - ImageBase;

            LogSuccess("Found FFixedUObjectArray GObjects at offset 0x%X", Off::InSDK::ObjArray::GObjects);

            ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
            {
                if (Index < 0 || Index > Num())
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
            GObjects = reinterpret_cast<uint8*>(SearchBase + i);
            NumElementsPerChunk = 0x10000;
            SizeOfFUObjectItem = 0x18;
            FUObjectItemInitialOffset = 0x0;

            Off::InSDK::ObjArray::GObjects = (SearchBase + i) - ImageBase;

            LogSuccess("Found FChunkedFixedUObjectArray GObjects at offset 0x%X", Off::InSDK::ObjArray::GObjects);

            ByIndex = [](void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) -> void*
            {
                if (Index < 0 || Index > Num())
                    return nullptr;

                const int32 ChunkIndex = Index / PerChunk;
                const int32 InChunkIdx = Index % PerChunk;

                uint8* ChunkPtr = DecryptPtr(*reinterpret_cast<uint8**>(ObjectsArray));

                uint8* Chunk = reinterpret_cast<uint8**>(ChunkPtr)[ChunkIndex];
                uint8* ItemPtr = Chunk + (InChunkIdx * FUObjectItemSize);

                return *reinterpret_cast<void**>(ItemPtr + FUObjectItemOffset);
            };
            
            uint8* ChunksPtr = DecryptPtr(*reinterpret_cast<uint8**>(GObjects + Off::FUObjectArray::GetObjectsOffset()));

            ObjectArray::InitializeFUObjectItem(*reinterpret_cast<uint8**>(ChunksPtr));

            ObjectArray::InitializeChunkSize(GObjects + Off::FUObjectArray::GetObjectsOffset());

            return;
        }
    }

    if (!bScanAllMemory)
    {
        LogInfo("Retrying with full memory scan...");
        ObjectArray::Init(true);
        return;
    }

    if (GObjects == nullptr)
    {
        LogError("GObjects couldn't be found!");
        sleep(3);
        exit(1);
    }
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
		if (Index < 0 || Index > Num())
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
		if (Index < 0 || Index > Num())
			return nullptr;

		const int32 ChunkIndex = Index / PerChunk;
		const int32 InChunkIdx = Index % PerChunk;

		uint8* Chunk = (*reinterpret_cast<uint8***>(ObjectsArray))[ChunkIndex];
		uint8* ItemPtr = reinterpret_cast<uint8*>(Chunk) + (InChunkIdx * FUObjectItemSize);

		return *reinterpret_cast<void**>(ItemPtr + FUObjectItemOffset);
	};

	uint8* ChunksPtr = DecryptPtr(*reinterpret_cast<uint8**>(GObjects + Off::FUObjectArray::GetObjectsOffset()));

	ObjectArray::InitializeFUObjectItem(*reinterpret_cast<uint8**>(ChunksPtr));
	LogSuccess("FChunkedFixedUObjectArray initialized successfully");
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
	return *reinterpret_cast<int32*>(GObjects + Off::FUObjectArray::GetNumElementsOffset());
}

template<typename UEType>
UEType ObjectArray::GetByIndex(int32 Index)
{
	return UEType(ByIndex(GObjects + Off::FUObjectArray::GetObjectsOffset(), Index, SizeOfFUObjectItem, FUObjectItemInitialOffset, NumElementsPerChunk));
}

template<typename UEType>
UEType ObjectArray::FindObject(const std::string& FullName, EClassCastFlags RequiredType)
{
	for (UEObject Object : ObjectArray())
	{
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

	for (UEObject Object : ObjArray)
	{
		if (Object.IsA(RequiredType) && Object.GetName() == Name)
		{
			return Object.Cast<UEType>();
		}
	}

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

bool ObjectArray::ObjectsIterator::operator!=(const ObjectsIterator& Other)
{
	return CurrentIndex != Other.CurrentIndex;
}

int32 ObjectArray::ObjectsIterator::GetIndex() const
{
	return CurrentIndex;
}

/*

* See https://stackoverflow.com/questions/456713/why-do-i-get-unresolved-external-symbol-errors-when-using-templates
*/
// Explicit Template Instantiation
// Bắt buộc trình biên dịch tạo ra code cho các hàm này để Linker có thể tìm thấy.

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