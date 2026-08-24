#pragma once
#include <ncurses.h>
/*  CLI.MUSIC.COM — settings.h  v3.0
 *  Changes from v2.1:
 *    - Only 2 visualiser styles: BARS (0) and SCOPE (1)
 *    - KeyMap: fully configurable hotkeys
 *    - FontMap: per-character unicode substitution
 *    - BorderChars: configurable box-drawing characters
 *    - LayoutMode: AUTO / DESKTOP / MOBILE
 *    - Layout DSL strings stored here (loaded from config.txt or defaults)
 */

#include <string>
#include <fstream>
#include <map>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <cmath>
#include <array>
#include <functional>

namespace fs = std::filesystem;

// ─── Visualiser (2 styles only) ──────────────────────────────────────────────
enum class VizStyle { BARS = 0, SCOPE = 1 };
static constexpr int VIZ_STYLE_COUNT = 2;
static constexpr const char* VIZ_STYLE_NAMES[VIZ_STYLE_COUNT] = {
    "BARS (braille)", "SCOPE (oscilloscope)"
};

// ─── Color ────────────────────────────────────────────────────────────────────
struct Color {
    int ansi = 255, brightness = 100;
    Color() = default;
    Color(int a, int b) noexcept : ansi(a), brightness(b) {}
};

struct ColorScheme {
    Color border       = {39,  100};
    Color title        = {231, 100};
    Color meta_key     = {153, 100};
    Color meta_val     = {123, 100};
    Color progress     = {82,  100};
    Color progress_bg  = {234, 100};
    Color status       = {252,  80};
    Color playlist     = {253, 100};
    Color pl_active_fg = {231, 100};
    Color pl_active_bg = {27,  100};
    Color viz          = {51,  100};
    // Named for what they visually are (left/center/right screen zones of
    // the bar visualizer), not frequency bands — the code was never doing
    // real bass/mid/treble frequency-based coloring, just column position.
    Color viz_left     = {196, 100};
    Color viz_center   = {82,  100};
    Color viz_right    = {45,  100};
    Color viz_peak     = {226, 100};
    Color lyr_dim      = {253,  60};
    Color lyr_hi       = {226, 100};
    Color lyr_word     = {214, 100};
    Color noise        = {48,   80};
    Color queue_item   = {189, 100};
    Color queue_active = {51,  100};
};

// ─── Themes ───────────────────────────────────────────────────────────────────
struct Theme { const char* name; ColorScheme scheme; };
static const Theme THEMES[] = {
    { "Default",   { {39,100},{231,100},{153,100},{123,100},{82,100},{234,100},{252,80},
                     {253,100},{231,100},{27,100},{51,100},{196,100},{82,100},{45,100},{226,100},
                     {253,60},{226,100},{214,100},{48,80},{189,100},{51,100} } },
    { "Ocean",     { {33,100},{255,100},{117,100},{87,100},{45,100},{17,100},{252,70},
                     {195,100},{15,100},{19,100},{39,100},{27,100},{45,100},{51,100},{159,100},
                     {195,50},{159,100},{123,100},{38,70},{159,100},{45,100} } },
    { "Forest",    { {34,100},{255,100},{114,100},{156,100},{40,100},{22,100},{248,70},
                     {193,100},{255,100},{22,100},{76,100},{160,100},{40,100},{118,100},{148,100},
                     {193,50},{184,100},{214,100},{34,70},{148,100},{40,100} } },
    { "Sunset",    { {202,100},{231,100},{216,100},{220,100},{196,100},{52,100},{252,70},
                     {223,100},{255,100},{88,100},{214,100},{196,100},{202,100},{208,100},{220,100},
                     {223,50},{226,100},{196,100},{202,70},{220,100},{196,100} } },
    { "Midnight",  { {93,100},{255,100},{141,100},{183,100},{129,100},{17,100},{248,60},
                     {189,100},{255,100},{54,100},{135,100},{129,100},{93,100},{183,100},{219,100},
                     {189,50},{219,100},{207,100},{91,60},{183,100},{93,100} } },
    { "Neon",      { {201,100},{255,100},{51,100},{118,100},{46,100},{232,100},{252,70},
                     {255,100},{0,100},{201,100},{196,100},{201,100},{46,100},{226,100},{51,100},
                     {255,50},{226,100},{196,100},{46,70},{118,100},{46,100} } },
};
static constexpr int THEME_COUNT = 6;

