/* audio_stub.cpp
 * Compiled on non-Android platforms where OpenSL ES is unavailable.
 * Provides silent audio output so the UI and decoder can still be tested.
 */
#ifdef AUDIO_STUB
#include "audio_engine.h"
#include <cstring>
#include <thread>
#include <chrono>
#include <algorithm>

AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() { shutdown(); }

bool AudioEngine::init(const AudioSpec& spec) {
    spec_        = spec;
    initialized_ = true;
    return true;
}

void AudioEngine::shutdown() {
    stop();
    initialized_ = false;
}

bool AudioEngine::start(AudioCallback cb) {
    if (!initialized_) return false;
    callback_ = cb;
    playing_  = true;
    paused_   = false;

    // Spin a thread that calls the callback at ~real-time rate (speed-adjusted)
    std::thread([this]() {
        std::vector<float> buf(spec_.buffer_size * spec_.channels);
        while (playing_) {
            if (!paused_ && callback_) {
                int written = callback_(buf.data(), spec_.buffer_size);
                push_viz_buffer(buf.data(), written);
            }
            // Adjust sleep for speed: faster speed = shorter sleep = more callbacks/sec
            double dur_ms = 1000.0 * spec_.buffer_size / spec_.sample_rate;
            double adjusted = dur_ms / std::max(0.1f, speed_.load());
            std::this_thread::sleep_for(
                std::chrono::microseconds((int)(adjusted * 1000)));
        }
    }).detach();
    return true;
}

void AudioEngine::stop()   { playing_ = false; }
void AudioEngine::pause()  { paused_  = true;  }
void AudioEngine::resume() { paused_  = false; }

void AudioEngine::set_volume(float v) { volume_ = std::clamp(v, 0.f, 3.f); }
void AudioEngine::set_speed(float s)  { speed_  = std::clamp(s, 0.5f, 2.f); }

void AudioEngine::push_viz_buffer(const float* data, int frames) {
    std::lock_guard<std::mutex> lk(viz_mutex_);
    for (int i = 0; i < frames * spec_.channels; ++i)
        viz_buf_.push_back(data[i]);
    if ((int)viz_buf_.size() > VIZ_BUF_SIZE)
        viz_buf_.erase(viz_buf_.begin(),
                       viz_buf_.begin() + ((int)viz_buf_.size() - VIZ_BUF_SIZE));
}

std::vector<float> AudioEngine::get_viz_buffer() {
    std::lock_guard<std::mutex> lk(viz_mutex_);
    return viz_buf_;
}
#endif // AUDIO_STUB
