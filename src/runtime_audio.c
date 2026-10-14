/* libSceAudioOut on SDL3 audio streams. sceAudioOutOutput returns once per
 * buffer period on a steady clock, like PS4 hardware: FMOD's output thread
 * reads 256-frame slots out of its mixer's 512-frame ring, and returning in
 * bursts (whenever the device pulls a quantum) lets it overtake the mixer and
 * play stale halves - a click every 10.7 ms. The lowest SDL queue level of
 * each window is steered to two buffers (silence prefill on underrun, ±3%
 * cadence corrections), which also absorbs device clock drift. Without an audio device (headless runs, BB_AUDIO=none)
 * ports only follow the clock. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <SDL3/SDL.h>
#include "gpu/bbgpu.h"
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

#define PORTS 25
#define ERR_NOT_OPENED ((int32_t)0x80260001)
#define ERR_INVALID_PORT ((int32_t)0x80260003)
#define ERR_INVALID_POINTER ((int32_t)0x80260004)
#define ERR_PORT_FULL ((int32_t)0x80260005)
#define ERR_INVALID_SIZE ((int32_t)0x80260006)
#define ERR_INVALID_FORMAT ((int32_t)0x80260007)
#define ERR_INVALID_FREQ ((int32_t)0x80260008)
#define ERR_INVALID_VOLUME ((int32_t)0x80260009)
#define ERR_INVALID_TYPE ((int32_t)0x8026000A)
#define ERR_ALREADY_INIT ((int32_t)0x8026000E)
#define ERR_NOT_INIT ((int32_t)0x8026000F)
#define VOLUME_0DB 32768

typedef struct {
    int used, type, channels, is_float, frames, sample_bytes, std_layout;
    int out_channels; /* channels sent to SDL: the device's when it has fewer (folded here) */
    int32_t volume[8];
    SDL_AudioStream *stream;
    int muted; /* gain 0 while the game window is in the background (bbgpu_audio_audible) */
    uint64_t next_deadline_ns; /* next return of sceAudioOutOutput */
    int64_t adjust_ns; int window_min, window_count; /* queue level control */
    uint64_t last_output_us;
    HostMutex lock;
    FILE *dump;                     /* BB_AUDIO_DUMP: raw converted PCM per port */
    int stats;
    uint64_t stat_start_ns, stat_last_ns, stat_max_gap_ns; /* BB_AUDIO_STATS */
    unsigned stat_starved, stat_buffers; int stat_min_queued;
} Port;
typedef struct { uint16_t output; uint8_t channel, reserved; int16_t volume; uint16_t reroute; uint64_t flag, reserved64[2]; } PortState;
_Static_assert(sizeof(PortState)==32,"AudioOut port state layout");

static HostMutex table_lock = HOST_MUTEX_INIT;
static Port ports[PORTS];
static int initialized, sdl_ready=-1;
static size_t buffers_out, ports_opened;
static uint64_t g_audio_underruns=0;

uint64_t runtime_audio_get_underruns(void) { return g_audio_underruns; }

static uint64_t now_ns(void) { return host_monotonic_ns(); }
static void sleep_until(uint64_t deadline) { host_sleep_until_ns(deadline); }

static inline float soft_clip(float x) {
    /* Linear to 0.75, then a tanh knee that approaches (never reaches) full scale: continuous,
     * monotonic, |out| < 1, so the boosted loud scenes cannot clip. */
    const float knee = 0.75f, a = x < 0.0f ? -x : x;
    if (a <= knee) return x;
    const float o = knee + (1.0f - knee) * tanhf((a - knee) / (1.0f - knee));
    return x < 0.0f ? -o : o;
}

/* Channels of the default playback device, 0 when unknown. (From Supermedo/bloodborne_pc d41e3da.)
 * SDL's 7.1-to-stereo mix scales the front channels by about 0.21, so the 8-channel game mix
 * played roughly 13 dB below other PC audio on stereo devices; mono and stereo devices get a
 * fold made here with the fronts at unity (fold_frame) and the stream opened in their count. */
static int device_channels(void) {
    static int cached=-1;
    if (cached<0) {
        SDL_AudioSpec spec;
        cached=SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,NULL) &&
               spec.channels>=1 && spec.channels<=8 ? spec.channels : 0;
        printf("Runtime: audio device %d ch\n", cached);
    }
    return cached;
}

/* One frame in SDL 7.1 order (L R C LFE BL BR SL SR) folded to 1 or 2 channels. */
static void fold_frame(const float *in, int channels, float *out, int out_channels) {
    const float k=0.70710678f;
    float l=in[0], r=channels>1 ? in[1] : in[0];
    if (channels==8) {
        const float c=k*in[2]+0.5f*in[3];
        l+=c+k*in[4]+k*in[6];
        r+=c+k*in[5]+k*in[7];
    }
    if (out_channels==1) out[0]=0.5f*(l+r);
    else { out[0]=l; out[1]=r; }
}

