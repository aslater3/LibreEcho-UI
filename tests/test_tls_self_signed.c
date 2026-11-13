/* le_tls_ensure_self_signed(): a current certificate/key pair is kept across
 * starts (#522), while a mismatched, truncated or missing pair is replaced
 * with one whose key matches its certificate. Links the real src/tls.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "backend.h"
#include "tls.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static size_t slurp(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "rb");
    size_t n = 0;
    if (f) { n = fread(buf, 1, size - 1, f); fclose(f); }
    buf[n] = '\0';
    return n;
}
static int put(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(data, 1, len, f) == len;
    if (f && fclose(f)) ok = 0;
    return ok;
}
static int fp(const char *cert, char *out, size_t size) { return le_tls_cert_info(cert, NULL, 0, out, size); }
static int pair_matches(const char *cert, const char *key)
{
    static char c[8192], k[8192];
    size_t cl = slurp(cert, c, sizeof c), kl = slurp(key, k, sizeof k);
    return cl && kl && le_tls_check_certificate_pair(c, cl, k, kl) == LE_OK;
}
int main(void)
{
    char dir[] = "/tmp/le-tls-XXXXXX", *base = getenv("TMPDIR");
    char tmpl[512], cert[600], key[600], cert2[600], key2[600];
    char fp1[128], fp2[128], fp3[128];
    static char key_before[8192], key_after[8192], other_key[8192], half[8192];
    size_t n;
    if (base && *base) { snprintf(tmpl, sizeof tmpl, "%s/le-tls-XXXXXX", base); } else snprintf(tmpl, sizeof tmpl, "%s", dir);
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 1; }
    snprintf(cert, sizeof cert, "%s/https.crt", tmpl); snprintf(key, sizeof key, "%s/https.key", tmpl);
    snprintf(cert2, sizeof cert2, "%s/other.crt", tmpl); snprintf(key2, sizeof key2, "%s/other.key", tmpl);

    /* first start generates a matching pair */
    CHECK(le_tls_ensure_self_signed(cert, key, "libreecho") == LE_OK);
    CHECK(fp(cert, fp1, sizeof fp1) == LE_OK && fp1[0]);
    CHECK(pair_matches(cert, key));
    CHECK(slurp(key, key_before, sizeof key_before) > 0);

    /* #522: later starts keep the same certificate and key */
    CHECK(le_tls_ensure_self_signed(cert, key, "libreecho") == LE_OK);
    CHECK(fp(cert, fp2, sizeof fp2) == LE_OK && !strcmp(fp1, fp2));
    CHECK(le_tls_ensure_self_signed(cert, key, "libreecho") == LE_OK);
    CHECK(fp(cert, fp2, sizeof fp2) == LE_OK && !strcmp(fp1, fp2));
    CHECK(slurp(key, key_after, sizeof key_after) > 0 && !strcmp(key_before, key_after));

    /* a key that does not match the certificate is replaced by a fresh pair */
    CHECK(le_tls_ensure_self_signed(cert2, key2, "libreecho") == LE_OK);
    n = slurp(key2, other_key, sizeof other_key);
    CHECK(n > 0 && put(key, other_key, n));
    CHECK(!pair_matches(cert, key));
    CHECK(le_tls_ensure_self_signed(cert, key, "libreecho") == LE_OK);
    CHECK(fp(cert, fp3, sizeof fp3) == LE_OK && strcmp(fp1, fp3));
    CHECK(pair_matches(cert, key));

    /* a truncated certificate (partial write) is replaced */
    n = slurp(cert, half, sizeof half);
    CHECK(n > 0 && put(cert, half, n / 2));
    CHECK(le_tls_ensure_self_signed(cert, key, "libreecho") == LE_OK);
    CHECK(pair_matches(cert, key));

    /* a missing key is replaced */
    CHECK(!unlink(key));
    CHECK(le_tls_ensure_self_signed(cert, key, "libreecho") == LE_OK);
    CHECK(pair_matches(cert, key));

    unlink(cert); unlink(key); unlink(cert2); unlink(key2); rmdir(tmpl);
    if (fails) { printf("test-tls-self-signed: %d failure(s)\n", fails); return 1; }
    printf("test-tls-self-signed: ok\n");
    return 0;
}
