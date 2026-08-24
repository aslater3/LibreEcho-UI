#include "tls.h"
#include "backend.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct le_tls {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
    mbedtls_net_context net;
    mbedtls_x509_crt cert;      /* server only */
    mbedtls_pk_context key;     /* server only */
    int is_server;
    int verified;
};

static const char *SEED = "libreecho-tls";
#define LE_TLS_HANDSHAKE_TIMEOUT_MS 10000

static long long monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return (long long)time(NULL) * 1000LL;
    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

struct le_tls *le_tls_client_open(int fd, const char *hostname)
{
    struct le_tls *tls = calloc(1, sizeof(*tls));

    if (!tls)
        return NULL;
    mbedtls_ssl_init(&tls->ssl);
    mbedtls_ssl_config_init(&tls->conf);
    mbedtls_ctr_drbg_init(&tls->drbg);
    mbedtls_entropy_init(&tls->entropy);
    mbedtls_net_init(&tls->net);
    tls->net.fd = fd;

    if (mbedtls_ctr_drbg_seed(&tls->drbg, mbedtls_entropy_func, &tls->entropy,
                              (const unsigned char *)SEED, strlen(SEED)) ||
        mbedtls_ssl_config_defaults(&tls->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT))
        goto fail;

    /*
     * No CA bundle ships on this image and there is no mechanism to keep one
     * current, so a chain cannot be validated. Encrypt anyway -- an
     * unauthenticated TLS stream is still better than cleartext for a radio
     * feed -- and record that it was not verified so callers do not claim
     * more than was actually established.
     */
    mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&tls->conf, mbedtls_ctr_drbg_random, &tls->drbg);

    if (mbedtls_ssl_setup(&tls->ssl, &tls->conf))
        goto fail;
    if (hostname && *hostname && mbedtls_ssl_set_hostname(&tls->ssl, hostname))
        goto fail;
    mbedtls_ssl_set_bio(&tls->ssl, &tls->net, mbedtls_net_send,
                        mbedtls_net_recv, NULL);

    for (;;) {
        int rc = mbedtls_ssl_handshake(&tls->ssl);

        if (!rc)
            break;
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE)
            goto fail;
    }
    tls->verified = 0;
    return tls;

fail:
    tls->net.fd = -1;            /* the caller still owns the socket */
    le_tls_close(tls);
    return NULL;
}

struct le_tls *le_tls_server_open(int fd, const char *cert_path,
                                  const char *key_path)
{
    struct le_tls *tls = calloc(1, sizeof(*tls));
    int rc, original_flags;
    long long deadline;

    if (!tls)
        return NULL;
    mbedtls_ssl_init(&tls->ssl);
    mbedtls_ssl_config_init(&tls->conf);
    mbedtls_ctr_drbg_init(&tls->drbg);
    mbedtls_entropy_init(&tls->entropy);
    mbedtls_net_init(&tls->net);
    mbedtls_x509_crt_init(&tls->cert);
    mbedtls_pk_init(&tls->key);
    tls->net.fd = fd;
    tls->is_server = 1;

    if (mbedtls_ctr_drbg_seed(&tls->drbg, mbedtls_entropy_func, &tls->entropy,
                              (const unsigned char *)SEED, strlen(SEED)) ||
        mbedtls_ssl_config_defaults(&tls->conf, MBEDTLS_SSL_IS_SERVER,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT))
        goto fail;

    if (mbedtls_x509_crt_parse_file(&tls->cert, cert_path) ||
        mbedtls_pk_parse_keyfile(&tls->key, key_path, NULL,
                                 mbedtls_ctr_drbg_random, &tls->drbg))
        goto fail;

    mbedtls_ssl_conf_rng(&tls->conf, mbedtls_ctr_drbg_random, &tls->drbg);
    if (mbedtls_ssl_conf_own_cert(&tls->conf, &tls->cert, &tls->key))
        goto fail;
    if (mbedtls_ssl_setup(&tls->ssl, &tls->conf))
        goto fail;
    mbedtls_ssl_set_bio(&tls->ssl, &tls->net, mbedtls_net_send,
                        mbedtls_net_recv, NULL);

    original_flags = fcntl(fd, F_GETFL);
    if (original_flags < 0 || fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) < 0)
        goto fail;
    deadline = monotonic_ms() + LE_TLS_HANDSHAKE_TIMEOUT_MS;
    while ((rc = mbedtls_ssl_handshake(&tls->ssl)) != 0) {
        struct pollfd waitfd;
        long long remaining = deadline - monotonic_ms();
        int waited;
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE)
            goto fail;
        if (remaining <= 0)
            goto fail;
        waitfd.fd = fd;
        waitfd.events = rc == MBEDTLS_ERR_SSL_WANT_WRITE ? POLLOUT : POLLIN;
        waitfd.revents = 0;
        waited = poll(&waitfd, 1, remaining > LE_TLS_HANDSHAKE_TIMEOUT_MS ?
                      LE_TLS_HANDSHAKE_TIMEOUT_MS : (int)remaining);
        if (waited < 0 && errno == EINTR)
            continue;
        if (waited <= 0 || (waited &&
                            (waitfd.revents & (POLLERR | POLLHUP | POLLNVAL))))
            goto fail;
    }
    if (fcntl(fd, F_SETFL, original_flags) < 0)
        goto fail;
    return tls;

