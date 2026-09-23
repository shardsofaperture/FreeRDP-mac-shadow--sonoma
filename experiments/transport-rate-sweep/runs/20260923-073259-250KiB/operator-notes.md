# 250 KiB/s physical observation

The user performed one standard large foreground-window promotion after
`READY @ 250 KiB/s`. They reported **fewer bars** than the preceding run,
estimated about **three seconds** for the change, and said icon response was
**fast and not glitchy**. This is an estimate from one action; they said
multiple swaps would be needed for a precise count. It is separate from the
transport counters in [analysis.txt](analysis.txt).

The bounded capture stopped automatically after substantial transfer and
return near the measured connected baseline. The external Send-Q ended near
its pre-action maximum, so the strict <=2 KiB queue-drain marker was not
observed; the raw queue series and auto-stop metadata are preserved.
