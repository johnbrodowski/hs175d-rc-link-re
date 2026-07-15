/*
  esp32_nrf24_optical_tx  —  RC243 / HS175D link IMPERSONATOR (replay)  [PUBLIC / REDACTED]
  ===========================================================================================
  This is a sanitized copy for portfolio/outreach purposes. Every protocol CONSTANT that would
  let this compile into a working transmitter against a real HS175D-family drone has been
  replaced with a placeholder (marked ▓▓ REDACTED ▓▓ below). The logic, structure, timing, and
  radio configuration are all real and unmodified — this is genuinely how the impersonator
  works, just without the specific numbers that make it work against a real aircraft.

  See docs/WRITEUP.md for the full writeup, including why these specifics are withheld.

  Full RC-free replay: streams the neutral heartbeat, holds OPTICAL, and fires
  the discrete ARM (motor-idle) command — all captured byte-for-byte off the real
  link and confirmed over the air.

  Hardware: ESP32 (NodeMCU-32S) + nRF24L01+  (CE=GPIO4, CSN=GPIO5, VSPI, 3V3 + cap).

  LINK (method, confirmed over the air on the real hardware):
     ADDRESS  = ▓▓ REDACTED — 5-byte nRF24 address, recovered from a live SPI tap ▓▓
     Hop set  = ▓▓ REDACTED — 4-channel round-robin table ▓▓, 2 packets per channel, ~4.4 ms apart
     DataRate = 1 Mbps,  CRC = 16-bit,  AutoAck = OFF,  addr width 5,  payload 10

  FRAMES (structure is real; exact captured byte values withheld):
     NEUTRAL  : ▓▓ REDACTED 10-byte frame, heartbeat toggle in byte0 (alternates two known values) ▓▓
     OPTICAL  : ▓▓ REDACTED — same frame with one payload byte set to a mode flag ▓▓
       // NOTE: the real RC only *pulses* the mode flag momentarily at the toggle edge; we HOLD it
       // continuously here and the drone accepts that just as well.
     ARM burst: ▓▓ REDACTED — a 3-frame sequence (precursor, flag, command) repeated across the hop set ▓▓

  Serial (115200) AND Bluetooth Classic SPP ("HS175D-ESP32", pair once in Windows —
  it then shows up as a normal COM port, same as USB) both accept the same commands:
     n / g = stream NEUTRAL (idle)
     o     = stream OPTICAL (mode flag held)
     a     = fire the ARM burst once (then resume current neutral/optical stream)
     s     = pause / resume streaming
     ?     = radio details

  ⚠ SAFETY: this arms a real drone. PROPS OFF, drone secured, before pressing 'a'.
*/

#include <SPI.h>
#include "RF24.h"
#include "printf.h"
#include "BluetoothSerial.h"

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled in this build of the ESP32 Arduino core.
#endif

RF24 radio(4, 5);                 // CE=GPIO4, CSN=GPIO5
BluetoothSerial SerialBT;         // Classic BT SPP — pairs like a headset, then shows up as a COM port

// Print to both transports so status/feedback reaches whichever one you're actually using.
void dlog(const String& s) { Serial.println(s); SerialBT.println(s); }

// CONTROL address — recovered by reversing over-the-air captures against the SPI tap.
// ▓▓ REDACTED: real 5-byte value withheld — see docs/WRITEUP.md §2.6 ▓▓
static const uint8_t LINK_ADDR[5] = { 0x00, 0x00, 0x00, 0x00, 0x00 };
// BIND address — read directly off the RC's power-on config via the SPI tap; this is the
// address a waiting/unpaired drone listens on for the "here I am" announce.
// ▓▓ REDACTED: real 5-byte value withheld ▓▓
static const uint8_t BIND_ADDR[5] = { 0x00, 0x00, 0x00, 0x00, 0x00 };

// ▓▓ REDACTED: real 4-channel hop table withheld — see docs/WRITEUP.md §2.3 ▓▓
static const uint8_t HOP[]        = { 0x00, 0x00, 0x00, 0x00 };
static const uint8_t N_HOP        = sizeof(HOP);
static const uint8_t  PKTS_PER_CH = 2;
static const uint16_t PKT_US      = 4400;

// exact 10-byte frames — structure (heartbeat toggle in byte0, checksum in byte9) is real;
// ▓▓ REDACTED: captured byte values withheld ▓▓
static const uint8_t NEUTRAL[2][10] = {
  { 0x00,0,0,0,0,0,0,0,0,0 },
  { 0x00,0,0,0,0,0,0,0,0,0 },
};
static const uint8_t OPTICAL[2][10] = {
  { 0x00,0,0,0,0,0,0,0,0,0 },
  { 0x00,0,0,0,0,0,0,0,0,0 },
};
static const uint8_t ARM_PRE[10] = { 0,0,0,0,0,0,0,0,0,0 };   // ▓▓ REDACTED ▓▓
static const uint8_t ARM_FLAG[10]= { 0,0,0,0,0,0,0,0,0,0 };   // ▓▓ REDACTED ▓▓
static const uint8_t ARM_CMD[10] = { 0,0,0,0,0,0,0,0,0,0 };   // ▓▓ REDACTED ▓▓

// COLD-START BIND handshake (what the RC does at power-on). Same LINK_ADDR, different
// channel set, captured off the cold-boot SPI trace.
// ▓▓ REDACTED: real bind-channel table and announce-frame bytes withheld ▓▓
static const uint8_t BIND_HOP[]  = { 0x00, 0x00, 0x00, 0x00 };
static const uint8_t N_BIND      = sizeof(BIND_HOP);
static const uint8_t BIND_ANN[10]= { 0,0,0,0,0,0,0,0,0,0 };   // address announce
static const uint8_t BIND_ALT[10]= { 0,0,0,0,0,0,0,0,0,0 };   // interspersed frame

