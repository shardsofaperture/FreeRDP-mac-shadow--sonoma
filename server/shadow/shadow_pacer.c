/* Mac shadow bitmap pacing. Licensed under the Apache License, Version 2.0. */
#include <freerdp/config.h>
#include <string.h>

#include "shadow_pacer.h"

#define PACER_INITIAL_RATE 150000.0
#define PACER_MIN_RATE 16000.0
#define PACER_MAX_RATE 50000000.0
#define PACER_FIXED_MAX_BURST 32768.0
#define PACER_LARGE_REFRESH_RATE (300U * 1024U)
#define PACER_LARGE_REFRESH_DURATION_MS 150U
#define PACER_LARGE_REFRESH_BYTE_CAP (48U * 1024U)
#define PACER_LARGE_REFRESH_COOLDOWN_MS 1000U
#define PACER_LARGE_REFRESH_PERCENT 25U
#define PACER_LARGE_REFRESH_QUEUE_LIMIT (64U * 1024U)
/* A fixed-rate admission must be able to hold one maximum 64x64 32-bit
 * bitmap operation plus the scheduler's transport-framing allowance. */
#define PACER_FIXED_ATOMIC_CREDIT (64U * 64U * 4U + 30U + 64U)

static double shadow_pacer_burst(const shadowPacer* pacer)
{
	double burst = pacer->rate * 0.15;
	if (burst < 16384.0)
		burst = 16384.0;
	if (burst > 65536.0)
		burst = 65536.0;
	return burst;
}

void shadow_pacer_reset(shadowPacer* pacer)
{
	memset(pacer, 0, sizeof(*pacer));
	pacer->rate = PACER_INITIAL_RATE;
	pacer->credit = 16384.0;
}

BOOL shadow_pacer_parse_fixed_rate_kib(const char* value, UINT32* rateKiB)
{
	if (!rateKiB)
		return FALSE;
	if (!value || strcmp(value, "0") == 0)
		*rateKiB = 0;
	else if (strcmp(value, "150") == 0)
		*rateKiB = 150;
	else if (strcmp(value, "175") == 0)
		*rateKiB = 175;
	else if (strcmp(value, "200") == 0)
		*rateKiB = 200;
	else if (strcmp(value, "250") == 0)
		*rateKiB = 250;
	else if (strcmp(value, "300") == 0)
		*rateKiB = 300;
	else if (strcmp(value, "400") == 0)
		*rateKiB = 400;
	else
		return FALSE;
	return TRUE;
}

BOOL shadow_pacer_set_fixed_rate_kib(shadowPacer* pacer, UINT32 rateKiB)
{
	if (!pacer || ((rateKiB != 150U) && (rateKiB != 175U) &&
	               (rateKiB != 200U) && (rateKiB != 250U) &&
	               (rateKiB != 300U) && (rateKiB != 400U)))
		return FALSE;
	shadow_pacer_reset(pacer);
	pacer->fixed = TRUE;
	pacer->rate = (double)rateKiB * 1024.0;
	pacer->maxCredit = pacer->rate / 10.0;
	if (pacer->maxCredit < PACER_FIXED_ATOMIC_CREDIT)
		pacer->maxCredit = PACER_FIXED_ATOMIC_CREDIT;
	if (pacer->maxCredit > PACER_FIXED_MAX_BURST)
		pacer->maxCredit = PACER_FIXED_MAX_BURST;
	pacer->credit = pacer->maxCredit;
	pacer->baseRate = pacer->rate;
	pacer->baseMaxCredit = pacer->maxCredit;
	return TRUE;
}

static void shadow_pacer_end_large_refresh_burst(shadowPacer* pacer, UINT64 nowMs,
                                                UINT32 reason)
{
	pacer->burstActive = FALSE;
	pacer->burstStoppedMs = nowMs;
	pacer->burstEndReason = reason;
	pacer->rate = pacer->baseRate;
	pacer->maxCredit = pacer->baseMaxCredit;
	if (pacer->credit > pacer->maxCredit)
		pacer->credit = pacer->maxCredit;
	if (pacer->requiredCredit > pacer->maxCredit)
		pacer->requiredCredit = pacer->maxCredit;
}

