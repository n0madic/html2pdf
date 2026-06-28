# html2pdf

A lightweight command-line HTML → **PNG** / **PDF** renderer built on
[litehtml](https://github.com/litehtml/litehtml) and [Cairo](https://www.cairographics.org/).

It loads an HTML document (local file or HTTP/HTTPS URL), lays it out with the
litehtml engine into an off-screen Cairo surface, and writes the result as a
raster **PNG** or a vector **PDF** — selected automatically from the `--output`
file extension. No Qt, GTK, headless browser or other heavy runtime is required.

## Features

- Render local files (`--input`) or remote pages (`--url`, HTTP/HTTPS via libcurl).
- PNG (raster) and PDF (single page, vector) output, chosen by file extension.
- External resources — stylesheets (`<link>`, `@import`) and images — are fetched
  relative to their own base: a stylesheet's `url(...)` and nested `@import`
  resolve against the stylesheet's location, and a relative `<base href>` and
  HTTP redirects are honoured.
- Image decoding for PNG / JPEG / GIF / BMP via the vendored, public-domain
  [`stb_image`](https://github.com/nothings/stb).
- Inline `data:` URIs (RFC 2397, base64 or percent-encoded) for images and
  stylesheets, decoded in-process with no network or filesystem access.
- Automatic content height (`--height` omitted) or a fixed canvas size; output
  dimensions are clamped to Cairo's surface limit (32767 px per side).
- Graceful degradation: unreachable stylesheets/images never abort the render.
- Security-conscious resource loading: only `http`/`https` URLs are fetched,
  HTTP requests are guarded against SSRF, and filesystem sub-resources are
  contained within the document directory — see [Security](#security).
- Small dependency surface; litehtml and its bundled gumbo parser are linked
  statically into the binary.
- Two selectable text backends: a dependency-light Cairo *toy* API (default
  fallback) or **Pango** for proper text shaping, BiDi and font fallback — see
  [Text backends](#text-backends).

## Usage

```
html2pdf (--input FILE | --url URL) --output FILE [options]
```

Exactly one source (`--input` or `--url`) is required, and `--output` is
mandatory. The output format is taken from the extension (`.png` or `.pdf`).

| Option | Default | Description |
|--------|---------|-------------|
| `--input FILE`     | —             | Render a local HTML file |
| `--url URL`        | —             | Fetch and render an HTTP(S) URL |
| `--output FILE`    | —             | Output path; `.png` or `.pdf` picks the format |
| `--width N`        | `1024`        | Render width in pixels |
| `--height N`       | `0` (auto)    | Render height in pixels; `0` = content height |
| `--user-agent STR` | `html2pdf/1.0`| HTTP `User-Agent` header |
| `--timeout N`      | `30`          | HTTP timeout in seconds |
| `--allow-local-network` | off      | Allow fetching loopback/private/link-local hosts (disables the SSRF guard) |
| `-h`, `--help`     | —             | Show help and exit |

### Examples

```bash
# Local file to PNG (auto height)
html2pdf --input page.html --output page.png

# Local file to a single-page PDF
html2pdf --input page.html --output page.pdf

# Remote page to a fixed-size PNG
html2pdf --url https://example.com/ --output example.png --width 1280 --height 720
```

### Exit codes

| Code | Meaning |
|------|---------|
| `0` | Success |
| `1` | Invalid arguments |
| `2` | File I/O error (local input, or writing the output file) |
| `3` | Network error (remote input, including a rejected scheme or an SSRF-blocked host) |
| `4` | Render / graphics error |

All diagnostic messages are written to `stderr`.

## Dependencies

| Dependency | How it is obtained | Linkage |
|------------|--------------------|---------|
| litehtml (+ bundled gumbo) | CMake `FetchContent` (needs network on first configure) | static |
| reference `container_cairo` | compiled from the litehtml source tree | static |
| `stb_image.h` | vendored in `third_party/` | header-only |
| Cairo | system / Homebrew, via `pkg-config` | mixed* |
| libcurl | system / Homebrew, via `pkg-config` | mixed* |
| Pango (`pangocairo`) — **optional** | system / Homebrew, via `pkg-config` | mixed* |

\* These and their transitive dependencies (freetype, libpng, fontconfig, glib,
harfbuzz, …) are linked according to `HTML2PDF_DEP_LINKAGE` — by default
**static where a `.a` archive is available, dynamic otherwise**. See
[Dependency linkage](#dependency-linkage) for the `shared`/`mixed`/`static`
modes.

Requirements: a C++17 compiler, CMake ≥ 3.16, `pkg-config`, and the Cairo and
libcurl development packages.

```bash
# macOS (Homebrew)
brew install cmake pkg-config cairo curl

# Debian/Ubuntu
sudo apt install build-essential cmake pkg-config libcairo2-dev libcurl4-openssl-dev
```

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j     # first configure clones litehtml v0.10
```

The executable is written to `build/html2pdf`.

litehtml and gumbo are always linked statically. By default the external
dependencies use **mixed** linkage (static where a `.a` exists, dynamic
otherwise) — see [Dependency linkage](#dependency-linkage). Inspect the
remaining dynamic dependencies with:

```bash
otool -L build/html2pdf      # macOS
ldd    build/html2pdf        # Linux
```

## Text backends

The text/font hooks have two interchangeable implementations, chosen at
configure time via `HTML2PDF_TEXT_BACKEND`:

| Value | Behaviour |
|-------|-----------|
| `auto` (default) | Use Pango if `pangocairo` is found via `pkg-config`, otherwise the toy backend |
| `pango` | Require Pango (configure fails if `pangocairo` is missing) |
| `toy` | Always use the Cairo toy text API |

```bash
# Force the Pango backend
cmake -S . -B build -DHTML2PDF_TEXT_BACKEND=pango
# Force the dependency-light toy backend
cmake -S . -B build -DHTML2PDF_TEXT_BACKEND=toy
```

The configure step prints which backend was selected, and the active backend is
also shown at the bottom of `--help`.

- **Toy** (Cairo toy text API) — no extra dependencies, but no complex-script
  shaping, BiDi or font fallback; text decorations are drawn manually
  (underline / line-through / overline only).
- **Pango** (`container_cairo_pango` from the litehtml tree) — proper shaping,
  bidirectional text, automatic font fallback and the full set of CSS
  `text-decoration` styles (solid / double / dotted / dashed / wavy). Requires
  the `pangocairo` development package:

  ```bash
  brew install pango                 # macOS (Homebrew)
  sudo apt install libpango1.0-dev   # Debian/Ubuntu
  ```

## Dependency linkage

litehtml and gumbo are always static. How the external dependencies (Cairo,
libcurl, optionally Pango and their transitive closure) are linked is selected
with `HTML2PDF_DEP_LINKAGE`:

| Value | Behaviour |
|-------|-----------|
| `mixed` (default) | Link each dependency statically when a static archive (`.a`) is available in its pkg-config search path, dynamically otherwise. |
| `shared` | Link all external dependencies dynamically (smallest binary). |
| `static` | Link **everything** statically (`-static`). Needs a full static toolchain (Linux/musl); fails on macOS. |

```bash
cmake -S . -B build                                   # mixed (default)
cmake -S . -B build -DHTML2PDF_DEP_LINKAGE=shared      # all dynamic
cmake -S . -B build -DHTML2PDF_DEP_LINKAGE=static      # all static (Linux/musl)
```

The configure step prints the selected mode. (`-DHTML2PDF_STATIC_DEPS=ON` is
kept as a back-compat alias for `static`.)

**`mixed`** maximises self-containment without requiring a full static
toolchain: on macOS it statically embeds Pango, GLib, HarfBuzz, FreeType,
Fontconfig, libpng, PCRE2, FriBidi, etc., while libraries with no Homebrew `.a`
(Cairo, pixman, expat, graphite2) and the system libraries (libc, libz,
libcurl…) stay dynamic. Verify with `otool -L` / `ldd`.

**`static`** produces a binary with **no** runtime shared-library dependencies.
It works on **Linux/musl** (e.g. Alpine), which ships `*-static` packages and a
static C library. It is **impossible on macOS**: Apple ships no static
`libSystem`/`libz`/`libiconv` (only `.tbd` stubs) and Homebrew ships no static
Cairo/pixman archives, so the link fails at `-lcairo`.

### Fully static Linux binary (Docker)

The provided Dockerfile builds the `static` variant on Alpine (Pango has no
static package in any distro, so it is compiled from source automatically):

```bash
# Pango backend (default); use --build-arg TEXT_BACKEND=toy for the toy backend
docker build -f docker/Dockerfile.alpine-static -t html2pdf-static .

# Extract the binary
id=$(docker create html2pdf-static)
docker cp "$id:/usr/local/bin/html2pdf" ./html2pdf
docker rm "$id"

ldd ./html2pdf      # -> "not a dynamic executable"
```

Approximate `strip`ped sizes:

| Backend | `shared` (macOS) | `mixed` (macOS) | `static` (Alpine x86_64) |
|---------|-----------------:|----------------:|-------------------------:|
| toy     | ~1.8 MiB | — | ~11 MiB |
| Pango   | ~1.9 MiB | ~6.6 MiB | ~14 MiB |

CJK / emoji rendering with the static Pango binary still depends on fonts being
present in the runtime image (`apk add font-noto-cjk font-noto-emoji`).

## Security

The renderer fetches resources named by the input document, so it treats both
the document and its references as potentially untrusted:

- **Scheme restriction** — `--url` and every sub-resource accept only
  `http`/`https`. `file:`, `ftp:`, `gopher:`, `dict:`, … are rejected (and
  redirects are constrained to `http`/`https`), preventing local-file
  disclosure and protocol smuggling.
- **SSRF guard** — HTTP connections whose resolved peer is a loopback, private,
  link-local (incl. the cloud metadata endpoint `169.254.169.254`), CGNAT or
  otherwise non-public address are blocked. The check runs per connection, so it
  also covers redirect targets. Pass `--allow-local-network` to disable it when
  you deliberately render an internal host (e.g. a localhost dashboard).
- **Filesystem containment** — relative references in a local document resolve
  within the document's directory; absolute paths and `../` escapes are
  rejected, so a document cannot read arbitrary files via its image/CSS
  references.
- **Response size cap** — a single HTTP response is capped at 64 MiB to bound
  worst-case memory use against an unbounded stream.

Blocked or rejected resources degrade gracefully (the sub-resource is skipped);
a blocked or rejected **main** URL fails with exit code 3.

## How it works

```
src/
├── main.cpp            Orchestration and exit codes
├── cli.{h,cpp}         Argument parsing -> Options
├── resource_loader.*   Disk + HTTP(S) loading (libcurl), relative-URL resolution
├── html_container.*    HtmlContainer : container_cairo[_pango]
└── renderer.*          Lay out the document and write PNG / PDF
```

`HtmlContainer` inherits all drawing (backgrounds, borders, gradients, image
blitting, clipping) from the reference `container_cairo`. Its base class is
selected at build time: with the Pango backend it derives from
`container_cairo_pango` (which supplies the font hooks), otherwise from
`container_cairo`. It implements the remaining hooks:

- **Fonts** — with the toy backend, via the Cairo "toy" text API
  (`cairo_select_font_face` / `cairo_font_extents` / `cairo_text_extents` /
  `cairo_show_text`), with decorations drawn manually; with the Pango backend
  these are provided by `container_cairo_pango`.
- **Images** through `stb_image`, decoded to a premultiplied Cairo `ARGB32`
  surface and cached.
- **Resource loading** (`get_image`, `load_image`, `import_css`) delegated to
  `ResourceLoader`, which resolves relative references against the document base
  (filesystem directory, or via libcurl's `CURLU` API for URLs) and decodes
  inline `data:` URIs without any I/O.

The PDF output is a single page sized to the full content height (1px = 1pt);
there is no pagination.

## Testing

```bash
tests/run_tests.sh                 # uses build/html2pdf by default
tests/run_tests.sh /path/to/html2pdf
```

The suite covers the four spec scenarios — a simple page to PNG/PDF, external
CSS + images (locally and, when `python3` is present, over HTTP), a tall page
whose height tracks the content, and the error/exit-code paths — plus graceful
degradation for unreachable resources and the empty-document edge case. The
HTTP tests additionally assert URL resolution from the server access log
(per-resource stylesheet base, relative `<base href>`, post-redirect base), and
a security/robustness section covers scheme rejection, the SSRF guard,
filesystem containment, strict numeric parsing, output-dimension clamping and
file-I/O exit codes. It checks exit codes, output-file magic bytes and (where
relevant) requested URLs, and is backend-agnostic (passes with either text
backend).

`tests/fixtures/unicode.html` (CJK, RTL, diacritics, emoji, wavy decoration) is
included for a manual visual comparison of the two backends; the automated test
only checks that it renders to a valid file.

## Limitations

- With the **toy** text backend there is no font fallback, complex-script
  shaping or BiDi, so CJK, emoji and right-to-left text may render incorrectly.
  This is a deliberate trade-off for a small dependency footprint. Build with the
  **Pango** backend (`-DHTML2PDF_TEXT_BACKEND=pango`) to render these correctly,
  subject to the relevant fonts being installed on the system.
- Only PNG / JPEG / GIF / BMP images are decoded; others (e.g. SVG, WebP) are
  skipped without error.
- PDF output is a single, un-paginated page. A4/Letter pagination is a possible
  future extension.
