# Topaz Labs FFmpeg Build Scripts
These scripts will setup an FFmpeg build environment with the right options enabled for our uses.

## Requirements
- CUDA Toolkit
- Windows SDK
- MSVC >= 14.12
- Intel Graphics Driver
- MSYS2

## Setup
Start mingw64 from the Visual Studio x64 Native Tools Command Prompt, and then:
```
cd /path/to/this/repository
source build-scripts/setup-msvc-toolchain.sh
export DEPENDENCIES_DIR=/path/to/where/you/want/dependencies
bash build-scripts/install-dependencies
```
**Before running the provided configure command you should build the Intel MSDK and place libmfx.lib in `${DEPENDENCIES_DIR}/msdk-lib/`**. This step must be done manually at this time.

### Running the Configure Command
```
./configure --toolchain=msvc --enable-shared --enable-nvenc --enable-nvdec --disable-vulkan --enable-amf --enable-libmfx --enable-zlib --extra-cflags="-I${DEPENDENCIES_DIR}/nv_sdk -I${DEPENDENCIES_DIR}/include -I/mingw64/include" --extra-ldflags="-libpath:${DEPENDENCIES_DIR}/nv_sdk -libpath:${DEPENDENCIES_DIR}/msdk-lib -libpath:${DEPENDENCIES_DIR}/zlib-binary"
```
You may want to add a `--prefix=/path/to/install/FFmpeg`.

## Compiling
Run `make -r -j8` followed by a `make -r install`

## Building with Intel ICX Compiler (Windows)

The Intel oneAPI DPC++/C++ Compiler (`icx`/`icpx`) can be used instead of MSVC's
`cl.exe` for improved performance on Intel CPUs (with no change to performance on AMD CPUs). ICX operates in MSVC-compatible
(clang-cl) mode, so FFmpeg is still configured with `--toolchain=msvc` while
overriding `--cc=icx --cxx=icpx`.

**Do NOT use `--toolchain=icl`** -- that targets the deprecated classic Intel
compiler. The build scripts handle this automatically.

### Prerequisites
- Everything listed in [Requirements](#requirements) above
- [Intel oneAPI DPC++/C++ Compiler](https://www.intel.com/content/www/us/en/developer/tools/oneapi/dpc-compiler-download.html) (standalone installer or via the Base Toolkit)

### Local Build Steps

1. Open a **cmd.exe** prompt and initialize the Intel oneAPI environment:
   ```
   "C:\Program Files (x86)\Intel\oneAPI\setvars.bat"
   ```

2. Launch MSYS2 with `-use-full-path` so it inherits the oneAPI + MSVC paths:
   ```
   C:\msys64\msys2_shell.cmd -defterm -no-start -use-full-path -mingw64
   ```

3. Inside the MSYS2 shell, run the build script with the `--icx` flag:
   ```bash
   cd /path/to/FFmpeg
   bash ./build-scripts/win/build_win.sh --icx --no-warnings
   ```

### Azure DevOps Pipeline

Set the **Intel Compiler (ICX)** parameter to `true` when queuing a build.

If the build agent does not already have Intel oneAPI installed, the pipeline
will download and install it automatically. The installer URL is resolved in
this order:

1. The **Intel ICX Installer URL** pipeline parameter (per-build override)
2. The `ICX_INSTALLER_URL` variable in the **Secrets** variable group (default)

#### Hosting the Installer on Azure Blob Storage

Intel's download URLs are dynamic and require registration. To avoid depending
on them, host the offline installer in Azure Blob Storage:

1. Download the Intel oneAPI DPC++/C++ Compiler offline installer from
   [Intel's download page](https://www.intel.com/content/www/us/en/developer/tools/oneapi/dpc-compiler-download.html).

2. Upload it to an Azure Blob Storage container:
   ```
   az storage blob upload \
     --account-name <storage-account> \
     --container-name build-tools \
     --name intel/w_dpcpp-cpp-compiler_offline.exe \
     --file ./w_dpcpp-cpp-compiler_p_<version>_offline.exe
   ```

3. Generate a SAS URL (e.g. valid for 2 years):
   ```
   az storage blob generate-sas \
     --account-name <storage-account> \
     --container-name build-tools \
     --name intel/w_dpcpp-cpp-compiler_offline.exe \
     --permissions r \
     --expiry 2028-01-01T00:00:00Z \
     --full-uri
   ```

4. Add the SAS URL as `ICX_INSTALLER_URL` in the Azure DevOps **Secrets**
   variable group (Pipelines > Library > Secrets). Mark it as secret.