// wick-renderer (Windows): CEF offscreen renderer.
//
// Modes (identical to Wick's macOS and Linux renderers, rust/cef/renderer.m
// and rust/cef/renderer_linux.c in github.com/wickproject/wick):
//   One-shot: wick-renderer.exe <url> [--cookie name=value]
//             Renders the page and writes the rendered HTML to stdout.
//   Daemon:   wick-renderer.exe            (no URL argument)
//             Reads "URL\n" or "URL\tSELECTOR\n" lines from stdin and answers
//             each with "<byte count>\n<html bytes>" on stdout. An empty line
//             or EOF shuts the daemon down.
//
// Windows specifics, compared with the other two renderers:
//   * cef_main_args_t carries an HINSTANCE, not argv, so Chromium switches
//     are added in on_before_command_line_processing instead of argv.
//   * stdout is switched to binary mode: text mode would rewrite "\n" as
//     "\r\n" inside the HTML and break the length-prefixed protocol.
//   * stdin is read on its own thread. The main thread keeps pumping the CEF
//     message loop between requests, so timers, cookie flushes and helper IPC
//     keep running while the daemon is idle.
//   * A document is only dumped when it was committed for the current
//     request. A navigation that never commits (download, HTTP 204, abort)
//     yields an empty result instead of the previous request's page.
//   * References received from CEF are released (the C API hands the callee
//     one reference per object argument).
//
// Behaviour that differs from the other two renderers, each explained where
// it is implemented:
//   * Every request is rendered in a browser of its own, closed as soon as
//     the answer has been written (create_browser, replace_browser).
//   * A page cannot reach outside the renderer: no downloads, windows,
//     dialogs, external programs, permissions, sound or printing ("Keeping
//     pages away from the desktop"). Only http and https URLs are rendered.
//   * Challenge widgets are clicked only with WICK_CEF_CLICK=1 (CLICK_DEFAULT),
//     the fingerprint script runs only with WICK_CEF_STEALTH=1.
//   * It drops administrator rights (run_without_elevation), ends quietly if
//     it crashes ("Crashes") and cannot keep wick waiting for ever
//     (stdin_thread, the watchdogs).
//
// Build: see build-windows.ps1 (MSVC, links against libcef.dll's import lib).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stealth.h"
#include "include/capi/cef_app_capi.h"
#include "include/capi/cef_browser_capi.h"
#include "include/capi/cef_browser_process_handler_capi.h"
#include "include/capi/cef_client_capi.h"
#include "include/capi/cef_command_line_capi.h"
#include "include/capi/cef_dialog_handler_capi.h"
#include "include/capi/cef_display_handler_capi.h"
#include "include/capi/cef_download_handler_capi.h"
#include "include/capi/cef_frame_capi.h"
#include "include/capi/cef_jsdialog_handler_capi.h"
#include "include/capi/cef_life_span_handler_capi.h"
#include "include/capi/cef_load_handler_capi.h"
#include "include/capi/cef_permission_handler_capi.h"
#include "include/capi/cef_render_handler_capi.h"
#include "include/capi/cef_render_process_handler_capi.h"
#include "include/capi/cef_request_context_capi.h"
#include "include/capi/cef_request_handler_capi.h"
#include "include/capi/cef_resource_request_handler_capi.h"
#include "include/capi/cef_string_visitor_capi.h"
#include "include/capi/cef_values_capi.h"
#include "include/cef_api_hash.h"
#include "include/cef_version.h"

// Protocol version marker — see Wick's rust/cef/renderer.m for usage and
// version history.
// Printed by --version, which also keeps the string in the linked binary so
// an installer can check an existing build the same way the macOS and Linux
// installers grep theirs.
static const char WICK_RENDERER_PROTOCOL[] = "WICK_RENDERER_PROTOCOL=v3-cf-aware";

#define VIEW_WIDTH 1920
#define VIEW_HEIGHT 1080
// Message-loop cadence while a render is running. The browser side of
// Chromium only gets to work when this loop pumps it, so the pause between
// pumps is added to every exchange with the page's process. One of those
// exchanges tells a new page its window size: until it arrives, page scripts
// read window.outerWidth and outerHeight as 0 — a classic sign of a headless
// browser. With a 15 ms pause that lasted 20 to 50 ms into each page.
#define TICK_MS 2
#define IDLE_TICK_MS 30     // message-loop cadence while waiting for a request
#define REQUEST_LINE_MAX 65536  // longest request line ("URL\tSELECTOR") accepted
// Clicking challenge widgets is off unless WICK_CEF_CLICK=1. The other
// renderers always click. The click is a real input event, so it also hands
// the page a user gesture — what browsers require before popups, file
// pickers, device choosers, sound and clipboard writes. The handlers below
// refuse what can be refused, but a gesture is still more than a page should
// get from a renderer nobody is watching. In tests against real sites the
// click cleared no challenge: Expedia and Hotels.com were clicked and kept
// their bot check, and on Cloudflare's challenge the click did not fire.
#define CLICK_DEFAULT 0

// ── State ─────────────────────────────────────────────────────

static cef_browser_t* g_browser = NULL;
static int g_browser_id = 0;     // identifier of g_browser, to tell it from any other
static int g_browser_closed = 0;
static int g_click_enabled = 0;  // WICK_CEF_CLICK=1: click challenge widgets
static int g_load_done = 0;      // browser reported isLoading == false
static int g_committed = 0;      // a main-frame document committed for this request
static int g_source_done = 0;    // the DOM dump for this request arrived
static int g_failed = 0;         // the main frame hit a final load error
static int g_nav_count = 0;      // loading-state transitions, for challenge reloads
static int g_click_x = -1;
static int g_click_y = -1;
static int g_clicked = 0;
static char* g_html = NULL;
static size_t g_html_len = 0;
static char g_cookie[2048] = {0};   // optional cookie to inject (one-shot --cookie)
static char g_selector[256] = {0};  // optional CSS selector gate for this request
static char g_proxy[512] = {0};     // WICK_PROXY, forwarded as --proxy-server

// Random per-process markers so page scripts cannot predict the console
// prefixes used to carry the DOM dump and the challenge coordinates.
static char g_marker[32];
static char g_click_prefix[48];
// Marker + request sequence. A poller left behind by an earlier request can
// still log after the next request started; its stale token is ignored.
static char g_token[48];
static unsigned g_request_seq = 0;

// Daemon request hand-off between the stdin thread and the main thread.
static HANDLE g_req_ready = NULL;
static HANDLE g_req_done = NULL;
static char* g_req_line = NULL;        // owned by the stdin thread, valid until g_req_done
static int g_req_too_long = 0;         // the line exceeded REQUEST_LINE_MAX
static volatile LONG g_stdin_eof = 0;

