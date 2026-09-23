/* Deterministic 0.2.0M pacing policy checks. Apache-2.0. */
#include <stdio.h>
#include "../shadow_pacer.h"

#define CHECK(condition) do { if (!(condition)) { \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); return 1; \
} } while (0)

static int policy_and_time(void)
{
	shadowPacer f;
	CHECK(shadow_pacer_set_fixed_rate_kib(&f, 150));
	CHECK(f.rate == 153600.0 && !f.largeRefreshBurstEnabled);
	CHECK(!shadow_pacer_note_damage(&f, 1000, 1000000, 1000000));
	CHECK(f.rate == 153600.0 && !f.burstActive);

	shadowPacer m;
	CHECK(shadow_pacer_set_fixed_rate_kib(&m, 150));
	CHECK(shadow_pacer_enable_large_refresh_diagnostic_m(&m));
	CHECK(m.baseRate == 153600.0 && m.burstRate == 614400.0);
	CHECK(m.burstDurationMs == 500 && m.burstByteCap == 262144);
	CHECK(m.burstCooldownMs == 1000 && m.largeDamagePercent == 25);
	CHECK(!shadow_pacer_note_damage(&m, 1000, 249999, 1000000));
	CHECK(!m.burstActive && m.rate == 153600.0);
	const double credit = m.credit;
	CHECK(shadow_pacer_note_damage(&m, 1000, 250000, 1000000));
	CHECK(m.burstActive && m.rate == 614400.0 && m.credit == credit);
	CHECK(m.burstEndMs == 1500 && m.burstEligibleMs == 2500);
	CHECK(!shadow_pacer_note_damage(&m, 1200, 1000000, 1000000));
	CHECK(m.burstEndMs == 1500 && m.burstEligibleMs == 2500);
	shadow_pacer_observe(&m, 1499, TRUE, 0, FALSE, FALSE);
	CHECK(m.burstActive && m.credit == credit); /* No idle burst credit. */
	shadow_pacer_observe(&m, 1500, TRUE, 0, FALSE, FALSE);
	CHECK(!m.burstActive && m.burstEndReason == 1);
	CHECK(m.burstStoppedMs == 1500 && m.rate == 153600.0);
	CHECK(m.credit <= m.baseMaxCredit);
	CHECK(!shadow_pacer_note_damage(&m, 2499, 0, 1000000));
	CHECK(shadow_pacer_note_damage(&m, 3200, 250001, 1000000));
	CHECK(m.rate == 614400.0 && m.burstStartMs == 3200);
	return 0;
}

static int byte_cap_and_idle(void)
{
	shadowPacer m;
	CHECK(shadow_pacer_set_fixed_rate_kib(&m, 150));
	CHECK(shadow_pacer_enable_large_refresh_diagnostic_m(&m));
	CHECK(shadow_pacer_can_submit(&m, 16478));
	shadow_pacer_submitted(&m, 16478);
	shadow_pacer_observe(&m, 0, TRUE, 0, FALSE, FALSE);
	shadow_pacer_observe(&m, 60000, TRUE, 0, FALSE, FALSE);
	CHECK(m.credit == m.baseMaxCredit);
	CHECK(shadow_pacer_note_damage(&m, 60000, 1000000, 1000000));
	CHECK(m.credit == m.baseMaxCredit && m.credit < m.burstByteCap);
	UINT64 endMs = 0;
	for (UINT64 ms = 60000; ms < 60500 && m.burstActive; ms++)
	{
		shadow_pacer_observe(&m, ms, TRUE, 0, FALSE, TRUE);
		if (shadow_pacer_can_submit(&m, 1024))
			shadow_pacer_submitted(&m, 1024);
		if (!m.burstActive)
			endMs = ms;
	}
	CHECK(endMs > 60000 && endMs < 60500);
	CHECK(m.burstBytes == 262144 && m.burstEndReason == 2);
	CHECK(m.rate == 153600.0 && m.maxCredit == m.baseMaxCredit);
	CHECK(m.burstEligibleMs == 61500); /* Nominal end plus cooldown. */
	CHECK(!shadow_pacer_note_damage(&m, 61499, 0, 1000000));
	CHECK(shadow_pacer_note_damage(&m, 62000, 1000000, 1000000));
	CHECK(m.burstBytes == 0 && m.burstEndReason == 0);
	return 0;
}

