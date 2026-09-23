# 200 KiB/s physical observation

The user performed one standard large foreground-window promotion after
`READY @ 200 KiB/s`. They reported **13 bars**, said it **felt faster**, and
estimated the window loaded in **under five seconds**. They counted bars during
this run, so the duration is an estimate. In a follow-up, they reported that
**icon/menu responsiveness was good**. This observation is separate from the transport counters
in [analysis.txt](analysis.txt).

The bounded capture stopped automatically after substantial graphics activity
and return of transport/queues near the measured connected baseline. The
capture included unrelated background traffic. The first two 200 KiB/s
captures in adjacent run folders ended before READY while the harness baseline
was being corrected; no user window promotion was requested or counted in
those attempts.
