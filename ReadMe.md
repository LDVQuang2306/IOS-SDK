# iOS-Dumper-7_THEOS

**iOS-Dumper-7** is a runtime SDK generator for Unreal Engine games running on iOS. It injects into the game process to dynamically analyze memory, resolve offsets, and generate a full C++ SDK, IDA scripts, and symbol dumps.

## Features

* **Runtime Generation**: Generates C++ SDK headers, IDA Mappings (`.idmap`), and JSON dumps directly on the device.
* **Dynamic Offset Scanning**: Automatically finds `GObjects`, `GNames`, and `UWorld` without requiring hardcoded offsets for most games.
* **Broad Compatibility**: Tested on Unreal Engine versions **4.17** to **4.26** (e.g., ARK 2.0, Ark Revamp, Special Forces 3).
* **Floating UI**: Uses a draggable floating button to toggle the menu, avoiding conflict with game gestures.

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

---

## Configuration & Overrides

If the dumper fails to find offsets automatically (common in games with encryption or obfuscation), you can manually configure overrides in **`Dumper/Generator/Private/Generators/Generator.cpp`** inside the `Generator::InitEngineCore()` function.

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


* Contributions are Highly Appreciated for more improvements!

## TODO

- Find ProcessEvent Offset in the Memory (Not Manual Overwrites)
- Find NamesArray (For UE below 4.22) in Memory
- Fix Fallback Methods in Finding FNames (AppendString at UnrealTypes.cpp)
- Tool have something error

