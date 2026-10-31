# LibreEcho timekeeping

LibreEcho keeps the kernel and PMIC real-time clock in UTC. Local timezone
presentation belongs at the UI/application boundary; NTP itself does not apply
timezones.

## Boot and synchronization flow

1. The kernel reads the MediaTek PMIC RTC during boot.
2. `libreecho-timed` creates `/dev/rtc0` from
   `/sys/class/rtc/rtc0/dev` when the minimal userspace has not populated the
   device node.
3. The daemon waits until a default route and DNS resolver are available.
4. BusyBox `ntpd -q` queries up to four validated public peers and exits only
   after a successful synchronization.
5. The daemon records that result in `/run/libreecho/time.status`.
6. Every successful synchronization writes the UTC clock back to the PMIC RTC
   with `hwclock -u -w`; normal operation repeats this periodically.

Every synchronization attempt has a bounded `--ntpd-timeout-seconds`
default of 30 seconds. A hung resolver or peer cannot block the daemon
indefinitely; the status file records `ntpd timed out` and the signal result,
then normal retry policy resumes.

## Default public sources

The image configuration at `/etc/libreecho/ntp.conf` uses:

- `time.cloudflare.com`
- `time.nist.gov`
- `ntp1.npl.co.uk`
- `ntp2.npl.co.uk`

These services publicly document general NTP access and use ordinary UTC leap
handling rather than a Google/Meta-style leap smear. LibreEcho deliberately
does not mix smeared and non-smeared sources.

The generic `*.pool.ntp.org` appliance defaults are not used. The NTP Pool
Project asks operating-system and appliance distributors to obtain a dedicated
vendor zone before shipping those names as defaults.

## Persistent override

An administrator may store one to four lines at:

```text
/data/libreecho/config/ntp.conf
```

Each line is either a hostname/address or `server hostname`. The daemon accepts
only alphanumeric characters, dots, hyphens, underscores, and colons, and
passes peers directly to `execv` without invoking a shell. An invalid
persistent file is ignored in favour of the immutable image defaults.

The override lives on userdata and therefore survives boot-image flashes and
OTA updates.

## Time zone

The clock stays UTC; the time zone is presentation only. It is a POSIX TZ
string stored as `timezone` in `web-config.json` (the image has no zoneinfo
database) and set from Device settings, the setup wizard, or Home Assistant:

- The picker lists IANA names and sends the matching POSIX string from
  `web/js/timezones.js`, generated from tzdata by `tools/generate-timezones.py`.
  The wizard starts from the browser's zone.
- With "Use the time zone from Home Assistant" on (`timezone_source:
  home_assistant`), esphomed asks the connected Home Assistant for the time
  when it subscribes and hourly after that. Home Assistant answers with its
  configured zone, which esphomed stores. Only the zone is taken; the clock
  itself still comes from NTP and the RTC.
- The initramfs exports the stored zone as `TZ` before the service graph
  starts. LED night mode, the assistant's clock and the live-session time tool
  also re-read it within seconds of a change (`src/timezone.h`); other daemons
  pick it up at the next boot.

The initramfs only exports letters, digits and `+-:,./`, so angle-bracket
abbreviations (`<+04>-4`) are stored as `LST`/`LDT` (`LST-4`). Offsets and
daylight-saving rules are unchanged.

## Runtime API

`GET /api/v1/system` reports:

- synchronization state and configured peers;
- last successful synchronization epoch;
- clock validity and source;
- PMIC RTC availability; and
- whether the latest synchronized time was persisted to the RTC.

The runtime status file is under `/run` and intentionally disappears on reboot,
so a new boot never claims an NTP synchronization that happened in an earlier
session.
