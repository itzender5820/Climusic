#pragma once
/*  MUSICO VERSE 2.0 — lyrics.h
 *  Features:
 *    - Word-level LRC sync (Enhanced LRC <mm:ss.xx> tokens)
 *    - End-of-song scroll fix (active line always centred; blank padding)
 *    - Real cancel fix: the fetch child process is actually killed on
 *      cancel, not just abandoned. (See BUGFIX note in fetch_async().)
 *    - Sidecar .lrc / .txt / .lyrics with graceful fallback
 *    - Async syncedlyrics fetch cached as .lrc
 *    - Fallback: yt-dlp subtitle fetch (--sub-langs en) when syncedlyrics
 *      has nothing, converted from VTT into the same line-synced format.
 */
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>
#include <filesystem>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>

// POSIX non-blocking pipe select (Android/Termux compatible)
#include <unistd.h>
#include <sys/select.h>

#include "proc_util.h"

namespace fs = std::filesystem;

// ─── Data types ──────────────────────────────────────────────────────────────

struct WordToken {
    double      time_sec = 0.0;   // timestamp when this word starts
    std::string text;             // the word text (may include trailing space)
};

struct LrcLine {
    double                  time_sec  = 0.0;
    std::string             full_text;        // full rendered line
    std::vector<WordToken>  words;            // empty = no word-level sync
};

// ─── Lyrics engine ───────────────────────────────────────────────────────────

class Lyrics {
public:
    enum class State { IDLE, FETCHING, FOUND, NOT_FOUND };

    // BUGFIX (audit A3): no destructor meant a joinable fetch_thread_ at
    // destruction time would hit std::thread's destructor calling
    // std::terminate() and hard-crashing the app. main.cpp already calls
    // cancel_fetch() explicitly before exit, which happens to avoid this
    // today, but the class itself wasn't safe to destroy on any other
    // path (an exception, a future refactor, etc.) — a destructor makes
    // that guarantee unconditional instead of relying on caller discipline.
    ~Lyrics() { cancel_fetch(); }

    // Load lyrics for the given audio file + metadata.
    // Always call cancel_fetch() before this to kill any running fetch.
    void load(const std::string& audio_path,
              const std::string& title,
              const std::string& artist)
    {
        cancel_fetch();
        {
            std::lock_guard lk(mtx_);
            lines_.clear();
            has_ts_    = false;
            state_     = State::IDLE;
            status_    = "";
        }

        if (try_sidecar(audio_path)) return;

        if (title.empty() && artist.empty()) {
            state_ = State::NOT_FOUND;
            return;
        }

        state_      = State::FETCHING;
        cancel_     = false;
        cache_path_ = make_cache_path(audio_path);
        {
            // BUGFIX (audit C1): this used to be written outside the
            // mutex while status() reads it under the mutex from another
            // thread — formally a data race even though this codebase's
            // actual call pattern (load() and status() both invoked from
            // the main thread) means it hasn't manifested as a visible
            // bug. Fixed for correctness regardless.
            std::lock_guard lk(mtx_);
            status_ = "Fetching lyrics…";
        }

        fetch_thread_ = std::thread([this, title, artist] {
            fetch_async(title, artist);
        });
    }

    // Signal the fetch thread to stop and wait for it (at most ~150 ms).
    void cancel_fetch() {
        cancel_ = true;
        if (fetch_thread_.joinable()) fetch_thread_.join();
        // BUGFIX (audit C2): used to also do `cancel_ = false;` here.
        // load() is the only place that should ever clear cancel_ (right
        // before spawning the thread that needs to see it start false) —
        // resetting it here too meant that if load() were ever called
        // from a second thread while another was mid-load(), the reset
        // could clear a flag a newly-spawned fetch thread still needed to
        // observe. Not reachable in this codebase's current single-caller
        // usage, but the class itself wasn't safe against it before.
    }

    // ── Queries ──────────────────────────────────────────────────────────────

    [[nodiscard]] State       state()      const { return state_.load(); }
    [[nodiscard]] std::string status()     const { std::lock_guard l(mtx_); return status_; }
    [[nodiscard]] bool        has_lyrics() const { std::lock_guard l(mtx_); return !lines_.empty(); }

