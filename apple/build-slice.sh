#!/usr/bin/env bash
# Builds one Apple slice of jowe_convert and merges it with its dependencies into a single static library.
set -euo pipefail

slice="$1"
out="$2"
ios_min=16.0
mac_min=12.0

case "$slice" in
  ios-arm64)
    args=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_SYSTEM_PROCESSOR=arm64 -DCMAKE_OSX_SYSROOT=iphoneos
          -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$ios_min) ;;
  iossim-arm64|iossim-x86_64)
    # kvazaar's CMake needs CMAKE_SYSTEM_PROCESSOR, which the iOS platform leaves empty.
    args=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_SYSTEM_PROCESSOR=${slice#iossim-} -DCMAKE_OSX_SYSROOT=iphonesimulator
          -DCMAKE_OSX_ARCHITECTURES=${slice#iossim-} -DCMAKE_OSX_DEPLOYMENT_TARGET=$ios_min) ;;
  catalyst-arm64|catalyst-x86_64)
    arch=${slice#catalyst-}
    target="-target $arch-apple-ios$ios_min-macabi"
    args=(-DCMAKE_OSX_SYSROOT=macosx -DCMAKE_OSX_ARCHITECTURES=$arch
          "-DCMAKE_C_FLAGS=$target" "-DCMAKE_CXX_FLAGS=$target") ;;
  macos-arm64|macos-x86_64)
    args=(-DCMAKE_OSX_ARCHITECTURES=${slice#macos-} -DCMAKE_OSX_DEPLOYMENT_TARGET=$mac_min) ;;
  *)
    echo "unknown slice $slice" >&2
    exit 2 ;;
esac

build="build-$slice"
cmake -S . -B "$build" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DJOWE_CONVERT_BUILD_CLI=OFF -DJOWE_CONVERT_BUILD_TESTS=OFF "${args[@]}"
cmake --build "$build" --target jowe_convert --parallel "$(sysctl -n hw.ncpu)"

mkdir -p "$out"
libs=$(find "$build" -name '*.a' -not -path '*/CMakeFiles/*' | sort)
echo "$libs"
# shellcheck disable=SC2086
libtool -static -no_warning_for_no_symbols -o "$out/libjowe_convert.a" $libs
lipo -info "$out/libjowe_convert.a"
