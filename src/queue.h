#pragma once
/*  CLI.MUSIC.COM — queue.h
 *  Play queue: ordered list of tracks inserted by the user.
 *  When a song finishes, the player drains the queue before
 *  falling back to the normal playlist advance.
 */
#include "playlist.h"
#include <deque>
#include <string>

class Queue {
public:
    // Add a playlist entry to the end of the queue.
    void push(const PlaylistEntry& e) { entries_.push_back(e); }

    // Remove a specific index (0-based). No-op if out of range.
    void remove(int i) {
        if (i >= 0 && i < (int)entries_.size())
            entries_.erase(entries_.begin() + i);
    }

    // Remove by path (first match).
    void remove_path(const std::string& path) {
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->path == path) { entries_.erase(it); return; }
        }
    }

    // Pop and return the front entry. Caller must check !empty() first.
    PlaylistEntry pop_front() {
        PlaylistEntry e = entries_.front();
        entries_.pop_front();
        return e;
    }

    void clear() { entries_.clear(); }

    [[nodiscard]] bool empty()  const { return entries_.empty(); }
    [[nodiscard]] int  count()  const { return (int)entries_.size(); }

    [[nodiscard]] const std::deque<PlaylistEntry>& entries() const { return entries_; }

    // Highlighted entry in the queue panel (for Add/Remove keys).
    int  highlight = 0;

    void nav_up()   { if (highlight > 0) --highlight; }
    void nav_down() { if (highlight < count() - 1) ++highlight; }
    void clamp()    { if (highlight >= count()) highlight = std::max(0, count() - 1); }

private:
    std::deque<PlaylistEntry> entries_;
};
