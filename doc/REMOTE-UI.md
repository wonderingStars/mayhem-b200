# Remote UI protocol

How a browser (or any other client) drives **all** of mayhem-b200's apps.

## Why it works this way

mayhem-b200 has ~103 apps. Reimplementing their screens in a web front end would
mean 103 rewrites, and — because those apps descend from GPL PortaPack Mayhem
code — a reimplementation carrying that lineage would be a derivative work
wearing a different licence. Neither is acceptable.

The app already draws every screen into one 240x320 framebuffer
(`host::Display::composite_bgra`) and already routes every key, encoder tick and
touch through one place (`app::EventDispatcher::dispatch`). So the remote UI is
not a reimplementation at all:

```
   browser  ──input events──►  mayhem-b200 (the real app, GPL)
            ◄───frames──────   the real 103 apps, unchanged
```

Every app works remotely the day it is written, because it *is* the app. No
per-app work, no duplicated decoders, no licence problem: this protocol is part
of mayhem-b200 and is GPL like the rest of it.

## Transport

One WebSocket, default `ws://<host>:8090/ui`. Text frames are JSON control
messages; binary frames are screen updates. A client may also `GET /ui/apps`
over plain HTTP for the app list without opening a socket.

## Client -> server (JSON text frames)

| `type` | Fields | Meaning |
|---|---|---|
| `hello` | `{"proto":1,"want_scale":2}` | First message. `want_scale` is a hint only; frames are always sent at 240x320 and the client scales. |
| `key` | `{"key":"up\|down\|left\|right\|select\|back\|dfu","down":true}` | Five-way switch. Sent for press and release so long-press works. |
| `encoder` | `{"delta":1}` | Rotary encoder detents, signed. |
| `touch` | `{"x":120,"y":160,"phase":"start\|move\|end"}` | Touch panel, in 240x320 coordinates. |
| `char` | `{"c":65}` | A printable character (0x20..0xFF) for text entry. |
| `launch` | `{"id":"adsbrx"}` | Open a registered app by its registry id. |
| `home` | `{}` | Pop back to the main menu. |
| `ping` | `{}` | Keepalive. |

## Server -> client

**JSON text frames**

| `type` | Fields |
|---|---|
| `welcome` | `{"proto":1,"width":240,"height":320,"version":"0.9.1"}` |
| `apps` | `{"apps":[{"id","name","category","hardware_limited"}...]}` sent after `hello` |
| `view` | `{"title":"ADS-B RX","can_go_back":true}` whenever the view changes |
| `status` | `{"device":"B200 EDR04ZDB2","receiving":true,"transmitting":false}` |
| `pong` | `{}` |

**Binary frames — screen updates**

```
offset  size  field
0       4     magic     "MBUI"
4       1     format    1 = full RGB565, 2 = full RGB565 zlib-deflated
5       1     flags     reserved, 0
6       2     width     uint16 LE (240)
8       2     height    uint16 LE (320)
10      4     seq       uint32 LE
14      ...   payload   width*height uint16 RGB565, little-endian, top-to-bottom
```

RGB565 rather than RGB888 halves the bytes for no visible loss — the app's own
framebuffer is RGB565 natively, so it is also the cheapest thing to send.

A frame is only sent when the display reports damage (`Display::take_damage`),
so an idle menu costs nothing. Worst case (a full-rate waterfall) is
240*320*2 = 150 kB per frame; at 20 fps that is ~3 MB/s uncompressed, which is
why format 2 exists and should be preferred on anything but localhost.

## Rules

- **One controlling client at a time.** The app has a single UI state; two
  clients fighting over the same menu is not a feature. A second connection is
  accepted read-only (frames, no input) and told so in `welcome`.
- Input is applied on the UI thread between frames, never concurrently with
  painting.
- Losing the socket does not close the app; it keeps running and the next client
  picks up where it left off.
- **Transmit is not gated by this protocol.** If an app can transmit, a remote
  client can trigger it exactly as a local user could. An operator exposing this
  beyond localhost is responsible for who reaches it — see the warning in the
  server's `--remote-ui` help text.