fail:
    le_tls_close(tls);
    return NULL;
}

int le_tls_client_verified(const struct le_tls *tls)
{
    return tls ? tls->verified : 0;
}

int le_tls_pending(struct le_tls *tls)
{
    return tls ? (int)mbedtls_ssl_get_bytes_avail(&tls->ssl) : 0;
}

long le_tls_read(struct le_tls *tls, void *buf, size_t len)
{
    int rc;

    if (!tls)
        return -1;
    do {
        rc = mbedtls_ssl_read(&tls->ssl, buf, len);
    } while (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        return 0;
    return rc < 0 ? -1 : rc;
}

long le_tls_write(struct le_tls *tls, const void *buf, size_t len)
{
    int rc;

    if (!tls)
        return -1;
    do {
        rc = mbedtls_ssl_write(&tls->ssl, buf, len);
    } while (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE);
    return rc < 0 ? -1 : rc;
}

void le_tls_close(struct le_tls *tls)
{
    if (!tls)
        return;
    if (tls->net.fd >= 0)
        mbedtls_ssl_close_notify(&tls->ssl);
    mbedtls_ssl_free(&tls->ssl);
    mbedtls_ssl_config_free(&tls->conf);
    mbedtls_ctr_drbg_free(&tls->drbg);
    mbedtls_entropy_free(&tls->entropy);
    if (tls->is_server) {          /* only a server context ever inits these */
        mbedtls_x509_crt_free(&tls->cert);
        mbedtls_pk_free(&tls->key);
    }
    free(tls);
}

/*
 * Self-signed certificate generation.
 *
 * Generated on the device rather than shipped in the image, because a
 * certificate baked into a public image would have its private key baked in
 * too -- every unit would share it, and anyone with the image could
 * impersonate any device. Generated once, on first use, and kept with the
 * rest of the configuration so it survives updates.
 *
 * Ten years and no revocation story: this is a LAN device with no clock
 * guarantee at first boot and no way to renew unattended, so an expiry the
 * owner would have to service is worse than a long one.
 */
int le_tls_cert_info(const char *cert_path, char *not_after, size_t na_size,
                     char *fingerprint, size_t fp_size)
{
    mbedtls_x509_crt crt;
    unsigned char sha[32];
    int rc = LE_IO;

    mbedtls_x509_crt_init(&crt);
    if (mbedtls_x509_crt_parse_file(&crt, cert_path))
        goto done;
    if (not_after && na_size)
        snprintf(not_after, na_size, "%04d-%02d-%02d",
                 crt.valid_to.year, crt.valid_to.mon, crt.valid_to.day);
    if (fingerprint && fp_size) {
        static const char hex[] = "0123456789ABCDEF";
        size_t i, n = 0;
        if (mbedtls_sha256(crt.raw.p, crt.raw.len, sha, 0))
            goto done;
        for (i = 0; i < sizeof(sha) && n + 3 < fp_size; i++) {
            if (i) fingerprint[n++] = ':';
            fingerprint[n++] = hex[sha[i] >> 4];
            fingerprint[n++] = hex[sha[i] & 15];
        }
        fingerprint[n] = '\0';
    }
    rc = LE_OK;
done:
    mbedtls_x509_crt_free(&crt);
    return rc;
}

static int write_private(const char *path, const unsigned char *data, size_t len)
{
    char tmp[512];
    int fd, rc = LE_IO;

    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp))
        return LE_IO;
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return LE_IO;
    if (write(fd, data, len) == (ssize_t)len && !fsync(fd))
        rc = LE_OK;
    if (close(fd) || rc != LE_OK || rename(tmp, path)) {
        unlink(tmp);
        return LE_IO;
    }
    return LE_OK;
}

