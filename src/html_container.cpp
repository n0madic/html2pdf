#include "html_container.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

#include <litehtml/html.h>

#ifdef HTML2PDF_USE_PANGO
#include "web_font_manager.h"
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

namespace html2pdf {

namespace {

#ifndef HTML2PDF_USE_PANGO
// Concrete font handle returned by create_font(). Holds everything needed to
// re-apply the font to any Cairo context and to draw text decorations, which
// the toy API does not render itself.
struct ToyFont {
    std::string family;
    double size = 0;
    cairo_font_slant_t slant = CAIRO_FONT_SLANT_NORMAL;
    cairo_font_weight_t weight = CAIRO_FONT_WEIGHT_NORMAL;
    bool underline = false;
    bool line_through = false;
    bool overline = false;
};

// Extract the first family name from a CSS font-family list, trimming spaces
// and quotes. Falls back to "sans-serif" when nothing usable is found.
std::string first_family(const std::string& list) {
    size_t end = list.find(',');
    std::string name = (end == std::string::npos) ? list : list.substr(0, end);

    size_t b = name.find_first_not_of(" \t\r\n\"'");
    size_t e = name.find_last_not_of(" \t\r\n\"'");
    if (b == std::string::npos) return "sans-serif";
    name = name.substr(b, e - b + 1);
    return name.empty() ? "sans-serif" : name;
}
#endif  // !HTML2PDF_USE_PANGO

#ifdef HTML2PDF_USE_PANGO
std::string pango_family_list(const std::string& css_families,
                              const WebFontManager& web_fonts) {
    litehtml::string_vector tokens;
    litehtml::split_string(css_families, tokens, ",", "", "\"'");

    std::string out;
    for (auto font : tokens) {
        litehtml::trim(font, " \t\r\n\f\v\"'");
        if (font.empty()) continue;

        std::string lower = font;
        litehtml::lcase(lower);
        if (!litehtml::value_in_list(lower, "serif;sans-serif;monospace;cursive;fantasy;")) {
            font = web_fonts.resolve_family_for_pango(font);
        }
        out += font;
        out += ",";
    }

    return out.empty() ? std::string("serif,") : out;
}
#endif  // HTML2PDF_USE_PANGO

// Premultiply one 8-bit channel by alpha with rounding.
inline uint8_t premul(uint8_t c, uint8_t a) {
    return static_cast<uint8_t>((static_cast<int>(c) * a + 127) / 255);
}

}  // namespace

HtmlContainer::HtmlContainer(int width, int height, ResourceLoader& loader)
    : width_(width), height_(height), loader_(loader) {
#ifdef HTML2PDF_USE_PANGO
    pango_scratch_surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 2, 2);
    pango_scratch_cr_ = cairo_create(pango_scratch_surface_);
    web_fonts_ = std::make_unique<WebFontManager>(loader_);
#else
    scratch_surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    scratch_cr_ = cairo_create(scratch_surface_);
#endif
}

HtmlContainer::~HtmlContainer() {
    for (auto& kv : image_cache_) {
        if (kv.second) cairo_surface_destroy(kv.second);
    }
#ifdef HTML2PDF_USE_PANGO
    if (pango_scratch_cr_) cairo_destroy(pango_scratch_cr_);
    if (pango_scratch_surface_) cairo_surface_destroy(pango_scratch_surface_);
#else
    if (scratch_cr_) cairo_destroy(scratch_cr_);
    if (scratch_surface_) cairo_surface_destroy(scratch_surface_);
#endif
}

#ifdef HTML2PDF_USE_PANGO

void HtmlContainer::load_web_fonts_from_html(const std::string& html) {
    if (web_fonts_) web_fonts_->load_from_html(html);
}

