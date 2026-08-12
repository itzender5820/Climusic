/*  CLI.MUSIC.COM — main.cpp  v3.0
 *
 *  ── What's new ──────────────────────────────────────────────────────────────
 *  • Declarative UI layout engine (Termux-style DSL from config.txt)
 *  • Play queue: A=add, D=remove, Q=jump to queue panel
 *  • config.txt parser: 100% control over layout, hotkeys, font, colors
 *  • Settings redesign: 6 tabbed panels (Colors / Viz / Layout / Keybinds /
 *    Font / Themes), cleaner formatting with live previews
 *  • Font map: per-character unicode substitution for all UI labels
 *  • Only 2 visualiser styles: BARS (0) and SCOPE (1)
 *  • Auto desktop/mobile mode based on terminal width
 */

#include <ncurses.h>
#include <locale.h>
#include <wchar.h>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <array>
#include <thread>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <functional>
#include <ranges>

#include "player.h"
#include "lyrics.h"
#include "settings.h"
#include "config_parser.h"
#include "ui_layout.h"
#include "vocal_viz.h"
#include "stream.h"

namespace fs = std::filesystem;
using clk = std::chrono::steady_clock;

// ═══════════════════════════════════════════════════════════════════════════
// UTF-8 display width helpers
// ═══════════════════════════════════════════════════════════════════════════
static int utf8_display_width(const std::string& s) {
    int w = 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.c_str());
    const unsigned char* end = p + s.size();
    while (p < end) {
        uint32_t cp = 0; int bytes = 1;
        if      (*p < 0x80)                    { cp = *p; bytes = 1; }
        else if ((*p & 0xE0) == 0xC0 && p+1<end){ cp=(*p&0x1F)<<6|(p[1]&0x3F); bytes=2; }
        else if ((*p & 0xF0) == 0xE0 && p+2<end){ cp=(*p&0x0F)<<12|(p[1]&0x3F)<<6|(p[2]&0x3F); bytes=3; }
        else if ((*p & 0xF8) == 0xF0 && p+3<end){ cp=(*p&0x07)<<18|(p[1]&0x3F)<<12|(p[2]&0x3F)<<6|(p[3]&0x3F); bytes=4; }
        int cw = wcwidth((wchar_t)cp);
        w += (cw >= 0) ? cw : 1;
        p += bytes;
    }
    return w;
}

static std::string utf8_truncate(const std::string& s, int max_cols) {
    if (max_cols <= 0) return "";
    int w = 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.c_str());
    const unsigned char* end = p + s.size();
    const unsigned char* last_ok = p;
    while (p < end) {
        uint32_t cp = 0; int bytes = 1;
        if      (*p < 0x80)                    { cp=*p; bytes=1; }
        else if ((*p & 0xE0)==0xC0 && p+1<end){ cp=(*p&0x1F)<<6|(p[1]&0x3F); bytes=2; }
        else if ((*p & 0xF0)==0xE0 && p+2<end){ cp=(*p&0x0F)<<12|(p[1]&0x3F)<<6|(p[2]&0x3F); bytes=3; }
        else if ((*p & 0xF8)==0xF0 && p+3<end){ cp=(*p&0x07)<<18|(p[1]&0x3F)<<12|(p[2]&0x3F)<<6|(p[3]&0x3F); bytes=4; }
        int cw = wcwidth((wchar_t)cp); if (cw < 0) cw = 1;
        if (w + cw > max_cols) break;
        w += cw; p += bytes; last_ok = p;
    }
    return s.substr(0, (size_t)(last_ok - reinterpret_cast<const unsigned char*>(s.c_str())));
}

// ─── UTF-8 codepoint writer ───────────────────────────────────────────────
static void waddcp(WINDOW* w, uint32_t cp) {
    char b[5] = {};
    if      (cp < 0x80)    { b[0] = (char)cp; }
    else if (cp < 0x800)   { b[0]=(char)(0xC0|(cp>>6));   b[1]=(char)(0x80|(cp&0x3F)); }
    else if (cp < 0x10000) { b[0]=(char)(0xE0|(cp>>12));  b[1]=(char)(0x80|((cp>>6)&0x3F));  b[2]=(char)(0x80|(cp&0x3F)); }
    else                   { b[0]=(char)(0xF0|(cp>>18));  b[1]=(char)(0x80|((cp>>12)&0x3F)); b[2]=(char)(0x80|((cp>>6)&0x3F)); b[3]=(char)(0x80|(cp&0x3F)); }
    waddstr(w, b);
}

static void waddutf8(WINDOW* w, const std::string& s) {
    waddstr(w, s.c_str());
}

static std::string fmt_time(double s) {
    if (s < 0) s = 0;
    char b[16]; snprintf(b, 16, "%02d:%02d", (int)s/60, (int)s%60);
    return b;
}

// ═══════════════════════════════════════════════════════════════════════════
// Color pairs
// ═══════════════════════════════════════════════════════════════════════════
// Number of interpolated steps across the bar visualizer's left->center->right
// gradient. Only usable on terminals with can_change_color() (custom RGB) —
// see apply_colors()/apply_basic_colors() and draw_viz().
static constexpr int VIZ_GRAD_STEPS = 24;

enum CP {
    CP_TITLE=1, CP_BORDER, CP_HDR_DECO,
    CP_META_KEY, CP_META_VAL,
    CP_PROGRESS, CP_PROGRESS_BG,
    CP_PLAYLIST, CP_PL_ACTIVE,
    CP_LYR_DIM, CP_LYR_HI, CP_LYR_WORD,
    CP_NOISE, CP_VIZ,
    CP_VIZ_LEFT, CP_VIZ_CENTER, CP_VIZ_RIGHT, CP_VIZ_PEAK,
    CP_STATUS, CP_STATUS_KEY, CP_STATUS_VAL,
    CP_SET_HDR, CP_SET_ITEM, CP_SET_SEL, CP_SET_TAB,
    CP_QUEUE_ITEM, CP_QUEUE_ACT,
    CP_SEARCH_BAR,
    CP_VIZ_GRAD_BASE,                        // first of VIZ_GRAD_STEPS gradient pairs
    CP_COUNT = CP_VIZ_GRAD_BASE + VIZ_GRAD_STEPS
};
enum CSLOT {
    CS_TITLE=16,CS_BORDER,CS_DECO,
    CS_META_KEY,CS_META_VAL,
    CS_PROGRESS,CS_PROGRESS_BG,
    CS_PLAYLIST,CS_PL_FG,CS_PL_BG,
    CS_LYR_DIM,CS_LYR_HI,CS_LYR_WORD,
    CS_NOISE,CS_VIZ,
    CS_VIZ_LEFT,CS_VIZ_CENTER,CS_VIZ_RIGHT,CS_VIZ_PEAK,
    CS_STATUS,
    CS_QUEUE_ITEM,CS_QUEUE_ACT,
    CS_SET_HDR_FG=230,CS_SET_HDR_BG=231,
    CS_SET_ITEM_FG=232,CS_SET_SEL_FG=233,CS_SET_SEL_BG=234,
    CS_SET_TAB_FG=235,CS_SET_TAB_BG=236,
    CS_VIZ_GRAD_BASE=40                      // 40..(40+VIZ_GRAD_STEPS-1), clear of all ranges above
};

static Settings g_cfg;
// Path to the real, user-editable config.txt — set once in main() after
// ConfigParser::find_config(). Used by the settings panel's "auto-save to
// config.txt" toggle (see ConfigParser::write_back()).
static std::string g_cfg_path;

static void set_ncurses_color(short slot, const Color& col) {
    short r, g, b;
    ansi256_to_rgb1000(col.ansi, r, g, b);
    apply_brightness(r, g, b, col.brightness);
    init_color(slot, r, g, b);
}

static void apply_colors() {
    const auto& C = g_cfg.colors;
    set_ncurses_color(CS_BORDER,     C.border);
    set_ncurses_color(CS_TITLE,      C.title);
    set_ncurses_color(CS_DECO,       C.border);
    set_ncurses_color(CS_META_KEY,   C.meta_key);
    set_ncurses_color(CS_META_VAL,   C.meta_val);
    set_ncurses_color(CS_PROGRESS,   C.progress);
    set_ncurses_color(CS_PROGRESS_BG,C.progress_bg);
    set_ncurses_color(CS_PLAYLIST,   C.playlist);
    set_ncurses_color(CS_PL_FG,      C.pl_active_fg);
    set_ncurses_color(CS_PL_BG,      C.pl_active_bg);
    set_ncurses_color(CS_LYR_DIM,    C.lyr_dim);
    set_ncurses_color(CS_LYR_HI,     C.lyr_hi);
    set_ncurses_color(CS_LYR_WORD,   C.lyr_word);
    set_ncurses_color(CS_NOISE,      C.noise);
    set_ncurses_color(CS_VIZ,        C.viz);
    set_ncurses_color(CS_VIZ_LEFT,   C.viz_left);
    set_ncurses_color(CS_VIZ_CENTER,    C.viz_center);
    set_ncurses_color(CS_VIZ_RIGHT, C.viz_right);
    set_ncurses_color(CS_VIZ_PEAK,   C.viz_peak);
    set_ncurses_color(CS_STATUS,     C.status);
    set_ncurses_color(CS_QUEUE_ITEM, C.queue_item);
    set_ncurses_color(CS_QUEUE_ACT,  C.queue_active);
    init_color(CS_SET_HDR_FG,  1000,1000,1000);
    init_color(CS_SET_HDR_BG,   80,  80, 200);
    init_color(CS_SET_ITEM_FG, 800, 800, 800);
    init_color(CS_SET_SEL_FG,    0,   0,   0);
    init_color(CS_SET_SEL_BG,  200, 700,1000);
    init_color(CS_SET_TAB_FG,  900, 900, 900);
    init_color(CS_SET_TAB_BG,   50,  50, 120);

    init_pair(CP_BORDER,      CS_BORDER,     -1);
    init_pair(CP_HDR_DECO,    CS_DECO,       -1);
    init_pair(CP_TITLE,       CS_TITLE,      -1);
    init_pair(CP_META_KEY,    CS_META_KEY,   -1);
    init_pair(CP_META_VAL,    CS_META_VAL,   -1);
    init_pair(CP_PROGRESS,    CS_PROGRESS,   -1);
    init_pair(CP_PROGRESS_BG, CS_PROGRESS_BG,-1);
    init_pair(CP_PLAYLIST,    CS_PLAYLIST,   -1);
    init_pair(CP_PL_ACTIVE,   CS_PL_FG, CS_PL_BG);
    init_pair(CP_LYR_DIM,     CS_LYR_DIM,   -1);
    init_pair(CP_LYR_HI,      CS_LYR_HI,    -1);
    init_pair(CP_LYR_WORD,    CS_LYR_WORD,  -1);
    init_pair(CP_NOISE,       CS_NOISE,      -1);
    init_pair(CP_VIZ,         CS_VIZ,       -1);
    init_pair(CP_VIZ_LEFT,    CS_VIZ_LEFT,  -1);
    init_pair(CP_VIZ_CENTER,     CS_VIZ_CENTER,   -1);
    init_pair(CP_VIZ_RIGHT,  CS_VIZ_RIGHT,-1);
    init_pair(CP_VIZ_PEAK,    CS_VIZ_PEAK,  -1);
    init_pair(CP_STATUS,      CS_STATUS,    -1);
    init_pair(CP_STATUS_KEY,  CS_STATUS,    -1);
    init_pair(CP_STATUS_VAL,  CS_STATUS,    -1);
    init_pair(CP_SET_HDR,     CS_SET_HDR_FG, CS_SET_HDR_BG);
    init_pair(CP_SET_ITEM,    CS_SET_ITEM_FG,-1);
    init_pair(CP_SET_SEL,     CS_SET_SEL_FG, CS_SET_SEL_BG);
    init_pair(CP_SET_TAB,     CS_SET_TAB_FG, CS_SET_TAB_BG);
    init_pair(CP_QUEUE_ITEM,  CS_QUEUE_ITEM, -1);
    init_pair(CP_QUEUE_ACT,   CS_QUEUE_ACT,  -1);
    init_pair(CP_SEARCH_BAR,  CS_LYR_HI,    -1);

    // Smooth left -> center -> right gradient for the bar visualizer,
    // instead of three hard-edged flat color blocks. Interpolated in
    // RGB space (not ANSI-256 index space, which isn't linear), reusing
    // the same ansi256->rgb1000 + brightness resolution set_ncurses_color
    // already does for the three endpoint colors.
    {
        short lr,lg,lb, cr,cg,cb, rr,rg,rb;
        ansi256_to_rgb1000(C.viz_left.ansi,   lr,lg,lb); apply_brightness(lr,lg,lb, C.viz_left.brightness);
        ansi256_to_rgb1000(C.viz_center.ansi, cr,cg,cb); apply_brightness(cr,cg,cb, C.viz_center.brightness);
        ansi256_to_rgb1000(C.viz_right.ansi,  rr,rg,rb); apply_brightness(rr,rg,rb, C.viz_right.brightness);

        for (int i = 0; i < VIZ_GRAD_STEPS; ++i) {
            double t = (VIZ_GRAD_STEPS <= 1) ? 0.0 : (double)i / (double)(VIZ_GRAD_STEPS - 1);
            short r, g, b;
            if (t <= 0.5) {
                double u = t / 0.5;
                r = (short)(lr + (cr - lr) * u);
                g = (short)(lg + (cg - lg) * u);
                b = (short)(lb + (cb - lb) * u);
            } else {
                double u = (t - 0.5) / 0.5;
                r = (short)(cr + (rr - cr) * u);
                g = (short)(cg + (rg - cg) * u);
                b = (short)(cb + (rb - cb) * u);
            }
            init_color((short)(CS_VIZ_GRAD_BASE + i), r, g, b);
            init_pair((short)(CP_VIZ_GRAD_BASE + i), (short)(CS_VIZ_GRAD_BASE + i), -1);
        }
    }
}

static void apply_basic_colors() {
    init_pair(CP_BORDER,     COLOR_CYAN,   -1);
    init_pair(CP_HDR_DECO,   COLOR_CYAN,   -1);
    init_pair(CP_TITLE,      COLOR_WHITE,  -1);
    init_pair(CP_META_KEY,   COLOR_CYAN,   -1);
    init_pair(CP_META_VAL,   COLOR_WHITE,  -1);
    init_pair(CP_PROGRESS,   COLOR_GREEN,  -1);
    init_pair(CP_PROGRESS_BG,COLOR_WHITE,  -1);
    init_pair(CP_PLAYLIST,   COLOR_WHITE,  -1);
    init_pair(CP_PL_ACTIVE,  COLOR_BLACK, COLOR_CYAN);
    init_pair(CP_LYR_DIM,    COLOR_WHITE,  -1);
    init_pair(CP_LYR_HI,     COLOR_YELLOW, -1);
    init_pair(CP_LYR_WORD,   COLOR_YELLOW, -1);
    init_pair(CP_NOISE,      COLOR_GREEN,  -1);
    init_pair(CP_VIZ,        COLOR_CYAN,   -1);
    init_pair(CP_VIZ_LEFT,   COLOR_RED,    -1);
    init_pair(CP_VIZ_CENTER,    COLOR_GREEN,  -1);
    init_pair(CP_VIZ_RIGHT, COLOR_CYAN,   -1);
    init_pair(CP_VIZ_PEAK,   COLOR_YELLOW, -1);
    init_pair(CP_STATUS,     COLOR_WHITE,  -1);
    init_pair(CP_STATUS_KEY, COLOR_WHITE,  -1);
    init_pair(CP_STATUS_VAL, COLOR_CYAN,   -1);
    init_pair(CP_SET_HDR,    COLOR_WHITE,  COLOR_BLUE);
    init_pair(CP_SET_ITEM,   COLOR_WHITE,  -1);
    init_pair(CP_SET_SEL,    COLOR_BLACK,  COLOR_CYAN);
    init_pair(CP_SET_TAB,    COLOR_WHITE,  COLOR_BLUE);
    init_pair(CP_QUEUE_ITEM, COLOR_WHITE,  -1);
    init_pair(CP_QUEUE_ACT,  COLOR_CYAN,   -1);
    init_pair(CP_SEARCH_BAR, COLOR_YELLOW, -1);
}

// ═══════════════════════════════════════════════════════════════════════════
// Noise generator (for lyrics placeholder)
// ═══════════════════════════════════════════════════════════════════════════
struct NoiseGen {
    double phase = 0.0;
    static constexpr uint32_t BRAILLE[9] = {
        0x2800,0x28C0,0x28C4,0x28E4,0x28E6,0x28F4,0x28F6,0x28FE,0x28FF
    };
    static constexpr uint32_t SYMS[12] = {
        0x00B7,0x2219,0x2022,0x25AA,0x25CF,0x25CB,
        0x2726,0x2727,0x2605,0x2606,0x00D7,0x007C
    };
    struct Cell { float base, speed, amp; };
    std::vector<Cell> cells; int W=0, H=0;
    void resize(int w, int h) {
        if (w==W && h==H) return; W=w; H=h; cells.resize(w*h);
        for (int i=0; i<w*h; ++i) {
            float s=(float)(i*2654435761u)/(float)UINT32_MAX;
            cells[i]={s, 0.3f+0.7f*std::fmod(s*1.618f,1.f), 0.5f+0.5f*std::fmod(s*2.718f,1.f)};
        }
    }
    void advance(double dt) { phase += dt; }
    uint32_t get(int col, int row) const {
        if (col>=W || row>=H) return ' ';
        const Cell& c = cells[row*W+col]; float v=0;
        v += std::sin((float)phase*c.speed*1.0f+c.base*6.28f)*0.40f;
        v += std::sin((float)phase*c.speed*2.3f+c.base*9.42f)*0.30f;
        v += std::sin((float)phase*c.speed*0.7f+(float)col*0.3f)*0.20f;
        v += std::sin((float)phase*c.speed*3.1f+(float)row*0.5f)*0.10f;
        v *= c.amp; float n = std::clamp((v+1.f)*0.5f, 0.f, 1.f);
        float sym_t = 0.06f*std::fabs(std::sin((float)phase*0.13f+c.base*17.f));
        if (n < sym_t) { int si=((int)(c.base*97.f+(float)phase*0.31f))%12; return SYMS[si<0?si+12:si]; }
        return BRAILLE[std::clamp((int)(n*8), 0, 8)];
    }
};
constexpr uint32_t NoiseGen::BRAILLE[9];
constexpr uint32_t NoiseGen::SYMS[12];

// ═══════════════════════════════════════════════════════════════════════════
// Border drawing helpers
// ═══════════════════════════════════════════════════════════════════════════
// Draw a box using the user-configured border characters.
static void draw_box(WINDOW* w, const BorderChars& bc) {
    int wh, ww; getmaxyx(w, wh, ww);
    wattron(w, COLOR_PAIR(CP_BORDER));
    // Corners
    mvwaddstr(w, 0,     0,     bc.top_left.c_str());
    mvwaddstr(w, 0,     ww-1,  bc.top_right.c_str());
    mvwaddstr(w, wh-1,  0,     bc.bot_left.c_str());
    mvwaddstr(w, wh-1,  ww-1,  bc.bot_right.c_str());
    // Horizontal edges
    for (int c = 1; c < ww-1; ++c) {
        mvwaddstr(w, 0,    c, bc.horiz.c_str());
        mvwaddstr(w, wh-1, c, bc.horiz.c_str());
    }
    // Vertical edges
    for (int r = 1; r < wh-1; ++r) {
        mvwaddstr(w, r, 0,    bc.vert.c_str());
        mvwaddstr(w, r, ww-1, bc.vert.c_str());
    }
    wattroff(w, COLOR_PAIR(CP_BORDER));
}

// Print a title label in the top border at col 2.
static void border_label(WINDOW* w, const std::string& label) {
    int wh, ww; getmaxyx(w, wh, ww); (void)wh;
    wattron(w, COLOR_PAIR(CP_BORDER));
    mvwprintw(w, 0, 2, " %s ", utf8_truncate(label, ww-6).c_str());
    wattroff(w, COLOR_PAIR(CP_BORDER));
}

