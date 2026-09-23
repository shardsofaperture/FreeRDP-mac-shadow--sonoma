/* Deterministic Mac bitmap pacing model. Licensed under the Apache License, Version 2.0. */
#include <stdio.h>
#include <stdlib.h>

#include "../shadow_pacer.h"

#define TILE_BYTES 4096U
#define STEP_MS 10U

#define CHECK(x)                                                                      \
	do                                                                                \
	{                                                                                 \
		if (!(x))                                                                  \
		{                                                                         \
			fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #x); \
			return 1;                                                         \
		}                                                                         \
	} while (0)

typedef struct
{
	shadowPacer pacer;
	double socketBytes;
	double forwarderBytes;
	double forwarderCapacity;
	double maxBacklog;
	double maxLateBacklog;
	UINT64 submittedBytes;
	UINT64 submittedLateBytes;
	UINT32 generations;
	UINT32 lastSentGeneration;
	UINT32 replacedGenerations;
	UINT32 pauses;
	UINT64 firstPressureMs;
	BOOL sawPressure;
} Model;

static void model_step(Model* model, UINT64 nowMs, double pathBytesPerSecond, BOOL motion)
{
	/* sshd can absorb a limited amount before its WAN sender slows its read
	 * from the accepted RDP socket. Neither buffer is acknowledged to RDP. */
	const double drained = pathBytesPerSecond * STEP_MS / 1000.0;
	model->forwarderBytes = model->forwarderBytes > drained ?
	                            model->forwarderBytes - drained : 0;
	const double capacity = model->forwarderCapacity > 0 ?
	                            model->forwarderCapacity : 65536.0;
	const double room = capacity - model->forwarderBytes;
	const double transfer = model->socketBytes < room ? model->socketBytes : room;
	model->socketBytes -= transfer;
	model->forwarderBytes += transfer;
	if (motion)
		model->generations++;
	shadow_pacer_observe(&model->pacer, nowMs, TRUE, (UINT32)model->socketBytes,
	                     model->socketBytes >= 131072.0, motion);
	if (model->pacer.pressure && !model->sawPressure)
	{
		model->firstPressureMs = nowMs;
		model->sawPressure = TRUE;
	}
	if (motion)
	{
		if (shadow_pacer_can_submit(&model->pacer, TILE_BYTES))
		{
			/* One latest-state tile per generated frame. Skipped generations
			 * were never serialized and therefore cannot be replayed. */
			model->replacedGenerations += model->generations - model->lastSentGeneration - 1;
			model->lastSentGeneration = model->generations;
			shadow_pacer_submitted(&model->pacer, TILE_BYTES);
			model->socketBytes += TILE_BYTES;
			model->submittedBytes += TILE_BYTES;
			if (nowMs >= 10000)
				model->submittedLateBytes += TILE_BYTES;
		}
		else
			model->pauses++;
	}
	const double backlog = model->socketBytes + model->forwarderBytes;
	if (backlog > model->maxBacklog)
		model->maxBacklog = backlog;
	if (nowMs >= 10000 && backlog > model->maxLateBacklog)
		model->maxLateBacklog = backlog;
}

static int slow_path(void)
{
	Model model = { 0 };
	shadow_pacer_reset(&model.pacer);
	for (UINT64 ms = 0; ms < 60000; ms += STEP_MS)
		model_step(&model, ms, 150000.0, TRUE);
	printf("slow: rate=%.0f B/s sent=%.0f B/s maxBacklog=%.0f lateMax=%.0f "
	       "replaced=%u pauses=%u\n", model.pacer.rate,
	       model.submittedLateBytes / 50.0, model.maxBacklog, model.maxLateBacklog,
	       model.replacedGenerations, model.pauses);
	CHECK(model.maxBacklog < 250000.0);
	CHECK(model.sawPressure);
	CHECK(model.maxLateBacklog < 200000.0);
	CHECK(model.submittedLateBytes / 50.0 < 190000.0);
	CHECK(model.submittedLateBytes / 50.0 > 100000.0);
	CHECK(model.replacedGenerations > 1000);
	CHECK(model.lastSentGeneration > 5900);
	return 0;
}

