/*
 * audio.c - the plugin audio: sound for games. A Sound holds samples (from a
 * WAV file, or made in Lube), and each play of one is a Voice, mixed with
 * the others on a thread of its own.
 *
 *   import audio.*                       (or import audio: audio.Sound(...))
 *   let jump = Sound("jump.wav")         also Sound(bytes), Sound(samples, rate?, channels?)
 *   jump.play()                          a Voice: v.stop(), v.set_volume(0.5),
 *   let music = song.loop(0.6)           v.set_pan(-1), v.set_speed(1.2), v.is_playing()
 *   stop_all(), set_volume(x), device()
 *
 * The system's sound is what every machine has, loaded when the first sound
 * plays (so a program that plays none never touches it, and no library is
 * needed to build Lube): Audio Queue on macOS and iOS, AAudio on Android,
 * ALSA on Linux and the Raspberry Pi, waveOut on Windows. Without one (a
 * server, a machine with no sound card, or LUBE_AUDIO=none) sounds play in
 * silence: they take their time and end, as they would out loud.
 */
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L  /* nanosleep, dlopen under -std=c99 */
#endif
#include "../lube_plugin.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#elif !defined(LUBE_SILENT) && (defined(__APPLE__) || defined(__linux__))
#include <dlfcn.h>
#endif

#define FN(name) static LubeValue name(LubeVM *vm, int argc, const LubeValue *argv)
#define RATE 44100        /* what the mixer makes: frames (left, right) a second */
#define MAX_VOICES 64     /* playing at once: a new one takes the place of the oldest */
#define RAMP (1.0f / 256) /* how far a gain moves in a frame: changes take ~6 ms, without clicks */

/* ======================================================================
 * Samples, and the voices playing them
 * ==================================================================== */

typedef struct SoundData {  /* a Sound's samples: never changed, shared by its voices */
    int refs;               /* (counted under the lock: the mixer's thread lets go too) */
    int channels;           /* 1 or 2 */
    double rate;
    size_t frames;
    float *samples;         /* frames * channels, left and right in turn */
} SoundData;

typedef struct {
    SoundData *sound;       /* NULL: a free slot */
    uint32_t id;            /* which play() this is */
    uint64_t order;         /* when it started (the oldest goes first when all are busy) */
    double pos, speed;      /* where it is (in its frames) and how fast it goes */
    float volume, pan;      /* what was asked for */
    float gl, gr;           /* the gains it has now, moving toward what was asked for */
    int loop, stopping;
} Slot;

static const LubeAPI *lube;
static LubeType *SoundType, *VoiceType;
static LubeLock *lock;           /* the mixer's: made when the plugin loads, before any of it runs */
static Slot slots[MAX_VOICES];
static uint32_t last_id;
static uint64_t plays;
static float master = 1;
static int started;        /* 0, 1 while the device opens, 2 */
static const char *device = "none";

static void sound_release(SoundData *d) {  /* (under the lock) */
    if (--d->refs == 0) {
        free(d->samples);
        free(d);
    }
}

static void slot_end(Slot *s) {
    sound_release(s->sound);
    s->sound = NULL;
}

/* The gains a voice is headed for. */
static void targets(const Slot *s, float *l, float *r) {
    float v = s->stopping ? 0 : s->volume;
    *l = v * (s->pan > 0 ? 1 - s->pan : 1);
    *r = v * (s->pan < 0 ? 1 + s->pan : 1);
}

static float toward(float g, float target) {
    float d = target - g;
    return d > RAMP ? g + RAMP : d < -RAMP ? g - RAMP : target;
}

/* Fills out with frames of every voice, mixed (left, right; -1 to 1). Under the lock. */
static void mix(float *out, int frames) {
    memset(out, 0, (size_t)frames * 2 * sizeof(float));
    for (int v = 0; v < MAX_VOICES; v++) {
        Slot *s = &slots[v];
        if (!s->sound) continue;
        const SoundData *d = s->sound;
        const float *x = d->samples;
        double step = d->rate / RATE * s->speed, n = (double)d->frames;
        float tl, tr;
        targets(s, &tl, &tr);
        int done = d->frames == 0;
        for (int f = 0; f < frames && !done; f++) {
            if (s->pos >= n) {
                if (!s->loop) {
                    done = 1;
                    break;
                }
                s->pos = fmod(s->pos, n);
            }
            size_t i = (size_t)s->pos, j = i + 1 < d->frames ? i + 1 : s->loop ? 0 : i;
            float t = (float)(s->pos - (double)i), l, r;
            if (d->channels == 1) {
                l = r = x[i] + (x[j] - x[i]) * t;
            } else {
                l = x[2 * i] + (x[2 * j] - x[2 * i]) * t;
                r = x[2 * i + 1] + (x[2 * j + 1] - x[2 * i + 1]) * t;
            }
            s->gl = toward(s->gl, tl);
            s->gr = toward(s->gr, tr);
            out[2 * f] += l * s->gl;
            out[2 * f + 1] += r * s->gr;
            s->pos += step;
        }
        if (done || (s->stopping && s->gl == 0 && s->gr == 0)) slot_end(s);
    }
    for (int i = 0; i < 2 * frames; i++) {
        float y = out[i] * master;
        out[i] = y > 1 ? 1 : y < -1 ? -1 : y;
    }
}

