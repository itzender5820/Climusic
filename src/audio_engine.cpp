#include "audio_engine.h"
#include <cstring>
#include <cmath>
#include <algorithm>

#ifdef __ANDROID__
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

// BUGFIX (audit A6): OpenSL state used to live in a single global static
// AndroidAudioCtx shared by every AudioEngine instance — two instances
// would silently clobber each other's engine/player/buffer-queue handles.
// audio_engine.h already declared instance members for exactly this
// purpose (engine_obj_, engine_itf_, etc.) but this file wasn't using
// them. Now each AudioEngine owns its own OpenSL state via those members,
// cast to their real SL types at each use site.
#define ENG_OBJ  ((SLObjectItf)engine_obj_)
#define ENG_ITF  ((SLEngineItf)engine_itf_)
#define OUT_MIX  ((SLObjectItf)output_mix_)
#define PLR_OBJ  ((SLObjectItf)player_obj_)
#define PLR_ITF  ((SLPlayItf)player_itf_)
#define VOL_ITF  ((SLVolumeItf)volume_itf_)
#define BUFQ_ITF ((SLAndroidSimpleBufferQueueItf)buffer_queue_)

static void bq_player_callback(SLAndroidSimpleBufferQueueItf bq, void* ctx) {
    (void)bq;
    AudioEngine* eng = static_cast<AudioEngine*>(ctx);
    eng->enqueue_buffer();
}
#endif

AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() { shutdown(); }

#ifdef __ANDROID__
// Unconditional teardown of whatever OpenSL/buffer state has been created
// so far — used both by a genuine shutdown() and by init()'s failure path
// (audit B5, see below), which previously just `return false`'d without
// releasing anything already-created up to that point, since initialized_
// (which shutdown() gates on) is never set true on a failed init().
void AudioEngine::teardown_android() {
    if (player_obj_)    { (*PLR_OBJ)->Destroy(PLR_OBJ); player_obj_ = nullptr; }
    if (output_mix_)    { (*OUT_MIX)->Destroy(OUT_MIX); output_mix_ = nullptr; }
    if (engine_obj_)    { (*ENG_OBJ)->Destroy(ENG_OBJ); engine_obj_ = nullptr; }
    engine_itf_ = nullptr; player_itf_ = nullptr; volume_itf_ = nullptr; buffer_queue_ = nullptr;
    delete[] pcm_bufs_[0]; pcm_bufs_[0] = nullptr;
    delete[] pcm_bufs_[1]; pcm_bufs_[1] = nullptr;
}
#endif

bool AudioEngine::init(const AudioSpec& spec) {
    spec_ = spec;
#ifdef __ANDROID__
    SLresult result;

    result = slCreateEngine((SLObjectItf*)&engine_obj_, 0, nullptr, 0, nullptr, nullptr);
    if (result != SL_RESULT_SUCCESS) { teardown_android(); return false; }
    result = (*ENG_OBJ)->Realize(ENG_OBJ, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) { teardown_android(); return false; }
    result = (*ENG_OBJ)->GetInterface(ENG_OBJ, SL_IID_ENGINE, &engine_itf_);
    if (result != SL_RESULT_SUCCESS) { teardown_android(); return false; }

    // Create output mix
    result = (*ENG_ITF)->CreateOutputMix(ENG_ITF, (SLObjectItf*)&output_mix_, 0, nullptr, nullptr);
    if (result != SL_RESULT_SUCCESS) { teardown_android(); return false; }
    result = (*OUT_MIX)->Realize(OUT_MIX, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) { teardown_android(); return false; }

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
    SLDataLocator_OutputMix outMixLoc = { SL_DATALOCATOR_OUTPUTMIX, OUT_MIX };
    SLDataSink audioSink = { &outMixLoc, nullptr };

    const SLInterfaceID ids[] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME };
    const SLboolean reqs[]   = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };

    result = (*ENG_ITF)->CreateAudioPlayer(ENG_ITF,
        (SLObjectItf*)&player_obj_, &audioSrc, &audioSink, 2, ids, reqs);
    if (result != SL_RESULT_SUCCESS) { teardown_android(); return false; }
    result = (*PLR_OBJ)->Realize(PLR_OBJ, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) { teardown_android(); return false; }

    (*PLR_OBJ)->GetInterface(PLR_OBJ, SL_IID_PLAY, &player_itf_);
    (*PLR_OBJ)->GetInterface(PLR_OBJ, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &buffer_queue_);
    (*PLR_OBJ)->GetInterface(PLR_OBJ, SL_IID_VOLUME, &volume_itf_);

    (*BUFQ_ITF)->RegisterCallback(BUFQ_ITF, bq_player_callback, this);

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
    teardown_android();
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
    (*PLR_ITF)->SetPlayState(PLR_ITF, SL_PLAYSTATE_PLAYING);
#endif
    return true;
}

void AudioEngine::stop() {
    playing_ = false;
#ifdef __ANDROID__
    if (player_itf_)
        (*PLR_ITF)->SetPlayState(PLR_ITF, SL_PLAYSTATE_STOPPED);
#endif
}

void AudioEngine::pause() {
    paused_ = true;
#ifdef __ANDROID__
    if (player_itf_)
        (*PLR_ITF)->SetPlayState(PLR_ITF, SL_PLAYSTATE_PAUSED);
#endif
}

void AudioEngine::resume() {
    paused_ = false;
#ifdef __ANDROID__
    if (player_itf_)
        (*PLR_ITF)->SetPlayState(PLR_ITF, SL_PLAYSTATE_PLAYING);
#endif
}

void AudioEngine::set_volume(float vol) {
    vol = std::clamp(vol, 0.0f, 3.0f);
    volume_ = vol;
#ifdef __ANDROID__
    if (volume_itf_) {
        // Convert to millibels: 0 = -∞, 1 = 0dB
        // OpenSL ES caps at 0 dB — keep it at max; PCM amplification handles >100%
        float capped = std::min(vol, 1.0f);
        SLmillibel mb = (capped <= 0.0f) ? SL_MILLIBEL_MIN : (SLmillibel)(2000.0f * std::log10(capped));
        (*VOL_ITF)->SetVolumeLevel(VOL_ITF, mb);
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

    (*BUFQ_ITF)->Enqueue(BUFQ_ITF, buf, buf_frames_ * spec_.channels * sizeof(int16_t));
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
