// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdio>
#include <filesystem>
#include <vector>

#include "SDL3/SDL_main.h"
#include "SDL3/SDL_messagebox.h"
#include "SDL3/SDL_stdinc.h"
#include "SDL3_image/SDL_image.h"
#include "SDL3_ttf/SDL_ttf.h"
#include "spdlog/sinks/android_sink.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"

#include "binding/mri/mri_main.h"
#include "components/filesystem/io_service.h"
#include "content/canvas/font_context.h"
#include "content/profile/i18n_profile.h"
#include "content/worker/content_runner.h"
#include "ui/widget/widget.h"

#if HAVE_ARB_ENCRYPTO_SUPPORT
#include "admenri/encryption/encrypt_arb.h"
#endif

#if defined(OS_ANDROID)
#include <dirent.h>
#include <fcntl.h>
#include <jni.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include "renderer/device/gpu_audit.h"
#include "renderer/device/render_device.h"

#if defined(OS_WIN)
#include <windows.h>
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif  //! OS_WIN

static int g_pfd[2];
static pthread_t g_android_stdio_thread;
// Path on removable storage (set from Java via nativeSetDiagPath). The engine's
// stdout/stderr (incl. Ruby's fatal errors) are tee'd here as well, so they
// survive a native crash and are reachable without ADB.
static std::string g_diag_path;
static std::string g_removable_diag_path;
static bool g_tv_device = false;
static FILE* g_stdio_fp = nullptr;

static void* StdioTransferThreadFunc(void*) {
  ssize_t rdsz;
  char buf[128];
  while ((rdsz = read(g_pfd[0], buf, sizeof buf - 1)) > 0) {
    if (buf[rdsz - 1] == '\n')
      --rdsz;
    buf[rdsz] = 0; /* add null-terminator */
    __android_log_write(ANDROID_LOG_DEBUG, "urge-stdio", buf);
    if (!g_diag_path.empty()) {
      if (g_stdio_fp == nullptr)
        g_stdio_fp = fopen((g_diag_path + "/stdio.txt").c_str(), "ab");
      if (g_stdio_fp) {
        fwrite(buf, 1, rdsz, g_stdio_fp);
        fputc('\n', g_stdio_fp);
        fflush(g_stdio_fp);
      }
    }
  }
  return 0;
}

int SetupAndroidStudioTransfer() {
  /* make stdout line-buffered and stderr unbuffered */
  setvbuf(stdout, 0, _IOLBF, 0);
  setvbuf(stderr, 0, _IONBF, 0);

  /* create the pipe and redirect stdout and stderr */
  pipe(g_pfd);
  dup2(g_pfd[1], 1);
  dup2(g_pfd[1], 2);

  /* spawn the logging thread */
  if (pthread_create(&g_android_stdio_thread, 0, StdioTransferThreadFunc, 0) ==
      -1)
    return -1;
  pthread_detach(g_android_stdio_thread);
  return 0;
}

// Called from URGEMain.onPause() (Java UI thread) *before* SDL releases the
// ANativeWindow in surfaceDestroyed(). Flagging the surface as gone here stops
// the render thread from resizing or presenting on a window that is about to
// be released, which used to crash the Vulkan backend inside
// vkCreateAndroidSurfaceKHR (SIGSEGV, fault addr 0x4).
extern "C" JNIEXPORT void JNICALL
Java_com_admenri_urge_URGEMain_nativeSuspendGraphics(JNIEnv*, jclass) {
  renderer::RenderDevice::NotifySurfaceLosing();
}

// Called from URGEMain.onCreate (Java) right after the native libs are loaded,
// to tell the engine where to dump logs on removable storage.
extern "C" JNIEXPORT void JNICALL
Java_com_admenri_urge_URGEMain_nativeSetDiagPath(JNIEnv* env, jclass,
                                                 jstring path) {
  if (path) {
    const char* p = env->GetStringUTFChars(path, nullptr);
    if (p) {
      g_diag_path = p;
      env->ReleaseStringUTFChars(path, p);
    }
  }
}

