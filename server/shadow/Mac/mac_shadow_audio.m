/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * macOS shadow server system-audio capture
 *
 * Licensed under the Apache License, Version 2.0.
 */

#import <Foundation/Foundation.h>
#import <CoreAudio/CATapDescription.h>
#import <CoreAudio/AudioHardware.h>
#import <CoreAudio/AudioHardwareTapping.h>

#include <math.h>
#include <stdlib.h>

#include "mac_shadow_audio.h"

struct mac_shadow_audio_capture
{
	AudioObjectID tap;
	AudioObjectID aggregate;
	AudioDeviceIOProcID ioProc;
	dispatch_queue_t queue;
	MacShadowAudioSamples samples;
	void* context;
	BOOL running;
};

static void mac_shadow_audio_reset(MacShadowAudioCapture* capture)
{
	if (!capture)
		return;

	if (capture->running && (capture->aggregate != kAudioObjectUnknown) && capture->ioProc)
		(void)AudioDeviceStop(capture->aggregate, capture->ioProc);
	capture->running = FALSE;

	if ((capture->aggregate != kAudioObjectUnknown) && capture->ioProc)
		(void)AudioDeviceDestroyIOProcID(capture->aggregate, capture->ioProc);
	capture->ioProc = nullptr;

	if (capture->aggregate != kAudioObjectUnknown)
		(void)AudioHardwareDestroyAggregateDevice(capture->aggregate);
	capture->aggregate = kAudioObjectUnknown;

	if (capture->tap != kAudioObjectUnknown)
	{
		if (@available(macOS 14.2, *))
			(void)AudioHardwareDestroyProcessTap(capture->tap);
	}
	capture->tap = kAudioObjectUnknown;
}

static BOOL mac_shadow_audio_get_tap_uid(AudioObjectID tap, CFStringRef* uid)
{
	AudioObjectPropertyAddress address = { kAudioTapPropertyUID, kAudioObjectPropertyScopeGlobal,
	                                       kAudioObjectPropertyElementMain };
	UInt32 size = sizeof(*uid);
	*uid = nullptr;
	return AudioObjectGetPropertyData(tap, &address, 0, nullptr, &size, uid) == noErr;
}

static BOOL mac_shadow_audio_get_tap_format(AudioObjectID tap,
	                                         AudioStreamBasicDescription* format)
{
	AudioObjectPropertyAddress address = { kAudioTapPropertyFormat, kAudioObjectPropertyScopeGlobal,
	                                       kAudioObjectPropertyElementMain };
	UInt32 size = sizeof(*format);
	memset(format, 0, sizeof(*format));
	return AudioObjectGetPropertyData(tap, &address, 0, nullptr, &size, format) == noErr;
}