static int fast_path(void)
{
	Model model = { 0 };
	shadow_pacer_reset(&model.pacer);
	for (UINT64 ms = 0; ms < 10000; ms += STEP_MS)
		model_step(&model, ms, 20000000.0, TRUE);
	printf("fast: rate=%.0f B/s sent=%.0f B/s replaced=%u\n", model.pacer.rate,
	       model.submittedBytes / 10.0, model.replacedGenerations);
	CHECK(model.lastSentGeneration >= 990);
	CHECK(model.replacedGenerations < 180);
	CHECK(model.pacer.rate > 400000.0);
	CHECK(model.pacer.rate < 2000000.0);
	return 0;
}

static int degrade_and_recover(void)
{
	Model model = { 0 };
	shadow_pacer_reset(&model.pacer);
	for (UINT64 ms = 0; ms < 10000; ms += STEP_MS)
		model_step(&model, ms, 20000000.0, TRUE);
	const double fastRate = model.pacer.rate;
	for (UINT64 ms = 10000; ms < 20000; ms += STEP_MS)
		model_step(&model, ms, 150000.0, TRUE);
	const double slowRate = model.pacer.rate;
	const double peak = model.maxLateBacklog;
	const UINT64 beforeRecovery = model.submittedBytes;
	for (UINT64 ms = 20000; ms < 30000; ms += STEP_MS)
		model_step(&model, ms, 20000000.0, TRUE);
	printf("transition: fastRate=%.0f slowRate=%.0f recoveredRate=%.0f "
	       "degradePeak=%.0f recoverySent=%.0f B/s\n", fastRate, slowRate,
	       model.pacer.rate, peak, (model.submittedBytes - beforeRecovery) / 10.0);
	CHECK(slowRate < fastRate * 0.5);
	CHECK(model.sawPressure && model.firstPressureMs < 11000);
	CHECK(peak < 250000.0);
	CHECK(model.pacer.rate > slowRate * 2.0);
	CHECK((model.submittedBytes - beforeRecovery) / 10.0 > 300000.0);
	return 0;
}

static int static_desktop(void)
{
	Model model = { 0 };
	shadow_pacer_reset(&model.pacer);
	for (UINT64 ms = 0; ms < 30000; ms += STEP_MS)
		model_step(&model, ms, 150000.0, FALSE);
	CHECK(model.submittedBytes == 0);
	CHECK(model.pacer.rate == 150000.0);
	return 0;
}

static int static_full_refresh(void)
{
	shadowPacer pacer;
	shadow_pacer_reset(&pacer);
	UINT32 remaining = 2U * 1024U * 1024U;
	UINT64 completeMs = 0;
	for (UINT64 ms = 0; ms < 10000; ms += STEP_MS)
	{
		/* The fast client consumes every prior pass; pending static damage
		 * keeps the controller active until the whole refresh is submitted. */
		shadow_pacer_observe(&pacer, ms, TRUE, 0, FALSE, remaining > 0);
		for (UINT32 n = 0; n < 4 && remaining > 0; n++)
		{
			if (!shadow_pacer_can_submit(&pacer, TILE_BYTES))
				break;
			shadow_pacer_submitted(&pacer, TILE_BYTES);
			remaining -= TILE_BYTES;
		}
		if (!remaining && !completeMs)
			completeMs = ms;
	}
	printf("static refresh: 2 MiB submitted in %llu ms\n",
	       (unsigned long long)completeMs);
	CHECK(remaining == 0);
	CHECK(completeMs < 5000);
	return 0;
}

static int local_zero_is_not_delivery(void)
{
	shadowPacer pacer;
	shadow_pacer_reset(&pacer);
	shadow_pacer_observe(&pacer, 0, TRUE, 0, FALSE, TRUE);
	CHECK(shadow_pacer_can_submit(&pacer, TILE_BYTES));
	shadow_pacer_submitted(&pacer, TILE_BYTES);
	const double afterSend = pacer.credit;
	shadow_pacer_observe(&pacer, 1, TRUE, 0, FALSE, TRUE);
	CHECK(pacer.drainRate == 0);
	CHECK(pacer.credit < afterSend + 151.0);
	shadow_pacer_observe(&pacer, 2, TRUE, 0, TRUE, TRUE);
	CHECK(!shadow_pacer_can_submit(&pacer, 1));
	CHECK(pacer.pressure);
	shadow_pacer_reset(&pacer);
	for (UINT64 ms = 0; ms <= 2000; ms += STEP_MS)
	{
		shadow_pacer_observe(&pacer, ms, TRUE, 0, FALSE, TRUE);
		if (shadow_pacer_can_submit(&pacer, TILE_BYTES))
			shadow_pacer_submitted(&pacer, TILE_BYTES);
	}
	CHECK(pacer.rate > 150000.0);
	shadow_pacer_observe(&pacer, 2010, FALSE, 0, FALSE, TRUE);
	CHECK(pacer.rate == 150000.0);
	return 0;
}