// ─── Color helpers ────────────────────────────────────────────────────────────
inline void ansi256_to_rgb1000(int code, short& r, short& g, short& b) noexcept {
    code = std::clamp(code, 0, 255);
    if (code < 16) {
        static constexpr short T[16][3] = {
            {0,0,0},{800,0,0},{0,800,0},{800,500,0},{0,0,800},{800,0,800},
            {0,800,800},{753,753,753},{500,500,500},{1000,333,333},{333,1000,333},
            {1000,1000,333},{333,333,1000},{1000,333,1000},{333,1000,1000},{1000,1000,1000}
        };
        r=T[code][0]; g=T[code][1]; b=T[code][2];
    } else if (code < 232) {
        int i=code-16, bv=i%6; i/=6; int gv=i%6; i/=6; int rv=i%6;
        static constexpr short lut[]={0,373,529,686,843,1000};
        r=lut[rv]; g=lut[gv]; b=lut[bv];
    } else {
        short v=(short)(31+39*(code-232)); r=g=b=v;
    }
}
inline void apply_brightness(short& r, short& g, short& b, int brightness) noexcept {
    float f = std::clamp(brightness, 0, 100) / 100.0f;
    r=(short)(r*f); g=(short)(g*f); b=(short)(b*f);
}

// ─── Border characters ────────────────────────────────────────────────────────
struct BorderChars {
    std::string top_left     = "\xe2\x95\xad";  // ╭
    std::string top_right    = "\xe2\x95\xae";  // ╮
    std::string bot_left     = "\xe2\x95\xb0";  // ╰
    std::string bot_right    = "\xe2\x95\xaf";  // ╯
    std::string horiz        = "\xe2\x94\x80";  // ─
    std::string vert         = "\xe2\x94\x82";  // │
    std::string hdr_fill     = "\xe2\x96\x92";  // ▒ (header decoration)
};

// ─── Font map ─────────────────────────────────────────────────────────────────
// Maps an ASCII char to a (possibly multi-byte) display string.
// Apply via font_translate() before rendering any label text.
using FontMap = std::map<char, std::string>;

inline std::string font_translate(const std::string& s, const FontMap& fm) {
    if (fm.empty()) return s;
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        auto it = fm.find((char)c);
        if (it != fm.end()) out += it->second;
        else                 out += (char)c;
    }
    return out;
}

// ─── Key map ──────────────────────────────────────────────────────────────────
// Keys are stored as ncurses int codes.  String → int resolved at load time.
struct KeyMap {
    int setting      = 's';
    int nav_up       = KEY_UP;
    int nav_down     = KEY_DOWN;
    int play         = '\n';
    int search       = '/';
    int next_song    = 'n';
    int prev_song    = 'b';
    int seek_fwd     = KEY_RIGHT;
    int seek_bwd     = KEY_LEFT;
    int vol_up       = '1';
    int vol_down     = '2';
    int queue_add    = 'a';
    int queue_remove = 'd';
    int tab_switch   = '\t';
    int toggle_repeat  = 'r';
    int play_pause   = 'p';
    int toggle_shuffle = 'm';
    int folder_filter  = 'f';
    int clear_filter   = 'c';
    int quit         = 'q';
    int reset_prefs  = 'e';
    int jump_queue   = 'Q';   // 'Q' jumps focus to queue panel
    int download_stream = 'y';   // download the currently-playing stream via yt-dlp

    // Parse a key string like "S", "ARROW_KEY_UP", "→", "ENTER", etc.
    static int parse_key(const std::string& s) {
        if (s.empty()) return ERR;
        if (s == "ARROW_KEY_UP"    || s == "UP")    return KEY_UP;
        if (s == "ARROW_KEY_DOWN"  || s == "DOWN")  return KEY_DOWN;
        if (s == "ARROW_KEY_LEFT"  || s == "LEFT")  return KEY_LEFT;
        if (s == "ARROW_KEY_RIGHT" || s == "RIGHT") return KEY_RIGHT;
        if (s == "ENTER" || s == "RETURN")          return '\n';
        if (s == "TAB")                             return '\t';
        if (s == "SPACE")                           return ' ';
        if (s == "BACKSPACE")                       return KEY_BACKSPACE;
        if (s == "ESC")                             return 27;
        // Arrow unicode glyphs
        if (s == "→") return KEY_RIGHT;
        if (s == "←") return KEY_LEFT;
        if (s == "↑") return KEY_UP;
        if (s == "↓") return KEY_DOWN;
        // Single printable character
        if (s.size() == 1) return (int)(unsigned char)s[0];
        // First char of quoted string
        return (int)(unsigned char)s[0];
    }

