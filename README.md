# Full RC-Link Bypass of the HS175D / SYMA X30 Pro GPS Drone

*Reverse-engineering the 2.4 GHz control link of a consumer GPS drone from a live bus
tap — and flying the aircraft from scratch-built hardware with the factory remote
powered off and never touched.*

**Status:** complete. Cold-start bind, arm, mode select, and full flight control
reproduced on a custom ESP32 + nRF24 transmitter, then driven end-to-end from a
desktop application. **Timeline:** ~11 days, June 23 – July 4.

**Code:** [`firmware/esp32_nrf24_optical_tx.PUBLIC.ino`](firmware/esp32_nrf24_optical_tx.PUBLIC.ino)
— the real impersonator, with every address, hop table, and captured frame byte
replaced by a marked placeholder. Structure, timing, and radio config are untouched.

---

## 0. Why this is worth writing down

Every *published* teardown of this drone family (Holy Stone HS-series / SYMA X-series
GPS models) attacks the **WiFi / camera / app / firmware** surface — telnet shells,
RTSP video interception, cross-brand app auth bypass, firmware acquisition over
SWD/UART. Valuable work, but all of it rides *on top of* a session the **factory
remote** has already established.

**Nobody published anything on the 2.4 GHz RC control link itself.** That link is the
component that holds *arm authority* — the black box that WiFi and debug access never
solved. This writeup documents reverse-engineering that link down to the physical
layer and rebuilding it from scratch, including the piece every prior effort stopped
short of: a **cold-start bind**, where a fresh, unpaired aircraft accepts a
from-scratch controller with the factory transmitter switched off.

### Honest scope of the claim

- This is **not** a defeat of cryptography. The link is **not encrypted or
  authenticated** and uses **no rolling codes**. That is *why* it is reproducible at
  all, and it is the ceiling on the difficulty — the hard part here was **blind
  protocol inference and a timing-bounded bind handshake**, not breaking a cipher.
- Impersonating an nRF24-class toy link is **not itself novel** — the hobby
  multi-protocol TX scene (Deviation, DIY-Multiprotocol-TX, `nrf24_multipro`) has done
  it for years for the *simple non-GPS toys*, and the two-address bind→control pattern
  found here matches that documented architecture.
- What appears to be new is doing it for **this newer GPS platform** — with arm
  gating, GPS/optical mode switching, and a cold-start bind — **with no existing
  protocol reference to copy from**, rederived from a live bus tap and over-the-air
  captures.

> ⚠️ **Safety.** Driving the control path spins motors and can cause uncommanded
> thrust, flyaway, or injury. All testing was done **props off, airframe clamped,
> battery within reach**.

> 🔐 **Ethics / scope.** This is interoperability and security research on hardware the
> author physically owns. **Replay-enabling specifics — notably the exact
> frequency-hopping sequence — are intentionally withheld** from this document.

---

## 1. Background: the controller as a device

The aircraft is famously "two brains" (a Rockchip vision SoC + an MM32 flight
controller). This writeup is about the **third computer** that everyone forgets: the
**remote control** itself.

Inside the RC, a microcontroller (MM32-class) drives a **BK2425** 2.4 GHz radio — an
nRF24L01+-compatible transceiver — over **SPI**, behind a small power amplifier. The
MCU also has an **I²C EEPROM** that persists the controller's pairing across power
cycles. Both of those buses are tappable.

The core realization that unlocked everything: the RC's radio is a *slave device on an
SPI bus*. Whatever the controller wants to transmit, it must first **write across that
SPI bus** to the radio. Tap the bus and you are reading the transmitter's intentions
in the clear, before they ever hit the air.

---

## 2. Method — reading the link off the wire

### 2.1 The tap

A Raspberry Pi Pico running **sigrok-pico** (as a cheap logic analyzer) was soldered
onto the RC's radio SPI bus: clock, both data lines, and a chip-select reference,
captured with `sigrok-cli` at 8 MS/s.

One detail that matters more than it sounds: the tap was taken on a **live** bus while
the RC was powered and transmitting. To avoid loading the controller's driver and
corrupting the signal, a **series resistor was placed inline on the chip-select tap**
so the analyzer input couldn't disturb the line. Tapping a running bus non-invasively
is the difference between clean captures and chasing ghosts.

### 2.2 The single most useful mistake

Early captures showed nothing but a two-value "heartbeat" repeating forever. Days were
lost to it. The cause: **the data line being decoded was the radio's reply line
(MISO), not the controller's command line (MOSI).** The "heartbeat" was the radio
answering *"receive FIFO empty"* over and over — the wrong half of the conversation.

The fix was a **passive activity scan** of every tapped line to find which one
actually carried the fast, high-entropy traffic. Re-decoding against the *correct*
data line turned the "heartbeat" into a clean, structured command stream instantly.

