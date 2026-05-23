#!/bin/bash

rm -rf ./conan

conan install ./build-scripts/conanfile.py -u -pr:b ./build-scripts/win/profile_win2022 -pr:h ./build-scripts/win/profile_win2022 -of ./conan
