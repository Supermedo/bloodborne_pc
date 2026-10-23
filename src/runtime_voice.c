#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <opus.h>

#define VOICE_ARGUMENT ((int32_t)0x8029000b)
#define AUDIO_IN_NOT_OPENED ((int32_t)0x80260109)

#define RATE 16000
#define FRAME 320
#define MAX_PACKET 400
#define MAX_PEERS 8

int bbgpu_voice_send(const uint8_t *data, size_t size);
int bbgpu_voice_peers(void);

typedef struct { uint64_t key; OpusDecoder *decoder; SDL_AudioStream *speaker; uint64_t last; } Peer;

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static int enabled=-1, sdl_ready=-1, gate_level, hangover, next_port=1;
static float play_gain=1.f;
static SDL_AudioStream *mic;
static OpusEncoder *encoder;
static Peer peers[MAX_PEERS];
static uint64_t sent, received, last_heard;
static int last_peers;

static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u; }

static int voice_enabled(void) {
    if (enabled<0) {
        const char *v=getenv("BB_VOICE");
        enabled=!v || v[0]!='0';
        const char *g=getenv("BB_VOICE_GATE");
        gate_level=g ? atoi(g) : 250;
        const char *p=getenv("BB_VOICE_VOLUME");
        play_gain=p ? (float)atof(p) : 1.f;
        printf("Voice: %s (gate %d, volume %.2f)\n",enabled ? "on" : "off (BB_VOICE=0)",gate_level,play_gain);
    }
    return enabled;
}

static int audio_ready(void) {
    if (sdl_ready<0) {
        sdl_ready=SDL_InitSubSystem(SDL_INIT_AUDIO) ? 1 : 0;
        if (!sdl_ready) fprintf(stderr,"Voice: SDL audio failed: %s\n",SDL_GetError());
    }
    return sdl_ready;
}

static int open_mic(void) {
    if (mic) return 1;
    if (!audio_ready()) return 0;
    SDL_AudioSpec spec={SDL_AUDIO_S16,1,RATE};
    mic=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING,&spec,NULL,NULL);
    if (!mic) { fprintf(stderr,"Voice: no microphone (%s)\n",SDL_GetError()); return 0; }
    SDL_ResumeAudioStreamDevice(mic);
    int error=0;
    encoder=opus_encoder_create(RATE,1,OPUS_APPLICATION_VOIP,&error);
    if (!encoder) { fprintf(stderr,"Voice: opus encoder failed (%d)\n",error); SDL_DestroyAudioStream(mic); mic=NULL; return 0; }
    opus_encoder_ctl(encoder,OPUS_SET_BITRATE(24000));
    const char *name=SDL_GetAudioDeviceName(SDL_AUDIO_DEVICE_DEFAULT_RECORDING);
    printf("Voice: microphone open (%s)\n",name ? name : "default");
    return 1;
}

static void close_mic(void) {
    if (mic) { SDL_DestroyAudioStream(mic); mic=NULL; printf("Voice: microphone closed\n"); }
    if (encoder) { opus_encoder_destroy(encoder); encoder=NULL; }
}

static int speaking(const int16_t *pcm) {
    double sum=0;
    for (int i=0;i<FRAME;++i) sum+=(double)pcm[i]*pcm[i];
    if (sqrt(sum/FRAME)>=gate_level) hangover=25;
    else if (hangover>0) --hangover;
    return hangover>0;
}

static void send_mic(void) {
    if (SDL_GetAudioStreamAvailable(mic)>RATE) SDL_ClearAudioStream(mic);
    int16_t pcm[FRAME];
    unsigned char packet[MAX_PACKET];
    while (SDL_GetAudioStreamAvailable(mic)>=(int)sizeof(pcm)) {
        if (SDL_GetAudioStreamData(mic,pcm,sizeof(pcm))!=(int)sizeof(pcm)) break;
        if (!speaking(pcm)) continue;
        int n=opus_encode(encoder,pcm,FRAME,packet,MAX_PACKET);
        if (n>0 && bbgpu_voice_send(packet,(size_t)n)>0) ++sent;
    }
}

static void free_peer(Peer *p) {
    if (p->decoder) opus_decoder_destroy(p->decoder);
    if (p->speaker) SDL_DestroyAudioStream(p->speaker);
    memset(p,0,sizeof(*p));
}