static int bounded_admission_and_fairness(void)
{
	shadowPacer m;
	CHECK(shadow_pacer_set_fixed_rate_kib(&m, 150));
	CHECK(shadow_pacer_enable_large_refresh_diagnostic_m(&m));
	CHECK(shadow_pacer_note_damage(&m, 1000, 250000, 1000000));
	UINT32 pending = 0, sent = 0, replaced = 0, input = 0, channels = 0;
	for (UINT64 ms = 1000; ms < 4000; ms += 10)
	{
		/* The newest captured generation replaces unsent work; each pass
		 * still services input and channel slices before graphics. */
		input++;
		channels += 2;
		if (pending)
			replaced++;
		pending = input;
		shadow_pacer_observe(&m, ms, TRUE, 0, FALSE, TRUE);
		if (shadow_pacer_can_submit(&m, 4096))
		{
			CHECK(pending > sent);
			sent = pending;
			pending = 0;
			shadow_pacer_submitted(&m, 4096);
		}
	}
	CHECK(replaced > 0 && sent > input - 10);
	CHECK(input == 300 && channels == 600 && pending <= input);
	CHECK(m.rate == 153600.0 && !m.burstActive);
	return 0;
}

static int residual_cap_and_preflight(void)
{
	shadowPacer m;
	CHECK(shadow_pacer_set_fixed_rate_kib(&m, 150));
	CHECK(shadow_pacer_enable_large_refresh_diagnostic_m(&m));
	CHECK(shadow_pacer_note_damage(&m, 1000, 250000, 1000000));
	UINT32 sent = 0;
	UINT64 ms = 1000;
	for (; ms < 1500 && sent < 52; ms++)
	{
		shadow_pacer_note_time(&m, ms, TRUE);
		if (shadow_pacer_can_submit(&m, 5000))
		{
			shadow_pacer_submitted(&m, 5000);
			sent++;
		}
	}
	CHECK(sent == 52 && m.burstActive && m.burstBytes == 260000);
	CHECK(m.burstByteCap - m.burstBytes == 2144);
	for (; ms < 1500 && !shadow_pacer_preflight(&m, 2048); ms++)
		shadow_pacer_note_time(&m, ms, TRUE);
	CHECK(shadow_pacer_preflight(&m, 2048));
	CHECK(m.admittedBytes == 0 && m.burstBytes == 260000);
	CHECK(shadow_pacer_can_submit(&m, 64));
	shadow_pacer_submitted(&m, 64);
	CHECK(m.burstActive && m.burstBytes == 260064);
	BOOL allowed = shadow_pacer_can_submit(&m, 5000);
	CHECK(!m.burstActive && m.burstEndReason == 3 && m.rate == 153600.0);
	CHECK(m.burstBytes == 260064 && m.burstEligibleMs == 2500);
	for (; ms < 1500 && !allowed; ms++)
	{
		shadow_pacer_note_time(&m, ms, TRUE);
		allowed = shadow_pacer_can_submit(&m, 5000);
	}
	CHECK(allowed);
	CHECK(m.admittedBytes == 5000);
	const double before = m.credit;
	shadow_pacer_submitted(&m, 5000);
	CHECK(m.credit == before - 5000 && m.admittedBytes == 0);
	CHECK(!shadow_pacer_note_damage(&m, 2499, 1000000, 1000000));
	return 0;
}

