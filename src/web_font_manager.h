#pragma once

#ifdef HTML2PDF_USE_PANGO

#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace litehtml {
class raw_rule;
}

namespace html2pdf {

class ResourceLoader;

class WebFontManager {
public:
    explicit WebFontManager(ResourceLoader& loader);
    ~WebFontManager();

    WebFontManager(const WebFontManager&) = delete;
    WebFontManager& operator=(const WebFontManager&) = delete;

    void load_from_html(const std::string& html);
    void load_from_css(const std::string& css, const std::string& base_url);

    // Returns the family name Pango should receive for a CSS family. With the
    // Fontconfig backend this is normally the CSS alias itself; with non-FC
    // Pango font maps it may be rewritten to the internal family discovered
    // after pango_font_map_add_font_file().
    std::string resolve_family_for_pango(const std::string& family) const;

    enum class CssFontStyle { Normal, Italic, Oblique };

    struct FontSource {
        std::string url;
    };

    struct FontFace {
        std::string family;
        std::vector<FontSource> sources;
        int weight = 400;
        CssFontStyle style = CssFontStyle::Normal;
    };

private:
    void load_from_css_rules(const std::vector<std::shared_ptr<litehtml::raw_rule>>& rules,
                             const std::string& base_url);
    void parse_import_rule(const std::shared_ptr<litehtml::raw_rule>& rule,
                           const std::string& base_url);
    void parse_font_face_rule(const std::shared_ptr<litehtml::raw_rule>& rule,
                              const std::string& base_url);
    bool register_font_face(const FontFace& face, const std::string& base_url);
    bool register_font_bytes(const FontFace& face, const std::string& resolved_url,
                             const std::vector<unsigned char>& data);
    bool install_font_file(const FontFace& face, const std::string& font_path);
    bool activate_fontconfig_backend();
    bool install_fontconfig_file(const FontFace& face, const std::string& font_path);
    bool install_pango_file(const FontFace& face, const std::string& font_path);
    void notify_font_maps_changed();
    void cleanup();

    std::string ensure_temp_dir();

    ResourceLoader& loader_;
    std::string temp_dir_;
    std::unordered_set<std::string> parsed_css_urls_;
    std::unordered_set<std::string> registered_faces_;
    std::unordered_map<std::string, std::string> family_aliases_;
    std::size_t font_file_counter_ = 0;
    bool fontconfig_available_ = false;
    bool installed_any_font_ = false;
};

}  // namespace html2pdf

#endif  // HTML2PDF_USE_PANGO