#if !defined(LUBE_SILENT) && ((defined(__linux__) && !defined(__ANDROID__)) || defined(_WIN32))  /* (the devices that take 16-bit samples) */
static void to_s16(const float *in, short *out, int n) {
    for (int i = 0; i < n; i++) out[i] = (short)(in[i] * 32767.0f);
}
#endif

static void sleep_for(double seconds) {
#ifdef _WIN32
    Sleep((DWORD)(seconds * 1000));
#else
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    while (nanosleep(&ts, &ts) && errno == EINTR) continue;  /* (woken early by a signal: the rest) */
#endif
}

/* ======================================================================
 * The system's sound
 * ==================================================================== */

#define FRAMES 512  /* in each buffer: ~12 ms */

/* No sound: the mixer runs on time all the same, and its sound goes nowhere. */
static void silent_main(void *arg) {
    (void)arg;
    float out[FRAMES * 2];
    double next = lube->now();
    for (;;) {
        lube->lock_acquire(lock);
        mix(out, FRAMES);
        lube->lock_release(lock);
        next += (double)FRAMES / RATE;
        double wait = next - lube->now();
        if (wait > 0) sleep_for(wait);
        else if (wait < -0.25) next = lube->now();  /* LCOV_EXCL_LINE: the machine was busy (catch up, don't rush) */
    }
}

/* A function from a library opened at run time (no ISO C cast from data to code, and no type punning). */
#define LOAD(lib, var, name) load_symbol((lib), (name), &(var), sizeof(var))

#if !defined(LUBE_SILENT) && (defined(__APPLE__) || defined(__linux__))
static int load_symbol(void *lib, const char *name, void *fn, size_t size) {
    void *sym = dlsym(lib, name);
    memcpy(fn, &sym, size);
    return sym != NULL;
}
#endif

#if defined(LUBE_SILENT)
/* Built without the system's sound (-DLUBE_SILENT): a static program, which
 * cannot load it, and the coverage build, whose tests make no sound. */
#define open_device() NULL

#elif defined(__APPLE__)
/* Audio Queue (AudioToolbox): its types as the system has them, declared here
 * so that building Lube needs no SDK headers and no framework to link. */
typedef int32_t OSStatus;
typedef struct OpaqueAudioQueue *AudioQueue;
typedef struct {
    double rate;
    uint32_t format, flags, bytes_per_packet, frames_per_packet, bytes_per_frame, channels, bits, reserved;
} StreamFormat;
typedef struct {
    const uint32_t capacity;
    void *const data;
    uint32_t size;
    void *user;
    const uint32_t descriptions_capacity;
    void *const descriptions;
    uint32_t descriptions_count;
} QueueBuffer;
typedef void (*QueueFill)(void *user, AudioQueue q, QueueBuffer *b);
static OSStatus (*queue_new)(const StreamFormat *, QueueFill, void *, void *, void *, uint32_t, AudioQueue *);
static OSStatus (*queue_buffer)(AudioQueue, uint32_t, QueueBuffer **);
static OSStatus (*queue_enqueue)(AudioQueue, QueueBuffer *, uint32_t, const void *);
static OSStatus (*queue_start)(AudioQueue, const void *);

static void queue_fill(void *user, AudioQueue q, QueueBuffer *b) {  /* (on the queue's own thread) */
    (void)user;
    lube->lock_acquire(lock);
    mix((float *)b->data, FRAMES);
    lube->lock_release(lock);
    b->size = FRAMES * 2 * sizeof(float);
    queue_enqueue(q, b, 0, NULL);
}

static const char *open_device(void) {
    void *lib = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_LAZY | RTLD_LOCAL);
    if (!lib || !LOAD(lib, queue_new, "AudioQueueNewOutput") || !LOAD(lib, queue_buffer, "AudioQueueAllocateBuffer") ||
        !LOAD(lib, queue_enqueue, "AudioQueueEnqueueBuffer") || !LOAD(lib, queue_start, "AudioQueueStart"))
        return NULL;
    StreamFormat f = {RATE, 0x6C70636Du /* 'lpcm' */, 1 | 8 /* float, packed */, 8, 1, 8, 2, 32, 0};
    AudioQueue q;
    if (queue_new(&f, queue_fill, NULL, NULL, NULL, 0, &q) != 0) return NULL;
    for (int i = 0; i < 3; i++) {
        QueueBuffer *b;
        if (queue_buffer(q, FRAMES * 2 * sizeof(float), &b) != 0) return NULL;
        queue_fill(NULL, q, b);
    }
    if (queue_start(q, NULL) != 0) return NULL;
    return "Audio Queue";
}

