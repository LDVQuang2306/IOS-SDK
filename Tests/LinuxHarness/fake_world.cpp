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

#include <mach-o/loader.h>
#include <mach-o/dyld.h>

#include "Engine/Public/Unreal/DeltaForce.h"

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
	size_t HeapUsed = 0;
	size_t DataUsed = 0;

	template<typename T> void W(uintptr_t Address, T Value) { memcpy(reinterpret_cast<void*>(Address), &Value, sizeof(T)); }

	uintptr_t Alloc(size_t Size, size_t Align = 16)
	{
		HeapUsed = (HeapUsed + Align - 1) & ~(Align - 1);
		const uintptr_t Result = reinterpret_cast<uintptr_t>(Heap) + HeapUsed;
		HeapUsed += Size;
		if (HeapUsed > 0x8000000) { fprintf(stderr, "heap exhausted\n"); abort(); }
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
	uintptr_t Pool = 0, Block0 = 0;
	uint32_t Cursor = 0;
	std::map<std::string, uint32_t> NameIds;

	uint32_t NameId(const std::string& Name)
	{
		auto It = NameIds.find(Name);
		if (It != NameIds.end()) return It->second;

		const uint32_t Offset = Cursor;
		const bool bWide = Name == "\xE6\xB5\x8B\xE8\xAF\x95"; // "测试" -> wide entry

		if (bWide)
		{
			uint16_t Text[2] = { 0x6D4B, 0x8BD5 };
			DeltaForce::DecryptWide(Text, 2); // XOR codec is its own inverse
			W<uint16_t>(Block0 + Offset, static_cast<uint16_t>((2 << 6) | 1));
			memcpy(reinterpret_cast<void*>(Block0 + Offset + 2), Text, 4);
			Cursor += 2 + 4;
		}
		else
		{
			std::vector<uint8_t> Text(Name.begin(), Name.end());
			DeltaForce::DecryptAnsi(Text.data(), Text.size());
			W<uint16_t>(Block0 + Offset, static_cast<uint16_t>(Name.size() << 6));
			memcpy(reinterpret_cast<void*>(Block0 + Offset + 2), Text.data(), Text.size());
			Cursor += static_cast<uint32_t>((2 + Name.size() + 1) & ~1u);
		}

		W<uint32_t>(Pool + 0x100C8, Cursor);
		return NameIds[Name] = Offset / 2;
	}

	/* ---- objects ---- */
	uintptr_t GObjectsArray = 0, Chunk0 = 0;
	std::vector<uintptr_t> Objects;
	uintptr_t Vtable = 0;

	constexpr uint32_t RF_Public = 0x1, RF_Native = 0x0, RF_ClassDefaultObject = 0x10;

	uintptr_t NewObject(size_t Size, uintptr_t Class, uintptr_t Outer, const std::string& Name, uint32_t Flags = RF_Public, uint32_t Number = 0)
	{
		const uintptr_t Obj = Alloc(Size);
		W<uintptr_t>(Obj + 0x00, Vtable);
		W<uintptr_t>(Obj + 0x08, Class);
		W<uintptr_t>(Obj + 0x10, Outer);
		W<uint32_t>(Obj + 0x18, Flags);
		W<uint32_t>(Obj + 0x1C, NameId(Name));
		W<uint32_t>(Obj + 0x20, Number);
		W<int32_t>(Obj + 0x24, static_cast<int32_t>(Objects.size()));
		W<uintptr_t>(Chunk0 + Objects.size() * 0x18, Obj);
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
}

uintptr_t GFakeWorld = 0;

void BuildFakeDeltaForce()
{
	Image = static_cast<uint8_t*>(mmap(nullptr, ImageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	Heap = static_cast<uint8_t*>(mmap(nullptr, 0x8000000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));

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
	Block0 = Alloc(0x80000, 0x1000);
	W<uintptr_t>(Pool + 0xC8, Block0);
	W<uint32_t>(Pool + 0x100CC, 0); // CurrentBlock
	NameId("None");
	NameId("ByteProperty");

	GObjectsArray = DataAlloc(0x80, 64);
	Chunk0 = Alloc(0x10000 * 0x18, 0x1000);
	const uintptr_t ChunkTable = Alloc(0x100 * 8);
	W<uintptr_t>(ChunkTable, Chunk0);
	W<int32_t>(GObjectsArray + 0x10, 0x210000); // MaxElements
	W<int32_t>(GObjectsArray + 0x18, 1);        // NumChunks
	W<int32_t>(GObjectsArray + 0x1C, 0x21);     // MaxChunks
	W<uintptr_t>(GObjectsArray + 0x20, ChunkTable);

	const uintptr_t GWorldPtr = DataAlloc(8, 8);

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
		W<uint32_t>(CDO + 0x18, RF_Public | RF_ClassDefaultObject);
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
