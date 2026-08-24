#pragma once
/*  CLI.MUSIC.COM — player.h  v3.0 */
#include "audio_engine.h"
#include "av_decoder.h"
#include "playlist.h"
#include "visualizer.h"
#include "vocal_viz.h"
#include "queue.h"
#include <atomic>
#include <string>
#include <mutex>

enum class PlayerState { STOPPED, PLAYING, PAUSED };

class Player {
public:
    Player();
    ~Player();

    bool init();
    void shutdown();

    void load_and_play(const std::string& path);
    void play_pause();
    void stop();
    void next();
    void prev();
    void seek(double secs);
    void set_volume(int delta);
    void set_speed(int delta);
    void toggle_repeat();
    void toggle_loop();
    void toggle_shuffle();

    [[nodiscard]] PlayerState state()      const { return state_.load(); }
    [[nodiscard]] double      position()   const;
    [[nodiscard]] double      duration()   const;
    [[nodiscard]] int         volume()     const { return volume_; }
    [[nodiscard]] int         speed_pct()  const { return (int)(audio_.get_speed()*100.0f); }
    [[nodiscard]] bool        is_repeat()  const { return playlist_.repeat; }
    [[nodiscard]] bool        is_loop()    const { return playlist_.loop; }
    [[nodiscard]] bool        is_shuffle() const { return playlist_.is_shuffle(); }
    // BUGFIX (audit E1): was set by audio_callback() but had no getter,
    // so nothing ever read it — auto-advance in main.cpp relied solely on
    // position() >= duration()-0.5, which can simply never become true if
    // metadata duration is inaccurately long (common with VBR MP3 padding),
    // leaving playback stuck outputting silence indefinitely. Exposed so
    // main.cpp can OR it into the auto-advance condition as a fallback.
    [[nodiscard]] bool        is_track_done() const { return track_done_.load(); }

    [[nodiscard]] const TrackMeta& current_meta() const { return meta_; }
    [[nodiscard]] Playlist&        playlist()           { return playlist_; }
    [[nodiscard]] Visualizer&      visualizer()         { return visualizer_; }
    [[nodiscard]] VocalVisualizer& vocal_viz()          { return vocal_viz_; }
    [[nodiscard]] Queue&           queue()              { return queue_; }

    std::string status_msg;

private:
    AudioEngine     audio_;
    AvDecoder       decoder_;
    mutable std::mutex decoder_mtx_;   // audit B2 — see player.cpp
    Playlist        playlist_;
    Visualizer      visualizer_;
    VocalVisualizer vocal_viz_;
    Queue           queue_;
    TrackMeta       meta_;

    std::atomic<PlayerState> state_{PlayerState::STOPPED};
    int  volume_ = 80;
    std::atomic<bool> quit_{false};
    std::atomic<bool> track_done_{false};

    int audio_callback(float* buf, int frames);
};
