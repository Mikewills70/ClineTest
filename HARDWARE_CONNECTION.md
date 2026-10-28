# HARDWARE CONNECTION DOCUMENT

# ESP32-S3 Light Show Controller — for a NEW session starting a fresh audio concept.
# Everything here is taken from the working firmware, not from memory.

## THE BOARD

**ESP32-S3, N16R8: 16 MB flash, 8 MB octal PSRAM.**

```
fqbn:  esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,FlashMode=qio,USBMode=hwcdc
libs:  D:\Dell 7720\Arduino\libraries
port:  COM5
```

**Kill any Arduino IDE / arduino-cli / serial monitor FIRST.** It holds COM5, and the upload then
fails with *exit status 2, one second after a successful build, silently*. The board keeps running
the old firmware and it looks exactly like a code fault. That cost hours on this project.

## THE AUDIO INPUT — ONE PIN

```
AUDIO_PIN = 2      MAX9814 microphone, ANALOGUE, on ADC1
```

It is **not I2S and not digital**. It is a plain ADC read of a microphone sitting at mid-rail, so
**the DC must be removed before the signal means anything** — the mic idles at about **2048 counts**
on a 12-bit ADC.

**Sample rate:** the firmware runs about 16.8 kHz, which is known to be **too low** — a 12 kHz
microphone wants at least 24 kHz. For a fresh test, sample as fast as a tight loop allows and
**measure the real rate rather than assuming it**.

**What the mic actually delivers — measured on this hardware:**

| | |
|---|---|
| **In silence** | the signal spans roughly **1 to 3 counts** |
| **In music, a bass beat** | roughly **5 to 30 counts** |

**That is the most useful fact in this document.** The entire usable range is a few tens of counts
and the gain is low. **Any absolute threshold must work inside 1 to 30 — a threshold of 12 that
looks modest is half the usable range**, which is exactly the mistake that was made repeatedly.

## IO48 — THE BEAT INDICATOR

```
POWER_LED_PIN = 48      the onboard status LED
```

**One short flash per beat.** A **100 ms** pulse is clearly visible; **300 ms is too long** —
consecutive flashes merge into one glow. The firmware also uses this pin for status blinks, so a
fresh sketch should keep those separate or drop them.

## THE 240V RELAY CHANNELS — TEN

```
channelPins[10] = {11, 12, 13, 14, 21, 38, 39, 40, 41, 42}
```

| Channel | GPIO | Channel | GPIO |
|---|---|---|---|
| **1** | **11** | **6** | **38** |
| **2** | **12** | **7** | **39** |
| **3** | **13** | **8** | **40** |
| **4** | **14** | **9** | **41** |
| **5** | **21** | **10** | **42** |

**Plain `digitalWrite`, HIGH = relay on.**

**The intended use for a fresh start: channels 1–4 light on beats 1, 2, 3 and 4, and channel 10 is
the beat-1 marker.**

**A trap that cost a day:** channel 10 was written inside an engine that steps at up to **one second
per step**, while a beat window lasts about **450 ms** — so it almost never looked while the window
was open and the LED never lit. **Write the channel at the beat itself, never from a slow engine.**

## THE TEN WS2811 STRIPS

```
ledDataPins[NUM_PORTS] = {10, 9, 8, 18, 17, 16, 15, 7, 6, 5}
```

**GPIO 2 is the microphone and GPIO 48 is IO48, so neither is free.**

## HOW TO READ THE BOARD WHILE IT RUNS

It is a **Wi-Fi access point**: SSID `ESP32-LightShow`, address `192.168.4.1`.

**The access point has no internet, so Windows silently drops it. Every read needs a fresh join:**

```
netsh wlan connect name="ESP32-LightShow"
```

**then fetch ONCE, immediately.** A read after any delay fails, and a retry loop just burns time.
The profile is already saved on the PC.

**USB serial is not dependable on this hardware** — the sketch needs DTR asserted, and asserting it
resets the port, so a host sees the boot banner and nothing else.

## WHAT TO MEASURE, NOT GUESS

The single most useful instrument on this project has been **counters published over HTTP and read
after a run**. Every wrong theory was killed by a counter; every right one was confirmed by one.

**Publish the count of every event you care about — beats seen, beats refused, flashes fired,
flashes filled — and read them rather than reasoning about what should be happening.**

One example worth carrying forward: `io48` and `leadFlashes` were **identical in every reading for a
whole day**, which proved the prediction-driven flash had **never once fired** and that every flash
was coming from the fallback path. That comparison was available from the first reading and was not
made.