extern "C" JNIEXPORT void JNICALL
Java_com_admenri_urge_URGEMain_nativeSetRemovableDiagPath(JNIEnv* env, jclass,
                                                          jstring path) {
  if (path) {
    const char* p = env->GetStringUTFChars(path, nullptr);
    if (p) {
      g_removable_diag_path = p;
      env->ReleaseStringUTFChars(path, p);
    }
  }
}

extern "C" JNIEXPORT void JNICALL
Java_com_admenri_urge_URGEMain_nativeSetTvDevice(JNIEnv* env, jclass,
                                                 jboolean is_tv) {
  g_tv_device = is_tv == JNI_TRUE;
}

// Drops the kernel page cache of every regular file under <path> (or of the file
// itself when <path> is a file). The extraction streams ~1GB of game data (plus a
// ~1GB APK read for the MD5) through the page cache; on a ~3GB TV with no swap
// that leaves only ~60MB of free RAM, and the engine's startup burst (RSS +90MB
// in seconds) then forces heavy direct reclaim — which lmkd answers by killing
// the process right before the first script runs. Releasing our own clean cache
// up front gives that burst headroom again.
static void DropPageCacheUnder(const std::string& path) {
  struct stat st = {};
  if (stat(path.c_str(), &st) != 0) return;
  if (S_ISREG(st.st_mode)) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd >= 0) {
      posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
      close(fd);
    }
    return;
  }
  if (!S_ISDIR(st.st_mode)) return;

  DIR* dir = opendir(path.c_str());
  if (!dir) return;
  while (dirent* entry = readdir(dir)) {
    if (!entry) break;
    if (entry->d_name[0] == '.') continue;
    DropPageCacheUnder(path + "/" + entry->d_name);
  }
  closedir(dir);
}

extern "C" JNIEXPORT void JNICALL
Java_com_admenri_urge_URGEMain_nativeDropPageCache(JNIEnv* env, jclass,
                                                   jstring path) {
  if (!path) return;
  const char* p = env->GetStringUTFChars(path, nullptr);
  if (!p) return;
  DropPageCacheUnder(p);
  env->ReleaseStringUTFChars(path, p);
}

#endif

#if defined(OS_WIN)
#include <windows.h>

// Allocate our own debug console window when show == true. We always create an
// independent console so behavior is identical whether launched by double-click
// or from a command prompt. When show == false we do nothing, restoring the
// previous behavior of no debug console.
//
// After AllocConsole, the process standard handles may still point at the
// parent terminal's pipes (when launched from cmd/terminal), so GetStdHandle
// would NOT return the new console handle. We reopen CONOUT$/CONIN$ explicitly
// via CreateFile and SetStdHandle to force the process (and spdlog, which
// writes through GetStdHandle) to use our own console. CRT streams are rebound
// the same way so Ruby's puts (CRT stdout) also reaches the window.
void CreateConsoleWin(bool show) {
  if (!show)
    return;
  if (::GetConsoleWindow() != nullptr)
    ::FreeConsole();
  ::AllocConsole();
  ::SetConsoleCP(CP_UTF8);
  ::SetConsoleOutputCP(CP_UTF8);
  ::SetConsoleTitleW(L"URGE Debugging Console");

  // Force the process standard handles to our own console. CreateFile("CONOUT$")
  // always returns the current console's handle, ignoring any inherited pipe.
  HANDLE hIn = ::CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, 0, nullptr);
  HANDLE hOut = ::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, 0, nullptr);
  if (hIn != INVALID_HANDLE_VALUE)
    ::SetStdHandle(STD_INPUT_HANDLE, hIn);
  if (hOut != INVALID_HANDLE_VALUE) {
    ::SetStdHandle(STD_OUTPUT_HANDLE, hOut);
    ::SetStdHandle(STD_ERROR_HANDLE, hOut);
  }

  // Rebind CRT streams to the console device so Ruby's puts reaches the window.
  if (hIn != INVALID_HANDLE_VALUE)
    std::freopen("CONIN$", "r", stdin);
  if (hOut != INVALID_HANDLE_VALUE) {
    std::freopen("CONOUT$", "w", stdout);
    std::freopen("CONOUT$", "w", stderr);
  }

  ::ShowWindow(::GetConsoleWindow(), SW_SHOW);
}
#endif

