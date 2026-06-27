#!/usr/bin/env bash
#
# End-to-end tests for html2pdf. Verifies exit codes and output file magic
# bytes for the four spec test cases. The HTTP scenario is optional and runs
# only when python3 is available.
#
# Usage: tests/run_tests.sh [path-to-html2pdf-binary]

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FIX="$SCRIPT_DIR/fixtures"

BIN="${1:-$ROOT_DIR/build/html2pdf}"
OUT="$SCRIPT_DIR/output"

if [[ ! -x "$BIN" ]]; then
    echo "error: binary not found or not executable: $BIN" >&2
    echo "build first: cmake -S . -B build && cmake --build build -j" >&2
    exit 1
fi

mkdir -p "$OUT"
rm -f "$OUT"/*.png "$OUT"/*.pdf "$OUT"/*.html "$OUT"/*.log "$OUT"/redir_server.py 2>/dev/null
rm -rf "$OUT/rsrv" 2>/dev/null

PASS=0
FAIL=0

ok()   { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

# expect_code <expected> <actual> <desc>
expect_code() {
    if [[ "$1" == "$2" ]]; then ok "$3 (exit $2)"; else bad "$3 (expected exit $1, got $2)"; fi
}

# Check that a file exists, is non-empty and starts with the given magic bytes.
# expect_magic <file> <magic-string> <desc>
expect_magic() {
    local file="$1" magic="$2" desc="$3"
    if [[ ! -s "$file" ]]; then bad "$desc (missing or empty: $file)"; return; fi
    local head
    head="$(head -c "${#magic}" "$file")"
    if [[ "$head" == "$magic" ]]; then ok "$desc"; else bad "$desc (bad magic in $file)"; fi
}

# Echo the pixel height of a PNG via file(1), or nothing if unavailable.
png_height() {
    command -v file >/dev/null 2>&1 || return 0
    file -b "$1" | sed -nE 's/.*[0-9]+ x ([0-9]+).*/\1/p'
}

# Assert a file contains (expect_grep) / does not contain (expect_no_grep) a
# pattern. Used to verify which URLs the renderer actually requested.
expect_grep() {
    local file="$1" pat="$2" desc="$3"
    if grep -qE "$pat" "$file" 2>/dev/null; then ok "$desc"; else bad "$desc (no match: $pat)"; fi
}
expect_no_grep() {
    local file="$1" pat="$2" desc="$3"
    if grep -qE "$pat" "$file" 2>/dev/null; then bad "$desc (unexpected match: $pat)"; else ok "$desc"; fi
}

echo "html2pdf test suite"
echo "binary: $BIN"
echo

# ----------------------------------------------------------------------------
echo "[1] simple local HTML -> PNG and PDF"
"$BIN" --input "$FIX/simple.html" --output "$OUT/simple.png" >/dev/null 2>&1
expect_code 0 $? "simple -> png"
expect_magic "$OUT/simple.png" $'\x89PNG' "simple.png magic"

"$BIN" --input "$FIX/simple.html" --output "$OUT/simple.pdf" >/dev/null 2>&1
expect_code 0 $? "simple -> pdf"
expect_magic "$OUT/simple.pdf" '%PDF' "simple.pdf magic"

# ----------------------------------------------------------------------------
echo "[2] external CSS + image (local relative resolution)"
"$BIN" --input "$FIX/with_css.html" --output "$OUT/with_css.png" >/dev/null 2>&1
expect_code 0 $? "with_css (file) -> png"
expect_magic "$OUT/with_css.png" $'\x89PNG' "with_css.png magic"

echo "[2b] graceful degradation: unreachable resources must not fail the render"
MISS="$OUT/missing.html"
cat > "$MISS" <<'EOF'
<html><head><link rel="stylesheet" href="http://127.0.0.1:1/nope.css"></head>
<body><h1>missing</h1><img src="http://127.0.0.1:1/missing.png"></body></html>
EOF
"$BIN" --input "$MISS" --output "$OUT/missing.png" >/dev/null 2>&1
expect_code 0 $? "unreachable resources -> still renders"

