#!/bin/bash

rm -rf ./conan_x64
rm -rf ./conan_arm

conan install ./build-scripts/conanfile.py --update -pr ./build-scripts/mac/profile_mac13.0 -if ./conan_x64/
conan install ./build-scripts/conanfile.py --update -pr ./build-scripts/mac/profile_mac_armv8 -if ./conan_arm/
