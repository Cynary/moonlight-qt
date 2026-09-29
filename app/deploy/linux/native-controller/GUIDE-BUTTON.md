# Steam button menus during native streaming

With `MOONMACHINE_GUIDE_TAPS=1`, the native Steam Controller helper routes:

- One tap: the Windows Steam overlay, after a 250 ms second-tap window.
- Two taps: the local Linux Steam menu.

This is for the 2026 Steam Controller using native forwarding. Windows Steam
Input still handles the game's layout. Other controls are forwarded immediately;
only the Steam button is withheld while distinguishing taps. Outside a native
stream, the helper is absent and the button keeps its normal local behavior.

## How it works

`guide_taps.py` observes the physical button in Triton state reports. It removes
that bit from forwarded reports, then emits one short native Guide press for a
single tap. A double tap invokes the local menu without sending a host Guide
press. Synthetic reports advance the sequence number and pass through the same
IMU clock normalization as ordinary reports. Release is generated even when no
new physical report arrives.

The physical controller remains visible to local Steam. `guide_guard.py` uses
Steam's local debugging endpoint to install `guide_guard.js`, which intercepts
only the 2026 Steam Controller's system Guide event in the shared UI input
source. Other buttons and other controller types retain their original handling.
The local-menu action calls the original handler. This UI hook relies on Steam
internals and must be checked after Steam updates; it is not a public Steam API.

The guard has a three-second lease renewed by the helper. It restores the
original handler on normal shutdown, and the lease restores it if the helper
crashes or loses the connection. Physical reports are filtered only after the
local guard is installed. If setup fails, normal Guide forwarding remains in
place and an explanation goes to Moonlight's log. Local Steam must have its
loopback debugging endpoint available at port 8080, and the helper needs the
optional `websocket-client` dependency listed in `requirements.txt`.

## Validation

Run:

```
python3 tests/native-controller/test_guide_taps.py
node tests/native-controller/test_guide_guard.js
```

These cover tap timing, held-button repeats, separated taps, cancellation,
sequence wrap, preserving other controls, short/unknown reports, controller
selection, local replay, repeated installation and lease expiry. A physical
controller check is also required to confirm that the current Steam build's UI
uses the intercepted path and Windows opens its overlay from the synthetic Guide
press. Automated checks alone do not establish that visual behavior.