    // Returns `window` lines centred on the active line.
    // Active line is always at index window/2; edges padded with empty strings.
    // FIX: old code stopped centring near end of song — now always centred.
    [[nodiscard]] std::vector<std::string> visible(double pos, int window) const {
        std::lock_guard l(mtx_);
        if (lines_.empty()) return {};
        int cur  = active_idx(pos);
        int half = window / 2;

        std::vector<std::string> out;
        out.reserve(window);
        for (int i = 0; i < window; ++i) {
            int src = cur - half + i;
            if (src >= 0 && src < (int)lines_.size())
                out.push_back(lines_[src].full_text);
            else
                out.push_back("");   // blank padding — keeps active line centred
        }
        return out;
    }

    // Active line is always at window/2 (fixed centred position).
    [[nodiscard]] int active_in_visible(double /*pos*/, int window) const {
        return window / 2;
    }

    // Returns word tokens for the currently active line (empty if no word sync).
    [[nodiscard]] std::vector<WordToken> active_words(double pos) const {
        std::lock_guard l(mtx_);
        if (lines_.empty()) return {};
        return lines_[active_idx(pos)].words;
    }

    // Index of the currently spoken word within the active line's words array.
    // Returns -1 if no word-level data.
    [[nodiscard]] int active_word_idx(double pos) const {
        std::lock_guard l(mtx_);
        if (lines_.empty()) return -1;
        const auto& words = lines_[active_idx(pos)].words;
        if (words.empty()) return -1;
        int cur = 0;
        for (int i = 0; i < (int)words.size(); ++i)
            if (words[i].time_sec <= pos) cur = i;
        return cur;
    }

private:
    std::vector<LrcLine>  lines_;
    bool                  has_ts_ = false;
    std::atomic<State>    state_{State::IDLE};
    std::string           status_, cache_path_;
    std::thread           fetch_thread_;
    std::atomic<bool>     cancel_{false};
    mutable std::mutex    mtx_;

    // ── Helpers ──────────────────────────────────────────────────────────────

    [[nodiscard]] int active_idx(double pos) const {
        int cur = 0;
        if (has_ts_)
            for (int i = 0; i < (int)lines_.size(); ++i)
                if (lines_[i].time_sec <= pos) cur = i;
        return cur;
    }

    [[nodiscard]] static std::string make_cache_path(const std::string& p) {
        return fs::path(p).replace_extension(".lrc").string();
    }

    bool try_sidecar(const std::string& p) {
        for (const char* ext : {".lrc", ".txt", ".lyrics"}) {
            fs::path c = fs::path(p);
            c.replace_extension(ext);
            if (fs::exists(c) && parse_lrc_file(c.string())) {
                state_  = State::FOUND;
                status_ = "Lyrics loaded";
                return true;
            }
        }
        return false;
    }

    // ── Enhanced LRC parser ──────────────────────────────────────────────────
    // Handles standard LRC  [mm:ss.xx]text
    // and Enhanced LRC      [mm:ss.xx]<mm:ss.xx>word <mm:ss.xx>word ...

    static double parse_ts(const std::ssub_match& m1,
                            const std::ssub_match& m2,
                            const std::ssub_match& m3)
    {
        return std::stod(m1) * 60.0
             + std::stod(m2)
             + std::stod(m3) * 0.01;
    }

