#include "resource_loader.h"

#include "base64.hpp"

#include <curl/curl.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace html2pdf {

namespace {

namespace fs = std::filesystem;

// Hard cap on a single HTTP response held in memory, to bound worst-case
// memory use against a server that streams unbounded data.
constexpr size_t kMaxResponseBytes = 64u * 1024u * 1024u;  // 64 MiB

std::once_flag g_curl_once;

void ensure_curl_global() {
    std::call_once(g_curl_once, [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        // Pair the one-time global init with a matching teardown at process
        // exit, so libcurl's global allocations are released cleanly.
        std::atexit(curl_global_cleanup);
    });
}

// True if the string starts with "scheme://".
bool has_scheme(const std::string& s) {
    auto pos = s.find("://");
    if (pos == std::string::npos || pos == 0) return false;
    for (size_t i = 0; i < pos; ++i) {
        char c = s[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

bool is_http_url(const std::string& s) {
    return s.compare(0, 7, "http://") == 0 || s.compare(0, 8, "https://") == 0;
}

// True if `s` begins with the case-insensitive "data:" scheme (RFC 2397).
bool is_data_uri(const std::string& s) {
    if (s.size() < 5) return false;
    static const char kData[5] = {'d', 'a', 't', 'a', ':'};
    for (int i = 0; i < 5; ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) != kData[i]) return false;
    }
    return true;
}

// Decode RFC 4648 base64 from `in` into `out`, tolerating embedded ASCII
// whitespace and omitted '=' padding. Returns false on an invalid character,
// data after padding, or an impossible 4n+1 length.
//
// The actual decode is delegated to base64::decode_into (third_party/base64.hpp),
// which is strict: it rejects whitespace and requires a length that is a multiple
// of 4. We first normalise the input to satisfy those preconditions while keeping
// the lenient contract browsers expect from data: URIs.
bool base64_decode(const std::string& in, std::vector<unsigned char>& out) {
    std::string cleaned;
    cleaned.reserve(in.size());
    for (unsigned char c : in) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') continue;
        cleaned.push_back(static_cast<char>(c));
    }
    // A 4n+1 length is impossible for valid base64.
    if ((cleaned.size() & 3u) == 1u) return false;
    // Restore any omitted '=' padding so the length is a multiple of 4.
    while ((cleaned.size() & 3u) != 0u) cleaned.push_back('=');
    try {
        out = base64::decode_into<std::vector<unsigned char>>(cleaned);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

// Decode percent-encoding (%XX) from `in` into `out`. A stray or truncated '%'
// is emitted literally, matching lenient browser handling of data: URIs.
void percent_decode(const std::string& in, std::vector<unsigned char>& out) {
    auto nibble = [](unsigned char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            const int hi = nibble(static_cast<unsigned char>(in[i + 1]));
            const int lo = nibble(static_cast<unsigned char>(in[i + 2]));
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<unsigned char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(static_cast<unsigned char>(in[i]));
    }
}

// Decode a data: URI (data:[<mediatype>][;base64],<data>) into its raw bytes.
// The media type is ignored: stb_image sniffs image formats by content and CSS
// is consumed as text, so only the payload matters. A data: URI is
// self-contained -> no network or filesystem access, so neither the SSRF guard
// nor filesystem containment applies.
FetchResult load_data_uri(const std::string& uri) {
    FetchResult r;
    const size_t comma = uri.find(',');
    if (comma == std::string::npos) {
        r.error = "malformed data: URI (missing comma)";
        return r;
    }
    // Metadata sits between "data:" (5 chars) and the comma; the payload is
    // base64 when it ends with ";base64" (case-insensitive, trailing spaces
    // ignored), otherwise it is percent-encoded text.
    std::string meta = uri.substr(5, comma - 5);
    const size_t last = meta.find_last_not_of(" \t");
    meta.resize(last == std::string::npos ? 0 : last + 1);

    bool base64 = false;
    static const std::string kTag = ";base64";
    if (meta.size() >= kTag.size()) {
        base64 = true;
        const size_t off = meta.size() - kTag.size();
        for (size_t i = 0; i < kTag.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(meta[off + i])) != kTag[i]) {
                base64 = false;
                break;
            }
        }
    }

    const std::string payload = uri.substr(comma + 1);
    if (base64) {
        if (!base64_decode(payload, r.data)) {
            r.data.clear();
            r.error = "invalid base64 in data: URI";
            return r;
        }
    } else {
        percent_decode(payload, r.data);
    }
    r.ok = true;
    return r;
}

// Directory part of a filesystem path, including the trailing separator.
// Returns "" when the path has no directory component.
std::string dir_of(const std::string& path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) return "";
    return path.substr(0, pos + 1);
}

