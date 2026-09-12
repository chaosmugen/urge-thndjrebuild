#!/bin/bash
# Apply MSVC-compatibility patches to a mingw-generated ruby install tree.
# Usage: apply_msvc_compat.sh <install_prefix>
PREFIX="$1"
INC="$PREFIX/include/ruby"
[ -d "$INC" ] || { echo "usage: $0 <install_prefix>"; exit 1; }

CFG="$INC/config.h"
cp -n "$CFG" "$CFG.orig"   # keep a pristine copy once

# --- 1. POSIX headers MSVC does not have -------------------------------
for m in STRINGS_H UNISTD_H SYS_FILE_H SYS_TIME_H SYS_WAIT_H SYS_RESOURCE_H \
         SYS_SELECT_H PWD_H GRP_H POLL_H ARPA_INET_H NETDB_H SYS_SOCKET_H \
         SYS_UN_H SYS_UIO_H SYS_IOCTL_H SYS_MMAN_H TERMIOS_H SYS_PARAM_H \
         SYS_SYSCALL_H IEEEFP_H; do
  sed -i "s|^#define HAVE_$m .*|/* #undef HAVE_$m (MSVC lacks it) */|" "$CFG"
done

# --- 2. GCC-only types / keywords --------------------------------------
sed -i 's|^#define restrict .*|/* #define restrict (MSVC: use __declspec(restrict)) */|' "$CFG"
sed -i 's|^#define HAVE___BUILTIN_UNREACHABLE.*|/* #undef HAVE___BUILTIN_UNREACHABLE */|' "$CFG"
sed -i 's|^#define rb_pid_t .*|#define rb_pid_t int|' "$CFG"
sed -i 's|^#define rb_dev_t .*|#define rb_dev_t int|' "$CFG"
sed -i 's|^#define rb_mode_t .*|#define rb_mode_t int|' "$CFG"
sed -i 's|^#define rb_clockid_t .*|#define rb_clockid_t int|' "$CFG"
sed -i 's|^#define rb_gid_t .*|#define rb_gid_t int|' "$CFG"
sed -i 's|^#define rb_uid_t .*|#define rb_uid_t int|' "$CFG"

# --- 3. GNU statement expressions --------------------------------------
sed -i 's|^#define HAVE_STMT_AND_DECL_IN_EXPR.*|/* #undef HAVE_STMT_AND_DECL_IN_EXPR */|' "$CFG"

# --- 4. int128 (MSVC has none) ------------------------------------------
sed -i 's|^#define SIZEOF___INT128 .*|/* #undef SIZEOF___INT128 */|' "$CFG"
sed -i 's|^#define HAVE_INT128_T .*|/* #undef HAVE_INT128_T */|' "$CFG"
sed -i 's|^#define int128_t .*|/* #define int128_t __int128 (MSVC has none) */|' "$CFG"
sed -i 's|^#define SIZEOF_INT128_T .*|/* #undef SIZEOF_INT128_T */|' "$CFG"
sed -i 's|^#define HAVE_UINT128_T .*|/* #undef HAVE_UINT128_T */|' "$CFG"
sed -i 's|^#define uint128_t .*|/* #define uint128_t (MSVC has none) */|' "$CFG"
sed -i 's|^#define SIZEOF_UINT128_T .*|/* #undef SIZEOF_UINT128_T */|' "$CFG"

# --- 5. __attribute__ macros -> pass-through ----------------------------
sed -i 's|^#define RBIMPL_ATTR_PACKED_STRUCT_END() .*|#define RBIMPL_ATTR_PACKED_STRUCT_END()|' "$CFG"
for m in CONSTFUNC PUREFUNC NORETURN DEPRECATED NOINLINE ALWAYS_INLINE \
         NO_SANITIZE_ADDRESS NO_ADDRESS_SAFETY_ANALYSIS WARN_UNUSED_RESULT \
         MAYBE_UNUSED WEAK FUNC_STDCALL FUNC_CDECL FUNC_FASTCALL \
         FUNC_UNOPTIMIZED FUNC_MINIMIZED; do
  sed -i "s|^#define $m(.*|#define $m(x) x|" "$CFG"
