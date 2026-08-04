@echo off
set "NoDefaultCurrentDirectoryInExePath="
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d d:\urge-251022
cmake -B build -A x64 ^
  -DRuby_EXECUTABLE="D:\urge-251022\ruby34\bin\ruby.exe" ^
  -DRuby_LIBRARY="D:\urge-251022\ruby34\lib\x64-vcruntime140-ruby340.lib" ^
  -DRuby_INCLUDE_DIR="D:\urge-251022\ruby34\include\ruby-3.4.0" ^
  -DRuby_CONFIG_INCLUDE_DIR="D:\urge-251022\ruby34\include\ruby-3.4.0\x64-mswin64_140" ^
  > d:\urge-251022\build_configure4.log 2>&1
echo EXIT_CODE=%ERRORLEVEL% >> d:\urge-251022\build_configure4.log
