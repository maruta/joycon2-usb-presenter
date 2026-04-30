# Joy-Con 2 USB Presenter

Firmware for nRF52840 USB dongles that turns a Nintendo Switch 2 Joy-Con
into a wireless **presentation controller** — keyboard arrows, mouse
pointer (gyro + optical), scroll wheel, and laser-pointer modifier — over
a single USB plug. The host sees a composite USB device exposing a
**HID keyboard**, a **HID mouse**, and a **CDC ACM serial console**.

## Disclaimer — read this before you start

This is a hobby project published as-is. There is **no warranty and no
support obligation** of any kind, express or implied. Specifically:

- **Use at your own risk.** Flashing custom firmware to a USB dongle,
  pairing experimental BLE clients to a Joy-Con 2, and re-pairing a
  Joy-Con 2 to a Switch 2 are all things that can go wrong. By building
  or installing this firmware you accept that risk.
- **No help with flashing or build problems.** "It won't flash", "the
  Programmer doesn't see my dongle", "my toolchain doesn't build", etc.
  are between you, your hardware vendor, and Nordic's documentation.
  Issue reports about generic toolchain or DFU problems will likely be
  closed without a fix.
- **Pairing with this dongle invalidates the Joy-Con 2's bond with your
  Switch 2.** You will need to re-pair the Joy-Con 2 with the Switch 2
  via the standard Switch 2 controllers menu afterward. This is intended
  behavior, not a bug.
- **No liability for hardware damage** — including the dongle, the
  Joy-Con 2, the host PC, the Switch 2, or anything else in the BLE/USB
  vicinity. The Joy-Con 2 BLE protocol is community-reverse-engineered;
  unknown commands could theoretically put the controller into an
  unexpected state. Nothing in this firmware is known to do so today,
  but you accept the residual risk by running it.
- The Apache License 2.0 (see end of this file) already disclaims
  warranty and limits liability legally; this section is just a
  plain-language summary so expectations are clear up front.

If something doesn't work, please **debug it yourself** before opening
an issue, and include reproduction details if you do file one.

## Hardware

- **Dongle**: tested on **Raytac MDBT50Q-CX-40** (nRF52840 USB dongle).
  Other nRF52840 USB dongles with the same Zephyr board target should
  work; you may need a board-specific overlay if pinout differs.
- **Controller**: Nintendo Switch 2 **Joy-Con 2** (left or right).

## Software prerequisites

- **nRF Connect SDK** (NCS) **v3.3.0** — verified version. Newer versions
  may need minor adjustments (USB device_next API surface, BT scan helper
  field names).
- **Toolchain** matching that NCS version (the Zephyr SDK / arm-zephyr-eabi).
- One of the two front-ends below.

The two officially supported ways to set up the SDK and toolchain are
both listed here. Pick whichever you prefer.

## Build & flash — option A: VS Code (recommended)

This is the path the project was developed on, so it has been smoke-
tested end to end.

1. Install the **nRF Connect for VS Code** extension pack from the VS
   Code Marketplace.
2. Open the extension's welcome page, then under **Manage toolchains**
   install the toolchain that pairs with NCS v3.3.0. Under **Manage
   SDKs** install **nRF Connect SDK v3.3.0**. Both can take a while —
   they download multi-GB archives.
3. Clone this repository and open the project folder in VS Code (`File
   → Open Folder…`).
4. In the nRF Connect side panel, click **Add build configuration** for
   this application. In the dialog:
   - **Board target**: `raytac_mdbt50q_cx_40_dongle/nrf52840`
   - Leave the rest at defaults
   - Click **Build Configuration**
5. Wait for the initial CMake/build to finish. The HEX image lands at
   `build/joycon2-usb-presenter/zephyr/zephyr.hex`.
6. Subsequent rebuilds: click **Build** in the side panel.

If you change `prj.conf`, `app.overlay`, `Kconfig.sysbuild`, or anything
that affects the build system, click **Pristine Build** instead of
**Build** so CMake re-runs cleanly.

## Build & flash — option B: command line (`west`)

```bash
west build -b raytac_mdbt50q_cx_40_dongle/nrf52840 -p
```

The HEX image lands at
`build/joycon2-usb-presenter/zephyr/zephyr.hex`.

### Flash

`west flash` does **not** work with this dongle. Use the **nRF Connect
for Desktop → Programmer** app instead, going through the on-chip USB
DFU bootloader.

1. Unplug the dongle.
2. Hold down the on-board user button **while** plugging it in. Keep
   holding until Windows enumerates the dongle as a DFU device (a
   different VID/PID than the running firmware).
3. Open **nRF Connect for Desktop → Programmer**, select the dongle in
   the device list, choose **Add file → Browse**, pick
   `build/joycon2-usb-presenter/zephyr/zephyr.hex`, then click
   **Write**.
4. After the write completes the dongle re-enumerates with the new
   firmware.

Re-entering DFU later requires the same button-hold-on-plug-in dance —
the running firmware does not provide an automatic DFU trigger.

