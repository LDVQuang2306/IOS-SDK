# Linux harness (Delta Force)

Runs the real dumper code (engine core + all generators) on Linux against a synthetic `DeltaForceClient` image that uses
the Delta Force layout: obfuscated FNamePool (Blocks @0xC8, Cursor @0x100C8, CurrentBlock @0x100CC, 18 block bits),
reordered UObject/UStruct/FField/FProperty and the DF GUObjectArray.

```sh
./Tests/LinuxHarness/run.sh                                   # needs g++ (ASan is used if available)
IOS_SDK=/path/to/iPhoneOS16.5.sdk ./Tests/LinuxHarness/run.sh  # + compile the generated SDK for iOS
```

The object graph of a real dump can be replayed as well. `make_world.py` reads a Dumper-7 output folder
(`GObjects-Dump-WithProperties.txt` + `CppSDK/`) and writes every object, package, class/struct (super, size, alignment),
property (kind, offset, size, flags, types), function and enum into a JSON file that the harness rebuilds in the DF layout:

```sh
python3 Tests/LinuxHarness/make_world.py ~/Downloads/1.203.37117_65-DeltaForce world.json
DF_HARNESS_WORLD=$PWD/world.json ./Tests/LinuxHarness/run.sh
```

`DF_HARNESS_UNLOAD=N` / `DF_HARNESS_UNLOAD_LATE=N` remove every Nth object from GObjects right after the generator's snapshot /
while the SDK is written, like a garbage collection of the running game.

`shim/` provides the few Mach/dyld APIs the dumper uses (`vm_read_overwrite` -> `process_vm_readv`).
The harness is not part of the theos build.
