#pragma once
/*  CLI.MUSIC.COM — stream.h  v3
 *
 *  Online streaming support, layered entirely on top of external tools.
 *
 *  ARCHITECTURE (v3 — mpv resolves directly again, ffplay stays piped):
 *  -----------------------------------------------------------------------
 *  v1 opened up to three separate YouTube connections per stream (mpv's own
 *  resolver, a second fetch for the visualizer, a third for lyrics), which
 *  is a plausible trigger for YouTube's per-video throttling.
 *
 *  v2 tried fixing that by having a *single* yt-dlp fetch tee'd to both a
 *  cache file and a FIFO, with mpv/ffplay reading playback from the FIFO.
 *  That backfired: FIFOs are not seekable at the OS level, so seeking
 *  broke entirely, and pause reliability suffered too.
 *
 *  v3 splits the difference per backend:
 *    - mpv resolves + plays the URL itself again (its own bundled
 *      ytdl-hook), which is what actually gives real, reliable
 *      pause/seek/volume control — that's worth a second connection.
 *      Caching + the visualizer PCM tap come from a separate, lightweight
 *      background fetch that only ever writes to disk (`yt-dlp -o - url >
 *      cache`), staggered a few seconds behind the primary connection
 *      (see main.cpp) to avoid the throttling risk v2 was worried about.
 *    - ffplay has no YouTube support *and* no runtime seek/pause control
 *      either way (headless, no IPC channel) — so there's nothing to lose
 *      by keeping its fetch and playback on one combined connection, same
 *      as before, just without the FIFO middle-man (a plain 3-stage shell
 *      pipe: yt-dlp | tee cache | ffplay).
 *
 *  Once a track finishes downloading successfully, the cache file is a
 *  complete, ordinary audio file — StreamCache::is_complete() lets
 *  main.cpp notice this and, next time, just play it as a normal local
 *  file (real seeking, full visualizer, no network at all) instead of
 *  re-streaming, and register it into the local library.
 *
 *    - StreamSearch    : `yt-dlp --flat-playlist ytsearchN:"<query>"`,
 *                         async + cancellable exactly like Lyrics::load().
 *    - StreamCache      : disk cache path/completion helpers.
 *    - StreamPlayer     : mpv (direct URL, full IPC control) or ffplay
 *                         (combined fetch+play pipe, play/stop only).
 *    - StreamVizFeed    : tails the cache file (not the network) to feed
 *                         the existing Visualizer/VocalVisualizer.
 *    - StreamDownloader : separate, explicit `yt-dlp -x --audio-format mp3`
 *                         download into the real music library — distinct
 *                         from the automatic raw cache above, which keeps
 *                         whatever container YouTube served and isn't
 *                         necessarily tagged/converted for permanent use.
 */
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <regex>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <filesystem>
#include <chrono>

#include "proc_util.h"
#include "visualizer.h"
#include "vocal_viz.h"

namespace fs = std::filesystem;

// ─── Search result ────────────────────────────────────────────────────────
struct StreamResult {
    std::string title;
    std::string url;
    std::string duration;   // "3:45" style, empty if unavailable (e.g. live)
};

// ─── Async YouTube search via yt-dlp ────────────────────────────────────────
class StreamSearch {
public:
    enum class State { IDLE, SEARCHING, DONE, FAILED };

    // BUGFIX (audit A3): no destructor meant a joinable search thread at
    // destruction time would hit std::thread's destructor and call
    // std::terminate(), hard-crashing the app. main.cpp calls cancel()
    // explicitly before exit today, but the class wasn't safe to destroy
    // on any other path without that discipline — this makes it
    // unconditionally safe.
    ~StreamSearch() { cancel(); }

    void search(const std::string& query, int max_results = 30) {
        cancel();
        {
            std::lock_guard lk(mtx_);
            results_.clear();
            status_ = "Searching \xE2\x80\x9C" + query + "\xE2\x80\x9D\xE2\x80\xA6";
        }
        state_  = State::SEARCHING;
        cancel_ = false;
        thread_ = std::thread([this, query, max_results] { run(query, max_results); });
    }

    // Stop any in-flight search and wait for the thread to exit (~100ms max,
    // for the same reason documented in proc_util.h's run_killable()).
    void cancel() {
        cancel_ = true;
        if (thread_.joinable()) thread_.join();
        cancel_ = false;
    }

