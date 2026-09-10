# URGE Windows 构建命令速查

适用环境：**普通 cmd 窗口或直接双击即可** —— 脚本会自动定位 Visual Studio 2022、调用
`vcvarsall.bat x64` 初始化编译环境，并回退使用 VS 自带的 CMake/Ninja。
（不再需要预先打开 "x64 Native Tools Command Prompt for VS 2022"。）
项目根目录：`D:\urge-251116`
构建目录：`D:\urge-251116\out\winout`（Ninja + MSVC cl）
日志文件：`D:\urge-251116\build_windows.log`
可执行文件：`D:\urge-251116\out\winout\app\Game.exe`

---

## 一、日常构建

脚本启动时会先弹出模式选择菜单：

```
============================================================
 URGE Windows Build
------------------------------------------------------------
  [1] Incremental build  - reuse out\winout (fast)
  [2] Full rebuild       - wipe out\winout and reconfigure
============================================================
Select build mode [1/2] (Enter = 1):
```

- 输入 `1`（或直接回车）：增量构建，只重编改动过的文件，秒~分钟级。
- 输入 `2`：全量构建，先删除 `out\winout`，再重新 configure + 编译。

也可以在命令行直接指定模式以跳过交互：

```bat
"D:\urge-251116\build_windows.bat" 1      REM 增量
"D:\urge-251116\build_windows.bat" 2      REM 全量
"D:\urge-251116\build_windows.bat" full   REM 全量（别名：2 / clean）
```

兼容性：`set URGE_CLEAN=1` 仍等价于全量构建（供已有脚本/自动化使用）。

窗口行为：双击运行时，脚本结束（成功或失败）后会显示
`Press any key to close this window...` 等待按键，便于查看结果；
在终端里手动运行不会暂停。自动化调用可设 `set URGE_NO_PAUSE=1` 跳过。

> 另：`configure_ruby.bat` 用于生成 **Visual Studio 工程**（输出到 `build\`，供 IDE 打开），
> 同样会自动定位 VS；日志写在 `build\configure.log`（该目录已被 .gitignore 忽略）。

### 直接用 cmake/ninja 增量构建（脚本已 configure 过时）
```bat
cd /d D:\urge-251116
cmake --build out\winout --target Game
```

### 仅重新运行 cmake 配置（不编译）
```bat
cd /d D:\urge-251116
cmake -S . -B out\winout -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
  -DRuby_EXECUTABLE="D:\urge-251116\ruby40\bin\ruby.exe" ^
  -DRuby_LIBRARY="D:\urge-251116\ruby40\lib\x64-vcruntime140-ruby400.lib" ^
  -DRuby_INCLUDE_DIR="D:\urge-251116\ruby40\include\ruby-4.0.0" ^
  -DRuby_CONFIG_INCLUDE_DIR="D:\urge-251116\ruby40\include\ruby-4.0.0\x64-mswin64_140"
```

---

## 二、编译器缓存（sccache）— 当前未启用

脚本**不再**把 sccache 作为编译器 launcher：此前遇到过陈旧缓存导致的构建失败
（845 错误排查），因此 configure 时显式传入空的 `CMAKE_*_COMPILER_LAUNCHER`，
即使 CMake 缓存里残留 launcher 也不会生效。

如需重新启用，把 `build_windows.bat` 中 cmake 配置行的两个空 launcher 换成：

```bat
  -DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache ^
```

### sccache 常用命令（手动使用）
```bat
sccache --version          # 查看版本（需 x86-64 版，ARM64 不兼容）
sccache --show-stats       # 查看命中/未命中/缓存大小
sccache --zero-stats       # 清零统计
sccache --stop-server      # 停止守护进程
```

### 缓存位置
默认：`%LOCALAPPDATA%\sccache`，可用 `SCCACHE_DIR` / `SCCACHE_MAX_SIZE` 调整。

---

## 三、排查与清理

```bat
# 查看构建日志尾部错误
powershell -command "Get-Content D:\urge-251116\build_windows.log -Tail 60"

