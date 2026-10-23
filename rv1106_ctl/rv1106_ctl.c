/* rv1106_ctl — демон керування на борту: TCP :5001 (JSON по рядках) <-> UART камери (ESP32, 921600 8N1).
 * Протокол: seek-ground-station/docs/PROTOCOL.md. Один потік, poll(); без залежностей (uclibc, статично).
 *   hello|ping|status -> відповідь самого демона;  cam -> {cmd} в UART, перша JSON-відповідь з "ok" назад клієнту.
 * Запити до камери йдуть по черзі (одна в польоті), дрібний текст ESP32 ("Calib start") ігнорується.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAXC 4
#define LINE 4096
#define QMAX 16
#define CAM_TIMEOUT_MS 5000

struct client { int fd; char buf[LINE]; int len; };
struct job { int cfd; long id; char cmd[LINE]; };

static struct client cl[MAXC];
static struct job q[QMAX];
static int qn;                       /* q[0] — у польоті, якщо inflight */
static int inflight;
static long long sent_ms;
static int ser = -1;
static char sbuf[LINE]; static int slen;
static const char *tty = "/dev/ttyS3";
static int baud_c = B921600;
static int cam_ok;                   /* остання відповідь камери вдала */
static volatile sig_atomic_t quit;

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

static int open_serial(void) {
    int fd = open(tty, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct termios t;
    if (tcgetattr(fd, &t)) { close(fd); return -1; }
    cfmakeraw(&t);
    cfsetispeed(&t, baud_c); cfsetospeed(&t, baud_c);
    t.c_cflag |= CLOCAL | CREAD; t.c_cflag &= ~(CSTOPB | CRTSCTS | PARENB);
    tcsetattr(fd, TCSANOW, &t);
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static void cl_send(int i, const char *s, size_t n) {
    if (cl[i].fd < 0) return;
    if (send(cl[i].fd, s, n, MSG_NOSIGNAL | MSG_DONTWAIT) != (ssize_t)n) { close(cl[i].fd); cl[i].fd = -1; }
}
static int cl_find(int fd) { for (int i = 0; i < MAXC; i++) if (cl[i].fd == fd && fd >= 0) return i; return -1; }

/* --- мінімальний розбір JSON-рядка запиту --- */
static const char *skip_str(const char *p) { /* p на відкривній " ; повертає за закривною */
    for (p++; *p && *p != '"'; p++) if (*p == '\\' && p[1]) p++;
    return *p ? p + 1 : p;
}
static const char *find_key(const char *s, const char *key) { /* значення ключа верхнього рівня */
    int depth = 0; size_t kl = strlen(key);
    for (const char *p = s; *p; ) {
        if (*p == '"') {
            const char *e = skip_str(p);
            if (depth == 1 && (size_t)(e - p - 2) == kl && !strncmp(p + 1, key, kl)) {
                const char *v = e; while (*v == ' ') v++;
                if (*v == ':') { v++; while (*v == ' ') v++; return v; }
            }
            p = e; continue;
        }
        if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') depth--;
        p++;
    }
    return NULL;
}
static int obj_len(const char *v) { /* довжина {...} */
    int d = 0; const char *p = v;
    for (; *p; ) {
        if (*p == '"') { p = skip_str(p); continue; }
        if (*p == '{') d++;
        else if (*p == '}' && --d == 0) return (int)(p - v + 1);
        p++;
    }
    return -1;
}

static void reply_to(int ci, long id, int has_id, const char *body) { /* body — вміст без дужок, напр. "ok":true */
    char o[LINE + 64];
    int n = has_id ? snprintf(o, sizeof o, "{\"id\":%ld,%s}\n", id, body) : snprintf(o, sizeof o, "{%s}\n", body);
    if (n > 0 && n < (int)sizeof o) cl_send(ci, o, n);
}

static void start_next(void) {
    if (inflight || qn == 0 || ser < 0) return;
    char o[LINE + 2]; int n = snprintf(o, sizeof o, "%s\n", q[0].cmd);
    tcflush(ser, TCIFLUSH);
    if (write(ser, o, n) != n) { /* UART пішов — відповісти помилкою */ }
    inflight = 1; sent_ms = now_ms();
}
static void finish_job(const char *camreply, const char *err) {
    struct job j = q[0];
    memmove(q, q + 1, (--qn) * sizeof q[0]); inflight = 0;
    int ci = cl_find(j.cfd);
    if (ci >= 0) {
        if (camreply) { /* вставити "id" після першої '{' */
            char o[LINE + 64]; const char *b = strchr(camreply, '{');
            int n = b[1] == '}' ? snprintf(o, sizeof o, "{\"id\":%ld}\n", j.id)
                                : snprintf(o, sizeof o, "{\"id\":%ld,%s\n", j.id, b + 1);
            if (n > 0 && n < (int)sizeof o) cl_send(ci, o, n);
        } else {
            char b[96]; snprintf(b, sizeof b, "\"ok\":false,\"err\":\"%s\"", err); reply_to(ci, j.id, 1, b);
        }
    }
    start_next();
}

static void handle_line(int ci, char *line) {
    const char *op = find_key(line, "op"), *idv = find_key(line, "id");
    if (!op || *op != '"') return;
    long id = idv ? strtol(idv, NULL, 10) : 0; int has_id = idv != NULL;
    if (!strncmp(op, "\"cam\"", 5)) {
        const char *c = find_key(line, "cmd"); int n = c && *c == '{' ? obj_len(c) : -1;
        if (n < 0 || n >= LINE) { reply_to(ci, id, has_id, "\"ok\":false,\"err\":\"bad_cmd\""); return; }
        if (qn >= QMAX || ser < 0) { reply_to(ci, id, has_id, ser < 0 ? "\"ok\":false,\"err\":\"camera_lost\"" : "\"ok\":false,\"err\":\"busy\""); return; }
        q[qn].cfd = cl[ci].fd; q[qn].id = id; memcpy(q[qn].cmd, c, n); q[qn].cmd[n] = 0; qn++;
        start_next();
    } else if (!strncmp(op, "\"hello\"", 7) || !strncmp(op, "\"ping\"", 6) || !strncmp(op, "\"status\"", 8)) {
        reply_to(ci, id, has_id, "\"ok\":true,\"proto\":1,\"role\":\"rv1106\"");
    } else reply_to(ci, id, has_id, "\"ok\":false,\"err\":\"unknown_op\"");
}

static void serial_line(char *l) {
    if (l[0] != '{' || !inflight) return;          /* діагностичний текст / нічого не чекаємо */
    if (!find_key(l, "ok")) return;
    cam_ok = strstr(l, "\"ok\":false") == NULL && strstr(l, "\"ok\": false") == NULL;
    finish_job(l, NULL);
}

static void push_status(void) {
    struct sysinfo si; sysinfo(&si);
    double temp = -1; FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (f) { long m; if (fscanf(f, "%ld", &m) == 1) temp = m / 1000.0; fclose(f); }
    char o[200]; int n = snprintf(o, sizeof o, "{\"evt\":\"status\",\"cam\":\"%s\",\"temp_c\":%.1f,\"uptime\":%ld}\n",
                                  ser < 0 ? "lost" : (cam_ok ? "ok" : "idle"), temp, si.uptime);
    for (int i = 0; i < MAXC; i++) cl_send(i, o, n);
}

static void onsig(int s) { (void)s; quit = 1; }

int main(int argc, char **argv) {
    int port = 5001; const char *bind_ip = "0.0.0.0";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tty") && i + 1 < argc) tty = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bind") && i + 1 < argc) bind_ip = argv[++i];
        else if (!strcmp(argv[i], "--baud") && i + 1 < argc) { int b = atoi(argv[++i]); baud_c = b == 115200 ? B115200 : B921600; }
        else { fprintf(stderr, "usage: %s [--tty DEV] [--baud 921600|115200] [--port N] [--bind IP]\n", argv[0]); return 2; }
    }
    signal(SIGINT, onsig); signal(SIGTERM, onsig); signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < MAXC; i++) cl[i].fd = -1;
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(port);
    inet_pton(AF_INET, bind_ip, &a.sin_addr);
    if (bind(ls, (void *)&a, sizeof a) || listen(ls, 4)) { perror("bind/listen"); return 1; }
    fprintf(stderr, "rv1106_ctl: %s:%d <-> %s\n", bind_ip, port, tty);
    long long last_push = 0, last_open = 0;
    while (!quit) {
        long long t = now_ms();
        if (ser < 0 && t - last_open > 2000) { last_open = t; ser = open_serial(); slen = 0; }
        struct pollfd p[MAXC + 2]; int n = 0;
        p[n++] = (struct pollfd){ ls, POLLIN, 0 };
        p[n++] = (struct pollfd){ ser, POLLIN, 0 };           /* ser<0 ігнорується poll */
        for (int i = 0; i < MAXC; i++) p[n++] = (struct pollfd){ cl[i].fd, POLLIN, 0 };
        poll(p, n, 100);
        t = now_ms();
        if (p[0].revents & POLLIN) {
            int fd = accept(ls, NULL, NULL), i;
            for (i = 0; i < MAXC && fd >= 0; i++) if (cl[i].fd < 0) break;
            if (fd >= 0 && i < MAXC) {
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
                int idle = 5, intv = 2, cnt = 3;
                setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
                setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intv, sizeof intv);
                setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
                cl[i].fd = fd; cl[i].len = 0;
            } else if (fd >= 0) close(fd);
        }
        if (ser >= 0 && (p[1].revents & (POLLIN | POLLERR | POLLHUP))) {
            char b[512]; ssize_t r = read(ser, b, sizeof b);
            if (r <= 0 && errno != EAGAIN) { close(ser); ser = -1; cam_ok = 0; if (inflight) finish_job(NULL, "camera_lost"); }
            for (ssize_t k = 0; k < r; k++) {
                if (b[k] == '\n' || b[k] == '\r') { if (slen) { sbuf[slen] = 0; serial_line(sbuf); slen = 0; } }
                else if (slen < LINE - 1) sbuf[slen++] = b[k]; else slen = 0;
            }
        }
        for (int i = 0; i < MAXC; i++) {
            if (cl[i].fd < 0 || !(p[2 + i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t r = recv(cl[i].fd, cl[i].buf + cl[i].len, LINE - 1 - cl[i].len, 0);
            if (r <= 0) { close(cl[i].fd); cl[i].fd = -1; continue; }
            cl[i].len += r;
            char *s = cl[i].buf, *nl;
            while ((nl = memchr(s, '\n', cl[i].buf + cl[i].len - s))) { *nl = 0; handle_line(i, s); s = nl + 1; if (cl[i].fd < 0) break; }
            if (cl[i].fd >= 0) { cl[i].len -= s - cl[i].buf; memmove(cl[i].buf, s, cl[i].len); if (cl[i].len >= LINE - 1) cl[i].len = 0; }
        }
        for (int i = 0; i < qn; i++) if (cl_find(q[i].cfd) < 0 && i > 0) { memmove(q + i, q + i + 1, (--qn - i) * sizeof q[0]); i--; }
        if (inflight && t - sent_ms > CAM_TIMEOUT_MS) { cam_ok = 0; finish_job(NULL, "camera_timeout"); }
        if (t - last_push >= 1000) { last_push = t; push_status(); }
    }
    return 0;
}
