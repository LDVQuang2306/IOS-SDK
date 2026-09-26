/*
* Synthetic Delta Force process image for the Linux harness.
* Builds an ARM64 Mach-O "image" with __TEXT/__DATA segments, an obfuscated FNamePool (DF-v1 codec, Blocks @0xC8,
* Cursor @0x100C8, CurrentBlock @0x100CC, 18 block bits) and a GUObjectArray with the reordered DF UObject/UStruct/FField layout.
*/
#include <sys/mman.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <functional>
#include <unordered_map>

#include <mach-o/loader.h>
#include <mach-o/dyld.h>

#include "Engine/Public/Unreal/DeltaForce.h"
#include "Utils/Json/json.hpp"

struct FFakeImage { const mach_header* Header; std::string Name; intptr_t Slide; };
extern std::vector<FFakeImage> GFakeImages;

namespace
{
	constexpr uint64_t VmBase = 0x100000000ULL;
	constexpr size_t TextSize = 0x100000;
	constexpr size_t DataSize = 0x300000;
	constexpr size_t LinkEditSize = 0x4000;
	constexpr size_t ImageSize = TextSize + DataSize + LinkEditSize;

	uint8_t* Image = nullptr;
	uint8_t* Heap = nullptr;
	size_t HeapSize = 0x8000000;
	size_t HeapUsed = 0;
	size_t DataUsed = 0;

	template<typename T> void W(uintptr_t Address, T Value) { memcpy(reinterpret_cast<void*>(Address), &Value, sizeof(T)); }

	uintptr_t Alloc(size_t Size, size_t Align = 16)
	{
		HeapUsed = (HeapUsed + Align - 1) & ~(Align - 1);
		const uintptr_t Result = reinterpret_cast<uintptr_t>(Heap) + HeapUsed;
		HeapUsed += Size;
		if (HeapUsed > HeapSize) { fprintf(stderr, "heap exhausted\n"); abort(); }
		return Result;
	}

	uintptr_t DataAlloc(size_t Size, size_t Align = 16)
	{
		DataUsed = (DataUsed + Align - 1) & ~(Align - 1);
		const uintptr_t Result = reinterpret_cast<uintptr_t>(Image) + TextSize + DataUsed;
		DataUsed += Size;
		return Result;
	}

	/* ---- FNamePool ---- */
	constexpr uint32_t NameBlockBytes = 2u << 18; // stride 2, 18 block offset bits

	uintptr_t Pool = 0, Block = 0;
	uint32_t CurrentBlock = 0, Cursor = 0;
	std::map<std::string, uint32_t> NameIds;

	uint32_t NameId(const std::string& Name)
	{
		auto It = NameIds.find(Name);
		if (It != NameIds.end()) return It->second;

		const bool bWide = Name == "\xE6\xB5\x8B\xE8\xAF\x95"; // "测试" -> wide entry
		const uint32_t EntrySize = bWide ? 2 + 4 : static_cast<uint32_t>((2 + Name.size() + 1) & ~size_t(1));

		if (Cursor + EntrySize > NameBlockBytes)
		{
			Block = Alloc(NameBlockBytes, 0x1000);
			CurrentBlock++;
			W<uintptr_t>(Pool + 0xC8 + CurrentBlock * 8, Block);
			W<uint32_t>(Pool + 0x100CC, CurrentBlock);
			Cursor = 0;
		}

		const uint32_t Offset = Cursor;

		if (bWide)
		{
			uint16_t Text[2] = { 0x6D4B, 0x8BD5 };
			DeltaForce::DecryptWide(Text, 2); // XOR codec is its own inverse
			W<uint16_t>(Block + Offset, static_cast<uint16_t>((2 << 6) | 1));
			memcpy(reinterpret_cast<void*>(Block + Offset + 2), Text, 4);
		}
		else
		{
			std::vector<uint8_t> Text(Name.begin(), Name.end());
			DeltaForce::DecryptAnsi(Text.data(), Text.size());
			W<uint16_t>(Block + Offset, static_cast<uint16_t>(Name.size() << 6));
			memcpy(reinterpret_cast<void*>(Block + Offset + 2), Text.data(), Text.size());
		}

		Cursor += EntrySize;
		W<uint32_t>(Pool + 0x100C8, Cursor);
		return NameIds[Name] = (CurrentBlock << 18) | (Offset / 2);
	}

	/* ---- objects ---- */
	uintptr_t GObjectsArray = 0, ChunkTable = 0;
	std::vector<uintptr_t> Chunks;
	std::vector<uintptr_t> Objects;

	/* FUObjectItem of an object index, chunks of 0x10000 items are allocated when needed */
	uintptr_t ObjectItem(size_t Index)
	{
		const size_t Chunk = Index >> 16;
		while (Chunks.size() <= Chunk)
		{
			Chunks.push_back(Alloc(0x10000 * 0x18, 0x1000));
			W<uintptr_t>(ChunkTable + (Chunks.size() - 1) * 8, Chunks.back());
			W<int32_t>(GObjectsArray + 0x18, static_cast<int32_t>(Chunks.size())); // NumChunks
		}
		return Chunks[Chunk] + (Index & 0xFFFF) * 0x18;
	}
	uintptr_t Vtable = 0;

	constexpr uint32_t RF_Public = 0x1, RF_Native = 0x0, RF_ClassDefaultObject = 0x10;

	/* DF_HARNESS_NO_OBJECT_FLAGS=1: UObject::Flags can't be identified (like on the real game), the SDK must not use a 'Flags' member */
	const bool bScrambleObjectFlags = getenv("DF_HARNESS_NO_OBJECT_FLAGS") != nullptr;

	uint32_t ObjectFlagsValue(uint32_t Flags, size_t Index) { return bScrambleObjectFlags ? static_cast<uint32_t>((Index + 1) * 2654435761u) : Flags; }

	uintptr_t NewObject(size_t Size, uintptr_t Class, uintptr_t Outer, const std::string& Name, uint32_t Flags = RF_Public, uint32_t Number = 0)
	{
		const uintptr_t Obj = Alloc(Size);
		W<uintptr_t>(Obj + 0x00, Vtable);
		W<uintptr_t>(Obj + 0x08, Class);
		W<uintptr_t>(Obj + 0x10, Outer);
		W<uint32_t>(Obj + 0x18, ObjectFlagsValue(Flags, Objects.size()));
		W<uint32_t>(Obj + 0x1C, NameId(Name));
		W<uint32_t>(Obj + 0x20, Number);
		W<int32_t>(Obj + 0x24, static_cast<int32_t>(Objects.size()));
		W<uintptr_t>(ObjectItem(Objects.size()), Obj);
		Objects.push_back(Obj);
		W<int32_t>(GObjectsArray + 0x14, static_cast<int32_t>(Objects.size()));
		return Obj;
	}