litehtml::uint_ptr HtmlContainer::create_font(const litehtml::font_description& descr,
                                              const litehtml::document* doc,
                                              litehtml::font_metrics* fm) {
    const std::string families = web_fonts_ ? pango_family_list(descr.family, *web_fonts_)
                                            : std::string("serif,");

    PangoFontDescription* desc = pango_font_description_new();
    pango_font_description_set_family(desc, families.c_str());
    pango_font_description_set_absolute_size(desc, descr.size * PANGO_SCALE);
    pango_font_description_set_style(desc,
                                     descr.style == litehtml::font_style_italic
                                         ? PANGO_STYLE_ITALIC
                                         : PANGO_STYLE_NORMAL);
    pango_font_description_set_weight(desc, static_cast<PangoWeight>(descr.weight));

    cairo_font* ret = nullptr;

    if (fm) {
        fm->font_size = descr.size;

        cairo_save(pango_scratch_cr_);
        PangoLayout* layout = pango_cairo_create_layout(pango_scratch_cr_);
        PangoContext* context = pango_layout_get_context(layout);
        PangoLanguage* language = pango_language_get_default();
        pango_layout_set_font_description(layout, desc);
        PangoFontMetrics* metrics = pango_context_get_metrics(context, desc, language);

        fm->ascent = PANGO_PIXELS(static_cast<double>(pango_font_metrics_get_ascent(metrics)));
        fm->height = PANGO_PIXELS(static_cast<double>(pango_font_metrics_get_height(metrics)));
        fm->descent = fm->height - fm->ascent;
        fm->x_height = fm->height;
        fm->draw_spaces = (descr.decoration_line != litehtml::text_decoration_line_none);
        fm->sub_shift = descr.size / 5;
        fm->super_shift = descr.size / 3;

        pango_layout_set_text(layout, "x", 1);

        PangoRectangle ink_rect;
        PangoRectangle logical_rect;
        pango_layout_get_pixel_extents(layout, &ink_rect, &logical_rect);
        fm->x_height = ink_rect.height;
        if (fm->x_height == fm->height) fm->x_height = fm->x_height * 4 / 5;

        pango_layout_set_text(layout, "0", 1);

        pango_layout_get_pixel_extents(layout, &ink_rect, &logical_rect);
        fm->ch_width = logical_rect.width;

        cairo_restore(pango_scratch_cr_);

        ret = new cairo_font;
        ret->font = desc;
        ret->size = descr.size;
        ret->strikeout = (descr.decoration_line & litehtml::text_decoration_line_line_through) != 0;
        ret->underline = (descr.decoration_line & litehtml::text_decoration_line_underline) != 0;
        ret->overline = (descr.decoration_line & litehtml::text_decoration_line_overline) != 0;
        ret->ascent = fm->ascent;
        ret->descent = fm->descent;
        ret->decoration_color = descr.decoration_color;
        ret->decoration_style = descr.decoration_style;

        auto thickness = descr.decoration_thickness;
        if (!thickness.is_predefined() && doc) {
            litehtml::css_length one_em(1.0, litehtml::css_units_em);
            doc->cvt_units(one_em, *fm, 0);
            doc->cvt_units(thickness, *fm, static_cast<int>(one_em.val()));
        }

        ret->underline_position = -pango_font_metrics_get_underline_position(metrics);
        if (thickness.is_predefined()) {
            ret->underline_thickness = pango_font_metrics_get_underline_thickness(metrics);
        } else {
            ret->underline_thickness = static_cast<int>(thickness.val() * PANGO_SCALE);
        }
        pango_quantize_line_geometry(&ret->underline_thickness, &ret->underline_position);
        ret->underline_thickness = PANGO_PIXELS(ret->underline_thickness);
        ret->underline_position = PANGO_PIXELS(ret->underline_position);

        ret->strikethrough_position = pango_font_metrics_get_strikethrough_position(metrics);
        if (thickness.is_predefined()) {
            ret->strikethrough_thickness = pango_font_metrics_get_strikethrough_thickness(metrics);
        } else {
            ret->strikethrough_thickness = static_cast<int>(thickness.val() * PANGO_SCALE);
        }
        pango_quantize_line_geometry(&ret->strikethrough_thickness,
                                     &ret->strikethrough_position);
        ret->strikethrough_thickness = PANGO_PIXELS(ret->strikethrough_thickness);
        ret->strikethrough_position = PANGO_PIXELS(ret->strikethrough_position);

        ret->overline_position = pango_units_from_double(fm->ascent);
        if (thickness.is_predefined()) {
            ret->overline_thickness = pango_font_metrics_get_underline_thickness(metrics);
        } else {
            ret->overline_thickness = static_cast<int>(thickness.val() * PANGO_SCALE);
        }
        pango_quantize_line_geometry(&ret->overline_thickness, &ret->overline_position);
        ret->overline_thickness = PANGO_PIXELS(ret->overline_thickness);
        ret->overline_position = PANGO_PIXELS(ret->overline_position);

        g_object_unref(layout);
        pango_font_metrics_unref(metrics);
    } else {
        pango_font_description_free(desc);
    }

    return reinterpret_cast<litehtml::uint_ptr>(ret);
}

#else

