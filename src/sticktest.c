// [advanced] stick_test=1: measure how the lying-down aim responds to the
// right stick, so gyro can be converted into the right amount of stick.
//
// The first time a player's camera enters mode 1 / state 1 (lying on the
// ground), this plays a fixed sequence of right-stick deflections on that
// player's controller and records the camera angles every frame to
// re6gyro_sticktest.csv. The player should keep their hands off the right
// stick for the ~25 s it takes.
#include "common.h"
#include <stdio.h>
#include <string.h>

static const float k_amp[] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.6f, 0.8f, 1.0f };
#define N_AMP   (int)(sizeof k_amp / sizeof k_amp[0])
#define LEAD    2.0f   // seconds of no input before the sequence
#define STEP    0.5f   // seconds per deflection
#define REST    0.25f  // seconds of no input after each deflection
#define PER_AMP (2 * (STEP + REST))
#define PER_AX  (N_AMP * PER_AMP)

static FILE *g_f;
static int g_port = -1, g_done;
static double g_t;
static DWORD g_t0;

// Stick command at time t into the sequence; returns 0 when the sequence is over.
static int command(double t, float *rx, float *ry)
{
    *rx = *ry = 0;
    if (t < LEAD) return 1;
    t -= LEAD;
    if (t >= 2 * PER_AX) return 0;
    int axis = t >= PER_AX;
    if (axis) t -= PER_AX;
    int a = (int)(t / PER_AMP);
    double u = t - a * PER_AMP;
    float v = 0;
    if (u < STEP) v = k_amp[a];
    else if (u >= STEP + REST && u < 2 * STEP + REST) v = -k_amp[a];
    if (axis) *ry = v; else *rx = v;
    return 1;
}

int stick_test_frame(int port, void *cam, void *pl, int mode, int state, float dt)
{
    if (!g_cfg.stick_test || g_done) return 0;
    if (g_port >= 0 && port != g_port) return 0;
    int lying = mode == 1 && state == 1;

    if (g_port < 0) {
        if (!lying) return 0;
        char path[MAX_PATH];
        extern HMODULE g_self;
        GetModuleFileNameA(g_self, path, MAX_PATH);
        char *slash = strrchr(path, '\\');
        if (slash) slash[1] = 0;
        strcat(path, "re6gyro_sticktest.csv");
        g_f = fopen(path, "w");
        if (!g_f) { g_done = 1; return 0; }
        fputs("t,dt,mode,state,rx,ry,cam_pitch,cam_yaw,pl_4ed4,pl_4ed8\n", g_f);
        g_port = port;
        g_t0 = GetTickCount();
        logf_("stick test: started on port %d - keep the right stick still", port);
    }

    g_t = (GetTickCount() - g_t0) / 1000.0;
    float rx, ry;
    int running = lying && command(g_t, &rx, &ry);
    float *c = (float *)((uint8_t *)cam + 0x290);
    float *p = (float *)((uint8_t *)pl + 0x4ed4);
    if (g_f)
        fprintf(g_f, "%.4f,%.4f,%d,%d,%.2f,%.2f,%.5f,%.5f,%.5f,%.5f\n", g_t, dt, mode, state,
                running ? rx : 0, running ? ry : 0, c[0], c[1], p[0], p[1]);
    if (!running) {
        logf_("stick test: %s after %.1f s", lying ? "finished" : "stopped (player got up)", g_t);
        if (g_f) { fclose(g_f); g_f = NULL; }
        g_done = 1;
        return 0;
    }
    g_stick[port].rx = rx;
    g_stick[port].ry = ry;
    g_stick[port].ms = GetTickCount();
    return 1;
}
