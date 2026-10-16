#define _POSIX_C_SOURCE 200809L

#include "adapter/live_dns.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static int fake_dns(int fd)
{
    unsigned char query[512];
    unsigned char response[512];
    struct sockaddr_in peer;
    socklen_t peer_size = sizeof(peer);
    ssize_t count = recvfrom(fd, query, sizeof(query), 0,
                             (struct sockaddr *)&peer, &peer_size);
    size_t question;

    if (count < 16)
        return 1;
    question = (size_t)count - 12U;
    memcpy(response, query, (size_t)count);
    response[2] = 0x81; response[3] = 0x80; /* response, no error */
    response[6] = 0; response[7] = 1;       /* one answer */
    /* Name pointer to the question, A, IN, TTL=60, length=4, 203.0.113.7. */
    {
        unsigned char answer[] = {
            0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x3c, 0x00, 0x04,
            203, 0, 113, 7
        };
        (void)question;
        memcpy(response + count, answer, sizeof(answer));
        count += (ssize_t)sizeof(answer);
    }
    return sendto(fd, response, (size_t)count, 0,
                  (struct sockaddr *)&peer, peer_size) == count ? 0 : 1;
}

int main(void)
{
    struct sockaddr_in server;
    struct in_addr resolved;
    char path[128];
    FILE *conf;
    socklen_t size = sizeof(server);
    pid_t child;
    int fd;
    int status;

    alarm(20);
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(fd >= 0);
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server.sin_port = 0;
    CHECK(bind(fd, (struct sockaddr *)&server, sizeof(server)) == 0);
    CHECK(getsockname(fd, (struct sockaddr *)&server, &size) == 0);

    snprintf(path, sizeof(path), "/tmp/le-live-dns-%d.conf", (int)getpid());
    conf = fopen(path, "w");
    CHECK(conf != NULL);
    fprintf(conf, "nameserver 127.0.0.1:%u\n", ntohs(server.sin_port));
    fclose(conf);

    child = fork();
    CHECK(child >= 0);
    if (child == 0)
        _exit(fake_dns(fd));
    close(fd);

    CHECK(le_live_dns_resolve_ipv4("chatgpt.com", path, &resolved, 2000) == 0);
    CHECK(ntohl(resolved.s_addr) == 0xcb007107U);
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* Bad labels and an absent resolver fail boundedly. */
    CHECK(le_live_dns_resolve_ipv4("", path, &resolved, 100) < 0);
    CHECK(le_live_dns_resolve_ipv4("chatgpt.com", "/nonexistent/resolv.conf",
                                   &resolved, 100) < 0);
    unlink(path);
    puts("live dns: ok");
    return 0;
}