static int hidden_forwarder_limit(void)
{
	Model model = { 0 };
	model.forwarderCapacity = 648000.0; /* Seen in the local sshd Recv-Q. */
	shadow_pacer_reset(&model.pacer);
	for (UINT64 ms = 0; ms < 30000; ms += STEP_MS)
		model_step(&model, ms, 150000.0, TRUE);
	printf("large forwarder: maxBacklog=%.0f bytes (unobservable from SO_NWRITE)\n",
	       model.maxBacklog);
	CHECK(model.maxBacklog > 500000.0);
	return 0;
}

static int unpaced_comparison(void)
{
	double socketBytes = 0;
	double forwarderBytes = 0;
	for (UINT64 ms = 0; ms < 60000; ms += STEP_MS)
	{
		forwarderBytes = forwarderBytes > 1500.0 ? forwarderBytes - 1500.0 : 0;
		const double room = 65536.0 - forwarderBytes;
		const double transfer = socketBytes < room ? socketBytes : room;
		socketBytes -= transfer;
		forwarderBytes += transfer;
		socketBytes += TILE_BYTES;
	}
	printf("unpaced model: backlog=%.0f bytes after 60s\n",
	       socketBytes + forwarderBytes);
	CHECK(socketBytes + forwarderBytes > 10000000.0);
	return 0;
}

/* Keep the transport reservoirs distinct. Successful application submission
 * enters the buffered BIO; a full accepted socket can leave serialized bytes
 * there. sshd reads the local socket quickly until its own reservoir fills. */
typedef struct
{
	shadowPacer pacer;
	double bioBytes;
	double socketBytes;
	double hiddenBytes;
	double socketCapacity;
	double hiddenCapacity;
	double maxBio;
	double maxSocket;
	double maxHidden;
	double maxCombined;
	UINT64 submittedBytes;
	UINT32 remainingRefresh;
	UINT32 replacedGenerations;
	UINT32 lastSentGeneration;
	UINT32 generation;
	UINT32 pauses;
	UINT32 blockedTicks;
	UINT32 blockedTransitions;
	UINT32 rateDrops;
	BOOL wasBlocked;
	UINT64 submittedMs;
	UINT64 drainedMs;
} SocketCapModel;

static double model_min(double left, double right)
{
	return left < right ? left : right;
}

static void socket_model_flush(SocketCapModel* model)
{
	const double room = model->socketCapacity - model->socketBytes;
	const double moved = model_min(model->bioBytes, room);
	model->bioBytes -= moved;
	model->socketBytes += moved;
}