void HtmlContainer::apply_font(cairo_t* cr, litehtml::uint_ptr hFont) {
    auto* f = reinterpret_cast<ToyFont*>(hFont);
    if (!f) return;
    cairo_select_font_face(cr, f->family.c_str(), f->slant, f->weight);
    cairo_set_font_size(cr, f->size);
}

litehtml::uint_ptr HtmlContainer::create_font(const litehtml::font_description& descr,
                                              const litehtml::document* /*doc*/,
                                              litehtml::font_metrics* fm) {
    auto* font = new ToyFont();
    font->family = first_family(descr.family);
    font->size = descr.size > 0 ? static_cast<double>(descr.size)
                                : static_cast<double>(get_default_font_size());
    font->slant = (descr.style == litehtml::font_style_italic) ? CAIRO_FONT_SLANT_ITALIC
                                                               : CAIRO_FONT_SLANT_NORMAL;
    font->weight = (descr.weight >= 600) ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL;
    font->underline = (descr.decoration_line & litehtml::text_decoration_line_underline) != 0;
    font->line_through = (descr.decoration_line & litehtml::text_decoration_line_line_through) != 0;
    font->overline = (descr.decoration_line & litehtml::text_decoration_line_overline) != 0;

    if (fm) {
        apply_font(scratch_cr_, reinterpret_cast<litehtml::uint_ptr>(font));
        cairo_font_extents_t fe;
        cairo_font_extents(scratch_cr_, &fe);

        cairo_text_extents_t te_x;
        cairo_text_extents(scratch_cr_, "x", &te_x);
        cairo_text_extents_t te_0;
        cairo_text_extents(scratch_cr_, "0", &te_0);

        fm->font_size = static_cast<litehtml::pixel_t>(font->size);
        fm->ascent = static_cast<litehtml::pixel_t>(fe.ascent);
        fm->descent = static_cast<litehtml::pixel_t>(fe.descent);
        fm->height = static_cast<litehtml::pixel_t>(fe.height);
        fm->x_height = static_cast<litehtml::pixel_t>(te_x.height);
        fm->ch_width = static_cast<litehtml::pixel_t>(te_0.x_advance);
        fm->draw_spaces = true;
        fm->sub_shift = static_cast<litehtml::pixel_t>(font->size * 0.3);
        fm->super_shift = static_cast<litehtml::pixel_t>(font->size * 0.3);
    }

    return reinterpret_cast<litehtml::uint_ptr>(font);
}

void HtmlContainer::delete_font(litehtml::uint_ptr hFont) {
    delete reinterpret_cast<ToyFont*>(hFont);
}

litehtml::pixel_t HtmlContainer::text_width(const char* text, litehtml::uint_ptr hFont) {
    if (!text) return 0;
    apply_font(scratch_cr_, hFont);
    cairo_text_extents_t te;
    cairo_text_extents(scratch_cr_, text, &te);
    return static_cast<litehtml::pixel_t>(te.x_advance);
}

void HtmlContainer::draw_text(litehtml::uint_ptr hdc, const char* text,
                              litehtml::uint_ptr hFont, litehtml::web_color color,
                              const litehtml::position& pos) {
    if (!text || !*text) return;
    auto* cr = reinterpret_cast<cairo_t*>(hdc);
    auto* f = reinterpret_cast<ToyFont*>(hFont);
    if (!cr || !f) return;

    apply_font(cr, hFont);
    set_color(cr, color);  // rgba/255 conversion provided by the base container

    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    const double baseline = pos.y + fe.ascent;

    cairo_move_to(cr, pos.x, baseline);
    cairo_show_text(cr, text);

    // The toy API does not draw decorations; render them manually.
    if (f->underline || f->line_through || f->overline) {
        cairo_text_extents_t te;
        cairo_text_extents(cr, text, &te);
        const double width = te.x_advance;
        const double line_w = std::max(1.0, f->size / 14.0);
        cairo_set_line_width(cr, line_w);

        auto stroke_line = [&](double y) {
            cairo_move_to(cr, pos.x, y);
            cairo_line_to(cr, pos.x + width, y);
            cairo_stroke(cr);
        };
        if (f->underline) stroke_line(baseline + std::max(1.0, fe.descent * 0.3));
        if (f->line_through) stroke_line(baseline - fe.ascent * 0.3);
        if (f->overline) stroke_line(pos.y + line_w);
    }
}

#endif  // !HTML2PDF_USE_PANGO

void HtmlContainer::make_url(const char* url, const char* basepath, litehtml::string& out) {
    if (!url) {
        out.clear();
        return;
    }
    // Resolve against the per-resource base when one is supplied (e.g. the
    // location of the stylesheet that referenced this image), otherwise against
    // the document base. The result is the absolute URL/path get_image keys on.
    out = loader_.resolve(url, (basepath && *basepath) ? std::string(basepath)
                                                       : std::string());
}