# ----------------------------------------------------------------------------
echo "[2c] HTTP fetch, URL resolution and SSRF guard (optional, needs python3)"
if command -v python3 >/dev/null 2>&1; then
    PORT=8731
    LOG="$OUT/http_access.log"
    : > "$LOG"
    # Server logs each request line to stderr -> $LOG, so we can verify which
    # URLs the renderer actually resolved and requested. Run python directly (no
    # subshell) so $! is the server PID and kill+wait flushes its log on exit.
    python3 -m http.server "$PORT" --directory "$FIX" >/dev/null 2>>"$LOG" &
    SVPID=$!
    sleep 1

    # Loopback is blocked by the SSRF guard unless --allow-local-network is set.
    "$BIN" --url "http://127.0.0.1:$PORT/with_css.html" --output "$OUT/with_css_http.png" \
        --timeout 10 --allow-local-network >/dev/null 2>&1
    expect_code 0 $? "with_css (http) -> png"
    expect_magic "$OUT/with_css_http.png" $'\x89PNG' "with_css_http.png magic"

    # [2d] per-resource base: bg.png and the @import inside sub/assets/sheet.css
    # must resolve against the stylesheet's location, not the HTML's directory.
    "$BIN" --url "http://127.0.0.1:$PORT/sub/index.html" --output "$OUT/sub_http.png" \
        --timeout 10 --allow-local-network >/dev/null 2>&1
    expect_code 0 $? "per-resource CSS base (http) -> png"

    # [2e] relative <base href> on an HTTP page must stay in HTTP mode and join
    # the origin, not flip the loader into filesystem mode.
    "$BIN" --url "http://127.0.0.1:$PORT/baseref/page.html" --output "$OUT/baseref_http.png" \
        --timeout 10 --allow-local-network >/dev/null 2>&1
    expect_code 0 $? "relative <base href> (http) -> png"

    # [2g] SSRF guard: loopback is blocked by default (no --allow-local-network).
    "$BIN" --url "http://127.0.0.1:$PORT/with_css.html" --output "$OUT/ssrf.png" \
        --timeout 10 >/dev/null 2>&1
    expect_code 3 $? "SSRF: loopback blocked by default -> code 3"

    # Stop the server first: its access log is only flushed to the file on exit,
    # so the URL-resolution assertions below must run after kill+wait.
    kill "$SVPID" >/dev/null 2>&1
    wait "$SVPID" 2>/dev/null

    expect_grep "$LOG" 'GET /sub/assets/bg.png ' "background resolved against stylesheet dir"
    expect_grep "$LOG" 'GET /sub/assets/nested.css ' "@import resolved against stylesheet dir"
    expect_no_grep "$LOG" 'GET /sub/bg.png ' "background NOT resolved against HTML dir"
    expect_grep "$LOG" 'GET /baseref/static/pic.png ' "relative base href joined onto origin"

    # [2f] base updated after an HTTP redirect: relative sub-resources resolve
    # against the FINAL location, not the original URL.
    RPORT=8732
    RLOG="$OUT/redir_access.log"
    : > "$RLOG"
    RSRV="$OUT/redir_server.py"
    cat > "$RSRV" <<PY
import http.server, socketserver, sys
PORT = int(sys.argv[1]); ROOT = sys.argv[2]
class Server(socketserver.TCPServer):
    allow_reuse_address = True  # tolerate TIME_WAIT across back-to-back runs
class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k): super().__init__(*a, directory=ROOT, **k)
    def do_GET(self):
        if self.path == "/old/page.html":
            self.send_response(301); self.send_header("Location", "/new/page.html"); self.end_headers(); return
        return super().do_GET()
with Server(("127.0.0.1", PORT), H) as h: h.serve_forever()
PY
    mkdir -p "$OUT/rsrv/new"
    printf '<!DOCTYPE html><html><body><img src="after.png"></body></html>' > "$OUT/rsrv/new/page.html"
    cp "$FIX/img.png" "$OUT/rsrv/new/after.png"
    python3 "$RSRV" "$RPORT" "$OUT/rsrv" >/dev/null 2>>"$RLOG" &
    RPID=$!
    sleep 1
    "$BIN" --url "http://127.0.0.1:$RPORT/old/page.html" --output "$OUT/redir.png" \
        --timeout 10 --allow-local-network >/dev/null 2>&1
    expect_code 0 $? "post-redirect render -> png"
    expect_grep "$RLOG" 'GET /new/after.png ' "relative sub-resource resolved against final URL"
    kill "$RPID" >/dev/null 2>&1
    wait "$RPID" 2>/dev/null
else
    echo "  SKIP: python3 not available"
fi

# ----------------------------------------------------------------------------
echo "[3] tall page -> output height tracks content height (no clipping)"
"$BIN" --input "$FIX/tall.html" --output "$OUT/tall.png" >/dev/null 2>&1
expect_code 0 $? "tall -> png"
expect_magic "$OUT/tall.png" $'\x89PNG' "tall.png magic"
# A 25-row page must be much taller than the default viewport height.
if command -v file >/dev/null 2>&1; then
    DIMS="$(file -b "$OUT/tall.png")"
    HEIGHT="$(echo "$DIMS" | sed -nE 's/.*[0-9]+ x ([0-9]+).*/\1/p')"
    if [[ -n "$HEIGHT" && "$HEIGHT" -gt 768 ]]; then
        ok "tall.png height ($HEIGHT) > default viewport (768)"
    else
        bad "tall.png height not greater than viewport ($DIMS)"
    fi
fi

# ----------------------------------------------------------------------------
echo "[3b] Unicode / shaping / BiDi smoke test (backend-agnostic)"
# Renders to a valid file under both backends; correct shaping of CJK/RTL is a
# manual visual check and only meaningful with the Pango backend.
"$BIN" --input "$FIX/unicode.html" --output "$OUT/unicode.png" >/dev/null 2>&1
expect_code 0 $? "unicode -> png"
expect_magic "$OUT/unicode.png" $'\x89PNG' "unicode.png magic"

