cmake_minimum_required(VERSION 3.18.0)

# Ruby 4.0.6 vendored sources for Android (mini runtime form).
#
# The source list mirrors the object set of the official cross build
# (NDK r28c / clang 19, API 21, YJIT/ZJIT disabled, --with-out-ext=,
# miniinit replacing localeinit). Reference artifacts were produced by
# `configure --host=<triple> && make` inside WSL; scripts in D:/wsl-setup.
#
# Conventions:
# - Per-ABI generated config.h lives in config/<ANDROID_ABI>/ruby/config.h.
# - verconf.h is force-included (-include) to provide RUBY_LIB_VERSION etc.
# - 4.0 builtin blobs are emitted as builtin_binary.rbbin (3.4 used .inc).
# - Files that are #included by other sources (id.c, id_table.c, vm_eval.c,
#   vm_args.c, vm_exec.c, thread_sync.c, thread_pthread.c, mini_builtin.c,
#   prism_compile.c, universal_parser.c, miniprelude.c) must NOT be listed
#   here; nor may dmyenc.c / dmyext.c / loadpath.c (their symbols are
#   provided by miniinit.c), builtin.c (builtin/Init_prelude symbols are
#   provided by miniinit.c in the mini form; builtin.c is the full-form
#   counterpart and the two are mutually exclusive), or main() providers
#   (main.c, goruby.c, rubystub.c, ruby-runner.c).