	void SetClass(uintptr_t Obj, uintptr_t Class) { W<uintptr_t>(Obj + 0x08, Class); }

	/* UStruct */
	constexpr size_t ClassSize = 0x300;
	void StructInfo(uintptr_t S, uintptr_t Super, int32_t Size, int32_t Align) { W<uintptr_t>(S + 0x40, Super); W<int32_t>(S + 0x3C, Size); W<int32_t>(S + 0x48, Align); }

	void AddChild(uintptr_t Struct, uintptr_t Child)
	{
		uintptr_t* Link = reinterpret_cast<uintptr_t*>(Struct + 0x50);
		while (*Link) Link = reinterpret_cast<uintptr_t*>(*Link + 0x28);
		*Link = Child;
	}

	/* ---- FField / FProperty ---- */
	std::map<std::string, uintptr_t> FieldClasses;

	uintptr_t FieldClass(const std::string& Name)
	{
		auto It = FieldClasses.find(Name);
		if (It != FieldClasses.end()) return It->second;
		const uintptr_t FC = Alloc(0x40);
		W<uint32_t>(FC + 0x0, NameId(Name));
		W<uint64_t>(FC + 0x8, FieldClasses.size() + 1);
		return FieldClasses[Name] = FC;
	}

	uintptr_t NewProperty(uintptr_t OwnerStruct, bool bOwnerIsUObject, const std::string& Kind, const std::string& Name, int32_t ElementSize, int32_t Offset, uint64_t Flags = 0x1, int32_t ArrayDim = 1)
	{
		const uintptr_t P = Alloc(0xA0);
		W<uintptr_t>(P + 0x00, Vtable);
		W<uintptr_t>(P + 0x08, OwnerStruct);
		W<uint8_t>(P + 0x10, bOwnerIsUObject ? 1 : 0);
		W<uintptr_t>(P + 0x20, FieldClass(Kind));
		W<uint32_t>(P + 0x28, NameId(Name));
		W<int32_t>(P + 0x38, ArrayDim);
		W<int32_t>(P + 0x3C, ElementSize);
		W<uint64_t>(P + 0x40, Flags);
		W<int32_t>(P + 0x4C, Offset);
		return P;
	}

	/* DF_HARNESS_CHANGED_LAYOUT=1 simulates a game update that moved UStruct::ChildProperties: the dumper must refuse, not crash */
	const uint32_t ChildPropertiesOffset = getenv("DF_HARNESS_CHANGED_LAYOUT") ? 0x70 : 0x68;

	void LinkProperty(uintptr_t Struct, uintptr_t Property)
	{
		uintptr_t* Link = reinterpret_cast<uintptr_t*>(Struct + ChildPropertiesOffset);
		while (*Link) Link = reinterpret_cast<uintptr_t*>(*Link + 0x18);
		*Link = Property;
	}

	uintptr_t AddProperty(uintptr_t Struct, const std::string& Kind, const std::string& Name, int32_t ElementSize, int32_t Offset, uint64_t Flags = 0x1, int32_t ArrayDim = 1)
	{
		const uintptr_t P = NewProperty(Struct, true, Kind, Name, ElementSize, Offset, Flags, ArrayDim);
		LinkProperty(Struct, P);
		return P;
	}

	uintptr_t TextCode(uint32_t Index) { return reinterpret_cast<uintptr_t>(Image) + 0x8000 + Index * 0x100; }

