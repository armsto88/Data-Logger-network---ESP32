# Ultrasonic Anemometer Hardware and Firmware Function Note

## Purpose

This document explains how the ultrasonic subsystem on the custom ESP32 ultrasonic anemometer is intended to work, and what the firmware must do to operate it correctly.

It is based on the current schematic set and verified pin map.

---

## Verified pin map

```cpp
// Receive timing and gate
GPIO34: TOF_EDGE input
GPIO4:  RX_EN

// RX direction mux
GPIO16: MUX_A
GPIO17: MUX_B

// TX driver select lines
GPIO26: DRV_N
GPIO27: DRV_E
GPIO14: DRV_S
GPIO13: DRV_W

// TX relay/select lines
GPIO33: REL_N
GPIO32: REL_E
GPIO21: REL_S
GPIO22: REL_W

// TX burst gate
GPIO25: TX_PWM
```

### RX direction decode used in firmware

```text
N: A=0, B=0
S: A=1, B=0
W: A=0, B=1
E: A=1, B=1
```

---

## 1. Overall ultrasonic measurement concept

The board is designed as a time-of-flight ultrasonic anemometer front end.

A single measurement consists of:

1. select one transducer direction to transmit
2. select the opposite transducer direction to receive
3. generate a short 40 kHz transmit burst
4. allow the receive chain to listen after a controlled blanking period
5. detect the first valid receive event using the comparator
6. timestamp that event in the ESP32
7. repeat in the opposite direction
8. compare opposite-direction times to estimate wind along that axis

At system level, the ultrasonic hardware has four main functions:

- **TX burst generation**
- **TX directional routing**
- **RX directional routing and analog conditioning**
- **edge detection and timing capture**

---

## 2. How the transmit side is intended to function

### 2.1 TX burst generation

`GPIO25 = TX_PWM` is the master transmit burst signal.

The intended chain is:

```text
TX_PWM -> high-speed gate driver -> PWM_5V -> burst-enable switch -> TX_PULSE
```

#### What the hardware is doing

- The high-speed gate driver converts the 3.3 V logic burst into a stronger switching signal.
- The burst-enable stage gates the elevated transmit rail onto `TX_PULSE`.
- `TX_PULSE` is the global high-voltage burst node that is then routed to a selected transducer direction.

#### Firmware implication

Firmware must generate a real **40 kHz burst** on `TX_PWM`, not just hold the pin high.

A valid burst should have:

- fixed frequency
- fixed cycle count
- deterministic start/stop timing

For early testing, a suitable starting point is:

- **40 kHz**
- **8 to 16 cycles**
- **50% duty**

---

### 2.2 TX directional routing

Directional transmit routing is controlled by:

- `DRV_N`, `DRV_E`, `DRV_S`, `DRV_W`
- `REL_N`, `REL_E`, `REL_S`, `REL_W`

Each direction has its own local transistorized transmit routing block.

#### Intended behavior

For a given shot:

- only one direction is selected for transmit
- that selected directional block connects the chosen transducer to `TX_PULSE`
- all other transmit directions remain inactive

#### Important practical point

The directional blocks are not simple one-pin enables. They use paired logic through `DRV_*` and `REL_*`.

That means firmware must:

- keep all `DRV_*` and `REL_*` low by default
- assert only one direction at a time
- use a validated truth table for that direction
- never drive multiple TX directions at once

#### Current practical note

Because the route logic was explored electrically during testing but not yet fully acoustically validated, the final truth table for each direction should be treated as a firmware configuration table that may still need refinement.

---

## 3. How the receive side is intended to function

### 3.1 RX directional mux

Receive selection is controlled by:

- `GPIO16 = MUX_A`
- `GPIO17 = MUX_B`

The 74HC4052 receive mux selects which directional receive pair is passed into the analog front end.

#### RX direction decode

```text
N: MUX_A=0, MUX_B=0
S: MUX_A=1, MUX_B=0
W: MUX_A=0, MUX_B=1
E: MUX_A=1, MUX_B=1
```

#### Firmware implication

Firmware must select the desired receive direction before each measurement shot.

---

### 3.2 RX gate control

`GPIO4 = RX_EN`

This signal gates the receive path.

#### Intended behavior

`RX_EN` should normally be used as:

- **LOW during TX burst**
- **LOW during blanking**
- **HIGH only when ready to listen**

This is important because the receive chain can otherwise see:

- TX feedthrough
- transducer ringdown
- front-end overload
- comparator false triggers

#### Current hardware note

This is especially important on the current board state because the original front-end clamp diodes `D8` and `D9` were removed during bring-up after a footprint/pinout fault. Without those intended clamp elements, the RX path is more vulnerable to overdrive and false triggering.

---

### 3.3 Analog receive chain

The receive signal path is intended to operate like this:

