#pragma once
/*  MUSICO VERSE 2.0 — audio_engine.h  */
#include <cstdint>
#include <functional>
#include <atomic>
#include <vector>
#include <mutex>
<<<<<<< HEAD
#include <thread>
#include <span>

using AudioCallback = std::function<int(float* buffer, int frames)>;

struct AudioSpec {
    int sample_rate = 44100;
    int channels    = 2;
    int buffer_size = 4096;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    // BUGFIX (audit B7): raw-resource-owning class (pcm_bufs_ on Android)
    // had no copy control, so an accidental pass-by-value would
    // double-free on destruction. Move isn't needed anywhere in this
    // codebase and viz_mutex_ isn't movable anyway, so just delete copy.
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    [[nodiscard]] bool init(const AudioSpec& spec);
    void shutdown();

    bool start(AudioCallback cb);
    void stop();
    void pause();
    void resume();

    void set_volume(float vol);
    void set_speed(float speed);
    [[nodiscard]] float get_volume() const { return volume_.load(); }
    [[nodiscard]] float get_speed()  const { return speed_.load(); }
    [[nodiscard]] bool  is_playing() const { return playing_.load(); }

    [[nodiscard]] std::vector<float> get_viz_buffer();
    void push_viz_buffer(const float* data, int frames);

#ifdef __ANDROID__
    void enqueue_buffer();
#endif

private:
#ifdef __ANDROID__
    void teardown_android();   // audit A6/B5 — see audio_engine.cpp
    void* engine_obj_   = nullptr;
    void* engine_itf_   = nullptr;
    void* output_mix_   = nullptr;
    void* player_obj_   = nullptr;
    void* player_itf_   = nullptr;
    void* volume_itf_   = nullptr;
    void* buffer_queue_ = nullptr;
    static void buffer_queue_callback(void* bq, void* ctx);
    int16_t* pcm_bufs_[2] = {nullptr, nullptr};
    int cur_buf_   = 0;
    int buf_frames_= 0;
#endif
    AudioSpec  spec_;
    AudioCallback callback_;
    std::atomic<float> volume_{1.0f};
    std::atomic<float> speed_{1.0f};
    std::atomic<bool>  playing_{false};
    std::atomic<bool>  paused_{false};
    std::atomic<bool>  initialized_{false};

    std::mutex          viz_mutex_;
    std::vector<float>  viz_buf_;
    static constexpr int VIZ_BUF_SIZE = 8192;

    std::thread worker_;   // audit A2 — non-Android stub's playback thread, joined in shutdown()
||||||| empty tree
=======
#include <span>

using AudioCallback = std::function<int(float* buffer, int frames)>;

struct AudioSpec {
    int sample_rate = 44100;
    int channels    = 2;
    int buffer_size = 4096;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    [[nodiscard]] bool init(const AudioSpec& spec);
    void shutdown();

    bool start(AudioCallback cb);
    void stop();
    void pause();
    void resume();

    void set_volume(float vol);
    void set_speed(float speed);
    [[nodiscard]] float get_volume() const { return volume_.load(); }
    [[nodiscard]] float get_speed()  const { return speed_.load(); }
    [[nodiscard]] bool  is_playing() const { return playing_.load(); }

    [[nodiscard]] std::vector<float> get_viz_buffer();
    void push_viz_buffer(const float* data, int frames);

#ifdef __ANDROID__
    void enqueue_buffer();
#endif

private:
#ifdef __ANDROID__
    void* engine_obj_   = nullptr;
    void* engine_itf_   = nullptr;
    void* output_mix_   = nullptr;
    void* player_obj_   = nullptr;
    void* player_itf_   = nullptr;
    void* volume_itf_   = nullptr;
    void* buffer_queue_ = nullptr;
    static void buffer_queue_callback(void* bq, void* ctx);
    int16_t* pcm_bufs_[2] = {nullptr, nullptr};
    int cur_buf_   = 0;
    int buf_frames_= 0;
#endif
    AudioSpec  spec_;
    AudioCallback callback_;
    std::atomic<float> volume_{1.0f};
    std::atomic<float> speed_{1.0f};
    std::atomic<bool>  playing_{false};
    std::atomic<bool>  paused_{false};
    std::atomic<bool>  initialized_{false};

    std::mutex          viz_mutex_;
    std::vector<float>  viz_buf_;
    static constexpr int VIZ_BUF_SIZE = 8192;
>>>>>>> origin/master
};
