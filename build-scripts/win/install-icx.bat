@echo off
setlocal enabledelayedexpansion

REM Checks whether Intel oneAPI ICX compiler is already installed.
REM If not, downloads the offline installer and installs silently.
REM
REM Environment variables (set before calling):
REM   ICX_INSTALLER_URL  - Full URL to the Intel oneAPI DPC++/C++ Compiler
REM                        offline installer (.exe). Can point to Azure Blob
REM                        Storage, AWS S3, or Intel's download center.
REM                        Required only if ICX is not already installed.

set "ONEAPI_ROOT=C:\Program Files (x86)\Intel\oneAPI"
set "SETVARS=%ONEAPI_ROOT%\setvars.bat"
set "INSTALLER_PATH=%TEMP%\icx_installer.exe"

if exist "%SETVARS%" (
    echo Intel oneAPI already installed at %ONEAPI_ROOT%
    exit /b 0
)

if "%ICX_INSTALLER_URL%"=="" (
    echo ERROR: ICX_INSTALLER_URL is not set and Intel oneAPI is not installed.
    echo.
    echo Set ICX_INSTALLER_URL to the offline installer URL before running this script.
    echo The URL can point to Azure Blob Storage, AWS S3, or Intel's download center.
    exit /b 1
)

echo ============================================================
echo  Downloading Intel oneAPI DPC++/C++ Compiler...
echo  URL: %ICX_INSTALLER_URL%
echo ============================================================
curl -SL --retry 3 --retry-delay 5 -o "%INSTALLER_PATH%" "%ICX_INSTALLER_URL%"
if %errorlevel% neq 0 (
    echo ERROR: Failed to download Intel oneAPI installer.
    exit /b 1
)

echo ============================================================
echo  Installing Intel oneAPI (silent, this may take a while)...
echo ============================================================
"%INSTALLER_PATH%" -s -a --silent --eula accept -p=NEED_VS2019_INTEGRATION=0 -p=NEED_VS2022_INTEGRATION=0
if %errorlevel% neq 0 (
    echo ERROR: Intel oneAPI installer returned error code %errorlevel%.
    del /f /q "%INSTALLER_PATH%" 2>nul
    exit /b 1
)

if not exist "%SETVARS%" (
    echo ERROR: Installation appeared to succeed but setvars.bat not found at:
    echo   %SETVARS%
    del /f /q "%INSTALLER_PATH%" 2>nul
    exit /b 1
)

echo ============================================================
echo  Intel oneAPI installed successfully.
echo ============================================================

del /f /q "%INSTALLER_PATH%" 2>nul
exit /b 0
