#include "renderer.h"

#include <cairo-pdf.h>
#include <cairo.h>
#include <litehtml/render_item.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include "exit_codes.h"

namespace html2pdf {

namespace {

// Paint an opaque white background, then draw the whole document onto cr.
void paint_document(cairo_t* cr, const litehtml::document::ptr& doc, int w, int h) {
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    litehtml::position clip(0, 0, static_cast<litehtml::pixel_t>(w),
                           static_cast<litehtml::pixel_t>(h));
    doc->draw(reinterpret_cast<litehtml::uint_ptr>(cr), 0, 0, &clip);
}

// Paint the document onto a fresh context for `surface` (issuing show_page for
// paginated/vector backends) and report whether drawing succeeded. The context
// is destroyed before returning.
bool draw_to_surface(cairo_surface_t* surface, const litehtml::document::ptr& doc,
                     int w, int h, bool show_page) {
    cairo_t* cr = cairo_create(surface);
    paint_document(cr, doc, w, h);
    if (show_page) cairo_show_page(cr);
    const bool ok = cairo_status(cr) == CAIRO_STATUS_SUCCESS;
    if (!ok) {
        std::cerr << "error: drawing failed (" << cairo_status_to_string(cairo_status(cr))
                  << ")\n";
    }
    cairo_destroy(cr);
    return ok;
}

using ri_ptr = std::shared_ptr<litehtml::render_item>;

// True for boxes that must not be split internally when paginating: leaves with
// no child render items (text lines, replaced elements) and table rows, which
// look broken when cut mid-height. litehtml has no CSS fragmentation support,
// so break points are derived from the render tree rather than from CSS.
bool is_atomic(const ri_ptr& item) {
    if (item->children().empty()) return true;
    const char* tag = item->src_el()->get_tagName();
    if (tag && std::strcmp(tag, "tr") == 0) return true;
    return item->src_el()->css().get_display() == litehtml::display_table_row;
}

// litehtml lays out tables on a grid: row-groups (`<tbody>`/`<thead>`/`<tfoot>`)
// and rows (`<tr>`) report height()==0, and the real geometry lives in the cells
// (cell.pos().y == grid row top). For those wrapper boxes bottom() is unusable,
// so the true content bottom must be taken from the descendant cells.
bool is_table_wrapper(litehtml::style_display d) {
    return d == litehtml::display_table_row_group ||
           d == litehtml::display_table_header_group ||
           d == litehtml::display_table_footer_group ||
           d == litehtml::display_table_row;
}

// Absolute bottom edge of a box's painted content. For ordinary boxes this is
// origin_y + bottom(); for the zero-height table wrappers above it descends to
// the cells, which carry the actual row positions.
double effective_bottom(const ri_ptr& item, double origin_y) {
    double b = origin_y + static_cast<double>(item->bottom());
    if (is_table_wrapper(item->src_el()->css().get_display())) {
        const double child_origin = origin_y + static_cast<double>(item->pos().y);
        for (const ri_ptr& child : item->children()) {
            b = std::max(b, effective_bottom(child, child_origin));
        }
    }
    return b;
}

// Find the deepest box boundary that falls within the page band
// (page_top, page_top + capacity]. Coordinates mirror
// render_item::calc_document_size: `item` occupies the absolute vertical band
// [origin_y + top(), origin_y + bottom()], and each child is laid out at
// origin_y + item.pos().y. Returns the absolute Y of the chosen break, or -1 if
// `item` cannot contribute a boundary inside the page.
double find_break(const ri_ptr& item, double origin_y, double page_top, double capacity) {
    constexpr double kEps = 1e-6;
    const double page_bottom = page_top + capacity;
    const double item_bottom = effective_bottom(item, origin_y);
    if (item_bottom <= page_bottom + kEps) return item_bottom;  // whole box fits
    if (is_atomic(item)) return -1.0;                           // indivisible and too tall

    const double child_origin_y = origin_y + static_cast<double>(item->pos().y);
    double best = -1.0;
    for (const ri_ptr& child : item->children()) {
        const double child_top = child_origin_y + static_cast<double>(child->top());
        // Children are not guaranteed to be in ascending vertical order (floats
        // and other out-of-flow boxes can appear anywhere in the list), so skip
        // — not break on — a child that starts below the page and keep scanning.
        if (child_top >= page_bottom) continue;
        const double cb = find_break(child, child_origin_y, page_top, capacity);
        if (cb > page_top + kEps && cb <= page_bottom + kEps && cb > best) best = cb;
    }
    return best;
}

// Greedy pagination over the box tree. Returns ascending break offsets in layout
// pixels, [0 .. content_h]; each consecutive pair delimits one page worth of
// content. An indivisible unit taller than a page forces a hard cut. A short or
// empty document yields a single page ([0, content_h]).
std::vector<double> compute_page_breaks(const litehtml::document::ptr& doc,
                                        double content_h, double capacity) {
    std::vector<double> breaks;
    breaks.push_back(0.0);

    const ri_ptr root = doc->root_render();
    if (root && content_h > 0.0 && capacity > 0.0) {
        constexpr double kEps = 1e-6;
        // Safety cap: pathological input (an enormous content height, or a tiny
        // printable area) could otherwise emit a runaway number of pages. Stop
        // paginating past this many pages; any remaining content is omitted.
        constexpr std::size_t kMaxPages = 10000;
        double cur = 0.0;
        while (cur < content_h - kEps) {
            if (breaks.size() > kMaxPages) {
                std::cerr << "warning: page count capped at " << kMaxPages
                          << "; remaining content omitted\n";
                break;
            }
            double b = find_break(root, 0.0, cur, capacity);
            if (b <= cur + kEps) b = cur + capacity;  // nothing divisible fits: hard cut
            if (b > content_h) b = content_h;
            breaks.push_back(b);
            cur = b;
        }
    }

    if (breaks.size() < 2) breaks.push_back(content_h > 0.0 ? content_h : 1.0);
    return breaks;
}

}  // namespace

int render_to_png(const litehtml::document::ptr& doc, int w, int h, const std::string& path) {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        std::cerr << "error: cannot create image surface ("
                  << cairo_status_to_string(cairo_surface_status(surface)) << ")\n";
        cairo_surface_destroy(surface);
        return kRenderError;
    }