#if defined(OS_ANDROID)
// Crash self-capture. When the engine dies on SIGSEGV/SIGABRT/SIGBUS the
// system's crash_dump writes the backtrace to the logcat ring buffer only —
// which the diag export may truncate before anyone reads it. Instead we write
// module+offset frames ourselves, directly (signal-safe open/write), into a
// dedicated file that the export copies verbatim. The pc offsets are enough to
// symbolize offline against the local unstripped .so.
#include <android/log.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <exception>
#include <signal.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>

namespace {

void UrgeLogBacktrace();
void UrgeLogStackFrames(uintptr_t sp);
void UrgeCrashLogLine(const char* text);

struct UrgeBacktraceState {
  uintptr_t frames[32];
  size_t count;
};

_Unwind_Reason_Code UrgeUnwindCallback(struct _Unwind_Context* context,
                                       void* arg) {
  UrgeBacktraceState* state = static_cast<UrgeBacktraceState*>(arg);
  uintptr_t pc = _Unwind_GetIP(context);
  if (pc != 0) {
    if (state->count >= sizeof(state->frames) / sizeof(state->frames[0]))
      return _URC_END_OF_STACK;
    state->frames[state->count++] = pc;
  }
  return _URC_NO_REASON;
}

// Dumps the ring buffer of recent GPU operations (see renderer/device/
// gpu_audit.h): the engine operation that led to the crash, with the surface
// state recorded at that moment.
static void UrgeLogGpuAudit() {
#if defined(OS_ANDROID)
  char line[256];
  const uint32_t count = renderer::GpuAudit::count();
  if (!count)
    return;

  UrgeCrashLogLine("---- gpu operations before crash (oldest first) ----\n");
  const renderer::GpuAudit::Entry& newest =
      renderer::GpuAudit::Get(count - 1);
  for (uint32_t i = 0; i < count; ++i) {
    const renderer::GpuAudit::Entry& entry = renderer::GpuAudit::Get(i);
    snprintf(line, sizeof(line), "gpu[%u] %s surface=%s t=-%.3fms\n",
             entry.seq, entry.what ? entry.what : "?",
             entry.surface_valid ? "valid" : "INVALID",
             renderer::GpuAudit::MillisBefore(entry, newest));
    UrgeCrashLogLine(line);
  }
#endif  //! OS_ANDROID
}

void UrgeWriteCrashReport(int signal_number,
                          siginfo_t* info,
                          void* ucontext_void,
                          uintptr_t sp,
                          uintptr_t pc) {
  char line[320];
  // fault_pc is the instruction that actually faulted — the only value here
  // that needs no unwinding, so it stays trustworthy even when the unwinder
  // itself dies on a table-less frame. UrgeSetCrashTag() already turned it
  // into the report's self-describing file name.
  int n = snprintf(line, sizeof(line),
                   "==== crash signal=%d fault_addr=%p fault_pc=0x%zx pid=%d ====\n",
                   signal_number, info ? info->si_addr : nullptr,
                   static_cast<size_t>(pc), static_cast<int>(getpid()));
  UrgeCrashLogLine(line);

  // When the faulting pc itself is null (a call through an empty vtable or
  // function pointer) the pc tells nothing — but lr (x30) still points at the
  // instruction that made the call. Dump the core registers so those crashes
  // are diagnosable too.
  if (ucontext_void) {
    auto* uc = static_cast<ucontext_t*>(ucontext_void);
#if defined(__aarch64__)
    n = snprintf(line, sizeof(line),
                 "regs x0=%llx x1=%llx x2=%llx x3=%llx x4=%llx x5=%llx "
                 "x6=%llx x7=%llx\n",
                 (unsigned long long)uc->uc_mcontext.regs[0],
                 (unsigned long long)uc->uc_mcontext.regs[1],
                 (unsigned long long)uc->uc_mcontext.regs[2],
                 (unsigned long long)uc->uc_mcontext.regs[3],
                 (unsigned long long)uc->uc_mcontext.regs[4],
                 (unsigned long long)uc->uc_mcontext.regs[5],
                 (unsigned long long)uc->uc_mcontext.regs[6],
                 (unsigned long long)uc->uc_mcontext.regs[7]);
    UrgeCrashLogLine(line);
    n = snprintf(line, sizeof(line),
                 "regs x8=%llx x9=%llx x10=%llx x11=%llx x19=%llx x20=%llx "
                 "x21=%llx x22=%llx\n",
                 (unsigned long long)uc->uc_mcontext.regs[8],
                 (unsigned long long)uc->uc_mcontext.regs[9],
                 (unsigned long long)uc->uc_mcontext.regs[10],
                 (unsigned long long)uc->uc_mcontext.regs[11],
                 (unsigned long long)uc->uc_mcontext.regs[19],
                 (unsigned long long)uc->uc_mcontext.regs[20],
                 (unsigned long long)uc->uc_mcontext.regs[21],
                 (unsigned long long)uc->uc_mcontext.regs[22]);
    UrgeCrashLogLine(line);
    n = snprintf(line, sizeof(line),
                 "regs lr(x30)=0x%llx sp=0x%llx pc=0x%llx\n",
                 (unsigned long long)uc->uc_mcontext.regs[30],
                 (unsigned long long)uc->uc_mcontext.sp,
                 (unsigned long long)uc->uc_mcontext.pc);
    UrgeCrashLogLine(line);
#endif
  }

  // GPU operations performed right before the crash. Written before the
  // unwinder runs: the audit tells which engine operation preceded the driver
  // crash, which the crash address alone can never reveal.
  UrgeLogGpuAudit();

  // Safe scan first: if the unwinder below crashes on a table-less Ruby frame,
  // the scan (and the header) are already on disk.
  UrgeLogStackFrames(sp);
  UrgeLogBacktrace();
}

// Self-describing artifact name: the module and pc offset of the faulting
// instruction go into the FILENAME (crash_<module>_0x<offset>_p<pid>.log), so
// the diagnosis survives even a corrupted transfer and can be read straight
// from any file browser without opening the file.
static char g_crash_file_base[192] = "handler_start";

void UrgeSetCrashTag(uintptr_t pc) {
  char tag[128] = "unknown";
  Dl_info sym;
  memset(&sym, 0, sizeof(sym));
  if (pc != 0 && dladdr(reinterpret_cast<void*>(pc), &sym) != 0 &&
      sym.dli_fname != nullptr) {
    const char* name = strrchr(sym.dli_fname, '/');
    name = name ? name + 1 : sym.dli_fname;
    uintptr_t base = reinterpret_cast<uintptr_t>(sym.dli_fbase);
    snprintf(tag, sizeof(tag), "%s_0x%zx", name,
             static_cast<size_t>(pc - base));
  }
  snprintf(g_crash_file_base, sizeof(g_crash_file_base), "crash_%s_p%d", tag,
           static_cast<int>(getpid()));
}

void UrgeCrashLogLine(const char* text) {
  // Write into every known diag dir: the internal one is only reachable on a
  // PC through the periodic export (which loses the race against the crash),
  // while the removable one is the copy the user actually reads. The file name
  // itself carries the verdict (see UrgeSetCrashTag).
  const std::string* dirs[] = {&g_diag_path, &g_removable_diag_path};
  for (const std::string* dir : dirs) {
    if (dir->empty())
      continue;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.log", dir->c_str(), g_crash_file_base);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
      write(fd, text, strlen(text));
      close(fd);
    }
  }
  __android_log_print(ANDROID_LOG_FATAL, "urgecrash", "%s", text);
}

