#!/bin/bash
# Runs the Delta Force dump path on Linux against a synthetic DeltaForceClient image (no device needed):
#   1. builds the engine + generators with Mach/dyld shims (AddressSanitizer when available)
#   2. dumps the synthetic image (encrypted FNamePool, DF UObject/UStruct/FField layout) into build/home/Documents
#   3. compiles the generated SDK and uses it to read the synthetic image back
#   4. optional: IOS_SDK=/path/to/iPhoneOS.sdk also compiles the generated SDK for arm64-apple-ios
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$HERE/build"
mkdir -p "$BUILD/obj"

CXX=${CXX:-g++}
CC=${CC:-gcc}
SAN=""
if echo 'int main(){}' | $CXX -x c++ -fsanitize=address -o "$BUILD/asan_probe" - 2>/dev/null; then SAN="-fsanitize=address -fno-omit-frame-pointer"; fi

INC="-I$ROOT -I$ROOT/Engine/Public -I$ROOT/Engine/Public/Unreal -I$ROOT/Engine/Public/OffsetFinder -I$ROOT/Generator/Public -I$ROOT/Generator/Public/Generators -I$ROOT/Generator/Public/Managers -I$ROOT/Generator/Public/Wrappers -I$ROOT/Platform/Public -I$ROOT/Platform/Private -I$ROOT/Utils -I$ROOT/fmt"
FLAGS="-g -O1 -std=gnu++20 -w $SAN -I$HERE/shim $INC"
SOURCES="$(cd "$ROOT" && ls Utils/Dumpspace/*.cpp Generator/Private/*.cpp Generator/Private/Generators/*.cpp Generator/Private/Managers/*.cpp Generator/Private/Wrappers/*.cpp Engine/Private/OffsetFinder/*.cpp Engine/Private/Unreal/*.cpp Platform/Private/*.cpp) Settings.cpp fmt/format.cc"

echo "[run.sh] building dumper ($CXX $SAN)"
cd "$ROOT"
for f in $SOURCES "$HERE/shim.cpp" "$HERE/logger.cpp" "$HERE/fake_world.cpp" "$HERE/test_main.cpp"; do
  o="$BUILD/obj/$(echo "$f" | tr '/' '_').o"
  echo "$CXX $FLAGS -c $f -o $o"
done | xargs -P "$(nproc)" -I{} sh -c '{}'
$CC -g -O1 -w $SAN -c "$ROOT/Utils/Compression/zstd.c" -o "$BUILD/obj/zstd.o"
$CXX $SAN "$BUILD"/obj/*.o -o "$BUILD/harness"

echo "[run.sh] dumping synthetic Delta Force image"
rm -rf "$BUILD/home" && mkdir -p "$BUILD/home/Documents"
HOME="$BUILD/home" ASAN_OPTIONS=detect_leaks=0 "$BUILD/harness" | tee "$BUILD/harness.log" | grep -E "DeltaForce|ProcessEvent|GWorld|\[E\]|HARNESS"

echo "[run.sh] simulated game update with a changed UStruct layout must abort cleanly"
if HOME="$BUILD/home_changed" ASAN_OPTIONS=detect_leaks=0 DF_HARNESS_CHANGED_LAYOUT=1 "$BUILD/harness" > "$BUILD/harness_changed.log" 2>&1; then
  echo "expected the dump to be refused"; exit 1
fi
grep -q "Reflection layout check failed" "$BUILD/harness_changed.log" && grep "Reflection layout check failed" "$BUILD/harness_changed.log"

SDKDIR="$BUILD/home/Documents/1.0.0_Test-DeltaForce/CppSDK"
echo "[run.sh] compiling the generated SDK and reading the image back with it"
$CXX -std=c++20 -w -I"$SDKDIR" -I"$SDKDIR/SDK" -I"$HERE/shim" -I"$ROOT" -o "$BUILD/sdk_runtime_test" "$HERE/sdk_runtime_test.cpp" "$SDKDIR"/SDK/*.cpp \
    "$HERE/fake_world.cpp" "$HERE/shim.cpp" "$ROOT/Engine/Private/Unreal/DeltaForceDiscovery.cpp" "$HERE/logger.cpp" $INC
"$BUILD/sdk_runtime_test"

if [ -n "$IOS_SDK" ]; then
  echo "[run.sh] compiling the generated SDK for arm64-apple-ios"
  for f in "$SDKDIR"/SDK/*.cpp; do
    clang++ -target arm64-apple-ios14.0 -isysroot "$IOS_SDK" -std=c++20 -fsyntax-only -w -I"$SDKDIR" -I"$SDKDIR/SDK" "$f"
  done
fi

echo "[run.sh] OK"
