#include <cmath>
#include <iostream>
#include <string>

#include <litehtml.h>

#include "cli.h"
#include "exit_codes.h"
#include "html_container.h"
#include "renderer.h"
#include "resource_loader.h"

#ifdef HTML2PDF_USE_PANGO
#include <fontconfig/fontconfig.h>
#endif

namespace {
// Provisional viewport height used for media queries / vh units when the
// output height is auto. The actual output height comes from the laid-out
// document, not from this value.
constexpr int kDefaultViewportHeight = 768;

// Cairo image surfaces are limited to ~32767 px per side (16-bit coordinates in
// pixman). Clamp both output dimensions so a very tall auto-height page does not
// trip surface creation, and so an out-of-range height never overflows the
// float->int conversion.
constexpr int kMaxCanvasDim = 32767;

// Convert a (possibly huge or NaN) pixel dimension to a valid surface size in
// [1, kMaxCanvasDim], avoiding undefined float->int narrowing for large values.
int clamp_dim(double v) {
    if (!(v >= 1.0)) return 1;  // also catches NaN
    if (v > static_cast<double>(kMaxCanvasDim)) return kMaxCanvasDim;
    return static_cast<int>(v);
}
}  // namespace

int main(int argc, char** argv) {
    using namespace html2pdf;

#ifdef HTML2PDF_USE_PANGO
    FcInit();
#endif

    ParseResult parsed = parse_args(argc, argv);
    switch (parsed.status) {
        case ParseResult::Status::HelpShown:
            return kOk;
        case ParseResult::Status::Error:
            return kArgsError;
        case ParseResult::Status::Ok:
            break;
    }

    const Options& opts = parsed.options;
    const bool is_url = !opts.url.empty();
    const std::string& source = is_url ? opts.url : opts.input;

    // 1. Load the main document.
    ResourceLoader loader(opts.user_agent, opts.timeout_sec, opts.allow_local_network);
    FetchResult main_doc = loader.load_main(source, is_url);
    if (!main_doc.ok) {
        std::cerr << "error: " << main_doc.error << "\n";
        return is_url ? kNetworkError : kFileError;
    }

    // 2. Build the container with a provisional viewport.
    const int viewport_h = opts.height > 0 ? opts.height : kDefaultViewportHeight;
    HtmlContainer container(opts.width, viewport_h, loader);

    // 3. Parse + render (layout). master_css is applied by default.
    // Guard the empty-document case: data() may be null, and constructing a
    // std::string from (nullptr, 0) is undefined behaviour.
    const std::string html =
        main_doc.data.empty()
            ? std::string()
            : std::string(reinterpret_cast<const char*>(main_doc.data.data()),
                          main_doc.data.size());
#ifdef HTML2PDF_USE_PANGO
    container.load_web_fonts_from_html(html);
#endif
    litehtml::document::ptr doc = litehtml::document::createFromString(html, &container);
    if (!doc) {
        std::cerr << "error: failed to parse HTML document\n";
        return kRenderError;
    }

    doc->render(static_cast<litehtml::pixel_t>(opts.width));

    // 3b. Paginated PDF: split into fixed-size pages. The layout width drives
    // line breaks; content is scaled to fit the page width, so there is no
    // canvas-dimension clamp here (height is split across pages, width auto-fit).
    if (opts.format == Format::PDF && opts.page_w_pt > 0.0) {
        return render_to_pdf_paged(doc, opts.width, opts.page_w_pt, opts.page_h_pt,
                                   opts.margin_pt, opts.output);
    }

    // 4. Determine output dimensions. Height defaults to the content height.
    // Both are clamped to the Cairo surface limit (and never narrowed unsafely).
    const double raw_w = static_cast<double>(opts.width);
    const double raw_h = opts.height > 0
                             ? static_cast<double>(opts.height)
                             : std::ceil(static_cast<double>(doc->height()));
    const int w = clamp_dim(raw_w);
    const int h = clamp_dim(raw_h);
    if (static_cast<double>(w) < raw_w || static_cast<double>(h) < raw_h) {
        std::cerr << "warning: output dimensions clamped to " << w << "x" << h
                  << " (Cairo limit " << kMaxCanvasDim << " px per side)\n";
    }

    // 5. Write PNG or PDF.
    const int rc = (opts.format == Format::PNG) ? render_to_png(doc, w, h, opts.output)
                                                : render_to_pdf(doc, w, h, opts.output);
    if (rc != kOk) return rc;

    return kOk;
}
