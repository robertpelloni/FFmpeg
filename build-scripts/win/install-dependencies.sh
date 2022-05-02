#!/bin/bash

set -e

if [[ -z "$DEPENDENCIES_DIR" ]]; then
	echo "DEPENDENCIES_DIR is not set. Aborting"
	exit 1
fi

pacman -S --noconfirm mingw-w64-x86_64-nasm mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2 make pkg-config diffutils yasm git wget
cd ${DEPENDENCIES_DIR}

mkdir -p nv_sdk
git clone https://git.videolan.org/git/ffmpeg/nv-codec-headers.git
cd nv-codec-headers
make install
cd ..

if [[ ! -z "$CUDA_PATH" ]]; then
	cp -r "${CUDA_PATH}/include/." "${DEPENDENCIES_DIR}/nv_sdk/"
	cp -r "${CUDA_PATH}/lib/x64/." "${DEPENDENCIES_DIR}/nv_sdk/"
else
	echo "WARNING: CUDA_PATH IS NOT SET. nvenc and nvdec will be unavailable."
	echo "If you have CUDA installed, make sure CUDA_PATH is set and re-source setup-msvc-toolchain.sh"
fi

git clone https://github.com/GPUOpen-LibrariesAndSDKs/AMF.git AMF-git
mkdir -p include/AMF
cp -r AMF-git/amf/public/include/. include/AMF/

if [[ `which link` =~ "usr/bin" ]]; then
	echo "We appear to be using the MSYS linker. This will cause issues."
	echo "You should move `which link` to another location, so that the MSVC linker is used"
	exit 1
fi

# Path must contain the Windows SDK for this to work
wget https://zlib.net/zlib-1.2.11.tar.gz
tar xvf zlib-1.2.11.tar.gz
cd zlib-1.2.11
sed -i.bak 's/-MD/-MT/g' win32/Makefile.msc
sed -i.bak '/unistd.h/d' zconf.h
nmake -f win32/Makefile.msc
cp -t ../include zconf.h zlib.h
mkdir -p ../zlib-binary
cp -t ../zlib-binary zlib.lib
cd ..

pacman -S --noconfirm mingw-w64-x86_64-libmfx
git clone https://github.com/Intel-Media-SDK/MediaSDK msdk
cd msdk
msbuild.exe AllBuild.sln -target:libmfx_vs2015 -p:Configuration=Release
cd ..
mkdir msdk-lib
cp build/win_x64/Release/lib/libmfx_vs2015.lib msdk-lib/libmfx.lib
cp build/win_x64/Release/lib/libmfx_vs2015.pdb msdk-lib/libmfx.pdb

echo "When ready:"
echo './configure --toolchain=msvc --enable-shared --enable-nvenc --enable-nvdec --disable-vulkan --enable-amf --enable-libmfx --enable-zlib --extra-cflags="-I${DEPENDENCIES_DIR}/nv_sdk -I${DEPENDENCIES_DIR}/include -I/mingw64/include" --extra-ldflags="-libpath:${DEPENDENCIES_DIR}/nv_sdk -libpath:${DEPENDENCIES_DIR}/msdk-lib -libpath:${DEPENDENCIES_DIR}/zlib-binary"'