1. selected transducer pair routed by the mux
2. front-end protection / coupling network
3. low-noise preamplifier stage
4. active band-pass gain stage
5. comparator / threshold detector
6. digital timing edge into the ESP32

#### Important nets in the RX chain

The intended analog progression is approximately:

```text
RX_A / RX_B
-> RX_HOT / RX_COLD
-> RX_IN
-> ST1_IN
-> preamp stage
-> band-pass stage
-> RX_AMP
-> comparator against VREF
-> TOF_EDGE
```

#### VREF role

`VREF` is the mid-rail bias reference for the analog path.

Because the analog chain runs from a single 3.3 V supply, the ultrasonic waveform is centered around `VREF` rather than around true 0 V.

Typical intended value:

- `VREF ≈ 1.65 V`

#### Comparator output

The comparator converts the conditioned analog receive waveform into a digital edge:

- `GPIO34 = TOF_EDGE`

This edge is what the ESP32 timestamps.

---

## 4. What the firmware must do

### 4.1 Safe initialization

On boot, firmware should put the ultrasonic subsystem into a safe idle state.

#### Safe default state

```cpp
TX_PWM = LOW
RX_EN  = LOW

DRV_N = LOW
DRV_E = LOW
DRV_S = LOW
DRV_W = LOW

REL_N = LOW
REL_E = LOW
REL_S = LOW
REL_W = LOW

MUX_A / MUX_B = known default
```

#### Other startup actions

Firmware should also:

- configure `GPIO34` as the `TOF_EDGE` input
- attach a rising-edge interrupt on `GPIO34`
- initialize timing capture variables
- initialize any burst generator peripheral

---

### 4.2 One measurement cycle

The firmware sequence for one ultrasonic measurement should be:

#### Step 1 — choose transmit and receive directions

Example:

- TX = North
- RX = South

#### Step 2 — configure TX route

Apply the validated `DRV_x / REL_x` combination for the chosen TX direction.

#### Step 3 — configure RX route

Set `MUX_A / MUX_B` for the chosen receive direction.

#### Step 4 — keep receive path disabled

Set:

```cpp
RX_EN = LOW;
```

#### Step 5 — clear old capture state

Before every shot, clear:

- edge seen flag
- captured edge timestamp
- timeout status

#### Step 6 — generate TX burst

Generate a fixed 40 kHz burst on `TX_PWM`.

#### Step 7 — apply blanking interval

After the burst, wait a fixed blanking interval with `RX_EN` still low.

This suppresses:

- electrical feedthrough
- transducer ringdown
- op-amp recovery artifacts
- early comparator false triggers

For the current hardware state, the blanking interval is especially important.

A practical starting tuning range is:

- **200 µs to 1000 µs**

#### Step 8 — enable receive path

Set:

```cpp
RX_EN = HIGH;
```

#### Step 9 — arm capture and wait for first valid edge

Wait for the first valid rising edge on `TOF_EDGE`, with a timeout.

Practical timeout range:

- **2 ms to 5 ms**

#### Step 10 — capture timestamp

On the first valid edge:

- store the timestamp
- compute delta relative to the chosen TX timing reference

#### Step 11 — disable receive path again

After either:

- first valid edge
- or timeout

set `RX_EN = LOW`.

#### Step 12 — clear TX routing

Return all `DRV_*` and `REL_*` lines to idle.

---

### 4.3 Timestamp reference choice

The firmware must choose a consistent TX timing reference.

Two practical choices are:

- burst start time
- burst end time

For early development, **burst end time** is often the easiest and most stable reference.

That means the measured value is:

> time from end of TX burst to first valid receive edge

This is not the final physical TOF directly, but it is a good development metric and can later be calibrated.

---

### 4.4 Interrupt behavior

The interrupt on `TOF_EDGE` should be minimal.

It should only:

- check that capture is armed
- store the first timestamp
- set a flag

Example:

```cpp
volatile bool edge_seen = false;
volatile uint32_t edge_time_us = 0;
volatile bool capture_armed = false;

void IRAM_ATTR onTofEdge() {
    if (capture_armed && !edge_seen) {
        edge_seen = true;
        edge_time_us = micros();
    }
}
```

Do not do heavy processing in the ISR.

---

### 4.5 Burst generation requirement

`TX_PWM` must produce a real 40 kHz burst.

A valid first implementation should provide:

- deterministic frequency
- deterministic cycle count
- deterministic burst start and end

A practical example is:

- **40 kHz**
- **12 cycles**
- **50% duty**
- **25 µs period**
- **12.5 µs high / 12.5 µs low**

For long-term reliability, hardware-timed generation is preferable over software bit-banging.

On ESP32 this could be done with:

- LEDC
- RMT
- MCPWM
- or careful timed toggling for initial testing

