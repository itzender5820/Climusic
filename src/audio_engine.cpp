#include "audio_engine.h"
#include <cstring>
#include <cmath>
#include <algorithm>

#ifdef __ANDROID__
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

// ─── Internal Android context ────────────────────────────────────────────────
struct AndroidAudioCtx {
    SLObjectItf engineObj   = nullptr;
    SLEngineItf engineItf   = nullptr;
    SLObjectItf outputMixObj= nullptr;
    SLObjectItf playerObj   = nullptr;
    SLPlayItf   playerItf   = nullptr;
    SLVolumeItf volumeItf   = nullptr;
    SLAndroidSimpleBufferQueueItf bufQueueItf = nullptr;

    AudioEngine* owner = nullptr;
};

static AndroidAudioCtx g_ctx;

static void bq_player_callback(SLAndroidSimpleBufferQueueItf bq, void* ctx) {
    AudioEngine* eng = static_cast<AudioEngine*>(ctx);
    eng->enqueue_buffer();
}
#endif

AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() { shutdown(); }

bool AudioEngine::init(const AudioSpec& spec) {
    spec_ = spec;
#ifdef __ANDROID__
    SLresult result;

    // Create engine
    result = slCreateEngine((SLObjectItf*)&g_ctx.engineObj, 0, nullptr, 0, nullptr, nullptr);
    if (result != SL_RESULT_SUCCESS) return false;
    result = (*( SLObjectItf)g_ctx.engineObj)->Realize((SLObjectItf)g_ctx.engineObj, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) return false;
    result = (*(SLObjectItf)g_ctx.engineObj)->GetInterface((SLObjectItf)g_ctx.engineObj, SL_IID_ENGINE, &g_ctx.engineItf);
    if (result != SL_RESULT_SUCCESS) return false;

    // Create output mix
    result = (*g_ctx.engineItf)->CreateOutputMix(g_ctx.engineItf, (SLObjectItf*)&g_ctx.outputMixObj, 0, nullptr, nullptr);
    if (result != SL_RESULT_SUCCESS) return false;
    result = (*(SLObjectItf)g_ctx.outputMixObj)->Realize((SLObjectItf)g_ctx.outputMixObj, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) return false;

    // Create buffer queue audio player
    SLDataLocator_AndroidSimpleBufferQueue bufQueueLoc = {
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2
    };
    SLDataFormat_PCM pcmFmt = {
        SL_DATAFORMAT_PCM,
        (SLuint32)spec.channels,
        (SLuint32)(spec.sample_rate * 1000),
        SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_PCMSAMPLEFORMAT_FIXED_16,
        (spec.channels == 2) ? (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT) : SL_SPEAKER_FRONT_CENTER,
        SL_BYTEORDER_LITTLEENDIAN
    };
    SLDataSource audioSrc = { &bufQueueLoc, &pcmFmt };
    SLDataLocator_OutputMix outMixLoc = { SL_DATALOCATOR_OUTPUTMIX, (SLObjectItf)g_ctx.outputMixObj };
    SLDataSink audioSink = { &outMixLoc, nullptr };

    const SLInterfaceID ids[] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME };
    const SLboolean reqs[]   = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };

    result = (*g_ctx.engineItf)->CreateAudioPlayer(g_ctx.engineItf,
        (SLObjectItf*)&g_ctx.playerObj, &audioSrc, &audioSink, 2, ids, reqs);
    if (result != SL_RESULT_SUCCESS) return false;
    result = (*(SLObjectItf)g_ctx.playerObj)->Realize((SLObjectItf)g_ctx.playerObj, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) return false;

    (*(SLObjectItf)g_ctx.playerObj)->GetInterface((SLObjectItf)g_ctx.playerObj, SL_IID_PLAY, &g_ctx.playerItf);
    (*(SLObjectItf)g_ctx.playerObj)->GetInterface((SLObjectItf)g_ctx.playerObj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &g_ctx.bufQueueItf);
    (*(SLObjectItf)g_ctx.playerObj)->GetInterface((SLObjectItf)g_ctx.playerObj, SL_IID_VOLUME, &g_ctx.volumeItf);

    g_ctx.owner = this;
    (*g_ctx.bufQueueItf)->RegisterCallback(g_ctx.bufQueueItf, bq_player_callback, this);

    // Allocate double buffers (int16)
    buf_frames_ = spec.buffer_size;
    pcm_bufs_[0] = new int16_t[buf_frames_ * spec.channels]();
    pcm_bufs_[1] = new int16_t[buf_frames_ * spec.channels]();
#endif
    initialized_ = true;
    return true;
}