static void *voice_thread(void *arg) {
    (void)arg;
    uint64_t idle_since=now_ms(), last_report=0;
    for (;;) {
        SDL_Delay(20);
        int count=bbgpu_voice_peers(), flags=bbgpu_voice_flags();
        uint64_t now=now_ms();
        pthread_mutex_lock(&lock);
        last_peers=count;
        if (count>0 && (flags&2)) {
            idle_since=now;
            if (open_mic()) send_mic();
        } else if (mic && (!(flags&2) || now-idle_since>5000)) { close_mic(); hangover=0; }
        for (int i=0;i<MAX_PEERS;++i) if (peers[i].key && now-peers[i].last>30000) free_peer(&peers[i]);
        if (now-last_report>=10000 && (count>0 || sent || received)) {
            last_report=now;
            printf("Voice: %d peer(s), %llu packets sent, %llu received, mic %s\n",count,(unsigned long long)sent,(unsigned long long)received,mic ? "open" : "closed");
        }
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

static void start_thread(void) {
    pthread_t thread;
    if (pthread_create(&thread,NULL,voice_thread,NULL)==0) { pthread_detach(thread); printf("Voice: network voice ready\n"); }
}

void runtime_voice_net_start(void) {
    if (voice_enabled()) pthread_once(&once,start_thread);
}

void runtime_voice_receive(const uint8_t *data, size_t size, uint64_t key) {
    if (!voice_enabled() || !size || size>MAX_PACKET || !(bbgpu_voice_flags()&1)) return;
    pthread_mutex_lock(&lock);
    Peer *p=NULL;
    for (int i=0;i<MAX_PEERS && !p;++i) if (peers[i].key==key) p=&peers[i];
    for (int i=0;i<MAX_PEERS && !p;++i) if (!peers[i].key) { p=&peers[i]; p->key=key; }
    if (p && audio_ready()) {
        p->last=now_ms();
        if (!p->decoder) { int error=0; p->decoder=opus_decoder_create(RATE,1,&error); }
        if (!p->speaker) {
            SDL_AudioSpec spec={SDL_AUDIO_S16,1,RATE};
            p->speaker=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,NULL,NULL);
            if (p->speaker) { SDL_ResumeAudioStreamDevice(p->speaker); printf("Voice: hearing a new player\n"); }
        }
        int16_t pcm[FRAME*6];
        int samples=p->decoder && p->speaker ? opus_decode(p->decoder,data,(int32_t)size,pcm,FRAME*6,0) : 0;
        if (samples>0) {
            if (play_gain!=1.f)
                for (int i=0;i<samples;++i) { float v=pcm[i]*play_gain; pcm[i]=(int16_t)(v>32767.f ? 32767.f : v<-32768.f ? -32768.f : v); }
            if (SDL_GetAudioStreamQueued(p->speaker)>RATE) SDL_ClearAudioStream(p->speaker);
            SDL_PutAudioStreamData(p->speaker,pcm,samples*2);
            ++received;
            last_heard=now_ms();
        }
    }
    pthread_mutex_unlock(&lock);
}

void runtime_voice_status(int *peers, int *talking, int *hearing) {
    pthread_mutex_lock(&lock);
    if (peers) *peers=last_peers;
    if (talking) *talking=mic && hangover>0;
    if (hearing) *hearing=last_heard && now_ms()-last_heard<400;
    pthread_mutex_unlock(&lock);
}

static ABI int32_t voice_ok(void) { return 0; }
static ABI int32_t voice_create_port(uint32_t *id, const void *param) {
    if (!id || !param) return VOICE_ARGUMENT;
    pthread_mutex_lock(&lock); *id=(uint32_t)next_port++; pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t voice_read(uint32_t id, void *data, uint32_t *size) { (void)id; (void)data; if (size) *size=0; return 0; }
static ABI int32_t voice_write(uint32_t id, const void *data, uint32_t *size, int16_t unknown) { (void)id; (void)data; (void)size; (void)unknown; return 0; }
static ABI int32_t voice_info(uint32_t id, void *info) { (void)id; if (!info) return VOICE_ARGUMENT; memset(info,0,32); return 0; }
static ABI int32_t audio_in_open(void) { return AUDIO_IN_NOT_OPENED; }

static const RuntimeExport exports[]={
    {"sceVoiceInit",voice_ok}, {"sceVoiceEnd",voice_ok}, {"sceVoiceStart",voice_ok}, {"sceVoiceStop",voice_ok},
    {"sceVoiceCreatePort",voice_create_port}, {"sceVoiceDeletePort",voice_ok},
    {"sceVoiceConnectIPortToOPort",voice_ok}, {"sceVoiceDisconnectIPortFromOPort",voice_ok},
    {"sceVoiceGetPortInfo",voice_info}, {"sceVoiceReadFromOPort",voice_read}, {"sceVoiceWriteToIPort",voice_write},
    {"sceAudioInOpen",audio_in_open}, {"sceAudioInInput",audio_in_open}, {"sceAudioInClose",audio_in_open},
};

uintptr_t runtime_voice_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