# 搜索日志中的 error/failed
findstr /i "error C FAILED EXIT_CODE" D:\urge-251116\build_windows.log

# 手动删除构建目录（等价于 URGE_CLEAN=1）
rmdir /s /q D:\urge-251116\out\winout

# 确认 Game.exe 是否生成
dir D:\urge-251116\out\winout\app\Game.exe
```

---

## 四、环境检查

```bat
where cl            # MSVC 编译器（脚本会自动通过 vcvarsall 初始化，无需 Native Tools 终端）
where cmake         # CMake
where ninja         # Ninja
where ruby          # 若用外部 ruby（脚本默认用 ruby40 内置）
where sccache       # 编译缓存（脚本当前未使用，仅供手动调用）
```

---

## 五、典型工作流

```bat
REM 1) 首次/大改动后全量构建
"D:\urge-251116\build_windows.bat" 2

REM 2) 之后日常改动，直接增量（也可双击脚本后在菜单里选 1）
"D:\urge-251116\build_windows.bat" 1

REM 3) 只想确认当前状态 / 重新链接
"D:\urge-251116\build_windows.bat" 1
REM 结果看 build_windows.log 的 EXIT_CODE（0 = 成功）
```

> 注意：源码改动若被外部操作（git checkout / stash）还原，编译错误会重现，
> 此时需重新应用改动或重新全量构建。

---

## Bundled Ruby 4.0.6 (x64-mswin64_140) 构建备注

本项目内置 `ruby40/`（`x64-vcruntime140-ruby400.dll` + `.lib` + `include/`），
由 ruby-4.0.6 官方 tarball 用 MSVC 构建（VS2022 + nmake）产出。

构建步骤（复现用）：

```bat
call "<VS>\VC\Auxiliary\Build\vcvars64.bat"
cd /d <ruby-4.0.6>
win32\configure.bat --prefix=<install-dir> --disable-install-doc ^
    --with-baseruby <host-ruby.exe>
nmake miniruby
nmake
nmake install
```

对 4.0.6 tarball 必需的三处补丁（tarball 直接构建会失败）：

1. `win32/setup.mak`（verconf 规则）：`<<"Creating $(@)"` 形式在新版 nmake
   （14.44）下不被识别为 inline file display-name，会把它当作 cl 的输入文件名。
   —— 仅在**整段替换规则**时处理（见第 2 点）。
2. `win32/setup.mak` 的 `verconf.mk: nul` 规则：改为直接 echo 固定值
   （跳过 `cl -EP` 展开；release 版不应输出 `ABI_VERSION`，否则
   `version.h` 报 `RUBY_ABI_VERSION is defined in non-development branch`）：

   ```
   verconf.mk: nul
   	@echo RUBY_RELEASE_YEAR = 2026 > $(@)
   	@echo RUBY_RELEASE_MONTH = 07 >> $(@)
   	@echo RUBY_RELEASE_DAY = 14 >> $(@)
   	@echo MAJOR = 4 >> $(@)
   	@echo MINOR = 0 >> $(@)
   	@echo TEENY = 6 >> $(@)
   	@echo MSC_VER = 1944 >> $(@)
   	@echo MSC_VER_LOWER = 1940 >> $(@)
   	@echo MSC_VER_UPPER = 1959 >> $(@)
   ```

3. 构建 shell 的 `PATH` 必须包含 `.`（cmd 默认不搜索当前目录，
   否则 `miniruby.exe -v` 之类调用报“不是内部或外部命令”）。

注意：`verconf.mk` 与 `.ext/include/<arch>/ruby/config.h` 是配置期产物，
改规则后需删除它们再跑 `win32\configure.bat`，否则沿用旧值。

切换/回退：`build_windows.bat` 通过 `RUBY_ROOT` 指向内置 ruby 目录，
库名与 include 版本号为 `x64-vcruntime140-ruby400.lib` / `ruby-4.0.0`。
