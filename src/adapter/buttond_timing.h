#ifndef LE_BUTTOND_TIMING_H
#define LE_BUTTOND_TIMING_H

long long buttond_poll_timeout_ms(long long now_ms, long long next_status_ms,
                                  long long next_rescan_ms,
                                  long long next_repeat_ms, int device_count,
                                  int held_key);
int buttond_repeat_due(long long now_ms, long long next_repeat_ms,
                       int held_key);

#endif
