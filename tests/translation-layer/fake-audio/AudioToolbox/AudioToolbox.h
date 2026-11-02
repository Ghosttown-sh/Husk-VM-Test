/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TEST_AUDIO_QUEUE_H
#define TEST_AUDIO_QUEUE_H
#include <stdint.h>
#include <stdbool.h>
typedef uint32_t UInt32;
typedef int32_t OSStatus;
typedef struct { double mSampleRate; UInt32 mFormatID,mFormatFlags,mBytesPerPacket,mFramesPerPacket,mBytesPerFrame,mChannelsPerFrame,mBitsPerChannel,mReserved; } AudioStreamBasicDescription;
typedef struct { UInt32 mAudioDataBytesCapacity; void *mAudioData; UInt32 mAudioDataByteSize; } AudioQueueBuffer;
typedef AudioQueueBuffer *AudioQueueBufferRef;
typedef struct fake_queue *AudioQueueRef;
typedef void (*AudioQueueOutputCallback)(void *,AudioQueueRef,AudioQueueBufferRef);
enum { noErr=0,kAudioFormatLinearPCM=1,kLinearPCMFormatFlagIsSignedInteger=4,kLinearPCMFormatFlagIsPacked=8 };
OSStatus AudioQueueNewOutput(const AudioStreamBasicDescription *,AudioQueueOutputCallback,void *,void *,void *,UInt32,AudioQueueRef *);
OSStatus AudioQueueAllocateBuffer(AudioQueueRef,UInt32,AudioQueueBufferRef *);
OSStatus AudioQueueEnqueueBuffer(AudioQueueRef,AudioQueueBufferRef,UInt32,const void *);
OSStatus AudioQueueStart(AudioQueueRef,const void *);
OSStatus AudioQueuePause(AudioQueueRef);
OSStatus AudioQueueDispose(AudioQueueRef,bool);
#endif
