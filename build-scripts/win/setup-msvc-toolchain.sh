#!/bin/bash

if [[ -z "$CUDA_PATH" ]]; then
	echo "CUDA_PATH is not set. nvenc will not be available."
elif [[ -z "$VCINSTALLDIR" ]]; then
	echo "Couldn't find Visual Studio install location. Aborting."
	return 1
elif [[ -z "$WindowsSdkVerBinPath" ]]; then
	echo "WindowsSdkVerBinPath is not set. Aborting."
	return 1
fi

export PATH="${VCINSTALLDIR}/Tools/MSVC/${VCToolsVersion}/bin/Hostx64/x64/":$PATH
export PATH="${VCINSTALLDIR}/../MSBuild/Current/Bin":$PATH
if [[ ! -z "$CUDA_PATH" ]]; then
	export PATH="${CUDA_PATH}/bin/":$PATH
fi
export PATH="${WindowsSdkVerBinPath}/x64/":$PATH
export PKG_CONFIG_PATH="/usr/local/lib/pkgconfig":$PKG_CONFIG_PATH