bool opticalMode = false;         // false=neutral, true=optical (mode flag set)
bool streaming   = false;         // control stream on/off
bool binding     = false;         // continuous cold-start bind on/off
bool coldStart   = false;         // act like a remote that just turned on: bind + control interleaved
uint8_t hb       = 0;             // heartbeat toggle 0/1

void configureRadio() {
  radio.setAddressWidth(5);
  radio.setDataRate(RF24_1MBPS);
  radio.setCRCLength(RF24_CRC_16);
  radio.setAutoAck(false);
  radio.setRetries(0, 0);
  radio.setPayloadSize(10);
  radio.setPALevel(RF24_PA_HIGH);
  radio.disableDynamicPayloads();
  radio.openWritingPipe(LINK_ADDR);
  radio.stopListening();
}

// send one 10-byte frame on the current hop channel, advance heartbeat
void tx(const uint8_t* f) { radio.write(f, 10); }

// Inter-packet gap PLUS a yield() — delayMicroseconds() busy-waits (no FreeRTOS yield), so a
// continuous streaming/binding loop with none of these ever lets the idle-task watchdog run,
// which panics and reboots the whole chip (killing USB + Bluetooth at once) after ~5s.
void txDelay() { delayMicroseconds(PKT_US); yield(); }

// Emulate one RC hop cycle streaming the current mode's frame.
void streamHopCycle() {
  radio.openWritingPipe(LINK_ADDR);        // control goes to the CONTROL address
  for (uint8_t h = 0; h < N_HOP; h++) {
    radio.setChannel(HOP[h]);
    for (uint8_t p = 0; p < PKTS_PER_CH; p++) {
      const uint8_t* f = opticalMode ? OPTICAL[hb] : NEUTRAL[hb];
      tx(f);
      hb ^= 1;
      txDelay();
    }
  }
}

// Fire the discrete ARM burst across the hop set (precursor once, then flag/cmd pairs).
void fireArmBurst() {
  dlog("ARM burst -> drone (PROPS OFF!)");
  for (uint8_t h = 0; h < N_HOP; h++) { radio.setChannel(HOP[h]); tx(ARM_PRE); txDelay(); }
  for (uint8_t rep = 0; rep < 6; rep++) {
    for (uint8_t h = 0; h < N_HOP; h++) {
      radio.setChannel(HOP[h]);
      tx(ARM_FLAG); txDelay();
      tx(ARM_CMD);  txDelay();
    }
  }
}

// One pass of the bind sweep. Called repeatedly while binding=true so the announce is
// ALWAYS on-air — the drone's power-on listen window can't miss it.
void bindSweepOnce() {
  radio.openWritingPipe(BIND_ADDR);        // bind announce goes to the DEFAULT address
  for (uint8_t h = 0; h < N_BIND; h++) {
    radio.setChannel(BIND_HOP[h]);
    radio.write(BIND_ALT, 10); txDelay();   // interspersed frame
    radio.write(BIND_ANN, 10); txDelay();   // address announce
    radio.write(BIND_ANN, 10); txDelay();
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {}
  SerialBT.begin("HS175D-ESP32");        // Classic BT SPP — pair once in Windows Bluetooth settings
  printf_begin();
  if (!radio.begin()) { dlog("radio NOT responding - check 3V3/cap/GND/wiring"); while (1) {} }
  configureRadio();
  radio.printDetails();
  dlog("\nHS175D impersonator [PUBLIC/REDACTED BUILD - fill in real constants above to use].");
  dlog("  Bluetooth SPP: \"HS175D-ESP32\" — pair once, then it's a COM port same as USB.");
  dlog("  COLD START (no RC): press 'c', THEN power the drone on. Watch for RED blink = paired.");
  dlog("  After it pairs (back to green), press n/o/a to control.");
  dlog("  c=COLD START(bind+control)  b=bind-only  n/g=neutral  o=optical  a=ARM burst  s=pause");
  dlog("  >>> PROPS OFF before 'a' <<<");
}

// Shared command dispatch — fed from either Serial (USB) or SerialBT (Bluetooth SPP),
// so both transports accept the exact same single-char protocol.
void handleCmd(char c) {
  switch (c) {
    case 'c': coldStart = true; binding = false; streaming = false;
              dlog("-> COLD START (bind+control like a fresh remote). Now power the DRONE on; watch for RED blink = paired."); break;
    case 'b': binding = true;  coldStart = false; streaming = false;
              dlog("-> CONTINUOUS BIND. Power drone on; when it links press n/o."); break;
    case 'o': binding = false; coldStart = false; streaming = true; opticalMode = true;
              dlog("-> CONTROL / OPTICAL (mode flag held)"); break;
    case 'g':
    case 'n': binding = false; coldStart = false; streaming = true; opticalMode = false;
              dlog("-> CONTROL / NEUTRAL"); break;
    case 'a': fireArmBurst(); break;
    case 's': streaming = !streaming; binding = false; coldStart = false; dlog(streaming ? "stream ON" : "stream OFF"); break;
    case '?': radio.printDetails(); break;
    default: break;
  }
}

void loop() {
  if (coldStart) {                         // impersonate a remote that just powered on:
    bindSweepOnce();                       //   announce on the bind channels, then
    for (uint8_t i = 0; i < 3; i++) streamHopCycle();  // stream control on the hop channels
  }
  else if (binding)   bindSweepOnce();     // continuous cold-start bind (drone can't miss it)
  else if (streaming) streamHopCycle();    // control stream

  if (Serial.available())   handleCmd(Serial.read());
  if (SerialBT.available()) handleCmd(SerialBT.read());
}
