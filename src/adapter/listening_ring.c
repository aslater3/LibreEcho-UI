#include "listening_ring.h"

#include "adapter.h"
#include "../json.h"

#include <stdio.h>

/* The beam snapshot is only useful for a cue if the field is directional
   enough that one beam stands out from the average. */
#define LISTENING_RING_MIN_CONTRAST_DB 2

/* The beam snapshot is a shared-memory read in micd's main process, so a
   healthy daemon answers at once.  Keep the timeout short: this runs on the
   wake path, and a stalled micd must not delay publishing the wake event or
   opening capture. */
#define LISTENING_RING_MIC_TIMEOUT_MS 40
#define LISTENING_RING_LED_TIMEOUT_MS 100

/* Fallback colours when ledd does not have a Listening theme colour.  The
   pattern carries profile "listening", so a configured theme wins in ledd. */
#define LISTENING_RING_FALLBACK_R 72
#define LISTENING_RING_FALLBACK_G 185
#define LISTENING_RING_FALLBACK_B 255

/*
 * Read micd's selected beam.  micd publishes the beam snapshot while waked
 * keeps the beamformed stream open; a wake without that stream simply gives
 * no azimuth and the caller shows the solid listening ring.
 */
static int read_direction(int *azimuth)
{
    struct le_adapter *mic;
    char beams[LE_ADAPTER_MSG_MAX];
    int running;
    int selected;
    int contrast;
    int result;

    if (!azimuth)
        return -1;
    mic = le_adapter_connect(LE_ADAPTER_MIC_SOCK,
                             LISTENING_RING_MIC_TIMEOUT_MS);
    if (!mic)
        return -1;
    le_adapter_set_io_timeout(mic, LISTENING_RING_MIC_TIMEOUT_MS);
    result = le_adapter_call(mic, "beams", "{}", beams, sizeof(beams));
    le_adapter_close(mic);
    if (result != LE_ADAPTER_OK)
        return -1;
    if (json_get_bool(beams, "running", &running) != 1 || !running)
        return -1;
    if (json_get_int(beams, "contrast_db", &contrast) != 1 ||
        contrast < LISTENING_RING_MIN_CONTRAST_DB)
        return -1;
    if (json_get_int(beams, "selected_deg", &selected) != 1 ||
        selected < 0 || selected >= 360)
        return -1;
    *azimuth = selected;
    return 0;
}

int le_listening_ring_show(const char *led_socket, const char *owner)
{
    struct le_adapter *led;
    char args[256];
    int azimuth;
    int has_direction;
    int result;
    int length;

    if (!led_socket || !led_socket[0] || !owner || owner[0] == '\0')
        return LE_ADAPTER_ERR_PROTO;
    has_direction = read_direction(&azimuth) == 0;
    if (has_direction) {
        length = snprintf(
            args, sizeof(args),
            "{\"name\":\"direction\",\"azimuth\":%d,"
            "\"r\":%d,\"g\":%d,\"b\":%d,\"brightness\":100,"
            "\"repeats\":0,\"profile\":\"listening\",\"owner\":\"%s\"}",
            azimuth, LISTENING_RING_FALLBACK_R,
            LISTENING_RING_FALLBACK_G, LISTENING_RING_FALLBACK_B, owner);
    } else {
        length = snprintf(
            args, sizeof(args),
            "{\"name\":\"direction\","
            "\"r\":%d,\"g\":%d,\"b\":%d,\"brightness\":100,"
            "\"repeats\":0,\"profile\":\"listening\",\"owner\":\"%s\"}",
            LISTENING_RING_FALLBACK_R,
            LISTENING_RING_FALLBACK_G, LISTENING_RING_FALLBACK_B, owner);
    }
    if (length < 0 || (size_t)length >= sizeof(args))
        return LE_ADAPTER_ERR_PROTO;

    led = le_adapter_connect(led_socket,
                             LISTENING_RING_LED_TIMEOUT_MS);
    if (!led)
        return LE_ADAPTER_ERR_CONNECT;
    le_adapter_set_io_timeout(led, LISTENING_RING_LED_TIMEOUT_MS);
    result = le_adapter_call(led, "pattern", args, NULL, 0);
    le_adapter_close(led);
    return result;
}