static void socket_model_step(SocketCapModel* model, UINT64 nowMs, UINT32 tickMs,
                              double pathBytesPerSecond, BOOL motion)
{
	const double drained = model_min(model->hiddenBytes,
	                                 pathBytesPerSecond * tickMs / 1000.0);
	model->hiddenBytes -= drained;
	const double room = model->hiddenCapacity - model->hiddenBytes;
	const double moved = model_min(model->socketBytes, room);
	model->socketBytes -= moved;
	model->hiddenBytes += moved;
	socket_model_flush(model);

	const BOOL newCapture = motion && nowMs % 10U == 0;
	if (newCapture)
		model->generation++;
	const BOOL pending = motion || model->remainingRefresh > 0;
	const BOOL blocked = model->bioBytes > 0;
	if (blocked)
	{
		model->blockedTicks++;
		if (!model->wasBlocked)
			model->blockedTransitions++;
	}
	model->wasBlocked = blocked;
	const double priorRate = model->pacer.rate;
	shadow_pacer_observe(&model->pacer, nowMs, TRUE, (UINT32)model->socketBytes,
	                     blocked, pending);
	if (model->pacer.rate < priorRate)
		model->rateDrops++;
	if (newCapture || model->remainingRefresh > 0)
	{
		const UINT32 limit = newCapture ? 1U : 4U;
		for (UINT32 n = 0; n < limit; n++)
		{
			if (model->remainingRefresh == 0 && !newCapture)
				break;
			if (!shadow_pacer_can_submit(&model->pacer, TILE_BYTES) || model->bioBytes > 0)
			{
				model->pauses++;
				break;
			}
			if (newCapture)
			{
				model->replacedGenerations += model->generation -
				                              model->lastSentGeneration - 1;
				model->lastSentGeneration = model->generation;
			}
			else
				model->remainingRefresh -= TILE_BYTES;
			shadow_pacer_submitted(&model->pacer, TILE_BYTES);
			model->submittedBytes += TILE_BYTES;
			model->bioBytes += TILE_BYTES;
			socket_model_flush(model);
			if (newCapture)
				break;
		}
	}
	if (model->maxBio < model->bioBytes)
		model->maxBio = model->bioBytes;
	if (model->maxSocket < model->socketBytes)
		model->maxSocket = model->socketBytes;
	if (model->maxHidden < model->hiddenBytes)
		model->maxHidden = model->hiddenBytes;
	const double combined = model->bioBytes + model->socketBytes + model->hiddenBytes;
	if (model->maxCombined < combined)
		model->maxCombined = combined;
	if (!model->remainingRefresh && !model->submittedMs)
		model->submittedMs = nowMs;
	if (model->submittedMs && combined == 0 && !model->drainedMs)
		model->drainedMs = nowMs;
}

static int socket_cap_scenarios(void)
{
	/* The control capacity is the isolated accepted TCP default observed on
	 * this host. Other kernels and sessions must read back their own value. */
	const UINT32 capacities[] = { 146988U, 128U * 1024U, 64U * 1024U,
	                              32U * 1024U, 16U * 1024U };
	for (size_t i = 0; i < sizeof(capacities) / sizeof(capacities[0]); i++)
	{
		SocketCapModel slow = { 0 };
		slow.socketCapacity = capacities[i];
		slow.hiddenCapacity = 648U * 1024U;
		shadow_pacer_reset(&slow.pacer);
		for (UINT64 ms = 0; ms < 30000; ms += STEP_MS)
			socket_model_step(&slow, ms, STEP_MS, 150U * 1024U, TRUE);
		printf("socket cap %u slow: socket=%.0f hidden=%.0f BIO=%.0f combined=%.0f "
		       "queuedMs=%.0f pauses=%u blockedTicks=%u\n", capacities[i], slow.maxSocket,
		       slow.maxHidden, slow.maxBio, slow.maxCombined,
		       slow.maxCombined * 1000.0 / (150U * 1024U), slow.pauses,
		       slow.blockedTicks);
		CHECK(slow.maxSocket <= slow.socketCapacity);
		CHECK(slow.maxHidden <= slow.hiddenCapacity);
		CHECK(slow.maxHidden > 500U * 1024U);
		CHECK(slow.maxCombined >= slow.maxHidden);
		CHECK(slow.replacedGenerations > 100);

		SocketCapModel fast = { 0 };
		fast.socketCapacity = capacities[i];
		fast.hiddenCapacity = 64U * 1024U;
		shadow_pacer_reset(&fast.pacer);
		for (UINT64 ms = 0; ms < 10000; ms++)
			socket_model_step(&fast, ms, 1, 20000000.0, TRUE);
		printf("socket cap %u fast: sent=%.0f B/s rate=%.0f B/s "
		       "blockedTransitions=%u blockedTicks=%u rateDrops=%u\n", capacities[i],
		       fast.submittedBytes / 10.0, fast.pacer.rate,
		       fast.blockedTransitions, fast.blockedTicks, fast.rateDrops);
		CHECK(fast.submittedBytes > 3500000U);
		CHECK(fast.blockedTicks == 0);
		CHECK(fast.rateDrops == 0);

		SocketCapModel refresh = { 0 };
		refresh.socketCapacity = capacities[i];
		refresh.hiddenCapacity = 64U * 1024U;
		refresh.remainingRefresh = 2U * 1024U * 1024U;
		shadow_pacer_reset(&refresh.pacer);
		for (UINT64 ms = 0; ms < 10000 && !refresh.drainedMs; ms++)
			socket_model_step(&refresh, ms, 1, 20000000.0, FALSE);
		printf("socket cap %u refresh: submittedMs=%llu drainedMs=%llu "
		       "blockedTransitions=%u\n", capacities[i],
		       (unsigned long long)refresh.submittedMs,
		       (unsigned long long)refresh.drainedMs, refresh.blockedTransitions);
		CHECK(refresh.remainingRefresh == 0);
		CHECK(refresh.drainedMs > 0 && refresh.drainedMs < 5000);
	}
	return 0;
}