    [[nodiscard]] bool parse_lrc_string(const std::string& src) {
        static const std::regex line_ts_re(R"(\[(\d+):(\d+)[\.:](\d+)\])");
        static const std::regex meta_re(R"(\[[a-zA-Z]+:)");
        static const std::regex word_ts_re(R"(<(\d+):(\d+)[\.:](\d+)>)");

        std::vector<LrcLine> tmp;
        bool timed = false;

        std::istringstream ss(src);
        std::string raw;
        while (std::getline(ss, raw)) {
            if (!raw.empty() && raw.back() == '\r') raw.pop_back();
            if (raw.empty() || std::regex_search(raw, meta_re)) continue;

            std::smatch m;
            if (!std::regex_search(raw, m, line_ts_re)) {
                tmp.push_back({0.0, raw, {}});
                continue;
            }

            double line_ts = parse_ts(m[1], m[2], m[3]);
            std::string text = std::regex_replace(raw, line_ts_re, "");

            LrcLine entry;
            entry.time_sec = line_ts;
            timed = true;

            // FIX-1: build full_text by stripping all <word_ts> tokens then
            // collapsing runs of whitespace → single space, trimmed.
            {
                std::string clean = std::regex_replace(text, word_ts_re, "");
                std::string norm;
                norm.reserve(clean.size());
                bool sp = false;
                for (char c : clean) {
                    if (c == ' ' || c == '\t') { if (!sp) norm += ' '; sp = true; }
                    else                        { norm += c; sp = false; }
                }
                size_t s = norm.find_first_not_of(' ');
                size_t e = norm.find_last_not_of(' ');
                entry.full_text = (s == std::string::npos) ? "" : norm.substr(s, e - s + 1);
            }

            if (entry.full_text.empty()) continue;

            // FIX-2: word token extraction via match positions (not sregex_token_iterator).
            // Each <ts> match owns the text between its end and the next match's start.
            {
                auto wit  = std::sregex_iterator(text.begin(), text.end(), word_ts_re);
                auto wend = std::sregex_iterator();
                for (auto it = wit; it != wend; ++it) {
                    const auto& wm = *it;
                    double wts = parse_ts(wm[1], wm[2], wm[3]);

                    size_t wtext_start = (size_t)(wm.position() + wm.length());
                    size_t wtext_end;
                    auto nxt = std::next(it);
                    wtext_end = (nxt != wend) ? (size_t)nxt->position() : text.size();

                    std::string word_text = text.substr(wtext_start, wtext_end - wtext_start);
                    // trim leading + trailing whitespace
                    size_t first = word_text.find_first_not_of(" \t\r\n");
                    if (first == std::string::npos) continue;
                    size_t last = word_text.find_last_not_of(" \t\r\n");
                    word_text = word_text.substr(first, last - first + 1);
                    if (!word_text.empty())
                        entry.words.push_back({wts, word_text});
                }
            }

            tmp.push_back(std::move(entry));
        }

        if (tmp.empty()) return false;

        if (timed)
            std::stable_sort(tmp.begin(), tmp.end(),
                [](const LrcLine& a, const LrcLine& b) {
                    return a.time_sec < b.time_sec;
                });

        std::lock_guard lk(mtx_);
        lines_  = std::move(tmp);
        has_ts_ = timed;
        return true;
    }