    [[nodiscard]] State state() const { return state_.load(); }
    [[nodiscard]] std::string status() const { std::lock_guard l(mtx_); return status_; }
    [[nodiscard]] std::vector<StreamResult> results() const { std::lock_guard l(mtx_); return results_; }

private:
    std::thread        thread_;
    std::atomic<bool>  cancel_{false};
    std::atomic<State> state_{State::IDLE};
    mutable std::mutex mtx_;
    std::string        status_;
    std::vector<StreamResult> results_;

    static std::string esc(const std::string& s) {
        std::string o;
        for (char c : s) { if (c == '"' || c == '\\' || c == '$' || c == '`') o += '\\'; o += c; }
        return o;
    }

    void run(const std::string& query, int max_results) {
        if (system("command -v yt-dlp >/dev/null 2>&1") != 0) {
            std::lock_guard lk(mtx_);
            state_ = State::FAILED;
            status_ = "yt-dlp not installed (pip install yt-dlp)";
            return;
        }
        if (cancel_) return;

        const std::string cmd =
            "yt-dlp --flat-playlist --no-warnings "
            "--print \"%(title)s | https://youtu.be/%(id)s | %(duration_string)s\" "
            "\"ytsearch" + std::to_string(max_results) + ":" + esc(query) + "\"";

        std::string out;
        if (!run_killable(cmd, cancel_, out)) return;   // cancelled mid-search

        std::vector<StreamResult> res;
        size_t pos = 0;
        while (pos < out.size()) {
            size_t nl = out.find('\n', pos);
            std::string line = out.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = (nl == std::string::npos) ? out.size() : nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            // rfind so a "|" inside the title itself doesn't split wrong
            size_t sep = line.rfind(" | https://youtu.be/");
            if (sep == std::string::npos) continue;
            std::string title     = line.substr(0, sep);
            std::string rest      = line.substr(sep + 3);   // "https://youtu.be/ID | DURATION" or just the id
            std::string url, duration;
            size_t dsep = rest.find(" | ");
            if (dsep == std::string::npos) {
                url = rest;
            } else {
                url      = rest.substr(0, dsep);
                duration = rest.substr(dsep + 3);
                if (duration == "NA") duration.clear();   // yt-dlp's placeholder for "unknown" (e.g. live)
            }
            if (!title.empty() && !url.empty()) res.push_back({title, url, duration});
        }

        std::lock_guard lk(mtx_);
        results_ = std::move(res);
        state_   = results_.empty() ? State::FAILED : State::DONE;
        status_  = results_.empty() ? "No results"
                                     : (std::to_string(results_.size()) + " results");
    }
};

// ─── Playback backend detection ─────────────────────────────────────────────
enum class StreamBackend { NONE, MPV, FFPLAY };

inline StreamBackend detect_stream_backend() {
    if (system("command -v mpv >/dev/null 2>&1") == 0)    return StreamBackend::MPV;
    if (system("command -v ffplay >/dev/null 2>&1") == 0) return StreamBackend::FFPLAY;
    return StreamBackend::NONE;
}

// ─── Disk cache for streamed tracks ─────────────────────────────────────────
// Filenames encode both a human-readable title and the video ID (for
// stable de-dup even if two videos share a title). ".opus" is used
// regardless of the actual container yt-dlp's bestaudio selection returns
// (commonly webm/opus, sometimes m4a/aac) — FFmpeg/mpv/ffplay all probe
// content rather than trusting the extension, so this is cosmetic labeling,
// not a correctness requirement, and it's also in Playlist::is_audio()'s
// recognized-extension list so cached tracks show up in the local library.
namespace StreamCache {
    inline std::string sanitize(std::string s) {
        for (char& c : s)
            if (c=='/'||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|') c = '_';
        if (s.size() > 100) s.resize(100);
        return s;
    }

    inline std::string dir() {
        const char* home = std::getenv("HOME");
        std::string base = home ? std::string(home) : std::string(".");
        std::string d = base + "/.cache/climusic/streams";
        std::error_code ec;
        fs::create_directories(d, ec);
        return d;
    }

    inline std::string video_id(const std::string& url) {
        size_t slash = url.find_last_of('/');
        std::string id = (slash == std::string::npos) ? url : url.substr(slash + 1);
        size_t q = id.find('?');
        if (q != std::string::npos) id = id.substr(0, q);
        return sanitize(id);
    }

    inline std::string path_for(const std::string& url, const std::string& title) {
        return dir() + "/" + sanitize(title) + "__" + video_id(url) + ".opus";
    }

    // A completed download gets a sibling "<path>.done" marker touched
    // once the fetch process exits with a genuine success status (see
    // StreamPlayer::watch_fetch) — the cache file's own existence isn't
    // enough, since a killed/failed fetch still leaves a (truncated) file.
    [[nodiscard]] inline bool is_complete(const std::string& cache_path) {
        std::error_code ec;
        return fs::exists(cache_path + ".done", ec);
    }

    inline void mark_complete(const std::string& cache_path) {
        std::ofstream f(cache_path + ".done");
    }
}

// ─── Stream playback ─────────────────────────────────────────────────────────
// mpv path: full control via its JSON IPC unix socket (play/pause/seek/vol).
// ffplay path: play/stop only — ffplay run headless (-nodisp) has no runtime
// control channel, so pause/seek are silently no-ops on that backend.
class StreamPlayer {
public:
    ~StreamPlayer() { stop(); }

