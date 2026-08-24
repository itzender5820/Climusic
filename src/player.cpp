/*  CLI.MUSIC.COM — player.cpp  */
#include "player.h"
#include <algorithm>
#include <cmath>
#include "proc_util.h"

Player::Player() = default;
Player::~Player() { shutdown(); }

bool Player::init(const std::string& music_dir) {
    AudioSpec spec{ .sample_rate = 44100, .channels = 2, .buffer_size = 2048 };
    if (!audio_.init(spec)) { status_msg = "AudioEngine init failed"; return false; }
    audio_.set_volume(volume_ / 100.0f);
    playlist_.load_dir(music_dir);
    return true;
}

void Player::shutdown() {
    quit_ = true;
    #ifndef __ANDROID__
    if (mpv_proc_.pid > 0) kill_child(mpv_proc_);
    #endif
    audio_.stop();
    audio_.shutdown();
    decoder_.close();
}

void Player::load_and_play(const std::string& path) {
    audio_.stop();
    #ifndef __ANDROID__
    if (mpv_proc_.pid > 0) kill_child(mpv_proc_);
    #endif
    decoder_.close();
    track_done_ = false;

    if (!decoder_.open(path)) {
        status_msg = "Failed to open: " + path;
        state_     = PlayerState::STOPPED;
        return;
    }
    meta_  = decoder_.meta();
    state_ = PlayerState::PLAYING;

    #ifdef __ANDROID__
    audio_.start([this](float* buf, int frames) -> int {
        return this->audio_callback(buf, frames);
    });
    #else
    // On Linux, spawn mpv to handle actual audio output to the speakers.
    std::string cmd = "mpv --no-video --no-terminal --really-quiet \"" + path + "\"";
    mpv_proc_ = spawn_killable(cmd);
    if (mpv_proc_.fd >= 0) close(mpv_proc_.fd); // We don't need mpv's stdout
    
    // Still use the internal audio engine to feed the visualizer in real-time.
    audio_.start([this](float* buf, int frames) -> int {
        return this->audio_callback(buf, frames);
    });
    #endif
}

int Player::audio_callback(float* buf, int frames) {
    if (state_ != PlayerState::PLAYING) {
        std::fill(buf, buf + frames * 2, 0.0f);
        return frames;
    }
    // BUGFIX (audit B2): decoder_ is touched here (the real-time audio
    // thread) and from seek()/position() (the UI thread) with no
    // synchronization — AvDecoder isn't internally thread-safe. A brief
    // mutex hold is an acceptable trade (a possible tiny glitch on seek)
    // against actual corruption/crashes from a genuine data race.
    int written;
    {
        std::lock_guard lk(decoder_mtx_);
        written = decoder_.decode_next(buf, frames);
    }
    // BUGFIX (audit A1): decode_next()'s documented contract allows a
    // negative return on error (see av_decoder.h), even though the
    // current FFmpeg-backed implementation never actually produces one
    // today. Clamping defends against that documented-but-unexercised
    // case (or any future implementation change) causing a negative
    // `written * 2` offset — which would make the std::fill below write
    // backwards into memory before `buf`.
    written = std::max(0, written);
    // BUGFIX (audit B1): these two calls used to have no braces, so
    // vocal_viz_.push() ran unconditionally regardless of the `if`,
    // pushing invalid/empty data whenever written <= 0.
    if (written > 0) {
        visualizer_.push_samples(buf, written, (int)meta_.channels, (int)meta_.sample_rate);
        vocal_viz_.push(buf, written, (int)meta_.channels, (int)meta_.sample_rate);
    }
    if (written < frames) {
        std::fill(buf + written * 2, buf + frames * 2, 0.0f);
        track_done_ = true;
    }
    return frames;
}

void Player::play_pause() {
    if (state_ == PlayerState::PLAYING) {
        state_ = PlayerState::PAUSED; audio_.pause();
    } else if (state_ == PlayerState::PAUSED) {
        state_ = PlayerState::PLAYING; audio_.resume();
    }
    // BUGFIX (audit D3): this used to call load_and_play() directly when
    // STOPPED, bypassing main.cpp's load_track() wrapper — which is the
    // only place lyrics.load() gets called. Pressing play/space on a
    // stopped player silently skipped fetching lyrics for the track it
    // just started. Player no longer auto-loads here; main.cpp's
    // play_pause handler checks state() == STOPPED and calls load_track()
    // itself instead, so lyrics fetching is never bypassed.
}

void Player::stop() {
    #ifndef __ANDROID__
    if (mpv_proc_.pid > 0) kill_child(mpv_proc_);
    #endif
    audio_.stop();
    state_ = PlayerState::STOPPED;
}

void Player::next() {
    // Shuffle: pick a random index different from current
    if (playlist_.is_shuffle() && playlist_.count() > 1) {
        int cur = playlist_.current_idx();
        int nxt = cur;
        while (nxt == cur)
            nxt = rand() % playlist_.count();
        playlist_.select(nxt);
    } else {
        playlist_.next();
    }
    // BUGFIX (audit C6): used to also call load_and_play() here — main.cpp
    // always calls load_track() right after next()/prev() anyway (the only
    // path that also fetches lyrics), so the track was being opened,
    // decoded-into, and the audio engine restarted twice per skip. Caller
    // owns loading now; this only updates playlist_'s selection.
}

void Player::prev()  {
    playlist_.prev();
}

void Player::seek(double s) {
    std::lock_guard lk(decoder_mtx_);   // audit B2 — see audio_callback()
    decoder_.seek(s);
}

void Player::set_volume(int delta) {
    volume_ = std::clamp(volume_ + delta * 5, 0, 300);
    audio_.set_volume(volume_ / 100.0f);
}
void Player::set_speed(int delta) {
    float sp = std::clamp(audio_.get_speed() + delta * 0.1f, 0.5f, 2.0f);
    audio_.set_speed(sp);
}
void Player::toggle_repeat()  { playlist_.repeat = !playlist_.repeat; }
void Player::toggle_loop()    { playlist_.loop   = !playlist_.loop;   }
void Player::toggle_shuffle() { playlist_.toggle_shuffle(); }
double Player::position() const {
    std::lock_guard lk(decoder_mtx_);   // audit B2 — see audio_callback()
    return decoder_.position();
}
double Player::duration() const { return (double)meta_.duration_sec; }