void UrgeLogBacktrace() {
  UrgeBacktraceState state = {};
  _Unwind_Backtrace(UrgeUnwindCallback, &state);

  char line[320];
  for (size_t i = 0; i < state.count; ++i) {
    uintptr_t pc = state.frames[i];
    Dl_info sym;
    memset(&sym, 0, sizeof(sym));
    const char* module = "??";
    uintptr_t base = 0;
    if (dladdr(reinterpret_cast<void*>(pc), &sym) != 0 &&
        sym.dli_fname != nullptr) {
      module = sym.dli_fname;
      base = reinterpret_cast<uintptr_t>(sym.dli_fbase);
    }
    snprintf(line, sizeof(line), "#%02zu pc %p %s base=%p off=0x%zx\n", i,
             reinterpret_cast<void*>(pc), module,
             reinterpret_cast<void*>(base), static_cast<size_t>(pc - base));
    UrgeCrashLogLine(line);
  }
}

// Scan the crashed thread's stack for return addresses.
//
// _Unwind_Backtrace consults unwind tables, and Ruby's C frames have none: the
// unwinder then dereferences garbage and kills the process in the middle of
// writing the report (the handler itself was crashing). A plain scan touches
// only the stack page that already contains the SP, so it cannot fault, and the
// addresses it turns up are symbolized offline.
void UrgeLogStackFrames(uintptr_t sp) {
  if (sp == 0)
    return;

  const uintptr_t kPageMask = ~static_cast<uintptr_t>(4095);
  uintptr_t start = sp & ~static_cast<uintptr_t>(3);
  uintptr_t end = (sp & kPageMask) + 4096;  // stay inside the page holding sp

  char line[320];
  for (uintptr_t addr = start; addr + sizeof(uintptr_t) <= end;
       addr += sizeof(uintptr_t)) {
    uintptr_t candidate = *reinterpret_cast<uintptr_t*>(addr);
    Dl_info sym;
    memset(&sym, 0, sizeof(sym));
    if (dladdr(reinterpret_cast<void*>(candidate), &sym) == 0 ||
        sym.dli_fname == nullptr)
      continue;
    uintptr_t base = reinterpret_cast<uintptr_t>(sym.dli_fbase);
    snprintf(line, sizeof(line), "stack+%04lu pc %p %s base=%p off=0x%zx\n",
             static_cast<unsigned long>(addr - start),
             reinterpret_cast<void*>(candidate), sym.dli_fname,
             reinterpret_cast<void*>(base),
             static_cast<size_t>(candidate - base));
    UrgeCrashLogLine(line);
  }
}