	/*
	* DF_HARNESS_WORLD: the object graph of a real dump (make_world.py), rebuilt in the Delta Force layout. Every object keeps its
	* index, class, outer and name; structs/classes/functions get their super, size, alignment and properties, enums their values.
	*/
	void BuildWorldObjects(const char* Path, uintptr_t GWorldPtr)
	{
		std::ifstream File(Path);
		if (!File) { fprintf(stderr, "[harness] can't open %s\n", Path); abort(); }

		const nlohmann::json World = nlohmann::json::parse(File);
		const nlohmann::json& JObjects = World.at("objects");
		const nlohmann::json& JStructs = World.at("structs");
		const nlohmann::json& JFunctions = World.at("functions");
		const nlohmann::json& JEnums = World.at("enums");

		const size_t Num = JObjects.size();

		std::vector<int32_t> StructSize(Num, 0);
		for (const auto& [Key, Value] : JStructs.items())
			StructSize[std::stoul(Key)] = Value[1].get<int32_t>();

		/* 1. every object at its index, null slots stay empty */
		std::vector<uintptr_t> Addr(Num, 0);
		for (size_t i = 0; i < Num; i++)
		{
			const nlohmann::json& O = JObjects[i];

			if (O.is_null())
			{
				ObjectItem(Objects.size());
				Objects.push_back(0);
				continue;
			}

			const std::string Key = std::to_string(i);
			const int64_t ClassIdx = O[0].get<int64_t>();

			size_t Size = 0x40;
			if (JFunctions.contains(Key))
				Size = 0xE0;
			else if (JStructs.contains(Key))
				Size = ClassSize;
			else if (JEnums.contains(Key))
				Size = 0x60;
			else if (ClassIdx >= 0)
				Size = std::max<int32_t>(StructSize[ClassIdx], 0x28) + 0x40;

			Addr[i] = NewObject(Size, 0, 0, O[2].get<std::string>(), O[3].get<uint32_t>());
		}
		W<int32_t>(GObjectsArray + 0x14, static_cast<int32_t>(Objects.size()));

		auto At = [&](const nlohmann::json& Index) -> uintptr_t
		{
			const int64_t I = Index.get<int64_t>();
			return I >= 0 && static_cast<size_t>(I) < Num ? Addr[I] : 0;
		};

		auto PackageOf = [&](size_t Index) -> uintptr_t
		{
			for (int Depth = 0; Depth < 64; Depth++)
			{
				const int64_t Outer = JObjects[Index][1].get<int64_t>();
				if (Outer < 0)
					return Addr[Index];
				Index = static_cast<size_t>(Outer);
			}
			return 0;
		};

		/* 2. class + outer */
		uintptr_t DelegateFunctionClass = 0;
		for (size_t i = 0; i < Num; i++)
		{
			if (!Addr[i])
				continue;

			SetClass(Addr[i], At(JObjects[i][0]));
			W<uintptr_t>(Addr[i] + 0x10, At(JObjects[i][1]));

			if (!DelegateFunctionClass && JObjects[i][2] == "DelegateFunction" && JStructs.contains(std::to_string(i)))
				DelegateFunctionClass = Addr[i];
		}

		/* 3. properties. Delegate properties get a DelegateFunction with the parameters of their signature. */
		uint32_t CodeIndex = 0, NumSignatures = 0, NumProperties = 0;

		std::function<uintptr_t(uintptr_t, bool, const nlohmann::json&, uintptr_t)> MakeProperty;

		auto MakeSignature = [&](const nlohmann::json& Params, uintptr_t Package) -> uintptr_t
		{
			const uintptr_t F = NewObject(0xE0, DelegateFunctionClass, Package, "HarnessSignature" + std::to_string(NumSignatures++) + "__DelegateSignature");

			int32_t ParmsSize = 0;
			uintptr_t* Link = reinterpret_cast<uintptr_t*>(F + ChildPropertiesOffset);
			for (const nlohmann::json& Param : Params)
			{
				const uintptr_t P = MakeProperty(F, true, Param, Package);
				*Link = P;
				Link = reinterpret_cast<uintptr_t*>(P + 0x18);
				ParmsSize = std::max(ParmsSize, Param.value("o", 0) + Param.value("s", 0));
			}

			StructInfo(F, 0, ParmsSize, 8);
			W<uint32_t>(F + 0xB8, 0x00100000 | 0x00020000 | 0x00010000); // Delegate | Public | MulticastDelegate
			W<uint8_t>(F + 0xB0, static_cast<uint8_t>(Params.size()));
			W<uint16_t>(F + 0xB2, static_cast<uint16_t>(ParmsSize));
			W<uintptr_t>(F + 0xD8, TextCode(0x100 + (CodeIndex++ % 0xE00)));
			return F;
		};

		MakeProperty = [&](uintptr_t Owner, bool bOwnerIsUObject, const nlohmann::json& P, uintptr_t Package) -> uintptr_t
		{
			NumProperties++;

			const uintptr_t Prop = NewProperty(Owner, bOwnerIsUObject, P.at("k").get<std::string>(), P.value("n", std::string("Inner")), P.value("s", 0), P.value("o", 0),
				P.value("f", uint64_t(0)), P.value("d", 1));

			if (P.contains("struct")) W<uintptr_t>(Prop + 0x88, At(P["struct"]));
			if (P.contains("cls"))    W<uintptr_t>(Prop + 0x88, At(P["cls"]));
			if (P.contains("meta"))   W<uintptr_t>(Prop + 0x90, At(P["meta"]));
			if (P.contains("enum"))   W<uintptr_t>(Prop + 0x88, At(P["enum"]));
			if (P.contains("under"))  W<uintptr_t>(Prop + 0x80, MakeProperty(Prop, false, P["under"], Package));
			if (P.contains("inner"))  W<uintptr_t>(Prop + 0x88, MakeProperty(Prop, false, P["inner"], Package));
			if (P.contains("elem"))   W<uintptr_t>(Prop + 0x80, MakeProperty(Prop, false, P["elem"], Package));
			if (P.contains("key"))    W<uintptr_t>(Prop + 0x88, MakeProperty(Prop, false, P["key"], Package));
			if (P.contains("val"))    W<uintptr_t>(Prop + 0x90, MakeProperty(Prop, false, P["val"], Package));
			if (P.contains("sig"))    W<uintptr_t>(Prop + 0x88, MakeSignature(P["sig"], Package));
			if (P.contains("field"))  W<uintptr_t>(Prop + 0x88, FieldClass(P["field"].get<std::string>()));
			if (P.contains("bool"))
			{
				const nlohmann::json& B = P["bool"];
				W<uint32_t>(Prop + 0x81, B[0].get<uint32_t>() | (B[1].get<uint32_t>() << 8) | (B[2].get<uint32_t>() << 16) | (B[3].get<uint32_t>() << 24));
			}
			return Prop;
		};

		for (const auto& [Key, S] : JStructs.items())
		{
			const size_t i = std::stoul(Key);
			const uintptr_t Struct = Addr[i];

			if (!Struct)
				continue;

			StructInfo(Struct, At(S[0]), S[1].get<int32_t>(), std::max(S[2].get<int32_t>(), 1));

			const uintptr_t Package = PackageOf(i);
			uintptr_t* Link = reinterpret_cast<uintptr_t*>(Struct + ChildPropertiesOffset);
			for (const nlohmann::json& P : S[3])
			{
				const uintptr_t Prop = MakeProperty(Struct, true, P, Package);
				*Link = Prop;
				Link = reinterpret_cast<uintptr_t*>(Prop + 0x18);
			}
		}

		/* 4. functions: flags, parameters, code, and linked into the Children of their class */
		std::unordered_map<uintptr_t, uintptr_t*> ChildrenTail;
		for (const auto& [Key, Flags] : JFunctions.items())
		{
			const size_t i = std::stoul(Key);
			const uintptr_t F = Addr[i];

			if (!F)
				continue;

			const nlohmann::json& S = JStructs.at(Key);
			W<uint32_t>(F + 0xB8, Flags.get<uint32_t>());
			W<uint8_t>(F + 0xB0, static_cast<uint8_t>(S[3].size()));
			W<uint16_t>(F + 0xB2, static_cast<uint16_t>(S[1].get<int32_t>()));
			W<uintptr_t>(F + 0xD8, TextCode(0x100 + (CodeIndex++ % 0xE00)));

			const int64_t Outer = JObjects[i][1].get<int64_t>();
			if (Outer < 0 || !JStructs.contains(std::to_string(Outer)))
				continue;

			uintptr_t*& Tail = ChildrenTail[Addr[Outer]];
			if (!Tail)
				Tail = reinterpret_cast<uintptr_t*>(Addr[Outer] + 0x50);
			*Tail = F;
			Tail = reinterpret_cast<uintptr_t*>(F + 0x28);
		}

		/* 5. enum values */
		for (const auto& [Key, Values] : JEnums.items())
		{
			const uintptr_t E = Addr[std::stoul(Key)];
			const uintptr_t Data = Alloc(16 * std::max<size_t>(Values.size(), 1));

			for (size_t v = 0; v < Values.size(); v++)
			{
				W<uint32_t>(Data + v * 16, NameId(Values[v][0].get<std::string>()));
				W<int64_t>(Data + v * 16 + 8, Values[v][1].get<int64_t>());
			}
			W<uintptr_t>(E + 0x40, Data);
			W<int32_t>(E + 0x48, static_cast<int32_t>(Values.size()));
			W<int32_t>(E + 0x4C, static_cast<int32_t>(Values.size()));
		}

		/* 6. UClass::ClassDefaultObject, a few real cast flags (the finder only reports the offset), GWorld */
		for (size_t i = 0; i < Num; i++)
		{
			if (!Addr[i] || JObjects[i][2].get<std::string>().rfind("Default__", 0) != 0)
				continue;

			const int64_t ClassIdx = JObjects[i][0].get<int64_t>();
			if (ClassIdx >= 0 && JStructs.contains(std::to_string(ClassIdx)))
				W<uintptr_t>(Addr[ClassIdx] + 0x118, Addr[i]);
		}
		for (size_t i = 0; i < Num; i++)
		{
			if (!Addr[i] || !JStructs.contains(std::to_string(i)))
				continue;

			if (JObjects[i][2] == "Actor")
				W<uint64_t>(Addr[i] + 0xE0, 0x0000001000000000ULL);
			else if (JObjects[i][2] == "Class")
				W<uint64_t>(Addr[i] + 0xE0, 0x1 | 0x8 | 0x20);
		}

		if (const int64_t WorldIdx = World.value("world", int64_t(-1)); WorldIdx >= 0)
			W<uintptr_t>(GWorldPtr, Addr[WorldIdx]);

		W<int32_t>(GObjectsArray + 0x14, static_cast<int32_t>(Objects.size()));
		printf("[harness] world: %zu objects (%u delegate signatures added), %u properties\n", Objects.size(), NumSignatures, NumProperties);
	}
}