BOOL shadow_pacer_enable_large_refresh_burst(shadowPacer* pacer)
{
	if (!pacer || !pacer->fixed || (pacer->baseRate != 150U * 1024U))
		return FALSE;
	pacer->largeRefreshBurstEnabled = TRUE;
	pacer->burstRate = PACER_LARGE_REFRESH_RATE;
	pacer->burstMaxCredit = PACER_FIXED_MAX_BURST;
	pacer->burstDurationMs = PACER_LARGE_REFRESH_DURATION_MS;
	pacer->burstByteCap = PACER_LARGE_REFRESH_BYTE_CAP;
	pacer->burstCooldownMs = PACER_LARGE_REFRESH_COOLDOWN_MS;
	pacer->largeDamagePercent = PACER_LARGE_REFRESH_PERCENT;
	return TRUE;
}

BOOL shadow_pacer_enable_large_refresh_diagnostic_m(shadowPacer* pacer)
{
	if (!shadow_pacer_enable_large_refresh_burst(pacer))
		return FALSE;
	pacer->largeRefreshDiagnosticM = TRUE;
	pacer->burstRate = 600U * 1024U;
	pacer->burstDurationMs = 500U;
	pacer->burstByteCap = 256U * 1024U;
	return TRUE;
}

BOOL shadow_pacer_note_damage(shadowPacer* pacer, UINT64 nowMs, UINT64 damagedPixels,
                              UINT64 totalPixels)
{
	if (pacer && pacer->burstActive && (nowMs >= pacer->burstEndMs))
		shadow_pacer_end_large_refresh_burst(pacer, pacer->burstEndMs, 1);
	const BOOL quiet = !pacer || !pacer->largeRefreshDiagnosticM ||
	                   !pacer->havePublicationDamage ||
	                   (nowMs >= pacer->lastPublicationDamageMs &&
	                    nowMs - pacer->lastPublicationDamageMs >=
	                        SHADOW_PACER_LARGE_REFRESH_QUIET_REARM_MS);
	if (pacer && pacer->largeRefreshDiagnosticM && totalPixels &&
	    damagedPixels * 100U >= totalPixels * pacer->largeDamagePercent)
	{
		pacer->lastPublicationDamageMs = nowMs;
		pacer->havePublicationDamage = TRUE;
	}
	if (!pacer || !pacer->largeRefreshBurstEnabled || !totalPixels || pacer->burstActive ||
	    (nowMs < pacer->burstEligibleMs) ||
	    (damagedPixels * 100U < totalPixels * pacer->largeDamagePercent) || !quiet ||
	    pacer->pressure ||
	    (pacer->haveQueue && nowMs >= pacer->lastQueueMs &&
	     nowMs - pacer->lastQueueMs <= 100 && pacer->lastQueued >= pacer->baseMaxCredit))
		return FALSE;
	/* Do not retroactively refill at the burst rate. Credit earned before this
	 * publication remains bounded by the base bucket. */
	if (!pacer->initialized)
	{
		pacer->initialized = TRUE;
		pacer->lastMs = nowMs;
		pacer->lastRampMs = nowMs;
	}
	else if (nowMs > pacer->lastMs)
	{
		const UINT64 elapsed = (nowMs - pacer->lastMs > 1000) ? 1000 : nowMs - pacer->lastMs;
		pacer->credit += pacer->baseRate * (double)elapsed / 1000.0;
		if (pacer->credit > pacer->baseMaxCredit)
			pacer->credit = pacer->baseMaxCredit;
		pacer->lastMs = nowMs;
	}
	pacer->burstActive = TRUE;
	pacer->rate = pacer->burstRate;
	pacer->maxCredit = pacer->burstMaxCredit;
	pacer->burstBytes = 0;
	pacer->admittedBytes = 0;
	pacer->burstStartMs = nowMs;
	pacer->burstEndReason = 0;
	pacer->burstEndMs = nowMs + pacer->burstDurationMs;
	pacer->burstEligibleMs = pacer->burstEndMs + pacer->burstCooldownMs;
	return TRUE;
}

double shadow_pacer_credit_limit(const shadowPacer* pacer)
{
	return pacer->fixed ? pacer->maxCredit : shadow_pacer_burst(pacer);
}

