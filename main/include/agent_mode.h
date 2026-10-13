#pragma once
#include <stdbool.h>
#include "esp_err.h"
typedef struct {
    char status[96], user[512], assistant[1024];
    unsigned revision;
    bool awake;
} agent_snapshot_t;
esp_err_t agent_mode_start(void);
void agent_mode_enable(bool enabled);
void agent_mode_snapshot(agent_snapshot_t *out);