// An exception that escapes into Ruby's C frames cannot unwind (those frames
// carry no unwind tables), so it lands here instead of in the binding method's
// catch block. Capture the message and the stack before aborting: this is the
// only place where the original cause is still visible.
void UrgeTerminateHandler() {
  std::string message = "no active exception";
  try {
    std::exception_ptr current = std::current_exception();
    if (current)
      std::rethrow_exception(current);
  } catch (const std::exception& e) {
    message = e.what();
  } catch (...) {
    message = "non-std exception";
  }

  char line[512];
  snprintf(line, sizeof(line), "==== std::terminate: %s (pid=%d) ====\n",
           message.c_str(), static_cast<int>(getpid()));
  UrgeCrashLogLine(line);
  UrgeLogStackFrames(reinterpret_cast<uintptr_t>(&message) & ~static_cast<uintptr_t>(3));
  UrgeLogBacktrace();
  abort();
}

void UrgeCrashHandler(int signal_number, siginfo_t* info, void* context) {
  uintptr_t sp = 0;
  uintptr_t pc = 0;
  if (context) {
    auto* uc = static_cast<ucontext_t*>(context);
#if defined(__arm__)
    sp = static_cast<uintptr_t>(uc->uc_mcontext.arm_sp);
    pc = static_cast<uintptr_t>(uc->uc_mcontext.arm_pc);
#elif defined(__aarch64__)
    sp = static_cast<uintptr_t>(uc->uc_mcontext.sp);
    pc = static_cast<uintptr_t>(uc->uc_mcontext.pc);
#endif
  }
  UrgeSetCrashTag(pc);
  UrgeWriteCrashReport(signal_number, info, context, sp, pc);
  // Restore the default disposition and re-raise so the system still produces
  // its own tombstone / crash dump on top of our report.
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_DFL;
  sigaction(signal_number, &sa, nullptr);
  raise(signal_number);
}

