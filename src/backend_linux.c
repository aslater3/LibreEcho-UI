#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "backend_internal.h"
#include "adapter/adapter.h"
#include "json.h"
#include "log.h"
#include "version.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/reboot.h>
#include <stdint.h>
#include <ctype.h>
#include <limits.h>
#include <time.h>
#include <sys/utsname.h>

struct linux_state {
    char config[384];
    char temp_path[256];
    unsigned long long cpu_prev_total[LE_MAX_CPUS];
    unsigned long long cpu_prev_busy[LE_MAX_CPUS];
    unsigned char cpu_prev_valid[LE_MAX_CPUS];
};

static struct linux_state *state(struct le_backend *b)
{
    return (struct linux_state *)b->data;
}

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    size_t n;

    if (!dst_size)
        return;
    if (!src)
        src = "";
    n = strlen(src);
    if (n >= dst_size)
        n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void read_hostname(char *out, size_t out_size)
{
    if (!out_size)
        return;
    out[0] = '\0';
    if (gethostname(out, out_size - 1) != 0 ||
        !out[0] || !strcmp(out, "(none)"))
        copy_string(out, out_size, "libreecho");
}

static int read_line(const char *path, char *out, size_t out_size)
{
    FILE *f;
    size_t n;

    if (!out || out_size < 2)
        return -1;
    out[0] = '\0';
    f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(out, (int)out_size, f)) {
        fclose(f);
        out[0] = '\0';
        return -1;
    }
    fclose(f);
    n = strcspn(out, "\r\n");
    out[n] = '\0';
    return 0;
}

static int read_device_serial(char *out, size_t out_size)
{
    const char *idme_path = getenv("LIBREECHO_IDME_SERIAL_PATH");
    size_t i;

    if (!idme_path || !idme_path[0])
        idme_path = "/proc/idme/serial";
    if (read_line(idme_path, out, out_size) != 0 || !out[0])
        return -1;
    for (i = 0; out[i]; ++i)
        if (!isalnum((unsigned char)out[i])) {
            out[0] = '\0';
            return -1;
        }
    return i >= 4 ? 0 : -1;
}

static int read_redacted_boot_id(char *out, size_t out_size)
{
    const char *cmdline_path = getenv("LIBREECHO_CMDLINE_PATH");
    char cmdline[4096], serial[128];
    const char *key = "androidboot.serialno=";
    const char *start;
    size_t i;
    unsigned long long hash = 1469598103934665603ULL;

    if (!cmdline_path || !cmdline_path[0])
        cmdline_path = "/proc/cmdline";
    if (read_line(cmdline_path, cmdline, sizeof(cmdline)) != 0)
        return -1;
    start = strstr(cmdline, key);
    if (!start)
        return -1;
    start += strlen(key);
    for (i = 0; start[i] && start[i] != ' ' && i + 1 < sizeof(serial); ++i) {
        if (!isalnum((unsigned char)start[i]))
            return -1;
        serial[i] = start[i];
    }
    if (i < 4 || start[i] != ' ' && start[i] != '\0')
        return -1;
    serial[i] = '\0';
    for (i = 0; serial[i]; ++i) {
        hash ^= (unsigned char)serial[i];
        hash *= 1099511628211ULL;
    }
    if (snprintf(out, out_size, "device-%016llx", hash) >= (int)out_size)
        return -1;
    return 0;
}

static void read_cpu_status(struct le_backend *b, struct le_system_status *o)
{
    FILE *f;
    char line[256];
    long configured;
    size_t count, i;

    configured = sysconf(_SC_NPROCESSORS_CONF);
    if (configured < 1)
        configured = 1;
    count = (size_t)configured;
    if (count > LE_MAX_CPUS)
        count = LE_MAX_CPUS;
    o->cpu_count = count;

    for (i = 0; i < count; ++i) {
        char path[PATH_MAX], online[8], frequency[32];
        o->cpus[i].online = i == 0;
        frequency[0] = '\0';
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%zu/online", i);
        if (!read_line(path, online, sizeof(online)))
            o->cpus[i].online = atoi(online) != 0;
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%zu/cpufreq/scaling_cur_freq", i);
        if (read_line(path, frequency, sizeof(frequency))) {
            snprintf(path, sizeof(path),
                     "/sys/devices/system/cpu/cpu%zu/cpufreq/cpuinfo_cur_freq", i);
            (void)read_line(path, frequency, sizeof(frequency));
        }
        o->cpus[i].frequency_khz = atoi(frequency);
    }

    f = fopen("/proc/stat", "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        unsigned int cpu;
        unsigned long long user, nice, system, idle, iowait;
        unsigned long long irq, softirq, steal;
        unsigned long long busy, total;
        unsigned long long delta_busy, delta_total;

        if (sscanf(line, "cpu%u %llu %llu %llu %llu %llu %llu %llu %llu",
                   &cpu, &user, &nice, &system, &idle, &iowait, &irq,
                   &softirq, &steal) != 9 || cpu >= count)
            continue;
        busy = user + nice + system + irq + softirq + steal;
        total = busy + idle + iowait;
        if (state(b)->cpu_prev_valid[cpu] &&
            total >= state(b)->cpu_prev_total[cpu] &&
            busy >= state(b)->cpu_prev_busy[cpu]) {
            delta_total = total - state(b)->cpu_prev_total[cpu];
            delta_busy = busy - state(b)->cpu_prev_busy[cpu];
            if (delta_total)
                o->cpus[cpu].utilization = (int)((delta_busy * 100) /
                                                 delta_total);
            if (o->cpus[cpu].utilization > 100)
                o->cpus[cpu].utilization = 100;
        }
        state(b)->cpu_prev_total[cpu] = total;
        state(b)->cpu_prev_busy[cpu] = busy;
        state(b)->cpu_prev_valid[cpu] = 1;
    }
    fclose(f);

    {
        int total_utilization = 0;
        int online_count = 0;
        for (i = 0; i < count; ++i) {
            if (!o->cpus[i].online)
                continue;
            total_utilization += o->cpus[i].utilization;
            ++online_count;
        }
        if (online_count)
            o->cpu = total_utilization / online_count;
    }
}

/* Return the first balanced JSON object associated with key. */
static int json_object(const char *json, const char *key,
                       char *out, size_t out_size)
{
    char needle[96];
    const char *p;
    const char *start;
    size_t i, n;
    int depth = 0;
    int string = 0;
    int escaped = 0;

    if (!json || !key || !out || !out_size)
        return 0;
    if (snprintf(needle, sizeof(needle), "\"%s\"", key) >=
        (int)sizeof(needle))
        return 0;
    p = strstr(json, needle);
    if (!p)
        return 0;
    p = strchr(p + strlen(needle), '{');
    if (!p)
        return 0;
    start = p;
    for (i = 0; p[i]; ++i) {
        char c = p[i];
        if (string) {
            if (escaped)
                escaped = 0;
            else if (c == '\\')
                escaped = 1;
            else if (c == '"')
                string = 0;
            continue;
        }
        if (c == '"') {
            string = 1;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}' && depth > 0) {
            --depth;
            if (!depth) {
                n = (size_t)(p + i - start + 1);
                if (n >= out_size)
                    return 0;
                memcpy(out, start, n);
                out[n] = '\0';
                return 1;
            }
        }
    }
    return 0;
}

