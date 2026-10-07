/* http_sim.c - serves the real dashboard and the real API bodies on the host.
 *
 * The response bodies come from core/src/api.c, the same function the ESP32's
 * httpd handlers call, so what you see in the browser here is byte-identical
 * to what the device will serve. Only the transport is different: BSD sockets
 * and one pthread instead of esp_http_server and a FreeRTOS task.
 *
 * The sampling loop runs on its own thread and the snapshot is taken under a
 * mutex through the same aeris_app_set_lock() hook the firmware uses, so the
 * locking discipline is exercised here too.
 */
#include <arpa/inet.h>
#include <inttypes.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "aeris/api.h"
#include "harness.h"

static harness_t      g_h;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int    g_stop = 0;
static char           *g_page;      /* dashboard.html, read once at start */
static size_t          g_page_len;

static void sim_lock(void *ctx, bool acquire)
{
    (void)ctx;
    if (acquire) pthread_mutex_lock(&g_lock);
    else         pthread_mutex_unlock(&g_lock);
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    *len = got;
    return buf;
}

static void send_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return;
        p += w; n -= (size_t)w;
    }
}

static void respond(int fd, const char *status, const char *ctype,
                    const char *body, size_t len)
{
    char hdr[256];
    int hn = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        status, ctype, len);
    send_all(fd, hdr, (size_t)hn);
    if (len) send_all(fd, body, len);
}

/* The sampling thread: exactly the loop the firmware's sensor task runs. */
static void *sample_thread(void *arg)
{
    (void)arg;
    while (!g_stop) {
        if (!harness_tick(&g_h)) {
            /* Scenario exhausted -- start another so the demo keeps going. */
            pthread_mutex_lock(&g_lock);
            scenario_random(&g_h.sc, &g_h.room, 20);
            g_h.sc.started = false;
            pthread_mutex_unlock(&g_lock);
        }
    }
    return NULL;
}

static void handle(int fd)
{
    char req[2048];
    ssize_t n = read(fd, req, sizeof req - 1);
    if (n <= 0) return;
    req[n] = '\0';

    char method[8] = {0}, path[256] = {0};
    if (sscanf(req, "%7s %255s", method, path) != 2) {
        respond(fd, "400 Bad Request", "text/plain", "bad request", 11);
        return;
    }
    if (strcmp(method, "GET")) {
        respond(fd, "405 Method Not Allowed", "text/plain", "GET only", 8);
        return;
    }

    static char buf[AERIS_API_HISTORY_BUF];
    size_t len = 0;

    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        /* Re-read from disk on every request. On the device the dashboard is
         * embedded in flash, but here editing web/dashboard.html and hitting
         * reload should just work -- caching it at startup turned every CSS
         * tweak into a restart. */
        size_t page_len = 0;
        char *page = read_file("web/dashboard.html", &page_len);
        if (page) {
            respond(fd, "200 OK", "text/html; charset=utf-8", page, page_len);
            free(page);
        } else {
            respond(fd, "200 OK", "text/html; charset=utf-8", g_page, g_page_len);
        }
        return;
    }
    if (!strcmp(path, "/api/live")) {
        aeris_snapshot_t s;
        aeris_app_snapshot(&g_h.app, &s);
        len = aeris_api_live(&s, buf, sizeof buf);
    } else if (!strcmp(path, "/api/history")) {
        pthread_mutex_lock(&g_lock);
        len = aeris_api_history(&g_h.app.hist, buf, sizeof buf);
        pthread_mutex_unlock(&g_lock);
    } else if (!strcmp(path, "/api/status")) {
        aeris_snapshot_t s;
        aeris_app_snapshot(&g_h.app, &s);
        pthread_mutex_lock(&g_lock);
        len = aeris_api_status(&s, &g_h.app, "aeris-sim", buf, sizeof buf);
        pthread_mutex_unlock(&g_lock);
    } else if (!strcmp(path, "/api/debug")) {
        aeris_snapshot_t s;
        aeris_app_snapshot(&g_h.app, &s);
        len = aeris_api_debug(&s, buf, sizeof buf);
    } else {
        respond(fd, "404 Not Found", "text/plain", "not found", 9);
        return;
    }

    if (len == 0) {
        respond(fd, "500 Internal Server Error", "text/plain",
                "response buffer overflow", 24);
        return;
    }
    respond(fd, "200 OK", "application/json", buf, len);
}

static void on_sigint(int s) { (void)s; g_stop = 1; }

int sim_serve(int port, int speed);

int sim_serve(int port, int speed)
{
    g_page = read_file("web/dashboard.html", &g_page_len);
    if (!g_page) {
        fprintf(stderr, "cannot read web/dashboard.html "
                        "(run make serve from the repo root)\n");
        return 1;
    }

    harness_init(&g_h, 0xA17157u);
    /* speed 1 is real time; larger values compress it, so --speed 60 shows a
     * minute of room behaviour every second and a cooking event plays out while
     * you watch. */
    g_h.bus.time_speedup = (uint32_t)(speed > 0 ? speed : 1);
    aeris_app_set_lock(&g_h.app, sim_lock, NULL);
    scenario_random(&g_h.sc, &g_h.room, 24);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); return 1; }
    if (listen(srv, 8) < 0) { perror("listen"); return 1; }

    signal(SIGINT, on_sigint);
    signal(SIGPIPE, SIG_IGN);

    pthread_t th;
    pthread_create(&th, NULL, sample_thread, NULL);

    printf("Aeris simulator on http://localhost:%d  "
           "(virtual time x%" PRIu32 ", Ctrl-C to stop)\n",
           port, g_h.bus.time_speedup);

    while (!g_stop) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) break; continue; }
        handle(fd);
        close(fd);
    }

    g_stop = 1;
    pthread_join(th, NULL);
    close(srv);
    free(g_page);
    return 0;
}
