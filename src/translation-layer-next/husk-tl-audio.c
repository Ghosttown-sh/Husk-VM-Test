/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _DARWIN_C_SOURCE
#include "husk-tl-audio.h"
#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "husk-tl-bionic.h"
#include "husk-tl-cocos.h"
#define NBUF 3
#define FRAMES_PER_BUF 1024
#define RING_FRAMES 8192

/* Each AAudio/OpenSL/FMOD mixer has its own persistent producer thread.
 * A single shared ring concatenated simultaneous sounds (including silence)
 * and played 44.1 kHz samples at whichever rate opened first. Core Audio mixes
 * independent AudioQueues and converts each declared rate to the device rate. */
typedef struct audio_sink {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t owner;
    struct audio_sink *next;
    AudioQueueRef q;
    AudioQueueBufferRef bufs[NBUF];
    int input_rate, rate, channels;
    int16_t *ring;
    size_t head, count;
    bool paused, cancelled, failed, wrote, have_prev;
    int16_t prev[2];
    uint64_t written, drained, missing;
    double report_at;
} audio_sink;
static pthread_mutex_t sinks_mu=PTHREAD_MUTEX_INITIALIZER;
static audio_sink *sinks;
static bool all_paused;
static pthread_key_t sink_key;
static pthread_once_t sink_once=PTHREAD_ONCE_INIT;
static _Thread_local bool (*producer_alive)(void *);
static _Thread_local void *producer_context;
void tl_audio_set_thread_alive(bool (*alive)(void *),void *context) {
    producer_alive=alive; producer_context=context;
}
static bool stopping(audio_sink *s) {
    if(producer_alive && !producer_alive(producer_context)) s->cancelled=true;
    return s->cancelled || s->failed;
}
static double seconds(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (double)ts.tv_sec+(double)ts.tv_nsec/1e9;
}
static void sleep_for(int frames,int rate) {
    if(frames<=0 || rate<=0) return;
    int64_t ns=(int64_t)frames*1000000000/rate;
    struct timespec ts={(time_t)(ns/1000000000),(long)(ns%1000000000)};
    nanosleep(&ts,NULL);
}
/* Device callbacks never wait for samples and touch only their own sink. */
static void on_buffer(void *user,AudioQueueRef q,AudioQueueBufferRef b) {
    audio_sink *s=user;
    size_t ch=(size_t)s->channels,want=FRAMES_PER_BUF;
    pthread_mutex_lock(&s->mu);
    size_t take=(s->paused || s->cancelled)?0:(s->count<want?s->count:want);
    size_t first=take<RING_FRAMES-s->head?take:RING_FRAMES-s->head;
    memcpy(b->mAudioData,s->ring+s->head*ch,first*ch*2);
    memcpy((int16_t *)b->mAudioData+first*ch,s->ring,(take-first)*ch*2);
    s->head=(s->head+take)%RING_FRAMES; s->count-=take; s->drained+=take;
    if(s->wrote && !s->paused && !s->cancelled) s->missing+=want-take;
    pthread_cond_broadcast(&s->cv); pthread_mutex_unlock(&s->mu);
    memset((int16_t *)b->mAudioData+take*ch,0,(want-take)*ch*2);
    b->mAudioDataByteSize=(UInt32)(want*ch*2);
    OSStatus st=AudioQueueEnqueueBuffer(q,b,0,NULL);
    if(st!=noErr) {
        pthread_mutex_lock(&s->mu); s->failed=true;
        pthread_cond_broadcast(&s->cv); pthread_mutex_unlock(&s->mu);
    }
}
static void destroy_sink(void *ptr) {
    audio_sink *s=ptr; if(!s) return;
    pthread_mutex_lock(&sinks_mu);
    audio_sink **p=&sinks;
    while(*p && *p!=s) p=&(*p)->next;
    if(*p) *p=s->next;
    pthread_mutex_unlock(&sinks_mu);
    /* Immediate disposal joins callbacks; never hold their mutex here. */
    if(s->q) AudioQueueDispose(s->q,true);
    free(s->ring); pthread_cond_destroy(&s->cv); pthread_mutex_destroy(&s->mu); free(s);
}
static int key_error;
static void make_key(void) { key_error=pthread_key_create(&sink_key,destroy_sink); }
static audio_sink *new_sink(int rate,int channels) {
    audio_sink *s=calloc(1,sizeof(*s)); if(!s) return NULL;
    pthread_mutex_init(&s->mu,NULL); pthread_cond_init(&s->cv,NULL);
    s->owner=pthread_self(); s->input_rate=rate; s->channels=channels;
    /* Preserve the low-rate FMOD workaround, with private resampling history. */
    s->rate=rate<=24000?rate*2:rate;
    s->ring=calloc((size_t)RING_FRAMES*channels,sizeof(int16_t));
    if(!s->ring) { destroy_sink(s); return NULL; }
    AudioStreamBasicDescription f={0};
    f.mSampleRate=s->rate; f.mFormatID=kAudioFormatLinearPCM;
    f.mFormatFlags=kLinearPCMFormatFlagIsSignedInteger|kLinearPCMFormatFlagIsPacked;
    f.mBitsPerChannel=16; f.mChannelsPerFrame=(UInt32)channels; f.mFramesPerPacket=1;
    f.mBytesPerFrame=f.mBytesPerPacket=(UInt32)channels*2;
    OSStatus st=AudioQueueNewOutput(&f,on_buffer,s,NULL,NULL,0,&s->q);
    for(int i=0;st==noErr && i<NBUF;i++) {
        st=AudioQueueAllocateBuffer(s->q,FRAMES_PER_BUF*(UInt32)channels*2,&s->bufs[i]);
        if(st!=noErr) break;
        memset(s->bufs[i]->mAudioData,0,FRAMES_PER_BUF*(size_t)channels*2);
        s->bufs[i]->mAudioDataByteSize=FRAMES_PER_BUF*(UInt32)channels*2;
        st=AudioQueueEnqueueBuffer(s->q,s->bufs[i],0,NULL);
    }
    if(st==noErr) {
        pthread_mutex_lock(&sinks_mu);
        s->paused=all_paused;
        if(!s->paused) st=AudioQueueStart(s->q,NULL);
        if(st==noErr) { s->next=sinks; sinks=s; }
        pthread_mutex_unlock(&sinks_mu);
    }
    if(st!=noErr) {
        tl_log_line("audio: independent output creation failed (%d)",(int)st);
        destroy_sink(s); return NULL;
    }
    s->report_at=seconds();
    tl_log_line("audio: independent producer output %p, %d Hz, %d channel(s)",(void *)s,s->rate,channels);
    return s;
}
static bool ring_write(audio_sink *s,const int16_t *samples,int frames) {
    pthread_mutex_lock(&s->mu);
    int done=0; size_t ch=(size_t)s->channels;
    while(done<frames && !stopping(s)) {
        while((s->count==RING_FRAMES || s->paused) && !stopping(s)) {
            if(producer_alive) {
                struct timespec t; clock_gettime(CLOCK_REALTIME,&t);
                t.tv_nsec+=20000000;
                if(t.tv_nsec>=1000000000) { t.tv_sec++; t.tv_nsec-=1000000000; }
                pthread_cond_timedwait(&s->cv,&s->mu,&t);
            } else pthread_cond_wait(&s->cv,&s->mu);
        }
        if(stopping(s)) break;
        size_t n=(size_t)(frames-done),room=RING_FRAMES-s->count;
        if(n>room) n=room;
        size_t tail=(s->head+s->count)%RING_FRAMES,first=n<RING_FRAMES-tail?n:RING_FRAMES-tail;
        memcpy(s->ring+tail*ch,samples+(size_t)done*ch,first*ch*2);
        memcpy(s->ring,samples+((size_t)done+first)*ch,(n-first)*ch*2);
        s->count+=n; s->written+=n; s->wrote=true; done+=(int)n;
    }
    bool ok=done==frames;
    double now=seconds();
    uint64_t written=s->written,drained=s->drained,missing=s->missing;
    bool report=now-s->report_at>=5.0;
    if(report) s->report_at=now;
    pthread_mutex_unlock(&s->mu);
    /* Empty frames include intentional gaps between effects, not just xruns. */
    if(report) tl_log_line("audio: producer %p rate=%d written=%llu drained=%llu empty_frames=%llu",
        (void *)s,s->rate,(unsigned long long)written,(unsigned long long)drained,(unsigned long long)missing);
    return ok;
}
static void host_write(const int16_t *samples,int frames,int channels,int rate) {
    if(!samples || frames<=0 || channels<1 || channels>2 || rate<=0) return;
    if(producer_alive && !producer_alive(producer_context)) return;
    pthread_once(&sink_once,make_key);
    if(key_error) { sleep_for(frames,rate); return; }
    audio_sink *s=pthread_getspecific(sink_key);
    if(s && (s->input_rate!=rate || s->channels!=channels)) {
        pthread_setspecific(sink_key,NULL); destroy_sink(s); s=NULL;
    }
    if(!s) {
        s=new_sink(rate,channels);
        if(!s) { sleep_for(frames,rate); return; }
        if(pthread_setspecific(sink_key,s)!=0) { destroy_sink(s); sleep_for(frames,rate); return; }
    }
    if(s->rate!=rate) {
        int16_t up[512*2*2]; /* Bounded scratch, no streaming heap allocation. */
        for(int done=0;done<frames;) {
            int n=frames-done; if(n>512) n=512;
            for(int f=0;f<n;f++) for(int c=0;c<channels;c++) {
                int16_t cur=samples[(size_t)(done+f)*channels+c];
                int16_t before=s->have_prev?s->prev[c]:cur;
                up[(size_t)(2*f)*channels+c]=(int16_t)(((int)before+cur)/2);
                up[(size_t)(2*f+1)*channels+c]=cur; s->prev[c]=cur;
                if(c==channels-1) s->have_prev=true;
            }
            if(!ring_write(s,up,n*2)) break;
            done+=n;
        }
    } else if(!ring_write(s,samples,frames)) sleep_for(frames,rate);
}
static void muted_write(const int16_t *samples,int frames,int channels,int rate) {
    static _Thread_local int seen,peak;
    static _Thread_local uint64_t loud,total;
    if(getenv("TL_AUDIO_STATS")) {
        for(int i=0;i<frames*channels;i++) {
            int v=samples[i]<0?-samples[i]:samples[i];
            if(v>peak) peak=v;
            if(v>64) loud++;
        }
        total+=(uint64_t)frames*channels;
        if((seen+=frames)>=rate) {
            tl_log_line("audio: %d Hz x %d, peak %d, %.0f%% of samples audible",rate,channels,peak,100.0*loud/(total?total:1));
            seen=peak=0; loud=total=0;
        }
    }
    sleep_for(frames,rate);
}
void tl_audio_install(void) { tl_cocos_audio_hook=getenv("TL_AUDIO_MUTE")?muted_write:host_write; }
void tl_audio_set_paused(bool paused) {
    pthread_mutex_lock(&sinks_mu); all_paused=paused;
    for(audio_sink *s=sinks;s;s=s->next) {
        pthread_mutex_lock(&s->mu); s->paused=paused;
        pthread_cond_broadcast(&s->cv); pthread_mutex_unlock(&s->mu);
        /* Queue control can synchronize with a callback; do not hold s->mu. */
        OSStatus st=paused?AudioQueuePause(s->q):AudioQueueStart(s->q,NULL);
        if(st!=noErr) tl_log_line("audio: %s failed (%d)",paused?"pause":"resume",(int)st);
    }
    pthread_mutex_unlock(&sinks_mu);
}
void tl_audio_cancel_thread(pthread_t producer) {
    pthread_mutex_lock(&sinks_mu);
    for(audio_sink *s=sinks;s;s=s->next) if(pthread_equal(s->owner,producer)) {
        pthread_mutex_lock(&s->mu); s->cancelled=true;
        pthread_cond_broadcast(&s->cv); pthread_mutex_unlock(&s->mu);
    }
    pthread_mutex_unlock(&sinks_mu);
}
