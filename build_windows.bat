@echo off
setlocal EnableExtensions

rem ---------------------------------------------------------------------------
rem  Build mode
rem    1 = incremental (default): reuse out\winout, recompile only what changed
rem    2 = full rebuild: wipe out\winout and reconfigure from scratch
rem  The mode can also be given on the command line (skips the prompt):
rem    build_windows.bat 1   |  build_windows.bat incremental
rem    build_windows.bat 2   |  build_windows.bat full   |  build_windows.bat clean
rem  URGE_CLEAN=1 is still accepted as a legacy alias of the full rebuild.
rem ---------------------------------------------------------------------------
set "MODE="
set "ARG=%~1"
if defined ARG set "ARG=%ARG:-=%"
if defined ARG set "ARG=%ARG:/=%"
if /i "%ARG%"=="1" set "MODE=1"
if /i "%ARG%"=="2" set "MODE=2"
if /i "%ARG%"=="incremental" set "MODE=1"
if /i "%ARG%"=="full" set "MODE=2"
if /i "%ARG%"=="clean" set "MODE=2"
if not defined MODE if "%URGE_CLEAN%"=="1" set "MODE=2"

if not defined MODE (
    echo ============================================================
    echo  URGE Windows Build
    echo ------------------------------------------------------------
    echo   [1] Incremental build  - reuse out\winout ^(fast^)
    echo   [2] Full rebuild       - wipe out\winout and reconfigure
    echo ============================================================
    echo  Tip: pass the mode directly, e.g.  build_windows.bat 2
    echo.
    goto :ask_mode
)
goto :mode_ready

:ask_mode
set "TRIES=0"

:ask_mode_loop
set "CHOICE="
set /p "CHOICE=Select build mode [1/2] (Enter = 1): "
if defined CHOICE set "CHOICE=%CHOICE: =%"
if not defined CHOICE set "MODE=1"
if "%CHOICE%"=="1" set "MODE=1"
if "%CHOICE%"=="2" set "MODE=2"
if defined MODE goto :mode_ready

set /a TRIES+=1
if %TRIES% GEQ 3 (
    echo [WARN] No valid input after 3 attempts, defaulting to mode 1.
    set "MODE=1"
    goto :mode_ready
)
echo [WARN] Invalid input: "%CHOICE%". Please enter 1 or 2.
goto :ask_mode_loop

:mode_ready
echo [MODE] Build mode = %MODE%  (1=incremental, 2=full)

set "SCRIPT_DIR=%~dp0"
set "PROJECT_DIR=%SCRIPT_DIR%"

