#pragma once

#include <stdint.h>

#define MAX_AGENTS 16  // must match MAX_AGENTS in bridge/speeddial_bridge.py

enum AgentStatus : uint8_t {
    STATUS_UNKNOWN,
    STATUS_IDLE,
    STATUS_WORKING,
    STATUS_BLOCKED,
    STATUS_DONE,
};

struct Agent {
    char id[24];    // herdr pane id, e.g. "w1:p2"
    char name[52];  // terminal title, or cwd basename
    char kind[16];  // "claude", "codex", ...
    AgentStatus status;
    bool focused;   // currently focused in herdr
};

enum UiAlert {
    ALERT_NONE,
    ALERT_DONE,     // an agent just finished
    ALERT_BLOCKED,  // an agent just started waiting on you
};

void ui_init(void (*on_tap)(const char *agent_id));

// Replaces the agent list. Returns the most urgent transition it saw, so the caller can buzz.
// `error` is non-null when the bridge is up but cannot reach herdr.
UiAlert ui_set_agents(const Agent *agents, int count, const char *error);

void ui_rotate(int steps);
void ui_set_offline(bool offline);

// Small status line at the top of the dial (link and battery). `warn` shows it in red.
void ui_set_status(const char *text, bool warn);