#elif defined(__ANDROID__)
/* AAudio (Android 8 and later), libaaudio.so opened when the first sound plays;
 * an older Android plays in silence. It asks for the mix on a thread of its own. */
typedef struct AAudioStreamStruct AAudioStream;
typedef struct AAudioStreamBuilderStruct AAudioStreamBuilder;
typedef int32_t (*AAudioData)(AAudioStream *stream, void *user, void *data, int32_t frames);
typedef void (*AAudioError)(AAudioStream *stream, void *user, int32_t error);
static int32_t (*aa_builder)(AAudioStreamBuilder **builder);
static void (*aa_set_format)(AAudioStreamBuilder *b, int32_t format);
static void (*aa_set_channels)(AAudioStreamBuilder *b, int32_t channels);
static void (*aa_set_rate)(AAudioStreamBuilder *b, int32_t rate);
static void (*aa_set_performance)(AAudioStreamBuilder *b, int32_t mode);
static void (*aa_set_data)(AAudioStreamBuilder *b, AAudioData fn, void *user);
static void (*aa_set_error)(AAudioStreamBuilder *b, AAudioError fn, void *user);
static int32_t (*aa_open)(AAudioStreamBuilder *b, AAudioStream **stream);
static int32_t (*aa_builder_delete)(AAudioStreamBuilder *b);
static int32_t (*aa_start)(AAudioStream *stream);
static int32_t (*aa_close)(AAudioStream *stream);
static int32_t (*aa_rate)(AAudioStream *stream);

static int32_t aa_fill(AAudioStream *stream, void *user, void *data, int32_t frames) {  /* (on AAudio's thread) */
    (void)stream, (void)user;
    lube->lock_acquire(lock);
    mix((float *)data, frames);
    lube->lock_release(lock);
    return 0;  /* (go on) */
}

static void aa_lost(AAudioStream *stream, void *user, int32_t error);

static AAudioStream *aa_stream(void) {
    AAudioStreamBuilder *b;
    AAudioStream *s = NULL;
    if (aa_builder(&b) != 0) return NULL;
    aa_set_format(b, 2 /* float */);
    aa_set_channels(b, 2);
    aa_set_rate(b, RATE);
    aa_set_performance(b, 12 /* low latency */);
    aa_set_data(b, aa_fill, NULL);
    aa_set_error(b, aa_lost, NULL);
    if (aa_open(b, &s) != 0) s = NULL;
    aa_builder_delete(b);
    if (s && (aa_rate(s) != RATE || aa_start(s) != 0)) {
        aa_close(s);
        s = NULL;
    }
    return s;
}

/* The sound went elsewhere (headphones out, Bluetooth in): a stream to wherever it goes now. */
static void aa_reopen(void *old) {
    aa_close((AAudioStream *)old);
    for (int i = 0; i < 25; i++) {
        if (aa_stream()) return;
        sleep_for(0.2);
    }
    silent_main(NULL);  /* (none to be had: voices still end on time) */
}

static void aa_lost(AAudioStream *stream, void *user, int32_t error) {
    (void)user;
    if (error == -899 /* disconnected */) lube->thread_start(aa_reopen, stream, 1 << 20);  /* (not on AAudio's thread) */
}

static const char *open_device(void) {
    void *lib = dlopen("libaaudio.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib || !LOAD(lib, aa_builder, "AAudio_createStreamBuilder") || !LOAD(lib, aa_set_format, "AAudioStreamBuilder_setFormat") ||
        !LOAD(lib, aa_set_channels, "AAudioStreamBuilder_setChannelCount") ||
        !LOAD(lib, aa_set_rate, "AAudioStreamBuilder_setSampleRate") ||
        !LOAD(lib, aa_set_performance, "AAudioStreamBuilder_setPerformanceMode") ||
        !LOAD(lib, aa_set_data, "AAudioStreamBuilder_setDataCallback") ||
        !LOAD(lib, aa_set_error, "AAudioStreamBuilder_setErrorCallback") || !LOAD(lib, aa_open, "AAudioStreamBuilder_openStream") ||
        !LOAD(lib, aa_builder_delete, "AAudioStreamBuilder_delete") || !LOAD(lib, aa_start, "AAudioStream_requestStart") ||
        !LOAD(lib, aa_close, "AAudioStream_close") || !LOAD(lib, aa_rate, "AAudioStream_getSampleRate"))
        return NULL;
    return aa_stream() ? "AAudio" : NULL;
}

#elif defined(__linux__)
/* ALSA: libasound, which every Linux with sound has (desktop sound servers too). */
typedef struct snd_pcm snd_pcm;
static int (*pcm_open)(snd_pcm **, const char *, int, int);
static int (*pcm_set_params)(snd_pcm *, int, int, unsigned, unsigned, int, unsigned);
static long (*pcm_writei)(snd_pcm *, const void *, unsigned long);
static int (*pcm_recover)(snd_pcm *, int, int);
static snd_pcm *pcm;

