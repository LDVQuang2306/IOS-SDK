# iOS-Dumper-7_THEOS

**iOS-Dumper-7** is a runtime SDK generator for Unreal Engine games running on iOS. It injects into the game process, locates the engine's reflection data in memory and generates a full C++ SDK, IDA mappings, USMAP mappings, Dumpspace JSON and a `UEOffsets.hpp` summary.

This is the Theos build of [Aethereux/iOS-Dumper-7](https://github.com/Aethereux/iOS-Dumper-7) (itself a port of [Encryqed/Dumper-7](https://github.com/Encryqed/Dumper-7)), with additional work to make it dump **Delta Force** reliably and without crashing the game.

## Features

* **No hard-coded offsets needed**: `GObjects`, `GNames`, `ProcessEvent` and `GWorld` are searched at runtime.
* **Delta Force support**:
  * the reordered `FChunkedFixedUObjectArray` members are detected automatically (known layouts first, then a layout-agnostic search that works when a client update shuffles the members again),
  * the XOR-encrypted `FNameEntry` strings are detected from the `"None"` / `"ByteProperty"` entries at the start of the `FNamePool`, and the decryption is installed and emitted into the generated SDK automatically.
* **Data-anchored GNames search**: the `FNamePool` (UE4.23+) or `TNameEntryArray` (UE4.22 and below) is located through its data, not through code patterns that break with every update.
* **ARM64 ProcessEvent detection**: the UObject vtable is scored against ProcessEvent fingerprints. ProcessEvent is only ever called if it was identified with high confidence.
* **Crash safety**: every scan stays inside mapped memory, a failed step aborts the dump with an error message instead of taking the game down, and everything is logged to a file.

## Usage

### 1. Build

1. Install [Theos](https://theos.dev) (on device, macOS or Linux).
2. Open a terminal in the project folder and run `make` (or `make package`).
3. The dylib (`LDVQuangDumper.dylib`) is written to `.theos/obj/`.

### 2. Inject

Use any signer that supports dylib injection (Sideloadly, ESign, GBox, TrollStore + Choicy, ...) and inject the dylib.

### 3. Dump

1. **Launch the game** and wait until it is fully loaded (lobby / main menu).
2. About **3 seconds** after launch a floating button appears (a logo, or a dark `D7` button without network access).
3. **Tap the button** to open the menu. Drag it if it covers the game UI.
4. Tap **Start Dump** and wait until `Generating SDK took ...` is printed.

If the dump is started too early, it stops with an error ("GObjects wasn't found ..."), nothing is modified and **Start Dump** can simply be pressed again later.

### 4. Output

Everything is saved to the app's Documents directory (enable "Supports Document Browser" before signing to access it from the Files app):

```
Documents/
├── Dumper-7.log                        # full log of the last run, written while dumping
└── [GameVersion-GameName]/
    ├── CppSDK/                          # C++ SDK headers
    ├── Mappings/                        # USMAP
    ├── IDAMappings/                     # .idmap for IDA
    ├── Dumpspace/                       # Dumpspace JSON
    ├── GObjects-Dump.txt
    ├── GObjects-Dump-WithProperties.txt
    └── UEOffsets.hpp                    # every discovered offset in one header
```

If something goes wrong, `Dumper-7.log` (or **Copy to Clipboard** in the menu) contains the full log.

---

## Configuration & Overrides

Everything is auto-detected by default. Only if auto-detection fails (for example after a game update changed something new), set overrides in **`Generator/Private/Generators/Generator.cpp`** (`namespace DumperOverrides`). Overrides that don't validate are ignored and auto-detection is used instead, so an outdated offset can't crash the game.

```cpp
namespace DumperOverrides
{
    /* Offset of the TUObjectArray (FUObjectArray::ObjObjects), 0 = auto */
    constexpr int32 GObjectsOffset = 0x0;
    constexpr int32 GObjectsElementsPerChunk = 0x10000;

    /* Delta Force example layout */
    constexpr FChunkedFixedUObjectArrayLayout GObjectsLayout = FChunkedFixedUObjectArrayLayout{
        .ObjectsOffset = 0x20, .MaxElementsOffset = 0x10, .NumElementsOffset = 0x04, .MaxChunksOffset = 0x00, .NumChunksOffset = 0x14
    };

    /* FNamePool (true, UE4.23+) or the global TNameEntryArray* (false, UE4.22-), 0 = auto */
    constexpr int32 GNamesOffset = 0x0;
    constexpr bool bGNamesIsNamePool = true;

    /* Vtable index of UObject::ProcessEvent, -1 = auto */
    constexpr int32 ProcessEventIndex = -1;
}
```

### Decryption hooks

For games that encrypt other things, install a hook at the top of `Generator::InitEngineCore()` (before GObjects/GNames are searched). Delta Force doesn't need any of these.

```cpp
InitObjectArrayDecryption([](void* ObjPtr) -> uint8* { return reinterpret_cast<uint8*>(uint64(ObjPtr) ^ 0x8375); });
InitNameStringDecryption([](std::string Decoded) -> std::string { /* ... */ return Decoded; });
InitNameEntryDecryption([](uint8_t* Entry) -> uint8_t* { return Entry; });
InitNameArrayDecryption([](uintptr_t Start) -> uintptr_t { return Start; });
InitNamePoolDecryption([](uintptr_t Start) -> uintptr_t { return Start; });
```

### Other settings (`Settings.h`)

* `UEVERSION`: selects the `TCHAR` width of `FString` (UE4.21+ uses 16-bit characters on iOS, older versions 32-bit). Default `426`.
* `Settings::General::DefaultModuleName`: Mach-O image that contains the engine. `nullptr` = main executable (correct for Delta Force).
* `Settings::Config::bCallProcessEvent`: set to `false` if a game doesn't tolerate ProcessEvent calls from the dumper (only used to probe the FText layout and to log the engine version).

---

## Credits

* **Encryqed**: original creator of [Dumper-7](https://github.com/Encryqed/Dumper-7).
* **Aethereux**: iOS/ARM64 port ([iOS-Dumper-7](https://github.com/Aethereux/iOS-Dumper-7)), including the Delta Force name decryption and the ProcessEvent vtable scorer.
* **MJx0**: [AndUEDumper / iOS_UEDumper](https://github.com/MJx0/AndUEDumper), ProcessEvent scoring algorithm.
* **LDVQuang2306**: Xcode to Theos conversion.

Contributions are highly appreciated!
