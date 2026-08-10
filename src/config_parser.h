#pragma once
/*  CLI.MUSIC.COM — config_parser.h  v3.0
 *
 *  Parses the user-editable config.txt (100% control interface).
 *  Loaded on startup, merged over the runtime settings.conf.
 *
 *  Supported config.txt syntax:
 *
 *    ## comment line
 *    KEY=value                          # simple key-value
 *    KEY="value"                        # quoted value
 *    BLOCK_KEY={                        # multi-line block
 *      line1 \
 *      line2
 *    };
 *    BLOCK_KEY={ [A][B], \ [C] };       # inline block
 */

#include "settings.h"
#include <string>
#include <fstream>
#include <sstream>
#include <map>
#include <vector>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

class ConfigParser {
public:
    // Parse config.txt and overlay its values onto `s`.
    // Returns false if file not found (s is unchanged).
    static bool parse(const std::string& path, Settings& s) {
        std::ifstream f(path);
        if (!f.is_open()) {
            // Try executable-adjacent config.txt
            auto alt = fs::path(path).parent_path() / "config.txt";
            f.open(alt.string());
            if (!f.is_open()) return false;
        }
        std::string src((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        parse_string(src, s);
        return true;
    }

    // Locate config.txt in standard paths.
    static std::string find_config() {
        // 1. ~/.config/climusic/config.txt
        const char* home = getenv("HOME");
        if (!home) home = "/data/data/com.termux/files/home";
        std::string p1 = std::string(home) + "/.config/climusic/config.txt";
        if (fs::exists(p1)) return p1;
        // 2. current directory
        if (fs::exists("config.txt")) return "config.txt";
        // 3. one level up
        if (fs::exists("../config.txt")) return "../config.txt";
        return "";
    }

    // Writes the settings-panel-editable subset of `s` (colors, viz ints,
    // layout mode, theme, hotkeys) back into the real config.txt at `path`
    // — non-destructively: existing lines for keys we manage are replaced
    // in place, keys not yet present are appended under a marked section,
    // and everything else (comments, the layout DSL, border chars, font
    // map, MUSIC_DIR, etc.) is left completely untouched. Returns false if
    // `path` can't be read or written.
    //
    // Not covered: font_map (font_en block) — rarely edited and awkward to
    // regenerate losslessly; left as whatever's already in the file.
    static bool write_back(const std::string& path, const Settings& s) {
        std::ifstream in(path);
        if (!in.is_open()) return false;
        std::vector<std::string> lines;
        {
            std::string line;
            while (std::getline(in, line)) lines.push_back(line);
        }
        in.close();

        std::map<std::string, std::string> managed = build_managed_kv(s);

        // Replace in place wherever a managed key already has a line.
        for (auto& line : lines) {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            auto ws = key.find_first_not_of(" \t");
            auto we = key.find_last_not_of(" \t");
            key = (ws == std::string::npos) ? "" : key.substr(ws, we - ws + 1);
            auto it = managed.find(key);
            if (it != managed.end()) {
                line = it->first + "=" + it->second;
                managed.erase(it);   // consumed — whatever's left gets appended below
            }
        }

        if (!managed.empty()) {
            lines.push_back("");
            lines.push_back("## ── Saved from the in-app settings panel ──────────────────────────");
            for (auto& [k, v] : managed) lines.push_back(k + "=" + v);
        }

        std::ofstream out(path, std::ios::trunc);
        if (!out.is_open()) return false;
        for (auto& line : lines) out << line << '\n';
        return true;
    }

private:
    static std::string color_kv(const Color& c) {
        return std::to_string(c.ansi) + "," + std::to_string(c.brightness);
    }

    static std::map<std::string, std::string> build_managed_kv(const Settings& s) {
        std::map<std::string, std::string> m;

        m["viz_style"]   = std::to_string((int)s.viz_style);
        m["viz_bands"]   = std::to_string(s.viz_bands);
        m["peak_hold"]   = std::to_string((int)s.peak_hold);
        m["viz_density"] = std::to_string(s.viz_density);
        m["layout_mode"] = std::to_string((int)s.layout_mode);
        m["theme_idx"]   = std::to_string(s.theme_idx);
        m["STREAM_VISUALIZER_ENABLED"] = std::to_string((int)s.stream_visualizer);
        m["auto_save_config"]          = std::to_string((int)s.auto_save_config);
        m["stream_search_results"]     = std::to_string(s.stream_search_results);

        m["border"]         = color_kv(s.colors.border);
        m["title"]          = color_kv(s.colors.title);
        m["meta_key"]       = color_kv(s.colors.meta_key);
        m["meta_val"]       = color_kv(s.colors.meta_val);
        m["progress"]       = color_kv(s.colors.progress);
        m["progress_bg"]    = color_kv(s.colors.progress_bg);
        m["status"]         = color_kv(s.colors.status);
        m["playlist"]       = color_kv(s.colors.playlist);
        m["pl_active_fg"]   = color_kv(s.colors.pl_active_fg);
        m["pl_active_bg"]   = color_kv(s.colors.pl_active_bg);
        m["viz"]            = color_kv(s.colors.viz);
        m["viz_left"]       = color_kv(s.colors.viz_left);
        m["viz_center"]     = color_kv(s.colors.viz_center);
        m["viz_right"]      = color_kv(s.colors.viz_right);
        m["viz_peak"]       = color_kv(s.colors.viz_peak);
        m["lyr_dim"]        = color_kv(s.colors.lyr_dim);
        m["lyr_hi"]         = color_kv(s.colors.lyr_hi);
        m["lyr_word"]       = color_kv(s.colors.lyr_word);
        m["noise"]          = color_kv(s.colors.noise);
        m["queue_item"]     = color_kv(s.colors.queue_item);
        m["queue_active"]   = color_kv(s.colors.queue_active);

        auto hk = [&](const char* key, int code) {
            std::string v = KeyMap::format_key(code);
            if (v != "?") m[key] = "\"" + v + "\"";   // "?" = unrepresentable, skip (leaves existing line as-is)
        };
        hk("HKEY_SETTING",                                 s.keys.setting);
        hk("HKEY_NAVIGATE_UP",                             s.keys.nav_up);
        hk("HKEY_NAVIGATE_DOWN",                           s.keys.nav_down);
        hk("HKEY_PRESS_TO_PLAY",                           s.keys.play);
        hk("HKEY_PRESS_TO_SEARCH",                         s.keys.search);
        hk("HKEY_PRESS_TO_PLAY_NEXT_SONG",                 s.keys.next_song);
        hk("HKEY_PRESS_TO_PLAY_PREVIOUS_SONG",             s.keys.prev_song);
        hk("HKEY_PRESS_TO_SEEK_FORWARD",                   s.keys.seek_fwd);
        hk("HKEY_PRESS_TO_SEEK_BACKWARD",                  s.keys.seek_bwd);
        hk("HKEY_PRESS_TO_INCRESE_VOLUME",                 s.keys.vol_up);
        hk("HKEY_PRESS_TO_DECRESE_VOLUME",                 s.keys.vol_down);
        hk("HKEY_PRESS_TO_ADD_HOVERING_SONG_TO_QUEUE",     s.keys.queue_add);
        hk("HKEY_PRESS_TO_REMOVE_HOVERING_SONG_FROK_QUEUE",s.keys.queue_remove);
        hk("HKEY_PRESS_TO_SWITCH_BETWEEN_CARDS",           s.keys.tab_switch);
        hk("KHEY_PRESS_TO_TOGGLE_REPEAT",                  s.keys.toggle_repeat);
        hk("HKEY_PRESS_TO_TOGGLE_PLAY_PAUSE",              s.keys.play_pause);
        hk("HKEY_PRESS_TO_TOGGLE_SHUFFLE",                 s.keys.toggle_shuffle);
        hk("HKEY_PRESS_TO_FILTER_FOR_FOLDER",              s.keys.folder_filter);
        hk("HKEY_PRESS_TO_CLEAR_FILTER",                   s.keys.clear_filter);
        hk("HKEY_PRESS_TO_QUIT",                           s.keys.quit);
        hk("HKEY_PRESS_TO_RESET_PREFRENCE",                s.keys.reset_prefs);
        hk("HKEY_PRESS_TO_DOWNLOAD_STREAM",                s.keys.download_stream);

        return m;
    }


    // ── Strip comments ────────────────────────────────────────────────────
    static std::string strip_comments(const std::string& src) {
        std::string out;
        out.reserve(src.size());
        for (size_t i = 0; i < src.size(); ++i) {
            if (src[i] == '#' && (i == 0 || src[i-1] == '\n')) {
                // Skip to end of line
                while (i < src.size() && src[i] != '\n') ++i;
            } else {
                out += src[i];
            }
        }
        return out;
    }

    // ── Extract all block and simple key-values ───────────────────────────
    //  Returns map<key, content_string>
    //  Block content = everything between { and matching }
    static std::map<std::string,std::string> extract_kv(const std::string& src) {
        std::map<std::string,std::string> kv;
        size_t i = 0;
        while (i < src.size()) {
            // Skip whitespace
            while (i < src.size() && isspace((unsigned char)src[i])) ++i;
            if (i >= src.size()) break;

            // Read key (up to '=')
            size_t key_start = i;
            while (i < src.size() && src[i] != '=' && src[i] != '\n') ++i;
            if (i >= src.size() || src[i] != '=') { ++i; continue; }

            std::string key = src.substr(key_start, i - key_start);
            // trim key
            {
                auto s = key.find_first_not_of(" \t");
                auto e = key.find_last_not_of(" \t");
                key = (s == std::string::npos) ? "" : key.substr(s, e - s + 1);
            }
            if (key.empty()) { ++i; continue; }
            ++i; // skip '='

            // Skip whitespace after '='
            while (i < src.size() && (src[i] == ' ' || src[i] == '\t')) ++i;

            // Block value?
            if (i < src.size() && src[i] == '{') {
                ++i;
                int depth = 1;
                size_t start = i;
                while (i < src.size() && depth > 0) {
                    if (src[i] == '{') ++depth;
                    else if (src[i] == '}') --depth;
                    if (depth > 0) ++i; else break;
                }
                kv[key] = src.substr(start, i - start);
                // skip to ';' or newline
                while (i < src.size() && src[i] != '\n') ++i;
            } else if (i < src.size() && src[i] == '"') {
                // Quoted string
                ++i;
                size_t start = i;
                while (i < src.size() && src[i] != '"') ++i;
                kv[key] = src.substr(start, i - start);
                if (i < src.size()) ++i;
            } else {
                // Simple value — read to end of line
                size_t start = i;
                while (i < src.size() && src[i] != '\n') ++i;
                std::string val = src.substr(start, i - start);
                // trim trailing whitespace and ;
                auto e = val.find_last_not_of(" \t;");
                kv[key] = (e == std::string::npos) ? "" : val.substr(0, e + 1);
            }
            ++i;
        }
        return kv;
    }

    // ── Parse row-heights block ───────────────────────────────────────────
    // "ROW_1=10\nROW_2=8\n..." -> {10, 8, ...}
    static std::vector<int> parse_row_heights(const std::string& block) {
        std::map<int, int> hmap;
        std::istringstream ss(block);
        std::string line;
        while (std::getline(ss, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            // k should be ROW_N
            auto under = k.find('_');
            if (under == std::string::npos) continue;
            try {
                int n = std::stoi(k.substr(under + 1));
                int h = std::stoi(v);
                hmap[n] = h;
            } catch (...) {}
        }
        if (hmap.empty()) return {};
        int max_n = hmap.rbegin()->first;
        std::vector<int> out(max_n, 3);
        for (auto& [n, h] : hmap)
            if (n >= 1 && n <= max_n) out[n-1] = h;
        return out;
    }

    // ── Parse font_en block ───────────────────────────────────────────────
    // "A={𝓐,𝓪},\nB={𝓑,𝓫}..." -> FontMap
    static FontMap parse_font_map(const std::string& block) {
        FontMap fm;
        // Each entry: CHAR={upper,lower},
        // We extract token by token
        size_t i = 0;
        while (i < block.size()) {
            while (i < block.size() && isspace((unsigned char)block[i])) ++i;
            if (i >= block.size()) break;
            // Read key char (single ASCII)
            char key_char = block[i];
            ++i;
            // Skip to '='
            while (i < block.size() && block[i] != '=') ++i;
            if (i >= block.size()) break;
            ++i; // skip =
            while (i < block.size() && (block[i] == ' ' || block[i] == '\t')) ++i;
            if (i >= block.size() || block[i] != '{') { ++i; continue; }
            ++i; // skip {
            // Read until matching }
            size_t start = i;
            while (i < block.size() && block[i] != '}') ++i;
            std::string inner = block.substr(start, i - start);
            if (i < block.size()) ++i; // skip }
            // Skip trailing comma
            while (i < block.size() && (block[i] == ',' || block[i] == ' ')) ++i;

            // inner = "UP,lo" — split on first comma
            auto comma = inner.find(',');
            std::string upper_val = (comma == std::string::npos)
                ? inner : inner.substr(0, comma);
            std::string lower_val = (comma == std::string::npos)
                ? inner : inner.substr(comma + 1);

            // Map the uppercase ASCII key to upper_val
            if (!upper_val.empty()) fm[(char)toupper((unsigned char)key_char)] = upper_val;
            // Map the lowercase ASCII key to lower_val
            if (!lower_val.empty()) fm[(char)tolower((unsigned char)key_char)] = lower_val;
        }
        return fm;
    }

    // ── Main parse ────────────────────────────────────────────────────────
    static void parse_string(const std::string& raw, Settings& s) {
        std::string src = strip_comments(raw);
        auto kv = extract_kv(src);

        auto has = [&](const std::string& k) { return kv.count(k) > 0; };
        auto gi  = [&](const std::string& k, int def) -> int {
            if (!has(k)) return def;
            try { return std::stoi(kv[k]); } catch (...) { return def; }
        };
        auto gc  = [&](const std::string& k, Color def) -> Color {
            if (!has(k)) return def;
            auto& v = kv[k]; auto cm = v.find(',');
            if (cm == std::string::npos) return def;
            try { return Color(std::stoi(v.substr(0,cm)), std::stoi(v.substr(cm+1))); }
            catch (...) { return def; }
        };

        // ── Visualiser ────────────────────────────────────────────────────
        if (has("viz_style")) {
            int vs = std::clamp(gi("viz_style", 0), 0, VIZ_STYLE_COUNT - 1);
            s.viz_style = (VizStyle)vs;
        }
        if (has("viz_bands"))   s.viz_bands   = (gi("viz_bands",32) >= 32) ? 32 : 16;
        if (has("peak_hold"))   s.peak_hold   = (gi("peak_hold",1) != 0);
        if (has("viz_density")) s.viz_density = std::clamp(gi("viz_density",5), 1, 10);
        if (has("STREAM_VISUALIZER_ENABLED"))
            s.stream_visualizer = (gi("STREAM_VISUALIZER_ENABLED", 1) != 0);
        if (has("auto_save_config"))
            s.auto_save_config = (gi("auto_save_config", 0) != 0);
        if (has("stream_search_results"))
            s.stream_search_results = std::clamp(gi("stream_search_results", 30), 5, 50);

        // ── Layout ────────────────────────────────────────────────────────
        if (has("DESK_MODE"))
            s.desk_layout = kv["DESK_MODE"];
        if (has("MOBILE_MODE"))
            s.mobile_layout = kv["MOBILE_MODE"];
        if (has("DESK_MODE_ROW")) {
            auto rh = parse_row_heights(kv["DESK_MODE_ROW"]);
            if (!rh.empty()) s.desk_row_h = rh;
        }
        if (has("MOBILE_MODE_ROW")) {
            auto rh = parse_row_heights(kv["MOBILE_MODE_ROW"]);
            if (!rh.empty()) s.mobile_row_h = rh;
        }

        // ── Border characters ─────────────────────────────────────────────
        auto bs = [&](const std::string& k, std::string& target) {
            if (has(k) && !kv[k].empty()) target = kv[k];
        };
        bs("upper_left_corner",  s.border_chars.top_left);
        bs("upper_right_corner", s.border_chars.top_right);
        bs("bottom_left_corner", s.border_chars.bot_left);
        bs("lower_right_corner", s.border_chars.bot_right);
        bs("horizontal",         s.border_chars.horiz);
        bs("vertical",           s.border_chars.vert);

        // ── Colors ────────────────────────────────────────────────────────
        s.colors.border       = gc("border",       s.colors.border);
        s.colors.title        = gc("title",         s.colors.title);
        s.colors.meta_key     = gc("meta_key",      s.colors.meta_key);
        s.colors.meta_val     = gc("meta_val",      s.colors.meta_val);
        s.colors.progress     = gc("progress",      s.colors.progress);
        s.colors.progress_bg  = gc("progress_bg",   s.colors.progress_bg);
        s.colors.status       = gc("status",        s.colors.status);
        s.colors.playlist     = gc("playlist",      s.colors.playlist);
        s.colors.pl_active_fg = gc("pl_active_fg",  s.colors.pl_active_fg);
        s.colors.pl_active_bg = gc("pl_active_bg",  s.colors.pl_active_bg);
        s.colors.viz          = gc("viz",           s.colors.viz);
        s.colors.viz_left     = gc(has("viz_left")   ? "viz_left"   : "viz_bass",   s.colors.viz_left);
        s.colors.viz_center   = gc(has("viz_center") ? "viz_center" : "viz_mid",    s.colors.viz_center);
        s.colors.viz_right    = gc(has("viz_right")  ? "viz_right"  : "viz_treble", s.colors.viz_right);
        s.colors.viz_peak     = gc("viz_peak",      s.colors.viz_peak);
        s.colors.lyr_dim      = gc("lyr_dim",       s.colors.lyr_dim);
        s.colors.lyr_hi       = gc("lyr_hi",        s.colors.lyr_hi);
        s.colors.lyr_word     = gc("lyr_word",      s.colors.lyr_word);
        s.colors.noise        = gc("noise",         s.colors.noise);

        // Theme index
        if (has("theme_idx"))
            s.theme_idx = std::clamp(gi("theme_idx",0), 0, THEME_COUNT-1);

        // ── Font map ──────────────────────────────────────────────────────
        if (has("font_en")) {
            auto fm = parse_font_map(kv["font_en"]);
            for (auto& [k, v] : fm) s.font_map[k] = v;
        }

        // ── Hotkeys ───────────────────────────────────────────────────────
        auto hk = [&](const std::string& k, int& target) {
            if (has(k)) target = KeyMap::parse_key(kv[k]);
        };
        hk("HKEY_SETTING",                              s.keys.setting);
        hk("HKEY_NAVIGATE_UP",                          s.keys.nav_up);
        hk("HKEY_NAVIGATE_DOWN",                        s.keys.nav_down);
        hk("HKEY_PRESS_TO_PLAY",                        s.keys.play);
        hk("HKEY_PRESS_TO_SEARCH",                      s.keys.search);
        hk("HKEY_PRESS_TO_PLAY_NEXT_SONG",              s.keys.next_song);
        hk("HKEY_PRESS_TO_PLAY_PREVIOUS_SONG",          s.keys.prev_song);
        hk("HKEY_PRESS_TO_SEEK_FORWARD",                s.keys.seek_fwd);
        hk("HKEY_PRESS_TO_SEEK_BACKWARD",               s.keys.seek_bwd);
        hk("HKEY_PRESS_TO_INCRESE_VOLUME",              s.keys.vol_up);
        hk("HKEY_PRESS_TO_DECRESE_VOLUME",              s.keys.vol_down);
        hk("HKEY_PRESS_TO_ADD_HOVERING_SONG_TO_QUEUE",  s.keys.queue_add);
        hk("HKEY_PRESS_TO_REMOVE_HOVERING_SONG_FROK_QUEUE", s.keys.queue_remove);
        hk("HKEY_PRESS_TO_SWITCH_BETWEEN_CARDS",        s.keys.tab_switch);
        hk("KHEY_PRESS_TO_TOGGLE_REPEAT",               s.keys.toggle_repeat);
        hk("HKEY_PRESS_TO_TOGGLE_PLAY_PAUSE",           s.keys.play_pause);
        hk("HKEY_PRESS_TO_TOGGLE_SHUFFLE",              s.keys.toggle_shuffle);
        hk("HKEY_PRESS_TO_FILTER_FOR_FOLDER",           s.keys.folder_filter);
        hk("HKEY_PRESS_TO_CLEAR_FILTER",                s.keys.clear_filter);
        hk("HKEY_PRESS_TO_QUIT",                        s.keys.quit);
        hk("HKEY_PRESS_TO_RESET_PREFRENCE",             s.keys.reset_prefs);
        hk("HKEY_PRESS_TO_DOWNLOAD_STREAM",             s.keys.download_stream);
    }
};
