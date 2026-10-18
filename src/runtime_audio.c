/* libSceAudioOut on SDL3 audio streams. sceAudioOutOutput returns once per
 * buffer period on a steady clock, like PS4 hardware: FMOD's output thread
 * reads 256-frame slots out of its mixer's 512-frame ring, and returning in
 * bursts (whenever the device pulls a quantum) lets it overtake the mixer and
 * play stale halves - a click every 10.7 ms. The lowest SDL queue level of
 * each window is steered to an adaptive cushion (two device periods, grown on underrun and
 * shrunk back after a clean stretch; silence refill on underrun, ±3% cadence corrections), which
 * also absorbs device clock drift. Without an audio device (headless runs, BB_AUDIO=none)
 * ports only follow the clock. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdarg.h>
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
    /* Steered queue floor (cushion): base..max, grown on underrun, shrunk after CUSHION_DECAY_NS clean. */
    int low_bytes, base_bytes, max_bytes, adaptive;
    uint64_t cushion_ns; /* last underrun or change of low_bytes */
    uint64_t last_output_us;
    HostMutex lock;
    FILE *dump;                     /* BB_AUDIO_DUMP: raw converted PCM per port */
    int stats;
    uint64_t stat_start_ns, stat_last_ns, stat_max_gap_ns; /* BB_AUDIO_STATS */
    unsigned stat_starved, stat_dropped, stat_buffers; int stat_min_queued;
    /* BB_AUDIO_LOG (launcher: Advanced / Debug > Audio log): a summary per MON_SUMMARY_NS and
     * event lines (late buffer, underrun, repeated buffer), at most one per s, for reports. */
    int mon;
    uint64_t mon_in_ns, mon_ret_ns, mon_start_ns, mon_log_ns, mon_hash;
    uint64_t mon_max_gap_ns, mon_max_guest_ns, mon_max_over_ns;
    unsigned mon_buffers, mon_late, mon_underruns, mon_repeats, mon_dropped, mon_quiet; int mon_min_queued;
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

/* Queue `count` bytes of silence (in chunks: a cushion can exceed one game buffer). */
static void put_silence(SDL_AudioStream *stream, int count) {
    static const unsigned char silence[2048*8*4];
    while (count>0) {
        const int n=count<(int)sizeof(silence) ? count : (int)sizeof(silence);
        SDL_PutAudioStreamData(stream,silence,n);
        count-=n;
    }
}

#define CUSHION_MAX_FRAMES 9600              /* 200 ms */
#define CUSHION_DROP_FRAMES 9600             /* a buffer is dropped this far over the floor */
#define CUSHION_DECAY_NS 20000000000ull      /* one buffer off the floor per 20 s without an underrun */

/* Starting frames of the steered queue floor: two game buffers or two device periods, whichever
 * is more (two buffers alone, 10.7 ms, sat under one ~10 ms shared-mode WASAPI period). It grows
 * only on a machine that underruns, so others keep the low latency. BB_AUDIO_CUSHION_MS (10-200)
 * fixes it instead (*fixed = 1). */
static int cushion_frames(int buffer_frames, int *fixed) {
    int frames=0;
    const char *env=getenv("BB_AUDIO_CUSHION_MS");
    *fixed=0;
    if (env && *env) { const int v=atoi(env); if (v>=10 && v<=200) { frames=v*48; *fixed=1; } }
    int device_frames=0;
    SDL_AudioSpec spec={0};
    if (!SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,&device_frames) || device_frames<0 || device_frames>16384) device_frames=0;
    /* The period is in the device's frames: at 44.1 or 192 kHz (HDMI often) it is shorter or
     * longer in our 48 kHz frames (idea from Supermedo/bloodborne_pc PR #80, andrecabralgit). */
    const int period_48k=spec.freq>0 ? (int)((int64_t)device_frames*48000/spec.freq) : device_frames;
    static int logged;
    if (!logged++)
        printf("Runtime: audio device %d Hz, %d ch, period %d frames (%.1f ms), driver %s\n",
               spec.freq,spec.channels,device_frames,period_48k/48.0,SDL_GetCurrentAudioDriver());
    if (frames<2*buffer_frames) frames=2*buffer_frames;
    if (frames<2*period_48k) frames=2*period_48k;
    if (frames>CUSHION_MAX_FRAMES) frames=CUSHION_MAX_FRAMES;
    return (frames+buffer_frames-1)/buffer_frames*buffer_frames; /* whole buffers */
}
static void sleep_until(uint64_t deadline) { host_sleep_until_ns(deadline); }