static int fixed_configuration(void)
{
	UINT32 rateKiB = 99;
	CHECK(shadow_pacer_parse_fixed_rate_kib(NULL, &rateKiB) && rateKiB == 0);
	CHECK(shadow_pacer_parse_fixed_rate_kib("0", &rateKiB) && rateKiB == 0);
	CHECK(shadow_pacer_parse_fixed_rate_kib("150", &rateKiB) && rateKiB == 150);
	CHECK(shadow_pacer_parse_fixed_rate_kib("175", &rateKiB) && rateKiB == 175);
	CHECK(shadow_pacer_parse_fixed_rate_kib("200", &rateKiB) && rateKiB == 200);
	CHECK(shadow_pacer_parse_fixed_rate_kib("250", &rateKiB) && rateKiB == 250);
	CHECK(shadow_pacer_parse_fixed_rate_kib("300", &rateKiB) && rateKiB == 300);
	CHECK(shadow_pacer_parse_fixed_rate_kib("400", &rateKiB) && rateKiB == 400);
	CHECK(!shadow_pacer_parse_fixed_rate_kib("100", &rateKiB));
	CHECK(!shadow_pacer_parse_fixed_rate_kib("401", &rateKiB));
	CHECK(!shadow_pacer_parse_fixed_rate_kib("150K", &rateKiB));
	CHECK(!shadow_pacer_parse_fixed_rate_kib("", &rateKiB));
	CHECK(!shadow_pacer_parse_fixed_rate_kib("150", NULL));

	shadowPacer pacer;
	CHECK(shadow_pacer_set_fixed_rate_kib(&pacer, 150));
	CHECK(pacer.fixed && pacer.rate == 150U * 1024U);
	/* 100 ms is 15360 bytes. The small floor admits one legal atomic
	 * 64x64 planar tile plus framing without permitting a second tile. */
	CHECK(pacer.maxCredit == 64U * 64U * 4U + 30U + 64U);
	CHECK(pacer.credit == pacer.maxCredit);
	CHECK(shadow_pacer_set_fixed_rate_kib(&pacer, 175));
	CHECK(pacer.fixed && pacer.rate == 175U * 1024U);
	CHECK(pacer.maxCredit == 175U * 1024U / 10U);
	CHECK(shadow_pacer_set_fixed_rate_kib(&pacer, 200));
	CHECK(pacer.fixed && pacer.rate == 200U * 1024U);
	CHECK(pacer.maxCredit == 20U * 1024U);
	CHECK(shadow_pacer_set_fixed_rate_kib(&pacer, 250));
	CHECK(pacer.fixed && pacer.rate == 250U * 1024U);
	CHECK(pacer.maxCredit == 25U * 1024U);
	for (UINT32 selected = 150; selected <= 400; selected += 50)
	{
		if (selected == 350)
			continue;
		CHECK(shadow_pacer_set_fixed_rate_kib(&pacer, selected));
		CHECK(pacer.fixed && pacer.rate == (double)selected * 1024.0);
		CHECK(pacer.baseRate == pacer.rate);
		CHECK(!pacer.largeRefreshBurstEnabled && !pacer.burstActive);
		CHECK(pacer.burstRate == 0 && pacer.burstDurationMs == 0);
	}
	CHECK(!shadow_pacer_set_fixed_rate_kib(&pacer, 100));
	CHECK(!shadow_pacer_set_fixed_rate_kib(NULL, 150));
	return 0;
}