    [[nodiscard]] bool available() const { return backend_ != StreamBackend::NONE; }
    [[nodiscard]] StreamBackend backend() const { return backend_; }

    void init() { backend_ = detect_stream_backend(); }

    bool play(const std::string& url, const std::string& title) {
        stop();
        if (backend_ == StreamBackend::NONE) return false;

        title_    = title;
        position_ = 0.0;
        duration_ = 0.0;
        paused_   = false;
        clear_error();
        start_time_ = std::chrono::steady_clock::now();
        cache_path_ = StreamCache::path_for(url, title);

        bool ok = spawn_pipeline(url);
        if (!ok) {
            set_error_if_empty("failed to launch player process");
            pb_state_ = PbState::FAILED;
            return false;
        }
        pb_state_ = PbState::PLAYING;
        return true;
    }

    void stop() {
        cache_fetch_delay_stop_ = true;
        if (cache_fetch_thread_.joinable()) cache_fetch_thread_.join();
        cache_fetch_delay_stop_ = false;

        reader_stop_ = true;
        if (reader_thread_.joinable()) reader_thread_.join();
        reader_stop_ = false;

        fetch_watch_stop_ = true;
        if (fetch_watch_thread_.joinable()) fetch_watch_thread_.join();
        fetch_watch_stop_ = false;

        if (ipc_fd_ >= 0) { close(ipc_fd_); ipc_fd_ = -1; }
        if (!ipc_path_.empty())  { std::error_code ec; fs::remove(ipc_path_,  ec); ipc_path_.clear();  }
        if (!log_path_.empty())  { std::error_code ec; fs::remove(log_path_,  ec); log_path_.clear();  }
        if (!fetch_log_path_.empty()) { std::error_code ec; fs::remove(fetch_log_path_, ec); fetch_log_path_.clear(); }

        if (playback_child_.pid > 0) kill_child(playback_child_);
        if (fetch_child_.pid > 0)    kill_child(fetch_child_);

        pb_state_ = PbState::IDLE;
    }

    void toggle_pause() {
        if (backend_ == StreamBackend::MPV) send_mpv(R"({"command":["cycle","pause"]})");
        // ffplay: no IPC channel available headless — unsupported.
    }

    void seek_relative(double delta_sec) {
        if (backend_ == StreamBackend::MPV)
            send_mpv(R"({"command":["seek",)" + std::to_string(delta_sec) + R"(,"relative"]})");
        // ffplay: unsupported without a display/keyboard focus.
    }

    void set_volume(int vol_0_100) {
        if (backend_ == StreamBackend::MPV)
            send_mpv(R"({"command":["set_property","volume",)" + std::to_string(vol_0_100) + R"(]})");
    }

    [[nodiscard]] bool   is_playing()  const { return pb_state_ == PbState::PLAYING; }
    // Cosmetic only: mpv/ffplay both take a moment to actually start
    // producing audio (yt-dlp resolving, buffering). Used just to show a
    // "starting…" indicator instead of claiming full playback from frame 1.
    [[nodiscard]] bool   is_resolving() const {
        return pb_state_ == PbState::PLAYING &&
               std::chrono::steady_clock::now() - start_time_ < std::chrono::seconds(2);
    }
    [[nodiscard]] bool   is_failed()   const { return pb_state_ == PbState::FAILED; }
    [[nodiscard]] bool   is_ended()    const { return pb_state_ == PbState::ENDED; }
    [[nodiscard]] bool   is_paused()   const { return paused_; }
    [[nodiscard]] double position()    const { return position_; }
    [[nodiscard]] double duration()    const { return duration_; }
    [[nodiscard]] const std::string& title() const { return title_; }
    // BUGFIX (audit B8): used to return `const std::string&` directly —
    // error_ is written from background threads (mark_stopped(), called
    // from the mpv/ffplay watcher threads) while this is read from the
    // main thread, with no synchronization at all. Returning a copy taken
    // under a lock removes both the data race and any reference-lifetime
    // hazard from a caller holding onto the reference across a write.
    [[nodiscard]] std::string last_error() const { std::lock_guard lk(error_mtx_); return error_; }
    [[nodiscard]] const std::string& cache_path() const { return cache_path_; }

