# wick-renderer-windows

An unofficial Windows (x64) build of the headless Chromium renderer that
[Wick](https://github.com/wickproject/wick) uses for pages that need
JavaScript or show a bot check. Wick ships this renderer for macOS and Linux
only. This project is not part of Wick and is not endorsed by its authors.

It is two programs built on [CEF](https://bitbucket.org/chromiumembedded/cef)
144.0.18 (Chromium 144.0.7559.246), through CEF's C API:

- `wick-renderer.exe` loads a page off-screen, waits until it has settled and
  prints its HTML. It speaks the same stdin/stdout protocol as Wick's other
  renderers, so Wick can drive it, and so can any other program.
- `wick-helper.exe` runs Chromium's sub-processes (pages, GPU, network).

## Status

- Built and used on Windows 11 x64 with Visual Studio 2022 Build Tools. Not
  tried on Windows 10, and x64 only.
- Tested with a separate suite, against pages served on loopback. It covered
  the protocol, every time limit, crashes, privileges, network traffic and
  everything in "What a page can and cannot do". There were also a 20-minute
  soak and 116 real sites through Wick. That suite is not part of this
  repository.
- Source only. There are no prebuilt downloads: build it yourself (below).

## Build

Needs:

- Visual Studio 2022, or Build Tools for Visual Studio 2022, with
  "Desktop development with C++" (MSVC x64 and a Windows 10/11 SDK).
- `curl.exe` and `tar.exe`, which come with Windows 10 and 11.
- About 1 GB of disk space while building.

```powershell
powershell -ExecutionPolicy Bypass -File build-windows.ps1
```

This downloads the CEF distribution into this folder (about 150 MB from
cef-builds.spotifycdn.com; its SHA-1 is checked against the published one),
compiles both programs and stages a runnable folder in `out\windows64`
(about 360 MB). Options: `-SdkRoot` (where CEF is kept), `-OutDir`,
`-InstallDir` (copy the result there too) and `-Locales en-US` (only the
language packs you name).

## Use with Wick

1. **Wick needs a small change to use a renderer on Windows.** Unpatched, it
   looks for `wick-renderer` without `.exe`, only under `HOME`, which
   Windows does not set, and it would open a console window for it.
   `patches/wick-windows.patch` fixes that, against Wick commit `a867821`
   (version 0.12.0). It also makes Wick keep its state in `%USERPROFILE%`
   when `HOME` is unset, and passes Wick's `--proxy` on to the renderer.

   ```powershell
   git clone https://github.com/wickproject/wick
   cd wick
   git checkout a867821
   git apply ..\wick-renderer-windows\patches\wick-windows.patch
   cd rust
   cargo build --release
   ```

2. **Build the renderer and install it where Wick looks for it:**

   ```powershell
   powershell -ExecutionPolicy Bypass -File install-cef-windows.ps1
   ```

   It installs to `%USERPROFILE%\.wick\cef` (or `%HOME%\.wick\cef` when
   `HOME` is set). It skips the build when an up-to-date renderer is already
   there (`-Force` rebuilds), and refuses while a renderer is running from
   that folder. With the patch, `wick install cef` runs this script when
   `WICK_CEF_INSTALLER` holds its path.

3. **Try it:** `wick fetch https://example.com --render cef`

If a per-application firewall is running, it will ask about
`wick-helper.exe`. That is Chromium's network process, which loads the pages.

## Protocol

```
wick-renderer.exe --version
    wick-renderer (windows) WICK_RENDERER_PROTOCOL=v3-cf-aware CEF 144.0.18+gc5b2ec2+chromium-144.0.7559.246

wick-renderer.exe <url> [--cookie name=value]
    Renders one page and writes its HTML to stdout.
    Exit code 0 when there is HTML, 2 when the page could not be rendered.

wick-renderer.exe
    Daemon. Reads one request per line from stdin:
        <url>\n
        <url>\t<css selector>\n      (also wait until the selector matches)
    and answers each one on stdout with
        <byte count>\n<HTML, UTF-8>
    An answer of "0\n" means the page could not be rendered.
    An empty line, or the end of stdin, stops the daemon.
```

- Only `http` and `https` URLs are rendered. Request lines can be up to
  64 KB long; a longer one gets a single `0` answer.
- One request at a time, each in a fresh browser that is closed as soon as
  its answer is written.
- A page is taken as soon as its text is longer than 500 characters, has
  not changed for a second and, when a selector was given, the selector
  matches. Otherwise it is taken as it is after 8 seconds (20 with a
  selector, 30 while a Cloudflare check is showing), so a short page always
  takes 8 seconds. A page whose scripts keep the browser busy ends as an
  empty answer after 18 to 51 seconds.
- Exit code 3: a render did not answer within 100 seconds (the watchdog), so
  the caller can restart the renderer. After stdin closes, the process is
  gone within 20 seconds. Exit code 1: it could not start, for example
  because another renderer is using the same profile.
- Diagnostics go to stderr, as lines starting with `wick-renderer:`.

## Environment variables

| Variable | Effect |
|---|---|
| `WICK_CEF_CACHE_DIR` | Chromium profile directory (cookies, cache). Default `<home>\.wick\cef-cache`. One renderer per profile. |
| `WICK_PROXY` | Proxy for page loads, e.g. `socks5://127.0.0.1:1080`. Wick sets it from its own `--proxy`. |
| `WICK_CEF_FLAGS` | Extra Chromium switches, separated by spaces, for troubleshooting. |
| `WICK_CEF_CLICK=1` | Click Turnstile, reCAPTCHA, hCaptcha and DataDome widgets, as Wick's other renderers always do. Off by default (see below). |
| `WICK_CEF_STEALTH=1` | Run Wick's fingerprint script (`stealth.h`), repaired. Off by default (see below). |
| `WICK_CEF_KEEP_ELEVATION=1` | Keep administrator rights when started from an elevated process. |

The renderer itself sends no telemetry.

## What a page can and cannot do

The renderer is a full browser engine running pages nobody is looking at, so
it refuses everything that reaches outside it:

| A page tries to… | What happens |
|---|---|
| download a file (also by script, or a link with `download`) | refused; nothing is written to the Downloads folder |
| open a window (`window.open`, `target=_blank`) | refused |
| show `alert` / `confirm` / `prompt`, or "Leave site?" | answered at once, nothing shown |
| print (`window.print()`) | returns at once; no Print dialog |
| open another program (`mailto:`, `slack://`, any registered scheme) | dropped; the page stays as it is |
| ask for a password (HTTP 401) | declined; the server's 401 page is returned |
| ask for a passkey or security key | no "Windows Security" dialog; no authenticator is found |
| use camera, microphone, screen capture, location, notifications, MIDI, local fonts, or read the clipboard | refused |
| write to the clipboard | refused (it has no click and no focus) |
| reach a service on this machine from a public address | refused |
| send direct UDP packets (WebRTC), the way a site sees past a proxy | none are sent |
| open a file picker | cancelled |
| play sound | muted |
| close the browser (`window.close()`) | ignored |
| keep running after its HTML was returned (timers, requests, wake locks) | the browser it ran in is closed |
| be loaded from `file:`, `chrome:` or any scheme but `http`/`https` | not rendered |

A crash ends the process quietly, without the "Application Error" box that
would otherwise stay on the desktop until someone closed it.

## Differences from Wick's macOS and Linux renderers

- **Drops administrator rights.** Started from an elevated process, it
  restarts itself with a normal medium-integrity token before any Chromium
  code runs.
- **A browser per request.** Each request gets a fresh browser, so the
  previous page is closed rather than left running. Every page also sees a
  real window size from its first script on: a reused browser reports
  `window.outerWidth` as 0 for the first 10–20 ms of each page, a known sign
  of a headless browser.
- **No clicking by default.** Wick's other renderers click the first
  challenge widget they find. The click is a real input event, so it also
  gives the page a "user gesture", which browsers require for popups, file
  pickers, speech and clipboard writes. With `WICK_CEF_CLICK=1`, a clicked
  page can start speech synthesis and overwrite the clipboard with the copy
  command; everything else above stays refused. In tests against real sites
  the click cleared no challenge.
- **Fewer calls home.** Chromium's network-time query, AI-mode eligibility
  check, search preconnect, component updates, Cloud Messaging check-in and
  Cast device discovery are off. One request remains at each start:
  `accounts.google.com/ListAccounts`, with no account and no cookies.
- **A failed load is reported as a failure.** A download, an aborted load or
  a network error gives an empty answer, not the previous page's HTML or
  Chromium's error page.
- **It cannot hang its caller.** See the watchdog in "Protocol".
- **The fingerprint script is opt-in.** In Wick's `stealth.h`, a JavaScript
  comment turns the whole script into a syntax error, so it has not been
  running on any platform. This build keeps that behaviour by default;
  `WICK_CEF_STEALTH=1` runs a repaired script. Wick's fixed time-zone
  override is left out on Windows, where the system time zone already
  matches the connection.

## Limitations

- x64 only, and tied to the CEF version in `build-windows.ps1`. Chromium 144
  gets no security fixes here unless you rebuild against a newer CEF: change
  `$CefVersion`, and expect to adapt code when CEF's C API changes.
- One render at a time per renderer.
- A page can tell this is not an ordinary desktop Chrome if it looks. There
  is no browser frame around the page (outer size equals inner size until
  Wick's fix-up script adds 85 px after load), the page never has focus, and
  nothing is ever granted.
- Content inside shadow DOM is not in the result: the HTML is the page's
  `outerHTML`.

## License

MIT; see [LICENSE](LICENSE). Parts come from Wick, which is published under
the MIT License.

CEF is BSD-licensed. It is downloaded at build time and is not part of this
repository. The build copies CEF's `LICENSE.txt` into the output as
`CEF-LICENSE.txt`. If you redistribute a build, also include `CREDITS.html`
from the CEF distribution, which holds the licenses of Chromium's
third-party code.
