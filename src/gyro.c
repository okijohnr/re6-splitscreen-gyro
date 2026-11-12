// Steam Input motion reader + gyro processing.
#include "common.h"
#include <math.h>
#include <string.h>
#include "steaminput.h"
#include <stdio.h>

static FILE *g_rec;

#define DEG2RAD 0.017453293f

typedef struct {
    float yaw, pitch;         // accumulated radians not yet applied
    DWORD last_take;          // GetTickCount of last drain by the camera hook
} accum_t;

static CRITICAL_SECTION g_cs;
static accum_t g_acc[MAX_PORTS];

void gyro_take(int port, float *dyaw, float *dpitch)
{
    EnterCriticalSection(&g_cs);
    *dyaw = g_acc[port].yaw;
    *dpitch = g_acc[port].pitch;
    g_acc[port].yaw = g_acc[port].pitch = 0;
    g_acc[port].last_take = GetTickCount();
    LeaveCriticalSection(&g_cs);
}

static void gyro_add(int port, float dyaw, float dpitch)
{
    EnterCriticalSection(&g_cs);
    // Only accumulate while the camera hook is actively consuming, so motion
    // during menus / cutscenes doesn't snap the camera when control returns.
    if (GetTickCount() - g_acc[port].last_take < 150) {
        g_acc[port].yaw += dyaw;
        g_acc[port].pitch += dpitch;
    } else {
        g_acc[port].yaw = g_acc[port].pitch = 0;
    }
    LeaveCriticalSection(&g_cs);
}

// Per-controller processing state.
typedef struct {
    float bias[3];       // raw-unit gyro bias
    float grav[3];       // smoothed accelerometer (gravity direction)
    float still_time;    // seconds the controller has been still
    int   have_grav;
    // calibration-of-units diagnostics
    double int_counts;   // integrated |rotVel| * dt
    float q_prev[4];
    double q_angle;      // integrated quaternion rotation
} ctl_state_t;

static ctl_state_t g_ctl[STEAM_INPUT_MAX_COUNT];

typedef HSteamUser (*pfnGetHUser)(void);
typedef HSteamPipe (*pfnGetHPipe)(void);
typedef void *(*pfnSteamClient)(void);

static float qangle(const float a[4], const float b[4])
{
    float d = fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    if (d > 1.0f) d = 1.0f;
    return 2.0f * acosf(d);
}

static void process(int i, int port, const InputMotionData_t *m, float dt, int paused)
{
    ctl_state_t *c = &g_ctl[i];
    float raw[3] = { m->rotVelX, m->rotVelY, m->rotVelZ };
    float acc[3] = { m->posAccelX, m->posAccelY, m->posAccelZ };

    // gravity low-pass
    if (!c->have_grav) { memcpy(c->grav, acc, sizeof acc); c->have_grav = 1; }
    float k = dt / (0.1f + dt);
    for (int a = 0; a < 3; a++) c->grav[a] += (acc[a] - c->grav[a]) * k;

    // bias-corrected angular velocity in deg/s
    float w[3];
    for (int a = 0; a < 3; a++) w[a] = (raw[a] - c->bias[a]) / g_cfg.counts_per_dps;
    float wmag = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);

    // continuous calibration: when still for a moment, track the bias
    if (wmag < g_cfg.still_dps) {
        c->still_time += dt;
        if (c->still_time > 0.5f) {
            float kb = dt / (1.0f + dt);
            for (int a = 0; a < 3; a++) c->bias[a] += (raw[a] - c->bias[a]) * kb;
        }
    } else {
        c->still_time = 0;
    }

    // diagnostics for unit calibration
    c->int_counts += sqrt((double)raw[0] * raw[0] + (double)raw[1] * raw[1] + (double)raw[2] * raw[2]) * dt;
    float q[4] = { m->rotQuatX, m->rotQuatY, m->rotQuatZ, m->rotQuatW };
    c->q_angle += qangle(c->q_prev, q);
    memcpy(c->q_prev, q, sizeof q);

    if (port < 0 || port >= MAX_PORTS || paused || !g_cfg.enabled) return;
    if (wmag < g_cfg.deadzone_dps) return;

    // Steam Input axes: X = right, Y = forward, Z = up (controller flat).
    float yaw_dps, pitch_dps = w[0];
    if (g_cfg.player_space) {
        // Player-space yaw (GyroWiki / JoyShockMapper): rotate around world-up
        // using gravity, with a relax factor so yaw still works when tilted.
        float gx = c->grav[0], gy = c->grav[1], gz = c->grav[2];
        float gl = sqrtf(gx * gx + gy * gy + gz * gz);
        if (gl > 1e-3f) { gx /= gl; gy /= gl; gz /= gl; } else { gx = 0; gy = 0; gz = 1; }
        float world_yaw = w[1] * gy + w[2] * gz;
        float relax = 1.41f;
        float lim = sqrtf(w[1] * w[1] + w[2] * w[2]);
        float mag = fabsf(world_yaw) * relax;
        yaw_dps = copysignf(mag < lim ? mag : lim, world_yaw);
    } else {
        yaw_dps = w[2];
    }

    float s = g_cfg.sensitivity * DEG2RAD * dt;
    float dyaw = yaw_dps * s * (g_cfg.invert_yaw ? -1.f : 1.f);
    float dpitch = pitch_dps * s * g_cfg.sensitivity_pitch * (g_cfg.invert_pitch ? -1.f : 1.f);
    gyro_add(port, dyaw, dpitch);
}

