#include "tls.h"
#include "backend.h"

struct le_tls { int unused; };

struct le_tls *le_tls_client_open(int fd, const char *hostname)
{
    (void)fd;
    (void)hostname;
    return NULL;
}

struct le_tls *le_tls_server_open(int fd, const char *cert_path,
                                  const char *key_path)
{
    (void)fd;
    (void)cert_path;
    (void)key_path;
    return NULL;
}

int le_tls_client_verified(const struct le_tls *tls)
{
    (void)tls;
    return 0;
}

long le_tls_read(struct le_tls *tls, void *buf, size_t len)
{
    (void)tls;
    (void)buf;
    (void)len;
    return -1;
}

int le_tls_pending(struct le_tls *tls)
{
    (void)tls;
    return 0;
}

long le_tls_write(struct le_tls *tls, const void *buf, size_t len)
{
    (void)tls;
    (void)buf;
    (void)len;
    return -1;
}

long le_tls_write_deadline(struct le_tls *tls, const void *buf, size_t len,
                           int timeout_ms)
{
    (void)tls;
    (void)buf;
    (void)len;
    (void)timeout_ms;
    return -1;
}

void le_tls_close(struct le_tls *tls)
{
    (void)tls;
}

int le_tls_cert_info(const char *cert_path, char *not_after, size_t na_size,
                    char *fingerprint, size_t fp_size)
{
    (void)cert_path;
    if (not_after && na_size) not_after[0] = '\0';
    if (fingerprint && fp_size) fingerprint[0] = '\0';
    return LE_NOT_SUPPORTED;
}

int le_tls_ensure_self_signed(const char *cert_path, const char *key_path,
                              const char *common_name)
{
    (void)cert_path;
    (void)key_path;
    (void)common_name;
    return LE_NOT_SUPPORTED;
}
