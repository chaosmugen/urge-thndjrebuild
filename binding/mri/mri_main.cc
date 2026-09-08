// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD - style license that can be
// found in the LICENSE file.

#include "binding/mri/mri_util.h"
#include "ruby/version.h"

#include "binding/mri/mri_main.h"

#include "SDL3/SDL_events.h"
#include "SDL3/SDL_messagebox.h"
#include "zlib/zlib.h"

#include "binding/mri/binding_patch.h"
#include "binding/mri/mri_file.h"
#include "binding/mri/mri_init_autogen.h"
#include "binding/mri/platform/mri_emscripten.h"

#ifdef HAVE_GET_MACHINE_HASH
#include "admenri/machineid/machineid.h"
#endif

extern "C" {
void Init_zlib(void);
void Init_ffi_c(void);
void rb_call_builtin_inits();
}

#if defined(OS_ANDROID)
// Defined in app/urge_main.cc: re-arms the engine's crash handler after the
// Ruby VM replaced it with its own "[BUG]" reporter during ruby_init().
void UrgeRearmCrashHandler();
#include <sys/system_properties.h>
#endif  //! OS_ANDROID

namespace binding {

content::ExecutionContext* g_current_execution_context = nullptr;
extern filesystem::IOService* g_io_service;

namespace {
bool s_tv_device = false;
}  // namespace

void MriSetTvDevice(bool is_tv) { s_tv_device = is_tv; }

namespace {

VALUE EvalString(VALUE string, VALUE filename, int32_t* state) {
  using EvaluteContext = struct {
    VALUE string;
    VALUE filename;
  };

  auto evaluate = [](VALUE parameter) -> VALUE {
    EvaluteContext* context = reinterpret_cast<EvaluteContext*>(parameter);
    VALUE argv[] = {context->string, Qnil, context->filename};
    return rb_funcall2(Qnil, rb_intern("eval"), std::size(argv), argv);
  };

  EvaluteContext context = {string, filename};
  return rb_protect(evaluate, reinterpret_cast<VALUE>(&context), state);
}

std::string InsertNewLines(const std::string& input, size_t interval) {
  std::string result;
  size_t length = input.length();

  for (size_t i = 0; i < length; i += interval) {
    result += input.substr(i, interval);
    if (i + interval < length)
      result += '\n';
  }

  return result;
}

std::string ParseExeceptionInfo(VALUE exception) {
  VALUE exeception_name = rb_class_path(rb_obj_class(exception));
  VALUE exception_message = rb_obj_as_string(exception);
  VALUE backtrace = rb_funcall2(exception, rb_intern("backtrace"), 0, NULL);
  VALUE backtrace_front = rb_str_new2("Unknown Location");
  if (backtrace != Qnil)
    backtrace_front = rb_ary_entry(backtrace, 0);

  VALUE format_errors =
      rb_sprintf("%" PRIsVALUE " (%" PRIsVALUE ") :\n%" PRIsVALUE,
                 backtrace_front, exeception_name, exception_message);

  std::string error_info = StringValueCStr(format_errors);
  return InsertNewLines(error_info, 128);
}

void MriProcessReset() {
  content::ExceptionState exception_state;
  MriGetGlobalModules()->Graphics->Reset(exception_state);
  MriGetGlobalModules()->Audio->Reset(exception_state);
  MriGetGlobalModules()->URGE->Reset(exception_state);
  MriProcessException(exception_state);
}

static VALUE RescueCallBlock(VALUE block) {
  return rb_funcall2(block, rb_intern("call"), 0, nullptr);
};

static VALUE RescueException(VALUE param, VALUE exception) {
  return *reinterpret_cast<VALUE*>(param) = exception;
};

MRI_METHOD(MRI_RGSSMain) {
  bool gc_required = false;

  LOG(INFO) << "[Binding] Game loop started";
  while (true) {
    VALUE exception = Qnil;
    if (gc_required) {
      rb_gc_start();
      gc_required = false;
    }

    // Ruby 3.x: rb_rescue2 takes strongly-typed callbacks
    // (VALUE(*)(VALUE) / VALUE(*)(VALUE, VALUE)) — no ANYARGS cast needed.
    rb_rescue2(RescueCallBlock, rb_block_proc(), RescueException,
               (VALUE)&exception, rb_eException, nullptr);

    if (NIL_P(exception))
      break;

    if (rb_obj_class(exception) ==
        MriGetException(content::ExceptionCode::CODE_NUMS)) {
      gc_required = true;
      MriProcessReset();
    } else {
      gc_required = false;
      // The game raised for real. Log it before re-raising — this is the only
      // place where the reason for an in-game quit becomes visible.
      LOG(ERROR) << "[Binding] Game loop exception: "
                 << ParseExeceptionInfo(exception);
      rb_exc_raise(exception);
    }
  }

  return Qnil;
}

MRI_METHOD(MRI_RGSSStop) {
  scoped_refptr<content::Graphics> screen = MriGetGlobalModules()->Graphics;

  content::ExceptionState exception_state;
  while (true)
    screen->Update(exception_state);

  return Qnil;
}

#ifdef HAVE_GET_MACHINE_HASH
MRI_METHOD(ADMENRI_getMachineHash) {
  auto hash = admenri::GetMachineHash();
  return rb_str_new(hash.data(), hash.size());
}
#endif

}  // namespace

BindingEngineMri::BindingEngineMri() : profile_(nullptr) {}

BindingEngineMri::~BindingEngineMri() = default;

void BindingEngineMri::PreEarlyInitialization(
    content::ContentProfile* profile,
    filesystem::IOService* io_service) {
  profile_ = profile;
  g_io_service = io_service;

  int32_t argc = 0;
  char** argv = nullptr;
  ruby_sysinit(&argc, &argv);

  RUBY_INIT_STACK;
  ruby_init();
  ruby_init_loadpath();

#if defined(OS_ANDROID)
  // ruby_init() installs its own SIGSEGV handler (the "[BUG]" reporter),
  // replacing the engine's crash handler: from here on a native crash in the
  // game loop produced no report at all. Re-arm ours on top.
  UrgeRearmCrashHandler();
#endif  //! OS_ANDROID

  // Platform flags for the game scripts: the Android TV boot path cannot feed
  // the full data preload (the box runs out of RAM before the title screen).
  rb_define_global_const("URGE_ANDROID_TV", s_tv_device ? Qtrue : Qfalse);
  LOG(INFO) << "[Binding] URGE_ANDROID_TV = "
            << (s_tv_device ? "true" : "false");

#if RAPI_FULL >= 300
  rb_call_builtin_inits();
#endif  //! RAPI_FULL >= 300

  rb_enc_set_default_internal(rb_enc_from_encoding(rb_utf8_encoding()));
  rb_enc_set_default_external(rb_enc_from_encoding(rb_utf8_encoding()));

  // internal exception
  MriInitException(profile->api_version >=
                   content::ContentProfile::APIVersion::RGSS3);

  // marshal data reader
  InitCoreFileBinding();

  // URGE autogen bindings
  InitMriAutogen();

  // C extensions
  Init_zlib();
#if defined(OS_WIN)
  Init_ffi_c();
#endif

  // Autogen binding patching
  MriApplyBindingPatch();

#if defined(OS_EMSCRIPTEN)
  InitEmscriptenBinding();
#endif  // OS_EMSCRIPTEN

  MriDefineModuleFunction(rb_mKernel, "rgss_main", MRI_RGSSMain);
  MriDefineModuleFunction(rb_mKernel, "rgss_stop", MRI_RGSSStop);

#ifdef HAVE_GET_MACHINE_HASH
  VALUE class_admenri = rb_define_module("Admenri");
  MriDefineModuleFunction(class_admenri, "get_machine_hash",
                          ADMENRI_getMachineHash);
#endif

  LOG(INFO) << "[Binding] CRuby Interpreter Version: " << RUBY_API_VERSION_CODE;
  LOG(INFO) << "[Binding] CRuby Interpreter Platform: " << RUBY_PLATFORM;

  VALUE debug = MRI_BOOL_VALUE(profile->game_debug);
  if (profile->api_version < content::ContentProfile::APIVersion::RGSS2)
    rb_gv_set("DEBUG", debug);
  else
    rb_gv_set("TEST", debug);
  rb_gv_set("BTEST", MRI_BOOL_VALUE(profile->game_battle_test));
}

void BindingEngineMri::OnMainMessageLoopRun(
    content::ExecutionContext* execution,
    ScopedModuleContext* module_context) {
  // Set global execution context
  g_current_execution_context = execution;

  // Define running modules
  MriGetGlobalModules()->Graphics = module_context->graphics;
  MriGetGlobalModules()->Input = module_context->input;
  MriGetGlobalModules()->Audio = module_context->audio;
  MriGetGlobalModules()->Mouse = module_context->mouse;
  MriGetGlobalModules()->URGE = module_context->engine;

  // Run packed scripts
  content::ExceptionState exception_state;
  LoadPackedScripts(profile_, exception_state);
  if (exception_state.HadException()) {
    std::string error_message;
    exception_state.FetchException(error_message);
    // The messagebox is easily missed (or unfocusable) on a TV, so the reason
    // must also land in the log — this is what explains a "silent" quit.
    LOG(ERROR) << "[Binding] " << error_message;
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "URGE",
                             error_message.c_str(), nullptr);
  }
}