static DWORD WINAPI gyro_thread(LPVOID unused)
{
    (void)unused;
    HMODULE sapi = NULL;
    pfnSteamClient pSteamClient = NULL;
    pfnGetHUser pUser = NULL;
    pfnGetHPipe pPipe = NULL;

    for (int tries = 0; tries < 1200; tries++) {
        sapi = GetModuleHandleA("steam_api.dll");
        if (sapi) {
            pSteamClient = (pfnSteamClient)(void *)GetProcAddress(sapi, "SteamClient");
            pUser = (pfnGetHUser)(void *)GetProcAddress(sapi, "SteamAPI_GetHSteamUser");
            pPipe = (pfnGetHPipe)(void *)GetProcAddress(sapi, "SteamAPI_GetHSteamPipe");
            if (pSteamClient && pUser && pPipe && pSteamClient() && pUser() && pPipe()) break;
        }
        Sleep(100);
    }
    void *client = pSteamClient ? pSteamClient() : NULL;
    HSteamUser hu = pUser ? pUser() : 0;
    HSteamPipe hp = pPipe ? pPipe() : 0;
    if (!client || !hu || !hp) { logf_("ERROR: Steam API not initialised - no gyro"); return 0; }

    void *si = ((pfnGetGenericInterface)VT(client, SC_GetISteamGenericInterface))(client, hu, hp, "SteamInput006");
    if (!si) { logf_("ERROR: SteamInput006 unavailable - Steam client too old?"); return 0; }
    uint8_t ok = ((pfnSI_Init)VT(si, SI_Init))(si, 1);
    logf_("Steam Input ready (Init=%d)", ok);

    LARGE_INTEGER freq, prev, now, last_report;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    last_report = prev;
    int last_n = -1;
    InputHandle_t last_handles[STEAM_INPUT_MAX_COUNT] = {0};

    for (;;) {
        config_maybe_reload();
        ((pfnSI_RunFrame)VT(si, SI_RunFrame))(si, 1);
        InputHandle_t h[STEAM_INPUT_MAX_COUNT];
        int n = ((pfnSI_GetConnected)VT(si, SI_GetConnectedControllers))(si, h);
        if (n < 0) n = 0;
        if (n > STEAM_INPUT_MAX_COUNT) n = STEAM_INPUT_MAX_COUNT;

        int changed = n != last_n;
        for (int i = 0; i < n && !changed; i++) changed = h[i] != last_handles[i];
        if (changed) {
            logf_("controllers: %d", n);
            for (int i = 0; i < n; i++) {
                int type = ((pfnSI_GetInputType)VT(si, SI_GetInputTypeForHandle))(si, h[i]);
                int pad = ((pfnSI_GetGamepadIndex)VT(si, SI_GetGamepadIndexForController))(si, h[i]);
                logf_("  [%d] %s -> xinput port %d", i, steam_input_type_name(type), pad);
                if (h[i] != last_handles[i]) memset(&g_ctl[i], 0, sizeof g_ctl[i]);
            }
            memcpy(last_handles, h, sizeof h);
            last_n = n;
        }

        QueryPerformanceCounter(&now);
        float dt = (float)((double)(now.QuadPart - prev.QuadPart) / freq.QuadPart);
        prev = now;
        if (dt > 0.05f) dt = 0.05f;

        // raw sample recording for calibration ([advanced] record=1)
        if (g_cfg.record && !g_rec) {
            char path[MAX_PATH];
            extern HMODULE g_self;
            GetModuleFileNameA(g_self, path, MAX_PATH);
            char *slash = strrchr(path, '\\');
            if (slash) slash[1] = 0;
            strcat(path, "re6gyro_record.csv");
            g_rec = fopen(path, "w");
            if (g_rec) fputs("t,ctl,port,rx,ry,rz,ax,ay,az,qx,qy,qz,qw\n", g_rec);
            logf_("recording raw gyro to %s", path);
        } else if (!g_cfg.record && g_rec) {
            fclose(g_rec); g_rec = NULL;
            logf_("recording stopped");
        }
        double tnow = (double)now.QuadPart / freq.QuadPart;

        for (int i = 0; i < n; i++) {
            InputMotionData_t m;
            ((pfnSI_GetMotion)VT(si, SI_GetMotionData))(si, &m, h[i]);
            int port = ((pfnSI_GetGamepadIndex)VT(si, SI_GetGamepadIndexForController))(si, h[i]);
            if (g_rec)
                fprintf(g_rec, "%.6f,%d,%d,%.1f,%.1f,%.1f,%.0f,%.0f,%.0f,%.6f,%.6f,%.6f,%.6f\n", tnow, i, port,
                        m.rotVelX, m.rotVelY, m.rotVelZ, m.posAccelX, m.posAccelY, m.posAccelZ,
                        m.rotQuatX, m.rotQuatY, m.rotQuatZ, m.rotQuatW);
            int paused = 0;
            if (port >= 0 && port < MAX_PORTS && g_cfg.disable_button)
                paused = (g_buttons[port] & g_cfg.disable_button) != 0;
            process(i, port, &m, dt, paused);
        }

        double since = (double)(now.QuadPart - last_report.QuadPart) / freq.QuadPart;
        if (since >= 2.0) {
            if (g_cfg.log_level >= 1) {
                for (int i = 0; i < n; i++) {
                    ctl_state_t *c = &g_ctl[i];
                    double ratio = c->q_angle > 0.5 ? c->int_counts / (c->q_angle * 57.29578) : 0;
                    logf_("ctl%d bias=(%.1f %.1f %.1f) grav=(%.0f %.0f %.0f) moved=%.0fdeg counts/dps~%.2f",
                          i, c->bias[0], c->bias[1], c->bias[2], c->grav[0], c->grav[1], c->grav[2],
                          c->q_angle * 57.29578, ratio);
                    c->int_counts = 0; c->q_angle = 0;
                }
                LONG o = InterlockedExchange(&g_hook_calls_other, 0);
                for (int p = 0; p < MAX_PORTS; p++) {
                    LONG k = InterlockedExchange(&g_hook_calls[p], 0);
                    if (k) logf_("camera port%d: %ld updates/2s", p, k);
                }
                if (o) logf_("camera (no port): %ld updates/2s", o);
            }
            last_report = now;
        }
        debug_poll();
        Sleep(2);
    }
    return 0;
}

void gyro_start(void)
{
    InitializeCriticalSection(&g_cs);
    CloseHandle(CreateThread(NULL, 0, gyro_thread, NULL, 0, NULL));
}
