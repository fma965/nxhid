#include "server.h"
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>

static FILE *log_file;
static volatile sig_atomic_t running = 1;
static bool fail_nonzero;
static const char *leds_path;
static bool caps_held;
static void stop(int sig) { (void)sig; running = 0; }
static uint8_t read_leds(void) {
    FILE *file = fopen(leds_path, "r");
    unsigned value = 0;
    if (file) {
        if (fscanf(file, "%u", &value) != 1) value = 0;
        fclose(file);
    }
    return (uint8_t)value;
}
static bool write_report(const uint8_t report[8]) {
    unsigned nonzero = 0;
    for (int i = 0; i < 8; i++) {
        fprintf(log_file, "%02x", report[i]);
        nonzero |= report[i];
    }
    fputc('\n', log_file);
    fflush(log_file);
    if (fail_nonzero && nonzero) return false;
    bool caps = false;
    for (int i = 2; i < 8; i++) caps |= report[i] == 57;
    if (caps && !caps_held) {
        unsigned leds = read_leds() ^ 2;
        FILE *file = fopen(leds_path, "w");
        if (!file) return false;
        fprintf(file, "%u", leds);
        fclose(file);
    }
    caps_held = caps;
    return true;
}

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    log_file = fopen(argv[1], "w");
    if (!log_file) return 3;
    fail_nonzero = argv[2][0] == 'f';
    leds_path = argv[3];
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    Server s;
    if (!server_init(&s, 0, write_report, read_leds)) { perror("server_init"); return 4; }
    struct sockaddr_in address;
    socklen_t size = sizeof(address);
    if (getsockname(s.listener, (struct sockaddr *)&address, &size)) return 5;
    printf("%u\n", ntohs(address.sin_port));
    fflush(stdout);
    while (running) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        server_tick(&s, now, true);
        nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL);
    }
    server_close(&s);
    fclose(log_file);
    return 0;
}
