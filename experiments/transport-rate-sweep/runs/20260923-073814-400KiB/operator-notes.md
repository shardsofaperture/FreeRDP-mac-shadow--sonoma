# 400 KiB/s physical observation

The user performed one standard large foreground-window promotion after
`READY @ 400 KiB/s`. They reported **no real visible change** from the prior
rate, estimated about **three seconds** for the swap, saw **5–7 bars**, and
said graphics/icons/phone movement responsiveness felt the **same**. This
observation is separate from transport counters in [analysis.txt](analysis.txt).

The capture auto-stopped after traffic and queues returned near the connected
baseline. sshd loopback Recv-Q peaked at 322,378 B and exceeded 64 KiB in 32
half-second samples, 128 KiB in 19, and 256 KiB in two. External Send-Q stayed
below 31 KiB. The user's later phone message about the 300 KiB/s run was
confirmed to have been typed **after** that run's AUTO-STOP.
