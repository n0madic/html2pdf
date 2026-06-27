#pragma once

#include <cairo.h>

#include <string>
#include <unordered_map>

#ifdef HTML2PDF_USE_PANGO
#include <container_cairo_pango.h>
#else
#include <container_cairo.h>
#endif
#include <litehtml.h>

#include "resource_loader.h"

namespace html2pdf {

// Base container selected at build time. With the Pango backend the font hooks
// (create_font / draw_text / text_width) come from container_cairo_pango;
// otherwise they are implemented here via the Cairo toy text API.
#ifdef HTML2PDF_USE_PANGO
using ContainerBase = container_cairo_pango;
#else
using ContainerBase = container_cairo;
#endif

// Document container backed by Cairo. It inherits all drawing from the
// reference container (and, with Pango, text rendering too) and implements the
// hooks left abstract: image decoding (stb_image), resource loading (delegated
// to ResourceLoader) and the viewport/screen environment. With the toy backend
// it additionally implements the font hooks.
class HtmlContainer : public ContainerBase {
public:
    HtmlContainer(int width, int height, ResourceLoader& loader);
    ~HtmlContainer() override;

    HtmlContainer(const HtmlContainer&) = delete;
    HtmlContainer& operator=(const HtmlContainer&) = delete;

#ifndef HTML2PDF_USE_PANGO
    // --- Fonts (Cairo toy text API; Pango backend supplies its own) ---
    litehtml::uint_ptr create_font(const litehtml::font_description& descr,
                                   const litehtml::document* doc,
                                   litehtml::font_metrics* fm) override;
    void delete_font(litehtml::uint_ptr hFont) override;
    litehtml::pixel_t text_width(const char* text, litehtml::uint_ptr hFont) override;
    void draw_text(litehtml::uint_ptr hdc, const char* text, litehtml::uint_ptr hFont,
                   litehtml::web_color color, const litehtml::position& pos) override;
#endif

    // --- Images / resources ---
    // Resolve a (possibly relative) resource reference against its base. This is
    // the single resolution hook the reference container calls before get_image;
    // overriding it lets every image path honour its per-resource base URL.
    void make_url(const char* url, const char* basepath, litehtml::string& out) override;
    cairo_surface_t* get_image(const std::string& url) override;
    void load_image(const char* src, const char* baseurl, bool redraw_on_ready) override;
    void import_css(litehtml::string& text, const litehtml::string& url,
                    litehtml::string& baseurl) override;

    // --- Screen / viewport ---
    double get_screen_dpi() const override { return 96.0; }
    int get_screen_width() const override { return width_; }
    int get_screen_height() const override { return height_; }
    void get_viewport(litehtml::position& viewport) const override;
    const char* get_default_font_name() const override { return "sans-serif"; }

    // --- No-op / trivial hooks ---
    void set_caption(const char* /*caption*/) override {}
    void set_base_url(const char* base_url) override;
    void on_anchor_click(const char* /*url*/, const litehtml::element::ptr& /*el*/) override {}
    void on_mouse_event(const litehtml::element::ptr& /*el*/,
                        litehtml::mouse_event /*event*/) override {}
    void set_cursor(const char* /*cursor*/) override {}

private:
#ifndef HTML2PDF_USE_PANGO
    // Apply a font handle to a Cairo context (face, size, slant, weight).
    static void apply_font(cairo_t* cr, litehtml::uint_ptr hFont);
#endif

    int width_;
    int height_;
    ResourceLoader& loader_;

#ifndef HTML2PDF_USE_PANGO
    // Scratch context for off-draw measurements (1x1 ARGB32 surface).
    cairo_surface_t* scratch_surface_ = nullptr;
    cairo_t* scratch_cr_ = nullptr;
#endif

    // Decoded image cache, keyed on the resolved absolute URL/path. Owns one
    // reference per non-null surface; get_image returns an additional reference
    // for the caller to destroy.
    std::unordered_map<std::string, cairo_surface_t*> image_cache_;
};

}  // namespace html2pdf