    // ffplay backend can't report pause state or seek; UI should treat it
    // as "playing until stopped" and hide pause/seek controls.
    [[nodiscard]] bool supports_transport_control() const { return backend_ == StreamBackend::MPV; }

    // Non-empty exactly once, the frame right after a fetch finishes
    // downloading successfully — main.cpp should register it into the
    // local library (Playlist::add) and this then clears back to empty.
    [[nodiscard]] std::string take_completed_download() {
        std::lock_guard lk(complete_mtx_);
        std::string r = std::move(completed_path_);
        completed_path_.clear();
        return r;
    }

private:
    enum class PbState { IDLE, PLAYING, FAILED, ENDED };
    StreamBackend backend_ = StreamBackend::NONE;
    ChildProc     playback_child_;   // mpv or ffplay, reading the FIFO
    ChildProc     fetch_child_;      // yt-dlp | tee, writing cache + FIFO
    std::string   title_;
    std::string   error_;
    mutable std::mutex error_mtx_;   // protects error_ (audit B8)
    std::atomic<PbState> pb_state_{PbState::IDLE};
    std::atomic<bool>   paused_{false};
    std::atomic<double> position_{0.0};
    std::atomic<double> duration_{0.0};
    std::chrono::steady_clock::time_point start_time_;

    std::string cache_path_;
    int         ipc_fd_ = -1;
    std::string ipc_path_;
    std::string log_path_;         // mpv --log-file, for diagnosing playback failures
    std::string fetch_log_path_;   // yt-dlp stderr capture, for diagnosing fetch failures
    std::thread reader_thread_;
    std::atomic<bool> reader_stop_{false};
    std::thread fetch_watch_thread_;
    std::atomic<bool> fetch_watch_stop_{false};
    std::thread cache_fetch_thread_;         // delays start_cache_fetch() a few seconds (mpv path)
    std::atomic<bool> cache_fetch_delay_stop_{false};

    std::mutex  complete_mtx_;
    std::string completed_path_;

    static std::string esc(const std::string& s) {
        std::string o;
        for (char c : s) { if (c == '"' || c == '\\' || c == '$' || c == '`') o += '\\'; o += c; }
        return o;
    }

    // All error_ writes go through these so error_mtx_ is never bypassed
    // (audit B8 — error_ is written from background watcher threads and
    // read from the main thread via last_error()).
    void clear_error() { std::lock_guard lk(error_mtx_); error_.clear(); }
    void set_error(std::string msg) { std::lock_guard lk(error_mtx_); error_ = std::move(msg); }
    void set_error_if_empty(const std::string& msg) {
        std::lock_guard lk(error_mtx_);
        if (error_.empty()) error_ = msg;
    }
    void append_error(const std::string& msg) { std::lock_guard lk(error_mtx_); error_ += msg; }

    // Escapes a string for safe embedding inside a *single-quoted* shell
    // argument (used to wrap the fetch pipeline inside `bash -c '...'`).
    static std::string sh_squote(const std::string& s) {
        std::string o;
        for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
        return o;
    }

    // Reads the last chunk of a small diagnostic log file and collapses it
    // to a single line, for surfacing in the header status when playback
    // fails fast. Best-effort — returns "" if the file doesn't exist/empty.
    static std::string read_log_tail(const std::string& path, size_t max_chars = 200) {
        if (path.empty()) return {};
        std::ifstream f(path);
        if (!f.is_open()) return {};
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string s = ss.str();
        if (s.size() > max_chars) s = s.substr(s.size() - max_chars);
        for (char& c : s) if (c == '\n' || c == '\r') c = ' ';
        size_t b = s.find_first_not_of(' ');
        return (b == std::string::npos) ? std::string() : s.substr(b);
    }