static int fixed_no_adaptive_probe(UINT32 rateKiB)
{
	shadowPacer pacer;
	CHECK(shadow_pacer_set_fixed_rate_kib(&pacer, rateKiB));
	const double configured = pacer.rate;
	for (UINT64 ms = 0; ms <= 10000; ms += 10)
	{
		shadow_pacer_observe(&pacer, ms, TRUE, 0, FALSE, TRUE);
		if (shadow_pacer_can_submit(&pacer, TILE_BYTES))
			shadow_pacer_submitted(&pacer, TILE_BYTES);
	}
	CHECK(pacer.rate == configured);
	shadow_pacer_observe(&pacer, 10010, TRUE, 131072, TRUE, TRUE);
	CHECK(pacer.rate == configured);
	CHECK(pacer.pressure);
	shadow_pacer_observe(&pacer, 10020, TRUE, 0, FALSE, TRUE);
	CHECK(pacer.rate == configured && !pacer.pressure);
	return 0;
}

static int fixed_sustained_rate(UINT32 rateKiB)
{
	shadowPacer pacer;
	CHECK(shadow_pacer_set_fixed_rate_kib(&pacer, rateKiB));
	const double configured = rateKiB * 1024.0;
	const double initialCredit = pacer.maxCredit;
	UINT64 admitted = 0;
	for (UINT64 ms = 0; ms < 60000; ms++)
	{
		shadow_pacer_observe(&pacer, ms, TRUE, 0, FALSE, TRUE);
		if (shadow_pacer_can_submit(&pacer, 1024))
		{
			shadow_pacer_submitted(&pacer, 1024);
			admitted += 1024;
		}
	}
	const double expected = configured * 60.0;
	printf("fixed %u KiB/s: admitted=%llu expected=%.0f burst=%.0f\n", rateKiB,
	       (unsigned long long)admitted, expected, pacer.maxCredit);
	CHECK(pacer.rate == configured);
	CHECK((double)admitted >= expected - 2048.0);
	CHECK((double)admitted <= expected + initialCredit);
	return 0;
}

static int fixed_burst_and_idle(UINT32 rateKiB)
{
	shadowPacer pacer;
	CHECK(shadow_pacer_set_fixed_rate_kib(&pacer, rateKiB));
	const UINT32 burst = (UINT32)pacer.maxCredit;
	CHECK(shadow_pacer_can_submit(&pacer, burst));
	shadow_pacer_submitted(&pacer, burst);
	CHECK(!shadow_pacer_can_submit(&pacer, 1));
	shadow_pacer_observe(&pacer, 0, TRUE, 0, FALSE, FALSE);
	shadow_pacer_observe(&pacer, 60000, TRUE, 0, FALSE, FALSE);
	CHECK(pacer.credit == pacer.maxCredit);
	CHECK(pacer.credit < pacer.rate);
	CHECK(shadow_pacer_can_submit(&pacer, burst));
	shadow_pacer_submitted(&pacer, burst - 1000U);
	CHECK(!shadow_pacer_can_submit(&pacer, 2048));
	CHECK(pacer.requiredCredit == 2048.0);
	CHECK(!shadow_pacer_can_submit(&pacer, burst + 1U));
	return 0;
}

static int fixed_replacement_and_fairness(UINT32 rateKiB)
{
	shadowPacer pacer;
	CHECK(shadow_pacer_set_fixed_rate_kib(&pacer, rateKiB));
	UINT32 newestGeneration = 0;
	UINT32 pendingGeneration = 0;
	UINT32 lastSentGeneration = 0;
	UINT32 coalesced = 0;
	UINT32 deferrals = 0;
	UINT32 inputPasses = 0;
	UINT32 channelPasses = 0;
	UINT32 encodedOperations = 0;
	for (UINT64 ms = 0; ms < 5000; ms += 10)
	{
		/* A single replaceable pending slot models shadow_bitmap_stage: it
		 * replaces latest pixels and never appends an encoded PDU. */
		newestGeneration++;
		if (pendingGeneration != 0)
			coalesced++;
		pendingGeneration = newestGeneration;
		inputPasses++;
		channelPasses++;
		shadow_pacer_observe(&pacer, ms, TRUE, 0, FALSE, TRUE);
		if (!shadow_pacer_can_submit(&pacer, TILE_BYTES))
		{
			deferrals++;
			continue;
		}
		lastSentGeneration = pendingGeneration;
		pendingGeneration = 0;
		shadow_pacer_submitted(&pacer, TILE_BYTES);
		encodedOperations++;
	}
	printf("fixed %u replacement: newest=%u lastSent=%u encoded=%u coalesced=%u "
	       "deferrals=%u\n", rateKiB, newestGeneration, lastSentGeneration,
	       encodedOperations, coalesced, deferrals);
	CHECK(deferrals > 0 && coalesced > 0);
	CHECK(encodedOperations < newestGeneration);
	CHECK(lastSentGeneration > newestGeneration - 10U);
	CHECK(inputPasses == newestGeneration);
	CHECK(channelPasses == newestGeneration);
	return 0;
}