static int superseded_wait_and_deadline(void)
{
	shadowPacer f;
	CHECK(shadow_pacer_set_fixed_rate_kib(&f, 150));
	shadow_pacer_observe(&f, 0, TRUE, 0, FALSE, TRUE);
	CHECK(shadow_pacer_can_submit(&f, 15000));
	shadow_pacer_submitted(&f, 15000);
	CHECK(!shadow_pacer_can_submit(&f, 16000));
	shadow_pacer_observe(&f, 10, TRUE, 0, FALSE, TRUE);
	CHECK(f.credit == 3014 && f.requiredCredit == 16000);
	CHECK(!shadow_pacer_preflight(&f, 2048)); /* Same deferred operation. */
	CHECK(!shadow_pacer_can_submit(&f, 100));
	shadow_pacer_cancel_wait(&f); /* New staged state replaced that operation. */
	CHECK(f.requiredCredit == 0 && shadow_pacer_preflight(&f, 2048));
	CHECK(f.admittedBytes == 0 && shadow_pacer_can_submit(&f, 100));
	const double credit = f.credit;
	shadow_pacer_submitted(&f, 100);
	CHECK(f.credit == credit - 100);

	shadowPacer m;
	CHECK(shadow_pacer_set_fixed_rate_kib(&m, 150));
	CHECK(shadow_pacer_enable_large_refresh_diagnostic_m(&m));
	CHECK(shadow_pacer_note_damage(&m, 1000, 1000000, 1000000));
	shadow_pacer_note_time(&m, 1499, TRUE);
	CHECK(shadow_pacer_can_submit(&m, 100));
	/* Admission just before expiry may finish just afterward, once. */
	shadow_pacer_submitted(&m, 100);
	CHECK(m.burstBytes == 100);
	shadow_pacer_note_time(&m, 1501, TRUE);
	CHECK(!m.burstActive && m.burstEndReason == 1 && m.rate == 153600.0);
	CHECK(shadow_pacer_can_submit(&m, 100));
	CHECK(m.burstBytes == 100);
	return 0;
}

static int queue_and_blocked_guard(void)
{
	shadowPacer m;
	CHECK(shadow_pacer_set_fixed_rate_kib(&m, 150));
	CHECK(shadow_pacer_enable_large_refresh_diagnostic_m(&m));
	shadow_pacer_observe(&m, 1000, TRUE, 50000, FALSE, TRUE);
	CHECK(!shadow_pacer_note_damage(&m, 1000, 1000000, 1000000));
	CHECK(!m.burstActive && m.rate == 153600.0);
	shadow_pacer_observe(&m, 1100, TRUE, 0, TRUE, TRUE);
	CHECK(!shadow_pacer_note_damage(&m, 1100, 1000000, 1000000));
	CHECK(!shadow_pacer_can_submit(&m, 100));
	shadow_pacer_observe(&m, 1200, TRUE, 0, FALSE, TRUE);
	CHECK(!shadow_pacer_note_damage(&m, 1200, 1000000, 1000000));
	shadow_pacer_observe(&m, 3200, TRUE, 0, FALSE, TRUE);
	CHECK(shadow_pacer_note_damage(&m, 3200, 1000000, 1000000));
	shadow_pacer_observe(&m, 3250, TRUE, 80000, FALSE, TRUE);
	CHECK(!m.burstActive && m.burstEndReason == 4 && m.rate == 153600.0);
	CHECK(m.burstEligibleMs == 4700);
	return 0;
}

static int prolonged_motion_duty(void)
{
	shadowPacer m;
	CHECK(shadow_pacer_set_fixed_rate_kib(&m, 150));
	CHECK(shadow_pacer_enable_large_refresh_diagnostic_m(&m));
	UINT32 starts = 0, activeTicks = 0;
	for (UINT64 ms = 1000; ms < 4000; ms += 10)
	{
		shadow_pacer_observe(&m, ms, TRUE, 0, FALSE, TRUE);
		if (shadow_pacer_note_damage(&m, ms, 1000000, 1000000))
			starts++;
		if (m.burstActive)
			activeTicks++;
		if (shadow_pacer_preflight(&m, 2048) && shadow_pacer_can_submit(&m, 4096))
			shadow_pacer_submitted(&m, 4096);
		CHECK(m.burstBytes <= m.burstByteCap);
	}
	CHECK(starts == 1);
	CHECK(activeTicks <= 50); /* One 500 ms burst in this three-second run. */
	shadow_pacer_observe(&m, 4000, TRUE, 0, FALSE, TRUE);
	CHECK(m.rate == 153600.0 && !m.burstActive);
	CHECK(!shadow_pacer_note_damage(&m, 5989, 0, 1000000));
	CHECK(shadow_pacer_note_damage(&m, 5990, 1000000, 1000000));
	return 0;
}

int main(void)
{
	return policy_and_time() || byte_cap_and_idle() || bounded_admission_and_fairness() ||
	       residual_cap_and_preflight() || superseded_wait_and_deadline() ||
	       queue_and_blocked_guard() || prolonged_motion_duty();
}