cairo_surface_t* HtmlContainer::get_image(const std::string& url) {
    // `url` has already been resolved to an absolute URL/path by make_url();
    // an empty string means the reference was unresolvable or blocked.
    auto it = image_cache_.find(url);
    if (it != image_cache_.end()) {
        // Return an extra reference for the caller; null stays null.
        return it->second ? cairo_surface_reference(it->second) : nullptr;
    }

    cairo_surface_t* surface = nullptr;
    FetchResult fr = url.empty() ? FetchResult{} : loader_.load_resolved(url);
    if (fr.ok && !fr.data.empty()) {
        int w = 0, h = 0, channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(fr.data.data(),
                                                static_cast<int>(fr.data.size()), &w, &h,
                                                &channels, 4);  // force RGBA
        if (pixels && w > 0 && h > 0) {
            surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
            if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
                unsigned char* dst = cairo_image_surface_get_data(surface);
                const int stride = cairo_image_surface_get_stride(surface);
                for (int y = 0; y < h; ++y) {
                    auto* row = reinterpret_cast<uint32_t*>(dst + static_cast<size_t>(y) * stride);
                    const stbi_uc* src = pixels + static_cast<size_t>(y) * w * 4;
                    for (int x = 0; x < w; ++x) {
                        const uint8_t r = src[x * 4 + 0];
                        const uint8_t g = src[x * 4 + 1];
                        const uint8_t b = src[x * 4 + 2];
                        const uint8_t a = src[x * 4 + 3];
                        // Cairo ARGB32 = native-endian 0xAARRGGBB, premultiplied.
                        row[x] = (static_cast<uint32_t>(a) << 24) |
                                 (static_cast<uint32_t>(premul(r, a)) << 16) |
                                 (static_cast<uint32_t>(premul(g, a)) << 8) |
                                 static_cast<uint32_t>(premul(b, a));
                    }
                }
                cairo_surface_mark_dirty(surface);
            } else {
                cairo_surface_destroy(surface);
                surface = nullptr;
            }
        }
        if (pixels) stbi_image_free(pixels);
    }

    image_cache_[url] = surface;  // cache owns one reference (or null)
    return surface ? cairo_surface_reference(surface) : nullptr;
}

void HtmlContainer::load_image(const char* src, const char* baseurl,
                               bool /*redraw_on_ready*/) {
    if (!src) return;
    // Resolve exactly as get_image will at draw time, so the cache is warmed
    // under the same (resolved) key. get_image returns a reference to release.
    litehtml::string resolved;
    make_url(src, baseurl, resolved);
    cairo_surface_t* s = get_image(resolved);
    if (s) cairo_surface_destroy(s);
}

void HtmlContainer::import_css(litehtml::string& text, const litehtml::string& url,
                               litehtml::string& baseurl) {
    text.clear();
    // Resolve the stylesheet URL against the importing context's base (the
    // parent sheet's location, or the document base when empty).
    const std::string resolved = loader_.resolve(url, baseurl);
    if (resolved.empty()) return;  // unresolvable / blocked
    FetchResult fr = loader_.load_resolved(resolved);
    if (fr.ok && !fr.data.empty()) {
        text.assign(reinterpret_cast<const char*>(fr.data.data()), fr.data.size());
#ifdef HTML2PDF_USE_PANGO
        if (web_fonts_) web_fonts_->load_from_css(text, resolved);
#endif
    }
    // Report the resolved location back so the sheet's own relative references
    // (nested @import, background images) resolve against it. On load failure
    // text stays empty: graceful degradation, no crash.
    baseurl = resolved;
}

void HtmlContainer::get_viewport(litehtml::position& viewport) const {
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = static_cast<litehtml::pixel_t>(width_);
    viewport.height = static_cast<litehtml::pixel_t>(height_);
}

void HtmlContainer::set_base_url(const char* base_url) {
    if (!base_url || !*base_url) return;
    // A <base href> may be relative; resolve it against the current document
    // base so that, e.g., an HTTP page with <base href="/assets/"> stays in
    // HTTP mode (joined onto the origin) instead of being read as a filesystem
    // path. set_base derives http-ness from the resolved value.
    const std::string resolved = loader_.resolve(base_url);
    if (resolved.empty()) return;
    loader_.set_base(resolved);
}

}  // namespace html2pdf