uintptr_t GFakeWorld = 0;

/* Like a garbage collection: every Nth object (not the first ones, which are the engine's own classes) leaves the object array */
void HarnessUnloadObjects(int EveryNth)
{
	if (EveryNth <= 0)
		return;

	int NumUnloaded = 0;
	for (size_t i = std::min<size_t>(0x1000, Objects.size() / 2); i < Objects.size(); i += EveryNth)
	{
		if (!Objects[i])
			continue;

		W<uintptr_t>(ObjectItem(i), 0);
		NumUnloaded++;
	}

	printf("[harness] unloaded %d objects\n", NumUnloaded);
}

void BuildFakeDeltaForce()
{
	Image = static_cast<uint8_t*>(mmap(nullptr, ImageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	/* DF_HARNESS_WORLD=<world.json> (make_world.py): rebuild the object graph of a real dump instead of the synthetic objects */
	const char* WorldFile = getenv("DF_HARNESS_WORLD");
	HeapSize = WorldFile ? 0x80000000 : 0x8000000;
	Heap = static_cast<uint8_t*>(mmap(nullptr, HeapSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));

	/* Mach-O header + segments */
	auto* Header = reinterpret_cast<mach_header_64*>(Image);
	Header->magic = MH_MAGIC_64;
	Header->cputype = CPU_TYPE_ARM64;
	Header->filetype = MH_EXECUTE;
	Header->ncmds = 4;
	Header->sizeofcmds = 4 * sizeof(segment_command_64);

	auto* Segments = reinterpret_cast<segment_command_64*>(Header + 1);
	auto MakeSeg = [&](int i, const char* Name, uint64_t Addr, uint64_t Size, int Prot)
	{
		Segments[i].cmd = LC_SEGMENT_64; Segments[i].cmdsize = sizeof(segment_command_64);
		strncpy(Segments[i].segname, Name, 16); Segments[i].vmaddr = Addr; Segments[i].vmsize = Size; Segments[i].initprot = Prot; Segments[i].maxprot = Prot;
	};
	MakeSeg(0, "__PAGEZERO", 0, VmBase, 0);
	MakeSeg(1, "__TEXT", VmBase, TextSize, 5);
	MakeSeg(2, "__DATA", VmBase + TextSize, DataSize, 3);
	MakeSeg(3, "__LINKEDIT", VmBase + TextSize + DataSize, LinkEditSize, 1);

	GFakeImages.push_back({ reinterpret_cast<const mach_header*>(Header), "/private/var/containers/Bundle/Application/X/DeltaForceClient.app/DeltaForceClient", static_cast<intptr_t>(reinterpret_cast<uintptr_t>(Image) - VmBase) });
	GFakeImages.push_back({ reinterpret_cast<const mach_header*>(Header), "/private/var/containers/Bundle/Application/X/DeltaForceClient.app/Frameworks/DeltaForceClientHelper.dylib", 0 });
	GFakeImages.pop_back(); // only the main image is real memory

	/* __TEXT: "functions" = RET; one fake UObject::ProcessEvent at vtable index 0x45 */
	for (size_t i = 0x4000; i < TextSize; i += 4) W<uint32_t>(reinterpret_cast<uintptr_t>(Image) + i, 0xD65F03C0);

	const uintptr_t ProcessEvent = TextCode(0x45);
	W<uint32_t>(ProcessEvent + 0x0, 0xB940B828);  // LDR W8, [X1, #0xB8]   (FunctionFlags)
	W<uint32_t>(ProcessEvent + 0x4, 0x37500048);  // TBNZ W8, #10, +8      (FUNC_Native)
	W<uint32_t>(ProcessEvent + 0x40, 0x36B00048); // TBZ  W8, #22, +8      (FUNC_HasOutParms)

	/* __DATA: some noise, the vtable, FNamePool, FUObjectArray, GWorld */
	for (size_t i = 0; i < 0x2000; i += 8) W<uint64_t>(DataAlloc(8, 8), 0x1234 * i);

	Vtable = DataAlloc(0x100 * 8, 8);
	for (uint32_t i = 0; i < 0x80; i++) W<uintptr_t>(Vtable + i * 8, TextCode(i));

	Pool = DataAlloc(0x100D0, 64);
	Block = Alloc(NameBlockBytes, 0x1000);
	W<uintptr_t>(Pool + 0xC8, Block);
	W<uint32_t>(Pool + 0x100CC, 0); // CurrentBlock
	NameId("None");
	NameId("ByteProperty");

	GObjectsArray = DataAlloc(0x80, 64);
	ChunkTable = Alloc(0x100 * 8);
	/* Like the real game: +0x10 looks like MaxChunks, MaxElements can't be identified (TUObjectArray must not get two members at 0x14) */
	W<int32_t>(GObjectsArray + 0x10, 0x21);     // MaxChunks
	W<int32_t>(GObjectsArray + 0x1C, 0x0);      // unknown
	W<uintptr_t>(GObjectsArray + 0x20, ChunkTable);
	ObjectItem(0);                              // NumChunks = 1

	const uintptr_t GWorldPtr = DataAlloc(8, 8);

	if (WorldFile)
	{
		BuildWorldObjects(WorldFile, GWorldPtr);

		printf("[harness] world %s: image %p, %zu objects, %zu names, %u name blocks\n", WorldFile, Image, Objects.size(), NameIds.size(), CurrentBlock + 1);
		return;
	}

	/* ---- CoreUObject ---- */
	const uintptr_t CoreUObject = NewObject(0x40, 0, 0, "/Script/CoreUObject");
	const uintptr_t ClassClass = NewObject(ClassSize, 0, CoreUObject, "Class");
	SetClass(ClassClass, ClassClass);
	auto NewClass = [&](uintptr_t Package, const std::string& Name, uintptr_t Super, int32_t Size, int32_t Align = 8) -> uintptr_t
	{
		const uintptr_t C = NewObject(ClassSize, ClassClass, Package, Name);
		StructInfo(C, Super, Size, Align);
		return C;
	};
	SetClass(CoreUObject, 0);

	const uintptr_t ObjectClass = NewClass(CoreUObject, "Object", 0, 0x28);
	const uintptr_t FieldClassObj = NewClass(CoreUObject, "Field", ObjectClass, 0x30);
	const uintptr_t StructClass = NewClass(CoreUObject, "Struct", FieldClassObj, 0xB0);
	StructInfo(ClassClass, StructClass, 0x230, 8);
	const uintptr_t ScriptStructClass = NewClass(CoreUObject, "ScriptStruct", StructClass, 0xC0);
	const uintptr_t FunctionClass = NewClass(CoreUObject, "Function", StructClass, 0xE0);
	const uintptr_t DelegateFunctionClass = NewClass(CoreUObject, "DelegateFunction", FunctionClass, 0xE0);
	const uintptr_t EnumClass = NewClass(CoreUObject, "Enum", FieldClassObj, 0x60);
	const uintptr_t PackageClass = NewClass(CoreUObject, "Package", ObjectClass, 0x80);
	SetClass(CoreUObject, PackageClass);
	const uintptr_t InterfaceClass = NewClass(CoreUObject, "Interface", ObjectClass, 0x28);

	/* ScriptStructs */
	auto NewStruct = [&](uintptr_t Package, const std::string& Name, int32_t Size, int32_t Align) -> uintptr_t
	{
		const uintptr_t S = NewObject(0xC0, ScriptStructClass, Package, Name);
		StructInfo(S, 0, Size, Align);
		return S;
	};

	const uintptr_t Guid = NewStruct(CoreUObject, "Guid", 0x10, 4);
	AddProperty(Guid, "IntProperty", "A", 4, 0x0);
	AddProperty(Guid, "IntProperty", "B", 4, 0x4);
	AddProperty(Guid, "IntProperty", "C", 4, 0x8);
	AddProperty(Guid, "IntProperty", "D", 4, 0xC);

	const uintptr_t Vector = NewStruct(CoreUObject, "Vector", 0xC, 4);
	AddProperty(Vector, "FloatProperty", "X", 4, 0x0);
	AddProperty(Vector, "FloatProperty", "Y", 4, 0x4);
	AddProperty(Vector, "FloatProperty", "Z", 4, 0x8);

	const uintptr_t Vector2D = NewStruct(CoreUObject, "Vector2D", 0x8, 4);
	AddProperty(Vector2D, "FloatProperty", "X", 4, 0x0);
	AddProperty(Vector2D, "FloatProperty", "Y", 4, 0x4);

	const uintptr_t SoftObjectPath = NewStruct(CoreUObject, "SoftObjectPath", 0x18, 8);
	AddProperty(SoftObjectPath, "NameProperty", "AssetPathName", 8, 0x0);
	AddProperty(SoftObjectPath, "StrProperty", "SubPathString", 0x10, 0x8);

	/* ---- Engine ---- */
	const uintptr_t Engine = NewObject(0x40, PackageClass, 0, "/Script/Engine");
	const uintptr_t Actor = NewClass(Engine, "Actor", ObjectClass, 0x220);
	const uintptr_t Pawn = NewClass(Engine, "Pawn", Actor, 0x280);
	const uintptr_t PlayerController = NewClass(Engine, "PlayerController", Actor, 0x5A0);
	const uintptr_t Level = NewClass(Engine, "Level", ObjectClass, 0x300);
	const uintptr_t World = NewClass(Engine, "World", ObjectClass, 0x700);
	const uintptr_t EngineClass = NewClass(Engine, "Engine", ObjectClass, 0xE00);
	const uintptr_t GameEngine = NewClass(Engine, "GameEngine", EngineClass, 0xF00);
	const uintptr_t DataTable = NewClass(Engine, "DataTable", ObjectClass, 0xB0);
	const uintptr_t Kismet = NewClass(Engine, "KismetSystemLibrary", ObjectClass, 0x28);
	const uintptr_t ViewportClient = NewClass(Engine, "GameViewportClient", ObjectClass, 0x3B0);
	W<uintptr_t>(AddProperty(ViewportClient, "ObjectProperty", "World", 8, 0x78) + 0x88, World);
	W<uintptr_t>(AddProperty(EngineClass, "ObjectProperty", "GameViewport", 8, 0x780) + 0x88, ViewportClient);
	AddProperty(DataTable, "ObjectProperty", "RowStruct", 8, 0x28);

	/* ENetRole */
	const uintptr_t NetRole = NewObject(0x60, EnumClass, Engine, "ENetRole");
	{
		const char* Names[] = { "ROLE_None", "ROLE_SimulatedProxy", "ROLE_AutonomousProxy", "ROLE_Authority", "ROLE_MAX" };
		const uintptr_t Data = Alloc(16 * 5);
		for (int i = 0; i < 5; i++) { W<uint32_t>(Data + i * 16, NameId(Names[i])); W<int64_t>(Data + i * 16 + 8, i); }
		W<uintptr_t>(NetRole + 0x40, Data); W<int32_t>(NetRole + 0x48, 5); W<int32_t>(NetRole + 0x4C, 5);
	}

	/* Functions */
	uint32_t CodeIndex = 0x100;
	auto NewFunction = [&](uintptr_t Owner, const std::string& Name, uint32_t Flags, uintptr_t Class = 0) -> uintptr_t
	{
		const uintptr_t F = NewObject(0xE0, Class ? Class : FunctionClass, Owner, Name);
		StructInfo(F, 0, 0, 1);
		W<uint32_t>(F + 0xB8, Flags);
		W<uintptr_t>(F + 0xD8, TextCode(CodeIndex++));
		AddChild(Owner, F);
		return F;
	};
	auto AddParam = [&](uintptr_t F, const std::string& Kind, const std::string& Name, int32_t Size, bool bReturn) -> uintptr_t
	{
		const int32_t Offset = *reinterpret_cast<int32_t*>(F + 0x3C);
		const uintptr_t P = AddProperty(F, Kind, Name, Size, Offset, 0x80 | (bReturn ? 0x400 : 0));
		W<int32_t>(F + 0x3C, Offset + Size);
		W<uint16_t>(F + 0xB2, static_cast<uint16_t>(Offset + Size));
		W<uint8_t>(F + 0xB0, *reinterpret_cast<uint8_t*>(F + 0xB0) + 1);
		W<int32_t>(F + 0x48, Size >= 8 ? 8 : 4);
		return P;
	};
	constexpr uint32_t FUNC_Native = 0x400, FUNC_Public = 0x20000, FUNC_BlueprintCallable = 0x4000000, FUNC_Static = 0x2000, FUNC_Final = 0x1;

	{
		const uintptr_t F = NewFunction(Actor, "K2_GetActorLocation", FUNC_Native | FUNC_Public | FUNC_BlueprintCallable | FUNC_Final);
		W<uintptr_t>(AddParam(F, "StructProperty", "ReturnValue", 0xC, true) + 0x88, Vector);
	}
	{
		const uintptr_t F = NewFunction(Actor, "GetOwner", FUNC_Native | FUNC_Public | FUNC_BlueprintCallable);
		W<uintptr_t>(AddParam(F, "ObjectProperty", "ReturnValue", 8, true) + 0x88, Actor);
	}
	{
		const uintptr_t F = NewFunction(Actor, "SetActorHiddenInGame", FUNC_Native | FUNC_Public | FUNC_BlueprintCallable);
		const uintptr_t B = AddParam(F, "BoolProperty", "bNewHidden", 1, false);
		W<uint32_t>(B + 0x81, 0xFF010001);
	}
	NewFunction(Actor, "K2_DestroyActor", FUNC_Native | FUNC_Public | FUNC_BlueprintCallable);
	{
		const uintptr_t F = NewFunction(ObjectClass, "ExecuteUbergraph", 0x00020000 | 0x00000800);
		AddParam(F, "IntProperty", "EntryPoint", 4, false);
	}
	{
		const uintptr_t F = NewFunction(Actor, "GetActorTimeDilation", FUNC_Native | FUNC_Public | FUNC_BlueprintCallable);
		AddParam(F, "FloatProperty", "ReturnValue", 4, true);
	}
	NewFunction(Actor, "ReceiveBeginPlay", 0x08000800 /* BlueprintEvent|Event */);
	{
		const uintptr_t F = NewFunction(Kismet, "GetObjectName", FUNC_Native | FUNC_Public | FUNC_Static | FUNC_BlueprintCallable);
		W<uintptr_t>(AddParam(F, "ObjectProperty", "Object", 8, false) + 0x88, ObjectClass);
		AddParam(F, "StrProperty", "ReturnValue", 0x10, true);
	}
	{
		const uintptr_t F = NewFunction(PlayerController, "WasInputKeyJustPressed", FUNC_Native | FUNC_Public | FUNC_BlueprintCallable);
		const uintptr_t B = AddParam(F, "BoolProperty", "ReturnValue", 1, true);
		W<uint32_t>(B + 0x81, 0xFF010001);
	}

	/* ---- Game package with every property kind ---- */
	const uintptr_t Game = NewObject(0x40, PackageClass, 0, "/Script/DFGame");

	const uintptr_t Mode = NewObject(0x60, EnumClass, Game, "EDFMode");
	{
		const char* Names[] = { "EDFMode::Idle", "EDFMode::Combat", "EDFMode::EDFMode_MAX" };
		const uintptr_t Data = Alloc(16 * 3);
		for (int i = 0; i < 3; i++) { W<uint32_t>(Data + i * 16, NameId(Names[i])); W<int64_t>(Data + i * 16 + 8, i); }
		W<uintptr_t>(Mode + 0x40, Data); W<int32_t>(Mode + 0x48, 3); W<int32_t>(Mode + 0x4C, 3);
	}

	const uintptr_t OnDied = NewObject(0xE0, DelegateFunctionClass, Game, "OnDied__DelegateSignature");
	StructInfo(OnDied, 0, 0, 1);

	const uintptr_t Character = NewClass(Game, "DFCharacter", Pawn, 0x400);
	for (int Pass = 0; Pass < 3; Pass++) // several classes => enough samples per property kind
	{
		const std::string Suffix = Pass == 0 ? "" : std::to_string(Pass);
		const uintptr_t C = Pass == 0 ? Character : NewClass(Game, "DFCharacterVariant" + Suffix, Character, 0x400);
		const int32_t Base = Pass == 0 ? 0x280 : 0x400;
		if (Pass) StructInfo(C, Character, 0x540, 8);

		W<uintptr_t>(AddProperty(C, "ObjectProperty", "OwnerActor" + Suffix, 8, Base + 0x0) + 0x88, Actor);
		W<uintptr_t>(AddProperty(C, "StructProperty", "Location" + Suffix, 0xC, Base + 0x8) + 0x88, Vector);
		W<uint32_t>(AddProperty(C, "BoolProperty", "bIsAlive" + Suffix, 1, Base + 0x14) + 0x81, 0xFF010001);
		W<uint32_t>(AddProperty(C, "BoolProperty", "bBitA" + Suffix, 1, Base + 0x15) + 0x81, 0x01010001);
		W<uint32_t>(AddProperty(C, "BoolProperty", "bBitB" + Suffix, 1, Base + 0x15) + 0x81, 0x02020001);
		{
			const uintptr_t A = AddProperty(C, "ArrayProperty", "Items" + Suffix, 0x10, Base + 0x18);
			const uintptr_t Inner = NewProperty(A, false, "ObjectProperty", "Items" + Suffix, 8, 0);
			W<uintptr_t>(Inner + 0x88, Actor);
			W<uintptr_t>(A + 0x88, Inner);
		}
		W<uintptr_t>(AddProperty(C, "ByteProperty", "Role" + Suffix, 1, Base + 0x28) + 0x88, NetRole);
		AddProperty(C, "ByteProperty", "RawByte" + Suffix, 1, Base + 0x29);
		{
			const uintptr_t E = AddProperty(C, "EnumProperty", "Mode" + Suffix, 1, Base + 0x2A);
			const uintptr_t U = NewProperty(E, false, "ByteProperty", "UnderlyingType", 1, 0);
			W<uintptr_t>(E + 0x80, U); W<uintptr_t>(E + 0x88, Mode);
		}
		{
			const uintptr_t M = AddProperty(C, "MapProperty", "Stats" + Suffix, 0x50, Base + 0x30);
			W<uintptr_t>(M + 0x88, NewProperty(M, false, "NameProperty", "Stats_Key", 8, 0));
			W<uintptr_t>(M + 0x90, NewProperty(M, false, "IntProperty", "Stats_Value", 4, 0));
		}
		{
			const uintptr_t S = AddProperty(C, "SetProperty", "Tags" + Suffix, 0x50, Base + 0x80);
			W<uintptr_t>(S + 0x80, NewProperty(S, false, "NameProperty", "Tags", 8, 0));
		}
		AddProperty(C, "StrProperty", "Nick" + Suffix, 0x10, Base + 0xD0);
		AddProperty(C, "NameProperty", "Tag" + Suffix, 8, Base + 0xE0);
		{
			const uintptr_t P = AddProperty(C, "ClassProperty", "ActorClass" + Suffix, 8, Base + 0xE8, 0x1 | 0x10000000000000ULL /* UObjectWrapper */);
			W<uintptr_t>(P + 0x88, ClassClass); W<uintptr_t>(P + 0x90, Actor);
		}
		W<uintptr_t>(AddProperty(C, "DelegateProperty", "OnDied" + Suffix, 0x10, Base + 0xF0) + 0x88, OnDied);
		AddProperty(C, "IntProperty", "Health" + Suffix, 4, Base + 0x100);
		AddProperty(C, "FloatProperty", "Speed" + Suffix, 4, Base + 0x104);
		AddProperty(C, "TextProperty", "DisplayName" + Suffix, 0x18, Base + 0x108);
		AddProperty(C, "IntProperty", "Ammo" + Suffix, 4, Base + 0x120, 0x1, 4); // C-array
		{
			const uintptr_t F = NewFunction(C, "TakeDamage" + Suffix, FUNC_Native | FUNC_Public | FUNC_BlueprintCallable);
			AddParam(F, "FloatProperty", "Amount", 4, false);
		}
	}

	/* A wide (UTF-16) name */
	NewClass(Game, "\xE6\xB5\x8B\xE8\xAF\x95", ObjectClass, 0x28);

	/*
	* Layouts seen in the real Delta Force SDK that didn't compile. The generated SDK has static_asserts for every size/offset,
	* run.sh compiles it (also for arm64-apple-ios), so a wrong layout fails the harness.
	*/
	auto NewEnum = [&](uintptr_t Package, const std::string& Name, const std::vector<std::pair<std::string, int64_t>>& Values) -> uintptr_t
	{
		const uintptr_t E = NewObject(0x60, EnumClass, Package, Name);
		const uintptr_t Data = Alloc(16 * Values.size());
		for (size_t i = 0; i < Values.size(); i++) { W<uint32_t>(Data + i * 16, NameId(Values[i].first)); W<int64_t>(Data + i * 16 + 8, Values[i].second); }
		W<uintptr_t>(E + 0x40, Data); W<int32_t>(E + 0x48, static_cast<int32_t>(Values.size())); W<int32_t>(E + 0x4C, static_cast<int32_t>(Values.size()));
		return E;
	};
	auto NewDerivedStruct = [&](uintptr_t Package, const std::string& Name, uintptr_t Super, int32_t Size, int32_t Align) -> uintptr_t
	{
		const uintptr_t S = NewStruct(Package, Name, Size, Align);
		StructInfo(S, Super, Size, Align);
		return S;
	};
	/* FBoolProperty { FieldSize, ByteOffset, ByteMask, FieldMask }: native bool = { 1, 0, 1, 0xFF }, bitfield = { 1, 0, Mask, Mask } */
	auto AddBool = [&](uintptr_t Owner, const std::string& Name, int32_t Offset, uint8_t Mask) { W<uint32_t>(AddProperty(Owner, "BoolProperty", Name, 1, Offset) + 0x81, Mask == 0xFF ? 0xFF010001u : (0x1u | (uint32_t(Mask) << 16) | (uint32_t(Mask) << 24))); };
	auto AddObject = [&](uintptr_t Owner, const std::string& Name, int32_t Offset, uintptr_t Class) { W<uintptr_t>(AddProperty(Owner, "ObjectProperty", Name, 8, Offset) + 0x88, Class); };

	/* Enums: 'Unknown = -1' (was printed as 18446744073709551615) and 'EFoo_MAX = 256' in a uint8 enum */
	const uintptr_t SwitchEnum = NewEnum(Game, "EDFSwitch", { { "EDFSwitch::Unknown", -1 }, { "EDFSwitch::Off", 0 }, { "EDFSwitch::On", 1 }, { "EDFSwitch::EDFSwitch_MAX", 2 } });
	const uintptr_t LoadingEnum = NewEnum(Game, "EDFLoading", { { "EDFLoading::Inherited", 0 }, { "EDFLoading::RetainOnLoad", 1 }, { "EDFLoading::Uninitialized", 255 }, { "EDFLoading::EDFLoading_MAX", 256 } });
	{
		const uintptr_t Holder = NewStruct(Game, "DFEnumHolder", 0x2, 1);
		W<uintptr_t>(AddProperty(Holder, "ByteProperty", "Switch", 1, 0x0) + 0x88, SwitchEnum);
		W<uintptr_t>(AddProperty(Holder, "ByteProperty", "Loading", 1, 0x1) + 0x88, LoadingEnum);
	}

	/* ScriptStruct inheritance reusing the tail padding of the base (FTTTrackBase/FTTPropertyTrack) */
	{
		const uintptr_t TrackBase = NewStruct(Game, "DFTrackBase", 0x18, 8);
		AddProperty(TrackBase, "NameProperty", "TrackName", 8, 0x8);
		AddBool(TrackBase, "bIsExternalCurve", 0x10, 0xFF);
		const uintptr_t PropertyTrack = NewDerivedStruct(Game, "DFPropertyTrack", TrackBase, 0x20, 8);
		AddProperty(PropertyTrack, "NameProperty", "PropertyName", 8, 0x14);
		const uintptr_t FloatTrack = NewDerivedStruct(Game, "DFFloatTrack", PropertyTrack, 0x28, 8);
		AddObject(FloatTrack, "CurveFloat", 0x20, ObjectClass);
	}

	/* A memberless class in between whose derived classes reuse padding (UBaseUIView -> UCommonHUDView -> URaidScreenMarkerView) */
	{
		const uintptr_t BaseView = NewClass(Game, "DFBaseView", ObjectClass, 0x38);
		AddObject(BaseView, "FadeAnim", 0x28, ObjectClass);
		AddBool(BaseView, "bCacheOn", 0x30, 0xFF);
		const uintptr_t OtherView = NewClass(Game, "DFOtherView", BaseView, 0x38);
		AddBool(OtherView, "bOther", 0x31, 0xFF);
		const uintptr_t HudView = NewClass(Game, "DFHudView", BaseView, 0x38);
		const uintptr_t RaidView = NewClass(Game, "DFRaidView", HudView, 0x40);
		AddBool(RaidView, "bNeedShowDistance", 0x32, 0xFF);
		AddObject(RaidView, "ProgressMID", 0x38, ObjectClass);
		const uintptr_t CountDownView = NewClass(Game, "DFCountDownView", HudView, 0x38);
		AddProperty(CountDownView, "IntProperty", "FinalTime", 4, 0x34);
	}

	/* A memberless class in between that doesn't reuse padding, below a class that does (UParticleModule -> ...SubUVBase -> ...SubUV) */
	{
		const uintptr_t Module = NewClass(Game, "DFModule", ObjectClass, 0x30);
		AddBool(Module, "bSpawnModule", 0x28, 0x01);
		AddBool(Module, "bEnabled", 0x28, 0x02);
		AddProperty(Module, "ByteProperty", "LODValidity", 1, 0x2A);
		const uintptr_t ModuleColor = NewClass(Game, "DFModuleColor", Module, 0x38);
		AddProperty(ModuleColor, "FloatProperty", "Alpha", 4, 0x2C);
		const uintptr_t SubBase = NewClass(Game, "DFModuleSubBase", Module, 0x30);
		const uintptr_t Sub = NewClass(Game, "DFModuleSub", SubBase, 0x40);
		AddObject(Sub, "Animation", 0x30, ObjectClass);
		AddBool(Sub, "bUseRealTime", 0x38, 0x01);
		const uintptr_t SubMovie = NewClass(Game, "DFModuleSubMovie", Sub, 0x40);
		AddBool(SubMovie, "bUseEmitterTime", 0x39, 0xFF);
		AddProperty(SubMovie, "IntProperty", "FrameRate", 4, 0x3C);
	}

	/* Two packages that need each other's structs inside of containers (GPGameplay <-> WeaponDataSystem) */
	{
		const uintptr_t PkgA = NewObject(0x40, PackageClass, 0, "/Script/DFPkgA");
		const uintptr_t PkgB = NewObject(0x40, PackageClass, 0, "/Script/DFPkgB");

		const uintptr_t Inner = NewStruct(PkgA, "DFAInner", 0x8, 4);
		AddProperty(Inner, "IntProperty", "Value", 4, 0x0);
		AddProperty(Inner, "IntProperty", "Extra", 4, 0x4);

		const uintptr_t Modify = NewStruct(PkgB, "DFBModify", 0x10, 8);
		AddProperty(Modify, "IntProperty", "Id", 4, 0x0);
		AddProperty(Modify, "FloatProperty", "Scale", 4, 0x4);
		AddObject(Modify, "Source", 0x8, ObjectClass);

		const uintptr_t Context = NewStruct(PkgA, "DFAContext", 0x58, 8);
		AddProperty(Context, "IntProperty", "RecId", 4, 0x0);
		{
			const uintptr_t M = AddProperty(Context, "MapProperty", "RuntimeFunctions", 0x50, 0x8);
			W<uintptr_t>(M + 0x88, NewProperty(M, false, "IntProperty", "RuntimeFunctions_Key", 4, 0));
			const uintptr_t Value = NewProperty(M, false, "StructProperty", "RuntimeFunctions_Value", 0x10, 0);
			W<uintptr_t>(Value + 0x88, Modify);
			W<uintptr_t>(M + 0x90, Value);
		}

		const uintptr_t List = NewStruct(PkgB, "DFBList", 0x18, 8);
		{
			const uintptr_t A = AddProperty(List, "ArrayProperty", "Items", 0x10, 0x0);
			const uintptr_t Item = NewProperty(A, false, "StructProperty", "Items", 0x8, 0);
			W<uintptr_t>(Item + 0x88, Inner);
			W<uintptr_t>(A + 0x88, Item);
		}
		AddProperty(List, "IntProperty", "Count", 4, 0x10);

		/* Enum inside of a map key of the other package (TMap<EHeroShapeShiftType, ...>) */
		const uintptr_t Shape = NewEnum(PkgA, "EDFShape", { { "EDFShape::Normal", 0 }, { "EDFShape::Big", 1 }, { "EDFShape::EDFShape_MAX", 2 } });
		const uintptr_t FaceAnim = NewStruct(PkgB, "DFFaceAnim", 0x50, 8);
		{
			const uintptr_t M = AddProperty(FaceAnim, "MapProperty", "DefaultFaceAnim", 0x50, 0x0);
			const uintptr_t Key = NewProperty(M, false, "ByteProperty", "DefaultFaceAnim_Key", 1, 0);
			W<uintptr_t>(Key + 0x88, Shape);
			W<uintptr_t>(M + 0x88, Key);
			W<uintptr_t>(M + 0x90, NewProperty(M, false, "IntProperty", "DefaultFaceAnim_Value", 4, 0));
		}

		/* Game specific property type the dumper doesn't know (PropertyFixup.hpp) */
		const uintptr_t Holder = NewClass(PkgB, "DFEncryptedHolder", ObjectClass, 0x30);
		AddProperty(Holder, "EncryptedObjectProperty", "EncryptedOwner", 8, 0x28);
	}

	/* CDOs for every class (after all classes exist) + FText defaults on DFCharacter CDOs */
	const size_t NumBeforeCDOs = Objects.size();
	for (size_t i = 0; i < NumBeforeCDOs; i++)
	{
		const uintptr_t Obj = Objects[i];
		if (*reinterpret_cast<uintptr_t*>(Obj + 0x08) != ClassClass) continue;

		std::string ClassName;
		for (auto& [Name, Id] : NameIds) if (Id == *reinterpret_cast<uint32_t*>(Obj + 0x1C)) ClassName = Name;

		const int32_t Size = *reinterpret_cast<int32_t*>(Obj + 0x3C);
		const uintptr_t CDO = NewObject(std::max(Size, 0x28) + 0x40, Obj, *reinterpret_cast<uintptr_t*>(Obj + 0x10), "Default__" + ClassName, RF_Public | RF_ClassDefaultObject);
		W<uint32_t>(CDO + 0x18, ObjectFlagsValue(RF_Public | RF_ClassDefaultObject, Objects.size()));
		W<uintptr_t>(Obj + 0x118, CDO);        // UClass::ClassDefaultObject
		W<uint64_t>(Obj + 0xE0, 0x0);           // UClass::CastFlags (filled below)

		if (ClassName.rfind("DFCharacter", 0) == 0)
		{
			static const char16_t Text[] = u"Operator";
			const uintptr_t TextData = Alloc(0x40);
			const uintptr_t String = Alloc(sizeof(Text));
			memcpy(reinterpret_cast<void*>(String), Text, sizeof(Text));
			W<uintptr_t>(TextData + 0x28, String); W<int32_t>(TextData + 0x30, 9); W<int32_t>(TextData + 0x34, 9);
			const int32_t TextOffset = ClassName == "DFCharacter" ? 0x280 + 0x108 : 0x400 + 0x108;
			W<uintptr_t>(CDO + TextOffset, TextData);
		}
	}

	/* Real cast flags (the dumper uses name based flags, the finder only reports the offset) */
	W<uint64_t>(Actor + 0xE0, 0x0000001000000000ULL);
	W<uint64_t>(ClassClass + 0xE0, 0x1 | 0x8 | 0x20);

	/* World instance + GWorld */
	const uintptr_t LobbyPackage = NewObject(0x80, PackageClass, 0, "/Game/Maps/Lobby");
	GFakeWorld = NewObject(0x700, World, LobbyPackage, "Lobby");
	W<uintptr_t>(GWorldPtr, GFakeWorld);

	/* Numbered names */
	for (int i = 1; i <= 20; i++)
		NewObject(0x220 + 0x40, Character, GFakeWorld, "DFCharacter", RF_Public, i);

	W<int32_t>(GObjectsArray + 0x14, static_cast<int32_t>(Objects.size()));

	printf("[harness] image %p, GNames RVA 0x%lx, GObjects RVA 0x%lx, %zu objects, %zu names, cursor 0x%x\n", Image,
		(unsigned long)(Pool - reinterpret_cast<uintptr_t>(Image)), (unsigned long)(GObjectsArray - reinterpret_cast<uintptr_t>(Image)), Objects.size(), NameIds.size(), Cursor);
}
