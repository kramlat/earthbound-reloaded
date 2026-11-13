/* Audio through Qt Multimedia, in the manner of the Sonic ports: a QAudioSink in push mode that
 * the game loop feeds once per frame (534 samples at 32040 Hz = one 60 Hz frame), so the SPC700
 * is clocked by the game, never by the audio device. The sink's own fill level is the feedback:
 * platform_audio_adjust_frame_period() stretches or trims the frame time by at most half a
 * percent so the sink stays near its target fill, instead of drifting into an underrun (clicks)
 * or a growing backlog (the audio lagging the picture). */
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QByteArray>
#include <QIODevice>
#include <QMediaDevices>

#include <cstdio>
#include <cstring>

extern "C" {
#include "platform/platform.h"
#include "game_main.h"
#include "game/audio.h"
}

namespace {

constexpr int kSampleRate = 32040;
constexpr int kChannels = 2;
constexpr int kSamplesPerFrame = 534;
constexpr int kFrameBytes = kSamplesPerFrame * kChannels * int(sizeof(int16_t));
constexpr int kBufferFrames = 8;     /* the sink's whole buffer */
constexpr int kTargetFrames = 3;     /* the fill the pacing aims at: slack against a late frame, little added delay */

QAudioSink *g_sink;
QIODevice *g_device;
float g_smooth = kTargetFrames * kFrameBytes;

int Buffered() {      /* bytes written but not yet played */
    return g_sink ? g_sink->bufferSize() - int(g_sink->bytesFree()) : 0;
}

}   // namespace

extern "C" {

bool platform_audio_init(void) {
    audio_init();
    QAudioFormat format;
    format.setSampleRate(kSampleRate);
    format.setChannelCount(kChannels);
    format.setSampleFormat(QAudioFormat::Int16);
    QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull() || !device.isFormatSupported(format)) {
        fprintf(stderr, "audio: no usable audio output (%s): running silent\n",
                device.isNull() ? "no device" : "32040 Hz stereo not supported");
        return false;
    }
    g_sink = new QAudioSink(device, format);
    g_sink->setBufferSize(kFrameBytes * kBufferFrames);
    g_device = g_sink->start();
    if (!g_device) {
        delete g_sink;
        g_sink = nullptr;
        return false;
    }
    g_device->write(QByteArray(kFrameBytes * kTargetFrames, 0));      /* prime: a late frame does not underrun */
    g_smooth = kTargetFrames * kFrameBytes;
    return true;
}

void platform_audio_shutdown(void) {
    if (g_sink) {
        g_sink->stop();
        delete g_sink;
        g_sink = nullptr;
        g_device = nullptr;
    }
    audio_shutdown();
}

/* No audio thread: everything runs on the game loop, so there is nothing to guard. */
void platform_audio_lock(void) {}
void platform_audio_unlock(void) {}

void platform_audio_produce_frame(void) {
    if (!g_sink || !g_device || game_is_fast_forward())
        return;
    int16_t frame[kSamplesPerFrame * kChannels];
    audio_generate_samples(frame, kSamplesPerFrame);      /* always: the music keeps its own time */
    /* A full buffer means the picture is ahead: drop this slice rather than block (the pacing
     * below slows the loop until the two balance), which drains with no discontinuity. */
    if (g_sink->bytesFree() >= kFrameBytes)
        g_device->write(reinterpret_cast<const char *>(frame), kFrameBytes);
}

uint64_t platform_audio_adjust_frame_period(uint64_t period) {
    if (!g_sink || game_is_fast_forward())
        return period;
    g_smooth += (float(Buffered()) - g_smooth) * 0.02f;     /* the device drains in chunks: smooth its sawtooth */
    float err = (g_smooth - kTargetFrames * kFrameBytes) / float(kFrameBytes * 3);   /* +: a backlog */
    if (err > 1.0f) err = 1.0f;
    if (err < -1.0f) err = -1.0f;
    return uint64_t(double(period) * (1.0 + 0.005 * err));
}

}   // extern "C"