static int json_container(const char *start, char open, char close,
                          char *out, size_t out_size)
{
    size_t i, n;
    int depth = 0;
    int string = 0;
    int escaped = 0;

    if (!start || *start != open || !out || !out_size)
        return 0;
    for (i = 0; start[i]; ++i) {
        char c = start[i];
        if (string) {
            if (escaped)
                escaped = 0;
            else if (c == '\\')
                escaped = 1;
            else if (c == '"')
                string = 0;
            continue;
        }
        if (c == '"') {
            string = 1;
        } else if (c == open) {
            ++depth;
        } else if (c == close && depth > 0) {
            --depth;
            if (!depth) {
                n = i + 1;
                if (n >= out_size)
                    return 0;
                memcpy(out, start, n);
                out[n] = '\0';
                return 1;
            }
        }
    }
    return 0;
}

static int adapter_result(int rc)
{
    if (rc == LE_ADAPTER_OK)
        return LE_OK;
    if (rc == LE_ADAPTER_ERR_CONNECT)
        return LE_NOT_SUPPORTED;
    return LE_IO;
}

/* A missing companion daemon is a normal condition during early bring-up. */
static int adapter_command(const char *socket_path, const char *command,
                           const char *args, char *response, size_t response_size)
{
    struct le_adapter *adapter;
    int rc;
    int result;

    if (response && response_size)
        response[0] = '\0';
    adapter = le_adapter_connect(socket_path, 100);
    if (!adapter) {
        le_log_debug("backend: adapter %s unavailable (daemon not running?)", command);
        return LE_NOT_SUPPORTED;
    }
    rc = le_adapter_call(adapter, command, args, response, response_size);
    le_adapter_close(adapter);
    result = adapter_result(rc);
    if (result != LE_OK)
        le_log_debug("backend: adapter %s failed (rc=%d)", command, rc);
    return result;
}

static int adapter_json_command(const char *socket_path, const char *command,
                                const char *args)
{
    return adapter_command(socket_path, command, args, NULL, 0);
}

static int read_wireless_signal(const char *iface)
{
    FILE *f;
    char line[256];
    char name[IFNAMSIZ];
    unsigned int status_word;
    double level;
    int signal;

    f = fopen("/proc/net/wireless", "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        memset(name, 0, sizeof(name));
        if (sscanf(line, " %15[^:]: %x %lf", name, &status_word, &level) != 3)
            continue;
        if (strcmp(name, iface))
            continue;
        (void)status_word;
        if (level <= 0.0)
            signal = (int)((level + 100.0) * 2.0);
        else
            signal = (int)((level * 100.0) / 70.0);
        if (signal < 0)
            signal = 0;
        if (signal > 100)
            signal = 100;
        fclose(f);
        return signal;
    }
    fclose(f);
    return 0;
}

static int read_gateway(const char *iface, char *out, size_t out_size)
{
    FILE *f;
    char line[256], name[IFNAMSIZ];
    unsigned long destination, gateway;
    unsigned int flags;
    struct in_addr addr;

    f = fopen("/proc/net/route", "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, " %15s %lx %lx %x", name, &destination,
                   &gateway, &flags) != 4)
            continue;
        if (destination != 0 || !(flags & 0x2) ||
            (iface && iface[0] && strcmp(iface, name)))
            continue;
        /* /proc/net/route prints IPv4 words in host byte order. */
        addr.s_addr = (uint32_t)gateway;
        if (!inet_ntop(AF_INET, &addr, out, out_size)) {
            fclose(f);
            return -1;
        }
        fclose(f);
        return 0;
    }
    fclose(f);
    return -1;
}

static int read_ip_address(const char *iface, char *out, size_t out_size)
{
    struct ifaddrs *all = NULL;
    struct ifaddrs *it;
    char fallback[INET6_ADDRSTRLEN];

    out[0] = '\0';
    fallback[0] = '\0';
    if (getifaddrs(&all) < 0)
        return -1;
    for (it = all; it; it = it->ifa_next) {
        void *address;
        int family;
        if (!it->ifa_addr || !iface || strcmp(it->ifa_name, iface))
            continue;
        family = it->ifa_addr->sa_family;
        if (family == AF_INET) {
            address = &((struct sockaddr_in *)it->ifa_addr)->sin_addr;
            if (inet_ntop(AF_INET, address, out, out_size)) {
                freeifaddrs(all);
                return 0;
            }
        } else if (family == AF_INET6 && !fallback[0]) {
            address = &((struct sockaddr_in6 *)it->ifa_addr)->sin6_addr;
            if (!inet_ntop(AF_INET6, address, fallback, sizeof(fallback)))
                fallback[0] = '\0';
        }
    }
    if (fallback[0])
        copy_string(out, out_size, fallback);
    freeifaddrs(all);
    return out[0] ? 0 : -1;
}

static void read_dns(char *out, size_t out_size)
{
    FILE *f;
    char line[256], server[128];
    size_t used = 0;

    out[0] = '\0';
    f = fopen("/etc/resolv.conf", "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, " nameserver %127s", server) != 1)
            continue;
        server[strcspn(server, "\r\n")] = '\0';
        if (!server[0])
            continue;
        if (used && used + 1 < out_size)
            out[used++] = ',';
        if (used >= out_size)
            break;
        if (strlen(server) >= out_size - used)
            break;
        strcpy(out + used, server);
        used += strlen(server);
    }
    fclose(f);
}

static int read_temperature(void)
{
    char path[PATH_MAX], type_path[PATH_MAX], raw[32], type[64];
    int fallback = 0;
    unsigned int zone;

    for (zone = 0; zone < 16; ++zone) {
        long value;
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%u/temp", zone);
        if (read_line(path, raw, sizeof(raw)))
            continue;
        value = strtol(raw, NULL, 10);
        if (value > 1000)
            value /= 1000;
        if (value < -40 || value > 150)
            continue;
        if (!fallback)
            fallback = (int)value;
        snprintf(type_path, sizeof(type_path),
                 "/sys/class/thermal/thermal_zone%u/type", zone);
        type[0] = '\0';
        (void)read_line(type_path, type, sizeof(type));
        if (value != 0 && (strstr(type, "cpu") || strstr(type, "soc") ||
                           strstr(type, "mtk") || strstr(type, "thermal")))
            return (int)value;
    }
    return fallback;
}

