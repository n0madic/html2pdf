#ifdef HTML2PDF_USE_PANGO

#include "web_font_manager.h"

#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>
#include <pango/pangofc-fontmap.h>
#include <woff2/decode.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <unordered_set>

#include <litehtml/css_parser.h>
#include <litehtml/html.h>
#include <litehtml/style.h>

#include "resource_loader.h"

namespace html2pdf {

namespace {

constexpr std::size_t kMaxDecodedFontBytes = 64u * 1024u * 1024u;

std::string ascii_lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string trim_copy(std::string s) {
    return litehtml::trim(s, " \t\r\n\f\v\"'");
}

std::string family_key(const std::string& family) {
    return ascii_lower(trim_copy(family));
}

bool starts_with_ci(const std::string& s, std::size_t pos, const char* needle) {
    for (std::size_t i = 0; needle[i]; ++i) {
        if (pos + i >= s.size()) return false;
        char a = s[pos + i];
        char b = needle[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

std::size_t find_ci(const std::string& s, const char* needle, std::size_t from) {
    const std::size_t n = std::char_traits<char>::length(needle);
    if (n == 0) return from <= s.size() ? from : std::string::npos;
    for (std::size_t i = from; i + n <= s.size(); ++i) {
        if (starts_with_ci(s, i, needle)) return i;
    }
    return std::string::npos;
}

std::vector<std::string> extract_style_blocks(const std::string& html) {
    std::vector<std::string> blocks;
    std::size_t pos = 0;
    while (pos < html.size()) {
        const std::size_t tag = find_ci(html, "<style", pos);
        if (tag == std::string::npos) break;
        // Skip commented-out markup: when an HTML comment opens before the next
        // <style>, jump past its close so a <style> hidden inside is ignored.
        const std::size_t comment = html.find("<!--", pos);
        if (comment != std::string::npos && comment < tag) {
            const std::size_t comment_end = html.find("-->", comment + 4);
            if (comment_end == std::string::npos) break;
            pos = comment_end + 3;
            continue;
        }
        const std::size_t tag_end = html.find('>', tag);
        if (tag_end == std::string::npos) break;
        const std::size_t close = find_ci(html, "</style", tag_end + 1);
        if (close == std::string::npos) break;
        blocks.push_back(html.substr(tag_end + 1, close - tag_end - 1));
        pos = close + 8;
    }
    return blocks;
}

bool token_to_url(const litehtml::css_token& tok, std::string& url) {
    if (tok.type == litehtml::STRING) {
        url = tok.str;
        return true;
    }
    return litehtml::parse_url(tok, url);
}

std::string parse_family_value(const litehtml::css_token_vector& tokens) {
    if (tokens.empty()) return "";
    if (tokens.size() == 1 && tokens[0].type == litehtml::STRING) {
        return trim_copy(tokens[0].str);
    }
    return trim_copy(litehtml::get_repr(tokens, 0, -1, true));
}

int parse_weight_value(const litehtml::css_token_vector& tokens) {
    if (tokens.empty()) return 400;
    const auto& tok = tokens[0];
    if (tok.type == litehtml::IDENT) {
        const std::string ident = tok.ident();
        if (ident == "normal") return 400;
        if (ident == "bold") return 700;
    }
    if (tok.type == litehtml::NUMBER) {
        const int weight = static_cast<int>(std::lround(tok.n.number));
        if (weight >= 1 && weight <= 1000) return weight;
    }
    return 400;
}

WebFontManager::CssFontStyle parse_style_value(const litehtml::css_token_vector& tokens) {
    if (tokens.empty() || tokens[0].type != litehtml::IDENT) {
        return WebFontManager::CssFontStyle::Normal;
    }
    const std::string ident = tokens[0].ident();
    if (ident == "italic") return WebFontManager::CssFontStyle::Italic;
    if (ident == "oblique") return WebFontManager::CssFontStyle::Oblique;
    return WebFontManager::CssFontStyle::Normal;
}

std::vector<WebFontManager::FontSource> parse_src_value(litehtml::css_token_vector tokens) {
    std::vector<WebFontManager::FontSource> sources;
    auto parts = litehtml::parse_comma_separated_list(tokens);
    for (auto& part : parts) {
        int index = 0;
        litehtml::skip_whitespace(part, index);
        const auto& tok = litehtml::at(part, index);
        std::string url;
        if (token_to_url(tok, url) && !url.empty()) {
            sources.push_back({url});
        }
    }
    return sources;
}

bool has_magic(const std::vector<unsigned char>& data, const char (&magic)[5]) {
    return data.size() >= 4 &&
           data[0] == static_cast<unsigned char>(magic[0]) &&
           data[1] == static_cast<unsigned char>(magic[1]) &&
           data[2] == static_cast<unsigned char>(magic[2]) &&
           data[3] == static_cast<unsigned char>(magic[3]);
}

bool is_woff2(const std::vector<unsigned char>& data) {
    static constexpr char kMagic[5] = {'w', 'O', 'F', '2', '\0'};
    return has_magic(data, kMagic);
}

bool is_sfnt(const std::vector<unsigned char>& data) {
    if (data.size() < 4) return false;
    if (data[0] == 0x00 && data[1] == 0x01 && data[2] == 0x00 && data[3] == 0x00) {
        return true;
    }
    static constexpr char kOtf[5] = {'O', 'T', 'T', 'O', '\0'};
    static constexpr char kTrue[5] = {'t', 'r', 'u', 'e', '\0'};
    static constexpr char kTyp1[5] = {'t', 'y', 'p', '1', '\0'};
    return has_magic(data, kOtf) || has_magic(data, kTrue) || has_magic(data, kTyp1);
}

std::string sfnt_extension(const std::vector<unsigned char>& data) {
    static constexpr char kOtf[5] = {'O', 'T', 'T', 'O', '\0'};
    return has_magic(data, kOtf) ? ".otf" : ".ttf";
}

bool is_woff1(const std::vector<unsigned char>& data) {
    static constexpr char kMagic[5] = {'w', 'O', 'F', 'F', '\0'};
    return has_magic(data, kMagic);
}

uint16_t read_be16(const unsigned char* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t read_be32(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void write_be16(unsigned char* p, uint16_t v) {
    p[0] = static_cast<unsigned char>(v >> 8);
    p[1] = static_cast<unsigned char>(v);
}

void write_be32(unsigned char* p, uint32_t v) {
    p[0] = static_cast<unsigned char>(v >> 24);
    p[1] = static_cast<unsigned char>(v >> 16);
    p[2] = static_cast<unsigned char>(v >> 8);
    p[3] = static_cast<unsigned char>(v);
}

// Decode a WOFF (v1) container into a bare SFNT. Each table is stored
// individually, zlib-compressed when that is smaller than the original and
// verbatim otherwise. google/woff2 only handles WOFF2, so WOFF1 is reassembled
// here. Returns false (caller falls back) on any malformed or oversized input.
bool woff1_to_sfnt(const std::vector<unsigned char>& in, std::vector<unsigned char>& out) {
    constexpr std::size_t kHeaderSize = 44;
    constexpr std::size_t kWoffEntrySize = 20;
    constexpr std::size_t kSfntEntrySize = 16;
    if (in.size() < kHeaderSize) return false;

    const uint32_t flavor = read_be32(&in[4]);
    const uint32_t length = read_be32(&in[8]);
    const uint16_t num_tables = read_be16(&in[12]);
    if (length != in.size() || num_tables == 0) return false;
    if (in.size() < kHeaderSize + static_cast<std::size_t>(num_tables) * kWoffEntrySize) {
        return false;
    }

    struct Entry {
        unsigned char tag[4];
        uint32_t offset;
        uint32_t comp_length;
        uint32_t orig_length;
        uint32_t checksum;
    };
    std::vector<Entry> entries(num_tables);

    std::size_t sfnt_size = 12 + static_cast<std::size_t>(num_tables) * kSfntEntrySize;
    for (uint16_t i = 0; i < num_tables; ++i) {
        const unsigned char* e = in.data() + kHeaderSize + static_cast<std::size_t>(i) * kWoffEntrySize;
        Entry& ent = entries[i];
        std::copy(e, e + 4, ent.tag);
        ent.offset = read_be32(e + 4);
        ent.comp_length = read_be32(e + 8);
        ent.orig_length = read_be32(e + 12);
        ent.checksum = read_be32(e + 16);

        if (ent.offset > in.size() || ent.comp_length > in.size() - ent.offset) return false;
        if (ent.comp_length > ent.orig_length) return false;
        if (ent.orig_length > kMaxDecodedFontBytes) return false;

        sfnt_size += ent.orig_length;
        sfnt_size = (sfnt_size + 3) & ~static_cast<std::size_t>(3);
        if (sfnt_size > kMaxDecodedFontBytes) return false;
    }

    out.assign(sfnt_size, 0);

    // SFNT offset table: sfntVersion, numTables, then the advisory
    // searchRange / entrySelector / rangeShift that real fonts carry.
    write_be32(out.data(), flavor);
    write_be16(out.data() + 4, num_tables);
    uint32_t max_pow2 = 1;
    uint16_t entry_selector = 0;
    while ((max_pow2 << 1) <= num_tables) {
        max_pow2 <<= 1;
        ++entry_selector;
    }
    const uint32_t search_range = max_pow2 * 16u;
    write_be16(out.data() + 6, static_cast<uint16_t>(search_range));
    write_be16(out.data() + 8, entry_selector);
    write_be16(out.data() + 10, static_cast<uint16_t>(num_tables * 16u - search_range));

    std::size_t data_off = 12 + static_cast<std::size_t>(num_tables) * kSfntEntrySize;
    for (uint16_t i = 0; i < num_tables; ++i) {
        const Entry& ent = entries[i];
        unsigned char* dst = out.data() + data_off;
        if (ent.comp_length == ent.orig_length) {
            std::copy(in.data() + ent.offset, in.data() + ent.offset + ent.orig_length, dst);
        } else {
            uLongf dest_len = ent.orig_length;
            const int zr = uncompress(dst, &dest_len, in.data() + ent.offset, ent.comp_length);
            if (zr != Z_OK || dest_len != ent.orig_length) return false;
        }

        unsigned char* de = out.data() + 12 + static_cast<std::size_t>(i) * kSfntEntrySize;
        std::copy(ent.tag, ent.tag + 4, de);
        write_be32(de + 4, ent.checksum);
        write_be32(de + 8, static_cast<uint32_t>(data_off));
        write_be32(de + 12, ent.orig_length);

        data_off += ent.orig_length;
        data_off = (data_off + 3) & ~static_cast<std::size_t>(3);
    }
    return true;
}

std::string face_key(const WebFontManager::FontFace& face, const std::string& resolved_url) {
    std::ostringstream out;
    out << family_key(face.family) << ':' << face.weight << ':'
        << static_cast<int>(face.style) << ':' << resolved_url;
    return out.str();
}

int fc_slant(WebFontManager::CssFontStyle style) {
    switch (style) {
        case WebFontManager::CssFontStyle::Italic:
            return FC_SLANT_ITALIC;
        case WebFontManager::CssFontStyle::Oblique:
            return FC_SLANT_OBLIQUE;
        case WebFontManager::CssFontStyle::Normal:
        default:
            return FC_SLANT_ROMAN;
    }
}

std::unordered_map<std::string, std::string> pango_family_map(PangoFontMap* map) {
    std::unordered_map<std::string, std::string> families;
    if (!map) return families;
    PangoFontFamily** raw = nullptr;
    int count = 0;
    pango_font_map_list_families(map, &raw, &count);
    for (int i = 0; i < count; ++i) {
        if (!PANGO_IS_FONT_FAMILY(raw[i])) continue;
        const char* name = pango_font_family_get_name(raw[i]);
        if (name && *name) families.emplace(family_key(name), name);
    }
    g_free(raw);
    return families;
}

}  // namespace

WebFontManager::WebFontManager(ResourceLoader& loader) : loader_(loader) {
    FcInit();
    PangoFontMap* map = pango_cairo_font_map_get_default();
    fontconfig_available_ = map && PANGO_IS_FC_FONT_MAP(map) && FcConfigGetCurrent();
}

WebFontManager::~WebFontManager() {
    cleanup();
}

void WebFontManager::load_from_html(const std::string& html) {
    for (const std::string& css : extract_style_blocks(html)) {
        load_from_css(css, "");
    }
}

void WebFontManager::load_from_css(const std::string& css, const std::string& base_url) {
    if (!base_url.empty()) {
        auto inserted = parsed_css_urls_.insert(base_url);
        if (!inserted.second) return;
    }

    auto rules = litehtml::css_parser::parse_stylesheet(css, true);
    load_from_css_rules(rules, base_url);
}

std::string WebFontManager::resolve_family_for_pango(const std::string& family) const {
    const auto it = family_aliases_.find(family_key(family));
    return it == family_aliases_.end() ? family : it->second;
}

void WebFontManager::load_from_css_rules(
    const std::vector<std::shared_ptr<litehtml::raw_rule>>& rules,
    const std::string& base_url) {
    for (const auto& rule : rules) {
        if (!rule || rule->type != litehtml::raw_rule::at) continue;
        const std::string name = ascii_lower(rule->name);
        if (name == "font-face") {
            parse_font_face_rule(rule, base_url);
        } else if (name == "import") {
            parse_import_rule(rule, base_url);
        } else if (name == "media" && rule->block.type == litehtml::CURLY_BLOCK) {
            auto nested = litehtml::css_parser::parse_stylesheet(rule->block.value, false);
            load_from_css_rules(nested, base_url);
        }
    }
}

void WebFontManager::parse_import_rule(const std::shared_ptr<litehtml::raw_rule>& rule,
                                       const std::string& base_url) {
    auto tokens = rule->prelude;
    int index = 0;
    litehtml::skip_whitespace(tokens, index);

    std::string url;
    if (!token_to_url(litehtml::at(tokens, index), url) || url.empty()) return;

    const std::string resolved = loader_.resolve(url, base_url);
    if (resolved.empty() || parsed_css_urls_.find(resolved) != parsed_css_urls_.end()) {
        return;
    }

    FetchResult fr = loader_.load_resolved(resolved);
    if (!fr.ok || fr.data.empty()) return;

    const std::string css(reinterpret_cast<const char*>(fr.data.data()), fr.data.size());
    load_from_css(css, resolved);
}

void WebFontManager::parse_font_face_rule(const std::shared_ptr<litehtml::raw_rule>& rule,
                                          const std::string& base_url) {
    if (rule->block.type != litehtml::CURLY_BLOCK) return;

    litehtml::raw_declaration::vector decls;
    litehtml::raw_rule::vector nested_rules;
    litehtml::css_parser(rule->block.value).consume_style_block_contents(decls, nested_rules);

    FontFace face;
    for (auto& decl : decls) {
        auto value = decl.value;
        litehtml::remove_whitespace(value);
        const std::string name = ascii_lower(decl.name);
        if (name == "font-family") {
            face.family = parse_family_value(value);
        } else if (name == "src") {
            face.sources = parse_src_value(value);
        } else if (name == "font-weight") {
            face.weight = parse_weight_value(value);
        } else if (name == "font-style") {
            face.style = parse_style_value(value);
        }
    }

    if (!face.family.empty() && !face.sources.empty()) {
        register_font_face(face, base_url);
    }
}

bool WebFontManager::register_font_face(const FontFace& face, const std::string& base_url) {
    for (const FontSource& source : face.sources) {
        const std::string resolved = loader_.resolve(source.url, base_url);
        if (resolved.empty()) continue;

        const std::string key = face_key(face, resolved);
        if (registered_faces_.find(key) != registered_faces_.end()) return true;

        FetchResult fr = loader_.load_resolved(resolved);
        if (!fr.ok || fr.data.empty()) continue;
        if (register_font_bytes(face, resolved, fr.data)) {
            registered_faces_.insert(key);
            return true;
        }
    }
    return false;
}

bool WebFontManager::register_font_bytes(const FontFace& face, const std::string& resolved_url,
                                         const std::vector<unsigned char>& data) {
    std::vector<unsigned char> sfnt;
    std::string ext;

    if (is_woff2(data)) {
        const std::size_t final_size = woff2::ComputeWOFF2FinalSize(data.data(), data.size());
        if (final_size == 0 || final_size > kMaxDecodedFontBytes) return false;
        sfnt.resize(final_size);
        if (!woff2::ConvertWOFF2ToTTF(sfnt.data(), sfnt.size(), data.data(), data.size())) {
            return false;
        }
        ext = ".ttf";
    } else if (is_woff1(data)) {
        if (!woff1_to_sfnt(data, sfnt)) return false;
        ext = sfnt_extension(sfnt);
    } else if (is_sfnt(data)) {
        sfnt = data;
        ext = sfnt_extension(data);
    } else {
        return false;
    }

    const std::string dir = ensure_temp_dir();
    if (dir.empty()) return false;

    std::ostringstream name;
    name << "font-" << (++font_file_counter_) << ext;
    const std::filesystem::path path = std::filesystem::path(dir) / name.str();

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(sfnt.data()), static_cast<std::streamsize>(sfnt.size()));
    if (!out) return false;
    out.close();

    (void)resolved_url;
    return install_font_file(face, path.string());
}

bool WebFontManager::install_font_file(const FontFace& face, const std::string& font_path) {
    if (!fontconfig_available_) {
        activate_fontconfig_backend();
    }
    if (fontconfig_available_) {
        return install_fontconfig_file(face, font_path);
    }
    return install_pango_file(face, font_path);
}

bool WebFontManager::activate_fontconfig_backend() {
    // Deliberately process-global: on platforms whose default Pango font map is
    // not Fontconfig-backed (e.g. the CoreText map on macOS) app-registered web
    // fonts cannot be aliased, so swap the process default to an FT/Fontconfig
    // map. Only reached when a document actually declares @font-face; from then
    // on all text in this single-shot render goes through the same backend, so
    // system and web fonts stay mutually consistent.
    PangoFontMap* current = pango_cairo_font_map_get_default();
    if (current && PANGO_IS_FC_FONT_MAP(current) && FcConfigGetCurrent()) {
        fontconfig_available_ = true;
        return true;
    }

    PangoFontMap* ft_map = pango_cairo_font_map_new_for_font_type(CAIRO_FONT_TYPE_FT);
    if (!ft_map) return false;

    const bool is_fc = PANGO_IS_FC_FONT_MAP(ft_map);
    if (is_fc) {
        pango_cairo_font_map_set_default(PANGO_CAIRO_FONT_MAP(ft_map));
    }
    g_object_unref(ft_map);

    current = pango_cairo_font_map_get_default();
    fontconfig_available_ = current && PANGO_IS_FC_FONT_MAP(current) && FcConfigGetCurrent();
    return fontconfig_available_;
}

bool WebFontManager::install_fontconfig_file(const FontFace& face, const std::string& font_path) {
    FcConfig* config = FcConfigGetCurrent();
    if (!config) return false;

    FcFontSet* before_set = FcConfigGetFonts(config, FcSetApplication);
    const int before_count = before_set ? before_set->nfont : 0;

    if (!FcConfigAppFontAddFile(config, reinterpret_cast<const FcChar8*>(font_path.c_str()))) {
        return false;
    }

    FcFontSet* app_set = FcConfigGetFonts(config, FcSetApplication);
    if (!app_set || app_set->nfont <= before_count) return false;

    const int weight = FcWeightFromOpenType(face.weight);
    const int slant = fc_slant(face.style);
    bool changed = false;
    for (int i = before_count; i < app_set->nfont; ++i) {
        FcPattern* pat = app_set->fonts[i];
        if (!pat) continue;
        FcPatternDel(pat, FC_FAMILY);
        FcPatternAddString(pat, FC_FAMILY, reinterpret_cast<const FcChar8*>(face.family.c_str()));
        FcPatternDel(pat, FC_WEIGHT);
        FcPatternAddInteger(pat, FC_WEIGHT, weight);
        FcPatternDel(pat, FC_SLANT);
        FcPatternAddInteger(pat, FC_SLANT, slant);
        changed = true;
    }

    if (!changed) return false;
    family_aliases_[family_key(face.family)] = face.family;
    installed_any_font_ = true;
    notify_font_maps_changed();
    return true;
}

bool WebFontManager::install_pango_file(const FontFace& face, const std::string& font_path) {
#if PANGO_VERSION_CHECK(1, 56, 0)
    PangoFontMap* map = pango_cairo_font_map_get_default();
    if (!map) return false;
    auto before = pango_family_map(map);

    GError* error = nullptr;
    const gboolean ok = pango_font_map_add_font_file(map, font_path.c_str(), &error);
    if (error) g_error_free(error);
    if (!ok) return false;

    std::string internal_family;
    auto after = pango_family_map(map);
    for (const auto& kv : after) {
        if (before.find(kv.first) == before.end()) {
            internal_family = kv.second;
            break;
        }
    }
    if (internal_family.empty()) internal_family = face.family;
    family_aliases_[family_key(face.family)] = internal_family;
    installed_any_font_ = true;
    pango_font_map_changed(map);
    return true;
#else
    (void)face;
    (void)font_path;
    return false;
#endif
}

void WebFontManager::notify_font_maps_changed() {
    PangoFontMap* map = pango_cairo_font_map_get_default();
    if (!map) return;
    if (PANGO_IS_FC_FONT_MAP(map)) {
        auto* fc_map = PANGO_FC_FONT_MAP(map);
        pango_fc_font_map_config_changed(fc_map);
        pango_fc_font_map_cache_clear(fc_map);
    }
    pango_font_map_changed(map);
}

void WebFontManager::cleanup() {
    if (installed_any_font_ && fontconfig_available_) {
        if (FcConfig* config = FcConfigGetCurrent()) {
            FcConfigAppFontClear(config);
            notify_font_maps_changed();
        }
    }
    installed_any_font_ = false;

    if (!temp_dir_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
        temp_dir_.clear();
    }
}

std::string WebFontManager::ensure_temp_dir() {
    if (!temp_dir_.empty()) return temp_dir_;

    std::error_code ec;
    const auto base = std::filesystem::temp_directory_path(ec);
    if (ec) return "";

    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::filesystem::path dir =
            base / ("html2pdf-fonts-" + std::to_string(now) + "-" + std::to_string(attempt));
        if (std::filesystem::create_directory(dir, ec)) {
            temp_dir_ = dir.string();
            return temp_dir_;
        }
        if (ec) ec.clear();
    }
    return "";
}

}  // namespace html2pdf

#endif  // HTML2PDF_USE_PANGO