static int sdl_audio(void) {
    if (sdl_ready<0) {
        const char *mode=getenv("BB_AUDIO");
        sdl_ready = (!mode || strcmp(mode,"none")) && SDL_InitSubSystem(SDL_INIT_AUDIO);
        printf("Runtime: audio backend %s\n", sdl_ready ? SDL_GetCurrentAudioDriver() : "timer (silent)");
    }
    return sdl_ready;
}

static int port_range(int type, int *first, int *last) {
    switch (type) {
    case 0: *first=0; *last=7; return 1;      /* main */
    case 1: *first=8; *last=8; return 1;      /* bgm */
    case 2: *first=9; *last=12; return 1;     /* voice */
    case 3: *first=13; *last=16; return 1;    /* personal */
    case 4: *first=17; *last=20; return 1;    /* pad speaker */
    case 126: *first=21; *last=22; return 1;  /* audio3d */
    case 127: *first=23; *last=24; return 1;  /* aux */
    default: return 0;
    }
}
static Port *port_of(int32_t handle, int32_t *error) {
    int id=handle & 0xff;
    if ((handle & 0x3f000000)!=0x20000000 || id>=PORTS) { *error=ERR_INVALID_PORT; return NULL; }
    if (!ports[id].used) { *error=ERR_NOT_OPENED; return NULL; }
    return &ports[id];
}

static ABI int32_t audio_init(void) {
    host_lock(&table_lock);
    int32_t r=initialized ? ERR_ALREADY_INIT : 0;
    if (!initialized) {
#ifdef _WIN32
        timeBeginPeriod(1);
#endif
    }
    initialized=1;
    host_unlock(&table_lock);
    return r;
}

static ABI int32_t audio_open(int32_t user, int32_t type, int32_t index, uint32_t length, uint32_t freq, uint32_t param) {
    (void)user; (void)index;
    if (!initialized) return ERR_NOT_INIT;
    if (!length || length>2048 || (length & 0xff)) return ERR_INVALID_SIZE;
    if (freq!=48000) return ERR_INVALID_FREQ;
    uint32_t format=param & 0xff;
    if (format>7) return ERR_INVALID_FORMAT;
    int first, last;
    if (!port_range(type,&first,&last)) return ERR_INVALID_TYPE;
    static const int channels[8]={1,2,8,1,2,8,8,8};
    host_lock(&table_lock);
    int id=-1;
    for (int i=first;i<=last;++i) if (!ports[i].used) { id=i; break; }
    if (id<0) { host_unlock(&table_lock); return ERR_PORT_FULL; }
    Port *p=&ports[id];
    memset(p,0,sizeof(*p));
    host_mutex_init(&p->lock);
    p->used=1; p->type=type; p->channels=channels[format]; p->is_float=format>=3 && format!=6;
    p->sample_bytes=p->is_float ? 4 : 2; p->frames=(int)length; p->std_layout=format>=6;
    p->out_channels=p->channels;
    for (int c=0;c<8;++c) p->volume[c]=VOLUME_0DB;
    if (sdl_audio()) {
        const int device=device_channels();
        if (device>=1 && device<=2 && device<p->channels) p->out_channels=device;
        SDL_AudioSpec spec={p->is_float ? SDL_AUDIO_F32 : SDL_AUDIO_S16, p->out_channels, 48000};
        p->stream=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,NULL,NULL);
        if (p->stream) {
            SDL_ResumeAudioStreamDevice(p->stream);
            /* Prefill buffer periods of silence to give initial playback headroom (configurable via BB_AUDIO_PREFILL, default 2) */
            int prefill_count = 2;
            const char *env_prefill = getenv("BB_AUDIO_PREFILL");
            if (env_prefill && *env_prefill) {
                int pf = atoi(env_prefill);
                if (pf >= 1 && pf <= 8) prefill_count = pf;
            }
            unsigned char silence[2048*8*4]={0};
            size_t prefill_bytes=(size_t)p->frames*(size_t)p->out_channels*(size_t)p->sample_bytes*(size_t)prefill_count;
            if (prefill_bytes<=sizeof(silence)) SDL_PutAudioStreamData(p->stream,silence,(int)prefill_bytes);
        } else {
            fprintf(stderr,"Runtime: SDL audio stream failed (%s); port %d uses the timer sink\n",SDL_GetError(),id);
        }
    }
    p->stats=getenv("BB_AUDIO_STATS")!=NULL;
    const char *dump=getenv("BB_AUDIO_DUMP");
    if (dump) {
        char path[4096];
        snprintf(path,sizeof(path),"%s.port%d.%dch.%s",dump,id,p->out_channels,p->is_float ? "f32" : "s16");
        p->dump=fopen(path,"wb");
    }
    ++ports_opened;
    host_unlock(&table_lock);
    printf("Runtime: audio port %d opened (type %d, %d ch, %s, %u frames)\n",id,type,p->channels,p->is_float ? "float" : "s16",length);
    return (type<<16) | id | 0x20000000;
}

