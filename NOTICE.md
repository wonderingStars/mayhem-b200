# Attribution

`mayhem-b200` is a derivative work of **PortaPack Mayhem**
(<https://github.com/portapack-mayhem/mayhem-firmware>), which is itself derived
from Jared Boone's original PortaPack firmware. It is distributed under the
GNU General Public License, version 2 or later — see `LICENSE.GPL-2.0-or-later`
(the upstream project's top-level `LICENSE` is GPL-3.0 and is included as
`LICENSE`; individual upstream source files carry GPL-2.0-or-later headers,
which is the licence this project inherits and continues under).

Upstream copyright holders include, but are not limited to:

- Copyright (C) 2014-2016 Jared Boone, ShareBrained Technology, Inc.
- Copyright (C) 2015-2024 Furrtek
- Copyright (C) 2023 Kyle Reed
- Copyright (C) 2024 zxkmm
- The PortaPack Mayhem contributors

## Files taken from Mayhem essentially unchanged

These retain their original copyright headers. Where a file was modified, the
modification is described and marked in the source with a `Host port note`
comment.

| File | Change |
|---|---|
| `src/ui/ui.hpp` | none |
| `src/ui/ui.cpp` | `sin_f32`/`int16_sin_s4` replaced with `std::sin`; long-press state comes from the Win32 event loop rather than the CPLD switch IRQ |
| `src/ui/ui_text.hpp`, `src/ui/ui_text.cpp` | none |
| `src/ui/ui_painter.hpp` | added `draw_line` |
| `src/ui/ui_painter.cpp` | `portapack::display` replaced with the host framebuffer; drawing logic unchanged |
| `src/ui/ui_focus.hpp`, `src/ui/ui_focus.cpp` | none |
| `src/ui/ui_font_fixed_8x16.*`, `src/ui/ui_font_fixed_5x8.*` | none — the glyph data is Mayhem's, which is why the UI looks right |
| `src/ui/theme.hpp`, `src/ui/theme.cpp` | none |
| `src/ui/spectrum_color_lut.hpp`, `src/ui/spectrum_color_lut.cpp` | none |

## Files reimplemented against Mayhem's design

Written for the host, but deliberately keeping upstream's class names, method
signatures and painting behaviour so that app code reads the same on both sides:

- `src/ui/ui_widget.{hpp,cpp}` — the widget set
- `src/ui/ui_menu.{hpp,cpp}` — `MenuView`
- `src/ui/ui_spectrum.{hpp,cpp}` — waterfall and frequency scale, including the
  vertical-scroll-region technique from `WaterfallWidget`
- `src/ui/display.{hpp,cpp}` — the `lcd::ILI9341` drawing API over a software
  framebuffer, with identical scroll arithmetic
- `src/apps/ui_navigation.{hpp,cpp}` — `NavigationView`, `SystemStatusView`
- `src/apps/event_dispatch.{hpp,cpp}` — `EventDispatcher`
- `src/core/string_format.{hpp,cpp}` — same functions, same output
- `src/radio/receiver_model.{hpp,cpp}` — `ReceiverModel` and the modes its
  M4 baseband processors implement
- `src/apps/analog_audio_app.*`, `src/apps/capture_app.*` — the corresponding
  Mayhem apps

The `.C16` IQ format and the `.TXT` metadata sidecar (`center_frequency=` and
`sample_rate=` lines) follow `firmware/application/metadata_file.cpp` so
recordings are interchangeable with PortaPack captures.

## Everything else

`src/radio/usrp_radio.*`, `src/audio/*`, `src/dsp/*`, `src/ui/window.*`,
`src/ui/input.*`, `src/core/file_path.*` and the remaining apps are original to
this project and are also GPL-2.0-or-later.

UHD is Copyright Ettus Research / National Instruments, licensed GPL-3.0-or-later.
This project links against it; no UHD source is included here.