#define MON_SUMMARY_NS 60000000000ull
/* One monitor event line, at most one per second per port; the skipped ones are counted into the next. */
static void mon_event(Port *p, uint64_t now, const char *fmt, ...) {
    if (p->mon_log_ns && now-p->mon_log_ns<1000000000u) { ++p->mon_quiet; return; }
    va_list ap; va_start(ap,fmt);
    printf("Runtime: audio port %d ",(int)(p-ports));
    vprintf(fmt,ap);
    va_end(ap);
    if (p->mon_quiet) printf(" (+%u more events in the last second)",p->mon_quiet);
    printf("\n");
    p->mon_log_ns=now; p->mon_quiet=0;
}

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
            int fixed;
            const int frame_bytes=p->out_channels*p->sample_bytes, cushion=cushion_frames(p->frames,&fixed);
            p->low_bytes=p->base_bytes=cushion*frame_bytes;
            p->max_bytes=fixed ? p->low_bytes : CUSHION_MAX_FRAMES/p->frames*p->frames*frame_bytes;
            p->adaptive=!fixed; p->cushion_ns=now_ns();
            /* Prefill the cushion (BB_AUDIO_PREFILL buffers when more, 1-8) so steering starts at its floor. */
            int prefill=p->low_bytes;
            const char *env_prefill=getenv("BB_AUDIO_PREFILL");
            if (env_prefill && *env_prefill) {
                const int pf=atoi(env_prefill);
                if (pf>=1 && pf<=8 && pf*p->frames*frame_bytes>prefill) prefill=pf*p->frames*frame_bytes;
            }
            put_silence(p->stream,prefill);
            printf("Runtime: audio port %d cushion %d frames (%.1f ms, %s)\n",id,cushion,cushion/48.0,fixed ? "fixed" : "adaptive");
        } else {
            fprintf(stderr,"Runtime: SDL audio stream failed (%s); port %d uses the timer sink\n",SDL_GetError(),id);
        }
    }
    p->stats=getenv("BB_AUDIO_STATS")!=NULL;
    { const char *log=getenv("BB_AUDIO_LOG"); p->mon=log && *log && strcmp(log,"0"); }
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
        /* Monitor: time since the last buffer, and how much of it the guest spent between our
         * return and this call (FMOD late: its thread preempted or waiting on the mixer). */
        const uint64_t in_ns=now_ns();
        const uint64_t gap_ns=p->mon_in_ns ? in_ns-p->mon_in_ns : 0, guest_ns=p->mon_ret_ns ? in_ns-p->mon_ret_ns : 0;
        p->mon_in_ns=in_ns;
        if (!p->mon_start_ns) { p->mon_start_ns=in_ns; p->mon_min_queued=1<<30; }
        /* A non-silent buffer equal to the last one is FMOD replaying a stale slot (mixer behind). */
        uint64_t hash=1469598103934665603ull; int nonzero=0;
        if (p->mon) for (size_t i=0,n=samples*(size_t)p->sample_bytes;i<n;++i) {
            const unsigned char b=((const unsigned char *)data)[i];
            nonzero|=b; hash=(hash^b)*1099511628211ull;
        }
        const int repeated=nonzero && hash==p->mon_hash;
        p->mon_hash=hash;
        uint64_t over_ns=0;
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
            else { sleep_until(p->next_deadline_ns); const uint64_t woke=now_ns(); over_ns=woke-p->next_deadline_ns; }
            p->next_deadline_ns+=period;
        }

        int queued_before=-1; /* SDL queue before this buffer, for BB_AUDIO_STATS */
        if (p->stream) {
            /* Steer the SDL queue against hardware device rate. Refill the cushion with silence
             * only when the stream has fully drained: zeros put after samples that are still queued
             * splice a gap into the waveform and pop (from Mrsuss60/bloodborne_pc_windows_port 367480b). */
            const uint64_t now=now_ns();
            int queued=SDL_GetAudioStreamQueued(p->stream);
            queued_before=queued;
            if (queued==0 && buffers_out>0) {
                ++g_audio_underruns;
                if (p->adaptive && p->low_bytes<p->max_bytes) { /* this machine needs more: +50%, at least 2 buffers */
                    const int step=p->low_bytes/2>2*(int)bytes ? p->low_bytes/2 : 2*(int)bytes;
                    p->low_bytes=(p->low_bytes+step+(int)bytes-1)/(int)bytes*(int)bytes;
                    if (p->low_bytes>p->max_bytes) p->low_bytes=p->max_bytes;
                    printf("Runtime: audio port %d cushion grown to %.1f ms\n",(int)(p-ports),(double)p->low_bytes/(double)bytes*(double)period/1e6);
                }
                p->cushion_ns=now;
            } else if (p->adaptive && p->low_bytes>p->base_bytes && now-p->cushion_ns>CUSHION_DECAY_NS) {
                p->low_bytes-=(int)bytes; p->cushion_ns=now; /* steering drains the extra buffer */
                if (p->low_bytes<=p->base_bytes) {
                    p->low_bytes=p->base_bytes;
                    printf("Runtime: audio port %d cushion back to %.1f ms\n",(int)(p-ports),(double)p->low_bytes/(double)bytes*(double)period/1e6);
                }
            }
            const int low=p->low_bytes;
            if (queued==0) { put_silence(p->stream,low); queued=low; }
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
            /* Far over the floor (device clock slower than ours, or a burst after a stall): drop
             * this one buffer rather than flushing the queue. */
            if (queued>low+CUSHION_DROP_FRAMES*p->out_channels*p->sample_bytes) { ++p->stat_dropped; ++p->mon_dropped; }
            else SDL_PutAudioStreamData(p->stream,converted,(int)bytes);
        }

        if (p->mon) {
            const uint64_t now=now_ns();
            const double ms_per_byte=1.0/(48.0*p->out_channels*p->sample_bytes);
            const double queue_ms=queued_before>=0 ? queued_before*ms_per_byte : -1.0, cushion_ms=p->low_bytes*ms_per_byte;
            ++p->mon_buffers;
            if (pace && gap_ns>p->mon_max_gap_ns) p->mon_max_gap_ns=gap_ns;
            if (pace && guest_ns>p->mon_max_guest_ns) p->mon_max_guest_ns=guest_ns;
            if (over_ns>p->mon_max_over_ns) p->mon_max_over_ns=over_ns;
            if (queued_before>=0 && queued_before<p->mon_min_queued) p->mon_min_queued=queued_before;
            if (pace && gap_ns>2*period) {
                ++p->mon_late;
                mon_event(p,now,"late buffer: %.1f ms since the last (period %.1f ms; guest %.1f ms from our return to its call, our sleep overshoot %.2f ms), queue before %.1f ms, cushion %.1f ms",
                          gap_ns/1e6,period/1e6,guest_ns/1e6,over_ns/1e6,queue_ms,cushion_ms);
            }
            if (queued_before==0 && buffers_out>0) {
                ++p->mon_underruns;
                mon_event(p,now,"underrun: the device ran out of sound (%.1f ms since the last buffer, guest %.1f ms), cushion now %.1f ms",
                          gap_ns/1e6,guest_ns/1e6,cushion_ms);
            }
            if (repeated) {
                ++p->mon_repeats;
                mon_event(p,now,"repeated buffer: the guest sent the same %d frames twice (FMOD mixer behind?)",p->frames);
            }
            if (now-p->mon_start_ns>=MON_SUMMARY_NS) {
                printf("Runtime: audio port %d last %.0f s: %u buffers (expected %u), %u late (max gap %.1f ms, max guest %.1f ms, max sleep overshoot %.2f ms), "
                       "%u underruns, %u repeated, %u dropped, min queue %.1f ms, cushion %.1f ms\n",
                       (int)(p-ports),(now-p->mon_start_ns)/1e9,p->mon_buffers,(unsigned)((now-p->mon_start_ns)/period),p->mon_late,
                       p->mon_max_gap_ns/1e6,p->mon_max_guest_ns/1e6,p->mon_max_over_ns/1e6,p->mon_underruns,p->mon_repeats,p->mon_dropped,
                       p->mon_min_queued<(1<<30) ? p->mon_min_queued*ms_per_byte : -1.0,cushion_ms);
                p->mon_start_ns=now; p->mon_buffers=p->mon_late=p->mon_underruns=p->mon_repeats=p->mon_dropped=0;
                p->mon_max_gap_ns=p->mon_max_guest_ns=p->mon_max_over_ns=0; p->mon_min_queued=1<<30;
            }
        }

        if (p->stats) {
            uint64_t now=now_ns();
            if (!p->stat_start_ns) { p->stat_start_ns=now; p->stat_min_queued=1<<30; }
            else if (now-p->stat_last_ns>p->stat_max_gap_ns) p->stat_max_gap_ns=now-p->stat_last_ns;
            p->stat_last_ns=now; ++p->stat_buffers;
            /* Sampled before this buffer is put: after it the queue can never read 0. */
            if (queued_before>=0) {
                if (queued_before<p->stat_min_queued) p->stat_min_queued=queued_before;
                if (queued_before==0) ++p->stat_starved;
            }
            if (now-p->stat_start_ns>=5000000000u) {
                const double frame_ms=1.0/(48.0*p->out_channels*p->sample_bytes); /* ms per queued byte */
                printf("Audio stats port %d: %u buffers/5s (expected %u), max gap %.2f ms, min queued %d bytes (%.1f ms, floor %.1f ms), starved %u, dropped %u\n",
                       (int)(p-ports),p->stat_buffers,(unsigned)(5u*48000u/(unsigned)p->frames),p->stat_max_gap_ns/1e6,
                       p->stat_min_queued,p->stat_min_queued*frame_ms,p->low_bytes*frame_ms,p->stat_starved,p->stat_dropped);
                p->stat_start_ns=now; p->stat_max_gap_ns=0; p->stat_starved=0; p->stat_dropped=0; p->stat_buffers=0; p->stat_min_queued=1<<30;
            }
        }
        ++buffers_out;
        p->mon_ret_ns=now_ns();
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
