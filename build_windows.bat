@echo off

set "SCRIPT_DIR=%~dp0"
set "PROJECT_DIR=%SCRIPT_DIR%"

where cl >nul 2>&1
if errorlevel 1 (
    echo [ERROR] cl MSVC not found on PATH. Run from x64 Native Tools Command Prompt.
    exit /b 1
)

set "RUBY_ROOT=%PROJECT_DIR%ruby34"
set "PATH=%RUBY_ROOT%\bin;%PATH%"

set "VULKAN_SDK=C:\VulkanSDK\1.4.341.1"
if not exist "%VULKAN_SDK%\Include\vulkan\vulkan.h" (
    echo [WARN] VULKAN_SDK header not found at %VULKAN_SDK%
)

set "RUBYOPT=-EUTF-8"

set "Ruby_EXECUTABLE=%RUBY_ROOT%\bin\ruby.exe"
set "Ruby_LIBRARY=%RUBY_ROOT%\lib\x64-vcruntime140-ruby340.lib"
set "Ruby_INCLUDE_DIR=%RUBY_ROOT%\include\ruby-3.4.0"
set "Ruby_CONFIG_INCLUDE_DIR=%RUBY_ROOT%\include\ruby-3.4.0\x64-mswin64_140"

if not exist "%Ruby_EXECUTABLE%" (
    echo [ERROR] Bundled ruby not found at %Ruby_EXECUTABLE%
    exit /b 1
)

cd /d "%PROJECT_DIR%"

set "BUILD_DIR=%PROJECT_DIR%out\winout"
set "LOG=%PROJECT_DIR%build_windows.log"
echo [%date% %time%] Starting URGE build... > "%LOG%"

rmdir /s /q "%BUILD_DIR%" 2>nul
mkdir "%BUILD_DIR%" 2>nul

echo [STEP] cmake configure MSVC cl + Ninja
cmake -S . -B "%BUILD_DIR%" -G Ninja ^
  -DCMAKE_C_COMPILER=cl ^
  -DCMAKE_CXX_COMPILER=cl ^
  -DRuby_EXECUTABLE="%Ruby_EXECUTABLE%" ^
  -DRuby_LIBRARY="%Ruby_LIBRARY%" ^
  -DRuby_INCLUDE_DIR="%Ruby_INCLUDE_DIR%" ^
  -DRuby_CONFIG_INCLUDE_DIR="%Ruby_CONFIG_INCLUDE_DIR%" ^
  >> "%LOG%" 2>&1
if errorlevel 1 (
    echo [FAIL] cmake configure failed. See %LOG%
    exit /b 1
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
exit /b %BUILD_RC%
