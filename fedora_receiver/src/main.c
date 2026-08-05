#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested = 0;
static pid_t player_pid = -1;

static void on_signal(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
    if (player_pid > 0) kill(player_pid, SIGTERM);
}

static void usage(const char *program) {
    fprintf(stderr,
            "Usage: %s BOARD_IP [PORT]\n"
            "Example: %s 192.168.1.50 5000\n",
            program, program);
}

static int connect_to_board(const char *host, const char *port) {
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int error = getaddrinfo(host, port, &hints, &addresses);
    if (error != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(error));
        return -1;
    }
    int socket_fd = -1;
    for (struct addrinfo *item = addresses; item; item = item->ai_next) {
        socket_fd = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (socket_fd < 0) continue;
        if (connect(socket_fd, item->ai_addr, item->ai_addrlen) == 0) break;
        close(socket_fd);
        socket_fd = -1;
    }
    freeaddrinfo(addresses);
    return socket_fd;
}

static int start_ffplay(int *input_fd) {
    int pipe_fds[2];
    if (pipe(pipe_fds) < 0) {
        perror("pipe");
        return -1;
    }
    player_pid = fork();
    if (player_pid < 0) {
        perror("fork");
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }
    if (player_pid == 0) {
        dup2(pipe_fds[0], STDIN_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        execlp("ffplay", "ffplay",
               "-hide_banner", "-loglevel", "warning",
               "-fflags", "nobuffer", "-flags", "low_delay",
               "-probesize", "32", "-analyzeduration", "0",
               "-f", "hevc", "-i", "pipe:0", (char *)NULL);
        perror("exec ffplay");
        _exit(127);
    }
    close(pipe_fds[0]);
    *input_fd = pipe_fds[1];
    return 0;
}

static bool write_all(int fd, const void *data, size_t length) {
    const unsigned char *cursor = data;
    while (length && !stop_requested) {
        ssize_t written = write(fd, cursor, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        cursor += written;
        length -= (size_t)written;
    }
    return length == 0;
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const char *port = argc == 3 ? argv[2] : "5000";
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    int socket_fd = connect_to_board(argv[1], port);
    if (socket_fd < 0) {
        fprintf(stderr, "Cannot connect to %s:%s: %s\n", argv[1], port, strerror(errno));
        return EXIT_FAILURE;
    }
    fprintf(stderr, "Connected to %s:%s\n", argv[1], port);

    int player_input = -1;
    if (start_ffplay(&player_input) < 0) {
        close(socket_fd);
        return EXIT_FAILURE;
    }

    unsigned char buffer[256 * 1024];
    while (!stop_requested) {
        ssize_t received = recv(socket_fd, buffer, sizeof(buffer), 0);
        if (received < 0 && errno == EINTR) continue;
        if (received < 0) {
            perror("recv");
            break;
        }
        if (received == 0) {
            fprintf(stderr, "Sender disconnected\n");
            break;
        }
        if (!write_all(player_input, buffer, (size_t)received)) break;
    }

    close(socket_fd);
    close(player_input);
    if (player_pid > 0) {
        if (stop_requested) kill(player_pid, SIGTERM);
        waitpid(player_pid, NULL, 0);
    }
    return stop_requested ? EXIT_SUCCESS : EXIT_FAILURE;
}