// True if `path` resolves to a location inside `dir` (lexical + symlink-aware
// containment). Used to keep filesystem sub-resources within the document tree.
bool path_within(const std::string& dir, const std::string& path) {
    if (dir.empty()) return true;  // no root configured -> no containment
    std::error_code ec;
    fs::path base = fs::weakly_canonical(fs::absolute(dir, ec), ec);
    if (ec) return false;
    fs::path cand = fs::weakly_canonical(fs::absolute(path, ec), ec);
    if (ec) return false;
    fs::path rel = fs::relative(cand, base, ec);
    if (ec || rel.empty()) return false;
    auto it = rel.begin();
    return it != rel.end() && *it != "..";
}

// --- SSRF address classification ------------------------------------------

// IPv4 address (host byte order) that must not be reached from a URL fetch.
bool ipv4_blocked(uint32_t a) {
    auto in = [a](uint32_t net, int bits) {
        const uint32_t mask = bits == 0 ? 0u : (0xFFFFFFFFu << (32 - bits));
        return (a & mask) == (net & mask);
    };
    return in(0x00000000u, 8) ||   // 0.0.0.0/8       "this host"
           in(0x0A000000u, 8) ||   // 10.0.0.0/8      private
           in(0x64400000u, 10) ||  // 100.64.0.0/10   CGNAT
           in(0x7F000000u, 8) ||   // 127.0.0.0/8     loopback
           in(0xA9FE0000u, 16) ||  // 169.254.0.0/16  link-local (cloud metadata)
           in(0xAC100000u, 12) ||  // 172.16.0.0/12   private
           in(0xC0000000u, 24) ||  // 192.0.0.0/24    IETF protocol assignments
           in(0xC0000200u, 24) ||  // 192.0.2.0/24    TEST-NET-1
           in(0xC0A80000u, 16) ||  // 192.168.0.0/16  private
           in(0xC6120000u, 15) ||  // 198.18.0.0/15   benchmarking
           in(0xC6336400u, 24) ||  // 198.51.100.0/24 TEST-NET-2
           in(0xCB007100u, 24) ||  // 203.0.113.0/24  TEST-NET-3
           in(0xE0000000u, 4) ||   // 224.0.0.0/4     multicast
           in(0xF0000000u, 4);     // 240.0.0.0/4     reserved / broadcast
}

// IPv6 address (16 bytes, network order) that must not be reached.
bool ipv6_blocked(const uint8_t b[16]) {
    bool all_zero = true;
    for (int i = 0; i < 16; ++i) {
        if (b[i] != 0) { all_zero = false; break; }
    }
    if (all_zero) return true;  // :: unspecified
    bool loopback = b[15] == 1;
    for (int i = 0; i < 15 && loopback; ++i) {
        if (b[i] != 0) loopback = false;
    }
    if (loopback) return true;  // ::1
    // IPv4-mapped (::ffff:a.b.c.d) and IPv4-compatible (::a.b.c.d): classify the
    // embedded IPv4 address.
    bool prefix96_zero = true;
    for (int i = 0; i < 10; ++i) {
        if (b[i] != 0) { prefix96_zero = false; break; }
    }
    if (prefix96_zero && ((b[10] == 0xFF && b[11] == 0xFF) || (b[10] == 0 && b[11] == 0))) {
        uint32_t v4 = (static_cast<uint32_t>(b[12]) << 24) |
                      (static_cast<uint32_t>(b[13]) << 16) |
                      (static_cast<uint32_t>(b[14]) << 8) | b[15];
        return ipv4_blocked(v4);
    }
    if ((b[0] & 0xFE) == 0xFC) return true;                     // fc00::/7  ULA
    if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return true;     // fe80::/10 link-local
    if (b[0] == 0xFF) return true;                              // ff00::/8  multicast
    return false;
}

// True if the textual IP (as reported by libcurl for the connected peer) is a
// non-public address that an SSRF fetch must not reach. Unknown/unparseable
// addresses are blocked, failing safe.
bool is_blocked_ip(const char* ip) {
    if (!ip || !*ip) return true;
    in_addr v4{};
    if (inet_pton(AF_INET, ip, &v4) == 1) {
        return ipv4_blocked(ntohl(v4.s_addr));
    }
    in6_addr v6{};
    if (inet_pton(AF_INET6, ip, &v6) == 1) {
        return ipv6_blocked(v6.s6_addr);
    }
    return true;
}

