// wick-helper (Windows): CEF subprocess with Chrome API stealth patches.
//
// Handles the renderer, GPU, utility and other CEF subprocess types. For
// renderer subprocesses it can inject STEALTH_JS in OnContextCreated, before
// any page script runs — the job helper.m and helper_linux.c were written
// for. The injection is opt-in here (WICK_CEF_STEALTH=1); see stealth_enabled.
//
// Built for the Windows GUI subsystem so a helper never opens a console
// window, whatever kind of process started the renderer.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>

#include "stealth.h"
#include "include/capi/cef_app_capi.h"
#include "include/capi/cef_render_process_handler_capi.h"
#include "include/cef_api_hash.h"

// ── Reference counting ────────────────────────────────────────

static void CEF_CALLBACK add_ref(cef_base_ref_counted_t* s) { (void)s; }
static int CEF_CALLBACK release_fn(cef_base_ref_counted_t* s) { (void)s; return 1; }
static int CEF_CALLBACK has_one(cef_base_ref_counted_t* s) { (void)s; return 1; }
static int CEF_CALLBACK has_any(cef_base_ref_counted_t* s) { (void)s; return 1; }

static void init_base(cef_base_ref_counted_t* base, size_t size) {
    base->size = size;
    base->add_ref = add_ref;
    base->release = release_fn;
    base->has_one_ref = has_one;
    base->has_at_least_one_ref = has_any;
}

// The C API hands the callee one reference per object argument.
static void cef_release(void* object) {
    cef_base_ref_counted_t* base = (cef_base_ref_counted_t*)object;
    if (base && base->release) base->release(base);
}

// ── Render process handler ────────────────────────────────────

// STEALTH_JS is injected only when WICK_CEF_STEALTH=1.
//
// Upstream's script has never actually run: both copies (stealth.h and the
// one inside helper.m) contain a JavaScript comment that turns the rest of
// the script into a syntax error (see stealth.h, section 14). Every result
// upstream reports was therefore obtained with plain Chromium plus the small
// POST_LOAD_FIXUP in the renderer. The Windows build parses the script
// correctly, but running a set of fingerprint overrides that nobody has
// tested would be a behaviour change, not a port — so it is opt-in.
static int stealth_enabled(void) {
    char value[8] = {0};
    DWORD len = GetEnvironmentVariableA("WICK_CEF_STEALTH", value, sizeof(value));
    return len == 1 && value[0] == '1';
}

static void CEF_CALLBACK on_context_created(
    cef_render_process_handler_t* self,
    cef_browser_t* browser,
    cef_frame_t* frame,
    struct _cef_v8_context_t* context) {
    (void)self;
    if (frame && stealth_enabled()) {
        cef_string_t code = {0};
        cef_string_t url = {0};
        cef_string_utf8_to_utf16(STEALTH_JS, strlen(STEALTH_JS), &code);
        cef_string_utf8_to_utf16("", 0, &url);
        frame->execute_java_script(frame, &code, &url, 0);
        cef_string_clear(&code);
        cef_string_clear(&url);
    }
    cef_release(context);
    cef_release(frame);
    cef_release(browser);
}

static cef_render_process_handler_t g_render_process_handler;

// ── App handler ───────────────────────────────────────────────

static cef_render_process_handler_t* CEF_CALLBACK get_render_process_handler(
    cef_app_t* self) {
    (void)self;
    return &g_render_process_handler;
}

static cef_app_t g_app;

// ── Crashes ───────────────────────────────────────────────────
//
// A helper that crashes ends; it never puts an "Application Error" box on the
// desktop. Chromium resets the settings that prevent the box while it runs,
// so a small thread keeps putting them back (see renderer_win.c).

static LONG WINAPI end_on_crash(EXCEPTION_POINTERS* info) {
    const UINT code = info && info->ExceptionRecord ? (UINT)info->ExceptionRecord->ExceptionCode : 1;
    TerminateProcess(GetCurrentProcess(), code);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void quiet_crashes(void) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(end_on_crash);
}

static DWORD WINAPI quiet_crashes_thread(LPVOID arg) {
    (void)arg;
    for (;;) {
        quiet_crashes();
        Sleep(250);
    }
}

static void keep_crashes_quiet(void) {
    quiet_crashes();
    HANDLE thread = CreateThread(NULL, 0, quiet_crashes_thread, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
}

// ── Main ──────────────────────────────────────────────────────

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE previous,
                      LPWSTR command_line, int show) {
    (void)previous; (void)command_line; (void)show;
    keep_crashes_quiet();
    cef_api_hash(CEF_API_VERSION, 0);

    init_base(&g_render_process_handler.base,
              sizeof(cef_render_process_handler_t));
    g_render_process_handler.on_context_created = on_context_created;

    init_base(&g_app.base, sizeof(cef_app_t));
    g_app.get_render_process_handler = get_render_process_handler;

    cef_main_args_t main_args = {0};
    main_args.instance = instance;
    int exit_code = cef_execute_process(&main_args, &g_app, NULL);
    // -1 means "not a subprocess": the helper was started by hand.
    return exit_code >= 0 ? exit_code : 1;
}