static int large_refresh_burst_policy(void)
{
	shadowPacer nonBaseline;
	CHECK(shadow_pacer_set_fixed_rate_kib(&nonBaseline, 175));
	CHECK(!shadow_pacer_enable_large_refresh_burst(&nonBaseline));

	shadowPacer baseline;
	CHECK(shadow_pacer_set_fixed_rate_kib(&baseline, 150));
	CHECK(!shadow_pacer_note_damage(&baseline, 0, 1000000, 1000000));
	CHECK(!baseline.burstActive && baseline.rate == 150U * 1024U);
	CHECK(baseline.maxCredit == 64U * 64U * 4U + 30U + 64U);

	shadowPacer pacer;
	CHECK(shadow_pacer_set_fixed_rate_kib(&pacer, 150));
	CHECK(shadow_pacer_enable_large_refresh_burst(&pacer));
	CHECK(pacer.burstRate == 300U * 1024U);
	CHECK(pacer.burstDurationMs == 150U);
	CHECK(pacer.burstByteCap == 48U * 1024U);
	CHECK(pacer.burstCooldownMs == 1000U);
	CHECK(pacer.largeDamagePercent == 25U);
	const double baseCredit = pacer.credit;
	CHECK(!shadow_pacer_note_damage(&pacer, 1000, 249999, 1000000));
	CHECK(!pacer.burstActive && pacer.rate == 150U * 1024U);
	CHECK(shadow_pacer_note_damage(&pacer, 1000, 250000, 1000000));
	CHECK(pacer.burstActive && pacer.rate == 300U * 1024U);
	CHECK(pacer.credit == baseCredit); /* Entry does not mint idle credit. */
	CHECK(!shadow_pacer_note_damage(&pacer, 1100, 1000000, 1000000));
	CHECK(pacer.burstEndMs == 1150 && pacer.burstEligibleMs == 2150);
	shadow_pacer_observe(&pacer, 1149, TRUE, 0, FALSE, TRUE);
	CHECK(pacer.burstActive);
	shadow_pacer_observe(&pacer, 1150, TRUE, 0, FALSE, TRUE);
	CHECK(!pacer.burstActive && pacer.rate == 150U * 1024U);
	CHECK(pacer.maxCredit == pacer.baseMaxCredit);
	CHECK(!shadow_pacer_note_damage(&pacer, 2149, 1000000, 1000000));
	CHECK(shadow_pacer_note_damage(&pacer, 2150, 1000000, 1000000));
	return 0;
}

static int large_refresh_byte_and_idle_bounds(void)
{
	shadowPacer pacer;
	CHECK(shadow_pacer_set_fixed_rate_kib(&pacer, 150));
	CHECK(shadow_pacer_enable_large_refresh_burst(&pacer));
	const UINT32 baseBurst = (UINT32)pacer.maxCredit;
	CHECK(shadow_pacer_can_submit(&pacer, baseBurst));
	shadow_pacer_submitted(&pacer, baseBurst);
	shadow_pacer_observe(&pacer, 0, TRUE, 0, FALSE, FALSE);
	shadow_pacer_observe(&pacer, 60000, TRUE, 0, FALSE, FALSE);
	CHECK(pacer.credit == pacer.baseMaxCredit);
	CHECK(shadow_pacer_note_damage(&pacer, 60000, 1000000, 1000000));
	CHECK(pacer.credit == pacer.baseMaxCredit);

	BOOL endedByBytes = FALSE;
	for (UINT64 ms = 60000; ms < 60150; ms++)
	{
		shadow_pacer_observe(&pacer, ms, TRUE, 0, FALSE, TRUE);
		if (shadow_pacer_can_submit(&pacer, 1024))
			shadow_pacer_submitted(&pacer, 1024);
		if (!pacer.burstActive)
		{
			endedByBytes = TRUE;
			break;
		}
	}
	CHECK(endedByBytes);
	CHECK(pacer.burstBytes == 48U * 1024U);
	CHECK(pacer.rate == 150U * 1024U);
	CHECK(pacer.credit <= pacer.baseMaxCredit);
	return 0;
}

