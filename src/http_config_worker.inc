/* Included after the response helpers and shared bounded child registry. */
static int config_worker_pending;

static int configuration_worker_request(const struct api_context *api,
                                        const struct api_request *q)
{
    /* Mock operations modify an in-memory backend, not shared device daemons. */
    return strcmp(le_backend_mode(api->backend), "mock") &&
        !strcmp(q->method, "POST") &&
        (!strcmp(q->path, "/api/v1/setup") ||
         !strcmp(q->path, "/api/v1/network/wifi/connect"));
}

static int refresh_setup_completed(struct api_context *api)
{
    char path[512], marker[32], saved[16384], wake[LE_TEXT];
    int n, local_only, telemetry, sensitivity, integrations;
    if (api->setup_completed || !api->config_path[0])
        return 0;
    n = snprintf(path, sizeof(path), "%s.setup-complete", api->config_path);
    if (n < 0 || (size_t)n >= sizeof(path))
        return -1;
    if (access(path, F_OK))
        return errno == ENOENT ? 0 : -1;
    if (config_read(path, marker, sizeof(marker)) <= 0 ||
        (strcmp(marker, "schema=1") && strcmp(marker, "schema=1\n") &&
         strcmp(marker, "schema=1\r\n")) ||
        (n = config_read(api->config_path, saved, sizeof(saved))) <= 0 ||
        !json_valid_object(saved, (size_t)n) ||
        json_get_bool(saved, "privacy_local_only", &local_only) != 1 ||
        json_get_bool(saved, "privacy_telemetry", &telemetry) != 1 ||
        json_get_string(saved, "wake_word", wake, sizeof(wake)) != 1 || !wake[0] ||
        json_get_int(saved, "wake_sensitivity", &sensitivity) != 1 ||
        sensitivity < 0 || sensitivity > 100 ||
        json_get_int(saved, "integrations", &integrations) != 1 ||
        integrations < 0 || integrations > 63)
        return -1;
    /* Only setup-owned fields are imported. Never replace live authentication,
       sessions, or the entire context with a stale forked copy. */
    api->privacy_local_only = local_only;
    api->privacy_telemetry = telemetry;
    strcpy(api->configured_wake_word, wake);
    api->configured_wake_sensitivity = sensitivity;
    api->configured_wake_valid = 1;
    /* Setup enables AirPlay; preserve all unrelated live integration choices. */
    api->integrations = (api->integrations & ~16u) | ((unsigned)integrations & 16u);
    api->setup_completed = 1;
    return 0;
}

static void sync_configuration_worker(struct api_context *api)
{
    sigset_t blocked, previous;
    if (!config_worker_pending)
        return;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous))
        return;
    reap_child_workers(0);
    /* The durable marker is written before the worker sends success. This
       also makes the very next GET see completion before SIGCHLD arrives. */
    if (refresh_setup_completed(api) == 0 &&
        child_worker_counts[CHILD_WORKER_CONFIG] == 0)
        config_worker_pending = 0;
    sigprocmask(SIG_SETMASK, &previous, NULL);
}

static int start_configuration_worker(int fd, struct api_context *api,
                                      const struct api_request *q)
{
    sigset_t previous;
    pid_t pid;
    int slot;
    struct api_response r;
    if (config_worker_pending ||
        child_worker_begin(CHILD_WORKER_CONFIG, &previous, &slot))
        return -1;
    pid = fork();
    if (pid < 0) {
        sigprocmask(SIG_SETMASK, &previous, NULL);
        return -1;
    }
    if (pid == 0) {
        signal(SIGCHLD, SIG_DFL);
        signal(SIGALRM, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        sigprocmask(SIG_SETMASK, &previous, NULL);
        le_close_inherited_fds(fd);
        /* Network and feature activation are bounded even if an adapter hangs. */
        alarm(120);
        memset(&r, 0, sizeof(r));
        api_handle(api, q, &r);
        response(fd, r.status, r.type, r.body, r.length);
        close(fd);
        _exit(0);
    }
    child_worker_register(slot, pid, CHILD_WORKER_CONFIG);
    config_worker_pending = 1;
    sigprocmask(SIG_SETMASK, &previous, NULL);
    close(fd);
    return 0;
}
