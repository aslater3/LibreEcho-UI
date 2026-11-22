# Echo Wall Clock (Alexa Gadget over SPP)

LibreEcho serves an **Amazon Echo Wall Clock** as an Alexa Gadget: the device
shows the time, mirrors timers, alarms and reminders on its LED ring, and lights
its notification LED. No Echo and no Amazon account are involved.

Tested against the protocol captured from a real Echo Dot in
[mpjanofsky/echo-wall-clock](https://github.com/mpjanofsky/echo-wall-clock)
(Apache-2.0, [`PROTOCOL.md`](https://github.com/mpjanofsky/echo-wall-clock/blob/main/PROTOCOL.md)).
That capture is the authority for the message set below; the codec in
`src/adapter/bt_agt.c` reproduces those frames byte for byte and
`tests/test_bt_agt.c` pins them.

## Supported hardware

| Device | Advertised name | Notes |
|---|---|---|
| Echo Wall Clock | `EchoWallClk10U` | Protocol captured from this unit (FCC ID UUU-8459) |
| Echo Wall Clock, Mickey Mouse Edition | `EWCMickeyMouse05B` | Reported to speak the same protocol; **not tested here** |

The clock is a **Bluetooth Classic** gadget, not BLE. It appears only in a
Classic (BR/EDR) inquiry.

## What LibreEcho provides

- **SPP on RFCOMM channel 4**, served by the kernel (`CONFIG_BT_RFCOMM`),
  advertised in the SDP record set as `Serial Port` (UUID `0x1101`) with the
  protocol list naming RFCOMM channel 4. The clock searches for exactly that
  record when it calls its partner back after pairing.
- **AGT framing and protobuf** (`bt_agt.c`): `Discover`, `StateUpdate`,
  `SetAlert`, `DeleteAlert`, `SetIndicator`, `ClearIndicator`.
- **Wall-clock state** (`bt_gadget.c`): the time to report and the set of
  active alerts, plus the once-per-ring `timers=active` semantics.

`GET /api/v1/bluetooth` reports `profile_services.rfcomm` as `true` once the
SPP listener is up, so the control centre can show it.

## Connecting the clock

Pairing is the delicate part, and it is a clock-side behaviour, not something
LibreEcho can relax:

1. Put the clock in pairing mode: **hold its button until the centre LED
   flashes amber**.
2. Pair and open the connection **in one window**. The clock is discoverable
   only while flashing, so pairing and connecting later usually fails with
   "Host is down".
3. The clock then **calls the partner back** and searches for SPP `0x1101`.
   LibreEcho must be connectable and advertising that record, which is what the
   SPP listener above does. Stay connectable for that call-back.
4. Once bonded, the clock connects at will; pairing mode is not needed again.

The clock talks to **one partner at a time**. Pairing it to an Echo again takes
it away from LibreEcho.

## Message ordering

Reproduced from the real-Echo capture (`PROTOCOL.md` section 5b):

| Event | What LibreEcho sends |
|---|---|
| connect | `Discover`, then `StateUpdate timeinfo`, then `ClearIndicator` |
| clock replies `Discover.Response` | the session is live; re-send active alerts |
| timer started / resumed | `SetAlert(token, TIMER, end-time)` |
| timer paused / cancelled | `DeleteAlert(token)` |
| timer ringing | `StateUpdate timers=active`, **once** |
| ringing stopped | `StateUpdate timers=cleared`, then `DeleteAlert(token)` |
| DST change / hourly | `StateUpdate timeinfo` |

`scheduledTime` is the alert's **end time**, local ISO-8601 with offset, not a
duration.

## Known gaps

- The match is on the protocol capture; the **Disney Mickey Mouse Edition is
  untested on hardware** (see the table above). Its EIR name differs, which is
  the only known difference.
- Alert mirroring is wired into the SPP session but not yet driven from
  `timerd`; until that lands the clock shows the correct time and clears a
  stale indicator on connect, and timers reach it only through the gadget state
  API.
- `timeinfo` is not yet refreshed on the half hour.
