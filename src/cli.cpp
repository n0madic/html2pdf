#include "cli.h"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>

namespace html2pdf {

namespace {

// Lowercase ASCII copy, used for case-insensitive extension matching.
std::string to_lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Strictly parse an integral option value: the whole string must be a number
// with no trailing garbage, and the value must fit the target type. Unlike
// atoi/atol this rejects "50abc", "" and out-of-range values instead of
// silently truncating or invoking undefined behaviour.
template <typename T>
bool parse_number(const std::string& s, T& out) {
    if (s.empty()) return false;
    const char* begin = s.data();
    const char* end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc() && ptr == end;
}

// Strictly parse a floating-point value: the whole string must be consumed and
// the result must be finite. std::from_chars<double> is unavailable on some
// libstdc++/libc++ versions, so use strtod and check the end pointer ourselves.
bool parse_double(const std::string& s, double& out) {
    if (s.empty()) return false;
    const char* begin = s.c_str();
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(begin, &end);
    if (end != begin + s.size()) return false;  // trailing garbage
    if (!std::isfinite(v)) return false;
    out = v;
    return true;
}

// Resolve a paper-size spec to width/height in PDF points (portrait), then swap
// for landscape. The spec is either a preset name (case-insensitive) or a
// "WxH[unit]" custom size where unit is one of mm|cm|in|pt (default mm).
// Returns false on an unknown preset, malformed custom size, or non-positive
// dimensions.
bool resolve_page_size(const std::string& spec, bool landscape, double& w_pt, double& h_pt) {
    // Preset table, portrait orientation, in points (1pt = 1/72 inch).
    struct Preset { const char* name; double w; double h; };
    static const Preset kPresets[] = {
        {"a3",     841.89, 1190.55},
        {"a4",     595.28,  841.89},
        {"a5",     419.53,  595.28},
        {"letter", 612.0,   792.0},
        {"legal",  612.0,  1008.0},
    };

    double w = 0.0, h = 0.0;
    const std::string lower = to_lower(spec);
    bool resolved = false;
    for (const Preset& p : kPresets) {
        if (lower == p.name) {
            w = p.w;
            h = p.h;
            resolved = true;
            break;
        }
    }

    if (!resolved) {
        // Custom "WxH[unit]" form. Split on the first 'x'.
        const auto xpos = lower.find('x');
        if (xpos == std::string::npos || xpos == 0 || xpos + 1 >= lower.size()) return false;
        const std::string w_str = lower.substr(0, xpos);
        std::string h_str = lower.substr(xpos + 1);

        // Trailing unit on the height part: mm|cm|in|pt (default mm).
        double factor = 72.0 / 25.4;  // mm -> pt
        const std::pair<const char*, double> units[] = {
            {"mm", 72.0 / 25.4}, {"cm", 72.0 / 2.54}, {"in", 72.0}, {"pt", 1.0},
        };
        for (const auto& u : units) {
            if (ends_with(h_str, u.first)) {
                factor = u.second;
                h_str.erase(h_str.size() - 2);
                break;
            }
        }

        if (!parse_double(w_str, w) || !parse_double(h_str, h)) return false;
        w *= factor;
        h *= factor;
    }

    if (!(w > 0.0) || !(h > 0.0)) return false;
    if (landscape) std::swap(w, h);
    w_pt = w;
    h_pt = h;
    return true;
}

// Pull the value for an option that expects an argument. Supports both
// "--opt value" and "--opt=value" forms. Returns false on a missing value.
bool take_value(int argc, char** argv, int& i, const std::string& inline_val,
                bool has_inline, const char* name, std::string& out) {
    if (has_inline) {
        out = inline_val;
        return true;
    }
    if (i + 1 >= argc) {
        std::cerr << "error: option " << name << " requires a value\n";
        return false;
    }
    out = argv[++i];
    return true;
}

}  // namespace

void print_usage(const char* prog) {
    std::cout <<
        "Usage: " << prog << " (--input FILE | --url URL) --output FILE [options]\n"
        "\n"
        "Render an HTML document to PNG (raster) or PDF (vector).\n"
        "The output format is selected by the --output file extension.\n"
        "\n"
        "Required (exactly one source):\n"
        "  --input FILE        Render a local HTML file\n"
        "  --url URL           Fetch and render an HTTP(S) URL\n"
        "Required:\n"
        "  --output FILE       Output path; extension .png or .pdf picks the format\n"
        "\n"
        "Options:\n"
        "  --width N           Render width in pixels (default: 1024)\n"
        "  --height N          Render height in pixels (default: 0 = auto)\n"
        "  --page-size SIZE    Paginate PDF output to this paper size; SIZE is a\n"
        "                      preset (A3, A4, A5, Letter, Legal) or a custom\n"
        "                      WxH[unit] (unit mm|cm|in|pt, default mm). Without\n"
        "                      this flag the PDF is a single page (the default).\n"
        "                      PDF output only; overrides --height when set.\n"
        "  --landscape         Rotate the --page-size paper to landscape\n"
        "  --margin MM         Page margin in millimetres (default: 10; needs --page-size)\n"
        "  --user-agent STR    HTTP User-Agent header (default: html2pdf/1.0)\n"
        "  --timeout N         HTTP timeout in seconds (default: 30)\n"
        "  --allow-local-network\n"
        "                      Allow fetching loopback/private/link-local hosts\n"
        "                      (disables the SSRF guard; off by default)\n"
        "  -h, --help          Show this help and exit\n"
        "\n"
        "Exit codes:\n"
        "  0 success   1 bad arguments   2 file I/O error\n"
        "  3 network error   4 render/graphics error\n"
        "\n"
#ifdef HTML2PDF_USE_PANGO
        "Text backend: Pango (shaping, BiDi, font fallback)\n";
#else
        "Text backend: Cairo toy API (no shaping/BiDi/fallback)\n";
#endif
}

ParseResult parse_args(int argc, char** argv) {
    ParseResult result;
    Options& opts = result.options;

    const char* prog = (argc > 0 && argv[0]) ? argv[0] : "html2pdf";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_usage(prog);
            result.status = ParseResult::Status::HelpShown;
            return result;
        }

