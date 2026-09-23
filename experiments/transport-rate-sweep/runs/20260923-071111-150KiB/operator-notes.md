# 150 KiB/s physical observation

User performed one foreground large-window promotion over the phone's SSH/RDP
connection after capture printed READY. The user reported approximately **eight
black bars** while the foreground window loaded and **6–8 seconds** to load.
This is a client-visible observation, separate from the transport counters.
No precise action-start or client-presentation timestamp was captured.

The raw capture spans 2026-09-23 11:11:11–11:12:28 UTC. It includes idle time
and other traffic, so its capture-wide mean is not a window-promotion average.
The parser drops nettop's first connection-lifetime sample and uses its later
one-second deltas. The [analysis](analysis.txt) reports 155,009 B/s p95
plaintext FreeRDP→sshd, 157,900 B/s p95 encrypted sshd→phone, maximum sampled
external Send-Q 28,012 B, and a 9.66-second observed queue-peak-to-near-empty
proxy. These transport figures do not establish the 6–8-second display time.
The external `re-tx` field was nonzero in two samples (raw sum 4,240; unit not
established). All raw files remain in this directory.