rem ---------------------------------------------------------------------------
rem  Resolve and initialize the MSVC x64 toolchain automatically, so the script
rem  can be run from a plain cmd window or by double-clicking (no "x64 Native
rem  Tools Command Prompt" required).
rem ---------------------------------------------------------------------------
set "PF=%ProgramFiles%"
set "PF86=%ProgramFiles(x86)%"
set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_PATH="
if exist "%VSWHERE%" (
    for /f "tokens=*" %%I in ('"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "VS_PATH=%%I"
)
if not defined VS_PATH (
    for %%D in (
        "%PF%\Microsoft Visual Studio\2022\Enterprise"
        "%PF%\Microsoft Visual Studio\2022\Professional"
        "%PF%\Microsoft Visual Studio\2022\Community"
        "%PF%\Microsoft Visual Studio\2022\BuildTools"
        "%PF86%\Microsoft Visual Studio\2019\Enterprise"
        "%PF86%\Microsoft Visual Studio\2019\Professional"
        "%PF86%\Microsoft Visual Studio\2019\Community"
        "%PF86%\Microsoft Visual Studio\2019\BuildTools"
    ) do if not defined VS_PATH if exist "%%~D\VC\Auxiliary\Build\vcvarsall.bat" set "VS_PATH=%%~D"
)

if not defined VS_PATH (
    set "ERR_MSG=Visual Studio with C++ tools not found. Install VS 2022 with 'Desktop development with C++'."
    goto :fail
)

set "VCVARS=%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" (
    set "ERR_MSG=vcvarsall.bat not found: %VCVARS%"
    goto :fail
)

echo [TOOL] Visual Studio : %VS_PATH%
rem vcvarsall / vsdevcmd resolve vswhere.exe through PATH, so expose the
rem Visual Studio Installer directory before calling them.
if exist "%PF86%\Microsoft Visual Studio\Installer\vswhere.exe" set "PATH=%PF86%\Microsoft Visual Studio\Installer;%PATH%"
echo [TOOL] vcvarsall x64
call "%VCVARS%" x64 >nul
where cl >nul 2>&1
if errorlevel 1 (
    set "ERR_MSG=cl.exe still not on PATH after vcvarsall x64."
    goto :fail
)

rem Fall back to the CMake/Ninja bundled with Visual Studio when not on PATH.
where cmake >nul 2>&1
if errorlevel 1 if exist "%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin" set "PATH=%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
where ninja >nul 2>&1
if errorlevel 1 if exist "%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja" set "PATH=%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
where cmake >nul 2>&1
if errorlevel 1 (
    set "ERR_MSG=cmake not found on PATH (and none bundled with Visual Studio)."
    goto :fail
)
where ninja >nul 2>&1
if errorlevel 1 (
    set "ERR_MSG=ninja not found on PATH (and none bundled with Visual Studio)."
    goto :fail
)

set "RUBY_ROOT=%PROJECT_DIR%ruby40"
set "PATH=%RUBY_ROOT%\bin;%PATH%"

rem Vulkan SDK: keep an inherited VULKAN_SDK when it is valid, otherwise pick
rem the newest SDK under C:\VulkanSDK (do not pin a version that may move).
if not exist "%VULKAN_SDK%\Include\vulkan\vulkan.h" (
    set "VULKAN_SDK="
    for /f "delims=" %%D in ('dir /b /ad /o-n "C:\VulkanSDK" 2^>nul') do if not defined VULKAN_SDK if exist "C:\VulkanSDK\%%D\Include\vulkan\vulkan.h" set "VULKAN_SDK=C:\VulkanSDK\%%D"
)
if defined VULKAN_SDK (
    echo [TOOL] Vulkan SDK  : %VULKAN_SDK%
) else (
    echo [WARN] Vulkan SDK not found; the Vulkan backend may fail to configure.
)

set "RUBYOPT=-EUTF-8"

set "Ruby_EXECUTABLE=%RUBY_ROOT%\bin\ruby.exe"
set "Ruby_LIBRARY=%RUBY_ROOT%\lib\x64-vcruntime140-ruby400.lib"
set "Ruby_INCLUDE_DIR=%RUBY_ROOT%\include\ruby-4.0.0"
set "Ruby_CONFIG_INCLUDE_DIR=%RUBY_ROOT%\include\ruby-4.0.0\x64-mswin64_140"

if not exist "%Ruby_EXECUTABLE%" (
    set "ERR_MSG=Bundled ruby not found at %Ruby_EXECUTABLE%"
    goto :fail
)

cd /d "%PROJECT_DIR%"

set "BUILD_DIR=%PROJECT_DIR%out\winout"
set "LOG=%PROJECT_DIR%build_windows.log"
echo [%date% %time%] Starting URGE build (mode=%MODE%)... > "%LOG%"

rem Apply the selected build mode.
if "%MODE%"=="2" (
  echo [MODE] Full rebuild: removing "%BUILD_DIR%"
  rmdir /s /q "%BUILD_DIR%" 2>nul
) else (
  echo [MODE] Incremental build
)
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%" 2>nul

rem NOTE: sccache is deliberately not used as compiler launcher anymore; stale
rem cache entries produced confusing build failures (the "845 error" hunt).
rem Launchers are still passed empty below so a leftover value in an existing
rem CMake cache cannot silently re-enable a launcher.

echo [STEP] cmake configure MSVC cl + Ninja
cmake -S . -B "%BUILD_DIR%" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=cl ^
  -DCMAKE_CXX_COMPILER=cl ^
  -DRuby_EXECUTABLE="%Ruby_EXECUTABLE%" ^
  -DRuby_LIBRARY="%Ruby_LIBRARY%" ^
  -DRuby_INCLUDE_DIR="%Ruby_INCLUDE_DIR%" ^
  -DRuby_CONFIG_INCLUDE_DIR="%Ruby_CONFIG_INCLUDE_DIR%" ^
  -DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER= ^
  >> "%LOG%" 2>&1
if errorlevel 1 (
    set "ERR_MSG=cmake configure failed. See %LOG%"
    goto :fail
)

echo [STEP] cmake build target Game
cmake --build "%BUILD_DIR%" --target Game >> "%LOG%" 2>&1
set "BUILD_RC=%errorlevel%"

echo EXIT_CODE=%BUILD_RC% >> "%LOG%"

if %BUILD_RC%==0 (
    echo [DONE] Build succeeded. Log: %LOG%
) else (
    echo [FAIL] Build failed code %BUILD_RC%. See %LOG%
)

rem Keep the window open when the script was launched by double-click.
rem Set URGE_NO_PAUSE=1 to opt out (recommended for automation/CI).
call :pause_if_clicked
endlocal & exit /b %BUILD_RC%

rem ---------------------------------------------------------------------------
rem  Subroutines
rem ---------------------------------------------------------------------------

:pause_if_clicked
if "%URGE_NO_PAUSE%"=="1" exit /b 0
set "CMD_LINE=%cmdcmdline%"
echo(%CMD_LINE% | find /i "%~nx0" >nul
if errorlevel 1 exit /b 0
echo.
echo Press any key to close this window...
pause >nul
exit /b 0

:fail
echo.
echo [ERROR] %ERR_MSG%
call :pause_if_clicked
endlocal & exit /b 1