    // Inverse of parse_key() — used when writing settings-panel changes
    // back out to config.txt, so a saved key round-trips to the same code.
    static std::string format_key(int code) {
        if (code == KEY_UP)        return "ARROW_KEY_UP";
        if (code == KEY_DOWN)      return "ARROW_KEY_DOWN";
        if (code == KEY_LEFT)      return "ARROW_KEY_LEFT";
        if (code == KEY_RIGHT)     return "ARROW_KEY_RIGHT";
        if (code == '\n')          return "ENTER";
        if (code == '\t')          return "TAB";
        if (code == ' ')           return "SPACE";
        if (code == KEY_BACKSPACE) return "BACKSPACE";
        if (code == 27)            return "ESC";
        if (code >= 32 && code < 127) return std::string(1, (char)code);
        return "?";   // unrepresentable — leave the existing config.txt value alone
    }
};

// ─── Layout mode ─────────────────────────────────────────────────────────────
enum class LayoutMode { AUTO = 0, DESKTOP, MOBILE };
static constexpr int DESKTOP_WIDTH_THRESHOLD = 120;

// Default layout strings (used if config.txt not found)
static const std::string DEFAULT_DESK_LAYOUT =
    "[LIST][METADATA][ICON][LYRICS],\\"
    "[UP][VOCAL-VIZ],\\"
    "[UP][VIZ],\\"
    "[PROGRESS-BAR],\\"
    "[QUEUE]";

static const std::string DEFAULT_MOBILE_LAYOUT =
    "[METADATA][ICON][LYRICS],\\"
    "[VOCAL-VIZ][LEFT][UP],\\"
    "[PROGRESS-BAR],\\"
    "[VIZ],\\"
    "[QUEUE],\\"
    "[HELP]";

static const std::vector<int> DEFAULT_DESK_ROW_H   = {12, 8, 12, 3, 8};
static const std::vector<int> DEFAULT_MOBILE_ROW_H = {8, 9, 3, 12, 8, 7};

// ─── Settings ─────────────────────────────────────────────────────────────────
struct Settings {
    static constexpr int VERSION = 300;

    // Visualiser
    VizStyle    viz_style   = VizStyle::BARS;
    int         viz_bands   = 32;
    bool        peak_hold   = true;
    int         viz_density = 5;
    // Live visualizer during yt: streaming needs a second, independent
    // yt-dlp+ffmpeg audio fetch purely to get PCM for the FFT (the actual
    // playback audio goes through mpv/ffplay separately) — real extra
    // bandwidth/battery cost. Default on; let people turn it off.
    bool        stream_visualizer = true;
    // When on, every settings-panel edit is also written straight into the
    // real config.txt (not just this internal session file) — see
    // ConfigParser::write_back(). Off by default: settings-panel changes
    // stay session-only unless explicitly opted into persisting.
    bool        auto_save_config = false;
    // How many results a `yt:` search asks yt-dlp for / shows.
    int         stream_search_results = 30;
    // Path to the local music library.
    std::string music_dir = "/sdcard/music/";

    // Layout
    LayoutMode  layout_mode = LayoutMode::AUTO;
    std::string desk_layout = DEFAULT_DESK_LAYOUT;
    std::string mobile_layout = DEFAULT_MOBILE_LAYOUT;
    std::vector<int> desk_row_h   = DEFAULT_DESK_ROW_H;
    std::vector<int> mobile_row_h = DEFAULT_MOBILE_ROW_H;

    // Theme + colors
    int         theme_idx   = 0;
    ColorScheme colors;

    // Border, fonts, keys
    BorderChars border_chars;
    FontMap     font_map;
    KeyMap      keys;

    void apply_theme(int idx) {
        if (idx < 0 || idx >= THEME_COUNT) return;
        theme_idx = idx;
        colors = THEMES[idx].scheme;
    }

    [[nodiscard]] static std::string config_path() {
        const char* home = getenv("HOME");
        if (!home) home = "/data/data/com.termux/files/home";
        return std::string(home) + "/.config/climusic/settings.conf";
    }