void shadow_pacer_note_time(shadowPacer* pacer, UINT64 nowMs, BOOL demand)
{
	if (pacer->fixed && pacer->burstActive && (nowMs >= pacer->burstEndMs))
		shadow_pacer_end_large_refresh_burst(pacer, pacer->burstEndMs, 1);
	if (!pacer->initialized)
	{
		pacer->initialized = TRUE;
		pacer->lastMs = nowMs;
		pacer->lastRampMs = nowMs;
	}
	if (nowMs > pacer->lastMs)
	{
		/* Cap elapsed credit after a static desktop or suspended session. */
		const UINT64 elapsed = (nowMs - pacer->lastMs > 1000) ? 1000 : nowMs - pacer->lastMs;
		/* M earns the temporary rate only while bitmap work is pending. */
		if (!(pacer->largeRefreshDiagnosticM && pacer->burstActive && !demand))
			pacer->credit += pacer->rate * (double)elapsed / 1000.0;
		const double burst = pacer->fixed ? pacer->maxCredit : shadow_pacer_burst(pacer);
		if (pacer->credit > burst)
			pacer->credit = burst;
		pacer->lastMs = nowMs;
	}
}

void shadow_pacer_observe(shadowPacer* pacer, UINT64 nowMs, BOOL queueValid,
                          UINT32 queuedBytes, BOOL writeBlocked, BOOL demand)
{
	shadow_pacer_note_time(pacer, nowMs, demand);
	if (pacer->fixed)
	{
		/* Fixed mode is deliberately independent of the adaptive queue estimator.
		 * A blocked transport still stops admission and discards accumulated credit. */
		pacer->pressure = writeBlocked;
		if (queueValid && (!pacer->haveQueue || nowMs - pacer->lastQueueMs >= 100))
		{
			pacer->lastQueued = queuedBytes;
			pacer->lastQueueMs = nowMs;
			pacer->haveQueue = TRUE;
			pacer->submittedSinceQueue = 0;
		}
		else if (!queueValid)
			pacer->haveQueue = FALSE;
		/* An actual local backlog is evidence against spending the temporary
		 * allowance. Preserve the original nominal cooldown after an early end. */
		if (pacer->burstActive && queueValid &&
		    queuedBytes >= PACER_LARGE_REFRESH_QUEUE_LIMIT)
			shadow_pacer_end_large_refresh_burst(pacer, nowMs, 4);
		if (pacer->pressure)
			pacer->credit = 0;
		return;
	}
	if (!queueValid)
	{
		if (pacer->rate > PACER_INITIAL_RATE)
			pacer->rate = PACER_INITIAL_RATE;
		if (pacer->credit > 16384.0)
			pacer->credit = 16384.0;
	}

	/* SO_NWRITE is only the local send socket. Its zero value does not acknowledge
	 * delivery by sshd, the WAN, or the RDP client. */
	double high = (pacer->rate * 0.15 > 12288.0) ? pacer->rate * 0.15 : 12288.0;
	double low = (pacer->rate * 0.05 > 4096.0) ? pacer->rate * 0.05 : 4096.0;
	if (high > 65536.0)
		high = 65536.0;
	if (low > 16384.0)
		low = 16384.0;
	/* Only a positive queue can reveal a local drain rate. A zero queue says
	 * nothing about bytes already consumed and buffered by a forwarding peer. */
	if (queueValid && pacer->haveQueue && pacer->lastQueued > 0 &&
	    queuedBytes > 0 && nowMs - pacer->lastQueueMs >= 100)
	{
		const double drained = (double)pacer->lastQueued + pacer->submittedSinceQueue -
		                       queuedBytes;
		if (drained >= 0)
		{
			const double sample = drained * 1000.0 / (nowMs - pacer->lastQueueMs);
			pacer->drainRate = pacer->drainRate == 0 ? sample :
			                   (pacer->drainRate * 0.75 + sample * 0.25);
		}
	}
	const BOOL overloaded = writeBlocked ||
	                        (queueValid && queuedBytes >= (UINT32)(pacer->pressure ? low : high));
	if (overloaded)
	{
		pacer->pressure = TRUE;
		if (!pacer->lastDecreaseMs || nowMs - pacer->lastDecreaseMs >= 100)
		{
			pacer->rate *= 0.65;
			if (pacer->drainRate > 0 && pacer->rate > pacer->drainRate * 1.1)
				pacer->rate = pacer->drainRate * 1.1;
			if (pacer->rate < PACER_MIN_RATE)
				pacer->rate = PACER_MIN_RATE;
			pacer->lastDecreaseMs = nowMs;
			pacer->lastRampMs = nowMs;
		}
	}
	else
	{
		if (queueValid && pacer->pressure && pacer->drainRate > pacer->rate)
			pacer->rate = pacer->drainRate * 0.9;
		pacer->pressure = FALSE;
		/* A bounded probe permits a fast LAN to become transparent. This cannot
		 * prove that a forwarding process has delivered its own buffered bytes. */
		/* Without the Darwin socket signal, retain the conservative rate;
		 * otherwise a forwarding process could absorb unlimited blind probes. */
		if (demand && queueValid && pacer->credit < shadow_pacer_burst(pacer) * 0.75 &&
		    nowMs - pacer->lastRampMs >= 500)
		{
			pacer->rate *= 1.5;
			if (pacer->rate > PACER_MAX_RATE)
				pacer->rate = PACER_MAX_RATE;
			pacer->lastRampMs = nowMs;
		}
		if (!demand)
			pacer->lastRampMs = nowMs;
	}
	if (queueValid && (!pacer->haveQueue || nowMs - pacer->lastQueueMs >= 100))
	{
		pacer->lastQueued = queuedBytes;
		pacer->lastQueueMs = nowMs;
		pacer->haveQueue = TRUE;
		pacer->submittedSinceQueue = 0;
	}
	else
	{
		if (!queueValid)
			pacer->haveQueue = FALSE;
	}

	if (pacer->pressure)
		pacer->credit = 0;
}