static void init_markers(void) {
    unsigned char buf[24];
    if (BCryptGenRandom(NULL, buf, sizeof(buf), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) {
        for (int i = 0; i < 12; i++)
            snprintf(g_marker + i * 2, 3, "%02x", buf[i]);
        for (int i = 0; i < 12; i++)
            snprintf(g_click_prefix + i * 2, 3, "%02x", buf[12 + i]);
    } else {
        snprintf(g_marker, sizeof(g_marker), "_%lu_%llu_",
                 (unsigned long)GetCurrentProcessId(), GetTickCount64());
        snprintf(g_click_prefix, sizeof(g_click_prefix), "_c%lu_",
                 (unsigned long)GetCurrentProcessId());
    }
}

// ── Reference counting ────────────────────────────────────────

// Handlers are static, so their own counting is a no-op.
static void CEF_CALLBACK add_ref(cef_base_ref_counted_t* self) { (void)self; }
static int CEF_CALLBACK release_fn(cef_base_ref_counted_t* self) { (void)self; return 1; }
static int CEF_CALLBACK has_one(cef_base_ref_counted_t* self) { (void)self; return 1; }
static int CEF_CALLBACK has_any(cef_base_ref_counted_t* self) { (void)self; return 1; }

static void init_base(cef_base_ref_counted_t* base, size_t size) {
    base->size = size;
    base->add_ref = add_ref;
    base->release = release_fn;
    base->has_one_ref = has_one;
    base->has_at_least_one_ref = has_any;
}

// Drop one reference to a CEF-owned object. Every ref-counted CEF struct
// starts with cef_base_ref_counted_t, so the cast is the documented layout.
static void cef_release(void* object) {
    cef_base_ref_counted_t* base = (cef_base_ref_counted_t*)object;
    if (base && base->release) base->release(base);
}

// ── Small helpers ─────────────────────────────────────────────

static void run_js(cef_frame_t* frame, const char* js) {
    cef_string_t code = {0};
    cef_string_utf8_to_utf16(js, strlen(js), &code);
    frame->execute_java_script(frame, &code, NULL, 0);
    cef_string_clear(&code);
}

// Run JS in the browser's main frame. Returns 0 when there is no frame.
static int run_js_main(const char* js) {
    if (!g_browser) return 0;
    cef_frame_t* frame = g_browser->get_main_frame(g_browser);
    if (!frame) return 0;
    run_js(frame, js);
    cef_release(frame);
    return 1;
}

static void pump(DWORD sleep_ms) {
    cef_do_message_loop_work();
    if (sleep_ms) Sleep(sleep_ms);
}

// http:// or https:// — the only URLs this renderer loads as pages.
static int is_web_url(const char* url) {
    return _strnicmp(url, "http://", 7) == 0 || _strnicmp(url, "https://", 8) == 0;
}

static int is_web_url_cef(const cef_string_t* url) {
    if (!url || !url->str) return 0;
    char head[10] = {0};
    for (size_t i = 0; i < 9 && i < url->length; i++) {
        head[i] = url->str[i] < 128 ? (char)url->str[i] : '?';
    }
    return is_web_url(head);
}

static void set_path(cef_string_t* target, const wchar_t* path) {
    cef_string_wide_to_utf16(path, wcslen(path), target);
}

// ── Display handler — captures console.log output ─────────────

static int CEF_CALLBACK on_console_message(
    cef_display_handler_t* self, cef_browser_t* browser,
    cef_log_severity_t level, const cef_string_t* message,
    const cef_string_t* source, int line) {
    (void)self; (void)level; (void)source; (void)line;
    cef_release(browser);
    if (!message || !message->str) return 0;

    cef_string_utf8_t utf8 = {0};
    cef_string_utf16_to_utf8(message->str, message->length, &utf8);
    if (!utf8.str) return 0;

    int handled = 0;
    size_t click_len = strlen(g_click_prefix);
    size_t token_len = strlen(g_token);

    if (utf8.length > click_len && strncmp(utf8.str, g_click_prefix, click_len) == 0) {
        // Challenge iframe coordinates ("x,y") from the detection script.
        int x = 0, y = 0;
        if (sscanf(utf8.str + click_len, "%d,%d", &x, &y) == 2) {
            g_click_x = x;
            g_click_y = y;
        }
        handled = 1;
    } else if (token_len > 0 && utf8.length >= token_len &&
               strncmp(utf8.str, g_token, token_len) == 0) {
        // Rendered DOM for the current request.
        if (!g_source_done) {
            size_t html_len = utf8.length - token_len;
            char* html = (char*)malloc(html_len ? html_len : 1);
            if (html) {
                memcpy(html, utf8.str + token_len, html_len);
                free(g_html);
                g_html = html;
                g_html_len = html_len;
            }
            g_source_done = 1;
        }
        handled = 1;
    } else if (utf8.length >= strlen(g_marker) &&
               strncmp(utf8.str, g_marker, strlen(g_marker)) == 0) {
        // A dump from an earlier request's page: drop it.
        handled = 1;
    }

    cef_string_utf8_clear(&utf8);
    return handled;  // 1 keeps our own messages out of the CEF log
}

static cef_display_handler_t g_display_handler;

// ── Load handler ──────────────────────────────────────────────

static void CEF_CALLBACK on_loading_state_change(
    cef_load_handler_t* self, cef_browser_t* browser,
    int isLoading, int canGoBack, int canGoForward) {
    (void)self; (void)canGoBack; (void)canGoForward;
    cef_release(browser);
    if (isLoading) {
        // New navigation (first load, or an AWS WAF / DataDome style reload
        // after a challenge). The content poller must wait for the new page.
        g_nav_count++;
        g_load_done = 0;
        g_source_done = 0;
    } else {
        g_load_done = 1;
    }
}

static void CEF_CALLBACK on_load_start(cef_load_handler_t* self,
    cef_browser_t* browser, cef_frame_t* frame, cef_transition_type_t tt) {
    (void)self; (void)tt;
    // Only a web page counts as the request's document — not the about:blank
    // a browser starts on.
    int web_document = 0;
    if (frame && frame->is_main(frame)) {
        cef_string_userfree_t url = frame->get_url(frame);
        if (url) {
            web_document = is_web_url_cef(url);
            cef_string_userfree_free(url);
        }
    }
    if (web_document) {
        g_committed = 1;
        // A new document gets a new view of the page, and until that view
        // has been told the window's size, scripts read window.outerWidth
        // and outerHeight as 0. Chromium sends the size on its own a little
        // later; ask for it now.
        cef_browser_host_t* host = browser->get_host(browser);
        if (host) {
            host->notify_screen_info_changed(host);
            cef_release(host);
        }
        // STEALTH_JS is not run from here (the other renderers do): when it
        // is enabled, wick-helper injects it once at V8 context creation. A
        // second run would throw on the non-configurable properties and leave
        // the injection scaffolding (window.__markNative) visible to the page.
        if (g_cookie[0]) {
            // Cookie from a solved CAPTCHA (one-shot --cookie).
            char cookie_js[2300];
            snprintf(cookie_js, sizeof(cookie_js),
                     "document.cookie='%s; path=/; secure; samesite=lax';", g_cookie);
            run_js(frame, cookie_js);
        }
    }
    cef_release(frame);
    cef_release(browser);
}

// Minimal fixup for properties CEF resets after context creation.
// Do NOT re-run full STEALTH_JS — configurable:false prevents redefining
// properties, and the cascading errors kill everything.
static const char* POST_LOAD_FIXUP =
    "(function(){"
    // chrome.runtime: CEF creates chrome without runtime
    "  var rt={"
    "    OnInstalledReason:{CHROME_UPDATE:'chrome_update',INSTALL:'install',SHARED_MODULE_UPDATE:'shared_module_update',UPDATE:'update'},"
    "    OnRestartRequiredReason:{APP_UPDATE:'app_update',OS_UPDATE:'os_update',PERIODIC:'periodic'},"
    "    PlatformArch:{ARM:'arm',ARM64:'arm64',MIPS:'mips',MIPS64:'mips64',X86_32:'x86-32',X86_64:'x86-64'},"
    "    PlatformOs:{ANDROID:'android',CROS:'cros',LINUX:'linux',MAC:'mac',OPENBSD:'openbsd',WIN:'win'},"
    "    RequestUpdateCheckStatus:{NO_UPDATE:'no_update',THROTTLED:'throttled',UPDATE_AVAILABLE:'update_available'},"
    "    connect:function(){return{onDisconnect:{addListener:function(){}},onMessage:{addListener:function(){}},postMessage:function(){}};},"
    "    sendMessage:function(){},"
    "    id:undefined"
    "  };"
    "  function fix(){"
    "    if(window.chrome&&!window.chrome.runtime)window.chrome.runtime=rt;"
    "    if(window.outerHeight===window.innerHeight){"
    "      try{Object.defineProperty(window,'outerHeight',{get:function(){return window.innerHeight+85;},configurable:true});}catch(e){}"
    "    }"
    "  }"
    "  fix();"
    "  setTimeout(fix,0);"
    "  setTimeout(fix,1);"
    "  setTimeout(fix,10);"
    "  setTimeout(fix,50);"
    "})();";

static void CEF_CALLBACK on_load_end(cef_load_handler_t* self,
    cef_browser_t* browser, cef_frame_t* frame, int httpStatusCode) {
    (void)self; (void)httpStatusCode;
    if (frame && frame->is_main(frame)) {
        run_js(frame, POST_LOAD_FIXUP);
    }
    cef_release(frame);
    cef_release(browser);
}

static void CEF_CALLBACK on_load_error(cef_load_handler_t* self,
    cef_browser_t* browser, cef_frame_t* frame,
    cef_errorcode_t errorCode, const cef_string_t* errorText,
    const cef_string_t* failedUrl) {
    (void)self; (void)errorText;
    if (frame && frame->is_main(frame)) {
        if (g_committed && !is_web_url_cef(failedUrl)) {
            // The page (requests themselves are always http or https) tried
            // to go somewhere that is not a web page — an "open in the app"
            // link such as slack:// or mailto:. Nothing was loaded and the
            // page is still there, so this is not a failure of the request.
            fprintf(stderr, "wick-renderer: ignored a navigation to a non-web URL (error %d)\n",
                    (int)errorCode);
        } else {
            fprintf(stderr, "wick-renderer: main frame load error %d\n", (int)errorCode);
            // ERR_ABORTED means a navigation was superseded (script or meta
            // redirect, challenge reload) or converted into a download. The
            // render loop decides: a follow-up document commits and is
            // dumped, or nothing commits and the request ends empty. Any
            // other error is final — fail fast so the caller can retry or
            // fall back, instead of reporting Chromium's error page as the
            // site's content.
            if (errorCode != ERR_ABORTED) {
                g_failed = 1;
            }
        }
    }
    cef_release(frame);
    cef_release(browser);
}

static cef_load_handler_t g_load_handler;

// ── Render handler (required for OSR, minimal no-op) ──────────

static void CEF_CALLBACK get_view_rect(cef_render_handler_t* self,
    cef_browser_t* browser, cef_rect_t* rect) {
    (void)self;
    cef_release(browser);
    rect->x = 0; rect->y = 0; rect->width = VIEW_WIDTH; rect->height = VIEW_HEIGHT;
}

static void CEF_CALLBACK on_paint(cef_render_handler_t* self,
    cef_browser_t* browser, cef_paint_element_type_t type,
    size_t n, cef_rect_t const* d, const void* buf, int w, int h) {
    (void)self; (void)type; (void)n; (void)d; (void)buf; (void)w; (void)h;
    cef_release(browser);
}

static int CEF_CALLBACK get_screen_info(cef_render_handler_t* self,
    cef_browser_t* browser, cef_screen_info_t* info) {
    (void)self; (void)info;
    cef_release(browser);
    return 0;
}

static cef_render_handler_t g_render_handler;

// ── Life span handler ─────────────────────────────────────────

static void CEF_CALLBACK on_after_created(cef_life_span_handler_t* self,
    cef_browser_t* browser) {
    (void)self;
    if (!g_browser) {
        g_browser = browser;  // keep the reference; released in on_before_close
        g_browser_id = browser->get_identifier(browser);
        return;
    }
    // There is exactly one browser. Popups are refused in on_before_popup; if
    // anything else ever gets created, it is closed again at once and never
    // takes the place of the browser that requests are rendered in.
    cef_browser_host_t* host = browser->get_host(browser);
    if (host) {
        host->close_browser(host, 1);
        cef_release(host);
    }
    cef_release(browser);
}

static int CEF_CALLBACK do_close(cef_life_span_handler_t* self,
    cef_browser_t* browser) {
    (void)self;
    cef_release(browser);
    return 0;
}

static void CEF_CALLBACK on_before_close(cef_life_span_handler_t* self,
    cef_browser_t* browser) {
    (void)self;
    const int id = browser->get_identifier(browser);
    cef_release(browser);
    if (g_browser && id == g_browser_id) {
        cef_release(g_browser);
        g_browser = NULL;
        g_browser_closed = 1;
        g_failed = 1;
    }
}

// ── Keeping pages away from the desktop ───────────────────────
//
// A rendered page gets no way to reach beyond the renderer: it cannot open
// windows or dialogs, save files, start other programs, or be given device,
// clipboard or local-network permissions. Chromium's own defaults for these
// assume a person in front of a visible window (a "Save as" bubble, a
// permission prompt, a print dialog); here nobody is, so each one is refused.

// window.open, target="_blank", and anything else that wants a new window.
static int CEF_CALLBACK on_before_popup(
    cef_life_span_handler_t* self, cef_browser_t* browser, cef_frame_t* frame,
    int popup_id, const cef_string_t* target_url, const cef_string_t* target_frame_name,
    cef_window_open_disposition_t target_disposition, int user_gesture,
    const cef_popup_features_t* popupFeatures, cef_window_info_t* windowInfo,
    cef_client_t** client, cef_browser_settings_t* settings,
    cef_dictionary_value_t** extra_info, int* no_javascript_access) {
    (void)self; (void)popup_id; (void)target_url; (void)target_frame_name;
    (void)target_disposition; (void)user_gesture; (void)popupFeatures; (void)windowInfo;
    (void)client; (void)settings; (void)extra_info; (void)no_javascript_access;
    cef_release(frame);
    cef_release(browser);
    return 1;  // cancel
}

static cef_life_span_handler_t g_life_span_handler;

// Downloads. Without a handler Chromium saved every attachment a page served
// into the user's Downloads folder.
static int CEF_CALLBACK can_download(cef_download_handler_t* self, cef_browser_t* browser,
                                     const cef_string_t* url, const cef_string_t* request_method) {
    (void)self; (void)url; (void)request_method;
    cef_release(browser);
    fprintf(stderr, "wick-renderer: refused a download\n");
    return 0;
}

// Not reached while can_download refuses; kept so that a download can never
// fall through to Chromium's default handling.
static int CEF_CALLBACK on_before_download(
    cef_download_handler_t* self, cef_browser_t* browser, cef_download_item_t* download_item,
    const cef_string_t* suggested_name, cef_before_download_callback_t* callback) {
    (void)self; (void)suggested_name;
    cef_release(callback);  // never continued, so nothing is written
    cef_release(download_item);
    cef_release(browser);
    return 1;
}

static void CEF_CALLBACK on_download_updated(
    cef_download_handler_t* self, cef_browser_t* browser, cef_download_item_t* download_item,
    cef_download_item_callback_t* callback) {
    (void)self;
    if (callback) {
        callback->cancel(callback);
        cef_release(callback);
    }
    cef_release(download_item);
    cef_release(browser);
}

static cef_download_handler_t g_download_handler;

// alert / confirm / prompt: answered at once (confirm and prompt as
// "cancel"), nothing shown.
static int CEF_CALLBACK on_jsdialog(
    cef_jsdialog_handler_t* self, cef_browser_t* browser, const cef_string_t* origin_url,
    cef_jsdialog_type_t dialog_type, const cef_string_t* message_text,
    const cef_string_t* default_prompt_text, cef_jsdialog_callback_t* callback,
    int* suppress_message) {
    (void)self; (void)origin_url; (void)dialog_type; (void)message_text; (void)default_prompt_text;
    cef_release(callback);
    cef_release(browser);
    if (suppress_message) *suppress_message = 1;
    return 0;
}

// "Leave site?": always leave, so the next request can load.
static int CEF_CALLBACK on_before_unload_dialog(
    cef_jsdialog_handler_t* self, cef_browser_t* browser, const cef_string_t* message_text,
    int is_reload, cef_jsdialog_callback_t* callback) {
    (void)self; (void)message_text; (void)is_reload;
    if (callback) {
        callback->cont(callback, 1, NULL);
        cef_release(callback);
    }
    cef_release(browser);
    return 1;
}

static cef_jsdialog_handler_t g_jsdialog_handler;

// File pickers (<input type=file>, showOpenFilePicker, ...).
static int CEF_CALLBACK on_file_dialog(
    cef_dialog_handler_t* self, cef_browser_t* browser, cef_file_dialog_mode_t mode,
    const cef_string_t* title, const cef_string_t* default_file_path,
    cef_string_list_t accept_filters, cef_string_list_t accept_extensions,
    cef_string_list_t accept_descriptions, cef_file_dialog_callback_t* callback) {
    (void)self; (void)mode; (void)title; (void)default_file_path;
    (void)accept_filters; (void)accept_extensions; (void)accept_descriptions;
    if (callback) {
        callback->cancel(callback);
        cef_release(callback);
    }
    cef_release(browser);
    return 1;
}

static cef_dialog_handler_t g_dialog_handler;

// Keyboard focus. CEF focuses the browser whenever a URL is loaded through
// the API; refused, so that pages stay unfocused (see unfocus_browser).
static int CEF_CALLBACK on_set_focus(cef_focus_handler_t* self, cef_browser_t* browser,
                                     cef_focus_source_t source) {
    (void)self; (void)source;
    cef_release(browser);
    return 1;  // cancel
}

static cef_focus_handler_t g_focus_handler;

// Camera, microphone and screen capture.
static int CEF_CALLBACK on_request_media_access_permission(
    cef_permission_handler_t* self, cef_browser_t* browser, cef_frame_t* frame,
    const cef_string_t* requesting_origin, uint32_t requested_permissions,
    cef_media_access_callback_t* callback) {
    (void)self; (void)requesting_origin; (void)requested_permissions;
    if (callback) {
        callback->cancel(callback);
        cef_release(callback);
    }
    cef_release(frame);
    cef_release(browser);
    return 1;
}

// Everything Chromium would show a permission prompt for: location,
// notifications, clipboard, MIDI, local fonts, local network access, ...
static int CEF_CALLBACK on_show_permission_prompt(
    cef_permission_handler_t* self, cef_browser_t* browser, uint64_t prompt_id,
    const cef_string_t* requesting_origin, uint32_t requested_permissions,
    cef_permission_prompt_callback_t* callback) {
    (void)self; (void)prompt_id; (void)requesting_origin; (void)requested_permissions;
    if (callback) {
        callback->cont(callback, CEF_PERMISSION_RESULT_DENY);
        cef_release(callback);
    }
    cef_release(browser);
    return 1;
}

static cef_permission_handler_t g_permission_handler;

// Links to other programs (mailto:, ms-settings:, steam://, ...): never
// handed to Windows. Called on the IO thread.
static void CEF_CALLBACK on_protocol_execution(
    cef_resource_request_handler_t* self, cef_browser_t* browser, cef_frame_t* frame,
    cef_request_t* request, int* allow_os_execution) {
    (void)self;
    if (allow_os_execution) *allow_os_execution = 0;
    cef_release(request);
    cef_release(frame);
    cef_release(browser);
    fprintf(stderr, "wick-renderer: refused a link to an external application\n");
}

static cef_resource_request_handler_t g_resource_request_handler;

// Called on the IO thread for every request. Web requests keep Chromium's
// default handling (return NULL); only a request for something that is not a
// web URL gets the handler above.
static cef_resource_request_handler_t* CEF_CALLBACK get_resource_request_handler(
    cef_request_handler_t* self, cef_browser_t* browser, cef_frame_t* frame,
    cef_request_t* request, int is_navigation, int is_download,
    const cef_string_t* request_initiator, int* disable_default_handling) {
    (void)self; (void)is_navigation; (void)is_download; (void)request_initiator;
    (void)disable_default_handling;
    int web = 1;
    if (request) {
        cef_string_userfree_t url = request->get_url(request);
        if (url) {
            web = is_web_url_cef(url);
            cef_string_userfree_free(url);
        }
    }
    cef_release(request);
    cef_release(frame);
    cef_release(browser);
    return web ? NULL : &g_resource_request_handler;
}

// Every navigation, of the page or of a frame in it, before it starts. A
// navigation to something that is not web content is dropped here, which
// leaves the page as it is — what a desktop browser does when a page tries an
// "open in the app" link (slack://, mailto:, ...). Left to fail later,
// Chromium would replace the page with its own error page.
static int CEF_CALLBACK on_before_browse(
    cef_request_handler_t* self, cef_browser_t* browser, cef_frame_t* frame,
    cef_request_t* request, int user_gesture, int is_redirect) {
    (void)self; (void)user_gesture; (void)is_redirect;
    int allowed = 1;
    if (request) {
        cef_string_userfree_t url = request->get_url(request);
        if (url) {
            // Web pages, plus the schemes Chromium itself uses for frames and
            // generated documents (about:blank, about:srcdoc, data:, blob:).
            static const char* const kAllowed[] = {"http:", "https:", "about:", "data:", "blob:"};
            char head[10] = {0};
            for (size_t i = 0; i < 9 && i < url->length; i++) {
                head[i] = url->str[i] < 128 ? (char)url->str[i] : '?';
            }
            allowed = 0;
            for (size_t i = 0; i < sizeof(kAllowed) / sizeof(kAllowed[0]); i++) {
                if (_strnicmp(head, kAllowed[i], strlen(kAllowed[i])) == 0) allowed = 1;
            }
            cef_string_userfree_free(url);
        }
    }
    cef_release(request);
    cef_release(frame);
    cef_release(browser);
    if (!allowed) {
        fprintf(stderr, "wick-renderer: dropped a navigation to a non-web URL\n");
    }
    return allowed ? 0 : 1;
}

// Ctrl/middle-click style "open in a new tab" navigations: there are no tabs.
static int CEF_CALLBACK on_open_urlfrom_tab(
    cef_request_handler_t* self, cef_browser_t* browser, cef_frame_t* frame,
    const cef_string_t* target_url, cef_window_open_disposition_t target_disposition,
    int user_gesture) {
    (void)self; (void)target_url; (void)target_disposition; (void)user_gesture;
    cef_release(frame);
    cef_release(browser);
    return 1;  // cancel
}

// HTTP authentication: declined, so the server's own 401 page becomes the
// document and is returned like any other page. Only reached because of the
// disable-chrome-login-prompt switch (see on_before_command_line_processing).
static int CEF_CALLBACK get_auth_credentials(
    cef_request_handler_t* self, cef_browser_t* browser, const cef_string_t* origin_url,
    int isProxy, const cef_string_t* host, int port, const cef_string_t* realm,
    const cef_string_t* scheme, cef_auth_callback_t* callback) {
    (void)self; (void)origin_url; (void)isProxy; (void)host; (void)port; (void)realm; (void)scheme;
    if (callback) {
        callback->cancel(callback);
        cef_release(callback);
    }
    cef_release(browser);
    return 1;
}

// A server asks for a client certificate and the user has matching ones
// installed: Chromium's default is a certificate-selection dialog. Continue
// without a certificate instead; the user's certificates are never offered.
static int CEF_CALLBACK on_select_client_certificate(
    cef_request_handler_t* self, cef_browser_t* browser, int isProxy,
    const cef_string_t* host, int port, size_t certificatesCount,
    cef_x509_certificate_t* const* certificates,
    cef_select_client_certificate_callback_t* callback) {
    (void)self; (void)isProxy; (void)host; (void)port;
    for (size_t i = 0; i < certificatesCount; i++) {
        if (certificates && certificates[i]) cef_release(certificates[i]);
    }
    if (callback) {
        callback->select(callback, NULL);
        cef_release(callback);
    }
    cef_release(browser);
    return 1;
}

// The process that runs a page's scripts died (crash, out of memory, killed).
// End the request now instead of waiting out its time limit; Chromium starts
// a new process for the next navigation.
static void CEF_CALLBACK on_render_process_terminated(
    cef_request_handler_t* self, cef_browser_t* browser, cef_termination_status_t status,
    int error_code, const cef_string_t* error_string) {
    (void)self; (void)error_string;
    cef_release(browser);
    fprintf(stderr, "wick-renderer: page process ended (status %d, code %d)\n",
            (int)status, error_code);
    g_failed = 1;
}

// Chromium noticed that a page's process has stopped answering. Its default
// is to wait (or, with a window, to ask the user). End the process instead.
static int CEF_CALLBACK on_render_process_unresponsive(
    cef_request_handler_t* self, cef_browser_t* browser,
    cef_unresponsive_process_callback_t* callback) {
    (void)self;
    fprintf(stderr, "wick-renderer: page process stopped responding; ending it\n");
    if (callback) {
        callback->terminate(callback);
        cef_release(callback);
    }
    cef_release(browser);
    return 1;
}

static cef_request_handler_t g_request_handler;

// ── Client ────────────────────────────────────────────────────

static cef_life_span_handler_t* CEF_CALLBACK get_life_span_handler(cef_client_t* s) {
    (void)s; return &g_life_span_handler;
}
static cef_load_handler_t* CEF_CALLBACK get_load_handler(cef_client_t* s) {
    (void)s; return &g_load_handler;
}
static cef_render_handler_t* CEF_CALLBACK get_render_handler(cef_client_t* s) {
    (void)s; return &g_render_handler;
}
static cef_display_handler_t* CEF_CALLBACK get_display_handler(cef_client_t* s) {
    (void)s; return &g_display_handler;
}
static cef_download_handler_t* CEF_CALLBACK get_download_handler(cef_client_t* s) {
    (void)s; return &g_download_handler;
}
static cef_jsdialog_handler_t* CEF_CALLBACK get_jsdialog_handler(cef_client_t* s) {
    (void)s; return &g_jsdialog_handler;
}
static cef_dialog_handler_t* CEF_CALLBACK get_dialog_handler(cef_client_t* s) {
    (void)s; return &g_dialog_handler;
}
static cef_focus_handler_t* CEF_CALLBACK get_focus_handler(cef_client_t* s) {
    (void)s; return &g_focus_handler;
}
static cef_permission_handler_t* CEF_CALLBACK get_permission_handler(cef_client_t* s) {
    (void)s; return &g_permission_handler;
}
static cef_request_handler_t* CEF_CALLBACK get_request_handler(cef_client_t* s) {
    (void)s; return &g_request_handler;
}

static cef_client_t g_client;

// ── App ───────────────────────────────────────────────────────

static void append_switch(cef_command_line_t* command_line, const char* name) {
    cef_string_t cef_name = {0};
    cef_string_utf8_to_utf16(name, strlen(name), &cef_name);
    command_line->append_switch(command_line, &cef_name);
    cef_string_clear(&cef_name);
}

static void append_switch_value(cef_command_line_t* command_line,
                                const char* name, const char* value) {
    cef_string_t cef_name = {0};
    cef_string_t cef_value = {0};
    cef_string_utf8_to_utf16(name, strlen(name), &cef_name);
    cef_string_utf8_to_utf16(value, strlen(value), &cef_value);
    command_line->append_switch_with_value(command_line, &cef_name, &cef_value);
    cef_string_clear(&cef_name);
    cef_string_clear(&cef_value);
}

// Chromium features that only exist to call Google from the browser process:
// the network-time query (http://clients2.google.com/time/...), the AI Mode
// eligibility probe (https://www.google.com/async/folae) and the preconnect
// to the default search engine. Found with --log-net-log; none of them is
// visible to pages. What remains at startup is one accounts.google.com
// /ListAccounts request (no account, no cookies). Chromium has no switch for
// it short of redefining the sign-in origin, which would also change how real
// Google pages are treated — so it is left alone.
//
// MediaRouter is Chromium's Cast support. A few seconds after start it looks
// for Cast devices on the local network: the browser process listens on UDP
// port 5353 (mDNS) for about a minute. That is not a web request, so it does
// not show in a net log; it shows as a UDP socket of the process, and to the
// user as a firewall prompt about an inbound connection from another device.
static const char DISABLED_FEATURES[] =
    "NetworkTimeServiceQuerying,"
    "AimServerEligibilityEnabled,"
    "AimServerRequestOnStartupEnabled,"
    "AimServerRequestOnIdentityChangeEnabled,"
    "PreconnectToSearch,"
    "MediaRouter";

// Current value of a switch, or "" when it is absent or does not fit.
static void read_switch_value(cef_command_line_t* command_line, const char* name,
                              char* out, size_t cap) {
    out[0] = '\0';
    cef_string_t cef_name = {0};
    cef_string_utf8_to_utf16(name, strlen(name), &cef_name);
    cef_string_userfree_t value = command_line->get_switch_value(command_line, &cef_name);
    cef_string_clear(&cef_name);
    if (!value) return;
    cef_string_utf8_t utf8 = {0};
    cef_string_utf16_to_utf8(value->str, value->length, &utf8);
    if (utf8.str && utf8.length < cap) {
        memcpy(out, utf8.str, utf8.length);
        out[utf8.length] = '\0';
    }
    cef_string_utf8_clear(&utf8);
    cef_string_userfree_free(value);
}

// Append |more| to a comma-separated list, if it fits.
static void join_list(char* list, size_t cap, const char* more) {
    if (!*more) return;
    const size_t have = strlen(list);
    if (have + strlen(more) + 2 > cap) return;
    if (have > 0) strcat(list, ",");
    strcat(list, more);
}

// WICK_CEF_FLAGS: extra Chromium switches for troubleshooting, separated by
// spaces, e.g. "--disable-gpu --lang=es". Values cannot contain spaces. A
// --disable-features value is merged into |features| rather than replacing
// the built-in list (Chromium keeps only the last occurrence of a switch).
static void append_extra_switches(cef_command_line_t* command_line,
                                  char* features, size_t features_cap) {
    char flags[2048] = {0};
    DWORD len = GetEnvironmentVariableA("WICK_CEF_FLAGS", flags, sizeof(flags));
    if (len == 0 || len >= sizeof(flags)) return;
    char* context = NULL;
    for (char* token = strtok_s(flags, " \t", &context); token;
         token = strtok_s(NULL, " \t", &context)) {
        while (*token == '-') token++;
        if (!*token) continue;
        char* value = strchr(token, '=');
        if (!value) {
            append_switch(command_line, token);
            continue;
        }
        *value++ = '\0';
        if (strcmp(token, "disable-features") == 0) {
            join_list(features, features_cap, value);
        } else {
            append_switch_value(command_line, token, value);
        }
    }
}

static void CEF_CALLBACK on_before_command_line_processing(
    cef_app_t* self, const cef_string_t* process_type,
    cef_command_line_t* command_line) {
    (void)self;
    if (!command_line) return;
    // An empty process type is the browser process; helpers get their
    // switches from Chromium.
    if (!process_type || process_type->length == 0) {
        // Same choice as macOS: keep the GPU process (ANGLE/Direct3D gives
        // real WebGL with the machine's real adapter string) but composite
        // in software, which is all an offscreen 1 fps view needs.
        append_switch(command_line, "disable-gpu-compositing");
        // Chromium relaunches itself through the shell when started
        // elevated, which would detach it from the stdin/stdout protocol.
        // Elevation is handled before CEF starts (see run_without_elevation).
        append_switch(command_line, "do-not-de-elevate");
        // A page renderer has no business talking to Google on its own:
        // no variations/component/Safe Browsing updates, sync or first-run.
        append_switch(command_line, "disable-background-networking");
        append_switch(command_line, "disable-component-update");
        append_switch(command_line, "disable-sync");
        append_switch(command_line, "disable-default-apps");
        append_switch(command_line, "no-first-run");
        // HTTP authentication: without this switch Chromium answers a 401
        // with its own "Sign in" window on the desktop, and the client's
        // get_auth_credentials (which declines) is never asked.
        append_switch(command_line, "disable-chrome-login-prompt");
        // Nothing a page plays should come out of the user's speakers.
        append_switch(command_line, "mute-audio");
        // Chromium's own error dialogs (a helper that fails to start, ...).
        append_switch(command_line, "noerrdialogs");
        // Chromium registers every profile with Google Cloud Messaging: a
        // device check-in at android.clients.google.com, then a standing
        // connection to mtalk.google.com:5228. Nothing here uses push
        // messages, and no switch turns the service off, so the check-in is
        // pointed at a closed loopback port; registration and the standing
        // connection only start after a successful check-in.
        append_switch_value(command_line, "gcm-checkin-url", "http://127.0.0.1:9/");
        if (g_proxy[0]) {
            append_switch_value(command_line, "proxy-server", g_proxy);
        }
        // One merged --disable-features: CEF's own list (it is already on the
        // command line, and a second switch would replace it, switching those
        // features back on), then the list above, then WICK_CEF_FLAGS.
        char features[2048];
        read_switch_value(command_line, "disable-features", features, sizeof(features));
        join_list(features, sizeof(features), DISABLED_FEATURES);
        append_extra_switches(command_line, features, sizeof(features));
        append_switch_value(command_line, "disable-features", features);
    }
    cef_release(command_line);
}

// Stealth injection for the case where this binary is its own subprocess
// (wick-helper.exe missing). Mirrors helper_win.c, including the
// WICK_CEF_STEALTH=1 opt-in explained there.
static void CEF_CALLBACK on_context_created(
    cef_render_process_handler_t* self, cef_browser_t* browser,
    cef_frame_t* frame, struct _cef_v8_context_t* context) {
    (void)self;
    char stealth[8] = {0};
    DWORD stealth_len = GetEnvironmentVariableA("WICK_CEF_STEALTH", stealth, sizeof(stealth));
    if (frame && stealth_len == 1 && stealth[0] == '1') {
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

static cef_render_process_handler_t* CEF_CALLBACK get_render_process_handler(
    cef_app_t* self) {
    (void)self;
    return &g_render_process_handler;
}

// CEF allows one process per root_cache_path. A second wick-renderer on the
// same profile exits early and forwards its launch to this one; the default
// reaction would open a Chrome-style window here. Swallow it instead.
static int CEF_CALLBACK on_already_running_app_relaunch(
    cef_browser_process_handler_t* self, cef_command_line_t* command_line,
    const cef_string_t* current_directory) {
    (void)self; (void)current_directory;
    cef_release(command_line);
    fprintf(stderr, "wick-renderer: ignored a second launch on the same profile\n");
    return 1;
}

static cef_browser_process_handler_t g_browser_process_handler;

static cef_browser_process_handler_t* CEF_CALLBACK get_browser_process_handler(
    cef_app_t* self) {
    (void)self;
    return &g_browser_process_handler;
}

static cef_app_t g_app;

// ── Paths ─────────────────────────────────────────────────────

// Directory containing this executable, without a trailing separator.
static int get_exe_dir(wchar_t* buf, DWORD cap) {
    DWORD len = GetModuleFileNameW(NULL, buf, cap);
    if (len == 0 || len >= cap) return -1;
    wchar_t* slash = wcsrchr(buf, L'\\');
    if (!slash) return -1;
    *slash = L'\0';
    return 0;
}

// "<home>\.wick": HOME when set (keeps tests and wrappers in control),
// otherwise USERPROFILE, otherwise the temp directory. Matches the Rust side.
static void get_wick_home(wchar_t* buf, size_t cap) {
    wchar_t home[MAX_PATH * 2];
    DWORD len = GetEnvironmentVariableW(L"HOME", home, (DWORD)(sizeof(home) / sizeof(home[0])));
    if (len == 0 || len >= sizeof(home) / sizeof(home[0])) {
        len = GetEnvironmentVariableW(L"USERPROFILE", home, (DWORD)(sizeof(home) / sizeof(home[0])));
    }
    if (len == 0 || len >= sizeof(home) / sizeof(home[0])) {
        len = GetTempPathW((DWORD)(sizeof(home) / sizeof(home[0])), home);
        if (len == 0 || len >= sizeof(home) / sizeof(home[0])) wcscpy(home, L"C:\\");
    }
    size_t n = wcslen(home);
    while (n > 0 && (home[n - 1] == L'\\' || home[n - 1] == L'/')) home[--n] = L'\0';
    _snwprintf(buf, cap, L"%ls\\.wick", home);
    buf[cap - 1] = L'\0';
}

// Create |path| and any missing parents. Existing directories are fine.
static void make_dirs(const wchar_t* path) {
    wchar_t partial[MAX_PATH * 2];
    const size_t cap = sizeof(partial) / sizeof(partial[0]);
    const size_t len = wcslen(path);
    if (len == 0 || len >= cap) return;
    for (size_t i = 0; i <= len; i++) {
        const wchar_t c = path[i];
        // Skip the root ("C:\", "\\server\share" prefixes fail harmlessly).
        if ((c == L'\\' || c == L'/' || c == L'\0') && i > 0 && path[i - 1] != L':') {
            partial[i] = L'\0';
            CreateDirectoryW(partial, NULL);
        }
        partial[i] = c;
    }
}

// ── Rendering ─────────────────────────────────────────────────

// Receives the page source from Chromium (cef_frame_t::get_source). Used only
// when the page could not report its own DOM through the console.
static unsigned g_visitor_seq = 0;  // the request the visitor was started for

static void CEF_CALLBACK visitor_visit(cef_string_visitor_t* self,
                                       const cef_string_t* string) {
    (void)self;
    // A late answer for an earlier request must not become this one's result.
    if (g_source_done || g_visitor_seq != g_request_seq) return;
    if (!string || !string->str || string->length == 0) return;
    cef_string_utf8_t utf8 = {0};
    cef_string_utf16_to_utf8(string->str, string->length, &utf8);
    if (utf8.str && utf8.length > 0) {
        char* html = (char*)malloc(utf8.length);
        if (html) {
            memcpy(html, utf8.str, utf8.length);
            free(g_html);
            g_html = html;
            g_html_len = utf8.length;
            g_source_done = 1;
        }
    }
    cef_string_utf8_clear(&utf8);
}

static cef_string_visitor_t g_html_visitor;

// Dump the DOM now. First by asking the page (the same console route the
// poller uses), waiting one second as the other renderers do. A page that is
// too busy to answer in that second is then given about five more: Chromium
// is asked for the document source, and whichever of the two answers arrives
// first is used. Without the longer wait, a page that is merely slow at that
// moment would end with no result at all.
static void force_dump(void) {
    if (!g_browser || g_source_done) return;
    char js[160];
    snprintf(js, sizeof(js),
             "console.log('%s'+document.documentElement.outerHTML);", g_token);
    if (run_js_main(js)) {
        for (int i = 0; i < 100 && !g_source_done; i++) pump(10);
    }
    if (g_source_done || !g_browser) return;

    cef_frame_t* frame = g_browser->get_main_frame(g_browser);
    if (!frame) return;
    g_visitor_seq = g_request_seq;
    frame->get_source(frame, &g_html_visitor);
    cef_release(frame);
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (!g_source_done && g_browser && GetTickCount64() < deadline) pump(10);
    if (g_source_done) {
        fprintf(stderr, "wick-renderer: the page was slow to answer; waited for it\n");
    }
}

// JS poller: checks text length every 300 ms. When content is >500 chars and
// stable for 1 s (and the optional selector matched) it dumps the DOM. It
// refuses to count the page as stable while a Cloudflare "Just a moment" /
// "Performing security verification" interstitial is on screen — see Wick's
// rust/cef/renderer.m for the full rationale.
static void inject_poller(void) {
    char js[2600];
    snprintf(js, sizeof(js),
        "(function(){"
        "var sel=%s%s%s;"
        "var last=0,stable=0,t=0;"
        "var iv=setInterval(function(){"
        "  var body=document.body;"
        "  var bt=body?body.innerText:'';"
        "  var len=bt.length;"
        "  var blt=bt.toLowerCase();"
        "  var isCF=blt.indexOf('performing security verification')>=0||"
        "           blt.indexOf('checking your browser')>=0;"
        "  t+=300;"
        "  if(isCF){stable=0;last=len;}"
        "  else if(len>500&&len===last){stable+=300;}"
        "  else{stable=0;last=len;}"
        "  var hit=sel?!!document.querySelector(sel):true;"
        "  var stableHit=!isCF&&len>500&&stable>=1000&&hit;"
        "  var cap=isCF?30000:(sel?20000:8000);"
        "  if(stableHit||t>=cap){"
        "    clearInterval(iv);"
        "    console.log('%s'+document.documentElement.outerHTML);"
        "  }"
        "},300);"
        "})();",
        g_selector[0] ? "'" : "",
        g_selector[0] ? g_selector : "null",
        g_selector[0] ? "'" : "",
        g_token);
    run_js_main(js);
}

// Report the centre of a visible challenge iframe (Turnstile, DataDome,
// reCAPTCHA, hCaptcha) through the click prefix.
static void inject_challenge_probe(void) {
    char js[560];
    snprintf(js, sizeof(js),
        "(function(){"
        "var f=document.querySelector("
        "'iframe[src*=\"challenges.cloudflare.com\"],"
        "iframe[src*=\"captcha-delivery.com\"],"
        "iframe[src*=\"recaptcha\"],"
        "iframe[src*=\"hcaptcha\"]');"
        "if(f){"
        "  var r=f.getBoundingClientRect();"
        "  if(r.width>0){"
        "    console.log('%s'+"
        "      Math.round(r.x+r.width/2)+','+"
        "      Math.round(r.y+r.height/2));"
        "  }"
        "}"
        "})();", g_click_prefix);
    run_js_main(js);
}

static void click_challenge(void) {
    cef_browser_host_t* host = g_browser->get_host(g_browser);
    if (!host) return;
    // A click on a widget in a cross-origin frame is routed by the
    // compositor's picture of the page. At the idle rate of one frame a
    // second that picture can predate the widget, and the click then lands
    // on the page behind it. Draw a few current frames first.
    host->set_windowless_frame_rate(host, 30);
    host->invalidate(host, PET_VIEW);
    for (int i = 0; i < 20; i++) pump(15);
    cef_mouse_event_t mouse = {0};
    mouse.x = g_click_x;
    mouse.y = g_click_y;
    // Move first, then press and release — closer to a real pointer.
    host->send_mouse_move_event(host, &mouse, 0);
    pump(100);
    host->send_mouse_click_event(host, &mouse, MBT_LEFT, 0, 1);
    pump(50);
    host->send_mouse_click_event(host, &mouse, MBT_LEFT, 1, 1);
    pump(50);
    host->set_windowless_frame_rate(host, 1);
    cef_release(host);
    fprintf(stderr, "wick-renderer: clicked challenge at %d,%d\n", g_click_x, g_click_y);
}

// Set one browser preference. Takes over the reference to |value|.
static void set_preference(const char* name, cef_value_t* value) {
    if (!value) return;
    cef_request_context_t* context = cef_request_context_get_global_context();
    if (!context) {
        cef_release(value);
        return;
    }
    cef_string_t cef_name = {0};
    cef_string_t error = {0};
    cef_string_utf8_to_utf16(name, strlen(name), &cef_name);
    if (!context->base.set_preference(&context->base, &cef_name, value, &error)) {
        fprintf(stderr, "wick-renderer: could not set the %s preference\n", name);
    }
    cef_string_clear(&cef_name);
    cef_string_clear(&error);
    cef_release(context);
}

// Preferences that have no command-line switch in this Chromium.
static void apply_preferences(void) {
    // Printing off. window.print() would otherwise open the Windows "Print"
    // dialog on the user's desktop and hold the page until someone closed it;
    // with printing disabled it returns at once.
    cef_value_t* printing = cef_value_create();
    if (printing) {
        printing->set_bool(printing, 0);
        set_preference("printing.enabled", printing);
    }

    // WebRTC may not send UDP on its own. By default any page can make the
    // browser send STUN packets straight to an address of its choosing —
    // past a configured proxy, which is how a site behind a proxy or VPN
    // learns the real address. With this policy WebRTC only uses what the
    // proxy carries; no page read here needs a peer connection.
    cef_value_t* webrtc = cef_value_create();
    if (webrtc) {
        cef_string_t policy = {0};
        cef_string_utf8_to_utf16("disable_non_proxied_udp", 23, &policy);
        webrtc->set_string(webrtc, &policy);
        cef_string_clear(&policy);
        set_preference("webrtc.ip_handling_policy", webrtc);
    }
}

// Passkey / security-key requests (navigator.credentials.get and .create with
// publicKey) need no click and no permission: any page can make one, and
// Windows answers with its "Windows Security" dialog on the user's desktop
// (or, with the native API switched off, Chromium with a window of its own).
// Chromium has no switch to refuse these requests. What it does have is the
// virtual authenticator environment of the DevTools protocol, which takes the
// requests away from the operating system. Enabled here with no authenticator
// in it and its UI off: a request then finds no authenticator, shows nothing,
// and the page gets its usual "not allowed" answer.
static void disable_passkey_prompts(void) {
    if (!g_browser) return;
    cef_browser_host_t* host = g_browser->get_host(g_browser);
    if (!host) return;
    static const char message[] =
        "{\"id\":1,\"method\":\"WebAuthn.enable\",\"params\":{\"enableUI\":false}}";
    if (!host->send_dev_tools_message(host, message, sizeof(message) - 1)) {
        fprintf(stderr, "wick-renderer: could not switch off passkey prompts\n");
    }
    cef_release(host);
}

// Create a browser and wait until it is ready for a request.
//
// Every request is rendered in a browser of its own (see replace_browser),
// started on about:blank. Two things depend on that start:
//   * A page's scripts read the window size (window.outerWidth/outerHeight)
//     from the first line on. In a browser that has already shown another
//     page they read 0 x 0 for the first 10-20 ms of each new page — a
//     well-known sign of a headless browser.
//   * Nothing of an earlier page is left: not its history, and not the page
//     itself, which would otherwise stay open (timers, requests, wake locks)
//     until the next request arrived.
static int create_browser(void) {
    cef_window_info_t window_info = {0};
    window_info.size = sizeof(cef_window_info_t);
    window_info.windowless_rendering_enabled = 1;

    cef_browser_settings_t browser_settings = {0};
    browser_settings.size = sizeof(cef_browser_settings_t);
    browser_settings.windowless_frame_rate = 1;
    // CEF lets any page close the browser with window.close() unless told
    // otherwise; a page that did so ended its own render with nothing.
    browser_settings.javascript_close_windows = STATE_DISABLED;
    // Chromium's defaults, stated: no clipboard access for scripts.
    browser_settings.javascript_access_clipboard = STATE_DISABLED;
    browser_settings.javascript_dom_paste = STATE_DISABLED;

    g_browser_closed = 0;
    g_load_done = 0;
    cef_string_t start = {0};
    cef_string_utf8_to_utf16("about:blank", 11, &start);
    cef_browser_t* created = cef_browser_host_create_browser_sync(
        &window_info, &g_client, &start, &browser_settings, NULL, NULL);
    cef_string_clear(&start);
    if (!created) return 0;
    cef_release(created);  // g_browser holds the reference from on_after_created

    // Let about:blank settle so the first real navigation starts from a
    // quiet browser.
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while ((!g_browser || !g_load_done) && GetTickCount64() < deadline) pump(2);
    if (!g_browser) return 0;

    disable_passkey_prompts();
    const ULONGLONG settle = GetTickCount64() + 60;
    while (GetTickCount64() < settle) pump(2);
    return g_browser != NULL;
}

// Take keyboard focus away from the page. CEF gives a new offscreen browser
// focus by itself, and a focused page may, among other things, write to the
// clipboard without asking.
static void unfocus_browser(void) {
    if (!g_browser) return;
    cef_browser_host_t* host = g_browser->get_host(g_browser);
    if (!host) return;
    host->set_focus(host, 0);
    cef_release(host);
}

// Drop every network connection Chromium holds. A closed page leaves its
// connections behind in the pool, where they stay open to the sites it used
// (and their ad and analytics hosts) for minutes; with no page open, nothing
// needs them. Only called while no browser exists.
static volatile LONG g_connections_closed = 0;

static void CEF_CALLBACK on_connections_closed(cef_completion_callback_t* self) {
    (void)self;
    InterlockedExchange(&g_connections_closed, 1);
}

static cef_completion_callback_t g_connections_callback;

static void close_connections(void) {
    cef_request_context_t* context = cef_request_context_get_global_context();
    if (!context) return;
    // The callback is not optional in practice: CEF's header says it may be
    // NULL, and CEF 144 then crashes when the operation completes.
    InterlockedExchange(&g_connections_closed, 0);
    context->close_all_connections(context, &g_connections_callback);
    cef_release(context);
    const ULONGLONG deadline = GetTickCount64() + 2000;
    while (!g_connections_closed && GetTickCount64() < deadline) pump(2);
}

// Close the browser and start a fresh one for the next request.
static void replace_browser(void) {
    if (g_browser) {
        cef_browser_host_t* host = g_browser->get_host(g_browser);
        if (host) {
            host->close_browser(host, 1);
            cef_release(host);
        }
        // A page may take a moment to unload; do not wait on one that will not.
        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (g_browser && GetTickCount64() < deadline) pump(2);
        if (g_browser) {
            // Did not close in time. Stop using it; on_before_close still
            // recognizes it by identifier and will not touch its successor.
            g_browser = NULL;
            g_browser_id = 0;
        }
        close_connections();
    }
    if (!create_browser()) {
        fprintf(stderr, "wick-renderer: could not start a new browser\n");
    }
}

static void clear_result(void) {
    free(g_html);
    g_html = NULL;
    g_html_len = 0;
}

// Navigate to |url| and wait for rendered content. On return g_html holds the
// DOM (g_html_len bytes), or is empty when the page could not be rendered.
static void render_url(const char* url) {
    clear_result();

    // Pages are http or https. Anything else — file:, chrome:, a link to
    // another program — is not rendered, whoever asks.
    if (!is_web_url(url)) {
        fprintf(stderr, "wick-renderer: refused a URL that is not http or https\n");
        return;
    }

    if (!g_browser) replace_browser();  // a page closed it: start another

    g_load_done = 0;
    g_source_done = 0;
    g_failed = 0;
    g_committed = 0;
    g_click_x = -1;
    g_click_y = -1;
    g_clicked = 0;
    g_request_seq++;
    snprintf(g_token, sizeof(g_token), "%s%08x:", g_marker, g_request_seq);

    if (!g_browser) return;
    int page_stuck = 0;
    unfocus_browser();
    cef_frame_t* frame = g_browser->get_main_frame(g_browser);
    if (!frame) return;
    cef_string_t cef_url = {0};
    cef_string_utf8_to_utf16(url, strlen(url), &cef_url);
    frame->load_url(frame, &cef_url);
    cef_string_clear(&cef_url);
    cef_release(frame);

    // Selector mode needs longer outer caps than smart-stability mode: the
    // in-page poller's own timeout is 20 s when waiting on a selector.
    const int post_load_cap = g_selector[0] ? 30000 : 12000;
    const int total_cap = g_selector[0] ? 60000 : 45000;
    const int uncommitted_cap = 1500;

    const ULONGLONG start = GetTickCount64();
    ULONGLONG last = start;
    int post_load_ms = 0;
    int uncommitted_ms = 0;
    int probe_wait_ms = 0;
    int poll_injected = 0;
    int last_nav = g_nav_count;

    while (!g_source_done && !g_failed && g_browser) {
        pump(TICK_MS);
        const ULONGLONG now = GetTickCount64();
        const int dt = (int)(now - last);
        last = now;

        // Page reload (AWS WAF challenge, DataDome redirect, ...): wait for
        // the NEW page's content.
        if (g_nav_count != last_nav) {
            last_nav = g_nav_count;
            post_load_ms = 0;
            uncommitted_ms = 0;
            poll_injected = 0;
        }

        if ((int)(now - start) >= total_cap) {
            fprintf(stderr, "wick-renderer: absolute timeout (%ds), dumping DOM\n",
                    total_cap / 1000);
            if (g_committed) {
                force_dump();
                page_stuck = !g_source_done;
            }
            break;
        }

        if (!g_load_done) continue;

        if (!g_committed) {
            // Loading stopped but no document committed for this request
            // (download, 204, cancelled navigation). The frame still shows an
            // older page, which must not be reported as this URL's content.
            uncommitted_ms += dt;
            if (uncommitted_ms >= uncommitted_cap) {
                fprintf(stderr, "wick-renderer: navigation produced no document\n");
                break;
            }
            continue;
        }

        post_load_ms += dt;

        if (!poll_injected) {
            inject_poller();
            poll_injected = 1;
        }

        // Challenge detection from 2 s after load, re-checked twice a second
        // until a widget shows up.
        if (g_click_enabled && post_load_ms >= 2000 && !g_clicked && g_click_x == -1) {
            probe_wait_ms += dt;
            if (probe_wait_ms >= 500) {
                probe_wait_ms = 0;
                inject_challenge_probe();
            }
        }

        if (g_click_x >= 0 && !g_clicked) {
            click_challenge();
            g_clicked = 1;
            // Give the page time to verify and redirect.
            post_load_ms = 0;
            g_load_done = 0;
            poll_injected = 0;
            last = GetTickCount64();
            continue;
        }

        if (post_load_ms >= post_load_cap) {
            fprintf(stderr, "wick-renderer: timeout waiting for content\n");
            force_dump();
            page_stuck = !g_source_done;
            break;
        }
    }

    if (page_stuck) {
        // Neither the page nor Chromium could produce the document: the
        // page's script is not yielding.
        fprintf(stderr, "wick-renderer: the page never answered\n");
    }
}

// ── Protocol I/O ──────────────────────────────────────────────

// Chromium passes STD_OUTPUT/STD_ERROR on to its helper processes. The
// protocol pipe must stay private to this process: a helper that inherited
// it could write into the stream, and would keep the pipe open after this
// process exits so the parent never sees EOF.
//
// The original stdout handle itself is left open and untouched. libcef.dll
// has a C runtime of its own that captured that handle when the process
// started; closing or replacing it here would leave Chromium writing to a
// stale handle value. (The one thing Chromium is known to print there —
// "Opening in existing browser session." — is avoided by lock_profile.)
static void isolate_stdio(void) {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (in && in != INVALID_HANDLE_VALUE) SetHandleInformation(in, HANDLE_FLAG_INHERIT, 0);
    if (out && out != INVALID_HANDLE_VALUE) SetHandleInformation(out, HANDLE_FLAG_INHERIT, 0);
    // The CRT's stdout keeps the real pipe; only the handle Chromium would
    // hand to children is redirected to stderr.
    if (err && err != INVALID_HANDLE_VALUE) SetStdHandle(STD_OUTPUT_HANDLE, err);
}

// One renderer per profile. Chromium enforces that itself, but its way of
// doing so is to hand the launch over to the running process and print
// "Opening in existing browser session." on stdout — into the protocol
// stream. Holding a lock file for the life of the process lets a second
// renderer step aside before Chromium starts, without disturbing the first.
// The lock is released by the system when the process ends, however it ends.
//
// Returns 1 when the lock is held (or cannot be used at all, which must not
// stop rendering), 0 when another renderer holds it.
static HANDLE g_profile_lock = INVALID_HANDLE_VALUE;

static int lock_profile(const wchar_t* cache_root) {
    wchar_t path[MAX_PATH * 2];
    const size_t cap = sizeof(path) / sizeof(path[0]);
    _snwprintf(path, cap, L"%ls\\wick-renderer.lock", cache_root);
    path[cap - 1] = L'\0';
    // A renderer that was just told to stop may still be shutting down:
    // give it a few seconds before giving up.
    for (int attempt = 0; attempt < 40; attempt++) {
        g_profile_lock = CreateFileW(path, GENERIC_WRITE | DELETE, 0, NULL, OPEN_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (g_profile_lock != INVALID_HANDLE_VALUE) return 1;
        if (GetLastError() != ERROR_SHARING_VIOLATION) return 1;
        Sleep(100);
    }
    return 0;
}

// Reads requests and doubles as the watchdog for the browser thread.
//
// wick waits for each response without a timeout, and holds a lock while it
// does, so a renderer whose browser thread got stuck would stall every later
// render until wick is restarted — and, holding the profile, would keep a
// replacement from starting. Two limits prevent that:
//   * a render that has not answered after RENDER_WATCHDOG_MS (well past the
//     longest the render loop allows) ends the process; wick sees the pipe
//     close, starts a fresh renderer and retries the request;
//   * after stdin closes, the process is gone within SHUTDOWN_WATCHDOG_MS
//     whether or not the browser thread is able to shut down.
#define RENDER_WATCHDOG_MS 100000
#define SHUTDOWN_WATCHDOG_MS 20000

// Read one request line of any length. Returns a malloc'd string without its
// line ending, or NULL at end of input. A line longer than REQUEST_LINE_MAX is
// read to its end and discarded (|too_long| is set): cutting it would turn
// one request into two and put every later response out of step.
static char* read_request_line(int* too_long) {
    size_t cap = 4096, len = 0;
    char* line = (char*)malloc(cap);
    *too_long = 0;
    if (!line) return NULL;
    line[0] = '\0';
    for (;;) {
        if (!fgets(line + len, (int)(cap - len), stdin)) {
            if (len == 0) { free(line); return NULL; }
            break;  // input ended in the middle of a line: take what there is
        }
        len += strlen(line + len);
        if (len > 0 && line[len - 1] == '\n') break;
        if (len + 1 < cap) continue;  // short read without a newline; keep reading
        if (cap >= REQUEST_LINE_MAX) {
            *too_long = 1;
            int c;
            while ((c = fgetc(stdin)) != EOF && c != '\n') {}
            break;
        }
        cap *= 2;
        char* bigger = (char*)realloc(line, cap);
        if (!bigger) {
            *too_long = 1;
            int c;
            while ((c = fgetc(stdin)) != EOF && c != '\n') {}
            break;
        }
        line = bigger;
    }
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
    return line;
}

static unsigned __stdcall stdin_thread(void* arg) {
    (void)arg;
    for (;;) {
        int too_long = 0;
        char* line = read_request_line(&too_long);
        if (!line) break;
        if (!too_long && line[0] == '\0') {  // empty line: orderly shutdown
            free(line);
            break;
        }
        g_req_line = line;
        g_req_too_long = too_long;
        SetEvent(g_req_ready);
        const DWORD waited = WaitForSingleObject(g_req_done, RENDER_WATCHDOG_MS);
        g_req_line = NULL;
        free(line);
        if (waited != WAIT_OBJECT_0) {
            fprintf(stderr, "wick-renderer: a render did not finish within %d s; "
                            "exiting so a fresh renderer can take over\n",
                    RENDER_WATCHDOG_MS / 1000);
            fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 3);
        }
    }
    InterlockedExchange(&g_stdin_eof, 1);
    SetEvent(g_req_ready);
    // The browser thread now shuts CEF down and the process exits, which also
    // ends this thread. If it cannot, do not linger.
    Sleep(SHUTDOWN_WATCHDOG_MS);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

// Split "URL\tSELECTOR" in place. The selector is spliced into a JS '...'
// literal, so single quotes and backslashes are dropped rather than escaped
// (same conservative rule as the other renderers).
static void parse_request(char* line) {
    g_selector[0] = '\0';
    char* tab = strchr(line, '\t');
    if (!tab) return;
    *tab = '\0';
    const char* sel = tab + 1;
    while (*sel == ' ' || *sel == '\t') sel++;
    char* dst = g_selector;
    const size_t dst_cap = sizeof(g_selector) - 1;
    for (; *sel && (size_t)(dst - g_selector) < dst_cap; sel++) {
        if (*sel == '\'' || *sel == '\\') continue;
        *dst++ = *sel;
    }
    *dst = '\0';
}

// ── Privilege drop ────────────────────────────────────────────

static int process_is_elevated(void) {
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 0;
    TOKEN_ELEVATION elevation = {0};
    DWORD size = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

// When wick runs elevated (an administrator shell, or an elevated host
// application), re-run this renderer with a filtered, medium-integrity token —
// the same kind of token UAC gives a normal process. Chromium runs here with
// --no-sandbox, so it should not also hold administrator rights while it
// renders arbitrary pages.
//
// Chromium has its own de-elevation, but it relaunches through the shell and
// loses the inherited stdin/stdout pipes. This version starts the copy
// directly with handle inheritance, so the copy speaks the protocol and this
// process only waits for it. A kill-on-close job ties the copy (and its
// helpers) to this process, so terminating the renderer still stops them all.
//
// Returns 1 when the de-elevated copy ran (|exit_code| is its exit code), 0
// when it could not be started and the caller should continue in-process.
static int run_without_elevation(int* exit_code) {
    int started = 0;
    HANDLE token = NULL, limited = NULL, job = NULL;
    PSID medium = NULL;
    wchar_t* command = NULL;
    PROCESS_INFORMATION child = {0};

    if (!OpenProcessToken(GetCurrentProcess(),
            TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT,
            &token)) goto done;
    if (!CreateRestrictedToken(token, LUA_TOKEN, 0, NULL, 0, NULL, 0, NULL, &limited)) goto done;

    SID_IDENTIFIER_AUTHORITY label_authority = SECURITY_MANDATORY_LABEL_AUTHORITY;
    if (!AllocateAndInitializeSid(&label_authority, 1, SECURITY_MANDATORY_MEDIUM_RID,
                                  0, 0, 0, 0, 0, 0, 0, &medium)) goto done;
    TOKEN_MANDATORY_LABEL label = {0};
    label.Label.Sid = medium;
    label.Label.Attributes = SE_GROUP_INTEGRITY;
    if (!SetTokenInformation(limited, TokenIntegrityLevel, &label,
                             sizeof(label) + GetLengthSid(medium))) goto done;

    wchar_t exe[MAX_PATH * 2];
    DWORD exe_len = GetModuleFileNameW(NULL, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
    if (exe_len == 0 || exe_len >= sizeof(exe) / sizeof(exe[0])) goto done;

    static const wchar_t marker[] = L" --wick-deelevated";
    const wchar_t* original = GetCommandLineW();
    const size_t command_len = wcslen(original) + wcslen(marker) + 1;
    command = (wchar_t*)malloc(command_len * sizeof(wchar_t));
    if (!command) goto done;
    wcscpy(command, original);
    wcscat(command, marker);

    job = CreateJobObjectW(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    STARTUPINFOW startup = {0};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    // The copy shares this process's console when there is one (wick starts
    // the renderer with a windowless console). With no console at all it
    // stays detached, so Windows never opens a console window for it. It must
    // not get a NEW console: creating one under the filtered token fails the
    // process at startup (STATUS_DLL_INIT_FAILED).
    DWORD flags = CREATE_SUSPENDED;
    if (GetConsoleCP() == 0) flags |= DETACHED_PROCESS;

    DWORD launch_error = 0;
    if (!CreateProcessAsUserW(limited, exe, command, NULL, NULL, TRUE,
                              flags, NULL, NULL, &startup, &child)) {
        launch_error = GetLastError();
        goto done;
    }
    if (job) AssignProcessToJobObject(job, child.hProcess);
    ResumeThread(child.hThread);

    WaitForSingleObject(child.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(child.hProcess, &code);
    // These two statuses are only ever produced while Windows initializes a
    // process, before any renderer code ran or read a request: the copy could
    // not start under the filtered token, so fall back to running here.
    if (code == 0xC0000142u /* STATUS_DLL_INIT_FAILED */ ||
        code == 0xC0000135u /* STATUS_DLL_NOT_FOUND */) {
        launch_error = code;
        goto done;
    }
    started = 1;
    *exit_code = (int)code;

done:
    if (!started) {
        fprintf(stderr, "wick-renderer: could not drop administrator rights (error 0x%lx); "
                        "continuing elevated\n", (unsigned long)launch_error);
    }
    if (child.hThread) CloseHandle(child.hThread);
    if (child.hProcess) CloseHandle(child.hProcess);
    if (job) CloseHandle(job);
    if (medium) FreeSid(medium);
    if (limited) CloseHandle(limited);
    if (token) CloseHandle(token);
    free(command);
    return started;
}

// ── Shutdown ──────────────────────────────────────────────────

static unsigned __stdcall shutdown_watchdog(void* arg) {
    (void)arg;
    Sleep(8000);
    // Output is already flushed; a stuck Chromium teardown must not keep an
    // orphaned daemon (and its profile lock) alive.
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

static void shutdown_cef(void) {
    fflush(stdout);
    HANDLE watchdog = (HANDLE)_beginthreadex(NULL, 0, shutdown_watchdog, NULL, 0, NULL);
    if (watchdog) CloseHandle(watchdog);

    if (g_browser) {
        cef_browser_host_t* host = g_browser->get_host(g_browser);
        if (host) {
            host->close_browser(host, 1);
            cef_release(host);
        }
        for (int i = 0; i < 300 && !g_browser_closed; i++) pump(10);
    }
    cef_shutdown();
}

// ── Crashes ───────────────────────────────────────────────────
//
// If this process ever crashes, it ends. Windows' default is an "Application
// Error" box on the desktop that keeps the dead process — and the pipe wick
// is waiting on — until someone clicks OK.
//
// Two settings prevent the box, and Chromium undoes both while it runs: it
// sets the process error mode its own way (crash boxes on) every time a
// browser is created, and may install exception filters of its own. So a
// small thread keeps putting them back.

static LONG WINAPI end_on_crash(EXCEPTION_POINTERS* info) {
    const UINT code = info && info->ExceptionRecord ? (UINT)info->ExceptionRecord->ExceptionCode : 1;
    TerminateProcess(GetCurrentProcess(), code);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void quiet_crashes(void) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(end_on_crash);
}

static unsigned __stdcall quiet_crashes_thread(void* arg) {
    (void)arg;
    for (;;) {
        quiet_crashes();
        Sleep(250);
    }
}

static void keep_crashes_quiet(void) {
    quiet_crashes();
    HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, quiet_crashes_thread, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
}

// ── Main ──────────────────────────────────────────────────────

static void utf8_from_wide(const wchar_t* src, char* dst, int cap) {
    if (WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, cap, NULL, NULL) == 0) dst[0] = '\0';
    dst[cap - 1] = '\0';
}

int wmain(int argc, wchar_t** argv) {
    keep_crashes_quiet();

    if (argc >= 2 && wcscmp(argv[1], L"--version") == 0) {
        printf("wick-renderer (windows) %s CEF %s\n", WICK_RENDERER_PROTOCOL, CEF_VERSION);
        return 0;
    }

    // Drop administrator rights before any Chromium code runs. Only the
    // browser role does this: helpers ("--type=...") inherit the browser's
    // token, and the de-elevated copy carries a marker so it never recurses.
    // WICK_CEF_KEEP_ELEVATION=1 opts out.
    int is_helper = 0, is_copy = 0;
    for (int i = 1; i < argc; i++) {
        if (wcsncmp(argv[i], L"--type=", 7) == 0) is_helper = 1;
        if (wcscmp(argv[i], L"--wick-deelevated") == 0) is_copy = 1;
    }
    char keep_elevation[8] = {0};
    GetEnvironmentVariableA("WICK_CEF_KEEP_ELEVATION", keep_elevation, sizeof(keep_elevation));
    if (!is_helper && !is_copy && keep_elevation[0] != '1' && process_is_elevated()) {
        int copy_exit = 1;
        if (run_without_elevation(&copy_exit)) return copy_exit;
    }

    // Read before CEF starts: the browser process forwards it as a switch.
    // wick accepts socks5h:// (resolve names at the proxy); Chromium spells
    // that socks5:// and always resolves at the proxy.
    {
        char proxy[sizeof(g_proxy)] = {0};
        DWORD len = GetEnvironmentVariableA("WICK_PROXY", proxy, sizeof(proxy));
        if (len > 0 && len < sizeof(proxy)) {
            if (_strnicmp(proxy, "socks5h://", 10) == 0) {
                snprintf(g_proxy, sizeof(g_proxy), "socks5://%s", proxy + 10);
            } else {
                snprintf(g_proxy, sizeof(g_proxy), "%s", proxy);
            }
        }
    }

    init_markers();
    cef_api_hash(CEF_API_VERSION, 0);

    init_base(&g_render_process_handler.base, sizeof(cef_render_process_handler_t));
    g_render_process_handler.on_context_created = on_context_created;

    init_base(&g_browser_process_handler.base, sizeof(cef_browser_process_handler_t));
    g_browser_process_handler.on_already_running_app_relaunch = on_already_running_app_relaunch;

    init_base(&g_app.base, sizeof(cef_app_t));
    g_app.on_before_command_line_processing = on_before_command_line_processing;
    g_app.get_browser_process_handler = get_browser_process_handler;
    g_app.get_render_process_handler = get_render_process_handler;

    cef_main_args_t main_args = {0};
    main_args.instance = GetModuleHandleW(NULL);

    // Helper role (only when wick-helper.exe is missing and this binary was
    // launched as its own subprocess).
    int exit_code = cef_execute_process(&main_args, &g_app, NULL);
    if (exit_code >= 0) return exit_code;

    isolate_stdio();

    // Arguments: one-shot mode (URL given) or daemon mode (no URL).
    static char url_arg[REQUEST_LINE_MAX];
    int one_shot = 0;
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--cookie") == 0 && i + 1 < argc) {
            utf8_from_wide(argv[++i], g_cookie, sizeof(g_cookie));
        } else if (argv[i][0] != L'-') {
            utf8_from_wide(argv[i], url_arg, sizeof(url_arg));
            one_shot = 1;
        }
    }

    init_base(&g_display_handler.base, sizeof(cef_display_handler_t));
    g_display_handler.on_console_message = on_console_message;

    init_base(&g_html_visitor.base, sizeof(cef_string_visitor_t));
    g_html_visitor.visit = visitor_visit;

    init_base(&g_load_handler.base, sizeof(cef_load_handler_t));
    g_load_handler.on_loading_state_change = on_loading_state_change;
    g_load_handler.on_load_start = on_load_start;
    g_load_handler.on_load_end = on_load_end;
    g_load_handler.on_load_error = on_load_error;

    init_base(&g_render_handler.base, sizeof(cef_render_handler_t));
    g_render_handler.get_view_rect = get_view_rect;
    g_render_handler.on_paint = on_paint;
    g_render_handler.get_screen_info = get_screen_info;

    init_base(&g_life_span_handler.base, sizeof(cef_life_span_handler_t));
    g_life_span_handler.on_before_popup = on_before_popup;
    g_life_span_handler.on_after_created = on_after_created;
    g_life_span_handler.do_close = do_close;
    g_life_span_handler.on_before_close = on_before_close;

    init_base(&g_download_handler.base, sizeof(cef_download_handler_t));
    g_download_handler.can_download = can_download;
    g_download_handler.on_before_download = on_before_download;
    g_download_handler.on_download_updated = on_download_updated;

    init_base(&g_jsdialog_handler.base, sizeof(cef_jsdialog_handler_t));
    g_jsdialog_handler.on_jsdialog = on_jsdialog;
    g_jsdialog_handler.on_before_unload_dialog = on_before_unload_dialog;

    init_base(&g_dialog_handler.base, sizeof(cef_dialog_handler_t));
    g_dialog_handler.on_file_dialog = on_file_dialog;

    init_base(&g_focus_handler.base, sizeof(cef_focus_handler_t));
    g_focus_handler.on_set_focus = on_set_focus;

    init_base(&g_connections_callback.base, sizeof(cef_completion_callback_t));
    g_connections_callback.on_complete = on_connections_closed;

    init_base(&g_permission_handler.base, sizeof(cef_permission_handler_t));
    g_permission_handler.on_request_media_access_permission = on_request_media_access_permission;
    g_permission_handler.on_show_permission_prompt = on_show_permission_prompt;

    init_base(&g_resource_request_handler.base, sizeof(cef_resource_request_handler_t));
    g_resource_request_handler.on_protocol_execution = on_protocol_execution;

    init_base(&g_request_handler.base, sizeof(cef_request_handler_t));
    g_request_handler.on_before_browse = on_before_browse;
    g_request_handler.on_open_urlfrom_tab = on_open_urlfrom_tab;
    g_request_handler.get_resource_request_handler = get_resource_request_handler;
    g_request_handler.get_auth_credentials = get_auth_credentials;
    g_request_handler.on_select_client_certificate = on_select_client_certificate;
    g_request_handler.on_render_process_unresponsive = on_render_process_unresponsive;
    g_request_handler.on_render_process_terminated = on_render_process_terminated;

    init_base(&g_client.base, sizeof(cef_client_t));
    g_client.get_life_span_handler = get_life_span_handler;
    g_client.get_load_handler = get_load_handler;
    g_client.get_render_handler = get_render_handler;
    g_client.get_display_handler = get_display_handler;
    g_client.get_download_handler = get_download_handler;
    g_client.get_jsdialog_handler = get_jsdialog_handler;
    g_client.get_dialog_handler = get_dialog_handler;
    g_client.get_focus_handler = get_focus_handler;
    g_client.get_permission_handler = get_permission_handler;
    g_client.get_request_handler = get_request_handler;

    char click[8] = {0};
    DWORD click_len = GetEnvironmentVariableA("WICK_CEF_CLICK", click, sizeof(click));
    if (click_len == 1 && (click[0] == '0' || click[0] == '1')) {
        g_click_enabled = click[0] == '1';
    } else {
        g_click_enabled = CLICK_DEFAULT;
    }

    cef_settings_t settings = {0};
    settings.size = sizeof(cef_settings_t);
    settings.windowless_rendering_enabled = 1;
    settings.no_sandbox = 1;
    settings.log_severity = LOGSEVERITY_ERROR;
    settings.persist_session_cookies = 1;

    wchar_t exe_dir[MAX_PATH * 2];
    wchar_t path[MAX_PATH * 2];
    const size_t path_cap = sizeof(path) / sizeof(path[0]);
    if (get_exe_dir(exe_dir, (DWORD)(sizeof(exe_dir) / sizeof(exe_dir[0]))) != 0) {
        fprintf(stderr, "wick-renderer: cannot resolve executable directory\n");
        return 1;
    }

    // wick-helper.exe injects the stealth patches at V8 context creation
    // (before page scripts run). Fall back to this binary, which carries the
    // same render-process handler.
    _snwprintf(path, path_cap, L"%ls\\wick-helper.exe", exe_dir);
    path[path_cap - 1] = L'\0';
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        GetModuleFileNameW(NULL, path, (DWORD)path_cap);
    }
    set_path(&settings.browser_subprocess_path, path);

    set_path(&settings.resources_dir_path, exe_dir);
    _snwprintf(path, path_cap, L"%ls\\locales", exe_dir);
    path[path_cap - 1] = L'\0';
    set_path(&settings.locales_dir_path, path);

    // Profile under <home>\.wick\cef-cache, as on the other platforms. The
    // Default profile directory is required for cookie persistence (AWS WAF
    // tokens and similar). Chromium allows one process per profile, so a
    // second wick that must render at the same time (tests, a CLI call next
    // to a running server) can point WICK_CEF_CACHE_DIR at its own directory.
    wchar_t cache_root[MAX_PATH * 2];
    const size_t cache_cap = sizeof(cache_root) / sizeof(cache_root[0]);
    DWORD custom = GetEnvironmentVariableW(L"WICK_CEF_CACHE_DIR", cache_root, (DWORD)cache_cap);
    if (custom == 0 || custom >= cache_cap) {
        wchar_t wick_home[MAX_PATH * 2];
        get_wick_home(wick_home, sizeof(wick_home) / sizeof(wick_home[0]));
        _snwprintf(cache_root, cache_cap, L"%ls\\cef-cache", wick_home);
        cache_root[cache_cap - 1] = L'\0';
    }
    make_dirs(cache_root);  // the lock and the log file below need the directory
    if (!lock_profile(cache_root)) {
        fprintf(stderr, "wick-renderer: another wick-renderer is already using the profile %ls. "
                        "Stop it, or set WICK_CEF_CACHE_DIR to a separate directory.\n",
                cache_root);
        return 1;
    }
    set_path(&settings.root_cache_path, cache_root);
    _snwprintf(path, path_cap, L"%ls\\Default", cache_root);
    path[path_cap - 1] = L'\0';
    set_path(&settings.cache_path, path);
    // Keep the log out of the install directory (the Windows default is
    // debug.log next to the executable).
    _snwprintf(path, path_cap, L"%ls\\debug.log", cache_root);
    path[path_cap - 1] = L'\0';
    set_path(&settings.log_file, path);

    if (!cef_initialize(&main_args, &settings, &g_app, NULL)) {
        const int code = cef_get_exit_code();
        if (code == CEF_RESULT_CODE_PROFILE_IN_USE ||
            code == CEF_RESULT_CODE_NORMAL_EXIT_PROCESS_NOTIFIED) {
            fprintf(stderr, "wick-renderer: another wick-renderer is already using the profile %ls. "
                            "Stop it, or set WICK_CEF_CACHE_DIR to a separate directory.\n",
                    cache_root);
        } else if (code == CEF_RESULT_CODE_MISSING_DATA) {
            fprintf(stderr, "wick-renderer: CEF runtime files are missing next to the "
                            "executable; reinstall the renderer.\n");
        } else {
            fprintf(stderr, "wick-renderer: cef_initialize failed (CEF exit code %d)\n", code);
        }
        return 1;
    }

    apply_preferences();

    if (!create_browser()) {
        fprintf(stderr, "wick-renderer: browser did not start\n");
        cef_shutdown();
        return 1;
    }

    if (one_shot) {
        render_url(url_arg);
        if (g_html && g_html_len > 0) fwrite(g_html, 1, g_html_len, stdout);
        fflush(stdout);
        int ok = g_html_len > 0;
        shutdown_cef();
        return ok ? 0 : 2;
    }

    // ── Daemon mode ───────────────────────────────────────────
    g_req_ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_req_done = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE reader = (HANDLE)_beginthreadex(NULL, 0, stdin_thread, NULL, 0, NULL);
    if (!g_req_ready || !g_req_done || !reader) {
        fprintf(stderr, "wick-renderer: cannot start the request reader\n");
        shutdown_cef();
        return 1;
    }

    for (;;) {
        cef_do_message_loop_work();
        if (WaitForSingleObject(g_req_ready, IDLE_TICK_MS) != WAIT_OBJECT_0) continue;
        if (g_stdin_eof) break;

        if (g_req_too_long) {
            fprintf(stderr, "wick-renderer: request line too long (over %d bytes)\n",
                    REQUEST_LINE_MAX);
            clear_result();
        } else {
            parse_request(g_req_line);
            render_url(g_req_line);
        }

        // Length-prefixed response; "0\n" tells the caller the render failed.
        fprintf(stdout, "%zu\n", g_html_len);
        if (g_html && g_html_len > 0) fwrite(g_html, 1, g_html_len, stdout);
        fflush(stdout);

        // The caller has its answer. Before taking the next request, close
        // this browser — and with it the page, whatever state it is in —
        // and start the one the next request will use.
        replace_browser();
        SetEvent(g_req_done);
        if (!g_browser) break;  // no browser can be started: let the caller restart us
    }

    shutdown_cef();
    return 0;
}
