#ifndef INCLUDE_RUBY_CONFIG_H
#define INCLUDE_RUBY_CONFIG_H 1
/* confdefs.h */
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
/* #undef HAVE_STRINGS_H (MSVC lacks it) */
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
/* #undef HAVE_UNISTD_H (MSVC lacks it) */
#define HAVE_WCHAR_H 1
#define STDC_HEADERS 1
#define _ALL_SOURCE 1
#define _DARWIN_C_SOURCE 1
#define _GNU_SOURCE 1
#define _HPUX_ALT_XOPEN_SOCKET_API 1
#define _NETBSD_SOURCE 1
#define _OPENBSD_SOURCE 1
#define _POSIX_PTHREAD_SEMANTICS 1
#define __STDC_WANT_IEC_60559_ATTRIBS_EXT__ 1
#define __STDC_WANT_IEC_60559_BFP_EXT__ 1
#define __STDC_WANT_IEC_60559_DFP_EXT__ 1
#define __STDC_WANT_IEC_60559_FUNCS_EXT__ 1
#define __STDC_WANT_IEC_60559_TYPES_EXT__ 1
#define __STDC_WANT_LIB_EXT2__ 1
#define __STDC_WANT_MATH_SPEC_FUNCS__ 1
#define _TANDEM_SOURCE 1
#define __EXTENSIONS__ 1
#define RUBY_MSVCRT_VERSION 140
#define RUBY_SYMBOL_EXPORT_BEGIN _Pragma("GCC visibility push(default)")
#define RUBY_SYMBOL_EXPORT_END _Pragma("GCC visibility pop")
/* #undef HAVE_STMT_AND_DECL_IN_EXPR */
#define THREAD_IMPL_H "thread_win32.h"
#define THREAD_IMPL_SRC "thread_win32.c"
#define HAVE_TYPE_NET_LUID 1
#define HAVE__GMTIME64_S 1
#define HAVE__WFREOPEN_S 1
#define HAVE_DIRENT_H 1
#define HAVE__BOOL 1
#define HAVE_AFUNIX_H 1
#define HAVE_DIRECT_H 1
#define HAVE_FCNTL_H 1
#define HAVE_FLOAT_H 1
/* #undef HAVE_IEEEFP_H (MSVC lacks it) */
#define HAVE_LIMITS_H 1
#define HAVE_LOCALE_H 1
#define HAVE_MALLOC_H 1
#define HAVE_PROCESS_H 1
#define HAVE_SETJMPEX_H 1
#define HAVE_STDALIGN_H 1
#define HAVE_STDIO_H 1
#define HAVE_SYS_FCNTL_H 1
/* #undef HAVE_SYS_FILE_H (MSVC lacks it) */
#define HAVE_SYS_UTIME_H 1
#define HAVE_TIME_H 1
/* #undef HAVE_STDCKDINT_H (MSVC has no C23 stdckdint.h) */
#define HAVE_STDATOMIC_H 1
#define HAVE_X86INTRIN_H 1
#define HAVE_GMP_H 1
#define HAVE_LIBGMP 1
#define _FILE_OFFSET_BITS 64
#define HAVE_TYPEOF 1
/* #define restrict (MSVC: use __declspec(restrict)) */
#define HAVE_LONG_LONG 1
#define HAVE_OFF_T 1
#define SIZEOF_INT 4
#define SIZEOF_SHORT 2
#define SIZEOF_LONG 4
#define SIZEOF_LONG_LONG 8
#define SIZEOF___INT64 8
/* #undef SIZEOF___INT128 */
#define SIZEOF_OFF_T 8
#define SIZEOF_VOIDP 8
#define SIZEOF_FLOAT 4
#define SIZEOF_DOUBLE 8
#define SIZEOF_TIME_T 8
#define SIZEOF_CLOCK_T 4
#define RBIMPL_ATTR_PACKED_STRUCT_BEGIN()
#define RBIMPL_ATTR_PACKED_STRUCT_END()
#define USE_UNALIGNED_MEMBER_ACCESS 1
#define PRI_LL_PREFIX "ll"
#define HAVE_PID_T 1
#define rb_pid_t int
#define SIGNEDNESS_OF_PID_T -1
#define PIDT2NUM(v) LL2NUM(v)
#define NUM2PIDT(v) NUM2LL(v)
#define PRI_PIDT_PREFIX PRI_LL_PREFIX
#define rb_uid_t int
#define SIGNEDNESS_OF_UID_T -1
#define UIDT2NUM(v) INT2NUM(v)
#define NUM2UIDT(v) NUM2INT(v)
#define PRI_UIDT_PREFIX PRI_INT_PREFIX
#define rb_gid_t int
#define SIGNEDNESS_OF_GID_T -1
#define GIDT2NUM(v) INT2NUM(v)
#define NUM2GIDT(v) NUM2INT(v)
#define PRI_GIDT_PREFIX PRI_INT_PREFIX
#define HAVE_TIME_T 1
#define rb_time_t time_t
#define SIGNEDNESS_OF_TIME_T -1
#define TIMET2NUM(v) LL2NUM(v)
#define NUM2TIMET(v) NUM2LL(v)
#define PRI_TIMET_PREFIX PRI_LL_PREFIX
#define HAVE_DEV_T 1
#define rb_dev_t int
#define SIGNEDNESS_OF_DEV_T +1
#define DEVT2NUM(v) UINT2NUM(v)
#define NUM2DEVT(v) NUM2UINT(v)
#define PRI_DEVT_PREFIX PRI_INT_PREFIX
#define HAVE_MODE_T 1
#define rb_mode_t int
#define SIGNEDNESS_OF_MODE_T +1
#define MODET2NUM(v) USHORT2NUM(v)
#define NUM2MODET(v) NUM2USHORT(v)
#define PRI_MODET_PREFIX PRI_SHORT_PREFIX
#define rb_rlim_t long
#define SIGNEDNESS_OF_RLIM_T -1
#define RLIM2NUM(v) LONG2NUM(v)
#define NUM2RLIM(v) NUM2LONG(v)
#define PRI_RLIM_PREFIX PRI_LONG_PREFIX
#define HAVE_OFF_T 1
#define rb_off_t off_t
#define SIGNEDNESS_OF_OFF_T -1
#define OFFT2NUM(v) LL2NUM(v)
#define NUM2OFFT(v) NUM2LL(v)
#define PRI_OFFT_PREFIX PRI_LL_PREFIX
#define HAVE_CLOCKID_T 1
#define rb_clockid_t int
#define SIGNEDNESS_OF_CLOCKID_T -1
#define CLOCKID2NUM(v) INT2NUM(v)
#define NUM2CLOCKID(v) NUM2INT(v)
#define PRI_CLOCKID_PREFIX PRI_INT_PREFIX
#define HAVE_VA_ARGS_MACRO 1
#define HAVE__ALIGNOF 1
#define CONSTFUNC(x) x
#define PUREFUNC(x) x
#define NORETURN(x) x
#define DEPRECATED(x) x
#define DEPRECATED_BY(n,x) x
#define NOINLINE(x) x
#define ALWAYS_INLINE(x) x
#define NO_SANITIZE(san, x) x
#define NO_SANITIZE_ADDRESS(x) x
#define NO_ADDRESS_SAFETY_ANALYSIS(x) x
#define WARN_UNUSED_RESULT(x) x
#define MAYBE_UNUSED(x) x
#define ERRORFUNC(mesg,x) x
#define WARNINGFUNC(mesg,x) x
#define WEAK(x) x
#define HAVE_FUNC_WEAK 1
#define HAVE_NULLPTR 1
#define FUNC_STDCALL(x) x
#define FUNC_CDECL(x) x
#define FUNC_FASTCALL(x) x
#define FUNC_UNOPTIMIZED(x) x
#define FUNC_MINIMIZED(x) x
#define HAVE_ATTRIBUTE_FUNCTION_ALIAS 1
#define RUBY_ALIAS_FUNCTION_TYPE(type, prot, name, args) type prot;
#define RUBY_ALIAS_FUNCTION_VOID(prot, name, args) RUBY_ALIAS_FUNCTION_TYPE(void, prot, name, args)
#define HAVE_GCC_ATOMIC_BUILTINS 1
#define HAVE_GCC_ATOMIC_BUILTINS_64 1
#define HAVE_GCC_SYNC_BUILTINS 1
/* #undef HAVE___BUILTIN_UNREACHABLE */
#define RUBY_FUNC_EXPORTED __declspec(dllexport) extern
#define RUBY_FUNC_NONNULL(n,x) x
#define RUBY_FUNCTION_NAME_STRING __func__
#define ENUM_OVER_INT 1
#define HAVE_DECL_SYS_NERR 1
#define HAVE_DECL_GETENV 1
#define SIZEOF_SIZE_T 8
#define SIZEOF_PTRDIFF_T 8
#define SIZEOF_DEV_T 4
#define PRI_SIZE_PREFIX "z"
#define PRI_PTRDIFF_PREFIX "t"
#define HAVE_STRUCT_STAT_ST_RDEV 1
#define SIZEOF_STRUCT_STAT_ST_SIZE SIZEOF_OFF_T
#define SIZEOF_STRUCT_STAT_ST_INO 2
#define SIZEOF_STRUCT_STAT_ST_DEV SIZEOF_DEV_T
#define SIZEOF_STRUCT_STAT_ST_RDEV SIZEOF_DEV_T
#define HAVE_STRUCT_TIMEVAL 1
#define SIZEOF_STRUCT_TIMEVAL_TV_SEC SIZEOF_LONG
#define TYPEOF_TIMEVAL_TV_SEC long
#define HAVE_STRUCT_TIMESPEC 1
#define HAVE_STRUCT_TIMEZONE 1
#define HAVE_RB_FD_INIT 1
#define HAVE_INT8_T 1
#define SIZEOF_INT8_T 1
#define HAVE_UINT8_T 1
#define SIZEOF_UINT8_T 1
#define HAVE_INT16_T 1
#define SIZEOF_INT16_T 2
#define HAVE_UINT16_T 1
#define SIZEOF_UINT16_T 2
#define HAVE_INT32_T 1
#define SIZEOF_INT32_T 4
#define HAVE_UINT32_T 1
#define SIZEOF_UINT32_T 4
#define HAVE_INT64_T 1
#define SIZEOF_INT64_T 8
#define HAVE_UINT64_T 1
#define SIZEOF_UINT64_T 8
/* #undef HAVE_INT128_T */
/* #define int128_t __int128 (MSVC has none) */
/* #undef SIZEOF_INT128_T */
/* #undef HAVE_UINT128_T */
/* #define uint128_t (MSVC has none) */
/* #undef SIZEOF_UINT128_T */
#define HAVE_INTPTR_T 1
#define SIZEOF_INTPTR_T 8
#define HAVE_UINTPTR_T 1
#define SIZEOF_UINTPTR_T 8
#define PRI_PTR_PREFIX "ll"
#define HAVE_SSIZE_T 1
#define SIZEOF_SSIZE_T 8
#define uid_t int
#define gid_t int
#define GETGROUPS_T int
#define HAVE_ALLOCA 1
#define HAVE_DUP 1
#define HAVE_DUP2 1
#define HAVE_ACOSH 1
#define HAVE_CBRT 1
#define HAVE_ERF 1
#define HAVE_FLOCK 1
#define HAVE_HYPOT 1
#define HAVE_MEMMOVE 1
#define HAVE_NAN 1
#define HAVE_NEXTAFTER 1
#define HAVE_STRCHR 1
#define HAVE_STRERROR 1
#define HAVE_STRSTR 1
#define HAVE_ISFINITE 1
#define HAVE_SIGNBIT 1
#define vfork fork
#define HAVE_ATAN2L 1
#define HAVE_ATAN2F 1
#define HAVE_DECL_ATOMIC_SIGNAL_FENCE 1
#define HAVE_CHMOD 1
#define HAVE_CHOWN 1
#define HAVE_CHSIZE 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_COSH 1
#define HAVE_EXECL 1
#define HAVE_EXECLE 1
#define HAVE_EXECV 1
#define HAVE_EXECVE 1
#define HAVE_FCNTL 1
#define HAVE_FMOD 1
#define HAVE_FSYNC 1
#define HAVE_FTRUNCATE 1
#define HAVE_FTRUNCATE64 1
#define HAVE_GETCWD 1
#define HAVE_GETEGID 1
#define HAVE_GETEUID 1
#define HAVE_GETGID 1
#define HAVE_GETLOGIN 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_GETUID 1
#define HAVE_GMTIME_R 1
#define HAVE_LCHOWN 1
#define HAVE_LINK 1
#define HAVE_LLABS 1
#define HAVE_LOG2 1
#define HAVE_LSTAT 1
#define HAVE_MBLEN 1
#define HAVE_MKTIME 1
#define HAVE_PCLOSE 1
#define HAVE_POPEN 1
#define HAVE_QSORT_S 1
#define HAVE_READLINK 1
#define HAVE_SEEKDIR 1
#define HAVE_SHUTDOWN 1
#define HAVE_SINH 1
#define HAVE_SNPRINTF 1
#define HAVE_SPAWNV 1
#define HAVE_SYMLINK 1
#define HAVE_SYSTEM 1
#define HAVE_TANH 1
#define HAVE_TELLDIR 1
#define HAVE_TIMES 1
#define HAVE_TRUNCATE 1
#define HAVE_TRUNCATE64 1
#define HAVE_TZSET 1
#define HAVE_UMASK 1
#define HAVE_WAITPID 1
/* #undef HAVE_BUILTIN___BUILTIN_ALLOCA_WITH_ALIGN (MSVC has no GCC builtins) */
/* #undef HAVE_BUILTIN___BUILTIN_ASSUME_ALIGNED (MSVC has no GCC builtins) */
/* #undef HAVE_BUILTIN___BUILTIN_BSWAP16 (MSVC has no GCC builtins) */
/* #undef HAVE_BUILTIN___BUILTIN_BSWAP32 (MSVC has no GCC builtins) */
/* #undef HAVE_BUILTIN___BUILTIN_BSWAP64 (MSVC has no GCC builtins) */
/* #undef HAVE_BUILTIN___BUILTIN_POPCOUNT (MSVC has no GCC builtins) */
/* #undef HAVE_BUILTIN___BUILTIN_POPCOUNTLL (MSVC has no GCC builtins) */
/* #undef HAVE_BUILTIN___BUILTIN_CLZ (MSVC has no GCC builtins) */
/* #undef HAVE_BUILTIN___BUILTIN_CLZL (MSVC has no GCC builtins) */
/* #undef HAVE_BUILTIN___BUILTIN_CLZLL (MSVC has no GCC builtins) */
/* #undef HAVE_BUILTIN___BUILTIN_CTZ (MSVC has no GCC builtins) */
/* #undef HAVE_BUILTIN___BUILTIN_CTZLL (MSVC has no GCC builtins) */
/* #undef HAVE_BUILTIN___BUILTIN_CONSTANT_P (MSVC has no GCC builtins) */
/* #undef HAVE_BUILTIN___BUILTIN_CHOOSE_EXPR (MSVC has no GCC builtins) */
/* #undef HAVE_BUILTIN___BUILTIN_CHOOSE_EXPR_CONSTANT_P (MSVC has no GCC builtins) */
/* #undef HAVE_BUILTIN___BUILTIN_TYPES_COMPATIBLE_P (MSVC has no GCC builtins) */
/* #undef HAVE_BUILTIN___BUILTIN_TRAP (MSVC has no GCC builtins) */
/* #undef HAVE_BUILTIN___BUILTIN_EXPECT (MSVC has no GCC builtins) */
/* #undef HAVE_BUILTIN___BUILTIN_ADD_OVERFLOW (MSVC has no GCC builtins) */
/* #undef HAVE_BUILTIN___BUILTIN_ADD_OVERFLOW_P (MSVC has no GCC builtins) */
#define USE___BUILTIN_ADD_OVERFLOW_LONG_LONG 1
/* #undef HAVE_BUILTIN___BUILTIN_SUB_OVERFLOW (MSVC has no GCC builtins) */
/* #undef HAVE_BUILTIN___BUILTIN_SUB_OVERFLOW_P (MSVC has no GCC builtins) */
#define USE___BUILTIN_SUB_OVERFLOW_LONG_LONG 1
/* #undef HAVE_BUILTIN___BUILTIN_MUL_OVERFLOW (MSVC has no GCC builtins) */
/* #undef HAVE_BUILTIN___BUILTIN_MUL_OVERFLOW_P (MSVC has no GCC builtins) */
#define USE___BUILTIN_MUL_OVERFLOW_LONG_LONG 1
#define ATAN2_INF_C99 1
#define HAVE_CLOCK_GETRES 1
#define VOID_UNSETENV 1
#define HAVE_DECL_TZNAME 1
#define HAVE_TZNAME 1
#define HAVE_DAYLIGHT 1
#define NEGATIVE_TIME_T 1
#define RSHIFT(x,y) ((x)>>(int)(y))
#define STACK_GROW_DIRECTION -1
#define COROUTINE_H "coroutine/win64/Context.h"
#define HAVE_CONST_PAGE_SIZE 0
#define DLEXT_MAXLEN 3
#define DLEXT ".so"
#define SOEXT ".dll"
#define LIBDIR_BASENAME "lib"
#define EXECUTABLE_EXTS ".exe",".com",".cmd",".bat"
#define RUBY_SETJMP(env) __builtin_setjmp((env))
#define RUBY_LONGJMP(env,val) __builtin_longjmp((env),val)
#define USE_MODULAR_GC 0
#define USE_YJIT 1
#define USE_ZJIT 0
#define LOAD_RELATIVE 1
#define RUBY_PLATFORM "x64-mingw-ucrt"
#define RB_DEFAULT_PARSER RB_DEFAULT_PARSER_PRISM
#endif /* INCLUDE_RUBY_CONFIG_H */

#if defined(_MSC_VER)
# define HAVE___ASSUME 1
#endif

/* --- 10. MSVC has no ssize_t (win32.h uses it) --- */
#if defined(_MSC_VER) && !defined(_SSIZE_T_DEFINED)
# include <basetsd.h>
typedef SSIZE_T ssize_t;
# define _SSIZE_T_DEFINED
#endif
