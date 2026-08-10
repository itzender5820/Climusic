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

    [[nodiscard]] const TrackMeta& current_meta() const { return meta_; }
    [[nodiscard]] Playlist&        playlist()           { return playlist_; }
    [[nodiscard]] Visualizer&      visualizer()         { return visualizer_; }
    [[nodiscard]] VocalVisualizer& vocal_viz()          { return vocal_viz_; }
    [[nodiscard]] Queue&           queue()              { return queue_; }

    std::string status_msg;

private:
    AudioEngine     audio_;
    AvDecoder       decoder_;
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
