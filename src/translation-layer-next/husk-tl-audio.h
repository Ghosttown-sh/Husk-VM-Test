/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The host's audio output for a guest's mixer thread.
 *
 * Android audio engines (FMOD here) write interleaved 16-bit samples from a thread of their own and
 * expect each write to block until the audio has made room, which paces the mixer. This is that
 * sink, over AudioQueue (the same on macOS and iOS): a short ring the mixer fills and the audio
 * hardware drains.
 */
#ifndef HUSK_TL_AUDIO_H
#define HUSK_TL_AUDIO_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Route the guest's audio output (tl_cocos_audio_hook) to the speakers. */
void tl_audio_install(void);

/* Silence the output and stop the clock while the game is not on screen (writers block until resumed). */
void tl_audio_set_paused(bool paused);

/* Wake a stopped producer even if the app is paused or its ring is full.
 * Call before joining it. The thread's sink is disposed by its TLS destructor. */
void tl_audio_cancel_thread(pthread_t producer);
/* Called on the producer, before its first write. The callback must be a
 * nonblocking atomic lifetime check. Closes the cancel-before-sink-open race. */
void tl_audio_set_thread_alive(bool (*alive)(void *), void *context);

#ifdef __cplusplus
}
#endif

#endif
