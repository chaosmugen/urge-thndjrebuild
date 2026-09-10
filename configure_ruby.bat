@echo off
setlocal EnableExtensions
set "NoDefaultCurrentDirectoryInExePath="

rem Generates the Visual Studio solution / project files into build\ (for IDE
rem work). For the command line Ninja build use build_windows.bat instead.
rem The log lands inside build\ so the repo root stays clean.

rem Locate Visual Studio (same approach as build_windows.bat).
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
    echo [ERROR] Visual Studio with C++ tools not found.
    exit /b 1
)

echo [TOOL] Visual Studio : %VS_PATH%
rem vcvarsall resolves vswhere.exe through PATH, so expose the Installer dir.
if exist "%PF86%\Microsoft Visual Studio\Installer\vswhere.exe" set "PATH=%PF86%\Microsoft Visual Studio\Installer;%PATH%"
call "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul

cd /d "%~dp0"
set "RUBY_ROOT=%~dp0ruby40"
if not exist "%RUBY_ROOT%\bin\ruby.exe" (
    echo [ERROR] Bundled ruby not found at %RUBY_ROOT%\bin\ruby.exe
    exit /b 1
)

if not exist "%~dp0build" mkdir "%~dp0build"

cmake -B "%~dp0build" -A x64 ^
  -DRuby_EXECUTABLE="%RUBY_ROOT%\bin\ruby.exe" ^
  -DRuby_LIBRARY="%RUBY_ROOT%\lib\x64-vcruntime140-ruby400.lib" ^
  -DRuby_INCLUDE_DIR="%RUBY_ROOT%\include\ruby-4.0.0" ^
  -DRuby_CONFIG_INCLUDE_DIR="%RUBY_ROOT%\include\ruby-4.0.0\x64-mswin64_140" ^
  > "%~dp0build\configure.log" 2>&1
set "RC=%errorlevel%"
echo EXIT_CODE=%RC% >> "%~dp0build\configure.log"

if not "%RC%"=="0" (
    echo [FAIL] cmake configure failed. See build\configure.log
    exit /b %RC%
)
echo [DONE] Visual Studio projects written to build\ (log: build\configure.log)
exit /b 0