    int rc = kOk;
    if (!draw_to_surface(surface, doc, w, h, /*show_page=*/false)) {
        rc = kRenderError;
    } else {
        cairo_surface_flush(surface);
        cairo_status_t ws = cairo_surface_write_to_png(surface, path.c_str());
        if (ws != CAIRO_STATUS_SUCCESS) {
            std::cerr << "error: cannot write PNG to " << path << " ("
                      << cairo_status_to_string(ws) << ")\n";
            // Writing the output file failed: this is a file I/O error, not a
            // graphics error.
            rc = kFileError;
        }
    }

    cairo_surface_destroy(surface);
    return rc;
}

int render_to_pdf(const litehtml::document::ptr& doc, int w, int h, const std::string& path) {
    cairo_surface_t* surface =
        cairo_pdf_surface_create(path.c_str(), static_cast<double>(w), static_cast<double>(h));
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        std::cerr << "error: cannot create PDF surface for " << path << " ("
                  << cairo_status_to_string(cairo_surface_status(surface)) << ")\n";
        cairo_surface_destroy(surface);
        // Creating the PDF surface opens the output file: a failure here is a
        // file I/O error (e.g. unwritable destination).
        return kFileError;
    }

    int rc = draw_to_surface(surface, doc, w, h, /*show_page=*/true) ? kOk : kRenderError;

    cairo_surface_finish(surface);
    if (rc == kOk && cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        std::cerr << "error: cannot finalize PDF " << path << " ("
                  << cairo_status_to_string(cairo_surface_status(surface)) << ")\n";
        // Finishing flushes the PDF to disk: a failure is a file I/O error.
        rc = kFileError;
    }
    cairo_surface_destroy(surface);
    return rc;
}

int render_to_pdf_paged(const litehtml::document::ptr& doc, int layout_w,
                        double page_w, double page_h, double margin, const std::string& path) {
    // 1. Effective content width (so wide content is shrunk to fit, not clipped)
    // and the scale that maps it onto the printable width.
    const double eff_w = std::max(static_cast<double>(layout_w),
                                  std::ceil(static_cast<double>(doc->width())));
    const double content_h = std::ceil(static_cast<double>(doc->height()));
    const double printable_w = page_w - 2.0 * margin;
    const double printable_h = page_h - 2.0 * margin;
    const double scale = printable_w / eff_w;
    const double capacity = printable_h / scale;  // vertical page capacity in layout px

    // 2. Break offsets along the layout height, at box boundaries.
    const std::vector<double> breaks = compute_page_breaks(doc, content_h, capacity);

    // 3. One PDF page per [breaks[k], breaks[k+1]] band. A single surface is
    // created once; each page gets a fresh context (identity CTM, no clip).
    cairo_surface_t* surface = cairo_pdf_surface_create(path.c_str(), page_w, page_h);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        std::cerr << "error: cannot create PDF surface for " << path << " ("
                  << cairo_status_to_string(cairo_surface_status(surface)) << ")\n";
        cairo_surface_destroy(surface);
        // Creating the PDF surface opens the output file: failure is file I/O.
        return kFileError;
    }

    int rc = kOk;
    for (std::size_t k = 0; k + 1 < breaks.size(); ++k) {
        const double top = breaks[k];
        const double band_h = breaks[k + 1] - top;
        cairo_t* cr = cairo_create(surface);

        // White background over the whole sheet (including margins). Painted
        // before clipping, so cairo_paint fills the full page, not just the band.
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_paint(cr);

        // Clip to the printable area (absolute points), then map the band into
        // it: clip (identity CTM) -> translate to the margin -> scale -> shift
        // the band's top to the top of the printable area.
        cairo_rectangle(cr, margin, margin, printable_w, printable_h);
        cairo_clip(cr);
        cairo_translate(cr, margin, margin);
        cairo_scale(cr, scale, scale);
        cairo_translate(cr, 0.0, -top);

        // litehtml clip is only a culling hint; the cairo clip is the true bound.
        litehtml::position band(0, static_cast<litehtml::pixel_t>(top),
                                static_cast<litehtml::pixel_t>(eff_w),
                                static_cast<litehtml::pixel_t>(std::ceil(band_h)));
        doc->draw(reinterpret_cast<litehtml::uint_ptr>(cr), 0, 0, &band);

        if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
            std::cerr << "error: drawing failed ("
                      << cairo_status_to_string(cairo_status(cr)) << ")\n";
            rc = kRenderError;
        }
        cairo_show_page(cr);
        cairo_destroy(cr);
        if (rc != kOk) break;
    }

    cairo_surface_finish(surface);
    if (rc == kOk && cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        std::cerr << "error: cannot finalize PDF " << path << " ("
                  << cairo_status_to_string(cairo_surface_status(surface)) << ")\n";
        // Finishing flushes the PDF to disk: a failure is a file I/O error.
        rc = kFileError;
    }
    cairo_surface_destroy(surface);
    return rc;
}

}  // namespace html2pdf
