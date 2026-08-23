/* Stand-in for tinymix: records the control writes micd makes and succeeds,
   so the harness can assert the capture path was configured. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *log = getenv("LE_FAKE_TINYMIX_LOG");
    int i;

    if (log && *log) {
        FILE *f = fopen(log, "a");

        if (f) {
            for (i = 1; i < argc; ++i)
                fprintf(f, "%s%s", argv[i], i + 1 < argc ? " " : "\n");
            fclose(f);
        }
    }
    return 0;
}
