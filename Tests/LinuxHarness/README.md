# Linux harness (Delta Force)

Runs the real dumper code (engine core + all generators) on Linux against a synthetic `DeltaForceClient` image that uses
the Delta Force layout: obfuscated FNamePool (Blocks @0xC8, Cursor @0x100C8, CurrentBlock @0x100CC, 18 block bits),
reordered UObject/UStruct/FField/FProperty and the DF GUObjectArray.

```sh
./Tests/LinuxHarness/run.sh                                   # needs g++ (ASan is used if available)
IOS_SDK=/path/to/iPhoneOS16.5.sdk ./Tests/LinuxHarness/run.sh  # + compile the generated SDK for iOS
```

`shim/` provides the few Mach/dyld APIs the dumper uses (`vm_read_overwrite` -> `process_vm_readv`).
The harness is not part of the theos build.
