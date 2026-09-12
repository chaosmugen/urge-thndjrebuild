# mingw-ucrt Ruby 4.0.6 构建流程（Windows，含 YJIT）

构建环境本体在 `D:\ruby-build\`（源码树与构建目录不动，见文末），本目录只归档脚本。
工具链依赖（均在 ruby-build 之外）：`D:\msys64`（ucrt64 gcc）、`C:\Ruby40-x64`（baseruby）、
cargo/rust（YJIT 的 Rust 核心）。

## 步骤

1. **配置 + 编译**：`bash run-ucrt-build.sh`
   在 `D:\ruby-build\ruby40-ucrt` 里执行 autoconf + configure + make -j8。
   注意：configure 用 `--with-baseruby=C:/Ruby40-x64/bin/ruby.exe --enable-yjit`，
   prefix 为 `D:/ruby-build/ruby40-ucrt-install`。日志：`conf40ucrt.log` / `make40ucrt.log`。
2. **增量编译**：`bash make-ucrt.sh`（含修复点说明，见脚本头注释）。
3. **安装**：`bash install-ucrt.sh` → `D:/ruby-build/ruby40-ucrt-install`。
4. **MSVC 适配**：`bash apply_msvc_compat.sh <install前缀>`
   mingw 生成的头文件给 MSVC 引擎消费所需的修补（POSIX 头、restrict、
   ssize_t、stdckdint、GCC __builtin_* 宏、attr 宏等）。
   修改的是 `<prefix>/include/ruby/config.h` 与 `include/ruby/internal/{has,attr}/*.h`。

## 关键修复点（重踩坑提示）

- **不要用 autoconf 重生成引擎所用的 config.h**：configure.ac 保持原时间戳，
  让 make 不触发 recheck（重跑会破坏 config.h，游戏即无法启动）。
- **rust 归档**：`target/release/libyjit.localized.a` 必须由 make 规则生成
  （objcopy 把 rust 符号局部化，规避与 lgamma_r 的冲突），不能预创建。
- **静态库合并**：用 `ar -M`（addlib 脚本）把 rust 归档并入 libruby，且 ar 步骤
  的输入列表要排除该归档（否则嵌套归档导致 def 损坏）。
- **mkexports.rb**：mingw 分支需过滤 rust 修饰名与 `.llvm.` 名字（.def 非法语法）。
- **链接库**：rbconfig 与 Makefile 需补 `ntdll/userenv/advapi32`（rust std 与
  扩展的特性探测都依赖），否则扩展链接测试失败。

## 产物去向

- 部署：把 `ruby40-ucrt-install/` 的内容复制为引擎的 `ruby40/`，
  再执行第 4 步适配；游戏只需 `bin/x64-ucrt-ruby400.dll` + `bin/libgmp-10.dll`
  （ucrt64 版）两个运行时文件。
- 引擎侧为 MSVC 消费 mingw 头文件的历史修补记录见
  `D:\ruby-build\apply_msvc_compat.sh`（本目录有副本）。

## D:\ruby-build 现存目录

| 目录 | 用途 |
|---|---|
| `ruby-4.0.6-win/` | ruby 源码 + 配置产物（550M，保留） |
| `ruby40-ucrt/` | ucrt 构建目录（433M，保留；增量编译用） |
| `ruby40-ucrt-install/` | make install 的产物（94M，部署 ruby40/ 的母本，保留） |
