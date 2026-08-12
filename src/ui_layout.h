#pragma once
/*  CLI.MUSIC.COM — ui_layout.h  v3.0
 *
 *  Declarative layout engine — Termux-style DSL.
 *
 *  ── Grammar ──────────────────────────────────────────────────────────────
 *
 *   layout  := row  (',' '\' row)*
 *   row     := cell+
 *   cell    := '[' spec ']'
 *   spec    := name  |  name '{' mods '}'  |  (empty = spacer)
 *   name    := META | COVER | LYRICS | VOCAL-VIZ | VIZ |
 *              PROGRESS-BAR | LIST | QUEUE | HELP |
 *              LEFT | RIGHT | UP | DOWN
 *   mods    := side ',' w_delta ',' h_val
 *   w_delta := [+-] integer      (extra terminal columns on that side)
 *   h_val   := integer           (absolute row height; sign prefix ignored)
 *
 *  ── Direction semantics ───────────────────────────────────────────────────
 *
 *   [LEFT]   — the block immediately to the LEFT in the same row expands
 *              rightward to fill this cell's space.
 *   [RIGHT]  — the block immediately to the RIGHT in the same row expands
 *              leftward.
 *   [UP]     — whichever block in the PREVIOUS row overlaps this cell's
 *              X-midpoint expands downward into this cell.
 *   [DOWN]   — same, but looks at the NEXT row.
 *
 *  ── Column sizing ─────────────────────────────────────────────────────────
 *
 *   Each row divides the terminal width equally among its cells
 *   (direction/empty cells still occupy space for column-count purposes).
 *   An optional w_delta ±n adjusts a cell's width (steals from / gives to
 *   its right neighbour).  Y-axis sign is stripped; only the magnitude is
 *   used as an absolute row height override.
 *
 *  ── Block exclusion ───────────────────────────────────────────────────────
 *
 *   Any block NOT referenced in the layout is simply not rendered.
 *   The `enabled` set returned by compute_layout() lists active blocks.
 */

#include <string>
#include <vector>
#include <map>
#include <set>
#include <sstream>
#include <algorithm>
#include <cctype>

// ─── Block identifiers ────────────────────────────────────────────────────────
enum class BlockId : int {
    NONE         = 0,
    META,           // metadata panel
    COVER,          // cover art / icon
    LYRICS,         // synced lyrics
    VOCAL_VIZ,      // stereo vocal visualiser
    VIZ,            // frequency / scope visualiser
    PROGRESS_BAR,   // progress + time
    LIST,           // playlist
    QUEUE,          // play queue
    HELP,           // controls / help
    EMPTY,          // [] spacer
    DIR_LEFT,
    DIR_RIGHT,
    DIR_UP,
    DIR_DOWN
};

static inline BlockId block_id_from_name(const std::string& raw) {
    std::string u;
    for (unsigned char c : raw) u += (char)toupper(c);
    // trim
    auto s = u.find_first_not_of(" \t\r\n");
    auto e = u.find_last_not_of(" \t\r\n");
    if (s == std::string::npos) return BlockId::EMPTY;
    u = u.substr(s, e - s + 1);

    if (u == "META"         || u == "METADATA")     return BlockId::META;
    if (u == "COVER"        || u == "ICON")          return BlockId::COVER;
    if (u == "LYRICS")                               return BlockId::LYRICS;
    if (u == "VOCAL-VIZ"    || u == "VOCAL_VIZ"
        || u == "VOCALVISUALIZER")                   return BlockId::VOCAL_VIZ;
    if (u == "VIZ"          || u == "VISUALISER"
        || u == "VISUALIZER")                        return BlockId::VIZ;
    if (u == "PROGRESS-BAR" || u == "PROGRESSBAR"
        || u == "PROGRESS_BAR")                      return BlockId::PROGRESS_BAR;
    if (u == "LIST"         || u == "PLAYLIST")      return BlockId::LIST;
    if (u == "QUEUE")                                return BlockId::QUEUE;
    if (u == "HELP"         || u == "CONTROLS")      return BlockId::HELP;
    if (u == "LEFT")                                 return BlockId::DIR_LEFT;
    if (u == "RIGHT")                                return BlockId::DIR_RIGHT;
    if (u == "UP")                                   return BlockId::DIR_UP;
    if (u == "DOWN")                                 return BlockId::DIR_DOWN;
    return BlockId::EMPTY;
}

static inline const char* block_id_name(BlockId id) {
    switch (id) {
        case BlockId::META:         return "META";
        case BlockId::COVER:        return "COVER";
        case BlockId::LYRICS:       return "LYRICS";
        case BlockId::VOCAL_VIZ:    return "VOCAL-VIZ";
        case BlockId::VIZ:          return "VIZ";
        case BlockId::PROGRESS_BAR: return "PROGRESS-BAR";
        case BlockId::LIST:         return "LIST";
        case BlockId::QUEUE:        return "QUEUE";
        case BlockId::HELP:         return "HELP";
        default:                    return "";
    }
}