static ABI int32_t audio_close(int32_t handle) {
    host_lock(&table_lock);
    int32_t error=0;
    Port *p=port_of(handle,&error);
    if (p) {
        host_lock(&p->lock);
        if (p->stream) SDL_DestroyAudioStream(p->stream);
        if (p->dump) fclose(p->dump);
        p->dump=NULL;
        p->stream=NULL; p->used=0;
        host_unlock(&p->lock);
    }
    host_unlock(&table_lock);
    return p ? 0 : error;
}

/* `pace`: wait for this port's next period. sceAudioOutOutputs waits once for all its ports. */
static int32_t output_port(int32_t handle, const void *data, int pace) {
    int32_t error=0;
    host_lock(&table_lock);
    Port *p=port_of(handle,&error);
    host_unlock(&table_lock);
    if (!p) return error;
#ifdef _WIN32
    static _Thread_local int s_audio_prio_set=0;
    if (!s_audio_prio_set) {
        /* Use THREAD_PRIORITY_HIGHEST: avoids starvation of Haswell 4C/4T CPU while maintaining low latency */
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        s_audio_prio_set=1;
    }
#endif
    host_lock(&p->lock);
    size_t samples=(size_t)p->frames*(size_t)p->channels; /* the guest's count */
    size_t bytes=(size_t)p->frames*(size_t)p->out_channels*(size_t)p->sample_bytes;
    uint64_t period=(uint64_t)p->frames*1000000000u/48000u;
    if (data) {
        /* Apply per-channel volume, channel remap, and soft-knee saturation to prevent harsh clipping */
        unsigned char converted[2048*8*4];
        static const int remap[8]={0,1,2,3,6,7,4,5};
        float master_gain=((p->channels==8) ? 0.60f : 1.0f) * bbgpu_audio_boost();
        const int out_channels=p->out_channels;
        for (size_t f=0;f<(size_t)p->frames;++f) {
            float frame[8];
            for (int c=0;c<p->channels;++c) {
                int target=p->channels==8 && !p->std_layout ? remap[c] : c;
                size_t from=f*(size_t)p->channels+(size_t)c;
                float gain=((float)p->volume[c]/VOLUME_0DB)*master_gain;
                if (p->is_float) {
                    float v; memcpy(&v,(const char *)data+from*4,4);
                    frame[target]=v*gain;
                } else {
                    int16_t v; memcpy(&v,(const char *)data+from*2,2);
                    frame[target]=((float)v/32768.0f)*gain;
                }
            }
            float folded[2];
            const float *out=frame;
            if (out_channels!=p->channels) { fold_frame(frame,p->channels,folded,out_channels); out=folded; }
            for (int c=0;c<out_channels;++c) {
                const float v=soft_clip(out[c]);
                size_t to=f*(size_t)out_channels+(size_t)c;
                if (p->is_float) {
                    memcpy(converted+to*4,&v,4);
                } else {
                    int32_t iv=(int32_t)(v*32767.0f);
                    if (iv>32767) iv=32767; else if (iv<-32768) iv=-32768;
                    int16_t sv=(int16_t)iv;
                    memcpy(converted+to*2,&sv,2);
                }
            }
        }
        if (p->dump) fwrite(converted,1,bytes,p->dump);

        if (pace) {
            uint64_t now=now_ns();
            if (!p->next_deadline_ns || now>p->next_deadline_ns+8*period) p->next_deadline_ns=now; /* start or stall: restart cadence */
            else sleep_until(p->next_deadline_ns);
            p->next_deadline_ns+=period;
        }

        if (p->stream) {
            /* Steer the SDL queue against hardware device rate. Prefill silence only when the
             * stream has fully drained: zeros put after samples that are still queued splice a
             * gap into the waveform and pop (from Mrsuss60/bloodborne_pc_windows_port 367480b). */
            int low=2*(int)bytes;
            int queued=SDL_GetAudioStreamQueued(p->stream);
            if (queued==0) {
                if (buffers_out>0) ++g_audio_underruns;
                static const unsigned char silence[2048*8*4];
                if ((int)bytes<=(int)sizeof(silence)) {
                    SDL_PutAudioStreamData(p->stream,silence,(int)bytes);
                }
                queued=low;
            }
            if (!p->window_count || queued<p->window_min) p->window_min=queued;
            if (++p->window_count>=32) {
                p->adjust_ns=p->window_min>low+2*(int)bytes ? (int64_t)(period/32) :
                             p->window_min<low ? -(int64_t)(period/32) : 0;
                p->window_count=0;
            }
            p->next_deadline_ns+=(uint64_t)p->adjust_ns;
            /* Mute instead of pausing: the queue keeps its pacing while the window is unfocused. */
            const int muted=!bbgpu_audio_audible();
            if (muted!=p->muted) { p->muted=muted; SDL_SetAudioStreamGain(p->stream,muted ? 0.0f : 1.0f); }
            SDL_PutAudioStreamData(p->stream,converted,(int)bytes);
        }

        if (p->stats) {
            uint64_t now=now_ns();
            if (!p->stat_start_ns) { p->stat_start_ns=now; p->stat_min_queued=1<<30; }
            else if (now-p->stat_last_ns>p->stat_max_gap_ns) p->stat_max_gap_ns=now-p->stat_last_ns;
            p->stat_last_ns=now; ++p->stat_buffers;
            if (p->stream) {
                int queued=SDL_GetAudioStreamQueued(p->stream);
                if (queued<p->stat_min_queued) p->stat_min_queued=queued;
                if (queued==0) ++p->stat_starved;
            }
            if (now-p->stat_start_ns>=5000000000u) {
                printf("Audio stats port %d: %u buffers/5s (expected %u), max gap %.2f ms, min queued %d bytes, starved %u\n",
                       (int)(p-ports),p->stat_buffers,(unsigned)(5u*48000u/(unsigned)p->frames),p->stat_max_gap_ns/1e6,
                       p->stat_min_queued,p->stat_starved);
                p->stat_start_ns=now; p->stat_max_gap_ns=0; p->stat_starved=0; p->stat_buffers=0; p->stat_min_queued=1<<30;
            }
        }
        ++buffers_out;
    }
    p->last_output_us=now_ns()/1000;
    host_unlock(&p->lock);
    return data ? (int32_t)samples : 0;
}