static void mac_shadow_audio_deliver(MacShadowAudioCapture* capture,
	                                  const AudioStreamBasicDescription* format,
	                                  const AudioBufferList* buffers)
{
	const BOOL planar = (format->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
	size_t frames = 0;
	INT16* samples = nullptr;

	if (!capture || !capture->samples || !buffers || (buffers->mNumberBuffers == 0))
		return;

	if (planar)
	{
		if ((buffers->mNumberBuffers < 2) || !buffers->mBuffers[0].mData ||
		    !buffers->mBuffers[1].mData)
			return;
		const UInt32 leftSize = buffers->mBuffers[0].mDataByteSize;
		const UInt32 rightSize = buffers->mBuffers[1].mDataByteSize;
		frames = ((leftSize < rightSize) ? leftSize : rightSize) / sizeof(float);
	}
	else
	{
		if (!buffers->mBuffers[0].mData)
			return;
		frames = buffers->mBuffers[0].mDataByteSize / (sizeof(float) * 2);
	}

	if ((frames == 0) || (frames > (SIZE_MAX / (2 * sizeof(INT16)))))
		return;
	samples = (INT16*)malloc(frames * 2 * sizeof(INT16));
	if (!samples)
		return;

	for (size_t frame = 0; frame < frames; frame++)
	{
		float left = 0;
		float right = 0;
		if (planar)
		{
			left = ((const float*)buffers->mBuffers[0].mData)[frame];
			right = ((const float*)buffers->mBuffers[1].mData)[frame];
		}
		else
		{
			left = ((const float*)buffers->mBuffers[0].mData)[frame * 2];
			right = ((const float*)buffers->mBuffers[0].mData)[(frame * 2) + 1];
		}
		samples[frame * 2] = (INT16)lrintf(fmaxf(-1.0f, fminf(left, 1.0f)) * 32767.0f);
		samples[(frame * 2) + 1] =
		    (INT16)lrintf(fmaxf(-1.0f, fminf(right, 1.0f)) * 32767.0f);
	}

	capture->samples(capture->context, samples, frames);
	free(samples);
}

MacShadowAudioCapture* mac_shadow_audio_new(MacShadowAudioSamples samples, void* context)
{
	MacShadowAudioCapture* capture = nullptr;
	if (!samples)
		return nullptr;

	capture = (MacShadowAudioCapture*)calloc(1, sizeof(MacShadowAudioCapture));
	if (!capture)
		return nullptr;
	capture->tap = kAudioObjectUnknown;
	capture->aggregate = kAudioObjectUnknown;
	capture->samples = samples;
	capture->context = context;
	capture->queue = dispatch_queue_create("mac.shadow.audio.capture", nullptr);
	if (!capture->queue)
	{
		free(capture);
		return nullptr;
	}
	return capture;
}

int mac_shadow_audio_start(MacShadowAudioCapture* capture)
{
	if (!capture)
		return -1;
	if (capture->running)
		return 1;

	if (@available(macOS 14.2, *))
	{
		@autoreleasepool
		{
			CATapDescription* description =
			    [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:@[]];
			description.name = @"FreeRDP Shadow System Audio";
			description.privateTap = YES;
			description.muteBehavior = CATapUnmuted;
			if (AudioHardwareCreateProcessTap(description, &capture->tap) != noErr)
				goto fail;

			CFStringRef tapUID = nullptr;
			if (!mac_shadow_audio_get_tap_uid(capture->tap, &tapUID) || !tapUID)
				goto fail;

			NSString* aggregateUID = [NSUUID UUID].UUIDString;
			NSDictionary* aggregateDescription = @{
				[NSString stringWithUTF8String:kAudioAggregateDeviceNameKey] :
					@"FreeRDP Shadow Audio",
				[NSString stringWithUTF8String:kAudioAggregateDeviceUIDKey] : aggregateUID,
				[NSString stringWithUTF8String:kAudioAggregateDeviceIsPrivateKey] : @YES,
				[NSString stringWithUTF8String:kAudioAggregateDeviceTapAutoStartKey] : @YES
			};
			if (AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggregateDescription,
			                                       &capture->aggregate) != noErr)
			{
				CFRelease(tapUID);
				goto fail;
			}

			const void* values[] = { tapUID };
			CFArrayRef tapList =
			    CFArrayCreate(kCFAllocatorDefault, values, 1, &kCFTypeArrayCallBacks);
			CFRelease(tapUID);
			if (!tapList)
				goto fail;
			AudioObjectPropertyAddress tapListAddress = {
				kAudioAggregateDevicePropertyTapList, kAudioObjectPropertyScopeGlobal,
				kAudioObjectPropertyElementMain
			};
			const UInt32 tapListSize = sizeof(tapList);
			const OSStatus tapListStatus =
			    AudioObjectSetPropertyData(capture->aggregate, &tapListAddress, 0, nullptr,
			                               tapListSize, &tapList);
			CFRelease(tapList);
			if (tapListStatus != noErr)
				goto fail;

			AudioStreamBasicDescription format = { 0 };
			if (!mac_shadow_audio_get_tap_format(capture->tap, &format) ||
			    (format.mFormatID != kAudioFormatLinearPCM) ||
			    ((format.mFormatFlags & kAudioFormatFlagIsFloat) == 0) ||
			    (format.mBitsPerChannel != 32) || (format.mChannelsPerFrame != 2) ||
			    (format.mSampleRate != 44100.0))
			{
				goto fail;
			}

			MacShadowAudioCapture* blockCapture = capture;
			const OSStatus ioStatus = AudioDeviceCreateIOProcIDWithBlock(
			    &capture->ioProc, capture->aggregate, capture->queue,
			    ^(const AudioTimeStamp* now, const AudioBufferList* input,
			      const AudioTimeStamp* inputTime, AudioBufferList* output,
			      const AudioTimeStamp* outputTime) {
			      (void)now;
			      (void)inputTime;
			      (void)output;
			      (void)outputTime;
			      MacShadowAudioCapture* strongCapture = blockCapture;
			      if (strongCapture && strongCapture->running)
				      mac_shadow_audio_deliver(strongCapture, &format, input);
			    });
			if (ioStatus != noErr)
				goto fail;

			capture->running = TRUE;
			if (AudioDeviceStart(capture->aggregate, capture->ioProc) != noErr)
				goto fail;
			return 1;
		}
	}

fail:
	mac_shadow_audio_reset(capture);
	return -1;
}

void mac_shadow_audio_stop(MacShadowAudioCapture* capture)
{
	mac_shadow_audio_reset(capture);
}

void mac_shadow_audio_free(MacShadowAudioCapture* capture)
{
	if (!capture)
		return;
	mac_shadow_audio_reset(capture);
#if !OS_OBJECT_USE_OBJC
	dispatch_release(capture->queue);
#endif
	free(capture);
}