// CURLOPT_PREREQFUNCTION / CURLOPT_*PROTOCOLS_STR are enum values, not
// preprocessor macros, so they must be gated on the library version, not with
// defined(). PREREQFUNCTION: 7.80.0; PROTOCOLS_STR: 7.85.0.
#define H2PDF_CURL_HAS_PREREQ (LIBCURL_VERSION_NUM >= 0x075000)
#define H2PDF_CURL_HAS_PROTOCOLS_STR (LIBCURL_VERSION_NUM >= 0x075500)

#if !H2PDF_CURL_HAS_PREREQ
#warning "libcurl < 7.80.0: SSRF connection guard unavailable; only the http/https scheme restriction is enforced"
#endif

#if H2PDF_CURL_HAS_PREREQ
// Called by libcurl after the connection is established but before the request
// is sent, once per connection (so redirects are checked too). Aborts the
// transfer when the resolved peer is a non-public address.
int ssrf_prereq(void* /*clientp*/, char* conn_primary_ip, char* /*conn_local_ip*/,
                int /*conn_primary_port*/, int /*conn_local_port*/) {
    return is_blocked_ip(conn_primary_ip) ? CURL_PREREQFUNC_ABORT : CURL_PREREQFUNC_OK;
}
#endif

// --- HTTP body sink with a size cap ---------------------------------------

struct WriteCtx {
    std::vector<unsigned char>* buf;
    size_t limit;
    bool exceeded;
};

size_t write_to_vector(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<WriteCtx*>(userdata);
    const size_t total = size * nmemb;
    // buf->size() never exceeds limit, so the subtraction does not underflow.
    if (total > ctx->limit - ctx->buf->size()) {
        ctx->exceeded = true;
        return 0;  // != total -> libcurl fails with CURLE_WRITE_ERROR
    }
    ctx->buf->insert(ctx->buf->end(), reinterpret_cast<unsigned char*>(ptr),
                     reinterpret_cast<unsigned char*>(ptr) + total);
    return total;
}

}  // namespace

ResourceLoader::ResourceLoader(std::string user_agent, long timeout_sec,
                               bool allow_local_network)
    : user_agent_(std::move(user_agent)),
      timeout_sec_(timeout_sec),
      allow_local_network_(allow_local_network) {
    ensure_curl_global();
}

ResourceLoader::~ResourceLoader() = default;

void ResourceLoader::set_base(const std::string& base) {
    base_ = base;
    base_is_http_ = is_http_url(base);
}

FetchResult ResourceLoader::load_main(const std::string& src, bool is_url) {
    if (is_url) {
        // Only http(s) is accepted for the main URL; file:, ftp:, gopher:,
        // dict:, ... are rejected to prevent local-file disclosure and
        // protocol smuggling.
        if (!is_http_url(src)) {
            FetchResult r;
            r.error = "unsupported URL scheme (only http/https supported): " + src;
            return r;
        }
        set_base(src);
        FetchResult r = load_http(src);
        // After redirects, resolve sub-resources against the final location.
        if (r.ok && !r.effective_url.empty()) set_base(r.effective_url);
        return r;
    }

    set_base(src);
    // Containment root: the canonical directory of the input file.
    std::error_code ec;
    fs::path dir = fs::path(src).parent_path();
    if (dir.empty()) dir = fs::path(".");
    fs::path canon = fs::weakly_canonical(fs::absolute(dir, ec), ec);
    fs_root_ = ec ? dir.string() : canon.string();
    return load_file(src);
}

std::string ResourceLoader::resolve(const std::string& ref, const std::string& base_in) {
    if (ref.empty()) return "";

    // data: URIs are self-contained (RFC 2397): pass them through unchanged so
    // load_resolved() can decode the inline payload. No base join applies.
    if (is_data_uri(ref)) return ref;

    // Absolute URL with an explicit scheme: only http(s) is usable here; other
    // schemes (file:, ftp:, gopher:, ...) are rejected.
    if (has_scheme(ref)) return is_http_url(ref) ? ref : "";

    std::string base;
    bool base_http;
    if (base_in.empty()) {
        base = base_;
        base_http = base_is_http_;
    } else {
        base = base_in;
        base_http = is_http_url(base_in);
    }

    if (base_http) {
        // Join the relative reference against the base URL via the CURLU API.
        // On any failure return "" rather than the raw reference, so the result
        // is never misrouted to the local filesystem.
        CURLU* h = curl_url();
        if (!h) return "";
        std::string result;
        if (curl_url_set(h, CURLUPART_URL, base.c_str(), 0) == CURLUE_OK &&
            curl_url_set(h, CURLUPART_URL, ref.c_str(), 0) == CURLUE_OK) {
            char* full = nullptr;
            if (curl_url_get(h, CURLUPART_URL, &full, 0) == CURLUE_OK && full) {
                result = full;
                curl_free(full);
            }
        }
        curl_url_cleanup(h);
        return result;
    }

    // Filesystem base: join, then contain within the document root. Absolute
    // paths and "../" escapes resolve outside the root and are rejected.
    std::string candidate = (ref[0] == '/') ? ref : dir_of(base) + ref;
    if (!path_within(fs_root_, candidate)) return "";
    return candidate;
}

