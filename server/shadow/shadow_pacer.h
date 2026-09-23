/* Mac shadow bitmap pacing. Licensed under the Apache License, Version 2.0. */
#ifndef FREERDP_SHADOW_PACER_H
#define FREERDP_SHADOW_PACER_H

#include <winpr/wtypes.h>

#define SHADOW_PACER_LARGE_REFRESH_QUIET_REARM_MS 2000U

typedef struct
{
	double rate; /* bytes/second; a conservative sending budget, not an RTT estimate */
	double credit;
	double maxCredit;
	double requiredCredit;
	double baseRate;
	double baseMaxCredit;
	double burstRate;
	double burstMaxCredit;
	double drainRate;
	UINT64 lastMs;
	UINT64 lastRampMs;
	UINT64 lastDecreaseMs;
	UINT64 lastQueueMs;
	UINT32 lastQueued;
	UINT32 submittedSinceQueue;
	UINT32 burstDurationMs;
	UINT32 burstCooldownMs;
	UINT32 largeDamagePercent;
	UINT64 burstEndMs;
	UINT64 burstStartMs;
	UINT64 burstStoppedMs;
	UINT64 burstEligibleMs;
	UINT64 lastPublicationDamageMs;
	UINT64 burstByteCap;
	UINT64 burstBytes;
	UINT32 admittedBytes;
	BOOL initialized;
	BOOL pressure;
	BOOL haveQueue;
	BOOL fixed;
	BOOL largeRefreshBurstEnabled;
	BOOL burstActive;
	BOOL largeRefreshDiagnosticM;
	BOOL havePublicationDamage;
	UINT32 burstEndReason; /* 0: none, 1: time, 2: exact cap, 3: unusable remainder,
	                        * 4: local socket queue */
} shadowPacer;

void shadow_pacer_reset(shadowPacer* pacer);
BOOL shadow_pacer_parse_fixed_rate_kib(const char* value, UINT32* rateKiB);
BOOL shadow_pacer_set_fixed_rate_kib(shadowPacer* pacer, UINT32 rateKiB);
BOOL shadow_pacer_enable_large_refresh_burst(shadowPacer* pacer);
BOOL shadow_pacer_enable_large_refresh_diagnostic_m(shadowPacer* pacer);
BOOL shadow_pacer_note_damage(shadowPacer* pacer, UINT64 nowMs, UINT64 damagedPixels,
                              UINT64 totalPixels);
void shadow_pacer_note_time(shadowPacer* pacer, UINT64 nowMs, BOOL demand);
void shadow_pacer_cancel_wait(shadowPacer* pacer);
BOOL shadow_pacer_preflight(shadowPacer* pacer, UINT32 bytes);
double shadow_pacer_credit_limit(const shadowPacer* pacer);
void shadow_pacer_observe(shadowPacer* pacer, UINT64 nowMs, BOOL queueValid,
                          UINT32 queuedBytes, BOOL writeBlocked, BOOL demand);
BOOL shadow_pacer_can_submit(shadowPacer* pacer, UINT32 bytes);
void shadow_pacer_submitted(shadowPacer* pacer, UINT32 bytes);
UINT32 shadow_pacer_wait_ms(const shadowPacer* pacer);

#endif
