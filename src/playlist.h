#pragma once
/*  CLI.MUSIC.COM — playlist.h  */
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <ranges>
#include <random>

namespace fs = std::filesystem;

struct PlaylistEntry {
    std::string path;
    std::string display_name;
    std::string folder_name;
};

class Playlist {
public:
    void load_dir(const std::string& dir);
    void import(const std::string& path);
    void add(const std::string& path);

    [[nodiscard]] const PlaylistEntry* current() const;
    const PlaylistEntry* next();
    const PlaylistEntry* prev();
    void select(int idx);
    [[nodiscard]] int current_idx() const { return idx_; }
    [[nodiscard]] int count()       const { return (int)entries_.size(); }
    [[nodiscard]] int total_count() const { return (int)all_entries_.size(); }

    void set_folder_filter(const std::string& name);
    void clear_folder_filter();
    [[nodiscard]] const std::string& folder_filter() const { return folder_filter_; }
    [[nodiscard]] bool has_filter() const { return !folder_filter_.empty(); }

    void toggle_shuffle();
    [[nodiscard]] bool is_shuffle() const { return shuffle_; }

    bool repeat = false;
    bool loop   = false;

    [[nodiscard]] const std::vector<PlaylistEntry>& entries() const { return entries_; }

private:
    std::vector<PlaylistEntry> all_entries_;
    std::vector<PlaylistEntry> entries_;
    int idx_ = 0;
    std::string folder_filter_;
    bool shuffle_ = false;
    std::mt19937 rng_{ std::random_device{}() };

    static bool is_audio(const fs::path& p);

    [[nodiscard]] static PlaylistEntry make_entry(const std::string& path) {
        fs::path fp(path);
        return { path, fp.filename().string(), fp.parent_path().filename().string() };
    }

    void apply_filter();
    void sort_entries(std::vector<PlaylistEntry>& v);
};