void InstallUrgeCrashHandler() {
  // SA_ONSTACK needs an actual alternate stack. Without one the handler runs on
  // the faulting thread's stack, which may already be exhausted — the handler
  // would then die before writing anything.
  static uint8_t s_alt_stack[64 * 1024];
  stack_t ss;
  memset(&ss, 0, sizeof(ss));
  ss.ss_sp = s_alt_stack;
  ss.ss_size = sizeof(s_alt_stack);
  sigaltstack(&ss, nullptr);

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = UrgeCrashHandler;
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK;
  int r_segv = sigaction(SIGSEGV, &sa, nullptr);
  int r_abrt = sigaction(SIGABRT, &sa, nullptr);
  int r_bus = sigaction(SIGBUS, &sa, nullptr);
  std::set_terminate(UrgeTerminateHandler);

  // Self-check marker: proves the handler is live and shows where the report
  // would be written. If this banner is missing, the file path itself is wrong
  // (or this build never reached the engine); if it is present without a crash
  // record after a crash, something re-installed its own handler on top.
  char banner[512];
  snprintf(banner, sizeof(banner),
           "==== engine start pid=%d diag=%s install=%d,%d,%d ====\n",
           static_cast<int>(getpid()), g_diag_path.c_str(), r_segv, r_abrt,
           r_bus);
  // logcat only: no per-start file, the banner served its purpose long ago.
  __android_log_print(ANDROID_LOG_INFO, "urgecrash", "%s", banner);
}

}  // namespace
#endif  // OS_ANDROID

#if defined(OS_ANDROID)
// Re-arm hook for BindingEngineMri: ruby_init() installs its own SIGSEGV
// handler (the "[BUG]" reporter) on top of ours, which left every native crash
// in the game loop without any report at all. Call this right after the Ruby
// VM is up.
void UrgeRearmCrashHandler() {
  InstallUrgeCrashHandler();
}
#endif  //! OS_ANDROID

