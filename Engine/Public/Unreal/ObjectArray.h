#pragma once

#include <string>
#include <vector>
#include <filesystem>

#include "UnrealObjects.h"
#include "../OffsetFinder/Offsets.h"

namespace fs = std::filesystem;

class ObjectArray
{
private:
	friend struct FChunkedFixedUObjectArray;
	friend struct FFixedUObjectArray;
	friend class ObjectArrayValidator;

	friend bool IsAddressValidGObjects(const uintptr_t, const struct FFixedUObjectArrayLayout&);
	friend bool IsAddressValidGObjects(const uintptr_t, const struct FChunkedFixedUObjectArrayLayout&);

private:
	static inline uint8* GObjects = nullptr;
	static inline uint32 NumElementsPerChunk = 0x10000;
	static inline uint32 SizeOfFUObjectItem = 0x18;
	static inline uint32 FUObjectItemInitialOffset = 0x0;

public:
	static inline std::string DecryptionLambdaStr;

private:
	static inline void*(*ByIndex)(void* ObjectsArray, int32 Index, uint32 FUObjectItemSize, uint32 FUObjectItemOffset, uint32 PerChunk) = nullptr;

	/* Object of every index when CreateSnapshot() was called, used by Num()/GetByIndex() from then on */
	static inline std::vector<void*> Snapshot;
	static inline bool bUseSnapshot = false;

	static inline uint8_t* (*DecryptPtr)(void* ObjPtr) = [](void* Ptr) -> uint8* { return static_cast<uint8*>(Ptr); };

private:
	static void InitializeFUObjectItem(uint8_t* FirstItemPtr);
	static void InitializeChunkSize(uint8_t* GObjects);

public:
	static void InitDecryption(uint8_t* (*DecryptionFunction)(void* ObjPtr), const char* DecryptionLambdaAsStr);

	static void Init(bool bScanAllMemory = false, const char* const ModuleName = nullptr);

	static void Init(int32 GObjectsOffset, const FFixedUObjectArrayLayout& ObjectArrayLayout = FFixedUObjectArrayLayout(), const char* const ModuleName = nullptr);
	static void Init(int32 GObjectsOffset, int32 ElementsPerChunk, const FChunkedFixedUObjectArrayLayout& ObjectArrayLayout = FChunkedFixedUObjectArrayLayout(), const char* const ModuleName = nullptr);

	/* Chunked GUObjectArray whose layout and FUObjectItem geometry are already known and validated (no heuristics). */
	static void InitWithKnownLayout(uint8* GObjectsAddress, int32 GObjectsOffset, int32 ElementsPerChunk, const FChunkedFixedUObjectArrayLayout& ObjectArrayLayout, uint32 ItemSize, uint32 ItemObjectOffset);

	static inline bool IsInitialized() { return GObjects != nullptr && ByIndex != nullptr; }

	/*
	* The game keeps running while the SDK is generated, objects are loaded and garbage collected in the meantime. Every manager
	* (PackageManager, StructManager, EnumManager, MemberManager) walks the object list on its own, without a snapshot they could each
	* see a different set of objects ("unordered_map::at: key not found"). After this call Num()/GetByIndex() return the snapshot.
	*/
	static void CreateSnapshot();

	/* Whether the object at this index still is the object of the snapshot (it wasn't garbage collected since) */
	static bool IsStillAlive(int32 Index);

	static void DumpObjects(const fs::path& Path, bool bWithPathname = false);
	static void DumpObjectsWithProperties(const fs::path& Path, bool bWithPathname = false);

	static int32 Num();

	template<typename UEType = UEObject>
	static UEType GetByIndex(int32 Index);

	template<typename UEType = UEObject>
	static UEType FindObject(const std::string& FullName, EClassCastFlags RequiredType = EClassCastFlags::None);

	template<typename UEType = UEObject>
	static UEType FindObjectFast(const std::string& Name, EClassCastFlags RequiredType = EClassCastFlags::None);

	template<typename UEType = UEObject>
	static UEType FindObjectFastInOuter(const std::string& Name, std::string Outer);

	static UEStruct FindStruct(const std::string& FullName);
	static UEStruct FindStructFast(const std::string& Name);

	static UEClass FindClass(const std::string& FullName);
	static UEClass FindClassFast(const std::string& Name);

	class ObjectsIterator
	{
		UEObject CurrentObject;
		int32 CurrentIndex;

	public:
		ObjectsIterator(int32 StartIndex = 0);

		UEObject operator*();
		ObjectsIterator& operator++();
		bool operator!=(const ObjectsIterator& Other);

		int32 GetIndex() const;
	};

	ObjectsIterator begin();
	ObjectsIterator end();

	static inline void* DEBUGGetGObjects()
	{
		return GObjects;
	}
};

#ifndef InitObjectArrayDecryption
#define InitObjectArrayDecryption(DecryptionLambda) ObjectArray::InitDecryption(DecryptionLambda, #DecryptionLambda)
#endif
