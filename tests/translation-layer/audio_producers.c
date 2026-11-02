/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <AudioToolbox/AudioToolbox.h>
struct fake_queue {
    AudioStreamBasicDescription format;
    AudioQueueOutputCallback cb;
    void *user;
    AudioQueueBufferRef buffers[3];
    int allocated;
    bool running;
};
static atomic_int created,disposed;
static int fail_at;
OSStatus AudioQueueNewOutput(const AudioStreamBasicDescription *f,AudioQueueOutputCallback cb,void *u,void *r,void *m,UInt32 flags,AudioQueueRef *out) {
    (void)r;(void)m;(void)flags;
    if(fail_at==1) return -1;
    *out=calloc(1,sizeof(**out)); assert(*out);
    (*out)->format=*f; (*out)->cb=cb; (*out)->user=u; atomic_fetch_add(&created,1); return 0;
}
OSStatus AudioQueueAllocateBuffer(AudioQueueRef q,UInt32 size,AudioQueueBufferRef *out) {
    if(fail_at==2) return -2;
    assert(q->allocated<3); *out=calloc(1,sizeof(**out)); assert(*out);
    (*out)->mAudioData=calloc(1,size); assert((*out)->mAudioData);
    (*out)->mAudioDataBytesCapacity=size; q->buffers[q->allocated++]=*out; return 0;
}
OSStatus AudioQueueEnqueueBuffer(AudioQueueRef q,AudioQueueBufferRef b,UInt32 n,const void *p) {
    (void)q;(void)n;(void)p; assert(b->mAudioDataByteSize<=b->mAudioDataBytesCapacity);
    return fail_at==3?-3:0;
}
OSStatus AudioQueueStart(AudioQueueRef q,const void *t) { (void)t; if(fail_at==4) return -4; q->running=true; return 0; }
OSStatus AudioQueuePause(AudioQueueRef q) { q->running=false; return 0; }
OSStatus AudioQueueDispose(AudioQueueRef q,bool now) {
    assert(now);
    for(int i=0;i<q->allocated;i++) { free(q->buffers[i]->mAudioData); free(q->buffers[i]); }
    free(q); atomic_fetch_add(&disposed,1); return 0;
}
#include "../../src/translation-layer-next/husk-tl-audio.c"
void (*tl_cocos_audio_hook)(const int16_t *,int,int,int);
void tl_log_line(const char *fmt,...) { (void)fmt; }
typedef struct { atomic_bool run; atomic_int stage,phase; pthread_t thread; int rate,channels,value; audio_sink *sink; } producer;
static bool alive(void *p) { return atomic_load(&((producer *)p)->run); }
static void *produce(void *ptr) {
    producer *p=ptr; int16_t buf[RING_FRAMES*2];
    for(size_t i=0;i<sizeof(buf)/sizeof(buf[0]);i++) buf[i]=(int16_t)p->value;
    tl_audio_set_thread_alive(alive,p);
    host_write(buf,RING_FRAMES,p->channels,p->rate);
    p->sink=pthread_getspecific(sink_key);
    atomic_store(&p->stage,1);
    while(atomic_load(&p->phase)<2 && alive(p)) usleep(1000);
    if(alive(p)) host_write(buf,1,p->channels,p->rate);
    atomic_store(&p->stage,3);
    while(atomic_load(&p->phase)<4 && alive(p)) usleep(1000);
    return NULL; /* TLS destructor must remove/dispose this sink. */
}
static void await_stage(producer *p,int n) {
    for(int t=0;t<3000 && atomic_load(&p->stage)!=n;t++) usleep(1000);
    assert(atomic_load(&p->stage)==n);
}
static void stop(producer *p) {
    atomic_store(&p->run,false); tl_audio_cancel_thread(p->thread);
    assert(pthread_join(p->thread,NULL)==0);
}
static void drain(audio_sink *s,int16_t expected) {
    AudioQueueRef q=s->q; assert(q->running);
    q->cb(q->user,q,q->buffers[0]);
    int16_t *out=q->buffers[0]->mAudioData;
    for(int i=0;i<FRAMES_PER_BUF*s->channels;i++) assert(out[i]==expected);
}
int main(void) {
    alarm(30); /* A cancellation/pause deadlock must fail CI, not hang it. */
    producer p[11]={0};
    for(int i=0;i<11;i++) {
        p[i].rate=i?44100:48000; p[i].channels=i==10?1:2; p[i].value=1000+i;
        atomic_store(&p[i].run,true);
        assert(pthread_create(&p[i].thread,NULL,produce,&p[i])==0);
    }
    for(int i=0;i<11;i++) {
        await_stage(&p[i],1);
        assert(p[i].sink && p[i].sink->q->format.mSampleRate==p[i].rate);
        assert(p[i].sink->q->format.mChannelsPerFrame==(unsigned)p[i].channels);
        assert(p[i].sink->count==RING_FRAMES);
        for(int j=0;j<i;j++) assert(p[j].sink->q!=p[i].sink->q);
        atomic_store(&p[i].phase,2);
    }
    usleep(20000);
    drain(p[0].sink,1000); await_stage(&p[0],3);
    for(int i=1;i<11;i++) assert(atomic_load(&p[i].stage)==1);
    for(int i=1;i<11;i++) { drain(p[i].sink,(int16_t)(1000+i)); await_stage(&p[i],3); }
    puts("PASS: 11 simultaneous sources have independent PCM/rate/channel/clock queues; draining one cannot consume or unblock another");
    tl_audio_set_paused(true);
    for(int i=0;i<11;i++) assert(!p[i].sink->q->running);
    tl_audio_set_paused(false);
    for(int i=0;i<11;i++) assert(p[i].sink->q->running);
    for(int i=0;i<11;i++) stop(&p[i]);
    assert(!sinks && atomic_load(&created)==atomic_load(&disposed));
    /* Pausing before a producer opens its sink and cancelling before it can
     * write is the race that otherwise deadlocks AA/OpenSL stop+join. */
    tl_audio_set_paused(true);
    producer late={0}; late.rate=48000;late.channels=2; atomic_store(&late.run,true);
    assert(pthread_create(&late.thread,NULL,produce,&late)==0);
    usleep(10000); stop(&late);
    tl_audio_set_paused(false);
    assert(!sinks && atomic_load(&created)==atomic_load(&disposed));
    puts("PASS: global pause/resume, stopped full/paused producers, TLS disposal and cancel-before-open complete without deadlock/leaks");
    tl_audio_set_thread_alive(NULL,NULL);
    int16_t low[]={100,300,-100,500};
    host_write(low,4,1,24000);
    audio_sink *s=pthread_getspecific(sink_key);
    assert(s && s->rate==48000 && s->count==8);
    const int16_t expected[]={100,100,200,300,100,-100,200,500};
    assert(memcmp(s->ring,expected,sizeof(expected))==0);
    /* A format change must replace only this producer and reset history. */
    int16_t stereo[]={-200,200}; host_write(stereo,1,2,16000);
    s=pthread_getspecific(sink_key); assert(s && s->rate==32000 && s->count==2);
    assert(s->ring[0]==-200 && s->ring[1]==200 && s->ring[2]==-200 && s->ring[3]==200);
    pthread_setspecific(sink_key,NULL); destroy_sink(s);
    for(fail_at=1;fail_at<=4;fail_at++) assert(new_sink(48000,2)==NULL);
    fail_at=0;
    assert(!sinks && atomic_load(&created)==atomic_load(&disposed));
    puts("PASS: isolated low-rate resampling history, format changes and partial AudioQueue initialization failure cleanup");
}