static int repeated_large_damage_has_base_gaps(void)
{
	shadowPacer pacer;
	CHECK(shadow_pacer_set_fixed_rate_kib(&pacer, 150));
	CHECK(shadow_pacer_enable_large_refresh_burst(&pacer));
	UINT32 entries = 0;
	UINT32 baseTicks = 0;
	UINT64 priorEntryMs = 0;
	for (UINT64 ms = 0; ms < 5000; ms += 10)
	{
		if (shadow_pacer_note_damage(&pacer, ms, 1000000, 1000000))
		{
			if (entries > 0)
				CHECK(ms - priorEntryMs >= 1150U);
			priorEntryMs = ms;
			entries++;
		}
		shadow_pacer_observe(&pacer, ms, TRUE, 0, FALSE, TRUE);
		if (!pacer.burstActive)
			baseTicks++;
	}
	CHECK(entries >= 4 && entries <= 5);
	CHECK(baseTicks >= 400U);
	CHECK(!pacer.burstActive && pacer.rate == 150U * 1024U);
	return 0;
}

static int large_refresh_replacement_and_fairness(void)
{
	shadowPacer pacer;
	CHECK(shadow_pacer_set_fixed_rate_kib(&pacer, 150));
	CHECK(shadow_pacer_enable_large_refresh_burst(&pacer));
	CHECK(shadow_pacer_note_damage(&pacer, 0, 1000000, 1000000));
	UINT32 newestGeneration = 0;
	UINT32 pendingGeneration = 0;
	UINT32 lastSentGeneration = 0;
	UINT32 coalesced = 0;
	UINT32 inputPasses = 0;
	UINT32 channelPasses = 0;
	for (UINT64 ms = 0; ms < 3000; ms += 10)
	{
		newestGeneration++;
		if (pendingGeneration != 0)
			coalesced++;
		pendingGeneration = newestGeneration;
		inputPasses++;
		channelPasses++;
		shadow_pacer_observe(&pacer, ms, TRUE, 0, FALSE, TRUE);
		if (!shadow_pacer_can_submit(&pacer, TILE_BYTES))
			continue;
		lastSentGeneration = pendingGeneration;
		pendingGeneration = 0;
		shadow_pacer_submitted(&pacer, TILE_BYTES);
	}
	printf("large-refresh replacement: newest=%u lastSent=%u coalesced=%u\n",
	       newestGeneration, lastSentGeneration, coalesced);
	CHECK(coalesced > 0);
	CHECK(lastSentGeneration > newestGeneration - 10U);
	CHECK(inputPasses == newestGeneration && channelPasses == newestGeneration);
	CHECK(!pacer.burstActive && pacer.rate == 150U * 1024U);
	return 0;
}

int main(void)
{
	if (slow_path() || fast_path() || degrade_and_recover() || static_desktop() ||
	    static_full_refresh() ||
	    local_zero_is_not_delivery() || hidden_forwarder_limit() || unpaced_comparison() ||
	    socket_cap_scenarios() || fixed_configuration() || fixed_no_adaptive_probe(175) ||
	    fixed_no_adaptive_probe(200) ||
	    fixed_no_adaptive_probe(300) || fixed_no_adaptive_probe(400) ||
	    fixed_sustained_rate(150) || fixed_sustained_rate(175) || fixed_sustained_rate(200) ||
	    fixed_sustained_rate(250) || fixed_sustained_rate(300) ||
	    fixed_sustained_rate(400) || fixed_burst_and_idle(175) || fixed_burst_and_idle(200) ||
	    fixed_replacement_and_fairness(175) || fixed_replacement_and_fairness(200) ||
	    large_refresh_burst_policy() || large_refresh_byte_and_idle_bounds() ||
	    repeated_large_damage_has_base_gaps() || large_refresh_replacement_and_fairness())
		return 1;
	return 0;
}