done
sed -i 's|^#define DEPRECATED_BY(.*|#define DEPRECATED_BY(n,x) x|' "$CFG"
sed -i 's|^#define NO_SANITIZE(.*|#define NO_SANITIZE(san, x) x|' "$CFG"
sed -i 's|^#define ERRORFUNC(.*|#define ERRORFUNC(mesg,x) x|' "$CFG"
sed -i 's|^#define WARNINGFUNC(.*|#define WARNINGFUNC(mesg,x) x|' "$CFG"
sed -i 's|^#define RUBY_FUNC_NONNULL(.*|#define RUBY_FUNC_NONNULL(n,x) x|' "$CFG"
sed -i 's|^#define RUBY_ALIAS_FUNCTION_TYPE(.*|#define RUBY_ALIAS_FUNCTION_TYPE(type, prot, name, args) type prot;|' "$CFG"

# --- 6. MSVC-provided bits ----------------------------------------------
grep -q "HAVE___ASSUME" "$CFG" || printf '\n#if defined(_MSC_VER)\n# define HAVE___ASSUME 1\n#endif\n' >> "$CFG"

echo "config.h patched: $CFG"
# --- 10. MSVC has no ssize_t (win32.h uses it) -------------------------
grep -q "_SSIZE_T_DEFINED" "$CFG" || printf '\n/* MSVC has no ssize_t */\n#if defined(_MSC_VER) && !defined(_SSIZE_T_DEFINED)\n# include <basetsd.h>\ntypedef SSIZE_T ssize_t;\n# define _SSIZE_T_DEFINED\n#endif\n' >> "$CFG"


# --- 7. internal/has/c_attribute.h: MSVC C frontend lies ---------------
F="$INC/internal/has/c_attribute.h"
if [ -f "$F" ] && ! grep -q "_MSC_VER" "$F"; then
  sed -i 's|^#if defined(__cplusplus)$|#if defined(__cplusplus) \|\| (defined(_MSC_VER) \&\& !defined(__cplusplus))|' "$F"
fi

# --- 8. internal/has/builtin.h: MSVC has no GCC builtins ---------------
F="$INC/internal/has/builtin.h"
if [ -f "$F" ] && ! grep -q "_MSC_VER" "$F"; then
  sed -i 's|^#if defined(__has_builtin)$|#if defined(__has_builtin) \&\& !defined(_MSC_VER)|' "$F"
  sed -i 's|^# define RBIMPL_HAS_BUILTIN(_) ((RBIMPL_HAS_BUILTIN_ ## _)+0)$|#  if defined(_MSC_VER)\n#   define RBIMPL_HAS_BUILTIN(_) 0\n#  else\n#   define RBIMPL_HAS_BUILTIN(_) ((RBIMPL_HAS_BUILTIN_ ## _)+0)\n#  endif|' "$F"
fi

# --- 9. internal/attr/restrict.h: token-paste breaks MSVC --------------
F="$INC/internal/attr/restrict.h"
[ -f "$F" ] && sed -i 's|__declspec(re ## strict)|__declspec(restrict)|' "$F"

echo "internal headers patched"
# --- 12. MSVC has none of the GCC __builtin_* --------------------------
sed -i -E 's|^#define (HAVE_BUILTIN___BUILTIN_[A-Z0-9_]+) 1.*|/* #undef \1 (MSVC) */|' "$CFG"

# --- 11. MSVC has no C23 <stdckdint.h> --------------------------------
sed -i 's|^#define HAVE_STDCKDINT_H .*|/* #undef HAVE_STDCKDINT_H (MSVC has no C23 header) */|' "$CFG"