int main(int argc, char* argv[]) {
#if defined(OS_ANDROID)
  InstallUrgeCrashHandler();
#endif
#if defined(OS_WIN)
  // Allocate console if need
  for (int i = 0; i < argc; ++i) {
    if (!std::strcmp(argv[i], "console")) {
      CreateConsoleWin(true);
      break;
    }
  }
#endif  //! defined(OS_WIN)

#if defined(OS_ANDROID)
  SetupAndroidStudioTransfer();
#endif  // !defined(OS_ANDROID)

#if defined(OS_ANDROID)
  // Get GAME_PATH string field from JNI (MainActivity.java)
  JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
  jobject activity = (jobject)SDL_GetAndroidActivity();
  jclass activity_klass = env->GetObjectClass(activity);
  jfieldID field_game_path =
      env->GetStaticFieldID(activity_klass, "GAME_PATH", "Ljava/lang/String;");
  jstring java_string_game_path =
      (jstring)env->GetStaticObjectField(activity_klass, field_game_path);
  const char* game_data_dir = env->GetStringUTFChars(java_string_game_path, 0);

  // Set and ensure current directory
  std::filesystem::path std_path(game_data_dir);
  if (!std::filesystem::exists(std_path) ||
      !std::filesystem::is_directory(std_path))
    std::filesystem::create_directories(std_path);

  std::filesystem::current_path(std_path);

  env->ReleaseStringUTFChars(java_string_game_path, game_data_dir);
  env->DeleteLocalRef(java_string_game_path);
  env->DeleteLocalRef(activity_klass);

  // Fixed configure file
  std::string app = "Game";
  std::string ini = app + ".ini";
#elif defined(OS_EMSCRIPTEN)
  std::string app = "Game";
  std::string ini = app + ".ini";
#else
  std::string app(argv[0]);
  for (size_t i = 0; i < app.size(); ++i)
    if (app[i] == '\\')
      app[i] = '/';

  auto last_sep = app.find_last_of('/');
  if (last_sep != std::string::npos)
    app = app.substr(last_sep + 1);

  last_sep = app.find_last_of('.');
  if (last_sep != std::string::npos)
    app = app.substr(0, last_sep);
  std::string ini = app + ".ini";
#endif  //! defined(OS_ANDROID)

  // Current path
  auto current_path =
      std::filesystem::current_path().generic_u8string();

  // Initialize filesystem
  std::unique_ptr<filesystem::IOService> io_service =
      filesystem::IOService::Create(argv[0]);
  io_service->AddLoadPath(reinterpret_cast<const char*>(current_path.c_str()),
                          "", false);
  io_service->SetWritePath(reinterpret_cast<const char*>(current_path.c_str()));

  filesystem::IOState io_state;
  SDL_IOStream* inifile = io_service->OpenReadRaw(ini, &io_state);
  if (io_state.error_count) {
    std::string error_info = "Failed to load configure: ";
    error_info += ini;
    error_info += '\n';
    error_info += "Current path: ";
    error_info += reinterpret_cast<const char*>(current_path.c_str());

    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "URGE", error_info.c_str(),
                             nullptr);
    return 1;
  }

  // Initialize profile
  std::unique_ptr<content::ContentProfile> profile =
      std::make_unique<content::ContentProfile>(app, inifile);
  profile->LoadCommandLine(argc, argv);

  if (!profile->LoadConfigure(app)) {
    std::string error_message = "Error when parse configure file: \n";
    error_message += ini;
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "URGE",
                             error_message.c_str(), nullptr);
    return 1;
  }

#if defined(OS_WIN)
  // Always create our own debug console so the C runtime stdout/stderr handles
  // are connected; the debugging_console flag only controls window visibility.
  CreateConsoleWin(profile->debugging_console);
#endif

  // Create spdlog logger
#if defined(OS_ANDROID)
  auto android_sink =
      std::make_shared<spdlog::sinks::android_sink_mt>("urgecore");
  android_sink->set_pattern("[%^%l%$] %v");

#if defined(OS_ANDROID)
  // 所有日志统一并入 files/urge_debug/（Java 侧导出与收集只看这一个目录）。
  std::string game_log_path = g_diag_path.empty()
                                  ? app + ".log"
                                  : g_diag_path + "/Game.log";
#else
  std::string game_log_path = app + ".log";
#endif
  auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
      game_log_path, true);
  file_sink->set_level(spdlog::level::trace);
#else
  auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  console_sink->set_pattern("[%^%l%$] %v");
#endif

  std::vector<spdlog::sink_ptr> logger_sinks;
#if defined(OS_ANDROID)
  logger_sinks.push_back(android_sink);
  logger_sinks.push_back(file_sink);
#else
  logger_sinks.push_back(console_sink);
#endif

  spdlog::logger logger_sink("urgecore", logger_sinks.begin(),
                             logger_sinks.end());
  // Flush on every line. Without this the last few seconds of log sit in the
  // stdio buffer and are lost when the process dies abruptly — exactly the
  // window we need (Game.log used to stop mid-script-eval while the engine
  // kept running for ~6 more seconds before dying).
  logger_sink.flush_on(spdlog::level::trace);
  base::logging::InitWithLogger(&logger_sink);

  LOG(INFO) << "[App] Current Path: "
            << reinterpret_cast<const char*>(current_path.c_str());
  LOG(INFO) << "[App] Configure File: " << ini;

  if (profile->game_debug)
    LOG(INFO) << "[App] Running debug test.";
  if (profile->game_battle_test)
    LOG(INFO) << "[App] Running battle test.";

