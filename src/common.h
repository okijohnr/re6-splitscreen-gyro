// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 okijohnr
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

#define MAX_PORTS 4

// ---------------------------------------------------------------- config
typedef struct {
    int   enabled;
    float sensitivity;       // camera radians per controller radian (yaw)
    float sensitivity_pitch; // multiplier relative to sensitivity
    float sensitivity_swim;  // multiplier while swimming (the game slows the stick there too)
    float swim_max_dps;      // turn-speed cap while swimming, deg/s (0 = none)
    int   invert_yaw;
    int   invert_pitch;
    int   player_space;      // 1 = gravity-aware yaw (recommended), 0 = local Z axis
    float counts_per_dps;    // Steam Input rotVel units per degree/second
    float deadzone_dps;      // tiny-motion cut-off (deg/s)
    float still_dps;         // below this (after bias) controller counts as still for calibration
    WORD  disable_button;    // XInput button mask; while held, gyro is paused (0 = none)
    int   log_level;
    int   record;            // write raw samples to re6gyro_record.csv
    int   debug;             // log camera state changes / hook gaps
} config_t;

extern config_t g_cfg;
void config_load(void);
void config_maybe_reload(void);

// ---------------------------------------------------------------- log
void logf_(const char *fmt, ...);

// ---------------------------------------------------------------- shared gyro state
// Accumulated camera rotation (radians) waiting to be applied, per XInput port.
void gyro_take(int port, float *dyaw, float *dpitch);
void gyro_start(void);

// Latest XInput buttons per port, captured by the proxy.
extern volatile WORD g_buttons[MAX_PORTS];

// Live state for external debugging tools (found by its magic string).
typedef struct {
    char magic[8];                     // "RE6GDBG"
    volatile short rx[MAX_PORTS], ry[MAX_PORTS];  // right stick per controller
    volatile DWORD now_ms;             // GetTickCount when last updated
    volatile DWORD cam_ms[MAX_PORTS];  // last camera-hook run per player slot
    volatile DWORD aim_ms[MAX_PORTS];  // last character-aim (0xabaa40) run per slot
    void *volatile pl[MAX_PORTS];      // player per slot
    void *volatile pad[MAX_PORTS];     // cPadEx per slot
} dbg_shared_t;
extern dbg_shared_t g_dbg;

// ---------------------------------------------------------------- hook
int hook_install(void);
extern volatile LONG g_hook_calls[MAX_PORTS];
extern volatile LONG g_hook_calls_other;

// Per-port info captured by the hook, for debug logging.
typedef struct {
    void *volatile cam;          // uCameraQFPS* last seen for this port
    void *volatile pl;           // uPlayer*
    volatile DWORD ret;          // caller return address of the last hook call
    volatile DWORD last_ms;      // GetTickCount of the last hook call
    volatile int mode, state;    // camera mode/state from the angle-update hook
} hook_seen_t;

extern hook_seen_t g_seen[MAX_PORTS];
void debug_poll(void);

#define MAX_STICK_SITES 64
typedef struct { volatile DWORD ret; int port; volatile DWORD last_ms; volatile LONG calls; } stick_site_t;
extern stick_site_t g_stick_sites[MAX_STICK_SITES];