static int read_block_capacity_mb(int *megabytes)
{
    FILE *f;
    char line[128];
    unsigned major, minor;
    unsigned long long blocks;
    char name[64];

    if (!megabytes)
        return -1;
    *megabytes = 0;
    f = fopen("/proc/partitions", "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%u %u %llu %63s", &major, &minor, &blocks,
                   name) != 4)
            continue;
        if (!strcmp(name, "mmcblk0") && blocks) {
            *megabytes = (int)((blocks * 1024ULL) / 1048576ULL);
            fclose(f);
            return *megabytes > 0 ? 0 : -1;
        }
    }
    fclose(f);
    return -1;
}

/* status() reads the Linux procfs/sysfs measurements directly. */
static int status(struct le_backend *b, struct le_system_status *o)
{
    FILE *f;
    double up = 0, l1 = 0;
    unsigned long total = 0, avail = 0;
    char key[64], unit[16];
    unsigned long value;
    struct statvfs sv;
    int storage_rc;

    memset(o, 0, sizeof(*o));
    strcpy(o->device_state, "online");
    o->storage = -1;
    o->storage_used_mb = -1;
    o->storage_total_mb = -1;
    strcpy(o->storage_state, "unavailable");
    f = fopen("/proc/uptime", "r");
    if (f) {
        if (fscanf(f, "%lf", &up) == 1)
            o->uptime = up;
        fclose(f);
    }
    f = fopen("/proc/loadavg", "r");
    if (f) {
        if (fscanf(f, "%lf", &l1) == 1)
            o->cpu = (int)(l1 * 25.0);
        fclose(f);
    }
    f = fopen("/proc/meminfo", "r");
    if (f) {
        while (fscanf(f, "%63s %lu %15s", key, &value, unit) == 3) {
            if (!strcmp(key, "MemTotal:"))
                total = value;
            if (!strcmp(key, "MemAvailable:"))
                avail = value;
        }
        fclose(f);
    }
    if (total) {
        o->memory_total_mb = (int)(total / 1024);
        o->memory_used_mb = (int)((total - avail) / 1024);
        o->memory = (int)((total - avail) * 100 / total);
    }
    storage_rc = statvfs("/data", &sv);
    if (storage_rc || !sv.f_blocks || !sv.f_frsize)
        storage_rc = statvfs("/", &sv);
    if (storage_rc || !sv.f_blocks || !sv.f_frsize)
        memset(&sv, 0, sizeof(sv));
    if (sv.f_blocks && sv.f_frsize) {
        unsigned long long t = (unsigned long long)sv.f_blocks * sv.f_frsize;
        unsigned long long u = (unsigned long long)(sv.f_blocks - sv.f_bavail) * sv.f_frsize;
        o->storage_total_mb = (int)(t / 1048576);
        o->storage_used_mb = (int)(u / 1048576);
        o->storage = (int)(u * 100 / t);
        o->storage_available = 1;
        strcpy(o->storage_state, "filesystem");
    } else if (!read_block_capacity_mb(&o->storage_total_mb)) {
        strcpy(o->storage_state, "block-device-unmounted");
    }
    (void)f;
    o->temperature = read_temperature();
    read_cpu_status(b, o);
    return LE_OK;
}

/* device() reports the kernel, host, and immutable IDME board identity. */
static int device(struct le_backend *b, struct le_device_info *o)
{
    struct utsname u;

    (void)b;
    memset(o, 0, sizeof(*o));
    read_hostname(o->hostname, sizeof(o->hostname));
    copy_string(o->name, sizeof(o->name), "LibreEcho");
    strcpy(o->model, "LibreEcho device");
    if (read_device_serial(o->serial, sizeof(o->serial)) != 0 &&
        read_redacted_boot_id(o->serial, sizeof(o->serial)) != 0)
        strcpy(o->serial, "unavailable");
    strcpy(o->os_version, LE_OS_VERSION_STRING);
    if (!uname(&u))
        copy_string(o->kernel, sizeof(o->kernel), u.release);
    strcpy(o->hardware_revision, "adapter pending");
    strcpy(o->backend, "linux");
    return LE_OK;
}

static int networkd_status(struct le_backend *b, struct le_network_state *o)
{
    char response[LE_ADAPTER_MSG_MAX];
    int found = 0;
    int rc;

    (void)b;
    rc = adapter_command(LE_ADAPTER_NETWORK_SOCK, "status", NULL,
                         response, sizeof(response));
    if (rc != LE_OK)
        return rc;

    memset(o, 0, sizeof(*o));
    o->rssi_dbm = -1;
    read_hostname(o->hostname, sizeof(o->hostname));
    if (json_get_string(response, "state", o->state, sizeof(o->state)) > 0)
        found = 1;
    if (json_get_string(response, "ssid", o->ssid, sizeof(o->ssid)) > 0)
        found = 1;
    (void)json_get_string(response, "ip", o->ip, sizeof(o->ip));
    (void)json_get_string(response, "gateway", o->gateway, sizeof(o->gateway));
    (void)json_get_string(response, "dns", o->dns, sizeof(o->dns));
    (void)json_get_int(response, "signal", &o->signal);
    (void)json_get_int(response, "rssi_dbm", &o->rssi_dbm);
    o->dhcp = o->ip[0] && (!strcmp(o->state, "connected") || o->gateway[0]);
    o->internet = o->gateway[0] && !strcmp(o->state, "connected");
    return found ? LE_OK : LE_IO;
}

static int network(struct le_backend *b, struct le_network_state *o)
{
    char path[PATH_MAX];
    char operstate[32];
    char mac[32];
    char iface[IFNAMSIZ];
    int have_iface = 0;

    if (networkd_status(b, o) == LE_OK)
        return LE_OK;

    memset(o, 0, sizeof(*o));
    o->rssi_dbm = -1;
    read_hostname(o->hostname, sizeof(o->hostname));

    snprintf(path, sizeof(path), "/sys/class/net/wlan0/operstate");
    if (!read_line(path, operstate, sizeof(operstate))) {
        copy_string(iface, sizeof(iface), "wlan0");
        have_iface = 1;
    } else {
        snprintf(path, sizeof(path), "/sys/class/net/usb0/operstate");
        if (!read_line(path, operstate, sizeof(operstate))) {
            copy_string(iface, sizeof(iface), "usb0");
            have_iface = 1;
        }
    }

    if (!have_iface) {
        strcpy(o->state, "disconnected");
        read_dns(o->dns, sizeof(o->dns));
        return LE_OK;
    }
    if (!strcmp(operstate, "up"))
        strcpy(o->state, "connected");
    else if (!strcmp(operstate, "down"))
        strcpy(o->state, "disconnected");
    else
        copy_string(o->state, sizeof(o->state), operstate);

    /* MAC is read here for the hardware adapter boundary; the public API has no MAC field. */
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", iface);
    (void)read_line(path, mac, sizeof(mac));
    o->signal = read_wireless_signal(iface);
    (void)read_gateway(iface, o->gateway, sizeof(o->gateway));
    (void)read_ip_address(iface, o->ip, sizeof(o->ip));
    read_dns(o->dns, sizeof(o->dns));
    o->dhcp = o->ip[0] && (!strcmp(o->state, "connected") || o->gateway[0]);
    o->internet = o->gateway[0] && !strcmp(o->state, "connected");
    return LE_OK;
}