    void save() const {
        std::string path = config_path();
        fs::create_directories(fs::path(path).parent_path());
        std::ofstream f(path);
        if (!f.is_open()) return;
        auto wc = [&](const char* k, const Color& c) {
            f << k << '=' << c.ansi << ',' << c.brightness << '\n';
        };
        f << "version="     << VERSION          << '\n';
        f << "viz_style="   << (int)viz_style   << '\n';
        f << "viz_bands="   << viz_bands        << '\n';
        f << "peak_hold="   << (int)peak_hold   << '\n';
        f << "viz_density=" << viz_density      << '\n';
        f << "stream_visualizer=" << (int)stream_visualizer << '\n';
        f << "auto_save_config=" << (int)auto_save_config << '\n';
        f << "stream_search_results=" << stream_search_results << '\n';
        f << "MUSIC_DIR=" << music_dir << '\n';
        f << "layout_mode=" << (int)layout_mode << '\n';
        f << "theme_idx="   << theme_idx        << '\n';
        wc("border",        colors.border);
        wc("title",         colors.title);
        wc("meta_key",      colors.meta_key);
        wc("meta_val",      colors.meta_val);
        wc("progress",      colors.progress);
        wc("progress_bg",   colors.progress_bg);
        wc("status",        colors.status);
        wc("playlist",      colors.playlist);
        wc("pl_active_fg",  colors.pl_active_fg);
        wc("pl_active_bg",  colors.pl_active_bg);
        wc("viz",           colors.viz);
        wc("viz_left",      colors.viz_left);
        wc("viz_center",    colors.viz_center);
        wc("viz_right",     colors.viz_right);
        wc("viz_peak",      colors.viz_peak);
        wc("lyr_dim",       colors.lyr_dim);
        wc("lyr_hi",        colors.lyr_hi);
        wc("lyr_word",      colors.lyr_word);
        wc("noise",         colors.noise);
        wc("queue_item",    colors.queue_item);
        wc("queue_active",  colors.queue_active);
    }

    void load() {
        std::ifstream f(config_path());
        if (!f.is_open()) return;
        std::map<std::string,std::string> kv;
        std::string line;
        while (std::getline(f, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            kv[line.substr(0, eq)] = line.substr(eq + 1);
        }
        auto gi = [&](const char* k, int def) -> int {
            auto it = kv.find(k);
            if (it == kv.end()) return def;
            try { return std::stoi(it->second); } catch (...) { return def; }
        };
        auto gc = [&](const char* k, Color def) -> Color {
            auto it = kv.find(k);
            if (it == kv.end()) return def;
            auto cm = it->second.find(',');
            if (cm == std::string::npos) return def;
            try { return Color(std::stoi(it->second.substr(0,cm)),
                               std::stoi(it->second.substr(cm+1))); }
            catch (...) { return def; }
        };
        // Reads `k`, falling back to `legacy_k` for configs saved before a
        // key was renamed (e.g. viz_bass/mid/treble -> viz_left/center/right).
        auto gc2 = [&](const char* k, const char* legacy_k, Color def) -> Color {
            if (kv.count(k)) return gc(k, def);
            if (kv.count(legacy_k)) return gc(legacy_k, def);
            return def;
        };

        int raw_vs = std::clamp(gi("viz_style", 0), 0, VIZ_STYLE_COUNT - 1);
        viz_style   = (VizStyle)raw_vs;
        viz_bands   = (gi("viz_bands",  32) >= 32) ? 32 : 16;
        peak_hold   = (gi("peak_hold",   1) != 0);
        viz_density = std::clamp(gi("viz_density", 5), 1, 10);
        stream_visualizer = (gi("stream_visualizer", 1) != 0);
        auto_save_config  = (gi("auto_save_config", 0) != 0);
        stream_search_results = std::clamp(gi("stream_search_results", 30), 5, 50);
        if (kv.count("MUSIC_DIR")) music_dir = kv["MUSIC_DIR"];
        layout_mode = (LayoutMode)std::clamp(gi("layout_mode", 0), 0, 2);
        theme_idx   = std::clamp(gi("theme_idx",   0), 0, THEME_COUNT - 1);

        colors.border       = gc("border",       colors.border);
        colors.title        = gc("title",         colors.title);
        colors.meta_key     = gc("meta_key",      colors.meta_key);
        colors.meta_val     = gc("meta_val",      colors.meta_val);
        colors.progress     = gc("progress",      colors.progress);
        colors.progress_bg  = gc("progress_bg",   colors.progress_bg);
        colors.status       = gc("status",        colors.status);
        colors.playlist     = gc("playlist",      colors.playlist);
        colors.pl_active_fg = gc("pl_active_fg",  colors.pl_active_fg);
        colors.pl_active_bg = gc("pl_active_bg",  colors.pl_active_bg);
        colors.viz          = gc("viz",           colors.viz);
        colors.viz_left     = gc2("viz_left",   "viz_bass",   colors.viz_left);
        colors.viz_center   = gc2("viz_center", "viz_mid",    colors.viz_center);
        colors.viz_right    = gc2("viz_right",  "viz_treble", colors.viz_right);
        colors.viz_peak     = gc("viz_peak",      colors.viz_peak);
        colors.lyr_dim      = gc("lyr_dim",       colors.lyr_dim);
        colors.lyr_hi       = gc("lyr_hi",        colors.lyr_hi);
        colors.lyr_word     = gc("lyr_word",      colors.lyr_word);
        colors.noise        = gc("noise",         colors.noise);
        colors.queue_item   = gc("queue_item",    colors.queue_item);
        colors.queue_active = gc("queue_active",  colors.queue_active);
    }
};