static ABI int32_t audio_output(int32_t handle, const void *data) { return output_port(handle,data,1); }
typedef struct { int32_t handle; const void *data; } OutputParam;
static ABI int32_t audio_outputs(const OutputParam *params, uint32_t count) {
    if (!params) return ERR_INVALID_POINTER;
    if (!count || count>PORTS) return ERR_PORT_FULL;
    int32_t result=0;
    /* One period per call for all ports (FMOD feeds main and BGM together): pacing every
       port would halve the rate and starve both. */
    for (uint32_t i=0;i<count;++i) { int32_t r=output_port(params[i].handle,params[i].data,i==0); if (r<0) return r; result=r; }
    return result;
}

static ABI int32_t audio_volume(int32_t handle, int32_t flags, const int32_t *volume) {
    int32_t error=0;
    host_lock(&table_lock);
    Port *p=port_of(handle,&error);
    host_unlock(&table_lock);
    if (!p) return error;
    if (!volume) return ERR_INVALID_POINTER;
    host_lock(&p->lock);
    for (int c=0;c<8;++c) if (flags & (1<<c)) {
        if (volume[c]<0 || volume[c]>VOLUME_0DB) { host_unlock(&p->lock); return ERR_INVALID_VOLUME; }
        p->volume[c]=volume[c];
    }
    host_unlock(&p->lock);
    return 0;
}

static ABI int32_t audio_state(int32_t handle, PortState *state) {
    int32_t error=0;
    host_lock(&table_lock);
    Port *p=port_of(handle,&error);
    host_unlock(&table_lock);
    if (!p) return error;
    if (!state) return ERR_INVALID_POINTER;
    memset(state,0,sizeof(*state));
    switch (p->type) {
    case 2: case 3: state->output=0x40; state->channel=1; break;          /* headphone */
    case 4: state->output=0x04; state->channel=1; state->volume=127; break; /* pad speaker */
    default: state->output=0x01; state->channel=(uint8_t)(p->channels>2 ? 2 : p->channels); break;
    }
    return 0;
}

static ABI int32_t audio_last_time(int32_t handle, uint64_t *time) {
    int32_t error=0;
    host_lock(&table_lock);
    Port *p=port_of(handle,&error);
    host_unlock(&table_lock);
    if (!p) return error;
    if (!time) return ERR_INVALID_POINTER;
    *time=p->last_output_us; return 0;
}

static const RuntimeExport exports[]={
    {"sceAudioOutInit",audio_init}, {"sceAudioOutOpen",audio_open}, {"sceAudioOutClose",audio_close},
    {"sceAudioOutOutput",audio_output}, {"sceAudioOutOutputs",audio_outputs},
    {"sceAudioOutSetVolume",audio_volume}, {"sceAudioOutGetPortState",audio_state},
    {"sceAudioOutGetLastOutputTime",audio_last_time},
};
uintptr_t runtime_audio_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_audio_report(void) { printf("Runtime: audio ports opened=%zu, buffers output=%zu\n",ports_opened,buffers_out); }