static int audio(struct le_backend *b, struct le_audio_state *o)
{
    char response[LE_ADAPTER_MSG_MAX];
    int v, found = 0;
    (void)b;
    int rc = adapter_command(LE_ADAPTER_AUDIO_SOCK, "status", NULL,
                              response, sizeof(response));
    if (rc != LE_OK)
        return rc;
    memset(o, 0, sizeof(*o));
    if (json_get_int(response, "volume", &v) > 0) { o->volume = v; found = 1; }
    if (json_get_int(response, "microphone_gain", &v) > 0) { o->microphone_gain = v; found = 1; }
    if (json_get_int(response, "notification_volume", &v) > 0) { o->notification_volume = v; found = 1; }
    if (json_get_bool(response, "muted", &v) > 0) { o->muted = v; found = 1; }
    else if (json_get_bool(response, "microphone_muted", &v) > 0) { o->muted = v; found = 1; }
    if (json_get_bool(response, "startup_sound", &v) > 0) { o->startup_sound = v; found = 1; }
    if (json_get_bool(response, "amplifier_on", &v) > 0) { o->amplifier_on = v; found = 1; }
    if (json_get_bool(response, "output_available", &v) > 0) { o->output_available = v; found = 1; }
    {
        const char *path = getenv("LE_TTS_VOICE_FILE");
        FILE *voice_file;
        if (!path || !path[0])
            path = "/data/libreecho/config/tts-voice";
        strcpy(o->tts_voice, "southern-female");
        voice_file = fopen(path, "r");
        if (voice_file) {
            char selected[32];
            if (fgets(selected, sizeof(selected), voice_file)) {
                selected[strcspn(selected, "\r\n")] = '\0';
                if (!strcmp(selected, "northern-male") ||
                    !strcmp(selected, "southern-female"))
                    strcpy(o->tts_voice, selected);
            }
            fclose(voice_file);
        }
    }
    return found ? LE_OK : LE_IO;
}

static int volume(struct le_backend *b, int value)
{
    char args[64];
    (void)b;
    if (value < 0 || value > 100) return LE_INVALID;
    snprintf(args, sizeof(args), "{\"volume\":%d}", value);
    return adapter_json_command(LE_ADAPTER_AUDIO_SOCK, "set_volume", args);
}

static int gain(struct le_backend *b, int value)
{
    char args[64];
    (void)b;
    if (value < 0 || value > 100) return LE_INVALID;
    snprintf(args, sizeof(args), "{\"gain\":%d}", value);
    return adapter_json_command(LE_ADAPTER_AUDIO_SOCK, "set_gain", args);
}

static int mute(struct le_backend *b, int muted)
{
    char args[64];
    (void)b;
    snprintf(args, sizeof(args), "{\"muted\":%s}", muted ? "true" : "false");
    return adapter_json_command(LE_ADAPTER_AUDIO_SOCK, "set_mute", args);
}

static int tone(struct le_backend *b)
{
    (void)b;
    return adapter_json_command(LE_ADAPTER_AUDIO_SOCK, "test_tone", NULL);
}

static int tts_voice(struct le_backend *b, const char *voice)
{
    const char *script = getenv("LE_TTS_INIT_SCRIPT");
    pid_t pid;
    int status;
    (void)b;
    if (!voice || (strcmp(voice, "northern-male") &&
                   strcmp(voice, "southern-female")))
        return LE_INVALID;
    if (!script || !script[0])
        script = "/etc/init.d/libreecho-ttsd.init";
    if (access(script, X_OK) != 0)
        return LE_NOT_SUPPORTED;
    pid = fork();
    if (pid < 0)
        return LE_IO;
    if (pid == 0) {
        execl(script, script, "switch", voice, (char *)NULL);
        _exit(127);
    }
    do {
        if (waitpid(pid, &status, 0) >= 0)
            break;
    } while (errno == EINTR);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? LE_OK : LE_IO;
}

static int announce(struct le_backend *b, const char *text)
{
    char args[1200];
    char escaped[1024];
    (void)b;
    if (!text || !text[0])
        return LE_INVALID;
    json_escape(escaped, sizeof(escaped), text);
    if (snprintf(args, sizeof(args), "{\"text\":\"%s\"}", escaped) >=
        (int)sizeof(args))
        return LE_INVALID;
    return adapter_json_command(LE_ADAPTER_AUDIO_SOCK, "speak", args);
}

static int stop_speech(struct le_backend *b)
{
    (void)b;
    return adapter_json_command(LE_ADAPTER_AUDIO_SOCK, "stop_speech", NULL);
}

static void parse_profile(const char *json, struct le_led_profile *profile)
{
    int v;
    if (json_get_int(json, "r", &v) > 0) profile->r = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    if (json_get_int(json, "g", &v) > 0) profile->g = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    if (json_get_int(json, "b", &v) > 0) profile->b = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    if (json_get_int(json, "brightness", &v) > 0) profile->brightness = v;
    if (json_get_int(json, "animation_speed", &v) > 0) profile->animation_speed = v;
}

static void parse_led_levels(const char *json,
                             uint8_t levels[LE_LED_PIXELS])
{
    const char *p = strstr(json, "\"visualizer_levels\"");
    size_t i;

    if (!p || !(p = strchr(p, '[')))
        return;
    p++;
    for (i = 0; i < LE_LED_PIXELS; i++) {
        char *end;
        unsigned long value;
        while (isspace((unsigned char)*p))
            p++;
        errno = 0;
        value = strtoul(p, &end, 10);
        if (errno || end == p || value > 255)
            return;
        levels[i] = (uint8_t)value;
        p = end;
        while (isspace((unsigned char)*p))
            p++;
        if (i + 1 < LE_LED_PIXELS) {
            if (*p++ != ',')
                return;
        } else if (*p != ']') {
            return;
        }
    }
}

static int parse_led_pixels(const char *json,
                            struct le_led_pixel pixels[LE_LED_PIXELS])
{
    const char *p = strstr(json, "\"pixels\"");
    size_t i;

