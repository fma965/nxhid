#pragma once
#include <stdbool.h>
#include <stdint.h>

#define NXHID_PORT 24872
#define NXHID_LINE_SIZE 96
#define NXHID_TIMEOUT_MS 3000

typedef enum { CMD_INVALID, CMD_PING, CMD_REPORT, CMD_LEDS } CommandType;
typedef struct {
    CommandType type;
    uint8_t report[8];
} Command;

Command protocol_parse(const char *line);
