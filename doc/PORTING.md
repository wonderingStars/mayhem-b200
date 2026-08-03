# Porting contract

Read this before touching anything. It is the shared context for everyone
working on `mayhem-b200`.

## What this project is

PortaPack Mayhem's UI and apps, rebuilt as a **Windows host application** that
drives an **Ettus USRP B200** through UHD. Mayhem is firmware for an LPC43xx
with an LCD and buttons; a B200 is a USB peripheral with no application
processor. So the firmware is not flashed anywhere — its interface and apps are
reimplemented on the host, with UHD replacing the SGPIO/HackRF front end.

Upstream source to port *from* is unpacked at:

```
../_mayhem_src/mayhem-firmware-next/firmware/
```

Read the original app before writing its replacement. Do not invent behaviour
that upstream does not have, and do not guess at a protocol — the upstream
implementation is the specification.

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build
```

```bash
ctest --test-dir build --output-on-failure
```

MSVC 2022 Build Tools, C++20. Sources are globbed per directory, so **adding a
`.cpp` needs no CMake edit**. Everything must compile warning-clean-ish under
`/W4` (the vendored PortaPack files are exempt).

## Layout

| Directory | Contents |
|---|---|
| `src/ui/` | Mayhem's UI core (ported), host display/window/input, widgets |
| `src/dsp/` | filters, FFT, demodulators, modulators, protocol primitives |
| `src/radio/` | UHD backend, receive and transmit chains |
| `src/audio/` | WinMM output and capture |
| `src/apps/` | app views and navigation |
| `src/core/` | string formatting, paths, file formats |
| `tests/` | one `test_*.cpp` per area, auto-globbed |

## Registering an app (READ THIS — it is how your app appears in the menu)

Apps self-register. There is no central list to edit. In your app's `.cpp`, after
the `namespace app { ... }` block that defines the view, add:

```cpp
#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_myapp{{
    "myapp",                    // id: the upstream short name (e.g. "adsbrx")
    "My App",                   // display name shown on the tile
    app::Category::Receive,     // Receive/Transmit/Transceiver/Utilities/Games/Settings/Home/Debug
    ui::Color::green(),         // tile colour (green=RX, follow upstream's iconColor)
    &ui::bitmap_icon_adsb,      // icon from bitmaps.hpp, or nullptr for a generic tile
    [] { return std::make_unique<app::MyAppView>(); },
    false                       // hardware_limited: true only for PortaPack-only apps
}};
}
```

Pick the `Category` from the app's upstream `app_location_t`. Pick the closest icon
in `bitmaps.hpp`; if none fits, pass `nullptr` (you get a generic tile) — do not
invent a new bitmap unless the app clearly needs one.

The registry only works because the app sources are an OBJECT library, so your
file-scope Registrar always runs. Do not add your app to any list by hand.

## Do NOT create shared/common files

You are one of many agents running at once. Create ONLY your own app's files
(`ui_<app>.hpp` / `.cpp`, and a `test_<app>.cpp`). If you need a helper that does
not exist in Phase A, implement it inside your own files, not a new shared header —
another agent may be creating a file by the same name at the same time. The one
exception is your Registrar, which lives in your own `.cpp`.

## Conventions that are not negotiable

1. **One app per `.hpp`/`.cpp` pair** in `src/apps/`, named after the upstream
   file where one exists (`ui_adsb_rx.*` stays `ui_adsb_rx.*`).

2. **App views derive from `ui::View`** and override `title()`. Push and pop
   through `app::globals().nav`. Never `delete` a view — the navigation stack
   owns them.

3. **Reach shared state through `app::globals()`** (`app_context.hpp`), never a
   new global. It carries `radio`, `receiver`, `transmitter`, `audio_out`,
   `audio_in` and `nav`.
   *Do not call the free function `context()` inside a `ui::View` subclass* —
   it resolves to `ui::Widget::context()`. That is why it is named `globals()`.

4. **`ui::Style` has const members.** `auto s = style(); s = s.invert();` does
   not compile. Write
   `const Style s = focused ? style().invert() : style();`.

5. **Per-frame work goes in `on_frame_sync()`**, which the main loop calls on
   every visible widget at ~60 Hz. There is no message queue from a second
   core. Do not spawn a thread from a view.

6. **Widgets are members of the view, not heap allocations.** Add them with
   `add_children({&a, &b, ...})` in the constructor.

7. **The screen is 240x320.** The status bar takes the top 16 px, so a view's
   usable area is 240x304 in view-local coordinates starting at 0,0.

8. **Colour escapes in strings** work as upstream: `STR_COLOR_GREEN "text"`.

9. **Tests are part of the change, not a follow-up.** Anything with logic that
   can be checked without hardware — a CRC, a packet parser, a modulator, a
   frequency calculation, a file format — gets a test in `tests/`. Test against
   values derived from the protocol or from upstream's implementation, not
   against whatever your code happens to produce.

10. **Never leave a stub that pretends to work.** A view that cannot do its job
    yet should say so on screen. Silence that looks like success is worse than
    an honest message.

## What replaces what

| Firmware | Host |
|---|---|
| `portapack::display` (ILI9341) | `host::display`, same drawing API |
| `baseband::` messages to the M4 | direct DSP calls on the DSP thread |
| `ReceiverModel` / `TransmitterModel` | `radio::ReceiverModel` / `radio::TransmitterModel` |
| SD card paths (`/CAPTURES` etc.) | `core::captures_directory()` and friends |
| `portapack::persistent_memory` | `core::Settings` (INI under the data dir) |
| ChibiOS threads | `std::thread`, or better, `on_frame_sync()` |
| `rf::Frequency` | `uint64_t` Hz |

## Radio facts (verified against the Ettus UHD manual)

- 70 MHz – 6 GHz, 1 TX + 1 RX, full duplex, USB 3.0
- Master clock 5 – 61.44 MHz; up to 61.44 Msps, 56 MHz analog bandwidth
- Analog filter settable 200 kHz – 56 MHz
- RX gain ~0–76 dB, TX gain ~0–89.8 dB, TX +20 dBm max, RX −15 dBm max
- Antennas: `TX/RX`, `RX2`

**Never hard-code these.** Read them from `radio->caps()`, which is populated
from the device; the published figures are only the no-device fallback.

## Honesty rules

- If an app depends on PortaPack hardware that a B200 does not have (SD card
  firmware flashing, the CPLD, on-board I2C sensors, the battery gauge), do not
  fake it. Implement the parts that do apply and say plainly on screen what is
  unavailable and why.
- No hardware is attached during development. Anything that can only be proven
  with RF is unverified — say so, do not claim otherwise.