    if (!p || !(p = strchr(p, '[')))
        return 0;
    p++;
    for (i = 0; i < LE_LED_PIXELS; i++) {
        const char *end;
        char object[128];
        size_t length;
        int r, g, b;

        while (isspace((unsigned char)*p) || *p == ',')
            p++;
        if (*p != '{' || !(end = strchr(p, '}')))
            return 0;
        length = (size_t)(end - p + 1);
        if (length >= sizeof(object))
            return 0;
        memcpy(object, p, length);
        object[length] = '\0';
        if (json_get_int(object, "r", &r) < 1 ||
            json_get_int(object, "g", &g) < 1 ||
            json_get_int(object, "b", &b) < 1 ||
            r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255)
            return 0;
        pixels[i].r = (uint8_t)r;
        pixels[i].g = (uint8_t)g;
        pixels[i].b = (uint8_t)b;
        p = end + 1;
    }
    return 1;
}

static int led(struct le_backend *b, struct le_led_state *o)
{
    char response[LE_ADAPTER_MSG_MAX], object[LE_ADAPTER_MSG_MAX];
    char profiles[LE_ADAPTER_MSG_MAX];
    (void)b;
    int rc = adapter_command(LE_ADAPTER_LED_SOCK, "status", NULL,
                              response, sizeof(response));
    if (rc != LE_OK)
        return rc;
    memset(o, 0, sizeof(*o));
    if (json_get_bool(response, "animation_active", &o->animation_active) < 0)
        o->animation_active = 0;
    if (json_get_bool(response, "pattern_active", &o->pattern_active) < 0)
        o->pattern_active = 0;
    if (json_get_bool(response, "visualizer_active",
                      &o->visualizer_active) < 0)
        o->visualizer_active = 0;
    if (json_get_bool(response, "visualizer_enabled",
                      &o->visualizer_enabled) < 0)
        o->visualizer_enabled = 1;
    (void)json_get_string(response, "pattern", o->pattern, sizeof(o->pattern));
    (void)json_get_string(response, "visualizer_owner",
                          o->visualizer_owner, sizeof(o->visualizer_owner));
    (void)json_get_string(response, "visualizer_mood",
                          o->visualizer_mood, sizeof(o->visualizer_mood));
    parse_led_levels(response, o->visualizer_levels);
    o->animation_profile = -1;
    if (json_get_string(response, "animation_profile", object, sizeof(object)) > 0) {
        if (!strcmp(object, "listening")) o->animation_profile = 0;
        else if (!strcmp(object, "thinking")) o->animation_profile = 1;
        else if (!strcmp(object, "error")) o->animation_profile = 2;
        else if (!strcmp(object, "dnd")) o->animation_profile = 3;
    }
    if (json_object(response, "current", object, sizeof(object)) ||
        json_object(response, "colour", object, sizeof(object)))
        parse_profile(object, &o->current);
    else
        parse_profile(response, &o->current);
    if (json_object(response, "boot", object, sizeof(object)) ||
        json_object(response, "boot_profile", object, sizeof(object)))
        parse_profile(object, &o->boot);
    if (json_object(response, "profiles", profiles, sizeof(profiles))) {
        if (json_object(profiles, "listening", object, sizeof(object))) parse_profile(object, &o->listening);
        if (json_object(profiles, "thinking", object, sizeof(object))) parse_profile(object, &o->thinking);
        if (json_object(profiles, "error", object, sizeof(object))) parse_profile(object, &o->error);
        if (json_object(profiles, "dnd", object, sizeof(object))) parse_profile(object, &o->dnd);
    }
    if (!parse_led_pixels(response, o->pixels)) {
        size_t i;
        for (i = 0; i < LE_LED_PIXELS; i++) {
            o->pixels[i].r = (uint8_t)((o->current.r *
                                      o->current.brightness + 50) / 100);
            o->pixels[i].g = (uint8_t)((o->current.g *
                                      o->current.brightness + 50) / 100);
            o->pixels[i].b = (uint8_t)((o->current.b *
                                      o->current.brightness + 50) / 100);
        }
    }
    return LE_OK;
}

static int colour(struct le_backend *b, uint8_t r, uint8_t g, uint8_t bl)
{
    char args[96];
    (void)b;
    snprintf(args, sizeof(args), "{\"r\":%u,\"g\":%u,\"b\":%u}", r, g, bl);
    return adapter_json_command(LE_ADAPTER_LED_SOCK, "set_colour", args);
}

static int brightness(struct le_backend *b, int value)
{
    char args[64];
    (void)b;
    if (value < 0 || value > 100) return LE_INVALID;
    snprintf(args, sizeof(args), "{\"brightness\":%d}", value);
    return adapter_json_command(LE_ADAPTER_LED_SOCK, "set_brightness", args);
}

static int visualizer_enabled(struct le_backend *b, int enabled)
{
    char args[64];
    (void)b;
    if (enabled != 0 && enabled != 1)
        return LE_INVALID;
    snprintf(args, sizeof(args), "{\"enabled\":%s}",
             enabled ? "true" : "false");
    return adapter_json_command(LE_ADAPTER_LED_SOCK,
                                "set_visualizer_enabled", args);
}

static int boot_led(struct le_backend *b, const struct le_led_profile *profile)
{
    char args[128];
    (void)b;
    if (!profile) return LE_INVALID;
    snprintf(args, sizeof(args),
             "{\"r\":%u,\"g\":%u,\"b\":%u,\"brightness\":%d,\"animation_speed\":%d}",
             profile->r, profile->g, profile->b, profile->brightness,
             profile->animation_speed);
    return adapter_json_command(LE_ADAPTER_LED_SOCK, "set_boot_profile", args);
}

static int led_profile(struct le_backend *b, const char *name,
                       const struct le_led_profile *profile)
{
    char args[192];
    (void)b;
    if (!name || !profile ||
        (strcmp(name, "listening") && strcmp(name, "thinking") &&
         strcmp(name, "error") && strcmp(name, "dnd")) ||
        profile->brightness < 0 || profile->brightness > 100)
        return LE_INVALID;
    snprintf(args, sizeof(args),
             "{\"name\":\"%s\",\"r\":%u,\"g\":%u,\"b\":%u,\"brightness\":%d}",
             name, profile->r, profile->g, profile->b, profile->brightness);
    return adapter_json_command(LE_ADAPTER_LED_SOCK, "set_profile", args);
}

static int led_test(struct le_backend *b)
{
    (void)b;
    return adapter_json_command(LE_ADAPTER_LED_SOCK, "test", NULL);
}

