/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
void tl_log_line(const char *fmt,...) { (void)fmt; }
void tl_audio_set_thread_alive(bool (*alive)(void *),void *ctx) { (void)alive; (void)ctx; }
void tl_audio_cancel_thread(pthread_t t) { (void)t; }
void (*tl_cocos_audio_hook)(const int16_t *,int,int,int);
#include "production_aaudio.inc"

static int expected_rate, expected_channels, callbacks;
static int64_t captured;
static void capture(const int16_t *pcm,int frames,int ch,int rate) {
    assert(rate==expected_rate && ch==2 && frames>0);
    for(int i=0;i<frames;i++) {
        assert(pcm[2*i]==8191);
        assert(pcm[2*i+1]==(expected_channels==1?8191:-16383));
    }
    captured+=frames;
}
static int fill(void *stream,void *user,void *data,int frames) {
    (void)user;
    aa_stream *s=stream;
    assert(frames==256 && b_AAudioStream_getSampleRate(s)==expected_rate);
    assert(b_AAudioStream_getChannelCount(s)==expected_channels);
    float *pcm=data;
    for(int i=0;i<frames;i++) for(int c=0;c<expected_channels;c++)
        pcm[i*expected_channels+c]=c?-0.5f:0.25f;
    return ++callbacks==10?1:0;
}
int main(void) {
    alarm(30);
    tl_cocos_audio_hook=capture;
    const int rates[]={0,8000,16000,22050,24000,32000,44100,48000,96000};
    for(unsigned r=0;r<sizeof(rates)/sizeof(rates[0]);r++) for(int ch=1;ch<=2;ch++) {
        aa_builder *b=NULL; aa_stream *s=NULL;
        expected_rate=rates[r]?rates[r]:48000; expected_channels=ch; captured=0;
        assert(b_AAudio_createStreamBuilder((void **)&b)==AA_OK);
        b_AAudioStreamBuilder_setSampleRate(b,rates[r]);
        b_AAudioStreamBuilder_setChannelCount(b,ch);
        b_AAudioStreamBuilder_setFormat(b,AA_FORMAT_FLOAT);
        assert(b_AAudioStreamBuilder_openStream(b,&s)==AA_OK);
        assert(b_AAudioStream_getSampleRate(s)==expected_rate);
        assert(b_AAudioStream_getHardwareSampleRate(s)==48000);
        assert(b_AAudioStream_requestStart(s)==AA_OK);
        float *pcm=malloc((size_t)expected_rate*ch*sizeof(float)); assert(pcm);
        for(int i=0;i<expected_rate;i++) for(int c=0;c<ch;c++) pcm[i*ch+c]=c?-0.5f:0.25f;
        assert(b_AAudioStream_write(s,pcm,expected_rate,0)==expected_rate);
        assert(captured==expected_rate && b_AAudioStream_getFramesWritten(s)==expected_rate);
        free(pcm); b_AAudioStream_close(s);
        /* Callback length counts frames, not bytes or interleaved samples. */
        b_AAudioStreamBuilder_setFramesPerDataCallback(b,256);
        b_AAudioStreamBuilder_setDataCallback(b,fill,NULL);
        assert(b_AAudioStreamBuilder_openStream(b,&s)==AA_OK);
        callbacks=0; captured=0; atomic_store(&s->run,true);
        pthread_t thread; assert(pthread_create(&thread,NULL,aa_pump,s)==0);
        assert(pthread_join(thread,NULL)==0);
        assert(callbacks==10 && captured==2560 && b_AAudioStream_getFramesWritten(s)==2560);
        b_AAudioStream_close(s); b_AAudioStreamBuilder_delete(b);
    }
    aa_builder invalid={0}; aa_stream *s=(void *)1;
    invalid.rate=-1; assert(b_AAudioStreamBuilder_openStream(&invalid,&s)!=AA_OK && s==NULL);
    invalid.rate=384000; assert(b_AAudioStreamBuilder_openStream(&invalid,&s)!=AA_OK && s==NULL);
    puts("PASS: 18 rate/channel combinations preserve one-second PCM duration, stereo conversion, 256-frame callbacks and stream frame counters; invalid rates fail honestly");
}