static void alsa_main(void *arg) {
    (void)arg;
    float mixed[FRAMES * 2];
    short out[FRAMES * 2];
    for (;;) {
        lube->lock_acquire(lock);
        mix(mixed, FRAMES);
        lube->lock_release(lock);
        to_s16(mixed, out, FRAMES * 2);
        long n = pcm_writei(pcm, out, FRAMES);  /* (waits while the card's buffer is full) */
        if (n < 0 && pcm_recover(pcm, (int)n, 1) < 0) sleep_for((double)FRAMES / RATE);
    }
}

static const char *open_device(void) {
    void *lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!lib || !LOAD(lib, pcm_open, "snd_pcm_open") || !LOAD(lib, pcm_set_params, "snd_pcm_set_params") ||
        !LOAD(lib, pcm_writei, "snd_pcm_writei") || !LOAD(lib, pcm_recover, "snd_pcm_recover"))
        return NULL;
    if (pcm_open(&pcm, "default", 0 /* playback */, 0) < 0) return NULL;
    if (pcm_set_params(pcm, 2 /* S16_LE */, 3 /* RW_INTERLEAVED */, 2, RATE, 1, 40000 /* us */) < 0) return NULL;
    if (!lube->thread_start(alsa_main, NULL, 1 << 20)) return NULL;
    return "ALSA";
}

#elif defined(_WIN32)
/* waveOut (winmm.dll), opened at run time: programs that play nothing never load it. */
#define WAVE_BUFFERS 4
#define WAVE_FRAMES 1024  /* (waveOut wants more in hand than the others) */
typedef MMRESULT(WINAPI *WaveOpen)(LPHWAVEOUT, UINT, LPCWAVEFORMATEX, DWORD_PTR, DWORD_PTR, DWORD);
typedef MMRESULT(WINAPI *WaveHeader)(HWAVEOUT, LPWAVEHDR, UINT);
static WaveOpen wave_open;
static WaveHeader wave_prepare, wave_write;
static HWAVEOUT wave;
static HANDLE wave_done;
static WAVEHDR headers[WAVE_BUFFERS];
static short wave_data[WAVE_BUFFERS][WAVE_FRAMES * 2];

static void wave_main(void *arg) {
    (void)arg;
    float mixed[WAVE_FRAMES * 2];
    for (;;) {
        WaitForSingleObject(wave_done, INFINITE);
        for (int i = 0; i < WAVE_BUFFERS; i++) {
            if (!(headers[i].dwFlags & WHDR_DONE)) continue;
            lube->lock_acquire(lock);
            mix(mixed, WAVE_FRAMES);
            lube->lock_release(lock);
            to_s16(mixed, wave_data[i], WAVE_FRAMES * 2);
            headers[i].dwFlags &= ~(DWORD)WHDR_DONE;
            wave_write(wave, &headers[i], sizeof(WAVEHDR));
        }
    }
}