// ═══════════════════════════════════════════════════════════════════════════
// Header bar
// ═══════════════════════════════════════════════════════════════════════════
static void draw_header(WINDOW* w, int cols, const std::string& icon,
                         bool repeat, bool shuffle, int vol,
                         const std::string& title_override)
{
    werase(w);
    wattron(w, COLOR_PAIR(CP_HDR_DECO));
    wmove(w, 0, 0);
    for (int i = 0; i < cols; ++i) waddcp(w, 0x2592);
    wattroff(w, COLOR_PAIR(CP_HDR_DECO));

    wattron(w, COLOR_PAIR(CP_TITLE) | A_BOLD);
    std::string left  = icon + " ";
    std::string right = " [R:" + std::string(repeat ? "ON" : "off")
                      + " M:" + std::string(shuffle ? "ON" : "off")
                      + " V:" + std::to_string(vol) + "%]";
    const char* center_text = title_override.empty()
        ? "CLI.MUSIC.COM" : title_override.c_str();
    int tc = (cols - (int)strlen(center_text)) / 2;
    mvwprintw(w, 1, 1, "%s", left.c_str());
    if (tc > 0) mvwprintw(w, 1, tc, "%s", center_text);
    mvwprintw(w, 1, cols - (int)right.size() - 1, "%s", right.c_str());
    wattroff(w, COLOR_PAIR(CP_TITLE) | A_BOLD);

    wattron(w, COLOR_PAIR(CP_HDR_DECO));
    wmove(w, 2, 0);
    for (int i = 0; i < cols; ++i) waddcp(w, 0x2592);
    wattroff(w, COLOR_PAIR(CP_HDR_DECO));
}

// ═══════════════════════════════════════════════════════════════════════════
// META panel
// ═══════════════════════════════════════════════════════════════════════════
static void draw_meta(WINDOW* w, const TrackMeta& m) {
    int wh, ww; getmaxyx(w, wh, ww);
    werase(w);
    draw_box(w, g_cfg.border_chars);
    border_label(w, "META DATA");

    auto kv = [&](int row, const char* key, const std::string& val) {
        if (row >= wh - 1) return;
        wattron(w, COLOR_PAIR(CP_META_KEY) | A_BOLD);
        mvwprintw(w, row, 2, "%-8s", key);
        wattroff(w, COLOR_PAIR(CP_META_KEY) | A_BOLD);
        wattron(w, COLOR_PAIR(CP_META_VAL));
        std::string sv = utf8_truncate(val, ww - 13);
        mvwprintw(w, row, 10, ": %s", sv.c_str());
        wattroff(w, COLOR_PAIR(CP_META_VAL));
    };
    kv(1, "NAME",    m.title.empty()   ? "Unknown" : m.title);
    kv(2, "ARTIST",  m.artist.empty()  ? "Unknown" : m.artist);
    kv(3, "ALBUM",   m.album.empty()   ? "Unknown" : m.album);
    kv(4, "FORMAT",  m.format.empty()  ? "?"       : m.format);
    kv(5, "QUALITY", std::to_string(m.sample_rate / 1000) + " KHz");
    kv(6, "YEAR",    m.year > 0 ? std::to_string(m.year) : "\xe2\x80\x94");
}

