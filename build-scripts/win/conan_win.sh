#!/bin/bash

rm -rf ./conan

conan install ./build-scripts/conanfile.py --update -pr ./build-scripts/win/profile_win2019 -if ./conan
