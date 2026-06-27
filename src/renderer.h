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

}  // namespace html2pdf