# ----------------------------------------------------------------------------
echo "[4] error handling and exit codes"
"$BIN" --input "$FIX/does_not_exist.html" --output "$OUT/x.png" >/dev/null 2>&1
expect_code 2 $? "missing input file -> code 2"

"$BIN" --url "http://nonexistent.invalid.localhost.example/" --output "$OUT/x.png" \
    --timeout 5 >/dev/null 2>&1
expect_code 3 $? "unresolvable host -> code 3"

"$BIN" --input "$FIX/simple.html" --url "http://x/" --output "$OUT/x.png" >/dev/null 2>&1
expect_code 1 $? "input + url conflict -> code 1"

"$BIN" --input "$FIX/simple.html" >/dev/null 2>&1
expect_code 1 $? "missing --output -> code 1"

"$BIN" --input "$FIX/simple.html" --output "$OUT/x.txt" >/dev/null 2>&1
expect_code 1 $? "bad output extension -> code 1"

# ----------------------------------------------------------------------------
echo "[5] edge case: empty document still produces a valid file (height >= 1)"
printf '' > "$OUT/empty.html"
"$BIN" --input "$OUT/empty.html" --output "$OUT/empty.png" >/dev/null 2>&1
expect_code 0 $? "empty document -> png"
expect_magic "$OUT/empty.png" $'\x89PNG' "empty.png magic"

# ----------------------------------------------------------------------------
echo "[6] security and robustness"

# Scheme restriction: only http/https URLs are accepted for --url.
"$BIN" --url "file:///etc/hostname" --output "$OUT/x.png" >/dev/null 2>&1
expect_code 3 $? "file:// URL rejected -> code 3"
"$BIN" --url "ftp://example.com/x" --output "$OUT/x.png" --timeout 5 >/dev/null 2>&1
expect_code 3 $? "ftp:// URL rejected -> code 3"

# A directory (openable but not a regular file) is a file I/O error, not a
# silent empty success.
"$BIN" --input "$FIX" --output "$OUT/x.png" >/dev/null 2>&1
expect_code 2 $? "directory as input -> code 2"

# Strict numeric parsing: trailing garbage, overflow and out-of-range rejected.
"$BIN" --input "$FIX/simple.html" --output "$OUT/x.png" --width 50abc >/dev/null 2>&1
expect_code 1 $? "--width 50abc rejected -> code 1"
"$BIN" --input "$FIX/simple.html" --output "$OUT/x.png" --width 99999999999 >/dev/null 2>&1
expect_code 1 $? "--width overflow rejected -> code 1"
"$BIN" --input "$FIX/simple.html" --output "$OUT/x.png" --height abc >/dev/null 2>&1
expect_code 1 $? "--height abc rejected -> code 1"

# Output write failure is a file I/O error (code 2), not a graphics error.
"$BIN" --input "$FIX/simple.html" --output "/no/such/dir/out.png" >/dev/null 2>&1
expect_code 2 $? "unwritable PNG destination -> code 2"
"$BIN" --input "$FIX/simple.html" --output "/no/such/dir/out.pdf" >/dev/null 2>&1
expect_code 2 $? "unwritable PDF destination -> code 2"

# Oversized auto-height is clamped to the Cairo limit instead of failing.
printf '<!DOCTYPE html><html><body><div style="height:40000px">tall</div></body></html>' \
    > "$OUT/huge.html"
"$BIN" --input "$OUT/huge.html" --output "$OUT/huge.png" >/dev/null 2>&1
expect_code 0 $? "oversized auto-height -> still renders"
HH="$(png_height "$OUT/huge.png")"
if [[ -n "$HH" ]]; then
    if [[ "$HH" -le 32767 && "$HH" -gt 768 ]]; then
        ok "huge.png height ($HH) clamped to Cairo limit (<= 32767)"
    else
        bad "huge.png height not clamped as expected ($HH)"
    fi
fi

# Filesystem containment: a stylesheet referenced via "../" or an absolute path
# escapes the document directory and must NOT be loaded (page stays short),
# while an in-tree stylesheet is applied (page becomes tall).
"$BIN" --input "$FIX/trav/doc/escape.html" --output "$OUT/escape.png" >/dev/null 2>&1
expect_code 0 $? "path-traversal references -> still renders"
EH="$(png_height "$OUT/escape.png")"
"$BIN" --input "$FIX/trav/doc/inside.html" --output "$OUT/inside.png" >/dev/null 2>&1
IH="$(png_height "$OUT/inside.png")"
if [[ -n "$EH" && -n "$IH" ]]; then
    if [[ "$EH" -lt 2000 ]]; then
        ok "escaping stylesheet blocked (height $EH stays small)"
    else
        bad "escaping stylesheet appears to have loaded (height $EH)"
    fi
    if [[ "$IH" -gt 5000 ]]; then
        ok "in-tree stylesheet applied (height $IH)"
    else
        bad "in-tree stylesheet not applied (height $IH)"
    fi
fi

echo
echo "----------------------------------------"
echo "PASS: $PASS   FAIL: $FAIL"
[[ "$FAIL" -eq 0 ]] && exit 0 || exit 1
