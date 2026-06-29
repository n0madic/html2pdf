#pragma once

#include <string>

#include <litehtml.h>

namespace html2pdf {

// Render a laid-out document to a raster PNG of size w x h.
// Returns 0 on success, 2 on an output file I/O error, 4 on a graphics error.
int render_to_png(const litehtml::document::ptr& doc, int w, int h, const std::string& path);

// Render a laid-out document to a single-page vector PDF of size w x h points
// (1px == 1pt).
// Returns 0 on success, 2 on an output file I/O error, 4 on a graphics error.
int render_to_pdf(const litehtml::document::ptr& doc, int w, int h, const std::string& path);

// Render to a multi-page PDF. The document is laid out at layout_w px (call
// doc->render(layout_w) first). Content is scaled so its full content width
// (max(layout_w, doc->width())) fits the printable width (page_w - 2*margin),
// so wide content is shrunk to fit rather than clipped. Pages break at box
// boundaries (blocks / table rows / lines); an element taller than a page is
// hard-cut. page_w/page_h/margin are in points.
// Returns 0 on success, 2 on an output file I/O error, 4 on a graphics error.
int render_to_pdf_paged(const litehtml::document::ptr& doc, int layout_w,
                        double page_w, double page_h, double margin, const std::string& path);

}  // namespace html2pdf