## Pairing the Joy-Con 2

> **Heads-up**: pairing the Joy-Con 2 with this dongle invalidates its
> bond with your Switch 2. Re-pair on the Switch 2 afterward by holding
> any button while in the Controllers menu.

1. *(recommended)* On Switch 2 → **Settings → Controllers → Disconnect
   Controllers** to release the bond cleanly.
2. Hold the small **SYNC** button on the Joy-Con 2 side rail for ~5
   seconds until the player LEDs scroll.
3. Plug in the dongle. It scans on boot, auto-connects, and lights
   **LED4** when ready (≈3 seconds after pairing-mode entry).

## Mapping

### Keyboard

| Joy-Con button     | Sends |
|--------------------|-------|
| `RIGHT`, `A`       | →     |
| `LEFT`, `Y`        | ←     |
| `UP`, `X`          | ↑     |
| `DOWN`, `B`        | ↓     |
| `MINUS`, `PLUS`    | P     |
| `CAPT`, `C`        | E     |

### Mouse buttons

| Joy-Con input              | Action       |
|----------------------------|--------------|
| `ZR`, `ZL`                 | Left click   |
| `R`, `L`                   | Right click  |
| `RJ`, `LJ` (stick press)   | Middle click |

### Cursor

The cursor is driven by **gyro** *and* the **optical mouse sensor**
simultaneously. They sum, so both sources contribute and neither has to
be silenced — the optical sensor reports near-zero deltas in mid-air, and
the gyro is gated by a button press.

- **Gyro pointing**: hold any side-rail button (`L_SL`, `L_SR`, `R_SL`,
  `R_SR`) or any click button (`ZR`, `ZL`, `R`, `L`) → the gyro moves the
  cursor while you wave the Joy-Con. X-axis gyro = vertical, Z-axis gyro
  = horizontal (both inverted so the cursor tracks where you point).
- **Optical mouse**: place the Joy-Con on a flat surface and slide it
  → the bottom-edge optical sensor moves the cursor. Always active.

### Scroll wheel

Either stick scrolls the page. Both axes are mapped:

- Stick **X** → horizontal wheel
- Stick **Y** → vertical wheel

Scroll **speed scales with stick deflection** — gentle tilt scrolls
slowly, full deflection scrolls fast.

### Laser-pointer mode (toggle)

Tap `L_SL` or `R_SR` to **toggle laser-pointer mode**. While ON, **Ctrl**
is held automatically whenever:

- the **left mouse button** is pressed (`ZR`/`ZL`), or
- a **scroll wheel event** is being generated.

Useful with browsers / slide tools (Ctrl+click to open in new tab,
Ctrl+wheel to zoom). The mode persists until you tap `L_SL` or `R_SR`
again.

### LED indicators

- **LED4**: connected
- **LED1**: laser-pointer mode active
- LED2 and LED3: unused

## Tuning

Edit constants near the top of [src/main.c](src/main.c):

| Constant | Effect |
| --- | --- |
| `GYRO_DEADZONE` | Raise to ignore more idle jitter |
| `GYRO_DIVISOR` | Increase to slow gyro pointing, decrease to speed it up |
| `OPTICAL_DIVISOR` | Same, for the optical mouse sensor |
| `STICK_WHEEL_THRESH` | Stick deadzone before scrolling kicks in |
| `WHEEL_ACCUM_THRESH` | Decrease for faster scroll at full deflection |

## Files

```text
joycon2-usb-presenter/
├── src/main.c        # All firmware logic (BLE central + HID + mapping)
├── prj.conf          # Zephyr Kconfig overrides
├── app.overlay       # USB HID device-tree node
├── CMakeLists.txt    # Build glue
└── README.md
```

## Acknowledgements

Joy-Con 2 is a new device and its BLE protocol is still being reverse-
engineered by hobbyists. This firmware would not exist without their
work:

- [**Nohzockt/Switch2-Controllers**](https://github.com/Nohzockt/Switch2-Controllers)
  — Joy-Con 2 BLE scanning, button bitmap, stick layout
- [**seitanmen/Joycon2forMac**](https://github.com/seitanmen/Joycon2forMac)
  — input-enable command sequence
- [**Misaka10571/joycon2-connector**](https://github.com/Misaka10571/joycon2-connector)
  — dedicated write characteristic UUID, IMU and optical mouse layout,
  player-LED command format

The Bluetooth-host bring-up scaffolding was originally inspired by
Nordic Semiconductor's NCS sample `samples/bluetooth/central_hids`, but
all of the code in this repository has been reimplemented from scratch
against the public Zephyr Bluetooth and USB APIs.

## Trademarks

Nintendo, Nintendo Switch, Joy-Con, and related marks are trademarks of
Nintendo Co., Ltd. This project is an independent, unofficial work and is
**not affiliated with, endorsed by, or sponsored by Nintendo Co., Ltd.**
Product names appear here only for compatibility identification.

## License

Apache License 2.0 — see [LICENSE](LICENSE).