static int scan(struct le_backend *b, struct le_wifi_scan *o)
{
    char response[LE_ADAPTER_MSG_MAX], array[LE_ADAPTER_MSG_MAX];
    char object[512];
    const char *p;
    int rc;
    (void)b;

    rc = adapter_command(LE_ADAPTER_NETWORK_SOCK, "scan", NULL,
                         response, sizeof(response));
    if (rc != LE_OK)
        return rc;
    memset(o, 0, sizeof(*o));
    p = strstr(response, "\"networks\"");
    p = p ? strchr(p, '[') : NULL;
    if (!p || !json_container(p, '[', ']', array, sizeof(array)))
        return LE_IO;
    p = array + 1;
    while (o->count < LE_MAX_WIFI && (p = strchr(p, '{')) != NULL) {
        if (!json_container(p, '{', '}', object, sizeof(object)))
            break;
        if (json_get_string(object, "ssid", o->networks[o->count].ssid,
                            sizeof(o->networks[o->count].ssid)) > 0) {
            if (json_get_string(object, "security", o->networks[o->count].security,
                                sizeof(o->networks[o->count].security)) < 1)
                strcpy(o->networks[o->count].security, "unknown");
            if (json_get_int(object, "signal", &o->networks[o->count].signal) < 1)
                o->networks[o->count].signal = 0;
            if (o->networks[o->count].signal < 0) o->networks[o->count].signal = 0;
            if (o->networks[o->count].signal > 100) o->networks[o->count].signal = 100;
            ++o->count;
        }
        p++;
    }
    return LE_OK;
}

static int connect_wifi(struct le_backend *b, const struct le_wifi_credentials *credentials)
{
    char ssid[LE_TEXT * 2], psk[sizeof(credentials->password) * 2];
    char args[sizeof(ssid) + sizeof(psk) + 32];
    (void)b;
    if (!credentials || !credentials->ssid[0]) return LE_INVALID;
    json_escape(ssid, sizeof(ssid), credentials->ssid);
    json_escape(psk, sizeof(psk), credentials->password);
    if (snprintf(args, sizeof(args), "{\"ssid\":\"%s\",\"psk\":\"%s\"}", ssid, psk) >= (int)sizeof(args))
        return LE_INVALID;
    return adapter_json_command(LE_ADAPTER_NETWORK_SOCK, "connect", args);
}

static int disconnect_wifi(struct le_backend *b)
{
    (void)b;
    return adapter_json_command(LE_ADAPTER_NETWORK_SOCK, "disconnect", NULL);
}

static int hostname(struct le_backend *b, const char *name)
{
    size_t i, length;
    (void)b;
    if (!name) return LE_INVALID;
    length = strlen(name);
    if (!length || length >= 64 || name[0] == '-' ||
        name[length - 1] == '-')
        return LE_INVALID;
    for (i = 0; i < length; i++)
        if (!isalnum((unsigned char)name[i]) && name[i] != '-')
            return LE_INVALID;
    return sethostname(name, length) == 0 ? LE_OK : LE_IO;
}

static int wake(struct le_backend *b, struct le_wake_word_state *o)
{
    char response[LE_ADAPTER_MSG_MAX];
    int v, rc, found = 0;
    (void)b;
    rc = adapter_command(LE_ADAPTER_WAKEWORD_SOCK, "status", NULL,
                         response, sizeof(response));
    if (rc != LE_OK) return rc;
    memset(o, 0, sizeof(*o));
    if (json_get_bool(response, "enabled", &v) > 0) { o->enabled = v; found = 1; }
    if (json_get_string(response, "wake_word", o->wake_word, sizeof(o->wake_word)) > 0) found = 1;
    if (json_get_string(response, "model_status", o->model_status, sizeof(o->model_status)) > 0) found = 1;
    if (json_get_int(response, "sensitivity", &v) > 0) { o->sensitivity = v; found = 1; }
    if (json_get_int(response, "cooldown_ms", &v) > 0) o->cooldown_ms = v;
    if (json_get_int(response, "detected_count", &v) > 0) o->detected_count = v;
    if (json_get_int(response, "cpu_cost", &v) > 0) o->cpu_cost = v;
    if (json_get_int(response, "memory_cost_mb", &v) > 0) o->memory_cost_mb = v;
    return found ? LE_OK : LE_IO;
}

static int wake_set(struct le_backend *b, const char *word)
{
    char escaped[LE_TEXT * 2], args[LE_TEXT * 2 + 16];
    (void)b;
    if (!word || !word[0] || strlen(word) >= LE_TEXT) return LE_INVALID;
    json_escape(escaped, sizeof(escaped), word);
    if (snprintf(args, sizeof(args), "{\"word\":\"%s\"}", escaped) >= (int)sizeof(args)) return LE_INVALID;
    return adapter_json_command(LE_ADAPTER_WAKEWORD_SOCK, "set_word", args);
}

static int sensitivity(struct le_backend *b, int value)
{
    char args[64];
    (void)b;
    if (value < 0 || value > 100) return LE_INVALID;
    snprintf(args, sizeof(args), "{\"sensitivity\":%d}", value);
    return adapter_json_command(LE_ADAPTER_WAKEWORD_SOCK, "set_sensitivity", args);
}

static int wake_test(struct le_backend *b)
{
    (void)b;
    return adapter_json_command(LE_ADAPTER_WAKEWORD_SOCK, "test", NULL);
}

static int bluetooth_address_valid(const char *address)
{
    int i;

    if (!address || strlen(address) != 17)
        return 0;
    for (i = 0; i < 17; ++i) {
        if ((i + 1) % 3 == 0) {
            if (address[i] != ':')
                return 0;
        } else if (!((address[i] >= '0' && address[i] <= '9') ||
                     (address[i] >= 'a' && address[i] <= 'f') ||
                     (address[i] >= 'A' && address[i] <= 'F'))) {
            return 0;
        }
    }
    return 1;
}

static void bluetooth_parse_devices(const char *response, const char *key,
                                    struct le_bluetooth_device *devices,
                                    size_t *count)
{
    const char *array;
    const char *end;
    const char *object;

    *count = 0;
    array = strstr(response, key);
    if (!array || !(array = strchr(array, '[')))
        return;
    end = strchr(array, ']');
    if (!end)
        return;
    object = array + 1;
    while (*count < LE_MAX_BLUETOOTH_DEVICES &&
           (object = strstr(object, "{\"address\"")) != NULL && object < end) {
        const char *object_end = strchr(object, '}');
        char item[512];
        size_t length;
        struct le_bluetooth_device *device = &devices[*count];

        if (!object_end || object_end > end)
            break;
        length = (size_t)(object_end - object + 1);
        if (length >= sizeof(item))
            break;
        memcpy(item, object, length);
        item[length] = '\0';
        memset(device, 0, sizeof(*device));
        if (json_get_string(item, "address", device->address,
                            sizeof(device->address)) < 1)
            break;
        (void)json_get_string(item, "name", device->name, sizeof(device->name));
        (void)json_get_int(item, "type", &device->type);
        (void)json_get_int(item, "rssi", &device->rssi);
        (void)json_get_bool(item, "paired", &device->paired);
        (void)json_get_bool(item, "connected", &device->connected);
        ++*count;
        object = object_end + 1;
    }
}

