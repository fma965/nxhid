#include "protocol.h"
#include <string.h>

static int unhex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Command protocol_parse(const char *line) {
    Command cmd = {0};
    size_t len = strlen(line);
    if (!strcmp(line, "PING")) {
        cmd.type = CMD_PING;
    } else if (!strcmp(line, "LEDS")) {
        cmd.type = CMD_LEDS;
    } else if (!strcmp(line, "RELEASE")) {
        cmd.type = CMD_REPORT;
    } else if (len == 23 && !memcmp(line, "REPORT ", 7)) {
        for (int i = 0; i < 8; i++) {
            int hi = unhex(line[7 + 2*i]), lo = unhex(line[8 + 2*i]);
            if (hi < 0 || lo < 0) return (Command){0};
            cmd.report[i] = (uint8_t)((hi << 4) | lo);
        }
        if (cmd.report[1]) return (Command){0};
        for (int i = 2; i < 8; i++) {
            unsigned key = cmd.report[i];
            if (key && (key < 4 || key > 0x73)) return (Command){0};
            for (int j = 2; key && j < i; j++)
                if (cmd.report[j] == key) return (Command){0};
        }
        cmd.type = CMD_REPORT;
    }
    return cmd;
}