static const char *open_device(void) {
    HMODULE lib = LoadLibraryA("winmm.dll");
    if (!lib) return NULL;
    wave_open = (WaveOpen)(void (*)(void))GetProcAddress(lib, "waveOutOpen");
    wave_prepare = (WaveHeader)(void (*)(void))GetProcAddress(lib, "waveOutPrepareHeader");
    wave_write = (WaveHeader)(void (*)(void))GetProcAddress(lib, "waveOutWrite");
    if (!wave_open || !wave_prepare || !wave_write) return NULL;
    WAVEFORMATEX f;
    memset(&f, 0, sizeof f);
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 2;
    f.nSamplesPerSec = RATE;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 4;
    f.nAvgBytesPerSec = RATE * 4;
    wave_done = CreateEventA(NULL, FALSE, TRUE, NULL);
    if (!wave_done || wave_open(&wave, WAVE_MAPPER, &f, (DWORD_PTR)wave_done, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR)
        return NULL;
    for (int i = 0; i < WAVE_BUFFERS; i++) {
        memset(&headers[i], 0, sizeof headers[i]);
        headers[i].lpData = (LPSTR)wave_data[i];
        headers[i].dwBufferLength = sizeof wave_data[i];
        if (wave_prepare(wave, &headers[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) return NULL;
        headers[i].dwFlags |= WHDR_DONE;  /* (all of them to fill first) */
    }
    if (!lube->thread_start(wave_main, NULL, 1 << 20)) return NULL;
    return "waveOut";
}

#else
static const char *open_device(void) { return NULL; }
#endif

/* Starts the mixer, the first time a sound plays: the system's sound, or
 * silence. With wait, until it has started (voices need not wait: they play
 * when it has). */
static void audio_start(int wait) {
    lube->lock_acquire(lock);
    int first = started == 0;
    if (first) started = 1;
    while (!first && wait && started != 2) lube->lock_wait(lock);
    lube->lock_release(lock);
    if (!first) return;
    const char *want = getenv("LUBE_AUDIO");
    const char *name = want && !strcmp(want, "none") ? NULL : open_device();
    if (!name) lube->thread_start(silent_main, NULL, 1 << 20);
    lube->lock_acquire(lock);
    device = name ? name : "none";
    started = 2;
    lube->lock_notify(lock);
    lube->lock_release(lock);
}

/* ======================================================================
 * Sound
 * ==================================================================== */

static void *must_alloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) abort();  /* LCOV_EXCL_LINE: out of memory */
    return p;
}

static SoundData *sound_data(int channels, double rate, size_t frames) {
    SoundData *d = (SoundData *)must_alloc(sizeof(SoundData));
    d->refs = 1;
    d->channels = channels;
    d->rate = rate;
    d->frames = frames;
    d->samples = (float *)must_alloc((frames ? frames : 1) * (size_t)channels * sizeof(float));
    return d;
}

static unsigned rd16(const unsigned char *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static uint32_t rd32(const unsigned char *p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }

/* The samples of a WAV file: PCM of 8, 16, 24 or 32 bits, or floats of 32
 * or 64; mono or stereo; any rate. NULL with *why set if it is not one. */
static SoundData *parse_wav(const unsigned char *p, size_t n, const char **why) {
    if (n < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) {
        *why = "not a WAV file";
        return NULL;
    }
    unsigned format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const unsigned char *data = NULL;
    size_t data_len = 0, at = 12;
    while (at + 8 <= n) {
        size_t size = rd32(p + at + 4), room = n - at - 8;
        const unsigned char *body = p + at + 8;
        if (size > room) size = room;  /* (a file cut short: what there is) */
        if (!memcmp(p + at, "fmt ", 4) && size >= 16) {
            format = rd16(body);
            channels = rd16(body + 2);
            rate = rd32(body + 4);
            bits = rd16(body + 14);
            if (format == 0xFFFE && size >= 26) format = rd16(body + 24);  /* WAVE_FORMAT_EXTENSIBLE: its kind */
        } else if (!memcmp(p + at, "data", 4)) {
            data = body;
            data_len = size;
        }
        at += 8 + size + (size & 1);
    }
    if (!format || !data) {
        *why = "not a WAV file (no format, or no samples)";
        return NULL;
    }
    if ((format != 1 && format != 3) || (format == 1 && bits != 8 && bits != 16 && bits != 24 && bits != 32) ||
        (format == 3 && bits != 32 && bits != 64)) {
        *why = "compressed or unusual: Sound plays PCM (8, 16, 24 or 32 bits) and float WAV files";
        return NULL;
    }
    if (channels != 1 && channels != 2) {
        *why = "in more than two channels: Sound plays mono and stereo";
        return NULL;
    }
    if (rate < 1000 || rate > 384000) {
        *why = "at a sample rate Sound does not play (it plays 1000 to 384000)";
        return NULL;
    }
    size_t width = bits / 8, frames = data_len / (width * channels);
    SoundData *d = sound_data((int)channels, rate, frames);
    for (size_t i = 0; i < frames * channels; i++) {
        const unsigned char *q = data + i * width;
        float x;
        if (format == 3 && bits == 32) {
            uint32_t w = rd32(q);
            memcpy(&x, &w, 4);
        } else if (format == 3) {
            uint64_t w = rd32(q) | (uint64_t)rd32(q + 4) << 32;
            double y;
            memcpy(&y, &w, 8);
            x = (float)y;
        } else if (bits == 8) {
            x = ((float)q[0] - 128) / 128;  /* (8-bit WAV is unsigned) */
        } else if (bits == 16) {
            x = (float)(int16_t)rd16(q) / 32768;
        } else if (bits == 24) {
            int32_t w = (int32_t)((uint32_t)rd16(q) << 8 | (uint32_t)q[2] << 24) >> 8;
            x = (float)w / 8388608;
        } else {
            x = (float)((double)(int32_t)rd32(q) / 2147483648.0);
        }
        d->samples[i] = x;
    }
    return d;
}

/* What a Sound object carries: its samples (shared with the voices playing it). */
typedef struct {
    SoundData *data;
} SoundObj;

/* What a Voice object carries: where the mixer has it. */
typedef struct {
    int slot;
    uint32_t id;             /* which play() it was: the slot is someone else's once it ends */
} VoiceObj;

static void sound_obj_release(void *data) {
    SoundObj *s = (SoundObj *)data;
    if (!s->data) return;  /* LCOV_EXCL_LINE: (one made but never filled) */
    lube->lock_acquire(lock);
    sound_release(s->data);
    lube->lock_release(lock);
}

static int sound_describe(const void *data, char *buf, size_t size) {
    const SoundData *d = ((const SoundObj *)data)->data;
    char secs[64], rate[64];
    lube->format_number(round((double)d->frames / d->rate * 1000) / 1000, secs, sizeof secs);
    lube->format_number(d->rate, rate, sizeof rate);
    return snprintf(buf, size, "Sound(%s seconds, %s Hz, %s)", secs, rate, d->channels == 1 ? "mono" : "stereo");
}

static LubeValue new_sound(LubeVM *vm, SoundData *d) {
    void *data;
    LubeValue v = lube->new_foreign(vm, SoundType, &data);
    ((SoundObj *)data)->data = d;
    return v;
}

static SoundData *data_of(LubeValue v) { return ((SoundObj *)lube->foreign(v, SoundType))->data; }

/* A whole file's bytes (malloc'd), or NULL. */
static unsigned char *read_whole(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 65536, n = 0, got;
    unsigned char *data = (unsigned char *)must_alloc(cap);
    while ((got = fread(data + n, 1, cap - n, f)) > 0) {
        n += got;
        if (n == cap) {
            unsigned char *more = (unsigned char *)realloc(data, cap *= 2);
            if (!more) abort();  /* LCOV_EXCL_LINE: out of memory */
            data = more;
        }
    }
    fclose(f);
    *len = n;
    return data;
}

/* Sound(path), Sound(bytes) (a WAV file's), Sound(samples, rate = 44100,
 * channels = 1) (Numbers from -1 to 1; stereo: left, right, left, ...), or
 * Sound(): silence. */
FN(f_Sound) {
    if (argc == 0) return new_sound(vm, sound_data(1, RATE, 0));
    LubeValue v = argv[0];
    int kind = lube->kind(v);
    const char *why = NULL;
    if (kind == LUBE_STR || kind == LUBE_BYTES) {
        if (argc > 1) return lube->error(vm, "Sound() takes one argument for a WAV file (its path or its bytes)");
        SoundData *d;
        if (kind == LUBE_BYTES) {
            size_t len;
            const unsigned char *p = lube->bytes(v, &len);
            d = parse_wav(p, len, &why);
            if (!d) return lube->error(vm, "Sound(): these bytes are %s", why);
            return new_sound(vm, d);
        }
        const char *path = lube->str(v, NULL);
        size_t len;
        unsigned char *file = read_whole(path, &len);
        if (!file) return lube->error(vm, "Sound(): cannot open '%s': %s", path, strerror(errno));
        d = parse_wav(file, len, &why);
        free(file);
        if (!d) return lube->error(vm, "Sound(): '%s' is %s", path, why);
        return new_sound(vm, d);
    }
    if (kind != LUBE_LIST)
        return lube->error(vm, "Sound() expects a path, a WAV file's Bytes or a List of samples, got %s", lube->type_name(v));
    double rate = RATE, channels = 1;
    if (argc > 1) {
        rate = lube->kind(argv[1]) == LUBE_NUMBER ? lube->number(argv[1]) : 0;
        if (!(rate >= 1000 && rate <= 384000))
            return lube->error(vm, "Sound(samples, rate): the rate is samples a second, from 1000 to 384000");
    }
    if (argc > 2) {
        channels = lube->kind(argv[2]) == LUBE_NUMBER ? lube->number(argv[2]) : 0;
        if (channels != 1 && channels != 2)
            return lube->error(vm, "Sound(samples, rate, channels): channels is 1 (mono) or 2 (stereo)");
    }
    size_t count = lube->list_len(v);
    if (count % (size_t)channels)
        return lube->error(vm, "Sound(): a stereo sound's samples come in pairs (left, right), but there are %lu",
                           (unsigned long)count);
    for (size_t i = 0; i < count; i++)
        if (lube->kind(lube->list_get(v, i)) != LUBE_NUMBER)
            return lube->error(vm, "Sound(): sample %lu is %s, not a Number", (unsigned long)i, lube->a_type(lube->list_get(v, i)));
    SoundData *d = sound_data((int)channels, rate, count / (size_t)channels);
    for (size_t i = 0; i < count; i++) d->samples[i] = (float)lube->number(lube->list_get(v, i));
    return new_sound(vm, d);
}

/* A volume (0 to 16) or a pan (-1 to 1) argument: 0 after raising an error (in *err). */
static int level_of(LubeVM *vm, const char *fn, LubeValue v, float *out, LubeValue *err) {
    double x = lube->kind(v) == LUBE_NUMBER ? lube->number(v) : -1;
    if (x >= 0 && x <= 16) {
        *out = (float)x;
        return 1;
    }
    *err = lube->error(vm, "%s(): a volume is a Number from 0 (silent) to 1 (as recorded), or up to 16 (louder)", fn);
    return 0;
}

static int pan_of(LubeVM *vm, const char *fn, LubeValue v, float *out, LubeValue *err) {
    double x = lube->kind(v) == LUBE_NUMBER ? lube->number(v) : 2;
    if (x >= -1 && x <= 1) {
        *out = (float)x;
        return 1;
    }
    *err = lube->error(vm, "%s(): pan is a Number from -1 (left) through 0 (both sides) to 1 (right)", fn);
    return 0;
}

/* Where a new voice goes: a free place; or one fading out; or else the
 * oldest that would end on its own; or else the oldest of all. */
static int free_slot(void) {
    int oldest = -1, oldest_once = -1, fading = -1;
    for (int i = 0; i < MAX_VOICES; i++) {
        const Slot *s = &slots[i];
        if (!s->sound) return i;
        if (s->stopping && fading < 0) fading = i;
        if (oldest < 0 || s->order < slots[oldest].order) oldest = i;
        if (!s->loop && (oldest_once < 0 || s->order < slots[oldest_once].order)) oldest_once = i;
    }
    return fading >= 0 ? fading : oldest_once >= 0 ? oldest_once : oldest;
}

/* s.play(volume = 1, pan = 0) and s.loop(...): a Voice. */
static LubeValue start(LubeVM *vm, const char *fn, int argc, const LubeValue *argv, int loop) {
    float volume = 1, pan = 0;
    LubeValue err;
    if (argc > 1 && !level_of(vm, fn, argv[1], &volume, &err)) return err;
    if (argc > 2 && !pan_of(vm, fn, argv[2], &pan, &err)) return err;
    SoundData *d = data_of(argv[0]);
    audio_start(0);
    void *data;
    LubeValue voice = lube->new_foreign(vm, VoiceType, &data);
    lube->lock_acquire(lock);
    int pick = free_slot();
    Slot *s = &slots[pick];
    if (s->sound) slot_end(s);
    d->refs++;
    s->sound = d;
    if (++last_id == 0) last_id = 1;
    s->id = last_id;
    s->order = plays++;
    s->pos = 0;
    s->speed = 1;
    s->volume = volume;
    s->pan = pan;
    s->loop = loop;
    s->stopping = 0;
    targets(s, &s->gl, &s->gr);  /* (it starts at once: no fading in) */
    ((VoiceObj *)data)->slot = pick;
    ((VoiceObj *)data)->id = s->id;
    lube->lock_release(lock);
    return voice;
}

FN(m_play) { return start(vm, "play", argc, argv, 0); }
FN(m_loop) { return start(vm, "loop", argc, argv, 1); }

/* s.duration(): its length in seconds. */
FN(m_duration) {
    (void)vm;
    (void)argc;
    const SoundData *d = data_of(argv[0]);
    return lube->number_value((double)d->frames / d->rate);
}

FN(m_rate) {
    (void)vm;
    (void)argc;
    return lube->number_value(data_of(argv[0])->rate);
}

FN(m_channels) {
    (void)vm;
    (void)argc;
    return lube->number_value(data_of(argv[0])->channels);
}

/* s.samples(): its samples, -1 to 1 (stereo: left, right, left, ...). */
FN(m_samples) {
    (void)argc;
    const SoundData *d = data_of(argv[0]);
    size_t n = d->frames * (size_t)d->channels;
    LubeValue l = lube->new_list(vm);
    for (size_t i = 0; i < n; i++) lube->list_push(l, lube->number_value((double)d->samples[i]));
    return l;
}

static void put16(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, uint32_t v) {
    put16(p, v & 0xFFFF);
    put16(p + 2, v >> 16);
}

/* s.to_wav(): a WAV file of it (16-bit), as Bytes: write_bytes("out.wav", s.to_wav()). */
FN(m_to_wav) {
    (void)argc;
    const SoundData *d = data_of(argv[0]);
    size_t n = d->frames * (size_t)d->channels;
    if (n > (0xFFFFFFFFu - 44) / 2) return lube->error(vm, "to_wav(): the sound is too long for a WAV file");  /* LCOV_EXCL_LINE */
    LubeValue b = lube->new_bytes(vm, NULL, 44 + 2 * n);
    unsigned char *p = lube->bytes_buffer(b);
    unsigned rate = (unsigned)lround(d->rate), align = 2 * (unsigned)d->channels;
    memcpy(p, "RIFF", 4);
    put32(p + 4, (uint32_t)(36 + 2 * n));
    memcpy(p + 8, "WAVEfmt ", 8);
    put32(p + 16, 16);
    put16(p + 20, 1);  /* PCM */
    put16(p + 22, (unsigned)d->channels);
    put32(p + 24, rate);
    put32(p + 28, rate * align);
    put16(p + 32, align);
    put16(p + 34, 16);
    memcpy(p + 36, "data", 4);
    put32(p + 40, (uint32_t)(2 * n));
    for (size_t i = 0; i < n; i++) {
        float x = d->samples[i];
        x = x > 1 ? 1 : x < -1 ? -1 : x;
        put16(p + 44 + 2 * i, (unsigned)(uint16_t)(int16_t)lrintf(x * 32767));
    }
    return b;
}

/* ======================================================================
 * Voice
 * ==================================================================== */

/* The slot still playing this voice, or NULL. Under the lock. */
static Slot *slot_of(const VoiceObj *voice) {
    Slot *s = &slots[voice->slot];
    return s->sound && s->id == voice->id ? s : NULL;
}

static Slot *voice_slot(LubeValue v) { return slot_of((const VoiceObj *)lube->foreign(v, VoiceType)); }

static int voice_describe(const void *data, char *buf, size_t size) {
    lube->lock_acquire(lock);
    const Slot *s = slot_of((const VoiceObj *)data);
    int on = s && !s->stopping;
    lube->lock_release(lock);
    return snprintf(buf, size, on ? "Voice(playing)" : "Voice(ended)");
}

/* v.stop(): it fades out at once (stopping one that has ended does nothing). */
FN(m_stop) {
    (void)vm;
    (void)argc;
    lube->lock_acquire(lock);
    Slot *s = voice_slot(argv[0]);
    if (s) s->stopping = 1;
    lube->lock_release(lock);
    return lube->nothing();
}

/* v.is_playing(): not ended, and not stopped. */
FN(m_is_playing) {
    (void)vm;
    (void)argc;
    lube->lock_acquire(lock);
    Slot *s = voice_slot(argv[0]);
    int on = s && !s->stopping;
    lube->lock_release(lock);
    return lube->boolean(on);
}

/* v.set_volume(x), v.set_pan(x), v.set_speed(x): at once, without clicks. */
FN(m_set_volume) {
    (void)argc;
    float x;
    LubeValue err;
    if (!level_of(vm, "set_volume", argv[1], &x, &err)) return err;
    lube->lock_acquire(lock);
    Slot *s = voice_slot(argv[0]);
    if (s) s->volume = x;
    lube->lock_release(lock);
    return lube->nothing();
}

FN(m_set_pan) {
    (void)argc;
    float x;
    LubeValue err;
    if (!pan_of(vm, "set_pan", argv[1], &x, &err)) return err;
    lube->lock_acquire(lock);
    Slot *s = voice_slot(argv[0]);
    if (s) s->pan = x;
    lube->lock_release(lock);
    return lube->nothing();
}

/* (Faster is higher, as a tape: 2 is an octave up; 0 holds it where it is.) */
FN(m_set_speed) {
    (void)argc;
    double x = lube->kind(argv[1]) == LUBE_NUMBER ? lube->number(argv[1]) : -1;
    if (!(x >= 0 && x <= 16)) return lube->error(vm, "set_speed(): speed is a Number from 0 (held) through 1 (as recorded) to 16");
    lube->lock_acquire(lock);
    Slot *s = voice_slot(argv[0]);
    if (s) s->speed = x;
    lube->lock_release(lock);
    return lube->nothing();
}

/* ======================================================================
 * The package's functions
 * ==================================================================== */

/* stop_all(): every voice fades out. */
FN(f_stop_all) {
    (void)vm;
    (void)argc;
    (void)argv;
    lube->lock_acquire(lock);
    for (int i = 0; i < MAX_VOICES; i++)
        if (slots[i].sound) slots[i].stopping = 1;
    lube->lock_release(lock);
    return lube->nothing();
}

/* set_volume(x): of everything (1: as it is). */
FN(f_set_volume) {
    (void)argc;
    float x;
    LubeValue err;
    if (!level_of(vm, "set_volume", argv[0], &x, &err)) return err;
    lube->lock_acquire(lock);
    master = x;
    lube->lock_release(lock);
    return lube->nothing();
}

/* device(): what sounds play on ("Audio Queue", "ALSA", "waveOut", or "none":
 * silence), opening it if no sound has yet. */
FN(f_device) {
    (void)argc;
    (void)argv;
    audio_start(1);
    lube->lock_acquire(lock);
    const char *name = device;
    lube->lock_release(lock);
    return lube->new_str(vm, name, strlen(name));
}

LUBE_PLUGIN(audio) {
    lube = api;
    if (!lock) lock = api->lock_new();  /* (once: plugins load one at a time) */
    api->function(plugin, "stop_all", f_stop_all, 0, 0);
    api->function(plugin, "set_volume", f_set_volume, 1, 1);
    api->function(plugin, "device", f_device, 0, 0);
    SoundType = api->type(plugin, "Sound", sizeof(SoundObj), sound_obj_release, sound_describe);
    api->constructor(SoundType, f_Sound, 0, 3);
    api->method(SoundType, "play", m_play, 1, 3);
    api->method(SoundType, "loop", m_loop, 1, 3);
    api->method(SoundType, "duration", m_duration, 1, 1);
    api->method(SoundType, "rate", m_rate, 1, 1);
    api->method(SoundType, "channels", m_channels, 1, 1);
    api->method(SoundType, "samples", m_samples, 1, 1);
    api->method(SoundType, "to_wav", m_to_wav, 1, 1);
    VoiceType = api->type(plugin, "Voice", sizeof(VoiceObj), NULL, voice_describe);
    api->method(VoiceType, "stop", m_stop, 1, 1);
    api->method(VoiceType, "is_playing", m_is_playing, 1, 1);
    api->method(VoiceType, "set_volume", m_set_volume, 2, 2);
    api->method(VoiceType, "set_pan", m_set_pan, 2, 2);
    api->method(VoiceType, "set_speed", m_set_speed, 2, 2);
    return LUBE_PLUGIN_API;
}