FetchResult ResourceLoader::load_resolved(const std::string& target, bool use_cache) {
    if (target.empty()) {
        FetchResult r;
        r.error = "unresolved or blocked reference";
        return r;
    }
    if (use_cache) {
        const auto it = response_cache_.find(target);
        if (it != response_cache_.end()) return it->second;
    }

    FetchResult r;
    if (is_data_uri(target)) {
        r = load_data_uri(target);
    } else if (is_http_url(target)) {
        r = load_http(target);
    } else if (has_scheme(target)) {
        r.error = "unsupported URL scheme: " + target;
    } else {
        r = load_file(target);
    }

    if (use_cache) response_cache_.emplace(target, r);
    return r;
}

FetchResult ResourceLoader::load_file(const std::string& path) {
    FetchResult r;
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        r.error = "not a regular file: " + path;
        return r;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        r.error = "cannot open file: " + path;
        return r;
    }
    f.seekg(0, std::ios::end);
    std::streamoff len = f.tellg();
    f.seekg(0, std::ios::beg);
    if (len < 0) {
        // tellg() failed: the stream is not a normal seekable file.
        r.error = "cannot determine file size: " + path;
        return r;
    }
    if (len > 0) {
        r.data.resize(static_cast<size_t>(len));
        f.read(reinterpret_cast<char*>(r.data.data()), len);
        if (!f && !f.eof()) {
            r.error = "read error: " + path;
            return r;
        }
        r.data.resize(static_cast<size_t>(f.gcount()));
    }
    r.ok = true;
    return r;
}

FetchResult ResourceLoader::load_http(const std::string& url) {
    FetchResult r;
    CURL* curl = curl_easy_init();
    if (!curl) {
        r.error = "failed to initialise libcurl";
        return r;
    }

    WriteCtx ctx{&r.data, kMaxResponseBytes, false};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec_);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout_sec_);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent_.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_vector);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // allow gzip/deflate
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                     static_cast<curl_off_t>(kMaxResponseBytes));

    // Restrict to http/https for the initial request and for redirect targets,
    // so a redirect cannot smuggle file:, gopher:, dict:, ... schemes.
#if H2PDF_CURL_HAS_PROTOCOLS_STR
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif

    // SSRF guard: reject connections whose resolved peer is a non-public
    // address. The prereq callback runs per connection, so it covers redirects.
#if H2PDF_CURL_HAS_PREREQ
    if (!allow_local_network_) {
        curl_easy_setopt(curl, CURLOPT_PREREQFUNCTION, ssrf_prereq);
        curl_easy_setopt(curl, CURLOPT_PREREQDATA, nullptr);
    }
#endif

    const CURLcode code = curl_easy_perform(curl);
    if (code != CURLE_OK) {
        r.data.clear();
        if (ctx.exceeded) {
            r.error = "response exceeds size limit (" +
                      std::to_string(kMaxResponseBytes) + " bytes): " + url;
        } else if (!allow_local_network_ && code == CURLE_ABORTED_BY_CALLBACK) {
            r.error = "blocked non-public address (SSRF protection): " + url;
        } else {
            r.error = std::string("network error: ") + curl_easy_strerror(code);
        }
        curl_easy_cleanup(curl);
        return r;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    char* eff = nullptr;
    if (curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK && eff) {
        r.effective_url = eff;
    }
    curl_easy_cleanup(curl);

    // For non-HTTP(S) schemes http_code may be 0; treat that as success since
    // perform() already reported OK. For HTTP(S) require a 2xx status.
    if (http_code != 0 && (http_code < 200 || http_code >= 300)) {
        r.data.clear();
        r.error = "HTTP status " + std::to_string(http_code) + " for " + url;
        return r;
    }

    r.ok = true;
    return r;
}

}  // namespace html2pdf