        // Split "--opt=value" into name + inline value.
        std::string name = arg;
        std::string inline_val;
        bool has_inline = false;
        if (arg.size() > 2 && arg[0] == '-' && arg[1] == '-') {
            auto eq = arg.find('=');
            if (eq != std::string::npos) {
                name = arg.substr(0, eq);
                inline_val = arg.substr(eq + 1);
                has_inline = true;
            }
        }

        std::string value;
        // Pull the value for the option currently being parsed (`name`), so the
        // option literal is not repeated at each call site.
        auto need = [&]() {
            return take_value(argc, argv, i, inline_val, has_inline, name.c_str(), value);
        };

        if (name == "--input") {
            if (!need()) return result;
            opts.input = value;
        } else if (name == "--url") {
            if (!need()) return result;
            opts.url = value;
        } else if (name == "--output") {
            if (!need()) return result;
            opts.output = value;
        } else if (name == "--width") {
            if (!need()) return result;
            if (!parse_number(value, opts.width)) {
                std::cerr << "error: --width must be an integer: " << value << "\n";
                return result;
            }
        } else if (name == "--height") {
            if (!need()) return result;
            if (!parse_number(value, opts.height)) {
                std::cerr << "error: --height must be an integer: " << value << "\n";
                return result;
            }
        } else if (name == "--page-size") {
            if (!need()) return result;
            opts.page_size = value;
        } else if (name == "--landscape") {
            // Boolean flag: rotate the paper size (only with --page-size).
            opts.landscape = true;
        } else if (name == "--margin") {
            if (!need()) return result;
            if (!parse_double(value, opts.margin_mm)) {
                std::cerr << "error: --margin must be a number (mm): " << value << "\n";
                return result;
            }
        } else if (name == "--user-agent") {
            if (!need()) return result;
            opts.user_agent = value;
        } else if (name == "--timeout") {
            if (!need()) return result;
            if (!parse_number(value, opts.timeout_sec)) {
                std::cerr << "error: --timeout must be an integer: " << value << "\n";
                return result;
            }
        } else if (name == "--allow-local-network") {
            // Boolean flag: disables the SSRF guard for loopback/private hosts.
            opts.allow_local_network = true;
        } else {
            std::cerr << "error: unknown argument: " << arg << "\n";
            return result;
        }
    }

    // Validation: exactly one source.
    const bool have_input = !opts.input.empty();
    const bool have_url = !opts.url.empty();
    if (have_input && have_url) {
        std::cerr << "error: --input and --url are mutually exclusive\n";
        return result;
    }
    if (!have_input && !have_url) {
        std::cerr << "error: one of --input or --url is required\n";
        return result;
    }

    // Validation: output present.
    if (opts.output.empty()) {
        std::cerr << "error: --output is required\n";
        return result;
    }

    // Validation: output extension picks the format.
    const std::string lower_out = to_lower(opts.output);
    if (ends_with(lower_out, ".png")) {
        opts.format = Format::PNG;
    } else if (ends_with(lower_out, ".pdf")) {
        opts.format = Format::PDF;
    } else {
        std::cerr << "error: --output must end with .png or .pdf\n";
        return result;
    }

    // Validation: numeric sanity. The defaults (width 1024, height 0, timeout
    // 30) all satisfy these checks, so they can be applied unconditionally.
    if (opts.width <= 0) {
        std::cerr << "error: --width must be a positive integer\n";
        return result;
    }
    if (opts.height < 0) {
        std::cerr << "error: --height must be zero or a positive integer\n";
        return result;
    }
    if (opts.timeout_sec <= 0) {
        std::cerr << "error: --timeout must be a positive integer\n";
        return result;
    }

    // Validation: pagination. --page-size enables it; --landscape/--margin only
    // take effect alongside it. --height is ignored in paged mode (height is
    // determined by the paper size and page breaks).
    if (!opts.page_size.empty()) {
        if (opts.format != Format::PDF) {
            std::cerr << "error: --page-size requires PDF output\n";
            return result;
        }
        if (!resolve_page_size(opts.page_size, opts.landscape, opts.page_w_pt, opts.page_h_pt)) {
            std::cerr << "error: invalid --page-size: " << opts.page_size << "\n";
            return result;
        }
        if (opts.margin_mm < 0.0) {
            std::cerr << "error: --margin must be zero or positive\n";
            return result;
        }
        opts.margin_pt = opts.margin_mm * 72.0 / 25.4;
        // Margins must leave a positive printable area on both axes.
        if (2.0 * opts.margin_pt >= opts.page_w_pt || 2.0 * opts.margin_pt >= opts.page_h_pt) {
            std::cerr << "error: --margin too large for the page size\n";
            return result;
        }
    }

    result.status = ParseResult::Status::Ok;
    return result;
}

}  // namespace html2pdf