---

## 5. Required firmware data structures

### 5.1 RX routing map

This one is already defined:

```text
N -> A=0, B=0
S -> A=1, B=0
W -> A=0, B=1
E -> A=1, B=1
```

### 5.2 TX routing map

Firmware also needs a validated transmit truth table for each direction.

A clean way to structure this is with a configuration object per direction.

Example:

```cpp
struct TxRoute {
    bool drv_n, drv_e, drv_s, drv_w;
    bool rel_n, rel_e, rel_s, rel_w;
};
```

The final truth table should define which combination of `DRV_*` and `REL_*` is valid for:

- North transmit
- East transmit
- South transmit
- West transmit

---

## 6. Intended measurement pattern at system level

The hardware is intended to make paired directional measurements.

### Example for North/South axis

- TX = North, RX = South -> measure `t_NS`
- TX = South, RX = North -> measure `t_SN`

### Example for East/West axis

- TX = East, RX = West -> measure `t_EW`
- TX = West, RX = East -> measure `t_WE`

These opposite-direction times are then used to estimate:

- wind component along that axis
- effective speed of sound

---

## 7. Wind extraction once acoustic timing is trustworthy

For a path length `d`:

- `t_ab` = A to B travel time
- `t_ba` = B to A travel time

Then the wind component along that axis is:

```text
v = (d / 2) * (1/t_ab - 1/t_ba)
```

And the effective speed of sound along that path is:

```text
c = (d / 2) * (1/t_ab + 1/t_ba)
```

So the firmware goal is not just to detect one TOF, but to measure **paired directional TOFs** cleanly.

---

## 8. What is currently limiting clean operation

Based on prior bench testing, the current board state shows that:

- the ultrasonic TX/RX chain is electrically active and repeatable
- but detections are currently dominated by non-acoustic triggering

The strongest likely causes are:

- TX to RX electrical feedthrough
- comparator false triggering
- receive front-end overdrive
- missing front-end clamp behavior because `D8/D9` were removed

This means the hardware is alive, but the firmware currently must be conservative.

---

## 9. What the firmware should do right now on this revision

Until the receive front-end protection is corrected, firmware should:

- keep `RX_EN` low during transmit
- use a deliberate blanking period after TX
- reject obviously early arrivals
- gather repeated shots and use robust statistics
- avoid trusting single-shot detections

### Practical per-shot behavior for current hardware

1. set TX route
2. set RX route
3. clear edge flags
4. `RX_EN = LOW`
5. send burst
6. wait blanking
7. `RX_EN = HIGH`
8. wait for edge or timeout
9. capture time if valid
10. `RX_EN = LOW`
11. clear TX routes

### Practical per-direction result handling

- collect multiple shots
- reject impossible early events
- use median or trimmed mean
- compare OPEN / BLOCKED / disconnected behavior
- only treat measurements as real acoustic TOF once they show path dependence

---

## 10. Recommended firmware function structure

A clean firmware design would include functions like:

- `initUltrasonicPins()`
- `setRxDirection(Direction d)`
- `setTxDirection(Direction d)`
- `clearTxDirections()`
- `enableRx(bool on)`
- `sendBurst40kHz(int cycles)`
- `measureTof(Direction tx, Direction rx)`
- `runAxisPair(Direction a, Direction b)`
- `computeWindComponent(...)`

---

## 11. Practical pseudocode

```cpp
measureTof(tx_dir, rx_dir):
    clearTxDirections()
    setRxDirection(rx_dir)
    setTxDirection(tx_dir)

    edge_seen = false
    capture_armed = false

    RX_EN = LOW
    wait 50-200 us for routing settle

    t_tx_end = sendBurst40kHz(cycles=12)

    wait blanking_us
    capture_armed = true
    RX_EN = HIGH

    wait until edge_seen or timeout

    RX_EN = LOW
    capture_armed = false
    clearTxDirections()

    if edge_seen:
        return edge_time - t_tx_end
    else:
        return TIMEOUT
```

---

## 12. Bottom line

### The circuit is intended to do this

- `TX_PWM` creates the 40 kHz burst
- the TX directional network routes that burst to one chosen transducer
- the RX mux selects the opposite transducer
- the analog chain amplifies and band-limits the return
- the comparator turns the first usable event into `TOF_EDGE`
- the ESP32 timestamps that edge
- opposite-direction timings are combined into wind information

### The firmware must do this

- control TX routing
- control RX mux routing
- gate the RX chain with `RX_EN`
- generate deterministic TX bursts
- apply blanking
- capture the first valid comparator edge
- repeat in both directions
- validate and process TOF results

### On the current hardware revision

The biggest firmware requirement is **careful RX gating and blanking**, because the missing RX clamp stage makes the system more sensitive to feedthrough and false triggering.