int le_tls_ensure_self_signed(const char *cert_path, const char *key_path,
                              const char *common_name)
{
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
    mbedtls_mpi serial;
    unsigned char buf[4096];
    char subject[128];
    char not_before[16], not_after[16];
    int rc = LE_IO, len;

    if (!access(cert_path, R_OK) && !access(key_path, R_OK)) {
        mbedtls_x509_crt existing;
        mbedtls_x509_crt_init(&existing);
        if (!mbedtls_x509_crt_parse_file(&existing, cert_path) &&
            !mbedtls_x509_time_is_past(&existing.valid_to)) {
            mbedtls_x509_crt_free(&existing);
            return LE_OK;                   /* existing certificate is current */
        }
        mbedtls_x509_crt_free(&existing);
        /* A certificate made while the clock was near the Unix epoch is
           expired after NTP corrects the clock; fall through and replace it. */
    }

    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_entropy_init(&entropy);
    mbedtls_mpi_init(&serial);

    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char *)SEED, strlen(SEED)))
        goto done;
    if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)))
        goto done;
    /* P-256 rather than RSA: seconds instead of minutes to generate on this
       CPU, and small enough that first boot is not visibly delayed. */
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                            mbedtls_ctr_drbg_random, &drbg))
        goto done;

    snprintf(subject, sizeof(subject), "CN=%s,O=LibreEcho",
             common_name && *common_name ? common_name : "libreecho.local");

    /* Use a fixed broad validity window. A first boot may occur before NTP
       has set the clock; deriving validity from time(NULL) would create a
       certificate that is already expired when the clock is corrected. */
    snprintf(not_before, sizeof(not_before), "%s", "20200101000000");
    snprintf(not_after, sizeof(not_after), "%s", "20991231235959");

    if (mbedtls_mpi_read_string(&serial, 10, "1"))
        goto done;
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    if (mbedtls_x509write_crt_set_subject_name(&crt, subject) ||
        mbedtls_x509write_crt_set_issuer_name(&crt, subject) ||
        mbedtls_x509write_crt_set_serial(&crt, &serial) ||
        mbedtls_x509write_crt_set_validity(&crt, not_before, not_after) ||
        mbedtls_x509write_crt_set_basic_constraints(&crt, 1, 0))
        goto done;
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);

    memset(buf, 0, sizeof(buf));
    if (mbedtls_pk_write_key_pem(&key, buf, sizeof(buf)))
        goto done;
    if (write_private(key_path, buf, strlen((char *)buf)) != LE_OK)
        goto done;

    memset(buf, 0, sizeof(buf));
    len = mbedtls_x509write_crt_pem(&crt, buf, sizeof(buf),
                                    mbedtls_ctr_drbg_random, &drbg);
    if (len < 0)
        goto done;
    if (write_private(cert_path, buf, strlen((char *)buf)) != LE_OK)
        goto done;
    (void)chmod(cert_path, 0644);           /* the certificate is public */
    rc = LE_OK;

done:
    mbedtls_mpi_free(&serial);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return rc;
}
