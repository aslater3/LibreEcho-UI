#ifndef LIBREECHO_LIVE_TOOLS_H
#define LIBREECHO_LIVE_TOOLS_H

/*
 * Allow-listed local actions for GPT-Live delegation.
 *
 * The model never talks to a daemon directly and never gets a general-purpose
 * execution path.  Every delegation is matched against a fixed table of names;
 * anything else fails closed with a concise result the model can speak.  A
 * delegated request is a *request*, not a command: arguments are validated
 * here before they reach a sibling daemon, so a prompt-injected "seconds" of
 * 10^18 or a url pointing at a local file never becomes an adapter call.
 *
 * Each handler reuses the daemon that already owns the capability rather than
 * reimplementing it.  lived owns no timer store, no radio player and no mixer;
 * it is a client of the same sockets agentd uses, so behaviour stays identical
 * between the local assistant and a Live conversation.
 */

#include "live_transport.h"

#include <stddef.h>

#define LE_LIVE_TOOL_RESULT_MAX 512

struct le_live_tool_environment {
    char timer_socket[128];
    char radio_socket[128];
    char audio_socket[128];
    char media_status_path[160];
    char time_status_path[160];
    /* Upper bound for any single sibling-daemon call. */
    int timeout_ms;
};

void le_live_tools_init(struct le_live_tool_environment *environment);

/* 1 when the tool is on the allow-list, 0 otherwise. */
int le_live_tools_supported(const char *tool);

/*
 * Run one delegated tool.  Writes a bounded JSON result object into `result`
 * that states plainly whether the action happened, and returns 0 on success,
 * -1 when the tool is unknown, refused or failed.  A failure still fills
 * `result` with a speakable sentence, because a refused tool must not end the
 * conversation.
 */
int le_live_tools_dispatch(const struct le_live_tool_environment *environment,
                           const char *tool, const char *arguments,
                           char *result, size_t size);

/* Names, in the order they are published to a client.  Bounded table. */
const char *le_live_tools_name(unsigned int index);

#endif