void AudioEngine::shutdown() {
    if (!initialized_) return;
    stop();
#ifdef __ANDROID__
    if (g_ctx.playerObj)   { (*(SLObjectItf)g_ctx.playerObj)->Destroy((SLObjectItf)g_ctx.playerObj);    g_ctx.playerObj   = nullptr; }
    if (g_ctx.outputMixObj){ (*(SLObjectItf)g_ctx.outputMixObj)->Destroy((SLObjectItf)g_ctx.outputMixObj); g_ctx.outputMixObj = nullptr; }
    if (g_ctx.engineObj)   { (*(SLObjectItf)g_ctx.engineObj)->Destroy((SLObjectItf)g_ctx.engineObj);     g_ctx.engineObj   = nullptr; }
    delete[] pcm_bufs_[0]; pcm_bufs_[0] = nullptr;
    delete[] pcm_bufs_[1]; pcm_bufs_[1] = nullptr;
#endif
    initialized_ = false;
}

bool AudioEngine::start(AudioCallback cb) {
    if (!initialized_) return false;
    callback_ = cb;
    playing_  = true;
    paused_   = false;
#ifdef __ANDROID__
    // Prime two buffers
    enqueue_buffer();
    enqueue_buffer();
    (*g_ctx.playerItf)->SetPlayState(g_ctx.playerItf, SL_PLAYSTATE_PLAYING);
#endif
    return true;
}

void AudioEngine::stop() {
    playing_ = false;
#ifdef __ANDROID__
    if (g_ctx.playerItf)
        (*g_ctx.playerItf)->SetPlayState(g_ctx.playerItf, SL_PLAYSTATE_STOPPED);
#endif
}

void AudioEngine::pause() {
    paused_ = true;
#ifdef __ANDROID__
    if (g_ctx.playerItf)
        (*g_ctx.playerItf)->SetPlayState(g_ctx.playerItf, SL_PLAYSTATE_PAUSED);
#endif
}

void AudioEngine::resume() {
    paused_ = false;
#ifdef __ANDROID__
    if (g_ctx.playerItf)
        (*g_ctx.playerItf)->SetPlayState(g_ctx.playerItf, SL_PLAYSTATE_PLAYING);
#endif
}

void AudioEngine::set_volume(float vol) {
    vol = std::clamp(vol, 0.0f, 3.0f);
    volume_ = vol;
#ifdef __ANDROID__
    if (g_ctx.volumeItf) {
        // Convert to millibels: 0 = -∞, 1 = 0dB
        // OpenSL ES caps at 0 dB — keep it at max; PCM amplification handles >100%
        float capped = std::min(vol, 1.0f);
        SLmillibel mb = (capped <= 0.0f) ? SL_MILLIBEL_MIN : (SLmillibel)(2000.0f * std::log10(capped));
        (*g_ctx.volumeItf)->SetVolumeLevel(g_ctx.volumeItf, mb);
    }
#endif
}

void AudioEngine::set_speed(float speed) {
    speed_ = std::clamp(speed, 0.5f, 2.0f);
    // Note: OpenSL ES does not natively support playback rate changing on all devices.
    // Speed change is handled in the decoder/resampler layer.
}

#ifdef __ANDROID__
void AudioEngine::enqueue_buffer() {
    if (!callback_ || !playing_) return;
    int16_t* buf = pcm_bufs_[cur_buf_];
    cur_buf_ ^= 1;

    std::vector<float> fbuf(buf_frames_ * spec_.channels);
    int written = callback_(fbuf.data(), buf_frames_);

    // Push to viz ring
    push_viz_buffer(fbuf.data(), written);

    // Convert float -> int16, apply volume gain (supports >1.0 amplification)
    for (int i = 0; i < written * spec_.channels; ++i) {
        float s = fbuf[i] * volume_.load() * 32767.0f;
        s = std::clamp(s, -32768.0f, 32767.0f);
        buf[i] = (int16_t)s;
    }
    // Zero pad if short
    for (int i = written * spec_.channels; i < buf_frames_ * spec_.channels; ++i)
        buf[i] = 0;

    (*g_ctx.bufQueueItf)->Enqueue(g_ctx.bufQueueItf, buf, buf_frames_ * spec_.channels * sizeof(int16_t));
}
#endif

void AudioEngine::push_viz_buffer(const float* data, int frames) {
    std::lock_guard<std::mutex> lk(viz_mutex_);
    for (int i = 0; i < frames * spec_.channels; ++i)
        viz_buf_.push_back(data[i]);
    // Keep ring size bounded
    if ((int)viz_buf_.size() > VIZ_BUF_SIZE)
        viz_buf_.erase(viz_buf_.begin(), viz_buf_.begin() + ((int)viz_buf_.size() - VIZ_BUF_SIZE));
}

std::vector<float> AudioEngine::get_viz_buffer() {
    std::lock_guard<std::mutex> lk(viz_mutex_);
    return viz_buf_;
}
