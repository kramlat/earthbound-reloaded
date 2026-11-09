#include "platform/platform.h"
#include "game_main.h"
#include "game/audio.h"

#ifdef EB_ENABLE_AUDIO

#include <SDL.h>
#include <stdatomic.h>
#include <string.h>

#define AUDIO_SAMPLE_RATE 32040   /* 534 samples per 60 Hz game frame, so one frame of video = one of audio */
#define AUDIO_BUFFER_SAMPLES 1024
#define SAMPLES_PER_FRAME 534  /* 32040 / 60 ~ 534 for NTSC */

/*
 * The SPC700/DSP is run by the game loop, one frame (534 samples) per video frame, into a ring buffer
 * that the audio callback only reads. Run by the callback, as it once was, the SPC had its own clock
 * (the audio device's) and slowly fell behind the game's. Now the two cannot drift apart: if the
 * audio device runs slower than the video, the ring fills and platform_audio_adjust_frame_period()
 * stretches the frame time a little until the buffers balance.
 */
#define RING_FRAMES 12
#define RING_SAMPLES (SAMPLES_PER_FRAME * RING_FRAMES)       /* stereo sample pairs */
#define RING_TARGET  (SAMPLES_PER_FRAME * 2 + AUDIO_BUFFER_SAMPLES)   /* fill to hold: two frames and a whole callback, so a drain never underruns */

static SDL_AudioDeviceID audio_device;
static SDL_mutex *audio_mutex;

static int16_t ring[RING_SAMPLES * 2];
static atomic_uint ring_w, ring_r;      /* monotonic counts of sample pairs written / read */

static void audio_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    int16_t *out = (int16_t *)stream;
    int total = len / (int)(2 * sizeof(int16_t));
    unsigned r = atomic_load_explicit(&ring_r, memory_order_relaxed);
    unsigned w = atomic_load_explicit(&ring_w, memory_order_acquire);
    if (game_is_fast_forward()) {                       /* silence while fast-forwarding; drop the backlog */
        memset(stream, 0, (size_t)len);
        atomic_store_explicit(&ring_r, w, memory_order_release);
        return;
    }
    int avail = (int)(w - r);
    int n = avail < total ? avail : total;
    for (int i = 0; i < n; i++) {                       /* underflow below: the rest is silence */
        unsigned idx = (r + (unsigned)i) % RING_SAMPLES;
        out[i * 2] = ring[idx * 2];
        out[i * 2 + 1] = ring[idx * 2 + 1];
    }
    if (n < total)
        memset(&out[n * 2], 0, (size_t)(total - n) * 2 * sizeof(int16_t));
    atomic_store_explicit(&ring_r, r + (unsigned)n, memory_order_release);
}

void platform_audio_produce_frame(void) {
    if (!audio_device || game_is_fast_forward())
        return;
    unsigned w = atomic_load_explicit(&ring_w, memory_order_relaxed);
    unsigned r = atomic_load_explicit(&ring_r, memory_order_acquire);
    if (RING_SAMPLES - (int)(w - r) < SAMPLES_PER_FRAME)
        return;                                         /* full: the video is ahead; pacing will slow it */
    int16_t frame_buf[SAMPLES_PER_FRAME * 2];
    audio_generate_samples(frame_buf, SAMPLES_PER_FRAME);
    for (int i = 0; i < SAMPLES_PER_FRAME; i++) {
        unsigned idx = (w + (unsigned)i) % RING_SAMPLES;
        ring[idx * 2] = frame_buf[i * 2];
        ring[idx * 2 + 1] = frame_buf[i * 2 + 1];
    }
    atomic_store_explicit(&ring_w, w + SAMPLES_PER_FRAME, memory_order_release);
}

uint64_t platform_audio_adjust_frame_period(uint64_t period) {
    if (!audio_device || game_is_fast_forward())
        return period;
    static float smooth = RING_TARGET;                  /* the callback drains in chunks: smooth its sawtooth */
    unsigned w = atomic_load_explicit(&ring_w, memory_order_acquire);
    unsigned r = atomic_load_explicit(&ring_r, memory_order_acquire);
    smooth += ((float)(int)(w - r) - smooth) * 0.02f;
    float err = (smooth - RING_TARGET) / (float)(SAMPLES_PER_FRAME * 3);   /* +: the audio has a backlog */
    if (err > 1.0f) err = 1.0f;
    if (err < -1.0f) err = -1.0f;
    return (uint64_t)((double)period * (1.0 + 0.005 * err));            /* at most half a percent either way */
}

bool platform_audio_init(void) {
    audio_mutex = SDL_CreateMutex();
    if (!audio_mutex) {
        fprintf(stderr, "audio: failed to create mutex\n");
        return false;
    }

    audio_init();

    SDL_AudioSpec want;
    SDL_memset(&want, 0, sizeof(want));
    want.freq = AUDIO_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = AUDIO_BUFFER_SAMPLES;
    want.callback = audio_callback;

    audio_device = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (audio_device == 0) {
        fprintf(stderr, "audio: failed to open audio device: %s\n", SDL_GetError());
        return false;
    }

    atomic_store(&ring_w, 0);
    atomic_store(&ring_r, 0);
    SDL_PauseAudioDevice(audio_device, 0);  /* start playback */
    return true;
}

void platform_audio_shutdown(void) {
    if (audio_device) {
        SDL_PauseAudioDevice(audio_device, 1);
        SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
    }
    audio_shutdown();
    if (audio_mutex) {
        SDL_DestroyMutex(audio_mutex);
        audio_mutex = NULL;
    }
}

void platform_audio_lock(void) {
    if (audio_mutex) SDL_LockMutex(audio_mutex);
}

void platform_audio_unlock(void) {
    if (audio_mutex) SDL_UnlockMutex(audio_mutex);
}

#else /* !EB_ENABLE_AUDIO */

bool platform_audio_init(void) { return true; }
void platform_audio_produce_frame(void) {}
uint64_t platform_audio_adjust_frame_period(uint64_t period) { return period; }
void platform_audio_shutdown(void) {}
void platform_audio_lock(void) {}
void platform_audio_unlock(void) {}

#endif /* EB_ENABLE_AUDIO */
