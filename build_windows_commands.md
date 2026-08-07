# URGE Windows 构建命令速查

适用环境：从 **x64 Native Tools Command Prompt for VS 2022** 运行（确保 `cl` 在 PATH）。
项目根目录：`D:\urge-251116`
构建目录：`D:\urge-251116\out\winout`（Ninja + MSVC cl）
日志文件：`D:\urge-251116\build_windows.log`
可执行文件：`D:\urge-251116\out\winout\app\Game.exe`

---

## 一、日常构建

### 1. 增量构建（默认，推荐）
只重编改动过的文件，秒~分钟级。sccache 命中缓存会进一步加速。
```bat
"D:\urge-251116\build_windows.bat"
```

### 2. 强制全量重新构建
清空 `out\winout` 后从头编译（解决增量状态不一致时）。
```bat
set URGE_CLEAN=1
"D:\urge-251116\build_windows.bat"
```

### 3. 直接用 cmake/ninja 增量构建（脚本已 configure 过时）
```bat
cd /d D:\urge-251116
cmake --build out\winout --target Game
```

### 4. 仅重新运行 cmake 配置（不编译）
```bat
cd /d D:\urge-251116
cmake -S . -B out\winout -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
  -DRuby_EXECUTABLE="D:\urge-251116\ruby34\bin\ruby.exe" ^
  -DRuby_LIBRARY="D:\urge-251116\ruby34\lib\x64-vcruntime140-ruby340.lib" ^
  -DRuby_INCLUDE_DIR="D:\urge-251116\ruby34\include\ruby-3.4.0" ^
  -DRuby_CONFIG_INCLUDE_DIR="D:\urge-251116\ruby34\include\ruby-3.4.0\x64-mswin64_140"
```

---

## 二、sccache 编译缓存

脚本已自动检测：若 `sccache` 在 PATH，则通过 `CMAKE_*_COMPILER_LAUNCHER=sccache` 启用，
构建结束后把统计写入 `build_windows.log`（搜 `[STAT]`）。

### 常用命令
```bat
sccache --version          # 查看版本（需 x86-64 版，ARM64 不兼容）
sccache --show-stats       # 查看命中/未命中/缓存大小
sccache --zero-stats       # 清零统计（测试加速效果前先清）
sccache --start-server     # 手动启动守护进程（一般按需自动起）
sccache --stop-server      # 停止守护进程
sccache --status           # 查看服务状态
```

### 缓存位置
默认：`%LOCALAPPDATA%\sccache`
可在运行前设置环境变量改变（可选）：
```bat
set SCCACHE_DIR=D:\sccache-cache
set SCCACHE_MAX_SIZE=10G
```

### 临时禁用 sccache（仍用脚本但不用缓存）
```bat
set PATH=%PATH:C:\Windows\sccache.exe;=%   # 不推荐，直接改脚本更稳
```
或更简单：临时把 sccache 改名/移出 PATH，脚本会自动跳过（`SCCACHE_CFG` 为空）。

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
where cl            # MSVC 编译器（必须在 Native Tools 终端）
where cmake         # CMake
where ninja         # Ninja
where ruby          # 若用外部 ruby（脚本默认用 ruby34 内置）
where sccache       # 编译缓存（需 x86-64 版）
```

---

## 五、典型工作流

```bat
REM 1) 首次/大改动后全量构建
set URGE_CLEAN=1
"D:\urge-251116\build_windows.bat"

REM 2) 之后日常改动，直接增量
"D:\urge-251116\build_windows.bat"

REM 3) 想看缓存加速效果
sccache --zero-stats
"D:\urge-251116\build_windows.bat"
REM 打开 build_windows.log 看 [STAT] 段 Cache Hits
```

> 注意：源码改动若被外部操作（git checkout / stash）还原，编译错误会重现，
> 此时需重新应用改动或重新全量构建。
