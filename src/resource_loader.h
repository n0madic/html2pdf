#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace html2pdf {

// Result of a single resource fetch (file or HTTP).
struct FetchResult {
    bool ok = false;
    std::vector<unsigned char> data;
    std::string error;
    // Final URL after following redirects (HTTP only); empty otherwise.
    std::string effective_url;
};

// Unified loader for the main document and for sub-resources (CSS, images),
// from the local filesystem or over HTTP(S). Relative references are resolved
// against a base set with set_base() (or an explicit per-reference base).
// Sub-resources may also be inline data: URIs (RFC 2397), decoded in-process
// with no network or filesystem access.
//
// Security:
//   - Only http/https URLs are accepted (file:, ftp:, gopher:, ... are rejected);
//     inline data: URIs are decoded locally and touch neither the network nor disk.
//   - HTTP fetches are guarded against SSRF: connections to loopback, private,
//     link-local (incl. cloud metadata 169.254.169.254) and other non-public
//     addresses are blocked unless allow_local_network is set.
//   - Filesystem sub-resources are contained within the directory of the main
//     input document; absolute paths and "../" escapes are rejected.
class ResourceLoader {
public:
    ResourceLoader(std::string user_agent, long timeout_sec,
                   bool allow_local_network = false);
    ~ResourceLoader();

    // Set the document base location. Whether it is an HTTP base is derived from
    // the value itself (http:// or https://). For files this is the path of the
    // main input; for HTTP this is the absolute URL of the main document.
    void set_base(const std::string& base);
    bool base_is_http() const { return base_is_http_; }

    // Load the main document. is_url selects HTTP vs filesystem and also
    // initialises the base (and, for files, the containment root).
    FetchResult load_main(const std::string& src, bool is_url);

    // Resolve a (possibly relative) reference against `base` (empty -> the
    // document base) into an absolute URL/path, WITHOUT loading it. Returns ""
    // when the reference cannot be resolved or is blocked (filesystem escape,
    // unsupported scheme).
    std::string resolve(const std::string& ref, const std::string& base = "");

    // Load an already-resolved absolute URL/path. Does no further resolution; an
    // empty target yields an error result. Pass use_cache=true to memoize the
    // result by target: CSS is requested twice (the web-font pre-scan and
    // litehtml's import_css both fetch every @import), so caching it avoids a
    // duplicate network round-trip. Images and fonts are fetched at most once
    // elsewhere and use the default (false) so their bytes are not retained.
    FetchResult load_resolved(const std::string& target, bool use_cache = false);

private:
    FetchResult load_file(const std::string& path);
    FetchResult load_http(const std::string& url);

    std::string user_agent_;
    long timeout_sec_;
    bool allow_local_network_;
    std::string base_;
    bool base_is_http_ = false;
    // Canonical directory of the main input file; filesystem sub-resources are
    // contained within it. Empty in HTTP mode.
    std::string fs_root_;
    // Response cache for resources fetched more than once (CSS via @import, seen
    // by both the web-font pre-scan and litehtml's import_css). Keyed on the
    // resolved absolute URL/path; only populated when load_resolved(..., true).
    std::unordered_map<std::string, FetchResult> response_cache_;
};

}  // namespace html2pdf
