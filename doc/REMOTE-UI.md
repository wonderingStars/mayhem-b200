# Remote UI protocol

How a browser (or any other client) drives **all** of mayhem-b200's apps.

This describes what is implemented. An earlier revision of this file specified a
different, single-WebSocket design with an `MBUI` magic and a 14-byte header;
none of that was ever built, and this replaces it. If you find a client written
against `MBUI`, `/ui`, `GET /ui/apps` or a `hello`/`welcome` handshake, it was
written against that draft.

## Why it works this way

mayhem-b200 has ~103 apps. Reimplementing their screens in a web front end would
mean 103 rewrites, and — because those apps descend from GPL PortaPack Mayhem
code — a reimplementation carrying that lineage would be a derivative work
wearing a different licence. Neither is acceptable.

The app already draws every screen into one 240x320 framebuffer
(`host::Display`) and already routes every key, encoder tick and touch through
one place (`app::EventDispatcher::dispatch`). So the remote UI is not a
reimplementation at all:

```
   browser  ──input events──►  mayhem-b200 (the real app, GPL)
            ◄───frames──────   the real 103 apps, unchanged
```

Every app works remotely the day it is written, because it *is* the app. No
per-app work, no duplicated decoders, no licence problem: this protocol is part
of mayhem-b200 and is GPL like the rest of it.

## The two hops

There is no single socket. mayhem-b200 itself serves plain HTTP; the Go portal
(`webgui/`) is a separate process that consumes that HTTP and gives the browser
a WebSocket:

```
  browser ──WebSocket /api/screen/ws──► mayhem-portal ──HTTP /api/screen──► mayhem-b200
          ◄───frames, status──────────               ◄──frames────────────
          ──{"events":[...]}─────────►               ──POST /api/input───►
```

A client that would rather not hold a socket open can talk to either side over
plain HTTP: the portal proxies `/api/screen` and `/api/input` through unchanged.

Start the C++ side with `--portal[=port]` (default 8090); see
`src/remote/remote_server.hpp` for the trust boundary, which is the important
part now that input is accepted.

## Hop 1 — screen frames, mayhem-b200 → portal

```
GET /api/screen                        the current frame immediately (204 if none yet)
GET /api/screen?after=SEQ&wait_ms=MS    the next frame with seq > SEQ, waiting up
                                        to MS milliseconds (capped at 10000);
                                        204 on timeout
```

`after=0` is not a sequence number: it means "whatever you have now", and is what
a bare `GET` and the portal's resync-after-restart both send. `seq` 0 is reserved
for "nothing has been captured yet".

A 200 body is binary, `Content-Type: application/octet-stream`:

```
offset  size   field
0       4      magic     "MBSF"
4       1      version   1
5       1      format    1 = raw RGB565 little-endian
6       2      width     uint16 LE (240)
8       2      height    uint16 LE (320)
10      4      seq       uint32 LE
14      2      reserved  0
16      w*h*2  payload   RGB565 little-endian, rows top to bottom
```

RGB565 rather than RGB888 halves the bytes for no visible loss — the app's own
framebuffer is RGB565 natively, so it is also the cheapest thing to send.

`seq` increments only when the display reported damage, so an idle menu costs
nothing: the long poll simply does not return. Capture happens on the UI thread
after `present()`, and reads the display's *non-destructive* damage counter —
`Display::take_damage()` belongs to the window layer and consuming it here would
stop the local window repainting.

## Hop 1 — input, portal → mayhem-b200

`POST /api/input`, body `{"events":[ ... ]}`, where each event is one of:

| Event | Fields |
|---|---|
| key | `{"type":"key","key":"up\|down\|left\|right\|select\|back","down":true\|false}` |
| encoder | `{"type":"encoder","delta":<signed detents>}` |
| touch | `{"type":"touch","x":0..239,"y":0..319,"phase":"start\|move\|end"}` |
| char | `{"type":"char","c":<int 0x20..0xFF>}` |

Keys are sent for press *and* release, so long-press works. `KeyEvent::Dfu`
exists in the app and is deliberately unreachable from the network: it is the
firmware's recovery entry point, not a navigation key. Neither is Quit — the
queue cannot carry one, so a remote client cannot close the application.

The response is `{"queued":N,"dropped":M}` at HTTP 200. `dropped` counts events
of *this* request that will never be dispatched: unknown types, malformed
events, out-of-range coordinates (dropped rather than clamped, since clamping
turns a client bug into a touch the operator never asked for). An event this
build does not understand is dropped and counted, never an error.

Events are enqueued from the HTTP thread and drained on the UI thread, in the
same place and the same frame a local key would be handled. Nothing is ever
dispatched from a connection thread.

## Hop 2 — the browser WebSocket

`ws://<portal>/api/screen/ws`.

**Server → client, binary:** exactly the MBSF frame above, except that `format`
may also be **2**: the same 16-byte header followed by a raw-DEFLATE payload
(Go `compress/flate`; the browser inflates with
`DecompressionStream("deflate-raw")`). Worst case uncompressed is 240*320*2 =
150 kB per frame, which is why format 2 exists; a typical menu compresses ~35x.
`?deflate=0` opts out and gets format 1.

**Server → client, text:** `{"type":"status","controlling":bool,"viewers":N}`,
sent on join and whenever either value changes.

**Client → server, text:** `{"events":[...]}`, the same event schema as
`POST /api/input`.

## Rules

- **One controlling client at a time.** The app has a single UI state; two
  clients fighting over the same menu is not a feature. The first client to
  connect controls; later ones are read-only and are told so in their `status`.
  Input from a non-controlling client is ignored, not rejected — a page whose
  user pressed a key has done nothing wrong, and answering with an error would
  only teach it to treat control as a failure condition.
- **Handover is by seniority.** When the controller disconnects, the
  longest-connected remaining client is promoted and gets a fresh `status`.
- Input is applied on the UI thread between frames, never concurrently with
  painting.
- Losing the socket does not close the app; it keeps running and the next client
  picks up where it left off. The portal releases any keys the departing
  controller was holding, so a tab that dies mid-press cannot leave a key
  latched down on the device.
- **Transmit is not gated by this protocol.** If an app can transmit, a remote
  client can trigger it exactly as a local user could. An operator exposing this
  beyond localhost is responsible for who reaches it — see the warning on
  `RemoteServer::start()` and in the `--portal` help text.

## Where the code is

| Piece | File |
|---|---|
| Frame capture, input queue, panel cache | `src/remote/app_bridge.cpp` |
| HTTP routes | `src/remote/remote_server.cpp` |
| Frame/input decode in Go | `webgui/internal/portal/client/screen.go` |
| Hub, control rules, deflate | `webgui/internal/portal/server/screen.go` |
| Browser side | `webgui/internal/portal/server/static/screenview.js` |
| Structured panels (a separate thing) | `webgui/internal/portal/PANELS.md` |
