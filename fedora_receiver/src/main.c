#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *program) {
    fprintf(stderr,
            "Usage: %s BOARD_IP [PORT]\n"
            "Example: %s 192.168.1.50 5000\n",
            program, program);
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *port = argc == 3 ? argv[2] : "5000";
    char url[512];
    int length = snprintf(url, sizeof(url), "tcp://%s:%s?tcp_nodelay=1", argv[1], port);
    if (length < 0 || (size_t)length >= sizeof(url)) {
        fprintf(stderr, "Board address is too long\n");
        return EXIT_FAILURE;
    }

    fprintf(stderr, "Opening %s\n", url);
    execlp("ffplay", "ffplay",
           "-hide_banner", "-loglevel", "warning",
           "-fflags", "nobuffer",
           "-flags", "low_delay",
           "-avioflags", "direct",
           "-probesize", "32",
           "-analyzeduration", "0",
           "-max_delay", "0",
           "-framedrop",
           "-sync", "ext",
           "-f", "hevc",
           "-i", url,
           (char *)NULL);

    fprintf(stderr, "Cannot start ffplay: %s\n", strerror(errno));
    return EXIT_FAILURE;
}