// ═══════════════════════════════════════════════════════════════════════════
// COVER panel  (ASCII art placeholder)
// ═══════════════════════════════════════════════════════════════════════════
static void draw_cover(WINDOW* w, const TrackMeta& m) {
    int wh, ww; getmaxyx(w, wh, ww);
    werase(w);
    draw_box(w, g_cfg.border_chars);
    border_label(w, "COVER");

    int inner_w = ww - 2, inner_h = wh - 2;
    if (inner_w < 2 || inner_h < 2) return;

    // Draw a simple music-note pattern as placeholder cover art
    wattron(w, COLOR_PAIR(CP_VIZ_CENTER));
    static const char* notes[] = {
        " \xe2\x99\xaa  \xe2\x99\xab ",
        "  \xe2\x99\xac  \xe2\x99\xad",
        "\xe2\x99\xaa   \xe2\x99\xae ",
        " \xe2\x99\xad \xe2\x99\xaa  ",
    };
    for (int r = 0; r < inner_h; ++r) {
        const char* pat = notes[r % 4];
        mvwprintw(w, r + 1, 1, "%-*.*s", inner_w, inner_w, pat);
    }
    wattroff(w, COLOR_PAIR(CP_VIZ_CENTER));

    // Title overlay in the center
    if (!m.title.empty()) {
        std::string disp = utf8_truncate(m.title, inner_w - 2);
        int cx = 1 + std::max(0, (inner_w - (int)utf8_display_width(disp)) / 2);
        int cy = inner_h / 2;
        wattron(w, COLOR_PAIR(CP_TITLE) | A_BOLD | A_REVERSE);
        mvwprintw(w, cy + 1, cx, "%s", disp.c_str());
        wattroff(w, COLOR_PAIR(CP_TITLE) | A_BOLD | A_REVERSE);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// LYRICS panel
// ═══════════════════════════════════════════════════════════════════════════
static void draw_lyrics(WINDOW* w, Lyrics& lyr, double pos, NoiseGen& noise) {
    int wh, ww; getmaxyx(w, wh, ww);
    werase(w);
    draw_box(w, g_cfg.border_chars);
    border_label(w, "LYRICS");

    int rw = ww - 2, rh = wh - 2;
    if (rw < 2 || rh < 1) return;

    if (lyr.has_lyrics()) {
        auto lines   = lyr.visible(pos, rh);
        int  active  = lyr.active_in_visible(pos, rh);
        auto words   = lyr.active_words(pos);
        int  wrd_idx = lyr.active_word_idx(pos);

        for (int i = 0; i < (int)lines.size() && i < rh; ++i) {
            const bool hi = (i == active);
            if (hi && !words.empty()) {
                std::string full;
                for (int wi = 0; wi < (int)words.size(); ++wi) {
                    if (wi > 0) full += ' ';
                    full += words[wi].text;
                }
                int dw = utf8_display_width(full);
                int xoff = std::max(0, (rw - dw) / 2);
                int cx = 1 + xoff;
                mvwprintw(w, 1 + i, 1, "%-*s", rw, " ");
                for (int wi = 0; wi < (int)words.size() && cx < ww - 1; ++wi) {
                    if (wi > 0 && cx < ww-1) { mvwaddch(w, 1+i, cx, ' '); ++cx; }
                    const std::string& wt = words[wi].text;
                    int avail = ww - 1 - cx; if (avail <= 0) break;
                    std::string disp = utf8_truncate(wt, avail);
                    int pair, attr = A_BOLD;
                    if (wi == wrd_idx)  { pair = CP_LYR_WORD; attr = A_BOLD | A_UNDERLINE; }
                    else if (wi < wrd_idx) pair = CP_LYR_HI;
                    else                { pair = CP_LYR_DIM; attr = 0; }
                    wattron(w, COLOR_PAIR(pair) | attr);
                    mvwprintw(w, 1+i, cx, "%s", disp.c_str());
                    wattroff(w, COLOR_PAIR(pair) | attr);
                    cx += utf8_display_width(disp);
                }
            } else {
                std::string disp = utf8_truncate(lines[i], rw);
                int xoff = disp.empty() ? 0 : std::max(0, (rw - (int)utf8_display_width(disp)) / 2);
                mvwprintw(w, 1+i, 1, "%-*s", rw, " ");
                if (!disp.empty()) {
                    if (hi) wattron(w, COLOR_PAIR(CP_LYR_HI) | A_BOLD);
                    else    wattron(w, COLOR_PAIR(CP_LYR_DIM));
                    mvwprintw(w, 1+i, 1 + xoff, "%s", disp.c_str());
                    if (hi) wattroff(w, COLOR_PAIR(CP_LYR_HI) | A_BOLD);
                    else    wattroff(w, COLOR_PAIR(CP_LYR_DIM));
                }
            }
        }
    } else {
        noise.resize(rw, rh);
        wattron(w, COLOR_PAIR(CP_NOISE));
        for (int r = 0; r < rh; ++r) {
            wmove(w, r + 1, 1);
            for (int c = 0; c < rw; ++c) waddcp(w, noise.get(c, r));
        }
        wattroff(w, COLOR_PAIR(CP_NOISE));
        auto lst = lyr.state();
        if (lst == Lyrics::State::FETCHING || lst == Lyrics::State::NOT_FOUND) {
            std::string msg = lyr.status();
            if (!msg.empty()) {
                wattron(w, COLOR_PAIR(CP_STATUS));
                mvwprintw(w, rh/2+1, 1 + std::max(0, (rw-(int)msg.size())/2), "%s", msg.c_str());
                wattroff(w, COLOR_PAIR(CP_STATUS));
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// VOCAL-VIZ panel
// ═══════════════════════════════════════════════════════════════════════════
static void draw_vocal_viz(WINDOW* w, VocalVisualizer& vviz) {
    int wh, ww; getmaxyx(w, wh, ww);
    werase(w);
    draw_box(w, g_cfg.border_chars);
    border_label(w, "VOCAL VISUALIZER");

    int vw = ww - 2, vh = wh - 2;
    if (vw <= 0 || vh <= 0) return;

    auto energy = vviz.compute();
    static constexpr int NS = VocalVisualizer::N_SLOTS;
    static constexpr wchar_t BRL[5] = {L' ', L'\u28C0', L'\u28E4', L'\u28F6', L'\u28FF'};

    for (int col = 0; col < vw; ++col) {
        float fslot = (float)col / (float)(vw - 1) * (NS - 1);
        int s0 = (int)fslot;
        int s1 = std::min(s0 + 1, NS - 1);
        float t = fslot - s0;
        float val = energy[s0] * (1.0f - t) + energy[s1] * t;

        int total_sc = (int)(val * (float)(vh * 4));
        total_sc = std::clamp(total_sc, 0, vh * 4);

        int near_s = s0;
        int pair;
        if      (near_s >= 9 && near_s <= 11) pair = CP_LYR_HI;
        else if (near_s >= 7 && near_s <= 13) pair = CP_VIZ_CENTER;
        else if (near_s >= 4 && near_s <= 16) pair = CP_VIZ;
        else                                   pair = CP_VIZ_LEFT;

        for (int row = vh - 1; row >= 0; --row) {
            int screen_row = 1 + row;
            if (screen_row >= wh - 1) continue;
            int avail = std::min(total_sc, 4);
            total_sc -= avail;
            if (avail > 0) {
                wattron(w, COLOR_PAIR(pair) | A_BOLD);
                wmove(w, screen_row, 1 + col);
                waddcp(w, (uint32_t)BRL[avail]);
                wattroff(w, COLOR_PAIR(pair) | A_BOLD);
            } else {
                mvwaddch(w, screen_row, 1 + col, ' ');
            }
        }
    }

    // Delay labels
    struct Label { int slot; const char* txt; };
    static const Label LABELS[] = {{0,"L80"},{5,"L5"},{10,"C"},{15,"R5"},{20,"R80"}};
    if (wh > 2) {
        wattron(w, COLOR_PAIR(CP_STATUS));
        for (auto& lb : LABELS) {
            int lc = (int)((float)lb.slot / (float)(NS-1) * (float)(vw-1));
            lc = std::clamp(lc, 0, vw - (int)strlen(lb.txt));
            mvwprintw(w, wh - 1, 1 + lc, "%s", lb.txt);
        }
        wattroff(w, COLOR_PAIR(CP_STATUS));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// VIZ panel
// ═══════════════════════════════════════════════════════════════════════════
static void draw_viz(WINDOW* w, Player& player) {
    int wh, ww; getmaxyx(w, wh, ww);
    werase(w);
    draw_box(w, g_cfg.border_chars);

    int si = (int)g_cfg.viz_style;
    si = std::clamp(si, 0, VIZ_STYLE_COUNT - 1);
    border_label(w, std::string("VISUALIZER \xe2\x80\x94 ") + VIZ_STYLE_NAMES[si]);

    int vw = ww - 2, vh = wh - 2;
    if (vw <= 0 || vh <= 0) return;

    player.visualizer().init(vw, vh);
    player.visualizer().set_peak_hold(g_cfg.peak_hold);
    player.visualizer().set_density(g_cfg.viz_density);
    auto frame = player.visualizer().render(g_cfg.viz_style, g_cfg.viz_bands);

    const bool is_scope = (g_cfg.viz_style == VizStyle::SCOPE);
    const bool gradient = can_change_color();   // cheap ncurses query, not a subprocess

    for (int r = 0; r < (int)frame.rows.size() && r < vh; ++r) {
        wmove(w, r + 1, 1);
        for (int c = 0; c < (int)frame.rows[r].size() && c < vw; ++c) {
            uint32_t ch = frame.rows[r][c];
            int pair;
            if (is_scope) {
                pair = CP_VIZ_CENTER;
            } else if (gradient) {
                // Continuous left->center->right position instead of three
                // hard-edged flat color blocks.
                int step = (vw <= 1) ? 0 : (c * (VIZ_GRAD_STEPS - 1)) / (vw - 1);
                pair = CP_VIZ_GRAD_BASE + std::clamp(step, 0, VIZ_GRAD_STEPS - 1);
            } else {
                int zone = (c * 3) / vw;
                pair = (zone == 0) ? CP_VIZ_LEFT : (zone == 1) ? CP_VIZ_CENTER : CP_VIZ_RIGHT;
            }
            if (ch == 0x2022u && g_cfg.peak_hold) pair = CP_VIZ_PEAK;
            wattron(w, COLOR_PAIR(pair) | A_BOLD);
            waddcp(w, ch);
            wattroff(w, COLOR_PAIR(pair) | A_BOLD);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// PROGRESS-BAR panel
// ═══════════════════════════════════════════════════════════════════════════
static void draw_progress(WINDOW* w, double pos, double dur) {
    int wh, ww; getmaxyx(w, wh, ww); (void)wh;
    werase(w);
    draw_box(w, g_cfg.border_chars);
    border_label(w, "PROGRESS BAR");

    const int time_w = 16;
    int bar_w = std::max(4, ww - 4 - time_w);
    double pct = (dur > 0.5) ? std::clamp(pos / dur, 0.0, 1.0) : 0.0;
    int filled = (int)(pct * bar_w);

    wmove(w, 1, 1); waddch(w, '[');
    for (int i = 0; i < bar_w; ++i) {
        if (i < filled) {
            wattron(w, COLOR_PAIR(CP_PROGRESS) | A_BOLD); waddch(w, '#');
            wattroff(w, COLOR_PAIR(CP_PROGRESS) | A_BOLD);
        } else {
            wattron(w, COLOR_PAIR(CP_PROGRESS_BG)); waddch(w, '-');
            wattroff(w, COLOR_PAIR(CP_PROGRESS_BG));
        }
    }
    waddch(w, ']');
    char ts[24]; snprintf(ts, 24, "[%s/%s]", fmt_time(pos).c_str(), fmt_time(dur).c_str());
    mvwprintw(w, 1, bar_w + 2, "%s", ts);
}

// ═══════════════════════════════════════════════════════════════════════════
// LIST panel  (playlist)
// ═══════════════════════════════════════════════════════════════════════════
struct SearchState { std::string query; std::vector<int> matches; int sel = 0; };

static void update_search(SearchState& ss, const Playlist& pl) {
    ss.matches.clear(); ss.sel = 0;
    if (ss.query.empty()) return;
<<<<<<< HEAD
    // BUGFIX (audit D2): ::tolower fed straight to std::ranges::transform
    // gets called with a plain (signed on most platforms) char — a
    // negative value (any byte >= 0x80, e.g. UTF-8 continuation bytes in
    // a non-ASCII filename) is undefined behavior per the C standard.
    auto safe_lower = [](char c) { return (char)std::tolower((unsigned char)c); };
    std::string ql = ss.query;
    std::ranges::transform(ql, ql.begin(), safe_lower);
    const auto& E = pl.entries();
    for (int i = 0; i < (int)E.size(); ++i) {
        std::string dl = E[i].display_name;
        std::ranges::transform(dl, dl.begin(), safe_lower);
        if (dl.find(ql) != std::string::npos) ss.matches.push_back(i);
    }
}

static void draw_pl_row(WINDOW* w, int row, int col, int inner_w,
                         const std::string& name, bool active)
{
    if (active) wattron(w, COLOR_PAIR(CP_PL_ACTIVE) | A_BOLD);
    else        wattron(w, COLOR_PAIR(CP_PLAYLIST));

    const int prefix_w = 2, suffix_w = 1;
    int name_field = inner_w - prefix_w - suffix_w;
    if (name_field < 1) name_field = 1;
    std::string trunc = utf8_truncate(name, name_field);
    int name_dw = utf8_display_width(trunc);
    int pad = name_field - name_dw;

    mvwprintw(w, row, col, "> ");
    waddstr(w, trunc.c_str());
    for (int p = 0; p < pad; ++p) waddch(w, ' ');
    waddch(w, '<');

    if (active) wattroff(w, COLOR_PAIR(CP_PL_ACTIVE) | A_BOLD);
    else        wattroff(w, COLOR_PAIR(CP_PLAYLIST));
}

static int draw_list(WINDOW* w, const Playlist& pl,
                      bool search_active, SearchState& ss, int key)
{
    int ww, wh; getmaxyx(w, wh, ww);
    int result = -1;
    if (search_active) {
        if (key==27 || key=='/') result = -2;
        else if (key=='\n' || key==KEY_ENTER)
            result = ss.matches.empty() ? -2 : ss.matches[ss.sel];
        else if (key==KEY_UP)   { if(ss.sel>0) --ss.sel; }
        else if (key==KEY_DOWN) { if(ss.sel<(int)ss.matches.size()-1) ++ss.sel; }
        else if (key==KEY_BACKSPACE||key==127||key==8)
            { if(!ss.query.empty()){ss.query.pop_back();update_search(ss,pl);} }
        else if (key>=32 && key<127) { ss.query += (char)key; update_search(ss,pl); }
    }
    werase(w);
    draw_box(w, g_cfg.border_chars);
    std::string lbl = pl.has_filter()
        ? "FILTER: " + pl.folder_filter() + " " + std::to_string(pl.count()) + "/" + std::to_string(pl.total_count())
        : Playlist::MUSIC_DIR;
    border_label(w, lbl);

    wattron(w, COLOR_PAIR(CP_SEARCH_BAR));
    wmove(w, 1, 1); waddcp(w, 0x25CB); waddch(w, ' ');
    if (search_active) {
        std::string sq = (ss.query.empty() ? "" : ss.query) + "_";
        int avail = ww - 4; if (avail < 1) avail = 1;
        wprintw(w, "%-*s", avail, sq.substr(0, avail).c_str());
    } else {
        wprintw(w, "%-*s", ww-4, "search ( press / )");
    }
    wattroff(w, COLOR_PAIR(CP_SEARCH_BAR));
    wattron(w, COLOR_PAIR(CP_BORDER));
    mvwhline(w, 2, 1, ACS_HLINE, ww-2);
    wattroff(w, COLOR_PAIR(CP_BORDER));

    int list_rows = wh - 4;
    int inner_w   = ww - 2;

    if (search_active && !ss.query.empty()) {
        int start = std::max(0, ss.sel - list_rows/2);
        int end   = std::min((int)ss.matches.size(), start + list_rows);
        if (end - start < list_rows) start = std::max(0, end - list_rows);
        if (ss.matches.empty()) {
            wattron(w, COLOR_PAIR(CP_STATUS));
            mvwprintw(w, 3, 2, "No results for: %s", ss.query.c_str());
            wattroff(w, COLOR_PAIR(CP_STATUS));
        }
        const auto& E = pl.entries();
        for (int i = start; i < end; ++i)
            draw_pl_row(w, 3+(i-start), 1, inner_w, E[ss.matches[i]].display_name, (i==ss.sel));
    } else {
        int cur   = pl.current_idx();
        int start = std::max(0, cur - list_rows/2);
        int end   = std::min(pl.count(), start + list_rows);
        if (end - start < list_rows) start = std::max(0, end - list_rows);
        const auto& E = pl.entries();
        for (int i = start; i < end; ++i)
            draw_pl_row(w, 3+(i-start), 1, inner_w, E[i].display_name, (i==cur));
    }
    return result;
}

// Shown in place of the FFT/vocal visualizers while streaming: audio decode
// happens inside mpv/ffplay's own process, so there's no PCM here to feed
// the visualizer with (see stream.h's top comment).
static void draw_stream_viz_placeholder(WINDOW* w, const char* label) {
    int wh, ww; getmaxyx(w, wh, ww);
    werase(w);
    draw_box(w, g_cfg.border_chars);
    border_label(w, label);
    wattron(w, COLOR_PAIR(CP_STATUS));
    std::string msg = "\xe2\x99\xaa  streaming \xe2\x80\x94 no visualizer data";
    int cx = std::max(1, (ww - (int)msg.size()) / 2);
    mvwprintw(w, wh/2, cx, "%s", msg.c_str());
    wattroff(w, COLOR_PAIR(CP_STATUS));
}

// ═══════════════════════════════════════════════════════════════════════════
// STREAM RESULTS panel — reuses the LIST window while browsing yt: results
// ═══════════════════════════════════════════════════════════════════════════
static void draw_stream_results(WINDOW* w, StreamSearch& ss, int sel,
                                 bool is_streaming, const std::string& playing_url) {
    int ww, wh; getmaxyx(w, wh, ww);
    werase(w);
    draw_box(w, g_cfg.border_chars);
    std::string qkey = (g_cfg.keys.queue_add >= 32 && g_cfg.keys.queue_add < 127)
        ? std::string(1, (char)g_cfg.keys.queue_add) : "?";
    border_label(w, "YOUTUBE  (ESC=back  ENTER=play  " + qkey + "=queue)");

    wattron(w, COLOR_PAIR(CP_SEARCH_BAR));
    std::string status = ss.status();
    if (ss.state() == StreamSearch::State::SEARCHING) status = "\xe2\x8f\xb3 " + status;
    mvwprintw(w, 1, 1, "%-*s", ww - 3, status.substr(0, std::max(0, ww-3)).c_str());
    wattroff(w, COLOR_PAIR(CP_SEARCH_BAR));
    wattron(w, COLOR_PAIR(CP_BORDER));
    mvwhline(w, 2, 1, ACS_HLINE, ww-2);
    wattroff(w, COLOR_PAIR(CP_BORDER));

    if (ss.state() != StreamSearch::State::DONE) return;

    auto results = ss.results();
    int list_rows = wh - 4;
    int inner_w   = ww - 2;
    int start = std::max(0, sel - list_rows/2);
    int end   = std::min((int)results.size(), start + list_rows);
    if (end - start < list_rows) start = std::max(0, end - list_rows);

    for (int i = start; i < end; ++i) {
        bool playing = is_streaming && results[i].url == playing_url;
        // Duration goes in front — titles can be long enough to get
        // truncated by draw_pl_row's fixed-width cut, which would hide a
        // trailing "[mm:ss]" entirely. Leading it keeps it always visible.
        std::string prefix = playing ? "\xe2\x96\xb6 " : "";
        if (!results[i].duration.empty()) prefix += "[" + results[i].duration + "] ";
        draw_pl_row(w, 3+(i-start), 1, inner_w, prefix + results[i].title, (i==sel));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// QUEUE panel
// ═══════════════════════════════════════════════════════════════════════════
static void draw_queue(WINDOW* w, Queue& q, bool focused) {
    int wh, ww; getmaxyx(w, wh, ww);
    werase(w);
    draw_box(w, g_cfg.border_chars);

    std::string lbl = std::string("QUEUE") +
                      (focused ? " \xe2\x80\x94 \xe2\x86\x91\xe2\x86\x93=nav  D=remove" : "") +
                      " (" + std::to_string(q.count()) + ")";
    border_label(w, lbl);

    int inner_w = ww - 2;
    int list_rows = wh - 2;

    if (q.empty()) {
        wattron(w, COLOR_PAIR(CP_STATUS));
        std::string msg = "Queue is empty  \xe2\x80\x94  press A to add songs";
        int cx = std::max(1, (ww - (int)msg.size()) / 2);
        mvwprintw(w, wh/2, cx, "%s", msg.c_str());
        wattroff(w, COLOR_PAIR(CP_STATUS));
        return;
    }

    q.clamp();
    int cur   = q.highlight;
    int start = std::max(0, cur - list_rows/2);
    int end   = std::min(q.count(), start + list_rows);
    if (end - start < list_rows) start = std::max(0, end - list_rows);

    const auto& entries = q.entries();
    for (int i = start; i < end; ++i) {
        bool active = (i == cur && focused);
        int  row    = 1 + (i - start);
        if (row >= wh - 1) break;

        if (active) wattron(w, COLOR_PAIR(CP_QUEUE_ACT) | A_BOLD);
        else        wattron(w, COLOR_PAIR(CP_QUEUE_ITEM));

        // Row number prefix
        char prefix[8]; snprintf(prefix, 8, "%2d ", i+1);
        int name_w = inner_w - (int)strlen(prefix) - 1;
        std::string name = utf8_truncate(entries[i].display_name, name_w);
        int pad = name_w - (int)utf8_display_width(name);
        mvwprintw(w, row, 1, "%s%s", prefix, name.c_str());
        for (int p = 0; p < pad; ++p) waddch(w, ' ');
        waddch(w, (i == 0) ? '\xe2' : ' ');   // ► on first (next-to-play)

        if (active) wattroff(w, COLOR_PAIR(CP_QUEUE_ACT) | A_BOLD);
        else        wattroff(w, COLOR_PAIR(CP_QUEUE_ITEM));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// HELP / controls panel
// ═══════════════════════════════════════════════════════════════════════════
static void draw_help(WINDOW* w) {
    int wh, ww; getmaxyx(w, wh, ww); (void)wh;
    werase(w);
    draw_box(w, g_cfg.border_chars);
    border_label(w, "CONTROLS");

    const KeyMap& k = g_cfg.keys;
    // Helper: format a key code as readable string
    auto key_str = [](int code) -> std::string {
        if (code == KEY_UP)        return "\xe2\x86\x91";
        if (code == KEY_DOWN)      return "\xe2\x86\x93";
        if (code == KEY_LEFT)      return "\xe2\x86\x90";
        if (code == KEY_RIGHT)     return "\xe2\x86\x92";
        if (code == '\n')          return "ENTER";
        if (code == '\t')          return "TAB";
        if (code == ' ')           return "SPACE";
        if (code == 27)            return "ESC";
        if (code >= 32 && code < 127) return std::string(1, (char)code);
        return "?";
    };

    struct Row { std::string key; std::string desc; };
    std::vector<Row> left_rows = {
        {key_str(k.nav_up)+"/"+key_str(k.nav_down), "navigate songs"},
        {key_str(k.play),            "play selected"},
        {key_str(k.search),          "search by name"},
        {key_str(k.setting),         "settings"},
        {key_str(k.next_song)+"/"+key_str(k.prev_song), "next / prev song"},
        {key_str(k.seek_fwd)+"/"+key_str(k.seek_bwd), "seek forward / back"},
        {key_str(k.vol_up)+"/"+key_str(k.vol_down), "volume up / down"},
        {key_str(k.queue_add),       "add to queue"},
        {key_str(k.tab_switch),      "switch panels"},
    };
    std::vector<Row> right_rows = {
        {key_str(k.toggle_repeat),   "toggle repeat"},
        {key_str(k.play_pause),      "play / pause"},
        {key_str(k.toggle_shuffle),  "toggle shuffle"},
        {key_str(k.folder_filter),   "filter by folder"},
        {key_str(k.clear_filter),    "clear filter"},
        {key_str(k.quit),            "quit"},
        {key_str(k.reset_prefs),     "reset preferences"},
        {key_str(k.queue_remove),    "remove from queue"},
        {key_str(k.jump_queue),      "jump to queue"},
        {key_str(k.download_stream), "download stream (yt:)"},
    };

    int cw = (ww - 2) / 2;
    int rows = std::min(std::max((int)left_rows.size(), (int)right_rows.size()), wh - 2);
    for (int i = 0; i < rows; ++i) {
        int r = 1 + i;
        if (r >= wh - 1) break;
        if (i < (int)left_rows.size()) {
            wattron(w, COLOR_PAIR(CP_STATUS_KEY) | A_BOLD);
            mvwprintw(w, r, 2, "%-7s", left_rows[i].key.c_str());
            wattroff(w, COLOR_PAIR(CP_STATUS_KEY) | A_BOLD);
            wattron(w, COLOR_PAIR(CP_STATUS_VAL));
            wprintw(w, "%-*s", cw - 10, left_rows[i].desc.c_str());
            wattroff(w, COLOR_PAIR(CP_STATUS_VAL));
        }

        wattron(w, COLOR_PAIR(CP_BORDER));
        mvwaddch(w, r, cw + 1, '|');
        wattroff(w, COLOR_PAIR(CP_BORDER));

        if (i < (int)right_rows.size()) {
            wattron(w, COLOR_PAIR(CP_STATUS_KEY) | A_BOLD);
            mvwprintw(w, r, cw + 3, "%-7s", right_rows[i].key.c_str());
            wattroff(w, COLOR_PAIR(CP_STATUS_KEY) | A_BOLD);
            wattron(w, COLOR_PAIR(CP_STATUS_VAL));
            wprintw(w, "%-*s", cw - 10, right_rows[i].desc.c_str());
            wattroff(w, COLOR_PAIR(CP_STATUS_VAL));
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// SETTINGS overlay — 6 tabs, redesigned
// ═══════════════════════════════════════════════════════════════════════════
struct SettingsState {
    int  tab     = 0;    // 0=Colors 1=Viz 2=Layout 3=Keybinds 4=Font 5=Themes
    int  row     = 0;
    int  col     = 0;    // 0=ANSI, 1=brightness for color fields
    bool editing = false;
    std::string buf;
};

static const int SET_TABS = 6;
static const char* SET_TAB_NAMES[SET_TABS] = {
    "1:Colors", "2:Viz", "3:Layout", "4:Keybinds", "5:Font", "6:Themes"
};

// Draw a section header inside the settings panel
static void set_section(WINDOW* w, int row, int ow, const char* title) {
    wattron(w, COLOR_PAIR(CP_SET_HDR) | A_BOLD);
    mvwprintw(w, row, 1, " %-*s", ow - 2, title);
    wattroff(w, COLOR_PAIR(CP_SET_HDR) | A_BOLD);
}

static bool draw_settings(WINDOW* parent, SettingsState& st, int key, bool rich) {
    // Persists to the internal session file always, and additionally
    // writes straight into the real config.txt when the user has opted
    // into that via the "Auto-save config.txt" toggle (Layout tab) — see
    // ConfigParser::write_back()'s doc comment for why this is a separate,
    // explicit opt-in rather than always-on.
    auto persist = [&] {
        g_cfg.save();
        if (g_cfg.auto_save_config && !g_cfg_path.empty())
            ConfigParser::write_back(g_cfg_path, g_cfg);
    };

    int pww, pwh; getmaxyx(parent, pwh, pww);
    const int OW = std::min(80, pww - 4);
    const int OH = std::min(36, pwh - 4);
    int oy = std::max(0, (pwh - OH) / 2);
    int ox = std::max(0, (pww - OW) / 2);

    WINDOW* w = newwin(OH, OW, oy, ox);
    keypad(w, TRUE);
    bool done = false;

    // ── Color fields (tab 0) ──────────────────────────────────────────────
    struct CField { const char* label; Color* c; };
    CField cfields[] = {
        {"Border / UI",      &g_cfg.colors.border},
        {"Title bar",        &g_cfg.colors.title},
        {"Meta key labels",  &g_cfg.colors.meta_key},
        {"Track name",       &g_cfg.colors.meta_val},
        {"Progress fill",    &g_cfg.colors.progress},
        {"Progress bg",      &g_cfg.colors.progress_bg},
        {"Status text",      &g_cfg.colors.status},
        {"Playlist text",    &g_cfg.colors.playlist},
        {"Active song (fg)", &g_cfg.colors.pl_active_fg},
        {"Active song (bg)", &g_cfg.colors.pl_active_bg},
        {"Viz bars",         &g_cfg.colors.viz},
        {"Viz left",         &g_cfg.colors.viz_left},
        {"Viz center",          &g_cfg.colors.viz_center},
        {"Viz right",       &g_cfg.colors.viz_right},
        {"Peak dots",        &g_cfg.colors.viz_peak},
        {"Lyrics (dim)",     &g_cfg.colors.lyr_dim},
        {"Lyrics (active)",  &g_cfg.colors.lyr_hi},
        {"Lyrics (word)",    &g_cfg.colors.lyr_word},
        {"Noise field",      &g_cfg.colors.noise},
        {"Queue item",       &g_cfg.colors.queue_item},
        {"Queue selected",   &g_cfg.colors.queue_active},
    };
    const int NCF = (int)(sizeof(cfields)/sizeof(cfields[0]));

    // ── Viz fields (tab 1) ────────────────────────────────────────────────
    int viz_int     = (int)g_cfg.viz_style;
    int bands_int   = g_cfg.viz_bands;
    int peak_int    = g_cfg.peak_hold ? 1 : 0;
    int density_int = g_cfg.viz_density;
    struct IField { const char* label; int* v; const char* hint; };
    IField vfields[] = {
        {"Viz style",    &viz_int,     "0=BARS  1=SCOPE"},
        {"Bands",        &bands_int,   "16 or 32"},
        {"Peak hold",    &peak_int,    "0=off  1=on"},
        {"Density",      &density_int, "1=fluid  10=dense"},
    };
    const int NVF = (int)(sizeof(vfields)/sizeof(vfields[0]));

    // ── Layout fields (tab 2) ─────────────────────────────────────────────
    int lmode_int    = (int)g_cfg.layout_mode;
    int autosave_int = g_cfg.auto_save_config ? 1 : 0;
    int results_int  = g_cfg.stream_search_results;
    IField lfields[] = {
        {"Layout mode",       &lmode_int,    "0=AUTO  1=DESKTOP  2=MOBILE"},
        {"Auto-save config.txt", &autosave_int, "0=off  1=on (writes every settings change to config.txt)"},
        {"YT search results", &results_int,  "5-50"},
    };
    const int NLF = (int)(sizeof(lfields)/sizeof(lfields[0]));

    // ── Keybind fields (tab 3) ────────────────────────────────────────────
    struct KField { const char* label; int* v; };
    KField kfields[] = {
        {"Settings",         &g_cfg.keys.setting},
        {"Nav up",           &g_cfg.keys.nav_up},
        {"Nav down",         &g_cfg.keys.nav_down},
        {"Play",             &g_cfg.keys.play},
        {"Search",           &g_cfg.keys.search},
        {"Next song",        &g_cfg.keys.next_song},
        {"Prev song",        &g_cfg.keys.prev_song},
        {"Seek forward",     &g_cfg.keys.seek_fwd},
        {"Seek backward",    &g_cfg.keys.seek_bwd},
        {"Volume up",        &g_cfg.keys.vol_up},
        {"Volume down",      &g_cfg.keys.vol_down},
        {"Queue add",        &g_cfg.keys.queue_add},
        {"Queue remove",     &g_cfg.keys.queue_remove},
        {"Download stream",  &g_cfg.keys.download_stream},
        {"Tab switch",       &g_cfg.keys.tab_switch},
        {"Toggle repeat",    &g_cfg.keys.toggle_repeat},
        {"Play/pause",       &g_cfg.keys.play_pause},
        {"Shuffle",          &g_cfg.keys.toggle_shuffle},
        {"Folder filter",    &g_cfg.keys.folder_filter},
        {"Clear filter",     &g_cfg.keys.clear_filter},
        {"Quit",             &g_cfg.keys.quit},
        {"Reset prefs",      &g_cfg.keys.reset_prefs},
    };
    const int NKF = (int)(sizeof(kfields)/sizeof(kfields[0]));

    // ── Get max rows for current tab ──────────────────────────────────────
    auto tab_max = [&]() -> int {
        switch (st.tab) {
            case 0: return NCF;
            case 1: return NVF;
            case 2: return NLF;
            case 3: return NKF;
            case 4: return (int)g_cfg.font_map.size() > 0 ? 26 : 26; // A-Z
            case 5: return THEME_COUNT;
            default: return 1;
        }
    };

    // ── Key handling ──────────────────────────────────────────────────────
    if (!st.editing) {
        if (key=='q'||key=='Q'||key==27||key==g_cfg.keys.setting) done = true;
        else if (key=='1') { st.tab=0; st.row=0; st.col=0; }
        else if (key=='2') { st.tab=1; st.row=0; st.col=0; }
        else if (key=='3') { st.tab=2; st.row=0; st.col=0; }
        else if (key=='4') { st.tab=3; st.row=0; st.col=0; }
        else if (key=='5') { st.tab=4; st.row=0; st.col=0; }
        else if (key=='6') { st.tab=5; st.row=0; st.col=0; }
        else if (key=='\t') { st.tab = (st.tab + 1) % SET_TABS; st.row = 0; st.col = 0; }
        else if (key==KEY_DOWN||key=='j') { int mx=tab_max(); st.row=(st.row+1)%mx; st.col=0; }
        else if (key==KEY_UP  ||key=='k') { int mx=tab_max(); st.row=(st.row-1+mx)%mx; st.col=0; }
        else if (key==KEY_RIGHT) { if(st.tab==0) st.col=std::min(1,st.col+1); }
        else if (key==KEY_LEFT)  { if(st.tab==0) st.col=std::max(0,st.col-1); }
        else if (key=='\n'||key==KEY_ENTER) {
            if (st.tab == 5) {
                // Apply theme instantly
                g_cfg.apply_theme(st.row);
                if (rich) apply_colors();
                persist();
            } else if (st.tab == 4) {
                // Font map: enter char to type new unicode
                char letter = 'A' + st.row;
                auto it = g_cfg.font_map.find(letter);
                st.buf = (it != g_cfg.font_map.end()) ? it->second : std::string(1, letter);
                st.editing = true;
            } else {
                st.editing = true;
                if (st.tab == 0) {
                    Color& c = *cfields[st.row].c;
                    st.buf = (st.col == 0) ? std::to_string(c.ansi) : std::to_string(c.brightness);
                } else if (st.tab == 1) {
                    st.buf = std::to_string(*vfields[st.row].v);
                } else if (st.tab == 2) {
                    st.buf = std::to_string(*lfields[st.row].v);
                } else if (st.tab == 3) {
                    // Keybind: show current printable / name
                    int code = *kfields[st.row].v;
                    if (code >= 32 && code < 127) st.buf = std::string(1, (char)code);
                    else if (code == KEY_UP)    st.buf = "ARROW_KEY_UP";
                    else if (code == KEY_DOWN)  st.buf = "ARROW_KEY_DOWN";
                    else if (code == KEY_LEFT)  st.buf = "ARROW_KEY_LEFT";
                    else if (code == KEY_RIGHT) st.buf = "ARROW_KEY_RIGHT";
                    else if (code == '\n')       st.buf = "ENTER";
                    else if (code == '\t')       st.buf = "TAB";
                    else                         st.buf = std::to_string(code);
                }
            }
        }
    } else {
        // Editing mode
        if (key=='\n'||key==KEY_ENTER) {
            if (st.tab == 0 && !st.buf.empty()) {
                try {
                    int v = std::stoi(st.buf);
                    Color& c = *cfields[st.row].c;
                    if (st.col == 0) c.ansi = std::clamp(v, 0, 255);
                    else             c.brightness = std::clamp(v, 0, 100);
                    if (rich) apply_colors();
                } catch (...) {}
            } else if (st.tab == 1 && !st.buf.empty()) {
                try {
                    int v = std::stoi(st.buf);
                    switch (st.row) {
                        case 0: g_cfg.viz_style = (VizStyle)std::clamp(v,0,1); break;
                        case 1: g_cfg.viz_bands = (v>=32)?32:16; break;
                        case 2: g_cfg.peak_hold = (v!=0); break;
                        case 3: g_cfg.viz_density = std::clamp(v,1,10); break;
                    }
                } catch (...) {}
            } else if (st.tab == 2 && !st.buf.empty()) {
                try {
                    int v = std::stoi(st.buf);
                    switch (st.row) {
                        case 0: g_cfg.layout_mode = (LayoutMode)std::clamp(v,0,2); break;
                        case 1: g_cfg.auto_save_config = (v != 0); break;
                        case 2: g_cfg.stream_search_results = std::clamp(v, 5, 50); break;
                    }
                } catch (...) {}
            } else if (st.tab == 3) {
                *kfields[st.row].v = KeyMap::parse_key(st.buf);
            } else if (st.tab == 4) {
                // Map the Nth letter to the typed unicode
                char letter = 'A' + st.row;
                if (!st.buf.empty()) {
                    g_cfg.font_map[letter] = st.buf;
                    g_cfg.font_map[(char)tolower((unsigned char)letter)] = st.buf;
                }
            }
            persist();
            st.editing = false; st.buf = "";
        } else if (key == 27) {
            st.editing = false; st.buf = "";
        } else if (key==KEY_BACKSPACE||key==127||key==8) {
            // Unicode-safe backspace: remove last codepoint
            if (!st.buf.empty()) {
                auto& s = st.buf;
                // Find last UTF-8 lead byte
                size_t i = s.size() - 1;
                while (i > 0 && (s[i] & 0xC0) == 0x80) --i;
                s = s.substr(0, i);
            }
        } else if (key >= 32) {
            // Accept any printable or multibyte input
            // For number tabs: digits and minus only
            if (st.tab == 0 || st.tab == 1 || st.tab == 2) {
                if ((key >= '0' && key <= '9') || (key == '-' && st.buf.empty()))
                    if (st.buf.size() < 4) st.buf += (char)key;
            } else if (st.tab == 3 || st.tab == 4) {
                // Accept any printable char (including unicode via multibyte)
                if (key < 128) st.buf += (char)key;
            }
        }
    }

    // ── Draw the panel ────────────────────────────────────────────────────
    werase(w);
    wattron(w, COLOR_PAIR(CP_SET_HDR) | A_BOLD);
    box(w, 0, 0);
    const char* hdr = " \xe2\x9c\xa6 CLI.MUSIC.COM  SETTINGS \xe2\x9c\xa6 ";
    mvwprintw(w, 0, std::max(0, (OW - (int)strlen(hdr)) / 2), "%s", hdr);
    wattroff(w, COLOR_PAIR(CP_SET_HDR) | A_BOLD);

    // Tab bar row 1
    int tx = 1;
    for (int t = 0; t < SET_TABS; ++t) {
        bool active = (t == st.tab);
        if (active) wattron(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
        else        wattron(w, COLOR_PAIR(CP_SET_TAB));
        mvwprintw(w, 1, tx, " %s ", SET_TAB_NAMES[t]);
        if (active) wattroff(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
        else        wattroff(w, COLOR_PAIR(CP_SET_TAB));
        tx += (int)strlen(SET_TAB_NAMES[t]) + 3;
    }

    // Separator row 2
    wattron(w, COLOR_PAIR(CP_SET_HDR));
    mvwhline(w, 2, 1, ACS_HLINE, OW - 2);
    wattroff(w, COLOR_PAIR(CP_SET_HDR));

    // Column headers row 3
    wattron(w, A_UNDERLINE | COLOR_PAIR(CP_SET_ITEM));
    switch (st.tab) {
        case 0:
            mvwprintw(w, 3, 2, "%-24s  %-6s  %-6s  %-8s", "ELEMENT", "ANSI", "BRITE%", "PREVIEW");
            break;
        case 1: case 2:
            mvwprintw(w, 3, 2, "%-24s  %-10s  %s", "PARAMETER", "VALUE", "HINT");
            break;
        case 3:
            mvwprintw(w, 3, 2, "%-20s  %-12s", "ACTION", "KEY");
            break;
        case 4:
            mvwprintw(w, 3, 2, "%-6s  %-12s  %s", "LETTER", "DISPLAY CHAR", "Enter to edit");
            break;
        case 5:
            mvwprintw(w, 3, 2, "%-4s  %-16s  %s", "#", "THEME", "DESCRIPTION");
            break;
    }
    wattroff(w, A_UNDERLINE | COLOR_PAIR(CP_SET_ITEM));

    const int VISIBLE = OH - 6;
    const int CONTENT_START = 4;

    // ── Tab content ───────────────────────────────────────────────────────
    if (st.tab == 0) {
        // Colors
        int scroll = std::max(0, st.row - VISIBLE + 2);
        for (int i = 0; i < NCF; ++i) {
            int dr = i - scroll;
            if (dr < 0 || dr >= VISIBLE) continue;
            int wr = CONTENT_START + dr;
            bool act = (i == st.row);
            if (act) wattron(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattron(w, COLOR_PAIR(CP_SET_ITEM));

            mvwprintw(w, wr, 2, "%-24s  ", cfields[i].label);
            Color& c = *cfields[i].c;

            // ANSI column
            bool ansi_sel = (act && st.col == 0);
            bool bri_sel  = (act && st.col == 1);
            if (ansi_sel && st.editing) wattron(w, A_UNDERLINE);
            std::string av = (ansi_sel && st.editing) ? st.buf + "_" : std::to_string(c.ansi);
            wprintw(w, "%-6s  ", av.c_str());
            if (ansi_sel && st.editing) wattroff(w, A_UNDERLINE);

            // Brightness column
            if (bri_sel && st.editing) wattron(w, A_UNDERLINE);
            std::string bv = (bri_sel && st.editing) ? st.buf + "_" : std::to_string(c.brightness) + "%";
            wprintw(w, "%-6s  ", bv.c_str());
            if (bri_sel && st.editing) wattroff(w, A_UNDERLINE);

            // Color swatch (block characters in ANSI color if supported)
            wprintw(w, "%.8s", "\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88");

            if (act) wattroff(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattroff(w, COLOR_PAIR(CP_SET_ITEM));
        }
    } else if (st.tab == 1) {
        // Viz settings
        for (int i = 0; i < NVF; ++i) {
            int wr = CONTENT_START + i;
            if (wr >= OH - 2) break;
            bool act = (i == st.row);
            if (act) wattron(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattron(w, COLOR_PAIR(CP_SET_ITEM));
            mvwprintw(w, wr, 2, "%-24s  ", vfields[i].label);
            std::string vs = (act && st.editing) ? st.buf + "_" : std::to_string(*vfields[i].v);
            if (i == 0 && !(act && st.editing))
                vs += "  (" + std::string(VIZ_STYLE_NAMES[std::clamp(*vfields[i].v,0,1)]) + ")";
            wprintw(w, "%-18s  %s", vs.c_str(), vfields[i].hint);
            if (act) wattroff(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattroff(w, COLOR_PAIR(CP_SET_ITEM));
        }
        // Density bar
        int br = CONTENT_START + NVF + 1;
        if (br < OH - 2) {
            wattron(w, COLOR_PAIR(CP_SET_ITEM));
            mvwprintw(w, br, 2, "Density: [");
            wattron(w, COLOR_PAIR(CP_VIZ) | A_BOLD);
            for (int d = 1; d <= 10; ++d)
                waddch(w, d <= g_cfg.viz_density ? '#' : '-');
            wattroff(w, COLOR_PAIR(CP_VIZ) | A_BOLD);
            wattron(w, COLOR_PAIR(CP_SET_ITEM));
            wprintw(w, "] %d/10", g_cfg.viz_density);
            wattroff(w, COLOR_PAIR(CP_SET_ITEM));
        }
    } else if (st.tab == 2) {
        // Layout
        for (int i = 0; i < NLF; ++i) {
            int wr = CONTENT_START + i;
            bool act = (i == st.row);
            if (act) wattron(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattron(w, COLOR_PAIR(CP_SET_ITEM));
            mvwprintw(w, wr, 2, "%-24s  ", lfields[i].label);
            std::string vs = (act && st.editing) ? st.buf + "_" : std::to_string(*lfields[i].v);
            wprintw(w, "%-6s  %s", vs.c_str(), lfields[i].hint);
            if (act) wattroff(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattroff(w, COLOR_PAIR(CP_SET_ITEM));
        }
        // Show current layout string truncated
        int lr = CONTENT_START + NLF + 1;
        if (lr < OH - 2) {
            wattron(w, COLOR_PAIR(CP_SET_ITEM));
            mvwprintw(w, lr, 2, "Desk layout (config.txt):");
            std::string dl = utf8_truncate(g_cfg.desk_layout, OW - 6);
            mvwprintw(w, lr + 1, 3, "%s", dl.c_str());
            mvwprintw(w, lr + 2, 2, "Mobile layout:");
            std::string ml = utf8_truncate(g_cfg.mobile_layout, OW - 6);
            mvwprintw(w, lr + 3, 3, "%s", ml.c_str());
            mvwprintw(w, lr + 5, 2, "\xe2\x84\xb9  Edit layout in config.txt for 100%% control.");
            wattroff(w, COLOR_PAIR(CP_SET_ITEM));
        }
    } else if (st.tab == 3) {
        // Keybinds
        int scroll = std::max(0, st.row - VISIBLE + 2);
        auto kstr = [](int code) -> std::string {
            if (code == KEY_UP)    return "↑";
            if (code == KEY_DOWN)  return "↓";
            if (code == KEY_LEFT)  return "←";
            if (code == KEY_RIGHT) return "→";
            if (code == '\n')      return "ENTER";
            if (code == '\t')      return "TAB";
            if (code == ' ')       return "SPACE";
            if (code == 27)        return "ESC";
            if (code >= 32 && code < 127) return std::string(1,(char)code);
            return "?";
        };
        for (int i = 0; i < NKF; ++i) {
            int dr = i - scroll;
            if (dr < 0 || dr >= VISIBLE) continue;
            int wr = CONTENT_START + dr;
            bool act = (i == st.row);
            if (act) wattron(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattron(w, COLOR_PAIR(CP_SET_ITEM));
            mvwprintw(w, wr, 2, "%-20s  ", kfields[i].label);
            std::string ks = (act && st.editing) ? st.buf + "_" : kstr(*kfields[i].v);
            wprintw(w, "%-12s", ks.c_str());
            if (act) wattroff(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattroff(w, COLOR_PAIR(CP_SET_ITEM));
        }
    } else if (st.tab == 4) {
        // Font map — show A-Z
        for (int i = 0; i < 26 && (CONTENT_START + i) < OH - 2; ++i) {
            int wr  = CONTENT_START + i;
            char uc = 'A' + i;
            bool act = (i == st.row);
            if (act) wattron(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattron(w, COLOR_PAIR(CP_SET_ITEM));
            auto it_u = g_cfg.font_map.find(uc);
            auto it_l = g_cfg.font_map.find((char)('a' + i));
            std::string uv = (it_u != g_cfg.font_map.end()) ? it_u->second : std::string(1, uc);
            std::string lv = (it_l != g_cfg.font_map.end()) ? it_l->second : std::string(1, (char)('a'+i));
            if (act && st.editing)
                mvwprintw(w, wr, 2, "  %c / %c   %s_", uc, (char)('a'+i), st.buf.c_str());
            else
                mvwprintw(w, wr, 2, "  %c / %c   %-14s  %-14s",
                    uc, (char)('a'+i), uv.c_str(), lv.c_str());
            if (act) wattroff(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattroff(w, COLOR_PAIR(CP_SET_ITEM));
        }
        wattron(w, COLOR_PAIR(CP_SET_ITEM));
        mvwprintw(w, OH - 3, 2,
            "\xe2\x84\xb9  For full font control edit font_en={} in config.txt");
        wattroff(w, COLOR_PAIR(CP_SET_ITEM));
    } else if (st.tab == 5) {
        // Themes
        static const char* descs[THEME_COUNT] = {
            "Blue/cyan palette  — default experience",
            "Deep ocean blues   — calm and focused",
            "Forest greens      — natural and earthy",
            "Warm sunset reds   — vibrant and energetic",
            "Midnight purples   — dark and mysterious",
            "Neon pink/green    — high-contrast hacker",
        };
        for (int i = 0; i < THEME_COUNT && (CONTENT_START + i) < OH - 2; ++i) {
            int wr  = CONTENT_START + i;
            bool act = (i == st.row);
            bool cur = (i == g_cfg.theme_idx);
            if (act) wattron(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattron(w, COLOR_PAIR(CP_SET_ITEM));
            mvwprintw(w, wr, 2, "%2d.  %-12s  %-40s %s",
                i + 1, THEMES[i].name, descs[i], cur ? "[ACTIVE]" : "");
            if (act) wattroff(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattroff(w, COLOR_PAIR(CP_SET_ITEM));
        }
    }

    // ── Footer ─────────────────────────────────────────────────────────────
    wattron(w, COLOR_PAIR(CP_SET_HDR));
    mvwprintw(w, OH - 1, 2,
        "Tab/1-6=switch  ↑↓=nav  ←→=field  Enter=edit  S/Esc=close");
    wattroff(w, COLOR_PAIR(CP_SET_HDR));

    wrefresh(w);
    if (done) { werase(w); wrefresh(w); }
    delwin(w);
    return !done;
}

// ═══════════════════════════════════════════════════════════════════════════
// Dynamic window manager using layout engine
// ═══════════════════════════════════════════════════════════════════════════
struct LayoutWindows {
    struct Entry {
        BlockId   id;
        WINDOW*   win = nullptr;
        PanelRect rect;
    };
    std::vector<Entry> entries;
    int last_cols = 0, last_rows = 0;
    bool last_desktop = false;

    void build(int cols, int rows, bool desktop) {
        destroy();
        last_cols = cols; last_rows = rows; last_desktop = desktop;

        // Select layout + row heights based on mode
        const std::string& layout_str = desktop
            ? g_cfg.desk_layout : g_cfg.mobile_layout;
        const std::vector<int>& row_h = desktop
            ? g_cfg.desk_row_h : g_cfg.mobile_row_h;

        // Header is always 3 rows at the top
        static constexpr int HDR_H = 3;
        int content_h = rows - HDR_H - 1; // -1 for footer
        (void)content_h;

        auto row_specs = parse_layout(layout_str, row_h);
        auto lr = compute_layout(row_specs, cols, rows - HDR_H);

        // Header entry (always present, y=0)
        {
            Entry e;
            e.id   = BlockId::NONE;  // special: header
            e.rect = {BlockId::NONE, 0, 0, cols, HDR_H, true};
            e.win  = newwin(HDR_H, cols, 0, 0);
            if (e.win) keypad(e.win, TRUE);
            entries.push_back(e);
        }

        // Content panels
        for (auto& pr : lr.panels) {
            Entry e;
            e.id   = pr.id;
            e.rect = pr;
            e.rect.y += HDR_H;   // shift down by header height
            e.win = newwin(std::max(2, e.rect.h),
                            std::max(4, e.rect.w),
                            e.rect.y, e.rect.x);
            if (e.win) keypad(e.win, TRUE);
            entries.push_back(e);
        }
    }

    WINDOW* get(BlockId id) const {
        for (auto& e : entries) if (e.id == id) return e.win;
        return nullptr;
    }
    WINDOW* header() const { return entries.empty() ? nullptr : entries[0].win; }

    bool has(BlockId id) const {
        for (auto& e : entries) if (e.id == id && e.win) return true;
        return false;
    }

    bool needs_rebuild(int cols, int rows, bool desktop) const {
        return cols != last_cols || rows != last_rows || desktop != last_desktop;
    }

    void destroy() {
        for (auto& e : entries) if (e.win) { delwin(e.win); e.win = nullptr; }
        entries.clear();
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// main()
// ═══════════════════════════════════════════════════════════════════════════
int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    setlocale(LC_ALL, "");

    // ── Load settings ────────────────────────────────────────────────────
    g_cfg.load();   // load saved runtime settings first
    std::string cfg_path = ConfigParser::find_config();
    if (!cfg_path.empty()) ConfigParser::parse(cfg_path, g_cfg);  // overlay config.txt
    g_cfg_path = cfg_path;

    // ── Init ncurses ──────────────────────────────────────────────────────
    initscr(); cbreak(); noecho();
    keypad(stdscr, TRUE); curs_set(0);
    nodelay(stdscr, TRUE); set_escdelay(50);
    start_color(); use_default_colors();
    bool rich = can_change_color();
    if (rich) apply_colors(); else apply_basic_colors();

    // ── Init player ───────────────────────────────────────────────────────
    Player player;
    if (!player.init()) {
        endwin();
        fprintf(stderr, "Player init failed: %s\n", player.status_msg.c_str());
        return 1;
    }

    Lyrics   lyrics;
    NoiseGen noise;

    // ── Streaming state ──────────────────────────────────────────────────
    // v2: exactly one network connection per stream — see stream.h's top
    // comment. StreamPlayer tees the single yt-dlp fetch to a disk cache
    // (StreamCache) + a FIFO that mpv/ffplay actually play from;
    // StreamVizFeed gets visualizer PCM by tailing that same cache file
    // rather than fetching anything itself.
    StreamSearch stream_search;
    StreamPlayer stream_player;
    stream_player.init();
    StreamVizFeed stream_viz_feed;
    bool         stream_browse   = false;   // browsing yt: search results
    int          stream_sel      = 0;
    bool         is_streaming    = false;   // currently playing a stream
    StreamResult current_stream;
    int          stream_volume   = 80;

    // v3: for mpv, StreamPlayer itself now delays its cache-only fetch by
    // ~3s (see spawn_pipeline()'s comment) before the cache file even
    // exists — the visualizer tap needs to wait at least that long too, or
    // `tail -f` on a not-yet-existing file just fails immediately. The
    // lyrics fetch is a real *second* YouTube connection when it falls
    // back to yt-dlp subtitles, so it keeps an even longer, separate head
    // start to avoid colliding with whichever connection(s) are already
    // establishing.
    static constexpr double kVizFeedDelaySec    = 3.5;
    static constexpr double kLyricsFetchDelaySec = 6.0;
    bool            viz_pending    = false;
    bool            lyrics_pending = false;
    clk::time_point viz_at{};
    clk::time_point lyrics_at{};

    std::string      header_flash;
    clk::time_point   header_flash_until{};
    auto flash = [&](const std::string& msg, double secs = 2.5) {
        header_flash = msg;
        header_flash_until = clk::now() + std::chrono::duration_cast<clk::duration>(
            std::chrono::duration<double>(secs));
    };

    auto sanitize_for_path = [](std::string s) {
        for (char& c : s) if (c=='/'||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|') c = '_';
        if (s.size() > 120) s.resize(120);
        return s;
    };

    auto stop_stream = [&] {
        if (is_streaming) { stream_player.stop(); stream_viz_feed.stop(); is_streaming = false; }
        viz_pending    = false;
        lyrics_pending = false;
    };

    auto load_track = [&](const std::string& path) {
        stop_stream();
        player.load_and_play(path);
        const auto& m = player.current_meta();
        lyrics.load(path, m.title, m.artist);
        noise.phase = 0.0;
    };

    auto play_stream = [&](const StreamResult& r) {
        stop_stream();
        player.stop();

        // Already fully downloaded from a previous play? Just play it as
        // an ordinary local file — real seeking, full visualizer, zero
        // network. This is also how a cached track keeps working forever
        // after StreamCache::dir() gets pruned/cleared by hand.
        std::string cached = StreamCache::path_for(r.url, r.title);
        if (StreamCache::is_complete(cached)) {
            load_track(cached);
            flash("Playing from cache \xe2\x80\x94 \xe2\x9c\x93 " + r.title, 2.0);
            return;
        }

        if (!stream_player.play(r.url, r.title)) {
            flash("Playback failed \xe2\x80\x94 install mpv or ffplay");
            return;
        }
        stream_player.set_volume(stream_volume);
        is_streaming    = true;
        current_stream  = r;
        noise.phase     = 0.0;

        viz_pending  = true;
        viz_at       = clk::now() + std::chrono::duration_cast<clk::duration>(
            std::chrono::duration<double>(kVizFeedDelaySec));
        lyrics_pending = true;
        lyrics_at      = clk::now() + std::chrono::duration_cast<clk::duration>(
            std::chrono::duration<double>(kLyricsFetchDelaySec));
    };

    // Queue entries are normally local files, but a yt: result can be
    // queued too — encoded as a PlaylistEntry whose path is prefixed with
    // "ytstream://" (stripped back off, with the url recovered, when the
    // queue is drained). This keeps Queue/PlaylistEntry generic rather
    // than teaching them about streaming specifically.
    static constexpr const char* kYtQueuePrefix = "ytstream://";
    auto make_stream_queue_entry = [](const StreamResult& r) {
        PlaylistEntry e;
        e.path         = std::string(kYtQueuePrefix) + r.url;
        e.display_name = r.title;
        e.folder_name  = "YouTube";
        return e;
    };
    auto play_queue_entry = [&](const PlaylistEntry& entry) {
        if (entry.path.rfind(kYtQueuePrefix, 0) == 0) {
            play_stream(StreamResult{entry.display_name, entry.path.substr(std::strlen(kYtQueuePrefix))});
        } else {
            load_track(entry.path);
        }
    };

    if (player.playlist().count() > 0)
        if (auto* e = player.playlist().current()) load_track(e->path);

    // Previously-completed stream downloads reappear in the local library
    // across restarts too (import() is additive, unlike load_dir()).
    player.playlist().import(StreamCache::dir());

    // ── Build windows ─────────────────────────────────────────────────────
    int COLS_ = 0, ROWS_ = 0; getmaxyx(stdscr, ROWS_, COLS_);
    auto is_desktop = [&]() -> bool {
        if (g_cfg.layout_mode == LayoutMode::DESKTOP) return true;
        if (g_cfg.layout_mode == LayoutMode::MOBILE)  return false;
        return COLS_ >= DESKTOP_WIDTH_THRESHOLD;
    };

    LayoutWindows wins;
    wins.build(COLS_, ROWS_, is_desktop());

    // ── State ─────────────────────────────────────────────────────────────
    bool settings_open = false;
    bool search_active = false;
    bool queue_focused = false;
    bool quit          = false;

    SettingsState set_st;
    SearchState   srch;

    std::string text_input, text_input_label;
    enum class InputMode { NONE, FOLDER_FILTER } input_mode = InputMode::NONE;

    auto last_frame = clk::now();

    // ═══════════════════════════════════════════════════════════════════════
    // Main loop
    // ═══════════════════════════════════════════════════════════════════════
    while (!quit) {
        // ── Resize check ──────────────────────────────────────────────────
        {
            int nc, nr; getmaxyx(stdscr, nr, nc);
            if (wins.needs_rebuild(nc, nr, is_desktop())) {
                COLS_ = nc; ROWS_ = nr;
                wins.build(COLS_, ROWS_, is_desktop());
                clear(); refresh();
            }
        }

        // ── Timing ────────────────────────────────────────────────────────
        auto now = clk::now();
        double dt = std::chrono::duration<double>(now - last_frame).count();
        last_frame = now;
        noise.advance(dt);

        // ── Streaming: detect the stream ending, failing, or resolving ─────
        if (is_streaming && (stream_player.is_ended() || stream_player.is_failed())) {
            if (stream_player.is_failed())
                flash("Stream failed: " + stream_player.last_error(), 6.0);
            else
                flash("Stream ended");
            is_streaming   = false;
            viz_pending    = false;
            lyrics_pending = false;
            stream_player.stop();
            stream_viz_feed.stop();
        }

        // A track finished downloading (StreamPlayer's fetch pipeline
        // exited cleanly) — register it into the local library so it's
        // playable/seekable/visualizable as a normal file from now on.
        if (auto done = stream_player.take_completed_download(); !done.empty())
            player.playlist().add(done);

        // ── Fire the deferred visualizer PCM tap (tails the cache file —
        // no network) once it's had a moment to exist, and the lyrics
        // fetch (a real second YouTube connection when it falls back to
        // yt-dlp subtitles) once the primary stream has had a longer,
        // uncontested head start. Staggered from each other too, so we
        // never open two network connections to the same video at once.
        if (viz_pending && is_streaming && clk::now() >= viz_at) {
            viz_pending = false;
            if (g_cfg.stream_visualizer)
                stream_viz_feed.start(stream_player.cache_path(), player.visualizer(), player.vocal_viz(), stream_player);
        }
        if (lyrics_pending && is_streaming && clk::now() >= lyrics_at) {
            lyrics_pending = false;
            std::string cache_key = (fs::temp_directory_path() /
                (sanitize_for_path(current_stream.title) + ".stream")).string();
            lyrics.load(cache_key, current_stream.title, "");
        }

        // ── Player state icon ─────────────────────────────────────────────
        std::string icon;
        if (is_streaming) {
            icon = stream_player.is_resolving() ? "\xe2\x8f\xb3"                        // ⏳ resolving
                 : stream_player.is_paused()    ? "\xe2\x8f\xb8" : "\xe2\x96\xb6";
        } else switch (player.state()) {
            case PlayerState::PLAYING: icon = "\xe2\x96\xb6"; break;  // ▶
            case PlayerState::PAUSED:  icon = "\xe2\x8f\xb8"; break;  // ⏸
            case PlayerState::STOPPED: icon = "\xe2\x8f\xb9"; break;  // ⏹
        }

        double cur_pos = is_streaming ? stream_player.position() : player.position();
        double cur_dur = is_streaming ? stream_player.duration() : player.duration();
        int    cur_vol = is_streaming ? stream_volume             : player.volume();

        TrackMeta stream_meta;
        if (is_streaming) {
            stream_meta.title    = current_stream.title;
            stream_meta.artist   = stream_player.is_resolving() ? "Resolving stream\xe2\x80\xa6" : "YouTube stream";
            stream_meta.album    = current_stream.url;
            stream_meta.format   = (stream_player.backend() == StreamBackend::MPV) ? "stream (mpv)" : "stream (ffplay)";
            stream_meta.duration_sec = (int)cur_dur;
        }
        const TrackMeta& meta_view = is_streaming ? stream_meta : player.current_meta();

        std::string header_title;
        if (clk::now() < header_flash_until) header_title = header_flash;
        else if (is_streaming) header_title = stream_player.is_resolving()
            ? ("\xe2\x8f\xb3 Resolving: " + current_stream.title)
            : ("\xe2\x97\x89 STREAMING: " + current_stream.title);

        // ── Draw all panels (skip if settings open) ───────────────────────
        if (!settings_open) {
            // Header
            if (auto* h = wins.header())
                draw_header(h, COLS_, icon,
                    player.is_repeat(), player.is_shuffle(), cur_vol, header_title);

            // Each layout panel
            if (auto* w = wins.get(BlockId::META))
                draw_meta(w, meta_view);

            if (auto* w = wins.get(BlockId::COVER))
                draw_cover(w, meta_view);

            if (auto* w = wins.get(BlockId::LYRICS))
                draw_lyrics(w, lyrics, cur_pos, noise);

            bool viz_live = is_streaming && g_cfg.stream_visualizer && StreamVizFeed::available();

            if (auto* w = wins.get(BlockId::VOCAL_VIZ)) {
                if (is_streaming && !viz_live) draw_stream_viz_placeholder(w, "VOCAL VISUALIZER");
                else                           draw_vocal_viz(w, player.vocal_viz());
            }

            if (auto* w = wins.get(BlockId::VIZ)) {
                if (is_streaming && !viz_live) draw_stream_viz_placeholder(w, "VISUALIZER");
                else                           draw_viz(w, player);
            }

            if (auto* w = wins.get(BlockId::PROGRESS_BAR))
                draw_progress(w, cur_pos, cur_dur);

            if (auto* w = wins.get(BlockId::LIST)) {
                if (stream_browse) draw_stream_results(w, stream_search, stream_sel, is_streaming, current_stream.url);
                else draw_list(w, player.playlist(), search_active, srch, ERR);
            }

            if (auto* w = wins.get(BlockId::QUEUE))
                draw_queue(w, player.queue(), queue_focused);

            if (auto* w = wins.get(BlockId::HELP))
                draw_help(w);

            // Flush all
            for (auto& e : wins.entries)
                if (e.win) wrefresh(e.win);
        }

        // ── Input ─────────────────────────────────────────────────────────
        int ch = getch();

        if (settings_open) {
            settings_open = draw_settings(stdscr, set_st, ch, rich);
            if (!settings_open) {
                if (rich) apply_colors();
                clear(); refresh();
                wins.build(COLS_, ROWS_, is_desktop());
            }
            goto frame_end;
        }

        // Queue focus: handle navigation inside queue panel
        if (queue_focused) {
            if (ch == KEY_UP   || ch == g_cfg.keys.nav_up)   { player.queue().nav_up();   goto frame_end; }
            if (ch == KEY_DOWN || ch == g_cfg.keys.nav_down)  { player.queue().nav_down(); goto frame_end; }
            if (ch == g_cfg.keys.queue_remove || ch == 'd' || ch == 'D') {
                player.queue().remove(player.queue().highlight);
                player.queue().clamp();
                goto frame_end;
            }
            if (ch == '\t' || ch == g_cfg.keys.tab_switch || ch == 27) {
                queue_focused = false; goto frame_end;
            }
            goto frame_end; // absorb other keys
        }

        // Stream results browse mode (after a "yt:<query>" search)
        if (stream_browse) {
            if (ch != ERR) {
                if (ch == 27 || ch == g_cfg.keys.search) {
                    stream_browse = false;
                    stream_search.cancel();
                } else if (ch == KEY_UP) {
                    if (stream_sel > 0) --stream_sel;
                } else if (ch == KEY_DOWN) {
                    int n = (int)stream_search.results().size();
                    if (stream_sel < n - 1) ++stream_sel;
                } else if (ch == '\n' || ch == KEY_ENTER) {
                    auto results = stream_search.results();
                    if (stream_sel >= 0 && stream_sel < (int)results.size())
                        play_stream(results[stream_sel]);
                    // Stay in the results list — don't kick back to local
                    // music on every play. Only ESC (or the search key
                    // again, handled above) returns to the local list.
                } else if (ch == g_cfg.keys.queue_add) {
                    auto results = stream_search.results();
                    if (stream_sel >= 0 && stream_sel < (int)results.size()) {
                        player.queue().push(make_stream_queue_entry(results[stream_sel]));
                        flash("Queued: " + results[stream_sel].title, 1.5);
                    }
                }
                // BUGFIX (pause/seek "not working in the YT window"): this
                // block used to swallow *every* key while browsing results
                // (unconditional goto below), so transport-control keys
                // never reached the dispatch that actually handles them —
                // they only worked once you left this list. Handle them
                // here too, mirroring the general dispatch further down.
                else if (ch == g_cfg.keys.play_pause || ch == ' ') {
                    if (is_streaming) stream_player.toggle_pause();
                } else if (ch == g_cfg.keys.seek_fwd) {
                    if (is_streaming) stream_player.seek_relative(+10.0);
                } else if (ch == g_cfg.keys.seek_bwd) {
                    if (is_streaming) stream_player.seek_relative(-10.0);
                } else if (ch == g_cfg.keys.vol_up) {
                    if (is_streaming) { stream_volume = std::min(100, stream_volume+5); stream_player.set_volume(stream_volume); }
                } else if (ch == g_cfg.keys.vol_down) {
                    if (is_streaming) { stream_volume = std::max(0, stream_volume-5); stream_player.set_volume(stream_volume); }
                } else if (ch == g_cfg.keys.download_stream) {
                    if (is_streaming) {
                        if (StreamDownloader::download(current_stream.url, Playlist::MUSIC_DIR))
                            flash("Downloading \xe2\x80\x9c" + current_stream.title + "\xe2\x80\x9d\xe2\x80\xa6");
                        else
                            flash("yt-dlp not found \xe2\x80\x94 can't download");
                    }
                }
            }
            goto frame_end;
        }

        // Search mode (local filename search; "yt:<query>" + Enter switches
        // into an online search instead of matching local files)
        if (search_active) {
            bool is_yt_query = srch.query.size() > 3 &&
                (srch.query[0]=='y'||srch.query[0]=='Y') &&
                (srch.query[1]=='t'||srch.query[1]=='T') &&
                srch.query[2]==':';

            if (is_yt_query && (ch == '\n' || ch == KEY_ENTER)) {
                std::string q = srch.query.substr(3);
                size_t s = q.find_first_not_of(' ');
                q = (s == std::string::npos) ? "" : q.substr(s);
                search_active = false; srch = {};
                if (!q.empty()) {
                    stream_search.search(q, g_cfg.stream_search_results);
                    stream_browse = true;
                    stream_sel    = 0;
                }
            } else if (ch != ERR && wins.has(BlockId::LIST)) {
                int res = draw_list(wins.get(BlockId::LIST),
                                    player.playlist(), search_active, srch, ch);
                if (wins.get(BlockId::LIST)) wrefresh(wins.get(BlockId::LIST));
                if (res == -2) { search_active = false; srch = {}; }
                else if (res >= 0) {
                    search_active = false;
                    player.playlist().select(res);
                    if (auto* e = player.playlist().current()) load_track(e->path);
                    srch = {};
                }
            }
            goto frame_end;
        }

        if (ch == ERR) goto frame_end;

        // Text input mode (folder filter)
        if (input_mode != InputMode::NONE) {
            if (ch == '\n' || ch == KEY_ENTER) {
                if (input_mode == InputMode::FOLDER_FILTER) {
                    if (text_input.empty()) player.playlist().clear_folder_filter();
                    else player.playlist().set_folder_filter(text_input);
                }
                text_input.clear(); input_mode = InputMode::NONE;
            } else if (ch == 27) {
                text_input.clear(); input_mode = InputMode::NONE;
            } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                if (!text_input.empty()) text_input.pop_back();
            } else if (ch >= 32 && ch < 127) {
                text_input += (char)ch;
            }
            // Show input in the help/footer area if available
            if (auto* hw = wins.get(BlockId::HELP)) {
                werase(hw); draw_box(hw, g_cfg.border_chars);
                wattron(hw, COLOR_PAIR(CP_SEARCH_BAR) | A_BOLD);
                int hwh, hww; getmaxyx(hw, hwh, hww); (void)hwh;
                mvwprintw(hw, 1, 2, "%s%s_",
                    text_input_label.c_str(),
                    text_input.substr(0, std::max(0, hww - (int)text_input_label.size() - 4)).c_str());
                wattroff(hw, COLOR_PAIR(CP_SEARCH_BAR) | A_BOLD);
                wrefresh(hw);
            }
            goto frame_end;
        }

        // ── Key dispatch ──────────────────────────────────────────────────
        {
            const KeyMap& k = g_cfg.keys;

            if (ch == k.quit)           { quit = true; }
            else if (ch == k.play_pause || ch == ' ') {
                if (is_streaming) {
                    stream_player.toggle_pause();
                } else if (player.state() == PlayerState::STOPPED) {
                    // BUGFIX (audit D3): Player::play_pause() used to
                    // auto-load the current track internally when stopped,
                    // bypassing load_track() — the only place lyrics get
                    // fetched. Route through load_track() here instead so
                    // starting playback via play/pause always fetches
                    // lyrics the same as starting it via Enter does.
                    if (auto* e = player.playlist().current()) load_track(e->path);
                } else {
                    player.play_pause();
                }
            }
            else if (ch == k.play || ch == '\n' || ch == KEY_ENTER) {
                if (auto* e = player.playlist().current()) load_track(e->path);
            }
            else if (ch == k.next_song) {
                stop_stream();
                // Check queue first
                if (!player.queue().empty()) {
                    auto entry = player.queue().pop_front();
                    play_queue_entry(entry);
                } else {
                    player.next();
                    if (auto* e = player.playlist().current()) load_track(e->path);
                }
            }
            else if (ch == k.prev_song) {
                stop_stream();
                player.prev();
                if (auto* e = player.playlist().current()) load_track(e->path);
            }
            else if (ch == k.toggle_repeat)  { player.toggle_repeat(); }
            else if (ch == k.toggle_shuffle) { player.toggle_shuffle(); }
            else if (ch == k.vol_up) {
                if (is_streaming) { stream_volume = std::min(100, stream_volume+5); stream_player.set_volume(stream_volume); }
                else                player.set_volume(+1);
            }
            else if (ch == k.vol_down) {
                if (is_streaming) { stream_volume = std::max(0, stream_volume-5); stream_player.set_volume(stream_volume); }
                else                player.set_volume(-1);
            }
            else if (ch == k.seek_fwd) {
                if (is_streaming) stream_player.seek_relative(+10.0);
                else              player.seek(player.position() + 10.0);
            }
            else if (ch == k.seek_bwd) {
                if (is_streaming) stream_player.seek_relative(-10.0);
                else              player.seek(std::max(0.0, player.position() - 10.0));
            }
            else if (ch == k.download_stream) {
                if (is_streaming) {
                    if (StreamDownloader::download(current_stream.url, Playlist::MUSIC_DIR))
                        flash("Downloading \xe2\x80\x9c" + current_stream.title + "\xe2\x80\x9d\xe2\x80\xa6");
                    else
                        flash("yt-dlp not found \xe2\x80\x94 can't download");
                } else {
                    flash("Nothing streaming to download");
                }
            }
            else if (ch == k.nav_down)       { player.playlist().select(player.playlist().current_idx()+1); }
            else if (ch == k.nav_up)         { player.playlist().select(player.playlist().current_idx()-1); }
            else if (ch == k.search) {
                search_active = true; srch = SearchState{};
                if (auto* w = wins.get(BlockId::LIST))
                    { draw_list(w, player.playlist(), search_active, srch, ERR); wrefresh(w); }
            }
            else if (ch == k.folder_filter) {
                input_mode = InputMode::FOLDER_FILTER;
                text_input_label = "Folder filter (empty=clear): ";
                text_input.clear();
            }
            else if (ch == k.clear_filter)  { player.playlist().clear_folder_filter(); }
            else if (ch == k.setting)       { settings_open = true; set_st = {}; }
            else if (ch == k.reset_prefs) {
                g_cfg = Settings{};
                if (!cfg_path.empty()) ConfigParser::parse(cfg_path, g_cfg);
                g_cfg.save();
                if (rich) apply_colors();
                wins.build(COLS_, ROWS_, is_desktop());
            }
            // Queue: add currently highlighted playlist entry
            else if (ch == k.queue_add || ch == 'a' || ch == 'A') {
                if (auto* e = player.playlist().current())
                    player.queue().push(*e);
            }
            // Queue: jump focus to queue panel
            else if (ch == 'Q' || ch == k.jump_queue) {
                if (wins.has(BlockId::QUEUE)) queue_focused = true;
            }
            // Tab: cycle focus (playlist <-> queue)
            else if (ch == k.tab_switch) {
                queue_focused = wins.has(BlockId::QUEUE) && !queue_focused;
            }
        }

        frame_end:
        // ── Auto-advance ──────────────────────────────────────────────────
        // BUGFIX (audit E1): used to rely solely on position() reaching
        // duration()-0.5, which can simply never happen if metadata
        // duration is inaccurately long (common with VBR MP3 padding) —
        // playback would sit PLAYING and silent forever. is_track_done()
        // is set reliably by the decoder itself once it's genuinely out of
        // frames, independent of what the (possibly wrong) metadata claims.
        if (player.state() == PlayerState::PLAYING
            && (player.is_track_done()
                || (player.duration() > 0.5 && player.position() >= player.duration() - 0.5)))
||||||| empty tree
=======
    std::string ql = ss.query;
    std::ranges::transform(ql, ql.begin(), ::tolower);
    const auto& E = pl.entries();
    for (int i = 0; i < (int)E.size(); ++i) {
        std::string dl = E[i].display_name;
        std::ranges::transform(dl, dl.begin(), ::tolower);
        if (dl.find(ql) != std::string::npos) ss.matches.push_back(i);
    }
}

static void draw_pl_row(WINDOW* w, int row, int col, int inner_w,
                         const std::string& name, bool active)
{
    if (active) wattron(w, COLOR_PAIR(CP_PL_ACTIVE) | A_BOLD);
    else        wattron(w, COLOR_PAIR(CP_PLAYLIST));

    const int prefix_w = 2, suffix_w = 1;
    int name_field = inner_w - prefix_w - suffix_w;
    if (name_field < 1) name_field = 1;
    std::string trunc = utf8_truncate(name, name_field);
    int name_dw = utf8_display_width(trunc);
    int pad = name_field - name_dw;

    mvwprintw(w, row, col, "> ");
    waddstr(w, trunc.c_str());
    for (int p = 0; p < pad; ++p) waddch(w, ' ');
    waddch(w, '<');

    if (active) wattroff(w, COLOR_PAIR(CP_PL_ACTIVE) | A_BOLD);
    else        wattroff(w, COLOR_PAIR(CP_PLAYLIST));
}

static int draw_list(WINDOW* w, const Playlist& pl,
                      bool search_active, SearchState& ss, int key)
{
    int ww, wh; getmaxyx(w, wh, ww);
    int result = -1;
    if (search_active) {
        if (key==27 || key=='/') result = -2;
        else if (key=='\n' || key==KEY_ENTER)
            result = ss.matches.empty() ? -2 : ss.matches[ss.sel];
        else if (key==KEY_UP)   { if(ss.sel>0) --ss.sel; }
        else if (key==KEY_DOWN) { if(ss.sel<(int)ss.matches.size()-1) ++ss.sel; }
        else if (key==KEY_BACKSPACE||key==127||key==8)
            { if(!ss.query.empty()){ss.query.pop_back();update_search(ss,pl);} }
        else if (key>=32 && key<127) { ss.query += (char)key; update_search(ss,pl); }
    }
    werase(w);
    draw_box(w, g_cfg.border_chars);
    std::string lbl = pl.has_filter()
        ? "FILTER: " + pl.folder_filter() + " " + std::to_string(pl.count()) + "/" + std::to_string(pl.total_count())
        : Playlist::MUSIC_DIR;
    border_label(w, lbl);

    wattron(w, COLOR_PAIR(CP_SEARCH_BAR));
    wmove(w, 1, 1); waddcp(w, 0x25CB); waddch(w, ' ');
    if (search_active) {
        std::string sq = (ss.query.empty() ? "" : ss.query) + "_";
        int avail = ww - 4; if (avail < 1) avail = 1;
        wprintw(w, "%-*s", avail, sq.substr(0, avail).c_str());
    } else {
        wprintw(w, "%-*s", ww-4, "search ( press / )");
    }
    wattroff(w, COLOR_PAIR(CP_SEARCH_BAR));
    wattron(w, COLOR_PAIR(CP_BORDER));
    mvwhline(w, 2, 1, ACS_HLINE, ww-2);
    wattroff(w, COLOR_PAIR(CP_BORDER));

    int list_rows = wh - 4;
    int inner_w   = ww - 2;

    if (search_active && !ss.query.empty()) {
        int start = std::max(0, ss.sel - list_rows/2);
        int end   = std::min((int)ss.matches.size(), start + list_rows);
        if (end - start < list_rows) start = std::max(0, end - list_rows);
        if (ss.matches.empty()) {
            wattron(w, COLOR_PAIR(CP_STATUS));
            mvwprintw(w, 3, 2, "No results for: %s", ss.query.c_str());
            wattroff(w, COLOR_PAIR(CP_STATUS));
        }
        const auto& E = pl.entries();
        for (int i = start; i < end; ++i)
            draw_pl_row(w, 3+(i-start), 1, inner_w, E[ss.matches[i]].display_name, (i==ss.sel));
    } else {
        int cur   = pl.current_idx();
        int start = std::max(0, cur - list_rows/2);
        int end   = std::min(pl.count(), start + list_rows);
        if (end - start < list_rows) start = std::max(0, end - list_rows);
        const auto& E = pl.entries();
        for (int i = start; i < end; ++i)
            draw_pl_row(w, 3+(i-start), 1, inner_w, E[i].display_name, (i==cur));
    }
    return result;
}

// Shown in place of the FFT/vocal visualizers while streaming: audio decode
// happens inside mpv/ffplay's own process, so there's no PCM here to feed
// the visualizer with (see stream.h's top comment).
static void draw_stream_viz_placeholder(WINDOW* w, const char* label) {
    int wh, ww; getmaxyx(w, wh, ww);
    werase(w);
    draw_box(w, g_cfg.border_chars);
    border_label(w, label);
    wattron(w, COLOR_PAIR(CP_STATUS));
    std::string msg = "\xe2\x99\xaa  streaming \xe2\x80\x94 no visualizer data";
    int cx = std::max(1, (ww - (int)msg.size()) / 2);
    mvwprintw(w, wh/2, cx, "%s", msg.c_str());
    wattroff(w, COLOR_PAIR(CP_STATUS));
}

// ═══════════════════════════════════════════════════════════════════════════
// STREAM RESULTS panel — reuses the LIST window while browsing yt: results
// ═══════════════════════════════════════════════════════════════════════════
static void draw_stream_results(WINDOW* w, StreamSearch& ss, int sel,
                                 bool is_streaming, const std::string& playing_url) {
    int ww, wh; getmaxyx(w, wh, ww);
    werase(w);
    draw_box(w, g_cfg.border_chars);
    std::string qkey = (g_cfg.keys.queue_add >= 32 && g_cfg.keys.queue_add < 127)
        ? std::string(1, (char)g_cfg.keys.queue_add) : "?";
    border_label(w, "YOUTUBE  (ESC=back  ENTER=play  " + qkey + "=queue)");

    wattron(w, COLOR_PAIR(CP_SEARCH_BAR));
    std::string status = ss.status();
    if (ss.state() == StreamSearch::State::SEARCHING) status = "\xe2\x8f\xb3 " + status;
    mvwprintw(w, 1, 1, "%-*s", ww - 3, status.substr(0, std::max(0, ww-3)).c_str());
    wattroff(w, COLOR_PAIR(CP_SEARCH_BAR));
    wattron(w, COLOR_PAIR(CP_BORDER));
    mvwhline(w, 2, 1, ACS_HLINE, ww-2);
    wattroff(w, COLOR_PAIR(CP_BORDER));

    if (ss.state() != StreamSearch::State::DONE) return;

    auto results = ss.results();
    int list_rows = wh - 4;
    int inner_w   = ww - 2;
    int start = std::max(0, sel - list_rows/2);
    int end   = std::min((int)results.size(), start + list_rows);
    if (end - start < list_rows) start = std::max(0, end - list_rows);

    for (int i = start; i < end; ++i) {
        bool playing = is_streaming && results[i].url == playing_url;
        // Duration goes in front — titles can be long enough to get
        // truncated by draw_pl_row's fixed-width cut, which would hide a
        // trailing "[mm:ss]" entirely. Leading it keeps it always visible.
        std::string prefix = playing ? "\xe2\x96\xb6 " : "";
        if (!results[i].duration.empty()) prefix += "[" + results[i].duration + "] ";
        draw_pl_row(w, 3+(i-start), 1, inner_w, prefix + results[i].title, (i==sel));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// QUEUE panel
// ═══════════════════════════════════════════════════════════════════════════
static void draw_queue(WINDOW* w, Queue& q, bool focused) {
    int wh, ww; getmaxyx(w, wh, ww);
    werase(w);
    draw_box(w, g_cfg.border_chars);

    std::string lbl = std::string("QUEUE") +
                      (focused ? " \xe2\x80\x94 \xe2\x86\x91\xe2\x86\x93=nav  D=remove" : "") +
                      " (" + std::to_string(q.count()) + ")";
    border_label(w, lbl);

    int inner_w = ww - 2;
    int list_rows = wh - 2;

    if (q.empty()) {
        wattron(w, COLOR_PAIR(CP_STATUS));
        std::string msg = "Queue is empty  \xe2\x80\x94  press A to add songs";
        int cx = std::max(1, (ww - (int)msg.size()) / 2);
        mvwprintw(w, wh/2, cx, "%s", msg.c_str());
        wattroff(w, COLOR_PAIR(CP_STATUS));
        return;
    }

    q.clamp();
    int cur   = q.highlight;
    int start = std::max(0, cur - list_rows/2);
    int end   = std::min(q.count(), start + list_rows);
    if (end - start < list_rows) start = std::max(0, end - list_rows);

    const auto& entries = q.entries();
    for (int i = start; i < end; ++i) {
        bool active = (i == cur && focused);
        int  row    = 1 + (i - start);
        if (row >= wh - 1) break;

        if (active) wattron(w, COLOR_PAIR(CP_QUEUE_ACT) | A_BOLD);
        else        wattron(w, COLOR_PAIR(CP_QUEUE_ITEM));

        // Row number prefix
        char prefix[8]; snprintf(prefix, 8, "%2d ", i+1);
        int name_w = inner_w - (int)strlen(prefix) - 1;
        std::string name = utf8_truncate(entries[i].display_name, name_w);
        int pad = name_w - (int)utf8_display_width(name);
        mvwprintw(w, row, 1, "%s%s", prefix, name.c_str());
        for (int p = 0; p < pad; ++p) waddch(w, ' ');
        waddch(w, (i == 0) ? '\xe2' : ' ');   // ► on first (next-to-play)

        if (active) wattroff(w, COLOR_PAIR(CP_QUEUE_ACT) | A_BOLD);
        else        wattroff(w, COLOR_PAIR(CP_QUEUE_ITEM));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// HELP / controls panel
// ═══════════════════════════════════════════════════════════════════════════
static void draw_help(WINDOW* w) {
    int wh, ww; getmaxyx(w, wh, ww); (void)wh;
    werase(w);
    draw_box(w, g_cfg.border_chars);
    border_label(w, "CONTROLS");

    const KeyMap& k = g_cfg.keys;
    // Helper: format a key code as readable string
    auto key_str = [](int code) -> std::string {
        if (code == KEY_UP)        return "\xe2\x86\x91";
        if (code == KEY_DOWN)      return "\xe2\x86\x93";
        if (code == KEY_LEFT)      return "\xe2\x86\x90";
        if (code == KEY_RIGHT)     return "\xe2\x86\x92";
        if (code == '\n')          return "ENTER";
        if (code == '\t')          return "TAB";
        if (code == ' ')           return "SPACE";
        if (code == 27)            return "ESC";
        if (code >= 32 && code < 127) return std::string(1, (char)code);
        return "?";
    };

    struct Row { std::string key; std::string desc; };
    std::vector<Row> left_rows = {
        {key_str(k.nav_up)+"/"+key_str(k.nav_down), "navigate songs"},
        {key_str(k.play),            "play selected"},
        {key_str(k.search),          "search by name"},
        {key_str(k.setting),         "settings"},
        {key_str(k.next_song)+"/"+key_str(k.prev_song), "next / prev song"},
        {key_str(k.seek_fwd)+"/"+key_str(k.seek_bwd), "seek forward / back"},
        {key_str(k.vol_up)+"/"+key_str(k.vol_down), "volume up / down"},
        {key_str(k.queue_add),       "add to queue"},
        {key_str(k.tab_switch),      "switch panels"},
    };
    std::vector<Row> right_rows = {
        {key_str(k.toggle_repeat),   "toggle repeat"},
        {key_str(k.play_pause),      "play / pause"},
        {key_str(k.toggle_shuffle),  "toggle shuffle"},
        {key_str(k.folder_filter),   "filter by folder"},
        {key_str(k.clear_filter),    "clear filter"},
        {key_str(k.quit),            "quit"},
        {key_str(k.reset_prefs),     "reset preferences"},
        {key_str(k.queue_remove),    "remove from queue"},
        {key_str(k.jump_queue),      "jump to queue"},
        {key_str(k.download_stream), "download stream (yt:)"},
    };

    int cw = (ww - 2) / 2;
    int rows = std::min(std::max((int)left_rows.size(), (int)right_rows.size()), wh - 2);
    for (int i = 0; i < rows; ++i) {
        int r = 1 + i;
        if (r >= wh - 1) break;
        if (i < (int)left_rows.size()) {
            wattron(w, COLOR_PAIR(CP_STATUS_KEY) | A_BOLD);
            mvwprintw(w, r, 2, "%-7s", left_rows[i].key.c_str());
            wattroff(w, COLOR_PAIR(CP_STATUS_KEY) | A_BOLD);
            wattron(w, COLOR_PAIR(CP_STATUS_VAL));
            wprintw(w, "%-*s", cw - 10, left_rows[i].desc.c_str());
            wattroff(w, COLOR_PAIR(CP_STATUS_VAL));
        }

        wattron(w, COLOR_PAIR(CP_BORDER));
        mvwaddch(w, r, cw + 1, '|');
        wattroff(w, COLOR_PAIR(CP_BORDER));

        if (i < (int)right_rows.size()) {
            wattron(w, COLOR_PAIR(CP_STATUS_KEY) | A_BOLD);
            mvwprintw(w, r, cw + 3, "%-7s", right_rows[i].key.c_str());
            wattroff(w, COLOR_PAIR(CP_STATUS_KEY) | A_BOLD);
            wattron(w, COLOR_PAIR(CP_STATUS_VAL));
            wprintw(w, "%-*s", cw - 10, right_rows[i].desc.c_str());
            wattroff(w, COLOR_PAIR(CP_STATUS_VAL));
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// SETTINGS overlay — 6 tabs, redesigned
// ═══════════════════════════════════════════════════════════════════════════
struct SettingsState {
    int  tab     = 0;    // 0=Colors 1=Viz 2=Layout 3=Keybinds 4=Font 5=Themes
    int  row     = 0;
    int  col     = 0;    // 0=ANSI, 1=brightness for color fields
    bool editing = false;
    std::string buf;
};

static const int SET_TABS = 6;
static const char* SET_TAB_NAMES[SET_TABS] = {
    "1:Colors", "2:Viz", "3:Layout", "4:Keybinds", "5:Font", "6:Themes"
};

// Draw a section header inside the settings panel
static void set_section(WINDOW* w, int row, int ow, const char* title) {
    wattron(w, COLOR_PAIR(CP_SET_HDR) | A_BOLD);
    mvwprintw(w, row, 1, " %-*s", ow - 2, title);
    wattroff(w, COLOR_PAIR(CP_SET_HDR) | A_BOLD);
}

static bool draw_settings(WINDOW* parent, SettingsState& st, int key, bool rich) {
    // Persists to the internal session file always, and additionally
    // writes straight into the real config.txt when the user has opted
    // into that via the "Auto-save config.txt" toggle (Layout tab) — see
    // ConfigParser::write_back()'s doc comment for why this is a separate,
    // explicit opt-in rather than always-on.
    auto persist = [&] {
        g_cfg.save();
        if (g_cfg.auto_save_config && !g_cfg_path.empty())
            ConfigParser::write_back(g_cfg_path, g_cfg);
    };

    int pww, pwh; getmaxyx(parent, pwh, pww);
    const int OW = std::min(80, pww - 4);
    const int OH = std::min(36, pwh - 4);
    int oy = std::max(0, (pwh - OH) / 2);
    int ox = std::max(0, (pww - OW) / 2);

    WINDOW* w = newwin(OH, OW, oy, ox);
    keypad(w, TRUE);
    bool done = false;

    // ── Color fields (tab 0) ──────────────────────────────────────────────
    struct CField { const char* label; Color* c; };
    CField cfields[] = {
        {"Border / UI",      &g_cfg.colors.border},
        {"Title bar",        &g_cfg.colors.title},
        {"Meta key labels",  &g_cfg.colors.meta_key},
        {"Track name",       &g_cfg.colors.meta_val},
        {"Progress fill",    &g_cfg.colors.progress},
        {"Progress bg",      &g_cfg.colors.progress_bg},
        {"Status text",      &g_cfg.colors.status},
        {"Playlist text",    &g_cfg.colors.playlist},
        {"Active song (fg)", &g_cfg.colors.pl_active_fg},
        {"Active song (bg)", &g_cfg.colors.pl_active_bg},
        {"Viz bars",         &g_cfg.colors.viz},
        {"Viz left",         &g_cfg.colors.viz_left},
        {"Viz center",          &g_cfg.colors.viz_center},
        {"Viz right",       &g_cfg.colors.viz_right},
        {"Peak dots",        &g_cfg.colors.viz_peak},
        {"Lyrics (dim)",     &g_cfg.colors.lyr_dim},
        {"Lyrics (active)",  &g_cfg.colors.lyr_hi},
        {"Lyrics (word)",    &g_cfg.colors.lyr_word},
        {"Noise field",      &g_cfg.colors.noise},
        {"Queue item",       &g_cfg.colors.queue_item},
        {"Queue selected",   &g_cfg.colors.queue_active},
    };
    const int NCF = (int)(sizeof(cfields)/sizeof(cfields[0]));

    // ── Viz fields (tab 1) ────────────────────────────────────────────────
    int viz_int     = (int)g_cfg.viz_style;
    int bands_int   = g_cfg.viz_bands;
    int peak_int    = g_cfg.peak_hold ? 1 : 0;
    int density_int = g_cfg.viz_density;
    struct IField { const char* label; int* v; const char* hint; };
    IField vfields[] = {
        {"Viz style",    &viz_int,     "0=BARS  1=SCOPE"},
        {"Bands",        &bands_int,   "16 or 32"},
        {"Peak hold",    &peak_int,    "0=off  1=on"},
        {"Density",      &density_int, "1=fluid  10=dense"},
    };
    const int NVF = (int)(sizeof(vfields)/sizeof(vfields[0]));

    // ── Layout fields (tab 2) ─────────────────────────────────────────────
    int lmode_int    = (int)g_cfg.layout_mode;
    int autosave_int = g_cfg.auto_save_config ? 1 : 0;
    int results_int  = g_cfg.stream_search_results;
    IField lfields[] = {
        {"Layout mode",       &lmode_int,    "0=AUTO  1=DESKTOP  2=MOBILE"},
        {"Auto-save config.txt", &autosave_int, "0=off  1=on (writes every settings change to config.txt)"},
        {"YT search results", &results_int,  "5-50"},
    };
    const int NLF = (int)(sizeof(lfields)/sizeof(lfields[0]));

    // ── Keybind fields (tab 3) ────────────────────────────────────────────
    struct KField { const char* label; int* v; };
    KField kfields[] = {
        {"Settings",         &g_cfg.keys.setting},
        {"Nav up",           &g_cfg.keys.nav_up},
        {"Nav down",         &g_cfg.keys.nav_down},
        {"Play",             &g_cfg.keys.play},
        {"Search",           &g_cfg.keys.search},
        {"Next song",        &g_cfg.keys.next_song},
        {"Prev song",        &g_cfg.keys.prev_song},
        {"Seek forward",     &g_cfg.keys.seek_fwd},
        {"Seek backward",    &g_cfg.keys.seek_bwd},
        {"Volume up",        &g_cfg.keys.vol_up},
        {"Volume down",      &g_cfg.keys.vol_down},
        {"Queue add",        &g_cfg.keys.queue_add},
        {"Queue remove",     &g_cfg.keys.queue_remove},
        {"Download stream",  &g_cfg.keys.download_stream},
        {"Tab switch",       &g_cfg.keys.tab_switch},
        {"Toggle repeat",    &g_cfg.keys.toggle_repeat},
        {"Play/pause",       &g_cfg.keys.play_pause},
        {"Shuffle",          &g_cfg.keys.toggle_shuffle},
        {"Folder filter",    &g_cfg.keys.folder_filter},
        {"Clear filter",     &g_cfg.keys.clear_filter},
        {"Quit",             &g_cfg.keys.quit},
        {"Reset prefs",      &g_cfg.keys.reset_prefs},
    };
    const int NKF = (int)(sizeof(kfields)/sizeof(kfields[0]));

    // ── Get max rows for current tab ──────────────────────────────────────
    auto tab_max = [&]() -> int {
        switch (st.tab) {
            case 0: return NCF;
            case 1: return NVF;
            case 2: return NLF;
            case 3: return NKF;
            case 4: return (int)g_cfg.font_map.size() > 0 ? 26 : 26; // A-Z
            case 5: return THEME_COUNT;
            default: return 1;
        }
    };

    // ── Key handling ──────────────────────────────────────────────────────
    if (!st.editing) {
        if (key=='q'||key=='Q'||key==27||key==g_cfg.keys.setting) done = true;
        else if (key=='1') { st.tab=0; st.row=0; st.col=0; }
        else if (key=='2') { st.tab=1; st.row=0; st.col=0; }
        else if (key=='3') { st.tab=2; st.row=0; st.col=0; }
        else if (key=='4') { st.tab=3; st.row=0; st.col=0; }
        else if (key=='5') { st.tab=4; st.row=0; st.col=0; }
        else if (key=='6') { st.tab=5; st.row=0; st.col=0; }
        else if (key=='\t') { st.tab = (st.tab + 1) % SET_TABS; st.row = 0; st.col = 0; }
        else if (key==KEY_DOWN||key=='j') { int mx=tab_max(); st.row=(st.row+1)%mx; st.col=0; }
        else if (key==KEY_UP  ||key=='k') { int mx=tab_max(); st.row=(st.row-1+mx)%mx; st.col=0; }
        else if (key==KEY_RIGHT) { if(st.tab==0) st.col=std::min(1,st.col+1); }
        else if (key==KEY_LEFT)  { if(st.tab==0) st.col=std::max(0,st.col-1); }
        else if (key=='\n'||key==KEY_ENTER) {
            if (st.tab == 5) {
                // Apply theme instantly
                g_cfg.apply_theme(st.row);
                if (rich) apply_colors();
                persist();
            } else if (st.tab == 4) {
                // Font map: enter char to type new unicode
                char letter = 'A' + st.row;
                auto it = g_cfg.font_map.find(letter);
                st.buf = (it != g_cfg.font_map.end()) ? it->second : std::string(1, letter);
                st.editing = true;
            } else {
                st.editing = true;
                if (st.tab == 0) {
                    Color& c = *cfields[st.row].c;
                    st.buf = (st.col == 0) ? std::to_string(c.ansi) : std::to_string(c.brightness);
                } else if (st.tab == 1) {
                    st.buf = std::to_string(*vfields[st.row].v);
                } else if (st.tab == 2) {
                    st.buf = std::to_string(*lfields[st.row].v);
                } else if (st.tab == 3) {
                    // Keybind: show current printable / name
                    int code = *kfields[st.row].v;
                    if (code >= 32 && code < 127) st.buf = std::string(1, (char)code);
                    else if (code == KEY_UP)    st.buf = "ARROW_KEY_UP";
                    else if (code == KEY_DOWN)  st.buf = "ARROW_KEY_DOWN";
                    else if (code == KEY_LEFT)  st.buf = "ARROW_KEY_LEFT";
                    else if (code == KEY_RIGHT) st.buf = "ARROW_KEY_RIGHT";
                    else if (code == '\n')       st.buf = "ENTER";
                    else if (code == '\t')       st.buf = "TAB";
                    else                         st.buf = std::to_string(code);
                }
            }
        }
    } else {
        // Editing mode
        if (key=='\n'||key==KEY_ENTER) {
            if (st.tab == 0 && !st.buf.empty()) {
                try {
                    int v = std::stoi(st.buf);
                    Color& c = *cfields[st.row].c;
                    if (st.col == 0) c.ansi = std::clamp(v, 0, 255);
                    else             c.brightness = std::clamp(v, 0, 100);
                    if (rich) apply_colors();
                } catch (...) {}
            } else if (st.tab == 1 && !st.buf.empty()) {
                try {
                    int v = std::stoi(st.buf);
                    switch (st.row) {
                        case 0: g_cfg.viz_style = (VizStyle)std::clamp(v,0,1); break;
                        case 1: g_cfg.viz_bands = (v>=32)?32:16; break;
                        case 2: g_cfg.peak_hold = (v!=0); break;
                        case 3: g_cfg.viz_density = std::clamp(v,1,10); break;
                    }
                } catch (...) {}
            } else if (st.tab == 2 && !st.buf.empty()) {
                try {
                    int v = std::stoi(st.buf);
                    switch (st.row) {
                        case 0: g_cfg.layout_mode = (LayoutMode)std::clamp(v,0,2); break;
                        case 1: g_cfg.auto_save_config = (v != 0); break;
                        case 2: g_cfg.stream_search_results = std::clamp(v, 5, 50); break;
                    }
                } catch (...) {}
            } else if (st.tab == 3) {
                *kfields[st.row].v = KeyMap::parse_key(st.buf);
            } else if (st.tab == 4) {
                // Map the Nth letter to the typed unicode
                char letter = 'A' + st.row;
                if (!st.buf.empty()) {
                    g_cfg.font_map[letter] = st.buf;
                    g_cfg.font_map[(char)tolower((unsigned char)letter)] = st.buf;
                }
            }
            persist();
            st.editing = false; st.buf = "";
        } else if (key == 27) {
            st.editing = false; st.buf = "";
        } else if (key==KEY_BACKSPACE||key==127||key==8) {
            // Unicode-safe backspace: remove last codepoint
            if (!st.buf.empty()) {
                auto& s = st.buf;
                // Find last UTF-8 lead byte
                size_t i = s.size() - 1;
                while (i > 0 && (s[i] & 0xC0) == 0x80) --i;
                s = s.substr(0, i);
            }
        } else if (key >= 32) {
            // Accept any printable or multibyte input
            // For number tabs: digits and minus only
            if (st.tab == 0 || st.tab == 1 || st.tab == 2) {
                if ((key >= '0' && key <= '9') || (key == '-' && st.buf.empty()))
                    if (st.buf.size() < 4) st.buf += (char)key;
            } else if (st.tab == 3 || st.tab == 4) {
                // Accept any printable char (including unicode via multibyte)
                if (key < 128) st.buf += (char)key;
            }
        }
    }

    // ── Draw the panel ────────────────────────────────────────────────────
    werase(w);
    wattron(w, COLOR_PAIR(CP_SET_HDR) | A_BOLD);
    box(w, 0, 0);
    const char* hdr = " \xe2\x9c\xa6 CLI.MUSIC.COM  SETTINGS \xe2\x9c\xa6 ";
    mvwprintw(w, 0, std::max(0, (OW - (int)strlen(hdr)) / 2), "%s", hdr);
    wattroff(w, COLOR_PAIR(CP_SET_HDR) | A_BOLD);

    // Tab bar row 1
    int tx = 1;
    for (int t = 0; t < SET_TABS; ++t) {
        bool active = (t == st.tab);
        if (active) wattron(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
        else        wattron(w, COLOR_PAIR(CP_SET_TAB));
        mvwprintw(w, 1, tx, " %s ", SET_TAB_NAMES[t]);
        if (active) wattroff(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
        else        wattroff(w, COLOR_PAIR(CP_SET_TAB));
        tx += (int)strlen(SET_TAB_NAMES[t]) + 3;
    }

    // Separator row 2
    wattron(w, COLOR_PAIR(CP_SET_HDR));
    mvwhline(w, 2, 1, ACS_HLINE, OW - 2);
    wattroff(w, COLOR_PAIR(CP_SET_HDR));

    // Column headers row 3
    wattron(w, A_UNDERLINE | COLOR_PAIR(CP_SET_ITEM));
    switch (st.tab) {
        case 0:
            mvwprintw(w, 3, 2, "%-24s  %-6s  %-6s  %-8s", "ELEMENT", "ANSI", "BRITE%", "PREVIEW");
            break;
        case 1: case 2:
            mvwprintw(w, 3, 2, "%-24s  %-10s  %s", "PARAMETER", "VALUE", "HINT");
            break;
        case 3:
            mvwprintw(w, 3, 2, "%-20s  %-12s", "ACTION", "KEY");
            break;
        case 4:
            mvwprintw(w, 3, 2, "%-6s  %-12s  %s", "LETTER", "DISPLAY CHAR", "Enter to edit");
            break;
        case 5:
            mvwprintw(w, 3, 2, "%-4s  %-16s  %s", "#", "THEME", "DESCRIPTION");
            break;
    }
    wattroff(w, A_UNDERLINE | COLOR_PAIR(CP_SET_ITEM));

    const int VISIBLE = OH - 6;
    const int CONTENT_START = 4;

    // ── Tab content ───────────────────────────────────────────────────────
    if (st.tab == 0) {
        // Colors
        int scroll = std::max(0, st.row - VISIBLE + 2);
        for (int i = 0; i < NCF; ++i) {
            int dr = i - scroll;
            if (dr < 0 || dr >= VISIBLE) continue;
            int wr = CONTENT_START + dr;
            bool act = (i == st.row);
            if (act) wattron(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattron(w, COLOR_PAIR(CP_SET_ITEM));

            mvwprintw(w, wr, 2, "%-24s  ", cfields[i].label);
            Color& c = *cfields[i].c;

            // ANSI column
            bool ansi_sel = (act && st.col == 0);
            bool bri_sel  = (act && st.col == 1);
            if (ansi_sel && st.editing) wattron(w, A_UNDERLINE);
            std::string av = (ansi_sel && st.editing) ? st.buf + "_" : std::to_string(c.ansi);
            wprintw(w, "%-6s  ", av.c_str());
            if (ansi_sel && st.editing) wattroff(w, A_UNDERLINE);

            // Brightness column
            if (bri_sel && st.editing) wattron(w, A_UNDERLINE);
            std::string bv = (bri_sel && st.editing) ? st.buf + "_" : std::to_string(c.brightness) + "%";
            wprintw(w, "%-6s  ", bv.c_str());
            if (bri_sel && st.editing) wattroff(w, A_UNDERLINE);

            // Color swatch (block characters in ANSI color if supported)
            wprintw(w, "%.8s", "\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88");

            if (act) wattroff(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattroff(w, COLOR_PAIR(CP_SET_ITEM));
        }
    } else if (st.tab == 1) {
        // Viz settings
        for (int i = 0; i < NVF; ++i) {
            int wr = CONTENT_START + i;
            if (wr >= OH - 2) break;
            bool act = (i == st.row);
            if (act) wattron(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattron(w, COLOR_PAIR(CP_SET_ITEM));
            mvwprintw(w, wr, 2, "%-24s  ", vfields[i].label);
            std::string vs = (act && st.editing) ? st.buf + "_" : std::to_string(*vfields[i].v);
            if (i == 0 && !(act && st.editing))
                vs += "  (" + std::string(VIZ_STYLE_NAMES[std::clamp(*vfields[i].v,0,1)]) + ")";
            wprintw(w, "%-18s  %s", vs.c_str(), vfields[i].hint);
            if (act) wattroff(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattroff(w, COLOR_PAIR(CP_SET_ITEM));
        }
        // Density bar
        int br = CONTENT_START + NVF + 1;
        if (br < OH - 2) {
            wattron(w, COLOR_PAIR(CP_SET_ITEM));
            mvwprintw(w, br, 2, "Density: [");
            wattron(w, COLOR_PAIR(CP_VIZ) | A_BOLD);
            for (int d = 1; d <= 10; ++d)
                waddch(w, d <= g_cfg.viz_density ? '#' : '-');
            wattroff(w, COLOR_PAIR(CP_VIZ) | A_BOLD);
            wattron(w, COLOR_PAIR(CP_SET_ITEM));
            wprintw(w, "] %d/10", g_cfg.viz_density);
            wattroff(w, COLOR_PAIR(CP_SET_ITEM));
        }
    } else if (st.tab == 2) {
        // Layout
        for (int i = 0; i < NLF; ++i) {
            int wr = CONTENT_START + i;
            bool act = (i == st.row);
            if (act) wattron(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattron(w, COLOR_PAIR(CP_SET_ITEM));
            mvwprintw(w, wr, 2, "%-24s  ", lfields[i].label);
            std::string vs = (act && st.editing) ? st.buf + "_" : std::to_string(*lfields[i].v);
            wprintw(w, "%-6s  %s", vs.c_str(), lfields[i].hint);
            if (act) wattroff(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattroff(w, COLOR_PAIR(CP_SET_ITEM));
        }
        // Show current layout string truncated
        int lr = CONTENT_START + NLF + 1;
        if (lr < OH - 2) {
            wattron(w, COLOR_PAIR(CP_SET_ITEM));
            mvwprintw(w, lr, 2, "Desk layout (config.txt):");
            std::string dl = utf8_truncate(g_cfg.desk_layout, OW - 6);
            mvwprintw(w, lr + 1, 3, "%s", dl.c_str());
            mvwprintw(w, lr + 2, 2, "Mobile layout:");
            std::string ml = utf8_truncate(g_cfg.mobile_layout, OW - 6);
            mvwprintw(w, lr + 3, 3, "%s", ml.c_str());
            mvwprintw(w, lr + 5, 2, "\xe2\x84\xb9  Edit layout in config.txt for 100%% control.");
            wattroff(w, COLOR_PAIR(CP_SET_ITEM));
        }
    } else if (st.tab == 3) {
        // Keybinds
        int scroll = std::max(0, st.row - VISIBLE + 2);
        auto kstr = [](int code) -> std::string {
            if (code == KEY_UP)    return "↑";
            if (code == KEY_DOWN)  return "↓";
            if (code == KEY_LEFT)  return "←";
            if (code == KEY_RIGHT) return "→";
            if (code == '\n')      return "ENTER";
            if (code == '\t')      return "TAB";
            if (code == ' ')       return "SPACE";
            if (code == 27)        return "ESC";
            if (code >= 32 && code < 127) return std::string(1,(char)code);
            return "?";
        };
        for (int i = 0; i < NKF; ++i) {
            int dr = i - scroll;
            if (dr < 0 || dr >= VISIBLE) continue;
            int wr = CONTENT_START + dr;
            bool act = (i == st.row);
            if (act) wattron(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattron(w, COLOR_PAIR(CP_SET_ITEM));
            mvwprintw(w, wr, 2, "%-20s  ", kfields[i].label);
            std::string ks = (act && st.editing) ? st.buf + "_" : kstr(*kfields[i].v);
            wprintw(w, "%-12s", ks.c_str());
            if (act) wattroff(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattroff(w, COLOR_PAIR(CP_SET_ITEM));
        }
    } else if (st.tab == 4) {
        // Font map — show A-Z
        for (int i = 0; i < 26 && (CONTENT_START + i) < OH - 2; ++i) {
            int wr  = CONTENT_START + i;
            char uc = 'A' + i;
            bool act = (i == st.row);
            if (act) wattron(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattron(w, COLOR_PAIR(CP_SET_ITEM));
            auto it_u = g_cfg.font_map.find(uc);
            auto it_l = g_cfg.font_map.find((char)('a' + i));
            std::string uv = (it_u != g_cfg.font_map.end()) ? it_u->second : std::string(1, uc);
            std::string lv = (it_l != g_cfg.font_map.end()) ? it_l->second : std::string(1, (char)('a'+i));
            if (act && st.editing)
                mvwprintw(w, wr, 2, "  %c / %c   %s_", uc, (char)('a'+i), st.buf.c_str());
            else
                mvwprintw(w, wr, 2, "  %c / %c   %-14s  %-14s",
                    uc, (char)('a'+i), uv.c_str(), lv.c_str());
            if (act) wattroff(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattroff(w, COLOR_PAIR(CP_SET_ITEM));
        }
        wattron(w, COLOR_PAIR(CP_SET_ITEM));
        mvwprintw(w, OH - 3, 2,
            "\xe2\x84\xb9  For full font control edit font_en={} in config.txt");
        wattroff(w, COLOR_PAIR(CP_SET_ITEM));
    } else if (st.tab == 5) {
        // Themes
        static const char* descs[THEME_COUNT] = {
            "Blue/cyan palette  — default experience",
            "Deep ocean blues   — calm and focused",
            "Forest greens      — natural and earthy",
            "Warm sunset reds   — vibrant and energetic",
            "Midnight purples   — dark and mysterious",
            "Neon pink/green    — high-contrast hacker",
        };
        for (int i = 0; i < THEME_COUNT && (CONTENT_START + i) < OH - 2; ++i) {
            int wr  = CONTENT_START + i;
            bool act = (i == st.row);
            bool cur = (i == g_cfg.theme_idx);
            if (act) wattron(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattron(w, COLOR_PAIR(CP_SET_ITEM));
            mvwprintw(w, wr, 2, "%2d.  %-12s  %-40s %s",
                i + 1, THEMES[i].name, descs[i], cur ? "[ACTIVE]" : "");
            if (act) wattroff(w, COLOR_PAIR(CP_SET_SEL) | A_BOLD);
            else     wattroff(w, COLOR_PAIR(CP_SET_ITEM));
        }
    }

    // ── Footer ─────────────────────────────────────────────────────────────
    wattron(w, COLOR_PAIR(CP_SET_HDR));
    mvwprintw(w, OH - 1, 2,
        "Tab/1-6=switch  ↑↓=nav  ←→=field  Enter=edit  S/Esc=close");
    wattroff(w, COLOR_PAIR(CP_SET_HDR));

    wrefresh(w);
    if (done) { werase(w); wrefresh(w); }
    delwin(w);
    return !done;
}

// ═══════════════════════════════════════════════════════════════════════════
// Dynamic window manager using layout engine
// ═══════════════════════════════════════════════════════════════════════════
struct LayoutWindows {
    struct Entry {
        BlockId   id;
        WINDOW*   win = nullptr;
        PanelRect rect;
    };
    std::vector<Entry> entries;
    int last_cols = 0, last_rows = 0;
    bool last_desktop = false;

    void build(int cols, int rows, bool desktop) {
        destroy();
        last_cols = cols; last_rows = rows; last_desktop = desktop;

        // Select layout + row heights based on mode
        const std::string& layout_str = desktop
            ? g_cfg.desk_layout : g_cfg.mobile_layout;
        const std::vector<int>& row_h = desktop
            ? g_cfg.desk_row_h : g_cfg.mobile_row_h;

        // Header is always 3 rows at the top
        static constexpr int HDR_H = 3;
        int content_h = rows - HDR_H - 1; // -1 for footer
        (void)content_h;

        auto row_specs = parse_layout(layout_str, row_h);
        auto lr = compute_layout(row_specs, cols, rows - HDR_H);

        // Header entry (always present, y=0)
        {
            Entry e;
            e.id   = BlockId::NONE;  // special: header
            e.rect = {BlockId::NONE, 0, 0, cols, HDR_H, true};
            e.win  = newwin(HDR_H, cols, 0, 0);
            if (e.win) keypad(e.win, TRUE);
            entries.push_back(e);
        }

        // Content panels
        for (auto& pr : lr.panels) {
            Entry e;
            e.id   = pr.id;
            e.rect = pr;
            e.rect.y += HDR_H;   // shift down by header height
            e.win = newwin(std::max(2, e.rect.h),
                            std::max(4, e.rect.w),
                            e.rect.y, e.rect.x);
            if (e.win) keypad(e.win, TRUE);
            entries.push_back(e);
        }
    }

    WINDOW* get(BlockId id) const {
        for (auto& e : entries) if (e.id == id) return e.win;
        return nullptr;
    }
    WINDOW* header() const { return entries.empty() ? nullptr : entries[0].win; }

    bool has(BlockId id) const {
        for (auto& e : entries) if (e.id == id && e.win) return true;
        return false;
    }

    bool needs_rebuild(int cols, int rows, bool desktop) const {
        return cols != last_cols || rows != last_rows || desktop != last_desktop;
    }

    void destroy() {
        for (auto& e : entries) if (e.win) { delwin(e.win); e.win = nullptr; }
        entries.clear();
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// main()
// ═══════════════════════════════════════════════════════════════════════════
int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    setlocale(LC_ALL, "");

    // ── Load settings ────────────────────────────────────────────────────
    g_cfg.load();   // load saved runtime settings first
    std::string cfg_path = ConfigParser::find_config();
    if (!cfg_path.empty()) ConfigParser::parse(cfg_path, g_cfg);  // overlay config.txt
    g_cfg_path = cfg_path;

    // ── Init ncurses ──────────────────────────────────────────────────────
    initscr(); cbreak(); noecho();
    keypad(stdscr, TRUE); curs_set(0);
    nodelay(stdscr, TRUE); set_escdelay(50);
    start_color(); use_default_colors();
    bool rich = can_change_color();
    if (rich) apply_colors(); else apply_basic_colors();

    // ── Init player ───────────────────────────────────────────────────────
    Player player;
    if (!player.init()) {
        endwin();
        fprintf(stderr, "Player init failed: %s\n", player.status_msg.c_str());
        return 1;
    }

    Lyrics   lyrics;
    NoiseGen noise;

    // ── Streaming state ──────────────────────────────────────────────────
    // v2: exactly one network connection per stream — see stream.h's top
    // comment. StreamPlayer tees the single yt-dlp fetch to a disk cache
    // (StreamCache) + a FIFO that mpv/ffplay actually play from;
    // StreamVizFeed gets visualizer PCM by tailing that same cache file
    // rather than fetching anything itself.
    StreamSearch stream_search;
    StreamPlayer stream_player;
    stream_player.init();
    StreamVizFeed stream_viz_feed;
    bool         stream_browse   = false;   // browsing yt: search results
    int          stream_sel      = 0;
    bool         is_streaming    = false;   // currently playing a stream
    StreamResult current_stream;
    int          stream_volume   = 80;

    // v3: for mpv, StreamPlayer itself now delays its cache-only fetch by
    // ~3s (see spawn_pipeline()'s comment) before the cache file even
    // exists — the visualizer tap needs to wait at least that long too, or
    // `tail -f` on a not-yet-existing file just fails immediately. The
    // lyrics fetch is a real *second* YouTube connection when it falls
    // back to yt-dlp subtitles, so it keeps an even longer, separate head
    // start to avoid colliding with whichever connection(s) are already
    // establishing.
    static constexpr double kVizFeedDelaySec    = 3.5;
    static constexpr double kLyricsFetchDelaySec = 6.0;
    bool            viz_pending    = false;
    bool            lyrics_pending = false;
    clk::time_point viz_at{};
    clk::time_point lyrics_at{};

    std::string      header_flash;
    clk::time_point   header_flash_until{};
    auto flash = [&](const std::string& msg, double secs = 2.5) {
        header_flash = msg;
        header_flash_until = clk::now() + std::chrono::duration_cast<clk::duration>(
            std::chrono::duration<double>(secs));
    };

    auto sanitize_for_path = [](std::string s) {
        for (char& c : s) if (c=='/'||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|') c = '_';
        if (s.size() > 120) s.resize(120);
        return s;
    };

    auto stop_stream = [&] {
        if (is_streaming) { stream_player.stop(); stream_viz_feed.stop(); is_streaming = false; }
        viz_pending    = false;
        lyrics_pending = false;
    };

    auto load_track = [&](const std::string& path) {
        stop_stream();
        player.load_and_play(path);
        const auto& m = player.current_meta();
        lyrics.load(path, m.title, m.artist);
        noise.phase = 0.0;
    };

    auto play_stream = [&](const StreamResult& r) {
        stop_stream();
        player.stop();

        // Already fully downloaded from a previous play? Just play it as
        // an ordinary local file — real seeking, full visualizer, zero
        // network. This is also how a cached track keeps working forever
        // after StreamCache::dir() gets pruned/cleared by hand.
        std::string cached = StreamCache::path_for(r.url, r.title);
        if (StreamCache::is_complete(cached)) {
            load_track(cached);
            flash("Playing from cache \xe2\x80\x94 \xe2\x9c\x93 " + r.title, 2.0);
            return;
        }

        if (!stream_player.play(r.url, r.title)) {
            flash("Playback failed \xe2\x80\x94 install mpv or ffplay");
            return;
        }
        stream_player.set_volume(stream_volume);
        is_streaming    = true;
        current_stream  = r;
        noise.phase     = 0.0;

        viz_pending  = true;
        viz_at       = clk::now() + std::chrono::duration_cast<clk::duration>(
            std::chrono::duration<double>(kVizFeedDelaySec));
        lyrics_pending = true;
        lyrics_at      = clk::now() + std::chrono::duration_cast<clk::duration>(
            std::chrono::duration<double>(kLyricsFetchDelaySec));
    };

    // Queue entries are normally local files, but a yt: result can be
    // queued too — encoded as a PlaylistEntry whose path is prefixed with
    // "ytstream://" (stripped back off, with the url recovered, when the
    // queue is drained). This keeps Queue/PlaylistEntry generic rather
    // than teaching them about streaming specifically.
    static constexpr const char* kYtQueuePrefix = "ytstream://";
    auto make_stream_queue_entry = [](const StreamResult& r) {
        PlaylistEntry e;
        e.path         = std::string(kYtQueuePrefix) + r.url;
        e.display_name = r.title;
        e.folder_name  = "YouTube";
        return e;
    };
    auto play_queue_entry = [&](const PlaylistEntry& entry) {
        if (entry.path.rfind(kYtQueuePrefix, 0) == 0) {
            play_stream(StreamResult{entry.display_name, entry.path.substr(std::strlen(kYtQueuePrefix))});
        } else {
            load_track(entry.path);
        }
    };

    if (player.playlist().count() > 0)
        if (auto* e = player.playlist().current()) load_track(e->path);

    // Previously-completed stream downloads reappear in the local library
    // across restarts too (import() is additive, unlike load_dir()).
    player.playlist().import(StreamCache::dir());

    // ── Build windows ─────────────────────────────────────────────────────
    int COLS_ = 0, ROWS_ = 0; getmaxyx(stdscr, ROWS_, COLS_);
    auto is_desktop = [&]() -> bool {
        if (g_cfg.layout_mode == LayoutMode::DESKTOP) return true;
        if (g_cfg.layout_mode == LayoutMode::MOBILE)  return false;
        return COLS_ >= DESKTOP_WIDTH_THRESHOLD;
    };

    LayoutWindows wins;
    wins.build(COLS_, ROWS_, is_desktop());

    // ── State ─────────────────────────────────────────────────────────────
    bool settings_open = false;
    bool search_active = false;
    bool queue_focused = false;
    bool quit          = false;

    SettingsState set_st;
    SearchState   srch;

    std::string text_input, text_input_label;
    enum class InputMode { NONE, FOLDER_FILTER } input_mode = InputMode::NONE;

    auto last_frame = clk::now();

    // ═══════════════════════════════════════════════════════════════════════
    // Main loop
    // ═══════════════════════════════════════════════════════════════════════
    while (!quit) {
        // ── Resize check ──────────────────────────────────────────────────
        {
            int nc, nr; getmaxyx(stdscr, nr, nc);
            if (wins.needs_rebuild(nc, nr, is_desktop())) {
                COLS_ = nc; ROWS_ = nr;
                wins.build(COLS_, ROWS_, is_desktop());
                clear(); refresh();
            }
        }

        // ── Timing ────────────────────────────────────────────────────────
        auto now = clk::now();
        double dt = std::chrono::duration<double>(now - last_frame).count();
        last_frame = now;
        noise.advance(dt);

        // ── Streaming: detect the stream ending, failing, or resolving ─────
        if (is_streaming && (stream_player.is_ended() || stream_player.is_failed())) {
            if (stream_player.is_failed())
                flash("Stream failed: " + stream_player.last_error(), 6.0);
            else
                flash("Stream ended");
            is_streaming   = false;
            viz_pending    = false;
            lyrics_pending = false;
            stream_player.stop();
            stream_viz_feed.stop();
        }

        // A track finished downloading (StreamPlayer's fetch pipeline
        // exited cleanly) — register it into the local library so it's
        // playable/seekable/visualizable as a normal file from now on.
        if (auto done = stream_player.take_completed_download(); !done.empty())
            player.playlist().add(done);

        // ── Fire the deferred visualizer PCM tap (tails the cache file —
        // no network) once it's had a moment to exist, and the lyrics
        // fetch (a real second YouTube connection when it falls back to
        // yt-dlp subtitles) once the primary stream has had a longer,
        // uncontested head start. Staggered from each other too, so we
        // never open two network connections to the same video at once.
        if (viz_pending && is_streaming && clk::now() >= viz_at) {
            viz_pending = false;
            if (g_cfg.stream_visualizer)
                stream_viz_feed.start(stream_player.cache_path(), player.visualizer(), player.vocal_viz(), stream_player);
        }
        if (lyrics_pending && is_streaming && clk::now() >= lyrics_at) {
            lyrics_pending = false;
            std::string cache_key = (fs::temp_directory_path() /
                (sanitize_for_path(current_stream.title) + ".stream")).string();
            lyrics.load(cache_key, current_stream.title, "");
        }

        // ── Player state icon ─────────────────────────────────────────────
        std::string icon;
        if (is_streaming) {
            icon = stream_player.is_resolving() ? "\xe2\x8f\xb3"                        // ⏳ resolving
                 : stream_player.is_paused()    ? "\xe2\x8f\xb8" : "\xe2\x96\xb6";
        } else switch (player.state()) {
            case PlayerState::PLAYING: icon = "\xe2\x96\xb6"; break;  // ▶
            case PlayerState::PAUSED:  icon = "\xe2\x8f\xb8"; break;  // ⏸
            case PlayerState::STOPPED: icon = "\xe2\x8f\xb9"; break;  // ⏹
        }

        double cur_pos = is_streaming ? stream_player.position() : player.position();
        double cur_dur = is_streaming ? stream_player.duration() : player.duration();
        int    cur_vol = is_streaming ? stream_volume             : player.volume();

        TrackMeta stream_meta;
        if (is_streaming) {
            stream_meta.title    = current_stream.title;
            stream_meta.artist   = stream_player.is_resolving() ? "Resolving stream\xe2\x80\xa6" : "YouTube stream";
            stream_meta.album    = current_stream.url;
            stream_meta.format   = (stream_player.backend() == StreamBackend::MPV) ? "stream (mpv)" : "stream (ffplay)";
            stream_meta.duration_sec = (int)cur_dur;
        }
        const TrackMeta& meta_view = is_streaming ? stream_meta : player.current_meta();

        std::string header_title;
        if (clk::now() < header_flash_until) header_title = header_flash;
        else if (is_streaming) header_title = stream_player.is_resolving()
            ? ("\xe2\x8f\xb3 Resolving: " + current_stream.title)
            : ("\xe2\x97\x89 STREAMING: " + current_stream.title);

        // ── Draw all panels (skip if settings open) ───────────────────────
        if (!settings_open) {
            // Header
            if (auto* h = wins.header())
                draw_header(h, COLS_, icon,
                    player.is_repeat(), player.is_shuffle(), cur_vol, header_title);

            // Each layout panel
            if (auto* w = wins.get(BlockId::META))
                draw_meta(w, meta_view);

            if (auto* w = wins.get(BlockId::COVER))
                draw_cover(w, meta_view);

            if (auto* w = wins.get(BlockId::LYRICS))
                draw_lyrics(w, lyrics, cur_pos, noise);

            bool viz_live = is_streaming && g_cfg.stream_visualizer && StreamVizFeed::available();

            if (auto* w = wins.get(BlockId::VOCAL_VIZ)) {
                if (is_streaming && !viz_live) draw_stream_viz_placeholder(w, "VOCAL VISUALIZER");
                else                           draw_vocal_viz(w, player.vocal_viz());
            }

            if (auto* w = wins.get(BlockId::VIZ)) {
                if (is_streaming && !viz_live) draw_stream_viz_placeholder(w, "VISUALIZER");
                else                           draw_viz(w, player);
            }

            if (auto* w = wins.get(BlockId::PROGRESS_BAR))
                draw_progress(w, cur_pos, cur_dur);

            if (auto* w = wins.get(BlockId::LIST)) {
                if (stream_browse) draw_stream_results(w, stream_search, stream_sel, is_streaming, current_stream.url);
                else draw_list(w, player.playlist(), search_active, srch, ERR);
            }

            if (auto* w = wins.get(BlockId::QUEUE))
                draw_queue(w, player.queue(), queue_focused);

            if (auto* w = wins.get(BlockId::HELP))
                draw_help(w);

            // Flush all
            for (auto& e : wins.entries)
                if (e.win) wrefresh(e.win);
        }

        // ── Input ─────────────────────────────────────────────────────────
        int ch = getch();

        if (settings_open) {
            settings_open = draw_settings(stdscr, set_st, ch, rich);
            if (!settings_open) {
                if (rich) apply_colors();
                clear(); refresh();
                wins.build(COLS_, ROWS_, is_desktop());
            }
            goto frame_end;
        }

        // Queue focus: handle navigation inside queue panel
        if (queue_focused) {
            if (ch == KEY_UP   || ch == g_cfg.keys.nav_up)   { player.queue().nav_up();   goto frame_end; }
            if (ch == KEY_DOWN || ch == g_cfg.keys.nav_down)  { player.queue().nav_down(); goto frame_end; }
            if (ch == g_cfg.keys.queue_remove || ch == 'd' || ch == 'D') {
                player.queue().remove(player.queue().highlight);
                player.queue().clamp();
                goto frame_end;
            }
            if (ch == '\t' || ch == g_cfg.keys.tab_switch || ch == 27) {
                queue_focused = false; goto frame_end;
            }
            goto frame_end; // absorb other keys
        }

        // Stream results browse mode (after a "yt:<query>" search)
        if (stream_browse) {
            if (ch != ERR) {
                if (ch == 27 || ch == g_cfg.keys.search) {
                    stream_browse = false;
                    stream_search.cancel();
                } else if (ch == KEY_UP) {
                    if (stream_sel > 0) --stream_sel;
                } else if (ch == KEY_DOWN) {
                    int n = (int)stream_search.results().size();
                    if (stream_sel < n - 1) ++stream_sel;
                } else if (ch == '\n' || ch == KEY_ENTER) {
                    auto results = stream_search.results();
                    if (stream_sel >= 0 && stream_sel < (int)results.size())
                        play_stream(results[stream_sel]);
                    // Stay in the results list — don't kick back to local
                    // music on every play. Only ESC (or the search key
                    // again, handled above) returns to the local list.
                } else if (ch == g_cfg.keys.queue_add) {
                    auto results = stream_search.results();
                    if (stream_sel >= 0 && stream_sel < (int)results.size()) {
                        player.queue().push(make_stream_queue_entry(results[stream_sel]));
                        flash("Queued: " + results[stream_sel].title, 1.5);
                    }
                }
                // BUGFIX (pause/seek "not working in the YT window"): this
                // block used to swallow *every* key while browsing results
                // (unconditional goto below), so transport-control keys
                // never reached the dispatch that actually handles them —
                // they only worked once you left this list. Handle them
                // here too, mirroring the general dispatch further down.
                else if (ch == g_cfg.keys.play_pause || ch == ' ') {
                    if (is_streaming) stream_player.toggle_pause();
                } else if (ch == g_cfg.keys.seek_fwd) {
                    if (is_streaming) stream_player.seek_relative(+10.0);
                } else if (ch == g_cfg.keys.seek_bwd) {
                    if (is_streaming) stream_player.seek_relative(-10.0);
                } else if (ch == g_cfg.keys.vol_up) {
                    if (is_streaming) { stream_volume = std::min(100, stream_volume+5); stream_player.set_volume(stream_volume); }
                } else if (ch == g_cfg.keys.vol_down) {
                    if (is_streaming) { stream_volume = std::max(0, stream_volume-5); stream_player.set_volume(stream_volume); }
                } else if (ch == g_cfg.keys.download_stream) {
                    if (is_streaming) {
                        if (StreamDownloader::download(current_stream.url, Playlist::MUSIC_DIR))
                            flash("Downloading \xe2\x80\x9c" + current_stream.title + "\xe2\x80\x9d\xe2\x80\xa6");
                        else
                            flash("yt-dlp not found \xe2\x80\x94 can't download");
                    }
                }
            }
            goto frame_end;
        }

        // Search mode (local filename search; "yt:<query>" + Enter switches
        // into an online search instead of matching local files)
        if (search_active) {
            bool is_yt_query = srch.query.size() > 3 &&
                (srch.query[0]=='y'||srch.query[0]=='Y') &&
                (srch.query[1]=='t'||srch.query[1]=='T') &&
                srch.query[2]==':';

            if (is_yt_query && (ch == '\n' || ch == KEY_ENTER)) {
                std::string q = srch.query.substr(3);
                size_t s = q.find_first_not_of(' ');
                q = (s == std::string::npos) ? "" : q.substr(s);
                search_active = false; srch = {};
                if (!q.empty()) {
                    stream_search.search(q, g_cfg.stream_search_results);
                    stream_browse = true;
                    stream_sel    = 0;
                }
            } else if (ch != ERR && wins.has(BlockId::LIST)) {
                int res = draw_list(wins.get(BlockId::LIST),
                                    player.playlist(), search_active, srch, ch);
                if (wins.get(BlockId::LIST)) wrefresh(wins.get(BlockId::LIST));
                if (res == -2) { search_active = false; srch = {}; }
                else if (res >= 0) {
                    search_active = false;
                    player.playlist().select(res);
                    if (auto* e = player.playlist().current()) load_track(e->path);
                    srch = {};
                }
            }
            goto frame_end;
        }

        if (ch == ERR) goto frame_end;

        // Text input mode (folder filter)
        if (input_mode != InputMode::NONE) {
            if (ch == '\n' || ch == KEY_ENTER) {
                if (input_mode == InputMode::FOLDER_FILTER) {
                    if (text_input.empty()) player.playlist().clear_folder_filter();
                    else player.playlist().set_folder_filter(text_input);
                }
                text_input.clear(); input_mode = InputMode::NONE;
            } else if (ch == 27) {
                text_input.clear(); input_mode = InputMode::NONE;
            } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                if (!text_input.empty()) text_input.pop_back();
            } else if (ch >= 32 && ch < 127) {
                text_input += (char)ch;
            }
            // Show input in the help/footer area if available
            if (auto* hw = wins.get(BlockId::HELP)) {
                werase(hw); draw_box(hw, g_cfg.border_chars);
                wattron(hw, COLOR_PAIR(CP_SEARCH_BAR) | A_BOLD);
                int hwh, hww; getmaxyx(hw, hwh, hww); (void)hwh;
                mvwprintw(hw, 1, 2, "%s%s_",
                    text_input_label.c_str(),
                    text_input.substr(0, std::max(0, hww - (int)text_input_label.size() - 4)).c_str());
                wattroff(hw, COLOR_PAIR(CP_SEARCH_BAR) | A_BOLD);
                wrefresh(hw);
            }
            goto frame_end;
        }

        // ── Key dispatch ──────────────────────────────────────────────────
        {
            const KeyMap& k = g_cfg.keys;

            if (ch == k.quit)           { quit = true; }
            else if (ch == k.play_pause || ch == ' ') {
                if (is_streaming) stream_player.toggle_pause();
                else              player.play_pause();
            }
            else if (ch == k.play || ch == '\n' || ch == KEY_ENTER) {
                if (auto* e = player.playlist().current()) load_track(e->path);
            }
            else if (ch == k.next_song) {
                stop_stream();
                // Check queue first
                if (!player.queue().empty()) {
                    auto entry = player.queue().pop_front();
                    play_queue_entry(entry);
                } else {
                    player.next();
                    if (auto* e = player.playlist().current()) load_track(e->path);
                }
            }
            else if (ch == k.prev_song) {
                stop_stream();
                player.prev();
                if (auto* e = player.playlist().current()) load_track(e->path);
            }
            else if (ch == k.toggle_repeat)  { player.toggle_repeat(); }
            else if (ch == k.toggle_shuffle) { player.toggle_shuffle(); }
            else if (ch == k.vol_up) {
                if (is_streaming) { stream_volume = std::min(100, stream_volume+5); stream_player.set_volume(stream_volume); }
                else                player.set_volume(+1);
            }
            else if (ch == k.vol_down) {
                if (is_streaming) { stream_volume = std::max(0, stream_volume-5); stream_player.set_volume(stream_volume); }
                else                player.set_volume(-1);
            }
            else if (ch == k.seek_fwd) {
                if (is_streaming) stream_player.seek_relative(+10.0);
                else              player.seek(player.position() + 10.0);
            }
            else if (ch == k.seek_bwd) {
                if (is_streaming) stream_player.seek_relative(-10.0);
                else              player.seek(std::max(0.0, player.position() - 10.0));
            }
            else if (ch == k.download_stream) {
                if (is_streaming) {
                    if (StreamDownloader::download(current_stream.url, Playlist::MUSIC_DIR))
                        flash("Downloading \xe2\x80\x9c" + current_stream.title + "\xe2\x80\x9d\xe2\x80\xa6");
                    else
                        flash("yt-dlp not found \xe2\x80\x94 can't download");
                } else {
                    flash("Nothing streaming to download");
                }
            }
            else if (ch == k.nav_down)       { player.playlist().select(player.playlist().current_idx()+1); }
            else if (ch == k.nav_up)         { player.playlist().select(player.playlist().current_idx()-1); }
            else if (ch == k.search) {
                search_active = true; srch = SearchState{};
                if (auto* w = wins.get(BlockId::LIST))
                    { draw_list(w, player.playlist(), search_active, srch, ERR); wrefresh(w); }
            }
            else if (ch == k.folder_filter) {
                input_mode = InputMode::FOLDER_FILTER;
                text_input_label = "Folder filter (empty=clear): ";
                text_input.clear();
            }
            else if (ch == k.clear_filter)  { player.playlist().clear_folder_filter(); }
            else if (ch == k.setting)       { settings_open = true; set_st = {}; }
            else if (ch == k.reset_prefs) {
                g_cfg = Settings{};
                if (!cfg_path.empty()) ConfigParser::parse(cfg_path, g_cfg);
                g_cfg.save();
                if (rich) apply_colors();
                wins.build(COLS_, ROWS_, is_desktop());
            }
            // Queue: add currently highlighted playlist entry
            else if (ch == k.queue_add || ch == 'a' || ch == 'A') {
                if (auto* e = player.playlist().current())
                    player.queue().push(*e);
            }
            // Queue: jump focus to queue panel
            else if (ch == 'Q' || ch == k.jump_queue) {
                if (wins.has(BlockId::QUEUE)) queue_focused = true;
            }
            // Tab: cycle focus (playlist <-> queue)
            else if (ch == k.tab_switch) {
                queue_focused = wins.has(BlockId::QUEUE) && !queue_focused;
            }
        }

        frame_end:
        // ── Auto-advance ──────────────────────────────────────────────────
        if (player.state() == PlayerState::PLAYING
            && player.duration() > 0.5
            && player.position() >= player.duration() - 0.5)
>>>>>>> origin/master
        {
            if (player.is_repeat()) {
                player.seek(0);
            } else if (!player.queue().empty()) {
                // Drain queue first
                auto entry = player.queue().pop_front();
                play_queue_entry(entry);
            } else {
                player.next();
                if (auto* e = player.playlist().current()) load_track(e->path);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    lyrics.cancel_fetch();
    stream_search.cancel();
    stream_player.stop();
    stream_viz_feed.stop();
    player.shutdown();
    wins.destroy();
    endwin();
    return 0;
}