void shadow_pacer_cancel_wait(shadowPacer* pacer)
{
	if (!pacer)
		return;
	pacer->requiredCredit = 0;
	pacer->admittedBytes = 0;
}

BOOL shadow_pacer_preflight(shadowPacer* pacer, UINT32 bytes)
{
	if (!pacer)
		return FALSE;
	if (!pacer->fixed)
		return shadow_pacer_can_submit(pacer, bytes);
	const double required = (pacer->requiredCredit > bytes) ? pacer->requiredCredit : bytes;
	if (required > pacer->maxCredit)
		return FALSE;
	if (pacer->pressure || pacer->credit < required)
	{
		if (pacer->requiredCredit < bytes)
			pacer->requiredCredit = bytes;
		return FALSE;
	}
	return TRUE;
}

BOOL shadow_pacer_can_submit(shadowPacer* pacer, UINT32 bytes)
{
	if (pacer->fixed)
	{
		if (pacer->burstActive &&
		    ((UINT64)bytes > pacer->burstByteCap - pacer->burstBytes))
		{
			/* End unusable residual allowance and retry this same operation at
			 * the base rate. The nominal end still governs rearm time. */
			shadow_pacer_end_large_refresh_burst(pacer, pacer->lastMs, 3);
		}
		double required = bytes;
		if (required > pacer->maxCredit)
		{
			pacer->requiredCredit = pacer->maxCredit;
			return FALSE;
		}
		if (required < pacer->requiredCredit)
			required = pacer->requiredCredit;
		if (!pacer->pressure && pacer->credit >= required)
		{
			pacer->admittedBytes = bytes;
			return TRUE;
		}
		pacer->requiredCredit = required;
		return FALSE;
	}
	const double required = (double)bytes < shadow_pacer_burst(pacer)
	                            ? (double)bytes : shadow_pacer_burst(pacer);
	return !pacer->pressure && pacer->credit >= required;
}

void shadow_pacer_submitted(shadowPacer* pacer, UINT32 bytes)
{
	pacer->credit -= bytes;
	pacer->requiredCredit = 0;
	pacer->submittedSinceQueue += bytes;
	if (pacer->burstActive)
	{
		/* M caps pre-serialization admission, including bitmap framing allowance.
		 * The measured transport delta remains the credit charge. */
		pacer->burstBytes += pacer->largeRefreshDiagnosticM ? pacer->admittedBytes : bytes;
		if (pacer->burstBytes >= pacer->burstByteCap)
			shadow_pacer_end_large_refresh_burst(pacer, pacer->lastMs, 2);
	}
	pacer->admittedBytes = 0;
}

UINT32 shadow_pacer_wait_ms(const shadowPacer* pacer)
{
	if (pacer->pressure)
		return 8;
	if (pacer->fixed)
	{
		const double target = pacer->requiredCredit > 0 ? pacer->requiredCredit : 2048.0;
		if (pacer->credit >= target)
			return 1;
		const double missing = target - pacer->credit;
		UINT32 ms = (UINT32)(missing * 1000.0 / pacer->rate);
		if (ms < 1)
			ms = 1;
		if (ms > 20)
			ms = 20;
		return ms;
	}
	if (pacer->credit >= 16384.0)
		return 1;
	const double missing = 16384.0 - pacer->credit;
	UINT32 ms = (UINT32)(missing * 1000.0 / pacer->rate);
	if (ms < 1)
		ms = 1;
	if (ms > 20)
		ms = 20;
	return ms;
}
