#define _POSIX_C_SOURCE 200809L

#include "inherited_fds.h"

#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    int keep[2];
    int unrelated[2];
    char message[3] = {0};
    pid_t child;
    int status;

    if (pipe(keep) || pipe(unrelated))
        return 1;
    child = fork();
    if (child < 0)
        return 1;
    if (child == 0) {
        close(keep[0]);
        le_close_inherited_fds(keep[1]);
        if (write(keep[1], "ok", 2) != 2)
            _exit(1);
        close(keep[1]);
        _exit(0);
    }

    close(keep[1]);
    close(unrelated[1]);
    if (read(keep[0], message, 2) != 2 ||
        message[0] != 'o' || message[1] != 'k' ||
        read(unrelated[0], message, 1) != 0 ||
        waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return 1;

    close(keep[0]);
    close(unrelated[0]);
    puts("inherited descriptor cleanup: keep one, close the rest: ok");
    return 0;
}
