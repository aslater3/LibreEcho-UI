#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define LE_BACKEND_LINUX_TESTING
#define main backend_linux_test_main
#include "../src/backend_linux.c"
#undef main

#include <stdio.h>

static char captured_command[64];
static char captured_args[1024];
static int captured_timeout_ms;

int le_backend_linux_test_adapter_json_command(const char *socket_path,
                                               const char *command,
                                               const char *args);

int le_backend_linux_test_adapter_json_command_timeout(const char *socket_path,
                                                       const char *command,
                                                       const char *args,
                                                       int timeout_ms)
{
    captured_timeout_ms = timeout_ms;
    return le_backend_linux_test_adapter_json_command(socket_path, command, args);
}

int le_backend_linux_test_adapter_json_command(const char *socket_path,
                                               const char *command,
                                               const char *args)
{
    (void)socket_path;
    snprintf(captured_command, sizeof(captured_command), "%s", command);
    snprintf(captured_args, sizeof(captured_args), "%s", args ? args : "{}");
    return LE_OK;
}

static void require_contains(const char *text, const char *needle)
{
    if (!strstr(text, needle)) {
        fprintf(stderr, "Wi-Fi emission test: missing %s in %s\n", needle, text);
        _exit(1);
    }
}

int main(void)
{
    struct le_wifi_credentials credentials;

    memset(&credentials, 0, sizeof(credentials));
    snprintf(credentials.ssid, sizeof(credentials.ssid), "Open WiFi");
    snprintf(credentials.security, sizeof(credentials.security), "open");
    if (connect_wifi(NULL, &credentials) != LE_OK)
        return 1;
    require_contains(captured_command, "connect");
    require_contains(captured_args, "\"security\":\"open\"");
    if (captured_timeout_ms != 120000) {
        fprintf(stderr, "Wi-Fi connect timeout was %d ms, expected 120000 ms\n",
                captured_timeout_ms);
        return 1;
    }

    memset(&credentials, 0, sizeof(credentials));
    snprintf(credentials.ssid, sizeof(credentials.ssid), "Unsupported WPA3 WiFi");
    snprintf(credentials.password, sizeof(credentials.password), "secret-pass");
    snprintf(credentials.security, sizeof(credentials.security), "wpa3");
    if (connect_wifi(NULL, &credentials) != LE_INVALID)
        return 1;

    puts("Linux Wi-Fi adapter emission: open and WPA3 rejected PASS");
    return 0;
}
