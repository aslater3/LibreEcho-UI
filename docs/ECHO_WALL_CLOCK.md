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

The partner opens the data channel. The clock becomes connectable, runs an SDP
search for `0x1101`, then **waits to be called** on RFCOMM 4 and drops the link
after ~58 s. Listening alone deadlocks: both sides wait.

| Event | What LibreEcho sends |
|---|---|
| connect | `Discover`, then `ClearIndicator`, then the alerts it holds |
| connect, no time known or last sync ≥ 30 min ago | also `StateUpdate timeinfo` |
| timer started / resumed | `SetAlert(token, TIMER, end-time)` |
| timer paused / cancelled | `DeleteAlert(token)` |
| countdown, alarm or reminder fires | `StateUpdate timers=active`, re-sent every 12 s while it rings |
| ringing stopped | `StateUpdate timers=cleared` |
| half hour, value changed | `StateUpdate timeinfo` |
| `POST /api/v1/bluetooth/clock-resync` | clears the stored timeinfo so the next connect sends it |

`scheduledTime` is the alert's **end time**, local ISO-8601 with offset, not a
duration. `timeinfo` carries **local** time: the clock ignores the offset and
shows the digits as written.

Three details matter and are easy to get wrong:

- The state key is always `timers`. A real Echo never sends `alarms` or
  `reminders` there, even when a reminder finished.
- `SetAlert` alone only plays the "set" animation. The pulse needs the
  matching `StateUpdate`.
- A `DeleteAlert` during a pulse cuts it short, so removing a finished alert is
  deferred by 11 s.

A `timeinfo` costs the clock a full twelve-hour sweep of its hands. One
revolution of the minute hand measures ~17 s, so a sweep is ~204 s. It is
therefore sent on the half hour, not on every reconnect; the clock keeps its
own time in between.

## Known gaps

- The **Disney Mickey Mouse Edition is what was tested on hardware**; the
  `EchoWallClk10U` variant in the protocol capture has not been. Its EIR name
  differs, which is the only known difference.
- Reminders are spoken by `timerd` through `ttsd`, not by the clock. The clock
  pulses at the same moment.
- Whether a real Echo holds the link for hours is unmeasured; `btd` keeps it
  open for as long as the clock stays connected.