    // MPV: launches a lightweight, cache-only background fetch (writes to
    // disk only — no playback dependency on it), then has mpv resolve and
    // play the real URL itself. FFPLAY: one combined fetch+cache+play pipe,
    // since ffplay can't resolve YouTube and has no seek either way.
    bool spawn_pipeline(const std::string& url) {
        fetch_log_path_ = (fs::temp_directory_path() /
            ("climusic_ytdlp_" + std::to_string(::getpid()) + ".log")).string();
        std::error_code ec; fs::remove(fetch_log_path_, ec);

        // With bash, PIPESTATUS[0] gives yt-dlp's own exit code — without
        // it, a downstream stage exiting cleanly (which it can even if
        // yt-dlp died mid-fetch and just gave it EOF early) would look
        // like success. Without bash, caching/playback still work, we
        // just never trust a download enough to mark it "done" and reuse
        // from disk later.
        const bool have_bash = (system("command -v bash >/dev/null 2>&1") == 0);

        if (backend_ == StreamBackend::MPV) {
            // Give mpv's own connection a few seconds' uncontested head
            // start before opening the second, cache-only one — starting
            // both at once risks the same YouTube-side throttling that
            // motivated v2's single-connection design in the first place.
            cache_fetch_delay_stop_ = false;
            cache_fetch_thread_ = std::thread([this, url, have_bash] {
                for (int i = 0; i < 150 && !cache_fetch_delay_stop_; ++i) usleep(20'000);  // ~3s
                if (cache_fetch_delay_stop_) return;
                start_cache_fetch(url, have_bash);
            });
            // Caching is best-effort — even if it never starts, actual
            // playback below doesn't depend on it at all.
            return play_mpv(url);
        }

        // ffplay: fetch + cache + play in one combined shell pipe (no
        // FIFO needed — `tee`'s pass-through output feeds straight into
        // ffplay's stdin via the pipe itself).
        const std::string body =
            "yt-dlp -f bestaudio --no-playlist --no-warnings -o - \"" + esc(url) + "\" "
            "2>'" + esc(fetch_log_path_) + "' | tee '" + esc(cache_path_) + "' | "
            "ffplay -nodisp -autoexit -loglevel error -i pipe:0";
        const std::string cmd = have_bash
            ? "bash -c '" + sh_squote(body) + "; exit ${PIPESTATUS[0]}'"
            : body;

        playback_child_ = spawn_killable(cmd);
        if (playback_child_.pid < 0) { set_error("failed to start yt-dlp|ffplay pipe"); return false; }
        if (playback_child_.fd >= 0) { close(playback_child_.fd); playback_child_.fd = -1; }

        reader_stop_ = false;
        reader_thread_ = std::thread([this, have_bash] { watch_ffplay(have_bash); });
        return true;
    }

    // Shared by watch_fetch() (mpv's separate cache fetch) and
    // watch_ffplay() (ffplay's combined pipe, where "the fetch" and "the
    // playback process" are the same thing) — on genuine success, marks
    // the cache complete and hands it off via take_completed_download().
    // On failure, deletes the (truncated) cache file so it's never
    // mistaken for a real download later.
    void finish_fetch(bool success) {
        if (success) {
            StreamCache::mark_complete(cache_path_);
            std::lock_guard lk(complete_mtx_);
            completed_path_ = cache_path_;
        } else {
            std::error_code ec; fs::remove(cache_path_, ec);
        }
    }

    // Launches the mpv-path cache-only fetch (see spawn_pipeline's delay
    // thread above) — separated out so it can be deferred without
    // blocking play()'s synchronous return.
    void start_cache_fetch(const std::string& url, bool have_bash) {
        const std::string fetch_body =
            "yt-dlp -f bestaudio --no-playlist --no-warnings -o - \"" + esc(url) + "\" "
            "2>'" + esc(fetch_log_path_) + "' > '" + esc(cache_path_) + "'";
        const std::string fetch_cmd = have_bash
            ? "bash -c '" + sh_squote(fetch_body) + "; exit ${PIPESTATUS[0]}'"
            : fetch_body;

        fetch_child_ = spawn_killable(fetch_cmd);
        if (fetch_child_.pid < 0) return;
        if (fetch_child_.fd >= 0) { close(fetch_child_.fd); fetch_child_.fd = -1; }
        fetch_watch_stop_ = false;
        fetch_watch_thread_ = std::thread([this, have_bash] { watch_fetch(have_bash); });
    }

    void watch_fetch(bool have_bash) {        while (!fetch_watch_stop_) {
            if (fetch_child_.pid <= 0) return;
            int status;
            pid_t r = waitpid(fetch_child_.pid, &status, WNOHANG);
            if (r == fetch_child_.pid) {
                bool success = have_bash && WIFEXITED(status) && WEXITSTATUS(status) == 0;
                fetch_child_.pid = -1;
                finish_fetch(success);
                return;
            }
            usleep(200'000);
        }
    }

    bool play_mpv(const std::string& url) {
        ipc_path_ = (fs::temp_directory_path() / ("climusic_mpv_" + std::to_string(::getpid()) + ".sock")).string();
        log_path_ = (fs::temp_directory_path() / ("climusic_mpv_" + std::to_string(::getpid()) + ".log")).string();
        std::error_code ec; fs::remove(ipc_path_, ec); fs::remove(log_path_, ec);

        // --log-file captures mpv's own log independent of --no-terminal /
        // --really-quiet (both needed so mpv never touches our
        // ncurses-owned TTY). Given the real URL — mpv resolves YouTube
        // itself via its bundled ytdl-hook, which is what makes real
        // seek/pause/volume control possible (a piped/FIFO'd stream can't
        // be seeked at all — see this file's top comment).
        const std::string cmd =
            "mpv --no-video --no-terminal --really-quiet "
            "--input-ipc-server='" + esc(ipc_path_) + "' "
            "--log-file='" + esc(log_path_) + "' "
            "\"" + esc(url) + "\"";

        playback_child_ = spawn_killable(cmd);
        if (playback_child_.pid < 0) return false;
        if (playback_child_.fd >= 0) { close(playback_child_.fd); playback_child_.fd = -1; }

        // Give mpv a moment to create the IPC socket, then connect.
        for (int i = 0; i < 50 && ipc_fd_ < 0; ++i) {
            ipc_fd_ = connect_ipc(ipc_path_);
            if (ipc_fd_ < 0) usleep(20'000);
        }
        if (ipc_fd_ < 0) {
            kill_child(playback_child_);
            set_error("mpv didn't open its IPC socket in time");
            std::string log = read_log_tail(log_path_);
            if (!log.empty()) append_error(": " + log);
            return false;
        }

        send_mpv(R"({"command":["observe_property",1,"time-pos"]})");
        send_mpv(R"({"command":["observe_property",2,"duration"]})");
        send_mpv(R"({"command":["observe_property",3,"pause"]})");

        reader_stop_ = false;
        reader_thread_ = std::thread([this] { read_mpv_events(); });
        return true;
    }

    static int connect_ipc(const std::string& path) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
        if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        return fd;
    }

    void send_mpv(const std::string& json_line) {
        if (ipc_fd_ < 0) return;
        std::string msg = json_line + "\n";
        ::write(ipc_fd_, msg.data(), msg.size());
    }

    // Mark the stream as over. If the process died within a few seconds of
    // starting, that's almost certainly a real failure (bad URL, missing
    // codec, blocked video, etc.) rather than the track finishing, so pull
    // the tail of its log file into error_ and report FAILED instead of
    // the more benign-sounding ENDED.
    void mark_stopped() {
        if (pb_state_ != PbState::PLAYING) return;
        bool early = std::chrono::steady_clock::now() - start_time_ < std::chrono::seconds(4);
        if (early) {
            std::string log = read_log_tail(log_path_);
            if (log.empty()) log = read_log_tail(fetch_log_path_);
            set_error(log.empty() ? "player exited immediately (see logs)" : log);
            pb_state_ = PbState::FAILED;
        } else {
            pb_state_ = PbState::ENDED;
        }
    }

    // Reads mpv's IPC event stream (one JSON object per line) and pulls out
    // the observed properties with small regex scans — consistent with how
    // the rest of this codebase (config_parser.h, lyrics.h) hand-parses
    // structured text rather than pulling in a JSON library for a couple
    // of fields.
    void read_mpv_events() {
        static const std::regex time_re (R"("name":"time-pos","data":([0-9.eE+-]+))");
        static const std::regex dur_re  (R"("name":"duration","data":([0-9.eE+-]+))");
        static const std::regex pause_re(R"("name":"pause","data":(true|false))");

        std::string buf;
        char chunk[1024];
        while (!reader_stop_) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(ipc_fd_, &rfds);
            struct timeval tv{0, 150'000};
            int ret = select(ipc_fd_ + 1, &rfds, nullptr, nullptr, &tv);
            if (ret > 0) {
                ssize_t n = read(ipc_fd_, chunk, sizeof(chunk));
                if (n <= 0) { mark_stopped(); break; }   // mpv exited / socket closed
                buf.append(chunk, (size_t)n);

                size_t nl;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    std::string line = buf.substr(0, nl);
                    buf.erase(0, nl + 1);
                    std::smatch m;
                    if (std::regex_search(line, m, time_re)) position_ = std::stod(m[1]);
                    if (std::regex_search(line, m, dur_re))  duration_ = std::stod(m[1]);
                    if (std::regex_search(line, m, pause_re)) paused_  = (m[1] == "true");
                }
            }
            // check the child is still alive; if mpv died without closing
            // the socket cleanly for some reason, don't spin forever.
            if (playback_child_.pid > 0 && waitpid(playback_child_.pid, nullptr, WNOHANG) == playback_child_.pid) {
                playback_child_.pid = -1;
                mark_stopped();
                break;
            }
        }
    }

    // For ffplay, the fetch and the playback process are the same pipe —
    // this both detects playback ending (mark_stopped(), same as before)
    // and, since have_bash means the wrapper's own exit code is yt-dlp's
    // PIPESTATUS[0], does the cache-completion bookkeeping too.
    void watch_ffplay(bool have_bash) {
        while (!reader_stop_) {
            if (playback_child_.pid > 0) {
                int status;
                pid_t r = waitpid(playback_child_.pid, &status, WNOHANG);
                if (r == playback_child_.pid) {
                    bool success = have_bash && WIFEXITED(status) && WEXITSTATUS(status) == 0;
                    playback_child_.pid = -1;
                    finish_fetch(success);
                    mark_stopped();
                    break;
                }
            } else break;
            usleep(200'000);
        }
    }
};

// ─── Live visualizer PCM feed for streams ───────────────────────────────────
// v2: tails the SAME cache file StreamPlayer's fetch pipeline is writing to
// (`tail -c +1 -f <cache>`), decodes it through ffmpeg, and pushes PCM into
// the existing Visualizer/VocalVisualizer — the same push_samples()/push()
// calls local file playback already uses. No second network connection.
//
// "500ms+ then real-time": samples are held in a pre-roll buffer until at
// least 500ms of audio has accumulated (so the very first frames aren't
// built from a half-empty buffer), then released at a steady ~20ms cadence
// from an internal buffer — decoupling "however bursty the pipe reads
// happen to be" (network/tee/ffmpeg scheduling jitter) from "how smoothly
// the animation updates". Delivering data in whatever chunk sizes a raw
// read() happens to return produces visibly jerky/bursty bars; a steady
// release cadence is what actually looks live.
//
// BUGFIX (visualizer kept moving while stream was paused): this pipeline
// runs independently of mpv/ffplay's own pause state, so it used to just
// keep reading/decoding/animating regardless. Now it checks
// StreamPlayer::is_paused() and simply stops reading from its pipe while
// paused — upstream (tail/ffmpeg) then blocks on backpressure once the OS
// pipe buffer fills, so nothing is lost or skipped, it just holds exactly
// where it was and resumes in sync when playback resumes.
class StreamVizFeed {
public:
    ~StreamVizFeed() { stop(); }

    static constexpr int SR = 44100;
    static constexpr int CH = 2;

    // Cached after the first check (see the note in stream.h's history —
    // calling this per-frame with a live system() call previously stalled
    // the whole UI while streaming).
    [[nodiscard]] static bool available() {
        static const bool ok = (system("command -v ffmpeg >/dev/null 2>&1") == 0)
                             && (system("command -v tail >/dev/null 2>&1") == 0);
        return ok;
    }

    // `player`: polled for is_paused() so the feed can freeze in lockstep
    // with actual playback (see class comment). Must outlive the feed.
    void start(const std::string& cache_path, Visualizer& viz, VocalVisualizer& vviz,
               StreamPlayer& player) {
        stop();
        if (!available()) return;

        // Wait briefly for the cache file to actually exist before
        // tailing it — `tail -f` on a nonexistent file just fails
        // immediately on most implementations. Plain POSIX loop (no bash
        // needed) since this is a lightweight local-disk check, not
        // network-timing-sensitive like the fetch delays in main.cpp.
        const std::string cmd =
            "i=0; while [ ! -f '" + esc(cache_path) + "' ] && [ $i -lt 8 ]; do sleep 1; i=$((i+1)); done; "
            "tail -c +1 -f '" + esc(cache_path) + "' 2>/dev/null | "
            "ffmpeg -hide_banner -loglevel quiet -re -i pipe:0 "
            "-f f32le -ar " + std::to_string(SR) + " -ac " + std::to_string(CH) + " pipe:1";

        child_ = spawn_killable(cmd);
        if (child_.pid < 0) return;

        stop_flag_ = false;
        thread_ = std::thread([this, &viz, &vviz, &player] { run(viz, vviz, player); });
    }

    void stop() {
        stop_flag_ = true;
        if (thread_.joinable()) thread_.join();
        if (child_.pid > 0) kill_child(child_);
        child_ = {};
    }

private:
    ChildProc          child_;
    std::thread        thread_;
    std::atomic<bool>  stop_flag_{false};

    static std::string esc(const std::string& s) {
        std::string o;
        for (char c : s) { if (c == '\'' || c == '\\') o += '\\'; o += c; }
        return o;
    }

    static void push(Visualizer& viz, VocalVisualizer& vviz, const char* bytes, size_t frames) {
        const float* samples = reinterpret_cast<const float*>(bytes);
        viz.push_samples(samples, (int)frames, CH, SR);
        vviz.push(samples, (int)frames, CH, SR);
    }

    void run(Visualizer& viz, VocalVisualizer& vviz, StreamPlayer& player) {
        using clock = std::chrono::steady_clock;
        constexpr size_t BYTES_PER_FRAME = CH * sizeof(float);
        const size_t lead_bytes    = (size_t)(0.5 * SR * BYTES_PER_FRAME);   // 500ms pre-roll
        constexpr double RELEASE_MS = 20.0;                                  // steady release cadence
        const size_t chunk_frames  = (size_t)(SR * RELEASE_MS / 1000.0);
        const size_t chunk_bytes   = chunk_frames * BYTES_PER_FRAME;

        std::vector<char> buf;       // accumulator; buf_off tracks what's been released
        size_t buf_off = 0;
        bool primed = false;
        char chunk[4096];
        auto last_release = clock::now();

        while (!stop_flag_) {
            if (player.is_paused()) {
                // Don't read at all while paused — tail/ffmpeg naturally
                // block on the OS pipe buffer filling, holding position
                // exactly (no drift, no discarded audio) until resumed.
                usleep(50'000);
                last_release = clock::now();   // don't "catch up" a burst on resume
                continue;
            }

            fd_set rfds; FD_ZERO(&rfds); FD_SET(child_.fd, &rfds);
            struct timeval tv{0, 20'000};
            int ret = select(child_.fd + 1, &rfds, nullptr, nullptr, &tv);
            if (ret > 0) {
                ssize_t n = read(child_.fd, chunk, sizeof(chunk));
                if (n <= 0) break;   // EOF — pipeline ended (track over / fetch failed)
                buf.insert(buf.end(), chunk, chunk + n);
            } else if (ret < 0) {
                break;
            }

            // Bound memory: drop the already-released prefix periodically
            // rather than on every iteration (erase-from-front is O(n)).
            if (buf_off > 1u << 20) { buf.erase(buf.begin(), buf.begin() + buf_off); buf_off = 0; }

            size_t avail = buf.size() - buf_off;
            if (!primed) {
                if (avail < lead_bytes) continue;   // keep accumulating pre-roll silently
                primed = true;
                last_release = clock::now();
            }

            auto now = clock::now();
            double elapsed_ms = std::chrono::duration<double, std::milli>(now - last_release).count();
            while (elapsed_ms >= RELEASE_MS && (buf.size() - buf_off) >= chunk_bytes) {
                push(viz, vviz, buf.data() + buf_off, chunk_frames);
                buf_off += chunk_bytes;
                elapsed_ms -= RELEASE_MS;
                last_release += std::chrono::duration_cast<clock::duration>(
                    std::chrono::duration<double, std::milli>(RELEASE_MS));
            }
        }
    }
};

// ─── Background download (explicit, into the real music library) ──────────
class StreamDownloader {
public:
    // Fire-and-forget: fully detaches so it survives even if the player
    // exits right after the hotkey is pressed. No progress reporting by
    // design (would need a persistent status file / IPC to track across
    // a detached process) — the caller should just show a transient
    // "Downloading in background…" status message.
    //
    // Distinct from the automatic StreamCache: this produces a properly
    // tagged, converted mp3 in the real library, versus the cache's raw
    // as-served container kept purely for fast replay/visualizer taps.
    static bool download(const std::string& url, const std::string& out_dir) {
        if (system("command -v yt-dlp >/dev/null 2>&1") != 0) return false;
        std::error_code ec;
        fs::create_directories(out_dir, ec);

        const std::string cmd =
            "yt-dlp -x --audio-format mp3 --no-playlist "
            "-o '" + esc(out_dir) + "/%(title)s.%(ext)s' "
            "\"" + esc(url) + "\"";
        spawn_detached(cmd);
        return true;
    }

private:
    static std::string esc(const std::string& s) {
        std::string o;
        for (char c : s) { if (c == '\'' || c == '\\') o += '\\'; o += c; }
        return o;
    }
};