// Setup encryption resource package
#if HAVE_ARB_ENCRYPTO_SUPPORT
  std::string app_package = app + ".arb";
  if (admenri::LoadCryptoPackage(app_package))
    LOG(INFO) << "[IOService] Encrypto pack \"" << app_package << "\" added.";
#endif

  // Disable IME on Windows
#if defined(OS_WIN)
  if (profile->disable_ime) {
    LOG(INFO) << "[Windows] Disable process IME.";
    ::ImmDisableIME(TRUE);
  }
#endif

  // Setup SDL init params
  SDL_SetHint(SDL_HINT_ORIENTATIONS, profile->orientation.c_str());
  SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
  SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");

  SDL_Init(SDL_INIT_EVENTS | SDL_INIT_VIDEO | SDL_INIT_AUDIO);
  TTF_Init();

  {
    // Initialize i18n profile
    auto* i18n_xml_stream =
        io_service->OpenReadRaw(profile->i18n_xml_path, nullptr);
    auto i18n_profile = std::make_unique<content::I18NProfile>(i18n_xml_stream);

    // Initialize font context
    auto font_context = std::make_unique<content::ScopedFontData>(
        io_service.get(), profile->default_font_path);

    {
      // Initialize engine main widget
      std::unique_ptr<ui::Widget> widget(new ui::Widget(true));
      ui::Widget::InitParams widget_params;
#if defined(OS_LINUX)
      widget_params.opengl = profile->driver_backend == "OPENGL";
#endif
      widget_params.size = profile->window_size;
      widget_params.resizable = true;
      widget_params.hpixeldensity =
#if !defined(OS_EMSCRIPTEN)
          true;
#else
          false;
#endif
      widget_params.fullscreen =
#if defined(OS_ANDROID)
          true;
#else
          profile->fullscreen;
#endif
      widget_params.title = profile->window_title;
      widget->Init(std::move(widget_params));

#if defined(OS_ANDROID)
      // SDL and its subsystems may install their own handlers during Init;
      // re-arm ours so a crash in the game loop still produces a report.
      InstallUrgeCrashHandler();
#endif

      binding::MriSetTvDevice(g_tv_device);

      // Setup content runner module
      content::ContentRunner::InitParams content_params;
      content_params.profile = profile.get();
      content_params.io_service = io_service.get();
      content_params.font_context = font_context.get();
      content_params.i18n_profile = i18n_profile.get();
      content_params.window = widget->AsWeakPtr();
      content_params.entry = std::make_unique<binding::BindingEngineMri>();

      std::unique_ptr<content::ContentRunner> runner =
          content::ContentRunner::Create(std::move(content_params));
      if (runner) {
        // Run main loop if no exception
        runner->RunMainLoop();
      } else {
        // Throw exception when initializing
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "URGE",
                                 "Failed to load engine.", nullptr);
      }

      // Finalize modules at end
      runner.reset();
      widget.reset();
    }

    // Release resources
    font_context.reset();
    i18n_profile.reset();
  }

  // Release objects
  profile.reset();
  io_service.reset();

  TTF_Quit();
  SDL_Quit();

#if defined(OS_ANDROID)
  // Returning from SDL_main hands control back to SDLActivity, which then
  // finishes the Activity: the static destructors of the engine and the Vulkan
  // loader cleanup race against the dying Activity and kill the process with a
  // silent SIGSEGV (no tombstone, no handler output - observed right after
  // "Finished main function"). The OS reclaims everything anyway, so exit hard.
  _exit(0);
#endif  //! OS_ANDROID

  return 0;
}
