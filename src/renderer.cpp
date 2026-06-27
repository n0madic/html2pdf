#include "renderer.h"

#include <cairo-pdf.h>
#include <cairo.h>

#include <iostream>

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

}  // namespace html2pdf
