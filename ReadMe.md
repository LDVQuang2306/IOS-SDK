# iOS-Dumper-7_THEOS

**iOS-Dumper-7** is a runtime SDK generator for Unreal Engine games running on iOS. It injects into the game process to dynamically analyze memory, resolve offsets, and generate a full C++ SDK, IDA scripts, and symbol dumps.

## Features

* **Runtime Generation**: Generates C++ SDK headers, IDA Mappings (`.idmap`), and JSON dumps directly on the device.
* **Dynamic Offset Scanning**: Automatically finds `GObjects`, `GNames`, and `UWorld` without requiring hardcoded offsets for most games.
* **Broad Compatibility**: Tested on Unreal Engine versions **4.17** to **4.26** (e.g., ARK 2.0, Ark Revamp, Special Forces 3).
* **Floating UI**: Uses a draggable floating button to toggle the menu, avoiding conflict with game gestures.
* **Delta Force support**: `DeltaForceClient` is detected automatically and dumped with a dedicated, validated engine profile (see below).

## Usage

### 1. Build

1. Open the project on device install theos.
2. Open folder open terminal and 'make'.
3. project to generate the folder '.theos/obj' `.dylib` file (e.g., `LDVQuangDumper.dylib`).

### 2. Inject

Use any signer (Sideloadly, ESign, GBox or whatever Signer that supports dylib injection) and inject the dylib

### 3. Dump

1. **Launch the game** and wait for the engine to fully load.
2. After approximately **3 seconds**, a **Floating Logo Button** will appear on the screen.
3. **Tap the Logo** to open the ImGui overlay.
* *Note: You can drag the logo to move it if it obstructs the game UI.*


4. Tap **Start Dump** in the menu.
5. Wait for the process to complete.

### 4. Output

The generated files will be saved to your device's Documents directory (Make Sure to enable "Supports Document Browser" before signing):
`/Documents/[GameVersion-GameName]/`

A full log of every run is written to `/Documents/Dumper-7.log` ("Copy to Clipboard" in the menu copies the console to the iOS pasteboard).

---

## Delta Force (`DeltaForceClient`)

Delta Force ships a modified UE4 core, which is why the generic Dumper-7 path could not dump it (and crashed while trying):

| | Generic UE4 | Delta Force |
|---|---|---|
| FNameEntry strings | plain | XOR obfuscated (per-length key) |
| FNamePool | `CurrentBlock`, `Cursor`, `Blocks[]` | `Blocks[]` @0xC8, `Cursor` @0x100C8, `CurrentBlock` @0x100CC, 18 block bits |
| UObject | Flags 0x8, Index 0xC, Class 0x10, Name 0x18, Outer 0x20 | Class 0x8, Outer 0x10, Flags 0x18, Name 0x1C, Index 0x24 |
| UStruct | Super 0x40, Children 0x48, ChildProperties 0x50, Size 0x58 | Size 0x3C, Super 0x40, MinAlignment 0x48, Children 0x50, ChildProperties 0x68 |
| FField / FProperty | Class 0x8, Next 0x20, Name 0x28 / ArrayDim 0x30 ... | Next 0x18, Class 0x20, Name 0x28 / ArrayDim 0x38, ElementSize 0x3C, Flags 0x40, Offset 0x4C |
| GUObjectArray | Objects 0x0, NumElements 0x14 | ObjObjects @0x10: NumElements 0x14, NumChunks 0x18, Objects 0x20 |

The layout comes from the validated reference dumper (`DFSDKDumper`). When the game is detected (`Engine/Private/Unreal/DeltaForce.cpp`):

1. **GNames/GObjects are discovered, not hard-coded**: the data segments are scanned, every candidate must decode `FName[0] == "None"` and hold the `CoreUObject` anchors at their `InternalIndex`. The name codec and the UObject header/FUObjectItem layout are inferred and must be unique, otherwise the dump is refused instead of guessing.
2. **Every read before verification goes through the kernel** (`vm_read_overwrite`), so a wrong candidate can't crash the game.
3. The deeper layout (UStruct, UFunction, UEnum, FField, FProperty) is **verified on live data** (`Class->Struct->Field->Object`, `FGuid{A,B,C,D}`, `AActor` functions, `ENetRole`) before the generator dereferences anything. If a game update changed it, the dump stops with an error in the console.
4. FProperty payload offsets (Struct/PropertyClass/Inner/Enum/Key/Value/...) and the bool layout are re-checked against real properties.
5. Cast flags are resolved from class names (no unverified `UClass::CastFlags` offset). Nothing calls game code (no ProcessEvent during the dump).
6. The SDK contains the name decryption (`DeltaForceNames::DecryptAnsi/DecryptWide`) and the Delta Force FNamePool layout, so `FName::ToString()` works in the generated SDK.

### Generated SDK (clang / iOS)

The generated SDK is built with Apple's C++ ABI in mind and every struct/class keeps its `static_assert`s for size, alignment and member offsets, so a wrong layout is a compile error instead of a silent wrong read:

