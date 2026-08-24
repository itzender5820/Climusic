/*  MUSICO VERSE 2.0 — playlist.cpp  */
#include "playlist.h"
#include <cctype>
#include <algorithm>
#include <cstdlib>

// Expands a leading `~` to the user's home directory (via $HOME).
// Allows config.txt paths like MUSIC_DIR="~/music" or MUSIC_DIR="~/.cache/..."
static std::string expand_path(const std::string& path) {
    if (!path.empty() && path[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) {
            return std::string(home) + path.substr(1);
        }
    }
    return path;
}

// BUGFIX (audit D2): ::tolower fed straight to std::ranges::transform gets
// called with a plain `char`, which is signed on most platforms — a
// negative value (any byte >= 0x80, e.g. UTF-8 continuation bytes in a
// non-ASCII filename) is undefined behavior per the C standard, which
// requires the argument be representable as unsigned char or EOF. This
// casts through unsigned char first, same as the is_audio() line below
// already did correctly.
static char safe_tolower(char c) { return (char)std::tolower((unsigned char)c); }

// ─── Audio file detection ─────────────────────────────────────────────────────
bool Playlist::is_audio(const fs::path& p) {
    std::string ext = p.extension().string();
    for (auto& c : ext) c = safe_tolower(c);
    return ext == ".flac" || ext == ".mp3"  || ext == ".m4a"  ||
           ext == ".opus" || ext == ".ogg"  || ext == ".wav"  ||
           ext == ".aac"  || ext == ".webm" || ext == ".wma"  ||
           ext == ".ape"  || ext == ".mka"  || ext == ".mp4"  ||
           ext == ".alac" || ext == ".aiff" || ext == ".tta";
}

// ─── Sort helper (case-insensitive by display name) ───────────────────────────
void Playlist::sort_entries(std::vector<PlaylistEntry>& v) {
    std::ranges::sort(v, [](const PlaylistEntry& a, const PlaylistEntry& b) {
        std::string la = a.display_name, lb = b.display_name;
        std::ranges::transform(la, la.begin(), safe_tolower);
        std::ranges::transform(lb, lb.begin(), safe_tolower);
        return la < lb;
    });
}

// ─── load_dir ────────────────────────────────────────────────────────────────
void Playlist::load_dir(const std::string& dir) {
    std::string path = expand_path(dir);
    all_entries_.clear();
    idx_ = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(path,
        fs::directory_options::skip_permission_denied, ec);
    if (ec) { apply_filter(); return; }
    // BUGFIX (audit B10): on some standard library implementations, an
    // erroring increment() can leave the iterator unmodified rather than
    // advancing — the old "if (ec) { ec.clear(); continue; }" would then
    // retry the identical increment forever, hanging the whole app on one
    // bad filesystem entry (a permission edge case skip_permission_denied
    // doesn't cover, a broken symlink, a network-mount hiccup). Give up on
    // the remaining traversal after a few consecutive failures instead of
    // looping unboundedly.
    int consecutive_errors = 0;
    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            if (++consecutive_errors > 5) break;
            continue;
        }
        consecutive_errors = 0;
        if (it->is_regular_file(ec) && !ec && is_audio(it->path()))
            all_entries_.push_back(make_entry(it->path().string()));
        ec.clear();
    }
    sort_entries(all_entries_);
    apply_filter();
}

// ─── import ───────────────────────────────────────────────────────────────────
void Playlist::import(const std::string& path) {
    std::string expanded = expand_path(path);
    std::error_code ec;
    if (fs::is_directory(expanded, ec)) {
        fs::recursive_directory_iterator it(expanded,
            fs::directory_options::skip_permission_denied, ec);
        for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (it->is_regular_file(ec) && !ec && is_audio(it->path()))
                all_entries_.push_back(make_entry(it->path().string()));
            ec.clear();
        }
        sort_entries(all_entries_);
    } else if (fs::is_regular_file(expanded, ec) && is_audio(expanded)) {
        all_entries_.push_back(make_entry(expanded));
        sort_entries(all_entries_);
    }
    apply_filter();
}

void Playlist::add(const std::string& path) {
    std::string expanded = expand_path(path);
    all_entries_.push_back(make_entry(expanded));
    apply_filter();
}

// ─── Navigation ──────────────────────────────────────────────────────────────
const PlaylistEntry* Playlist::current() const {
    if (entries_.empty()) return nullptr;
    return &entries_[idx_];
}
const PlaylistEntry* Playlist::next() {
    if (entries_.empty()) return nullptr;
    if (loop) return &entries_[idx_];
    idx_ = (idx_ + 1) % (int)entries_.size();
    return &entries_[idx_];
}
const PlaylistEntry* Playlist::prev() {
    if (entries_.empty()) return nullptr;
    idx_ = (idx_ - 1 + (int)entries_.size()) % (int)entries_.size();
    return &entries_[idx_];
}
void Playlist::select(int idx) {
    if (idx >= 0 && idx < (int)entries_.size()) idx_ = idx;
}

// ─── Folder filter ────────────────────────────────────────────────────────────
void Playlist::set_folder_filter(const std::string& name) {
    folder_filter_ = name;
    idx_ = 0;
    apply_filter();
}

void Playlist::clear_folder_filter() {
    folder_filter_.clear();
    idx_ = 0;
    apply_filter();
}

void Playlist::apply_filter() {
    if (folder_filter_.empty()) {
        entries_ = all_entries_;
        if (idx_ >= (int)entries_.size()) idx_ = 0;
        return;
    }

    std::string fl = folder_filter_;
    std::ranges::transform(fl, fl.begin(), safe_tolower);

    entries_.clear();
    for (const auto& e : all_entries_) {
        // Match against immediate parent folder name
        std::string fn = e.folder_name;
        std::ranges::transform(fn, fn.begin(), safe_tolower);
        if (fn.find(fl) != std::string::npos)
            entries_.push_back(e);
    }
    idx_ = 0;
}

void Playlist::toggle_shuffle() {
    shuffle_ = !shuffle_;
}

// Override next() to pick random index when shuffle is on
// NOTE: we patch next() behaviour via a free function wrapper called from player.cpp
// The actual random pick is done in Player::next() which calls playlist_.next()
// so we just need the flag readable. Done.
