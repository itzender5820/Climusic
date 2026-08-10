/*  CLI.MUSIC.COM — player.cpp  */
#include "player.h"
#include <algorithm>
#include <cmath>

Player::Player() = default;
Player::~Player() { shutdown(); }

bool Player::init() {
    AudioSpec spec{ .sample_rate = 44100, .channels = 2, .buffer_size = 2048 };
    if (!audio_.init(spec)) { status_msg = "AudioEngine init failed"; return false; }
    audio_.set_volume(volume_ / 100.0f);
    playlist_.load_dir(Playlist::MUSIC_DIR);
    return true;
}

void Player::shutdown() {
    quit_ = true;
    audio_.stop();
    audio_.shutdown();
    decoder_.close();
}

void Player::load_and_play(const std::string& path) {
    audio_.stop();
    decoder_.close();
    track_done_ = false;

    if (!decoder_.open(path)) {
        status_msg = "Failed to open: " + path;
        state_     = PlayerState::STOPPED;
        return;
    }
    meta_  = decoder_.meta();
    state_ = PlayerState::PLAYING;

    audio_.start([this](float* buf, int frames) -> int {
        return this->audio_callback(buf, frames);
    });
}

int Player::audio_callback(float* buf, int frames) {
    if (state_ != PlayerState::PLAYING) {
        std::fill(buf, buf + frames * 2, 0.0f);
        return frames;
    }
    int written = decoder_.decode_next(buf, frames);
    if (written > 0)
        visualizer_.push_samples(buf, written, (int)meta_.channels, (int)meta_.sample_rate);
        vocal_viz_.push(buf, written, (int)meta_.channels, (int)meta_.sample_rate);
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
    } else {
        if (auto* e = playlist_.current()) load_and_play(e->path);
    }
}

void Player::stop() { audio_.stop(); state_ = PlayerState::STOPPED; }

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
    if (auto* e = playlist_.current()) load_and_play(e->path);
}

void Player::prev()  {
    playlist_.prev();
    if (auto* e = playlist_.current()) load_and_play(e->path);
}

void Player::seek(double s) { decoder_.seek(s); }

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
double Player::position() const { return decoder_.position(); }
double Player::duration() const { return (double)meta_.duration_sec; }
