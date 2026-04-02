#!/bin/bash

USE_ICX=false
for arg in "$@"; do
  case $arg in
    --icx) USE_ICX=true; shift ;;
    *) ;;
  esac
done

rm -rf ./conan

if [ "$USE_ICX" = true ]; then
  PROFILE="./build-scripts/win/profile_win2022_icx"
  BUILD_FLAGS="--build=zimg --build=libvpx --build=libaom-av1"
else
  PROFILE="./build-scripts/win/profile_win2022"
  BUILD_FLAGS=""
fi

conan install ./build-scripts/conanfile.py -u \
  -pr:b ./build-scripts/win/profile_win2022 \
  -pr:h "$PROFILE" \
  -of ./conan \
  $BUILD_FLAGS