static int bluetooth(struct le_backend *b, struct le_bluetooth_state *o)
{
    char response[LE_ADAPTER_MSG_MAX];
    int rc;

    (void)b;
    rc = adapter_command(LE_ADAPTER_BLUETOOTH_SOCK, "status", NULL,
                         response, sizeof(response));
    if (rc != LE_OK)
        return rc;
    memset(o, 0, sizeof(*o));
    (void)json_get_string(response, "state", o->state, sizeof(o->state));
    (void)json_get_string(response, "transport", o->transport,
                          sizeof(o->transport));
    (void)json_get_string(response, "hci", o->hci, sizeof(o->hci));
    (void)json_get_string(response, "local_name", o->local_name,
                          sizeof(o->local_name));
    (void)json_get_string(response, "last_error", o->last_error,
                          sizeof(o->last_error));
    (void)json_get_bool(response, "available", &o->available);
    (void)json_get_bool(response, "enabled", &o->enabled);
    (void)json_get_bool(response, "activation_attempted",
                        &o->activation_attempted);
    (void)json_get_bool(response, "scanning", &o->scanning);
    (void)json_get_bool(response, "pairing", &o->pairing);
    (void)json_get_bool(response, "pairing_mode", &o->pairing_mode);
    (void)json_get_bool(response, "classic", &o->classic);
    (void)json_get_bool(response, "le", &o->le);
    (void)json_get_bool(response, "ssp", &o->ssp);
    (void)json_get_bool(response, "secure_connection", &o->secure_connection);
    (void)json_get_bool(response, "connectable", &o->connectable);
    (void)json_get_bool(response, "discoverable", &o->discoverable);
    (void)json_get_bool(response, "bondable", &o->bondable);
    (void)json_get_bool(response, "hidp", &o->hidp);
    bluetooth_parse_devices(response, "\"discovered\"", o->discovered,
                            &o->discovered_count);
    bluetooth_parse_devices(response, "\"known_devices\"", o->known,
                            &o->known_count);
    if (o->pairing)
        (void)json_get_string(response, "address", o->pending_pairing.address,
                              sizeof(o->pending_pairing.address));
    (void)json_get_int(response, "type", &o->pending_pairing.type);
    (void)json_get_string(response, "method", o->pending_pairing.method,
                          sizeof(o->pending_pairing.method));
    {
        int value;
        if (json_get_int(response, "value", &value) > 0 && value >= 0)
            o->pending_pairing.value = (unsigned int)value;
    }
    return LE_OK;
}

static int bluetooth_set(struct le_backend *b, int enabled)
{
    char args[32];

    (void)b;
    if (enabled != 0 && enabled != 1)
        return LE_INVALID;
    snprintf(args, sizeof(args), "{\"enabled\":%s}",
             enabled ? "true" : "false");
    return adapter_json_command(LE_ADAPTER_BLUETOOTH_SOCK,
                                "set_enabled", args);
}

static int bluetooth_scan(struct le_backend *b, int start)
{
    (void)b;
    return adapter_json_command(LE_ADAPTER_BLUETOOTH_SOCK,
                                start ? "scan" : "scan_stop", NULL);
}

static int bluetooth_pair(struct le_backend *b, const char *address, int type,
                          int io_capability)
{
    char args[128];

    (void)b;
    if (!bluetooth_address_valid(address) || type < 0 || type > 255 ||
        io_capability < 0 || io_capability > 4)
        return LE_INVALID;
    snprintf(args, sizeof(args),
             "{\"address\":\"%s\",\"type\":%d,\"io_capability\":%d}",
             address, type, io_capability);
    return adapter_json_command(LE_ADAPTER_BLUETOOTH_SOCK, "pair", args);
}

static int bluetooth_unpair(struct le_backend *b, const char *address, int type)
{
    char args[96];

    (void)b;
    if (!bluetooth_address_valid(address) || type < 0 || type > 255)
        return LE_INVALID;
    snprintf(args, sizeof(args), "{\"address\":\"%s\",\"type\":%d}",
             address, type);
    return adapter_json_command(LE_ADAPTER_BLUETOOTH_SOCK, "unpair", args);
}

static int bluetooth_disconnect(struct le_backend *b, const char *address,
                                int type)
{
    char args[96];

    (void)b;
    if (!bluetooth_address_valid(address) || type < 0 || type > 255)
        return LE_INVALID;
    snprintf(args, sizeof(args), "{\"address\":\"%s\",\"type\":%d}",
             address, type);
    return adapter_json_command(LE_ADAPTER_BLUETOOTH_SOCK, "disconnect", args);
}

static int bluetooth_pairing_response(struct le_backend *b, const char *address,
                                      int type, const char *method,
                                      unsigned int value, const char *pin)
{
    char args[192];
    const char *command;

    (void)b;
    if (!bluetooth_address_valid(address) || type < 0 || type > 255 || !method)
        return LE_INVALID;
    if (!strcmp(method, "confirm")) {
        command = "pair_confirm";
        snprintf(args, sizeof(args), "{\"address\":\"%s\",\"type\":%d}",
                 address, type);
    } else if (!strcmp(method, "reject")) {
        command = "pair_reject";
        snprintf(args, sizeof(args), "{\"address\":\"%s\",\"type\":%d}",
                 address, type);
    } else if (!strcmp(method, "passkey")) {
        command = "pair_passkey";
        if (value > 999999)
            return LE_INVALID;
        snprintf(args, sizeof(args),
                 "{\"address\":\"%s\",\"type\":%d,\"passkey\":%u}",
                 address, type, value);
    } else if (!strcmp(method, "pin")) {
        size_t i;
        command = "pair_pin";
        if (!pin || !pin[0] || strlen(pin) > 16)
            return LE_INVALID;
        for (i = 0; pin[i]; ++i)
            if ((unsigned char)pin[i] < 0x20 || pin[i] == '"' || pin[i] == '\\')
                return LE_INVALID;
        snprintf(args, sizeof(args),
                 "{\"address\":\"%s\",\"type\":%d,\"pin\":\"%s\"}",
                 address, type, pin);
    } else {
        return LE_INVALID;
    }
    return adapter_json_command(LE_ADAPTER_BLUETOOTH_SOCK, command, args);
}

static int bluetooth_controller_setting(struct le_backend *b, const char *command,
                                         int enabled)
{
    char args[32];

    (void)b;
    if (enabled != 0 && enabled != 1)
        return LE_INVALID;
    snprintf(args, sizeof(args), "{\"enabled\":%s}",
             enabled ? "true" : "false");
    return adapter_json_command(LE_ADAPTER_BLUETOOTH_SOCK, command, args);
}

static int bluetooth_discoverable(struct le_backend *b, int enabled)
{
    return bluetooth_controller_setting(b, "set_discoverable", enabled);
}

static int bluetooth_connectable(struct le_backend *b, int enabled)
{
    return bluetooth_controller_setting(b, "set_connectable", enabled);
}

