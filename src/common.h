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
    int   stick_test;        // measure stick->aim response while lying down
    float lie_rate_yaw;      // aim rad/s at full right-stick deflection, lying down
    float lie_rate_pitch;
    float lie_deadzone;      // game's stick deadzone as a fraction of full deflection
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

// Extra right-stick input added by the XInput proxy (fraction of full, -1..1).
typedef struct { volatile float rx, ry; volatile DWORD ms; } stick_inject_t;
extern stick_inject_t g_stick[MAX_PORTS];
int stick_test_frame(int port, void *cam, void *pl, int mode, int state, float dt);
extern hook_seen_t g_seen[MAX_PORTS];
void debug_poll(void);
