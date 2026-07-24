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

This provides RTC holdover while offline and fresh network time after Wi-Fi
becomes available.

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
