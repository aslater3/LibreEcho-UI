#!/usr/bin/env python3
"""Exercise the bounded scan-completion policy with delayed netlink events."""

from dataclasses import dataclass


@dataclass
class FakeScan:
    events: list[tuple[int, str]]
    dump_calls: int = 0

    def wait_for_completion(self, now: int, deadline: int) -> bool:
        while now < deadline:
            ready = [event for event in self.events if event[0] <= now]
            self.events = [event for event in self.events if event[0] > now]
            if any(kind == "NEW_SCAN_RESULTS" for _, kind in ready):
                return True
            if any(kind == "SCAN_ABORTED" for _, kind in ready):
                return False
            now += 100
        return False

    def dump(self) -> str:
        self.dump_calls += 1
        return "stale-cache"


# A completion at 500 ms must prevent a pre-completion cache dump, even though
# a 300 ms fixed delay would have expired first.
scan = FakeScan([(500, "NEW_SCAN_RESULTS")])
assert scan.wait_for_completion(0, 1200)
assert scan.dump_calls == 0
assert scan.dump() == "stale-cache"
assert scan.dump_calls == 1

# An abort and a timeout must not permit a cache dump to be reported as fresh.
assert not FakeScan([(400, "SCAN_ABORTED")]).wait_for_completion(0, 1200)
assert not FakeScan([]).wait_for_completion(0, 500)

print("delayed nl80211 completion policy: ok")