**Lesson, stated plainly:** distrust your own negative results. "There's nothing here"
usually means "I'm looking at the wrong thing," not "there's nothing."

### 2.3 The frame

With the correct line decoded, the traffic resolved into a short, fixed-length,
repeating frame built from standard nRF24-style SPI commands:

- a **register write** that retunes the radio's channel (the hopping mechanism), and
- a **payload write** (`W_TX_PAYLOAD`) carrying the actual control packet — stick axes,
  a command/button byte, and a trailing checksum.

**Frequency hopping.** The channel-set register is rewritten on a fixed, deterministic
schedule — a small round-robin table, each channel dwelt on briefly before hopping.
The table was fully characterized. *Its contents are intentionally omitted here.*

### 2.4 Sticks, arm, and mode — read directly

With the frame understood, individual fields fell out by **controlled experiment** —
change exactly one input, watch exactly one byte:

- **Stick axes** map to payload bytes; centered ≈ mid-scale, moving a stick moves its
  byte. (The aircraft must be awake and linked, or the RC transmits a fixed idle frame.)
- **Arming** is a stick *gesture*, not a toggle — a specific throttle motion — which is
  why it was long mistaken for a hidden command.
- **Mode select (GPS ↔ optical)** is a **momentary pulse** in the command byte: the
  button drives one byte to a specific value for a short, firmware-timed hold, then
  it clears back to idle. ⚠️ *Exact duration unreconciled*: an early estimate of
  ~1.75 s conflicted with a later capture measuring ~17 frames (≈75 ms) — a large
  discrepancy neither re-measured nor resolved as of this writing.

That last one produced the writeup's second big lesson. Comparing a **steady** GPS
capture against a **steady** optical capture showed them **byte-for-byte identical** —
leading (briefly, and wrongly) to the conclusion that mode wasn't transmitted at all.
It *was*; the command is a transient, and the steady-state captures were recording in
the **gaps after the pulse had already cleared**. It only became visible by pressing
the button **repeatedly during a live capture** and seeing the byte pulse in time with
the presses.

**Lesson:** transient commands cannot be found by diffing steady states. Catch them
live, and repeat the stimulus to be sure.

### 2.5 The checksum

The trailing byte is a simple, unkeyed integrity check over the payload — derived by
collecting frames that differ in exactly one byte and observing how the check byte
responds. Because it is unkeyed, arbitrary valid frames can be constructed offline.
(There is no cryptographic MAC; this is a data-integrity checksum, not a signature.)

### 2.6 The bind address — and cross-verification

Live control uses one radio address; **binding** uses a different, well-known-per-family
address, with the live control address handed off *during* the bind exchange. This
two-address pattern is the documented Syma/nRF24 architecture, which served as
independent confirmation that the reverse engineering was correct rather than a lucky
guess.

The bind address is only present on the wire for a **few milliseconds during
power-on**, inside a configuration burst — invisible over the air and easy to miss on
the bus. It was confirmed two independent ways, plus a functional check:

1. **SPI capture** of the power-on configuration burst (bus must be armed *before* the
   RC boots) — the source of the bind address, and (after a byte-order correction) the
   live control address too.
2. **Over-the-air**: a second nRF24-class radio, parked on the corrected live-control
   address, heard the real RC's live packets in flight — an independent confirmation of
   the control address.
3. **Functional**: transmitting the bind address from the impersonator produced a real
   cold-start bind (red-flash → green re-lock) on the aircraft — the bind address working
   in practice, not just looking right on a capture.

*(A third **passive** cross-check — reading the RC's I²C EEPROM, which likely persists
the pairing state at rest — was planned as an independent source but was superseded by
the SPI/OTA fix before it was carried out. It was never completed and does not factor
into the confirmation above; it remains open as future work.)*

Two sources agreeing, plus the bind address working in practice, is the standard that
separates *"I think this is it"* from *"this is it."*

---

## 3. Rebuilding it — the impersonator

The recovered protocol was reimplemented on an **ESP32 + nRF24L01+**:

1. Match the radio configuration (data rate, address length, CRC, auto-ack settings)
   observed at the RC's power-on.
2. Reproduce the bind exchange from a cold start on the bind address, hand off to the
   live control address.
3. Follow the hop schedule and stream well-formed, correctly-checksummed control
   frames.
4. Reproduce arm (the throttle gesture), mode select, and full stick control.

The end result: from a **fresh, unpaired aircraft** and the **factory remote powered
off**, pressing one key on the impersonator drives a red-flash → green re-lock on the
aircraft — **it bound to scratch-built hardware** — after which it flies entirely from
the custom transmitter.

Finally, the whole sequence was lifted out of a serial terminal and driven from a
**desktop application**, which bootstraps the cold-start bind and flies the aircraft
with no manual keypresses and the factory controller in a drawer: a self-contained,
end-to-end **remote bypass**.

---

## 4. Results at a glance

