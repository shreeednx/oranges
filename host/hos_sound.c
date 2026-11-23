/* The sound device (SDL3): what nn_audio.c's renderer mixes, played on the default output.

   hos_sound_init runs on the process's main thread before the game (SDL wants its subsystems started there) and
   decides whether there is sound: HOS_AUDIO=0 never, HOS_AUDIO=1 always, unset: when there is a window (headless test
   runs stay quiet, several of them run at once). hos_sound_play takes each mixed audio frame from the renderer thread
   and opens the device stream at the first one.

   Two clocks. The mixer renders on the host's clock (a frame every 5 ms) and the device plays on its own, so the queue
   between them would grow or drain by the difference (tens of ppm: a second an hour or so). The stream's frequency
   ratio follows the queue's average length to a target of the device's buffer and a half plus 20 ms (enough that a
   late mixer wake does not let it run dry): the correction is a fraction of a percent, too little to hear. What a
   ratio cannot follow is a stall (the mixer or the whole process held up): the device runs dry, and the mixer then
   renders the frames it owed back to back. The queue is filled with silence back to the target when it is found
   empty, and the frames that come while it holds 40 ms more than the target are dropped (it peaks at about 15 ms more
   otherwise), so the sound does not stay behind the picture. */
#include "hos.h"

#include <stdlib.h>
#include <string.h>
#ifdef HOS_HAVE_SDL3
#include <SDL3/SDL.h>
#endif

static int sound_on;   /* (hos_sound_init: the device may be opened; cleared when it can't be) */

void hos_sound_init(int window) {
    const char *s = getenv("HOS_AUDIO");
    const int want = s && *s ? strcmp(s, "0") != 0 : window;
#ifdef HOS_HAVE_SDL3
    if (!want) {
        hos_log("sound: off (%s)", s && *s ? "HOS_AUDIO=0" : "no window; HOS_AUDIO=1 plays anyway");
        return;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        hos_log("sound: none (%s)", SDL_GetError());
        return;
    }
    sound_on = 1;
#else
    if (want) hos_log("sound: none (built without SDL3)");
#endif
}

#ifdef HOS_HAVE_SDL3
static SDL_AudioStream *stream;
static int frame_bytes, rate, target;   /* target: the queue's length to keep, in sample frames */
static float ratio = 1;
static double average;                  /* the queue's length, averaged over ~64 audio frames */
static int dry, dropped;

static int sound_open(int channels, int frequency) {
    const SDL_AudioSpec spec = {SDL_AUDIO_S16, channels, frequency};
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!stream) {
        hos_log("sound: no device (%s)", SDL_GetError());
        return 0;
    }
    SDL_AudioSpec device;
    int buffer = 0;
    if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &device, &buffer) || buffer <= 0 ||
        device.freq <= 0)
        device.freq = frequency, buffer = 1024;
    frame_bytes = 2 * channels, rate = frequency;
    target = (int)((int64_t)buffer * frequency / device.freq) * 3 / 2 + frequency / 50;
    SDL_ResumeAudioStreamDevice(stream);
    const char *name = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(stream));
    hos_log("sound: \"%s\" (%s, %d Hz, %d channels, a %d-frame buffer): %d channels at %d Hz in, %.0f ms queued",
            name ? name : "?", SDL_GetCurrentAudioDriver(), device.freq, device.channels, buffer, channels, frequency,
            target * 1000.0 / frequency);
    return 1;
}

static void fill_silence(int frames) {   /* (to the target, ahead of a frame of that many) */
    static const int16_t zero[4096];
    for (int n = (target - frames) * frame_bytes; n > 0; n -= (int)sizeof zero)
        SDL_PutAudioStreamData(stream, zero, n < (int)sizeof zero ? n : (int)sizeof zero);
    average = target;
}

void hos_sound_play(const int16_t *pcm, int frames, int channels, int frequency) {
    if (!sound_on) return;
    if (!stream && !sound_open(channels, frequency)) { sound_on = 0; return; }
    const int queued = SDL_GetAudioStreamQueued(stream) / frame_bytes;
    if (queued <= 0) {   /* (the first time: the start) */
        if (average > 0 && (++dry & (dry - 1)) == 0) hos_log("sound: the device ran dry (%d times)", dry);
        fill_silence(frames);
    } else if (queued > target + rate / 25) {   /* (the mixer catching up: frames back to back) */
        if ((++dropped & (dropped - 1)) == 0) hos_log("sound: %d audio frames dropped (a stall)", dropped);
        return;
    } else {
        average += (queued - average) / 64;
    }
    SDL_PutAudioStreamData(stream, pcm, frames * frame_bytes);
    /* (above the target, play a little faster: 0.25% per 10 ms over, at most 0.5%) */
    float want = 1 + (float)((average - target) / rate) * 0.25f;
    want = want > 1.005f ? 1.005f : want < 0.995f ? 0.995f : want;
    if (want - ratio > 0.00002f || ratio - want > 0.00002f) {
        ratio = want;
        SDL_SetAudioStreamFrequencyRatio(stream, ratio);
    }
}
#else
void hos_sound_play(const int16_t *pcm, int frames, int channels, int frequency) {
    (void)pcm, (void)frames, (void)channels, (void)frequency;
}
#endif