// ─── Cell spec ────────────────────────────────────────────────────────────────
struct CellSpec {
    BlockId id      = BlockId::EMPTY;
    int     w_delta = 0;  // ±n extra columns
    int     h_abs   = 0;  // absolute height override (0 = use row default)
};

struct RowSpec {
    std::vector<CellSpec> cells;
    int row_h = 3;
};

// ─── Computed panel geometry ─────────────────────────────────────────────────
struct PanelRect {
    BlockId id    = BlockId::NONE;
    int x = 0, y = 0, w = 0, h = 0;
    bool valid = false;
};

// ─── Parse layout DSL string ─────────────────────────────────────────────────
/*
 *  layout_str  — the inner content from DESK_MODE={...}
 *  row_heights — parallel array; row_heights[0] = first row height, etc.
 */
inline std::vector<RowSpec> parse_layout(const std::string& layout_str,
                                          const std::vector<int>& row_heights)
{
    // ── Step 1: split into row strings on ',' ────────────────────────────
    std::vector<std::string> raw_rows;
    {
        std::string cur;
        for (char c : layout_str) {
            if (c == ',') {
                raw_rows.push_back(cur);
                cur.clear();
            } else if (c == '\\' || c == '\n' || c == '\r') {
                // cosmetic continuation characters — skip
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) raw_rows.push_back(cur);
        // strip trailing };
        for (auto& r : raw_rows) {
            auto p = r.find('}');
            if (p != std::string::npos) r = r.substr(0, p);
            auto sc = r.find(';');
            if (sc != std::string::npos) r = r.substr(0, sc);
        }
    }

    std::vector<RowSpec> rows;

    for (int ri = 0; ri < (int)raw_rows.size(); ++ri) {
        const std::string& rstr = raw_rows[ri];
        RowSpec row;
        row.row_h = (ri < (int)row_heights.size()) ? row_heights[ri] : 3;

        // ── Step 2: extract [cell] tokens ────────────────────────────────
        size_t p = 0;
        while (p < rstr.size()) {
            auto open = rstr.find('[', p);
            if (open == std::string::npos) break;
            // Find matching ']', accounting for nested '{}' inside
            size_t close = open + 1;
            int depth = 0;
            while (close < rstr.size()) {
                if (rstr[close] == '{') ++depth;
                else if (rstr[close] == '}') --depth;
                else if (rstr[close] == ']' && depth == 0) break;
                ++close;
            }
            if (close >= rstr.size()) break;

            std::string cell_str = rstr.substr(open + 1, close - open - 1);
            p = close + 1;

            CellSpec cs;

            // ── Check for modifier braces {side, ±w, h} ──────────────────
            auto bo = cell_str.find('{');
            auto bc = cell_str.rfind('}');
            if (bo != std::string::npos && bc != std::string::npos && bc > bo) {
                std::string name = cell_str.substr(0, bo);
                std::string mods = cell_str.substr(bo + 1, bc - bo - 1);
                cs.id = block_id_from_name(name);

                // Split mods by ','
                std::vector<std::string> parts;
                std::istringstream ss(mods);
                std::string tok;
                while (std::getline(ss, tok, ',')) {
                    auto ts = tok.find_first_not_of(" \t");
                    auto te = tok.find_last_not_of(" \t");
                    if (ts != std::string::npos)
                        parts.push_back(tok.substr(ts, te - ts + 1));
                }
                // parts[0] = side label (left/right/top/bottom) — informational
                // parts[1] = ±w_delta
                // parts[2] = h_abs
                if (parts.size() >= 2) {
                    try { cs.w_delta = std::stoi(parts[1]); } catch (...) {}
                }
                if (parts.size() >= 3) {
                    try {
                        std::string hs = parts[2];
                        // strip any ± prefix (Y axis: sign ignored per spec)
                        hs.erase(std::remove_if(hs.begin(), hs.end(),
                            [](char c){ return c == '+' || c == '-'; }), hs.end());
                        cs.h_abs = std::stoi(hs);
                        if (cs.h_abs > 0) row.row_h = cs.h_abs;
                    } catch (...) {}
                }
            } else {
                cs.id = block_id_from_name(cell_str);
            }

            row.cells.push_back(cs);
        }

        if (!row.cells.empty()) rows.push_back(row);
    }
    return rows;
}

// ─── Layout engine ────────────────────────────────────────────────────────────
struct LayoutResult {
    std::vector<PanelRect>   panels;
    std::set<BlockId>        enabled;   // blocks that appear in the layout
};

static inline bool is_direction(BlockId id) {
    return id == BlockId::DIR_LEFT  || id == BlockId::DIR_RIGHT
        || id == BlockId::DIR_UP    || id == BlockId::DIR_DOWN;
}
static inline bool is_real_block(BlockId id) {
    return id != BlockId::NONE && id != BlockId::EMPTY && !is_direction(id);
}

inline LayoutResult compute_layout(const std::vector<RowSpec>& rows,
                                    int terminal_w, int /*terminal_h*/)
{
    // ── Phase 1: assign pixel rects to every cell ─────────────────────────
    struct CellRect { BlockId id; int x, y, w, h; };
    std::vector<std::vector<CellRect>> grid;

    int cur_y = 0;
    for (const auto& row : rows) {
        int n = (int)row.cells.size();
        if (n == 0) continue;

        // Equal-width distribution
        std::vector<int> ws(n, terminal_w / n);
        int rem = terminal_w - (terminal_w / n) * n;
        for (int i = 0; i < rem; ++i) ws[i]++;

        // Apply w_delta (steals/gives from right neighbour)
        for (int i = 0; i < n; ++i) {
            int d = row.cells[i].w_delta;
            if (d == 0) continue;
            ws[i] += d;
            int nb = (d > 0) ? i + 1 : i + 1;
            if (nb < n) ws[nb] -= d;
        }
        // Clamp minimum width
        for (auto& w : ws) w = std::max(4, w);
        // Re-align last cell to terminal edge
        if (n > 0) {
            int used = 0; for (int i = 0; i < n - 1; ++i) used += ws[i];
            ws[n - 1] = std::max(4, terminal_w - used);
        }

        // Compute x positions
        std::vector<int> xs(n, 0);
        for (int i = 1; i < n; ++i) xs[i] = xs[i-1] + ws[i-1];

        int rh = row.row_h;
        // Cell h_abs can override
        for (const auto& cs : row.cells)
            if (cs.h_abs > 0) { rh = cs.h_abs; break; }

        std::vector<CellRect> rects;
        rects.reserve(n);
        for (int i = 0; i < n; ++i)
            rects.push_back({row.cells[i].id, xs[i], cur_y, ws[i], rh});

        grid.push_back(rects);
        cur_y += rh;
    }

    // ── Phase 2: register real blocks ────────────────────────────────────
    std::map<BlockId, PanelRect> pm;
    for (auto& gr : grid) {
        for (auto& cell : gr) {
            if (!is_real_block(cell.id)) continue;
            if (!pm.count(cell.id)) {
                pm[cell.id] = {cell.id, cell.x, cell.y, cell.w, cell.h, true};
            } else {
                auto& p = pm[cell.id];
                int x2 = std::max(p.x + p.w, cell.x + cell.w);
                int y2 = std::max(p.y + p.h, cell.y + cell.h);
                p.x = std::min(p.x, cell.x);
                p.y = std::min(p.y, cell.y);
                p.w = x2 - p.x;
                p.h = y2 - p.y;
            }
        }
    }

    // ── Phase 3: resolve direction cells ─────────────────────────────────
    // We use a helper to look up the "real" owner following direction chains.
    // Only one level of indirection is supported (LEFT/RIGHT/UP/DOWN of a real block).
    auto find_owner = [&](int row_i, int col_i, BlockId dir) -> BlockId {
        if (dir == BlockId::DIR_LEFT) {
            for (int c = col_i - 1; c >= 0; --c)
                if (is_real_block(grid[row_i][c].id)) return grid[row_i][c].id;
        } else if (dir == BlockId::DIR_RIGHT) {
            for (int c = col_i + 1; c < (int)grid[row_i].size(); ++c)
                if (is_real_block(grid[row_i][c].id)) return grid[row_i][c].id;
        } else if (dir == BlockId::DIR_UP && row_i > 0) {
            int mid_x = grid[row_i][col_i].x + grid[row_i][col_i].w / 2;
            for (auto& prev : grid[row_i-1])
                if (prev.x <= mid_x && mid_x < prev.x + prev.w && is_real_block(prev.id))
                    return prev.id;
        } else if (dir == BlockId::DIR_DOWN && row_i + 1 < (int)grid.size()) {
            int mid_x = grid[row_i][col_i].x + grid[row_i][col_i].w / 2;
            for (auto& nxt : grid[row_i+1])
                if (nxt.x <= mid_x && mid_x < nxt.x + nxt.w && is_real_block(nxt.id))
                    return nxt.id;
        }
        return BlockId::NONE;
    };

    for (int r = 0; r < (int)grid.size(); ++r) {
        for (int c = 0; c < (int)grid[r].size(); ++c) {
            BlockId dir = grid[r][c].id;
            if (!is_direction(dir)) continue;
            BlockId owner = find_owner(r, c, dir);
            if (owner == BlockId::NONE || !pm.count(owner)) continue;
            // Expand owner rect to include this cell
            const CellRect& cell = grid[r][c];
            auto& p = pm[owner];
            int x2 = std::max(p.x + p.w, cell.x + cell.w);
            int y2 = std::max(p.y + p.h, cell.y + cell.h);
            p.x = std::min(p.x, cell.x);
            p.y = std::min(p.y, cell.y);
            p.w = x2 - p.x;
            p.h = y2 - p.y;
        }
    }

    // ── Assemble result ───────────────────────────────────────────────────
    LayoutResult result;
    for (auto& [id, rect] : pm) {
        result.panels.push_back(rect);
        result.enabled.insert(id);
    }
    // Sort top-to-bottom, left-to-right for deterministic order
    std::sort(result.panels.begin(), result.panels.end(),
        [](const PanelRect& a, const PanelRect& b){
            return (a.y != b.y) ? a.y < b.y : a.x < b.x;
        });
    return result;
}