set(SRC ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(
  ${SRC}/include
  ${SRC}/config/${ANDROID_ABI}
  ${SRC}
  ${SRC}/ccan
  ${SRC}/prism
  ${SRC}/enc/unicode/17.0.0
)

set(MRI_SOURCE_FILES
  ## core
  ${SRC}/addr2line.c
  ${SRC}/array.c
  ${SRC}/ast.c
  ${SRC}/bignum.c
  ${SRC}/box.c
  ${SRC}/class.c
  ${SRC}/compar.c
  ${SRC}/compile.c
  ${SRC}/complex.c
  ${SRC}/concurrent_set.c
  ${SRC}/cont.c
  ${SRC}/debug.c
  ${SRC}/debug_counter.c
  ${SRC}/dir.c
  ${SRC}/dln.c
  ${SRC}/dln_find.c
  ${SRC}/encoding.c
  ${SRC}/enum.c
  ${SRC}/enumerator.c
  ${SRC}/error.c
  ${SRC}/eval.c
  ${SRC}/file.c
  ${SRC}/gc.c
  ${SRC}/hash.c
  ${SRC}/imemo.c
  ${SRC}/inits.c
  ${SRC}/io.c
  ${SRC}/io_buffer.c
  ${SRC}/iseq.c
  ${SRC}/load.c
  ${SRC}/marshal.c
  ${SRC}/math.c
  ${SRC}/memory_view.c
  ${SRC}/miniinit.c
  ${SRC}/node.c
  ${SRC}/node_dump.c
  ${SRC}/numeric.c
  ${SRC}/object.c
  ${SRC}/pack.c
  ${SRC}/parse.c
  ${SRC}/parser_st.c
  ${SRC}/pathname.c
  ${SRC}/proc.c
  ${SRC}/process.c
  ${SRC}/ractor.c
  ${SRC}/random.c
  ${SRC}/range.c
  ${SRC}/rational.c
  ${SRC}/re.c
  ${SRC}/regcomp.c
  ${SRC}/regenc.c
  ${SRC}/regerror.c
  ${SRC}/regexec.c
  ${SRC}/regparse.c
  ${SRC}/regsyntax.c
  ${SRC}/ruby.c
  ${SRC}/ruby_parser.c
  ${SRC}/scheduler.c
  ${SRC}/set.c
  ${SRC}/shape.c
  ${SRC}/signal.c
  ${SRC}/sprintf.c
  ${SRC}/st.c
  ${SRC}/strftime.c
  ${SRC}/string.c
  ${SRC}/struct.c
  ${SRC}/symbol.c
  ${SRC}/thread.c
  ${SRC}/time.c
  ${SRC}/transcode.c
  ${SRC}/util.c
  ${SRC}/variable.c
  ${SRC}/version.c
  ${SRC}/vm.c
  ${SRC}/vm_backtrace.c
  ${SRC}/vm_dump.c
  ${SRC}/vm_sync.c
  ${SRC}/vm_trace.c
  ${SRC}/weakmap.c

  ## prism parser (default since Ruby 3.4)
  ${SRC}/prism_init.c
  ${SRC}/prism/api_node.c
  ${SRC}/prism/api_pack.c
  ${SRC}/prism/diagnostic.c
  ${SRC}/prism/encoding.c
  ${SRC}/prism/extension.c
  ${SRC}/prism/node.c
  ${SRC}/prism/options.c
  ${SRC}/prism/pack.c
  ${SRC}/prism/prettyprint.c
  ${SRC}/prism/prism.c
  ${SRC}/prism/regexp.c
  ${SRC}/prism/serialize.c
  ${SRC}/prism/static_literals.c
  ${SRC}/prism/token_type.c
  ${SRC}/prism/util/pm_buffer.c
  ${SRC}/prism/util/pm_char.c
  ${SRC}/prism/util/pm_constant_pool.c
  ${SRC}/prism/util/pm_integer.c
  ${SRC}/prism/util/pm_list.c
  ${SRC}/prism/util/pm_memchr.c
  ${SRC}/prism/util/pm_newline_list.c
  ${SRC}/prism/util/pm_string.c
  ${SRC}/prism/util/pm_strncasecmp.c
  ${SRC}/prism/util/pm_strpbrk.c

  ## builtin encodings
  ${SRC}/enc/ascii.c
  ${SRC}/enc/us_ascii.c
  ${SRC}/enc/unicode.c
  ${SRC}/enc/utf_8.c
  ${SRC}/enc/trans/newline.c

  ## missing
  ${SRC}/missing/crypt.c
  ${SRC}/missing/explicit_bzero.c
  ${SRC}/missing/memcmp.c
  ${SRC}/missing/setproctitle.c
)

# Coroutine backend per ABI
if(ANDROID_ABI STREQUAL "arm64-v8a")
  set(CORO_ARCH arm64)
elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
  set(CORO_ARCH arm32)
elseif(ANDROID_ABI STREQUAL "x86_64")
  set(CORO_ARCH amd64)
elseif(ANDROID_ABI STREQUAL "x86")
  set(CORO_ARCH x86)
else()
  message(FATAL_ERROR "ruby_android: unsupported ANDROID_ABI '${ANDROID_ABI}'")
endif()
list(APPEND MRI_SOURCE_FILES ${SRC}/coroutine/${CORO_ARCH}/Context.S)

# The Makefile build compiles Context.S with
#   -DPREFIXED_SYMBOL(name)=name   (ELF targets have no symbol prefix)
set_source_files_properties(${SRC}/coroutine/${CORO_ARCH}/Context.S
  PROPERTIES COMPILE_OPTIONS "-DPREFIXED_SYMBOL(name)=name")

# Define the library to be built and its sources
add_library(mri_ruby SHARED ${MRI_SOURCE_FILES})

# addr2line.c compressed debug section support (SUPPORT_COMPRESSED_DEBUG_LINE,
# from config.h) needs zlib; the official Makefile build also links -lz.
target_link_libraries(mri_ruby PRIVATE z)

target_compile_options(mri_ruby PUBLIC -DRUBY_EXPORT -pthread)
target_compile_options(mri_ruby PRIVATE
  -O3 -fno-fast-math
  -fno-strict-overflow
  -fexcess-precision=standard
  -include ${SRC}/verconf.h
)
# NOTE: the official build uses -fvisibility=hidden, but the engine links
# internal symbols across the .so boundary (e.g. rb_call_builtin_inits),
# which hidden visibility would hide. Keep default visibility.

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  target_compile_definitions(mri_ruby PUBLIC ARCH_32BIT)
  # 【32 位构建绕行】vm_exec_core 确定性崩溃（switch/computed-goto 均复现），
  # 临时整体降级优化并关闭严格别名，排除编译器代码生成问题。
  target_compile_options(mri_ruby PRIVATE -O1 -fno-strict-aliasing)
  message("${ANDROID_ABI} ruby 40 32 bit build (O1 debug override)")
else()
  target_compile_definitions(mri_ruby PUBLIC ARCH_64BIT)
  message("${ANDROID_ABI} ruby 40 64 bit build")
endif()
