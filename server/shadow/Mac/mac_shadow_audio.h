/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * macOS shadow server system-audio capture
 *
 * Licensed under the Apache License, Version 2.0.
 */

#ifndef FREERDP_SERVER_SHADOW_MAC_SHADOW_AUDIO_H
#define FREERDP_SERVER_SHADOW_MAC_SHADOW_AUDIO_H

#include <winpr/wtypes.h>

typedef struct mac_shadow_audio_capture MacShadowAudioCapture;
typedef void (*MacShadowAudioSamples)(void* context, const INT16* samples, size_t frames);

#ifdef __cplusplus
extern "C"
{
#endif

	MacShadowAudioCapture* mac_shadow_audio_new(MacShadowAudioSamples samples, void* context);
	int mac_shadow_audio_start(MacShadowAudioCapture* capture);
	void mac_shadow_audio_stop(MacShadowAudioCapture* capture);
	void mac_shadow_audio_free(MacShadowAudioCapture* capture);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_MAC_SHADOW_AUDIO_H */