* **Tail padding reuse**: the game places members of a derived struct/class inside the tail padding of its base (e.g. `FTTPropertyTrack::PropertyName` @0x14 inside `FTTTrackBase`, `URaidScreenMarkerView::bNeedShowDistance` @0x3EA). clang only does that for non-POD bases, so those bases get a user-provided constructor. Every other type is padded up to its full size, and types without own members (e.g. `UCommonHUDView`, `ADFMWeaponC4`) are padded too, so derived types always start where the game starts them.
* **Enums**: negative values make the enum signed (`ESwitchOnOff : int8 { Unknown = -1 }`), reflection-only `_MAX` values that don't fit the real type (`= 256` in a `uint8` enum) are emitted as a comment.
* **Cyclic packages**: structs used inside `TArray/TSet/TMap` of a package that can't be included (e.g. `TMap<uint64, FWeaponDataModifyFunction>`) use `TStructCycleFixup<...>` like plain struct members, and enums used inside containers are forward declared (`TMap<EHeroShapeShiftType, ...>`).
* `TUObjectArray` only contains the members whose offset is known, `PropertyFixup.hpp` uses `unsigned char` (no MSVC `__int8`), `UObject::IsDefaultObject()` checks the `Default__` name when `UObject::Flags` can't be identified.

To check a dump, build its SDK for arm64 iOS with theos (see `Tests/SDKTheos/Makefile`):

```sh
cd Tests/SDKTheos
make SDK_DIR=/path/to/1.203.37117_65-DeltaForce/CppSDK            # Basic.cpp + CoreUObject/Engine functions
make SDK_DIR=/path/to/1.203.37117_65-DeltaForce/CppSDK SDK_ALL=1  # every <Package>_functions.cpp
```

`Tests/LinuxHarness/run.sh` dumps a synthetic Delta Force process that contains all of the layouts above and compiles the generated SDK (set `IOS_SDK=/path/to/iPhoneOS.sdk` to also compile it for `arm64-apple-ios`).

Usage: open the game, **wait until the lobby is fully loaded**, then press *Start Dump*. If it reports that GNames/GObjects were not found, wait a bit longer and press it again (nothing is written before the engine core validated).

Optional manual override (validated before use) in `Settings.h`:

```cpp
namespace Settings::DeltaForce
{
    inline uint64_t GNamesRVA = 0x18445040;   // 0 = scan
    inline uint64_t GObjectsRVA = 0x18881AB8; // 0 = scan
}
```

---

## Configuration & Overrides

If the dumper fails to find offsets automatically (common in games with encryption or obfuscation), you can manually configure overrides in **`Generator/Private/Generators/Generator.cpp`** inside the `Generator::InitEngineCore()` function.

`Settings.h` → `UEVERSION` (default `426`) selects the string type: `>= 421` uses UTF-16 `char16_t` (every FNamePool/FProperty game incl. Delta Force), below that `wchar_t` for old UE4.17-4.20 iOS builds.

### 1. GObjects (Global Object Array)

If the auto-scan fails, provide the address and layout manually:

```cpp
// For older UE4 (Fixed Layout)
ObjectArray::Init(0x12345678, FFixedUObjectArrayLayout {
    .ObjectsOffset = 0x0,
    .MaxObjectsOffset = 0x8,
    .NumObjectsOffset = 0xC
});

// For UE4.21+ / UE5 (Chunked Layout)
ObjectArray::Init(0x12345678, 0x10000 /* ElementsPerChunk */, FChunkedFixedUObjectArrayLayout {
    .ObjectsOffset = 0x00,
    .MaxElementsOffset = 0x10,
    .NumElementsOffset = 0x14,
    .MaxChunksOffset = 0x18,
    .NumChunksOffset = 0x1C
});

```

### 2. GNames (Global Name Array)

If `GNames` is not found via pattern scanning, initialize it manually:

```cpp
// Address, Mode, bIsNamePool, ModuleName (Optional)
FName::Init(0x10203040, FName::EOffsetOverrideType::GNames, true /* true for NamePool */, "UAGame");

```

### 3. Pointer Decryption

For games that encrypt pointers (e.g. IDK What Games Have ObjectArray Encrypted), define a decryption lambda:

```cpp
// Example: XOR decryption
ObjectArray::InitDecryption([](void* ObjPtr) -> uint8* {
    return reinterpret_cast<uint8*>(uint64(ObjPtr) ^ 0x8375ACDE);
});

```

### 4. ProcessEvent

`ProcessEvent` is located with an ARM64 heuristic (loads `UFunction::FunctionFlags` and tests `FUNC_Native`/`FUNC_HasOutParms`). The value is only written to the SDK, the dumper never calls it.
If the virtual table index for `ProcessEvent` is incorrect:

```cpp
// Manually set the VTable index
Off::InSDK::ProcessEvent::InitPE(69); 

```

---

## Credits

* **Encryqed**: Original creator of [Dumper-7](https://github.com/Encryqed/Dumper-7).
* **Aethereux**: Ported and adapted for iOS/ARM64 [upload ios dumper] (https://github.com/Aethereux/iOS-Dumper-7).
* **LDVQuang2306**: Convert xcode to theos
* **DFSDKDumper**: Delta Force layout, name codec and discovery/validation logic (ported into `Engine/Private/Unreal/DeltaForce*.cpp`)


* Contributions are Highly Appreciated for more improvements!

## TODO

- Find NamesArray (For UE below 4.22) in Memory
- Fix Fallback Methods in Finding FNames (AppendString at UnrealTypes.cpp)

