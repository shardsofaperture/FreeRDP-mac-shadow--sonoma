"""Bounded, transport-based stop decision for one physical RateSweep capture."""
import statistics


class AutoStop:
    def __init__(self, rate_kib, seen_samples=0, baseline_rates=(), baseline_queue=0):
        rate = rate_kib * 1024
        baseline = sorted(baseline_rates)
        baseline_p95 = baseline[-1] if baseline else 0
        baseline_median = statistics.median(baseline) if baseline else 0
        self.substantial_floor = max(int(rate * .75), int(baseline_p95 + 24 * 1024))
        self.idle_ceiling = max(int(rate * .35), int(baseline_median * 2))
        self.queue_ceiling = max(64 * 1024, int(baseline_queue + 16 * 1024))
        self.seen_samples = seen_samples
        self.first_active_sample = None
        self.last_active_sample = None
        self.substantial_count = 0
        self.substantial_bytes = 0

    def observe(self, loopback_rates, queue_samples):
        for index in range(self.seen_samples, len(loopback_rates)):
            amount = loopback_rates[index]
            if amount >= self.substantial_floor:
                self.substantial_count += 1
                self.substantial_bytes += amount
                self.last_active_sample = index
                if self.first_active_sample is None:
                    self.first_active_sample = index
        self.seen_samples = len(loopback_rates)

        if len(queue_samples) >= 4:
            recent = queue_samples[-4:]
            external = [row['sshd external Send-Q'] for _, row in recent]
            if all(value is not None and value >= 256 * 1024 for value in external):
                return 'dangerous_external_queue'
            loopback = [row['sshd loopback Recv-Q'] for _, row in recent]
            if any(value is not None and value >= 512 * 1024 for value in loopback):
                return 'dangerous_loopback_queue'
            if all(value is not None and value >= 256 * 1024 for value in loopback):
                return 'dangerous_loopback_queue'
        if (self.first_active_sample is None or self.substantial_count < 3 or
                self.substantial_bytes < 3 * self.substantial_floor or
                len(loopback_rates) - self.last_active_sample < 7 or
                len(queue_samples) < 8):
            return None
        recent_rates = loopback_rates[-6:]
        if (sum(value <= self.idle_ceiling for value in recent_rates) < 4 or
                statistics.median(recent_rates) > self.idle_ceiling):
            return None
        recent = queue_samples[-8:]
        if any(any(value is None for value in row.values()) for _, row in recent):
            return None
        totals = [max(row.values()) for _, row in recent]
        if (statistics.median(totals) > self.queue_ceiling or
                max(totals) > 2 * self.queue_ceiling or
                (recent[-1][0] - recent[0][0]).total_seconds() < 3):
            return None
        return 'activity_then_drain'
