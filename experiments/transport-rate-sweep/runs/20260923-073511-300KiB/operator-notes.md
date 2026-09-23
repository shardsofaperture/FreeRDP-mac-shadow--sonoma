# 300 KiB/s physical observation

The user performed one standard large foreground-window promotion after
`READY @ 300 KiB/s`. They estimated **2–3 seconds**. The first switch showed
**two large bars**; switching back showed **about 6–7 bars**. Icons/menus
felt about the same, perhaps **slightly slower**. These are user-visible
observations and are separate from the transport data in [analysis.txt](analysis.txt).

The capture auto-stopped after queue recovery. sshd loopback Recv-Q reached
285,412 B, exceeded 128 KiB for a longest observed stretch of about 2.5
seconds, and later drained. External Send-Q stayed below 31 KiB. This is the
first measured queue-pressure signal of the sweep, but it was transient in
this run. The 400 KiB/s run uses an additional harness-only loopback pressure
cutoff; the signed FreeRDP binary is unchanged.
The user's follow-up message from the phone was typed after AUTO-STOP, so it
did not add RDP input during this capture.