    [[nodiscard]] bool parse_lrc_file(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) return false;
        std::string src((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        return parse_lrc_string(src);
    }

    // NOTE: the actual process-cancellation happens in run_killable()
    // (proc_util.h), shared with stream.h. See its doc comment, and the
    // one on spawn_killable()/kill_child(), for the full explanation of
    // the bug this fixes (pclose() blocking on a still-alive network
    // call even after the local read loop had already given up).

    // ── Async fetch via syncedlyrics, falling back to yt-dlp subtitles ──────

    void fetch_async(const std::string& title, const std::string& artist) {
        if (try_syncedlyrics(title, artist)) return;
        if (cancel_) return;
        if (try_ytdlp_subs(title, artist))   return;
        if (cancel_) return;

        std::lock_guard lk(mtx_);
        state_  = State::NOT_FOUND;
        status_ = "No lyrics found";
    }

    static std::string esc_shell(const std::string& s) {
        std::string o;
        for (char c : s) { if (c == '\'' || c == '\\') o += '\\'; o += c; }
        return o;
    }

    // Cleans a YouTube-style video title for use as a lyrics *search query*
    // only — the cache filename elsewhere keeps the full original title.
    // YouTube titles are commonly like:
    //   "Alan Walker - Rise of the Drones | New Album, March 27 (Final
    //    Trailer) [z_xxWNOEdsA]"
    // Truncating at the first '(', '[', or vertical bar (ASCII '|' or the
    // fullwidth '｜' common in JP/KR titles) keeps just the clean
    // "Artist - Title" prefix syncedlyrics/yt-dlp actually want, instead of
    // searching on the full string including trailer/edition/video-ID
    // noise that tanks the match.
    static std::string clean_query_title(const std::string& title) {
        size_t cut = std::string::npos;
        auto consider = [&](size_t p) { if (p != std::string::npos && p < cut) cut = p; };
        consider(title.find('('));
        consider(title.find('['));
        consider(title.find('|'));
        consider(title.find("\xEF\xBD\x9C"));   // '｜' U+FF5C, fullwidth vertical bar
        std::string s = (cut == std::string::npos) ? title : title.substr(0, cut);
        while (!s.empty() && (std::isspace((unsigned char)s.back()) || s.back()=='-' || s.back()==':'))
            s.pop_back();
        return s.empty() ? title : s;   // never search on an empty query
    }

    // Primary source: syncedlyrics (word-level Enhanced LRC when available).
    [[nodiscard]] bool try_syncedlyrics(const std::string& title, const std::string& artist) {
        if (system("python3 -c \"import syncedlyrics\" 2>/dev/null") != 0) {
            std::lock_guard lk(mtx_);
            status_ = "syncedlyrics not installed, trying yt-dlp…";
            return false;
        }
        if (cancel_) return false;

        const std::string clean = clean_query_title(title);
        const std::string q = artist.empty() ? esc_shell(clean)
                                              : (esc_shell(artist) + " - " + esc_shell(clean));
        const std::string cmd =
            "python3 -c \""
            "import syncedlyrics,sys\n"
            "r=syncedlyrics.search('" + q + "',enhanced=True)\n"
            "if not r: r=syncedlyrics.search('" + q + "')\n"
            "sys.stdout.write(r if r else '')\n"
            "\"";

        std::string result;
        if (!run_killable(cmd, cancel_, result)) return false;
        if (result.size() < 10 || result.find('[') == std::string::npos) return false;

        if (!cache_path_.empty()) {
            std::ofstream cf(cache_path_);
            if (cf.is_open()) cf << result;
        }
        if (!parse_lrc_string(result)) return false;

        std::lock_guard lk(mtx_);
        state_  = State::FOUND;
        status_ = "Lyrics fetched ✓";
        return true;
    }

    // Fallback source: yt-dlp's own subtitle/auto-caption track.
    // Requires yt-dlp on PATH; searches YouTube for the track and downloads
    // just the English subs (--skip-download, no video/audio fetched).
    [[nodiscard]] bool try_ytdlp_subs(const std::string& title, const std::string& artist) {
        if (system("command -v yt-dlp >/dev/null 2>&1") != 0) {
            std::lock_guard lk(mtx_);
            status_ = "No lyrics found (install syncedlyrics or yt-dlp)";
            return false;
        }
        {
            std::lock_guard lk(mtx_);
            status_ = "Fetching lyrics via yt-dlp…";
        }
        if (cancel_) return false;

        std::string tmpl = (fs::temp_directory_path() / ("climusic_subs_" + std::to_string(::getpid()))).string();
        const std::string clean = clean_query_title(title);
        const std::string q = esc_shell(artist.empty() ? clean : (artist + " " + clean));
        const std::string cmd =
            "yt-dlp --skip-download --write-subs --write-auto-subs "
            "--sub-langs en --sub-format vtt --no-playlist "
            "-o '" + esc_shell(tmpl) + ".%(ext)s' "
            "\"ytsearch1:" + q + "\" >/dev/null 2>&1; "
            "cat '" + esc_shell(tmpl) + ".en.vtt' 2>/dev/null";

        std::string vtt;
        bool ok = run_killable(cmd, cancel_, vtt);

        // best-effort cleanup of whatever yt-dlp wrote alongside tmpl
        std::error_code ec;
        fs::remove(tmpl + ".en.vtt", ec);
        fs::remove(tmpl + ".info.json", ec);

        if (!ok || vtt.empty()) return false;

        std::string lrc = vtt_to_lrc(vtt);
        if (lrc.empty()) return false;

        if (!cache_path_.empty()) {
            std::ofstream cf(cache_path_);
            if (cf.is_open()) cf << lrc;
        }
        if (!parse_lrc_string(lrc)) return false;

        std::lock_guard lk(mtx_);
        state_  = State::FOUND;
        status_ = "Lyrics fetched via yt-dlp \xE2\x9C\x93";
        return true;
    }

    // Convert a WebVTT caption track into our standard LRC line format
    // ([mm:ss.xx]text per line). No word-level sync — VTT cue timing is
    // per-line/per-cue, not per-word, so this is line-level only.
    //
    // OFFSET FIX: YouTube's caption cues (auto-generated or uploaded) are
    // systematically late relative to the actual vocals — a cue's
    // timestamp marks when it starts being *displayed*, not when those
    // words are sung, and the lag is consistently close to the length of
    // the *previous* line. We correct for it by keeping each cue's text
    // in place but pulling its timestamp back to the previous cue's
    // timestamp, which lines up far better with when the words are
    // actually sung. The first line has no predecessor to borrow from, so
    // it keeps its original timestamp. A small additional fixed offset is
    // also available below to fine-tune further if a particular track
    // still feels early/late.
    [[nodiscard]] static std::string vtt_to_lrc(const std::string& vtt) {
        static const std::regex cue_ts_re(
            R"((\d+):(\d+):(\d+)[\.,](\d+)\s*-->)");
        // BUGFIX (audit D8): this was constructed fresh inside the
        // per-line loop below — regex compilation builds an NFA/DFA
        // internally and is genuinely expensive, so a caption track with
        // hundreds of lines meant hundreds of avoidable compilations.
        static const std::regex vtt_tag_re(R"(<[^>]*>)");
        std::istringstream ss(vtt);
        std::string raw;
        std::string pending_text;
        double pending_ts = -1.0;

        struct Cue { double ts; std::string text; };
        std::vector<Cue> cues;

        auto flush = [&] {
            if (pending_ts >= 0.0 && !pending_text.empty())
                cues.push_back({pending_ts, pending_text});
            pending_text.clear();
            pending_ts = -1.0;
        };

        while (std::getline(ss, raw)) {
            if (!raw.empty() && raw.back() == '\r') raw.pop_back();
            std::smatch m;
            if (std::regex_search(raw, m, cue_ts_re)) {
                flush();
                pending_ts = std::stod(m[1].str()) * 3600.0
                           + std::stod(m[2].str()) * 60.0
                           + std::stod(m[3].str())
                           + std::stod(m[4].str()) * 0.001;
            } else if (raw.empty() || raw == "WEBVTT" || raw.find("-->") != std::string::npos) {
                // skip blank/header/timing lines already handled above
            } else if (!std::all_of(raw.begin(), raw.end(), ::isdigit)) {
                // strip VTT inline tags like <00:00:01.000><c>word</c>
                std::string clean = std::regex_replace(raw, vtt_tag_re, "");
                if (!clean.empty()) {
                    if (!pending_text.empty()) pending_text += ' ';
                    pending_text += clean;
                }
            }
        }
        flush();
        if (cues.empty()) return {};

        // Pull each line's timestamp back to the previous line's — undoes
        // the ~one-line display lag described above.
        for (size_t i = cues.size(); i-- > 1; )
            cues[i].ts = cues[i - 1].ts;

        // Extra fixed fine-tune (seconds). Positive = shift lyrics later,
        // negative = earlier. 0.0 unless a specific track still drifts.
        constexpr double kYtdlpLyricsOffsetSec = 0.0;
        if constexpr (kYtdlpLyricsOffsetSec != 0.0)
            for (auto& c : cues) c.ts = std::max(0.0, c.ts + kYtdlpLyricsOffsetSec);

        std::ostringstream out;
        for (auto& c : cues) {
            int mm = (int)(c.ts / 60.0);
            double sec = c.ts - mm * 60.0;
            char ts[16];
            snprintf(ts, sizeof(ts), "[%02d:%05.2f]", mm, sec);
            out << ts << c.text << '\n';
        }
        return out.str();
    }
};