static int bluetooth_pairing_mode(struct le_backend *b, int enabled)
{
    return bluetooth_controller_setting(b, "pairing_mode", enabled);
}

static int airplay(struct le_backend *b, struct le_airplay_state *o)
{
    char response[LE_ADAPTER_MSG_MAX];
    int rc;
    (void)b;
    rc = adapter_command(LE_ADAPTER_AIRPLAY_SOCK, "status", NULL,
                         response, sizeof(response));
    if (rc != LE_OK)
        return rc;
    memset(o, 0, sizeof(*o));
    o->available = 0;
    (void)json_get_bool(response, "available", &o->available);
    (void)json_get_bool(response, "enabled", &o->enabled);
    (void)json_get_bool(response, "nqptp_running", &o->nqptp_running);
    (void)json_get_bool(response, "shairport_running", &o->shairport_running);
    (void)json_get_string(response, "playback_state", o->playback_state,
                          sizeof(o->playback_state));
    (void)json_get_string(response, "source", o->source,
                          sizeof(o->source));
    (void)json_get_string(response, "title", o->title, sizeof(o->title));
    (void)json_get_string(response, "artist", o->artist, sizeof(o->artist));
    (void)json_get_string(response, "album", o->album, sizeof(o->album));
    o->playing = !strcmp(o->playback_state, "playing");
    return LE_OK;
}

static int airplay_set(struct le_backend *b, int enabled)
{
    char args[32];
    (void)b;
    if (enabled != 0 && enabled != 1)
        return LE_INVALID;
    snprintf(args, sizeof(args), "{\"enabled\":%s}",
             enabled ? "true" : "false");
    return adapter_json_command(LE_ADAPTER_AIRPLAY_SOCK, "set_enabled", args);
}

static int playback(struct le_backend *b, struct le_playback_state *o)
{
    struct le_airplay_state airplay_state;
    char document[768];
    char active[32];
    int have_runtime_state = 0;

    memset(o, 0, sizeof(*o));
    copy_string(o->state, sizeof(o->state), "idle");
    active[0] = '\0';
    if (read_line("/run/libreecho-audio/status.json", document,
                  sizeof(document)) == 0) {
        char parsed_state[24];

        parsed_state[0] = '\0';
        if (json_get_string(document, "state", parsed_state,
                            sizeof(parsed_state)) > 0 &&
            (!strcmp(parsed_state, "idle") ||
             !strcmp(parsed_state, "playing") ||
             !strcmp(parsed_state, "system") ||
             !strcmp(parsed_state, "announcing") ||
             !strcmp(parsed_state, "alarm"))) {
            copy_string(o->state, sizeof(o->state), parsed_state);
            have_runtime_state = 1;
        }
        (void)json_get_string(document, "active", active, sizeof(active));
        (void)json_get_bool(document, "media", &o->media_active);
        (void)json_get_bool(document, "system", &o->system_active);
        (void)json_get_bool(document, "announcement",
                            &o->announcement_active);
        (void)json_get_bool(document, "alarm", &o->alarm_active);
    }

    if (active[0])
        copy_string(o->source, sizeof(o->source), active);
    else if (!strcmp(o->state, "system"))
        copy_string(o->source, sizeof(o->source), "system");
    else if (!strcmp(o->state, "announcing"))
        copy_string(o->source, sizeof(o->source), "announcement");
    else if (!strcmp(o->state, "alarm"))
        copy_string(o->source, sizeof(o->source), "alarm");

    if (airplay(b, &airplay_state) == LE_OK && airplay_state.playing &&
        (!have_runtime_state || o->media_active ||
         !strcmp(o->state, "playing"))) {
        if (!have_runtime_state) {
            copy_string(o->state, sizeof(o->state), "playing");
            o->media_active = 1;
        }
        copy_string(o->source, sizeof(o->source), "airplay2");
        copy_string(o->title, sizeof(o->title), airplay_state.title);
        copy_string(o->artist, sizeof(o->artist), airplay_state.artist);
        copy_string(o->album, sizeof(o->album), airplay_state.album);
        o->metadata_available = o->title[0] || o->artist[0] || o->album[0];
    }
    return LE_OK;
}

static int linux_reboot(struct le_backend *b)
{
    (void)b;
    le_log_info("backend: reboot requested");
    sync();
    return reboot(LINUX_REBOOT_CMD_RESTART) == 0 ? LE_OK : LE_IO;
}

static int linux_shutdown(struct le_backend *b)
{
    (void)b;
    le_log_info("backend: shutdown requested");
    sync();
    return reboot(LINUX_REBOOT_CMD_POWER_OFF) == 0 ? LE_OK : LE_IO;
}

static int factory_reset(struct le_backend *b)
{
    static const char marker[] = "FACTORY_RESET";
    int fd;
    int close_rc;
    ssize_t written;

    (void)b;
    le_log_info("backend: factory reset requested");
    fd = open("/tmp/.libreecho_factory_reset", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        le_log_perr("backend: cannot write factory reset marker");
        return LE_IO;
    }
    written = write(fd, marker, sizeof(marker) - 1);
    close_rc = close(fd);
    if (written != (ssize_t)(sizeof(marker) - 1) || close_rc < 0) {
        le_log_error("backend: factory reset marker write incomplete");
        return LE_IO;
    }
    return linux_reboot(b);
}

static int tick(struct le_backend *b)
{
    (void)b;
    return LE_OK;
}

static int control(struct le_backend *b, const char *action, const char *value)
{
    (void)b;
    (void)action;
    (void)value;
    return LE_NOT_SUPPORTED;
}

static void destroy(struct le_backend *b)
{
    free(b->data);
}

static const struct le_backend_ops ops = {
    destroy, status, device,
    audio, volume, gain, mute, tone, tts_voice, announce, stop_speech,
    led, colour, brightness, visualizer_enabled, boot_led, led_profile, led_test,
    network, scan, connect_wifi, disconnect_wifi, hostname,
    wake, wake_set, sensitivity, wake_test,
    bluetooth, bluetooth_set, bluetooth_scan, bluetooth_pair,
    bluetooth_unpair, bluetooth_disconnect, bluetooth_pairing_response,
    bluetooth_discoverable, bluetooth_connectable, bluetooth_pairing_mode,
    airplay, airplay_set, playback,
    linux_reboot, linux_shutdown, factory_reset, tick, control
};

int le_linux_create(struct le_backend *b, const char *cfg)
{
    struct linux_state *s = calloc(1, sizeof(*s));
    if (!s)
        return LE_IO;
    if (cfg)
        copy_string(s->config, sizeof(s->config), cfg);
    strcpy(s->temp_path, "/sys/class/thermal/thermal_zone0/temp");
    b->data = s;
    b->ops = &ops;
    return LE_OK;
}