| Layer | Result |
|---|---|
| Physical | Live SPI tap of RC → radio; correct data line identified by activity scan |
| Framing | Fixed-length frame: channel-set register write + `W_TX_PAYLOAD` control packet |
| Hopping | Deterministic small round-robin table — characterized *(sequence withheld)* |
| Control | Stick axes mapped; **arm = throttle gesture**; **mode = momentary command pulse** |
| Integrity | Unkeyed checksum reverse-engineered; arbitrary valid frames constructible |
| Addressing | Two-address bind→control handoff; live address via SPI + OTA, bind address via SPI + functional re-bind (EEPROM cross-check planned, not completed) |
| Bind | **Cold-start bind reproduced** with factory RC powered off |
| Rebuild | ESP32 + nRF24 impersonator: bind, arm, mode, full flight |
| Integration | Driven end-to-end from a desktop app — complete remote bypass |

---

## 5. What made it hard (and what didn't)

**Not hard:** the tools. Logic analyzers, SPI/I²C sniffing, and soldering are routine
bench work. Anyone with the gear can capture the bus.

**Hard:**

- **Blind protocol inference** — no datasheet frame map, no register spec. The
  structure had to be *derived from raw traffic* by pattern-matching bytes until the
  frame fell out.
- **Transient, timing-bounded signals** — the mode command is a sub-two-second pulse;
  the bind address exists for milliseconds at power-on. Observability, not complexity,
  was the wall.
- **Trusting the process through dead ends** — wrong data line, wrong address,
  promiscuous-mode noise, steady-state captures that hid a transient. Most of the
  eleven days were spent being wrong productively.

**Explicitly *not* claimed:** breaking encryption, defeating secure boot, or any
cryptographic attack. None of those exist on this link. A modern, signed/encrypted
control link (e.g. DJI-class) would be an entirely different and far harder problem.

---

## 6. Reproducibility

Everything here was obtained with commodity gear: a ~$4 microcontroller as a logic
analyzer, `sigrok`/`sigrok-cli`, a second nRF24-class radio for over-the-air capture,
and an ESP32 + nRF24 for the transmitter. No SDR, no specialized RF lab, no vendor
tools. The method — *tap the radio's own SPI bus and read the transmitter's intentions
before they reach the air* — generalizes to other nRF24/BK-class RC links.

The one non-obvious prerequisite: to catch the bind address you must have the tap
**armed and capturing before the RC powers on**, with the RC otherwise **radio-silent**,
so the power-on configuration burst is isolated.

---

## 7. Generalizing to the whole family

The single most useful outcome isn't that *this* aircraft flies from scratch-built
hardware — it's that the **method ports across the entire Holy Stone / SYMA GPS family**
with very little rework, because these drones share one platform:

- **One brain-set.** RV1109 vision SoC + MM32-class flight MCU, the same `img_trans_app`
  firmware lineage, the same service ports (JSON command channel, UDP control, session
  keepalive, RTSP video). The flight board even self-identifies with a fixed internal
  model string over its internal UART — a **fingerprint**: any unit that reports the same
  string is the same board, so the entire control-surface map transfers wholesale.
- **One radio architecture.** Every remote in this family drives a **BK2425** (nRF24L01+
  clone) over SPI behind a small PA. That is *the* reason the RC-link attack generalizes:
  the transceiver, the SPI command set, and the two-address bind→control pattern are
  identical across models. What changes from drone to drone is **only the data** — the
  bind address, the hop table, the channel set. **Same capture, same tooling, different
  numbers.** An nRF24-class radio impersonates any of them.

What this means in practice for the next drone in the family:

| Axis | Effort on a new model |
|---|---|
| WiFi control surface (camera, media, settings, telemetry) | **~free** — same ports, same commands, often identical |
| RTSP video / FTP media / adb root | **~free** — same services, same paths |
| RC link (bind, hop, channels) | **short re-derivation** — same method, new numbers |
| FC firmware extraction | **luck** — per-unit silicon lock, unrelated to flying it |

**And crucially: the airframe never has to be opened.** The SPI tap lives on the
*transmitter*, not the drone. The entire drone-side surface — control, video, media,
root shell — is reachable over the air with the aircraft **fully sealed**. Adding a new
model to the map means, at most, opening its *remote* once to re-capture the radio
parameters (and even that is a one-time job per RC hardware design, not per aircraft).

So the honest scope of the achievement: **the control surface generalizes for free, the
radio bind is a short per-model re-derivation, and the silicon lock is a coin flip you
don't need to win to fly.** This writeup is really the documentation of a *repeatable
method*, and this section is the part that says exactly which single piece you redo each
time — and which you never touch again.

---

*Author's note: this was a first reverse-engineering project. It is documented not as a
victory lap but so the next person working on this drone family has the RC-link axis
mapped — the one axis the published record left blank.*