void BindingEngineMri::PostMainLoopRunning() {
  // Show exception info
  VALUE exception = rb_errinfo();
  std::string exception_message;
  if (!NIL_P(exception) && !rb_obj_is_kind_of(exception, rb_eSystemExit))
    exception_message = ParseExeceptionInfo(exception);

  // Clean up ruby vm
  ruby_finalize();

  // Release binding reference
  g_current_execution_context = nullptr;
  GlobalModules empty_modules;
  std::swap(*MriGetGlobalModules(), empty_modules);

  // Show message box for errors
  if (!exception_message.empty())
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Scripts Error",
                             exception_message.c_str(), nullptr);

  LOG(INFO) << "[Binding] Quit CRuby binding engine.";
}

void BindingEngineMri::ExitSignalRequired() {
  rb_raise(rb_eSystemExit, "");
}

void BindingEngineMri::ResetSignalRequired() {
  VALUE rb_eReset = MriGetException(content::ExceptionCode::CODE_NUMS);
  rb_raise(rb_eReset, "");
}

void BindingEngineMri::LoadPackedScripts(
    content::ContentProfile* profile,
    content::ExceptionState& exception_state) {
  // No events are pumped while the scripts load (tens of seconds), so the
  // lifecycle state runs stale: a pause/resume from that window must be
  // processed before any GPU work happens again, otherwise the swap chain is
  // still bound to a surface that no longer exists (freeAllBuffers while
  // being dequeued, then SIGSEGV). Blocking pump semantics even park the
  // load itself while the user is away, which is the desired behavior.
#if defined(OS_ANDROID)
  SDL_PumpEvents();
#endif  //! OS_ANDROID

  VALUE packed_scripts = MriLoadData(profile->script_path, exception_state);
  if (exception_state.HadException())
    return;

  if (!RB_TYPE_P(packed_scripts, RUBY_T_ARRAY)) {
    LOG(INFO) << "[Binding] Failed to read script data.";
    return;
  }

  rb_gv_set("$RGSS_SCRIPTS", packed_scripts);

  int32_t scripts_count = RARRAY_LEN(packed_scripts);
  std::string zlib_decode_buffer(0x1000, 0);

  for (int32_t i = 0; i < scripts_count; ++i) {
    VALUE script = rb_ary_entry(packed_scripts, i);

    // 0 -> ScriptID for Editor
    VALUE script_name = rb_ary_entry(script, 1);
    VALUE script_src = rb_ary_entry(script, 2);

    unsigned long buffer_size;
    uint32_t source_size = 0;

    int32_t zlib_result = Z_OK;

    while (true) {
      uint8_t* buffer_ptr =
          reinterpret_cast<uint8_t*>(zlib_decode_buffer.data());
      buffer_size = zlib_decode_buffer.length();

      const uint8_t* source_ptr =
          reinterpret_cast<const uint8_t*>(RSTRING_PTR(script_src));
      source_size = RSTRING_LEN(script_src);

      zlib_result =
          uncompress(buffer_ptr, &buffer_size, source_ptr, source_size);

      // NOTE: no NUL terminator write here. When uncompress() bails out with
      // Z_BUF_ERROR it sets *destLen to the whole buffer capacity, so the old
      // buffer_ptr[buffer_size] = '\0' wrote one byte past the end of the
      // std::string allocation and corrupted the heap. The corruption blew up
      // later as a null dereference inside the Ruby VM while the scripts were
      // being evaluated (SIGSEGV, fault addr 0x0, in rb_class_new_instance).

      // zlib reports Z_DATA_ERROR (-3) instead of Z_BUF_ERROR (-5) whenever the
      // output buffer ran out AND the whole input had already been consumed
      // (see uncompress2() in zlib). Both codes mean "buffer too small" here, so
      // both have to trigger a retry with a bigger buffer.
      if (zlib_result != Z_BUF_ERROR && zlib_result != Z_DATA_ERROR)
        break;

      // Grow-only, and never below the initial size: an empty script would
      // otherwise leave the buffer at zero, where doubling can never recover.
      size_t next_size = zlib_decode_buffer.size() * 2;
      if (next_size < 0x1000) next_size = 0x1000;
      if (next_size > (size_t)64 * 1024 * 1024) break;  // sanity cap
      zlib_decode_buffer.resize(next_size);
    }

    if (zlib_result != Z_OK) {
      // zlib codes: -3 Z_DATA_ERROR (corrupt input), -4 Z_MEM_ERROR,
      // -5 Z_BUF_ERROR. Logging the code plus the input size makes the real
      // failure reason visible without a debugger.
      LOG(INFO) << "[Binding] Error when decoding: "
                << StringValueCStr(script_name) << " (zlib code " << zlib_result
                << ", source " << source_size << " bytes)";
      break;
    }

    // Hand the exact byte range to Ruby, so embedded NUL bytes cannot truncate
    // the script source. The decode buffer itself is deliberately NOT shrunk
    // back: it is grow-only, otherwise a tiny script leaves it too small for the
    // next one and zlib reports that as a data error.
    rb_ary_store(script, 3,
                 rb_str_new(zlib_decode_buffer.data(), (long)buffer_size));
  }

  for (;;) {
    for (int32_t i = 0; i < scripts_count; ++i) {
      VALUE script = rb_ary_entry(packed_scripts, i);
      VALUE script_name = rb_ary_entry(script, 1);
      VALUE script_source = rb_ary_entry(script, 3);

      // The decode loop above stops at the first script it cannot decompress,
      // so every later entry keeps Qnil here. Dereferencing that used to be a
      // guaranteed SIGSEGV (Qnil is 4 on 32-bit); skip out with a message
      // instead so the log shows which script actually failed.
      if (!RB_TYPE_P(script_source, RUBY_T_STRING)) {
        LOG(ERROR) << "[Binding] Script #" << (i + 1)
                   << " has no decoded source, decoding already failed on \""
                   << StringValueCStr(script_name) << "\"";
        break;
      }

      // The eval pass takes tens of seconds without any event pump of its
      // own: process pending lifecycle events regularly, so a pause/resume
      // from that window does not leave the swap chain bound to a dead
      // surface for the whole game session.
#if defined(OS_ANDROID)
      if ((i % 16) == 0)
        SDL_PumpEvents();
#endif  //! OS_ANDROID

      // Logged before every eval, so the last "Evaluating script" line in the log
      // identifies the exact script that dies inside the Ruby VM (a native crash
      // carries no Ruby backtrace of its own).
      LOG(INFO) << "[Binding] Evaluating script #" << (i + 1) << ": "
                << StringValueCStr(script_name);

      std::stringstream format_filename;
      format_filename << std::to_string(i + 1) << " - "
                      << StringValueCStr(script_name);

      int32_t state = 0;
      std::string filename = format_filename.str().c_str();
      EvalString(
          MriStringUTF8(RSTRING_PTR(script_source), RSTRING_LEN(script_source)),
          MriStringUTF8(filename.data(), filename.size()), &state);
      if (state) {
        // A failed eval used to break out silently: the engine kept running
        // without the rest of the scripts and the reason never reached any log.
        LOG(ERROR) << "[Binding] Script eval failed: "
                   << ParseExeceptionInfo(rb_errinfo());
        break;
      }
    }

    VALUE exception = rb_errinfo();
    if (rb_obj_class(exception) !=
        MriGetException(content::ExceptionCode::CODE_NUMS))
      break;

    MriProcessReset();
    rb_gc_start();
  }
}

}  // namespace binding
