// RE6 gyro mod - stage 1 probe.
// Proxy xinput1_3.dll: forwards XInput to the real system DLL and, on a worker
// thread, reads per-controller gyro from Steam Input and logs it.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include "steaminput.h"

// ---------------------------------------------------------------- XInput types
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XINPUT_GAMEPAD;
typedef struct { DWORD dwPacketNumber; XINPUT_GAMEPAD Gamepad; } XINPUT_STATE;

typedef DWORD (WINAPI *pfnGetState)(DWORD, XINPUT_STATE *);
typedef DWORD (WINAPI *pfnGeneric2)(DWORD, void *);
typedef DWORD (WINAPI *pfnGeneric3)(DWORD, DWORD, void *);
typedef void  (WINAPI *pfnEnable)(BOOL);
typedef DWORD (WINAPI *pfnDSound)(DWORD, GUID *, GUID *);
typedef DWORD (WINAPI *pfnBattery)(DWORD, BYTE, void *);

static HMODULE g_real;
static pfnGetState r_GetState, r_GetStateEx;
static pfnGeneric2 r_SetState;
static pfnGeneric3 r_GetCaps, r_GetKeystroke;
static pfnEnable r_Enable;
static pfnDSound r_DSound;
static pfnBattery r_Battery;

// ---------------------------------------------------------------- logging
static FILE *g_log;
static CRITICAL_SECTION g_logcs;

static void logf_(const char *fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_logcs);
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logcs);
}

static void open_log(HMODULE self)
{
    char path[MAX_PATH];
    GetModuleFileNameA(self, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    if (slash) slash[1] = 0;
    strcat(path, "re6gyro_log.txt");
    InitializeCriticalSection(&g_logcs);
    g_log = fopen(path, "w");
}

// ---------------------------------------------------------------- real xinput
static void load_real(void)
{
    if (g_real) return;
    char path[MAX_PATH];
    const char *names[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (int i = 0; i < 3 && !g_real; i++) {
        GetSystemDirectoryA(path, MAX_PATH);
        strcat(path, "\\");
        strcat(path, names[i]);
        g_real = LoadLibraryA(path);
        if (g_real) logf_("loaded real xinput: %s", path);
    }
    if (!g_real) { logf_("ERROR: could not load a real xinput dll"); return; }
    r_GetState     = (pfnGetState)GetProcAddress(g_real, "XInputGetState");
    r_GetStateEx   = (pfnGetState)GetProcAddress(g_real, (LPCSTR)100);
    r_SetState     = (pfnGeneric2)GetProcAddress(g_real, "XInputSetState");
    r_GetCaps      = (pfnGeneric3)GetProcAddress(g_real, "XInputGetCapabilities");
    r_GetKeystroke = (pfnGeneric3)GetProcAddress(g_real, "XInputGetKeystroke");
    r_Enable       = (pfnEnable)GetProcAddress(g_real, "XInputEnable");
    r_DSound       = (pfnDSound)GetProcAddress(g_real, "XInputGetDSoundAudioDeviceGuids");
    r_Battery      = (pfnBattery)GetProcAddress(g_real, "XInputGetBatteryInformation");
}

// ---------------------------------------------------------------- probe state
static volatile LONG g_getstate_calls[4];
static volatile LONG g_getstate_ok[4];
static XINPUT_GAMEPAD g_last_pad[4];

static void note_state(DWORD idx, DWORD rc, XINPUT_STATE *s)
{
    if (idx >= 4) return;
    InterlockedIncrement(&g_getstate_calls[idx]);
    if (rc == 0 && s) { InterlockedIncrement(&g_getstate_ok[idx]); g_last_pad[idx] = s->Gamepad; }
}

// ---------------------------------------------------------------- exports
DWORD WINAPI XInputGetState(DWORD i, XINPUT_STATE *s)
{
    load_real();
    DWORD rc = r_GetState ? r_GetState(i, s) : ERROR_DEVICE_NOT_CONNECTED;
    note_state(i, rc, s);
    return rc;
}
DWORD WINAPI XInputGetStateEx(DWORD i, XINPUT_STATE *s)
{
    load_real();
    DWORD rc = r_GetStateEx ? r_GetStateEx(i, s) : (r_GetState ? r_GetState(i, s) : ERROR_DEVICE_NOT_CONNECTED);
    note_state(i, rc, s);
    return rc;
}
DWORD WINAPI XInputSetState(DWORD i, void *v) { load_real(); return r_SetState ? r_SetState(i, v) : ERROR_DEVICE_NOT_CONNECTED; }
DWORD WINAPI XInputGetCapabilities(DWORD i, DWORD f, void *c) { load_real(); return r_GetCaps ? r_GetCaps(i, f, c) : ERROR_DEVICE_NOT_CONNECTED; }
DWORD WINAPI XInputGetKeystroke(DWORD i, DWORD r, void *k) { load_real(); return r_GetKeystroke ? r_GetKeystroke(i, r, k) : ERROR_DEVICE_NOT_CONNECTED; }
void  WINAPI XInputEnable(BOOL e) { load_real(); if (r_Enable) r_Enable(e); }
DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD i, GUID *a, GUID *b) { load_real(); return r_DSound ? r_DSound(i, a, b) : ERROR_DEVICE_NOT_CONNECTED; }
DWORD WINAPI XInputGetBatteryInformation(DWORD i, BYTE t, void *b) { load_real(); return r_Battery ? r_Battery(i, t, b) : ERROR_DEVICE_NOT_CONNECTED; }

// ---------------------------------------------------------------- steam input probe
typedef HSteamUser (*pfnGetHUser)(void);
typedef HSteamPipe (*pfnGetHPipe)(void);
typedef void *(*pfnSteamClient)(void);

static DWORD WINAPI probe_thread(LPVOID unused)
{
    (void)unused;
    HMODULE sapi = NULL;
    pfnSteamClient pSteamClient = NULL;
    pfnGetHUser pUser = NULL;
    pfnGetHPipe pPipe = NULL;

    // Wait for the game to load and initialise steam_api.
    for (int tries = 0; tries < 600; tries++) {
        sapi = GetModuleHandleA("steam_api.dll");
        if (sapi) {
            pSteamClient = (pfnSteamClient)GetProcAddress(sapi, "SteamClient");
            pUser = (pfnGetHUser)GetProcAddress(sapi, "SteamAPI_GetHSteamUser");
            pPipe = (pfnGetHPipe)GetProcAddress(sapi, "SteamAPI_GetHSteamPipe");
            if (pSteamClient && pUser && pPipe && pSteamClient() && pUser() && pPipe()) break;
        }
        Sleep(100);
    }
    void *client = pSteamClient ? pSteamClient() : NULL;
    HSteamUser hu = pUser ? pUser() : 0;
    HSteamPipe hp = pPipe ? pPipe() : 0;
    logf_("steam_api=%p client=%p user=%d pipe=%d", sapi, client, hu, hp);
    if (!client || !hu || !hp) { logf_("ERROR: steam not initialised, giving up"); return 0; }

    void *si = NULL;
    const char *versions[] = { "SteamInput006", "SteamInput005", "SteamInput002", "SteamInput001" };
    const char *got = NULL;
    for (int i = 0; i < 4 && !si; i++) {
        si = ((pfnGetGenericInterface)VT(client, SC_GetISteamGenericInterface))(client, hu, hp, versions[i]);
        got = versions[i];
    }
    logf_("ISteamInput=%p (%s)", si, si ? got : "none");
    if (!si) return 0;
    // NOTE: vtable indices below are for SteamInput006. If an older version was
    // returned, abort rather than call the wrong slots.
    if (strcmp(got, "SteamInput006") != 0) { logf_("ERROR: only SteamInput006 supported in probe"); return 0; }

    uint8_t ok = ((pfnSI_Init)VT(si, SI_Init))(si, 1);
    logf_("ISteamInput::Init -> %d", ok);

    LARGE_INTEGER freq, last, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    double accum_yaw[STEAM_INPUT_MAX_COUNT] = {0}, accum_pitch[STEAM_INPUT_MAX_COUNT] = {0};
    int samples = 0;
    int last_count = -1;

    for (;;) {
        ((pfnSI_RunFrame)VT(si, SI_RunFrame))(si, 1);
        InputHandle_t handles[STEAM_INPUT_MAX_COUNT];
        int n = ((pfnSI_GetConnected)VT(si, SI_GetConnectedControllers))(si, handles);
        if (n != last_count) {
            logf_("connected controllers: %d", n);
            for (int i = 0; i < n; i++) {
                int type = ((pfnSI_GetInputType)VT(si, SI_GetInputTypeForHandle))(si, handles[i]);
                int pad = ((pfnSI_GetGamepadIndex)VT(si, SI_GetGamepadIndexForController))(si, handles[i]);
                logf_("  [%d] handle=%016llx type=%s(%d) xinput_slot=%d", i, (unsigned long long)handles[i], steam_input_type_name(type), type, pad);
            }
            last_count = n;
        }
        for (int i = 0; i < n && i < STEAM_INPUT_MAX_COUNT; i++) {
            InputMotionData_t m;
            ((pfnSI_GetMotion)VT(si, SI_GetMotionData))(si, &m, handles[i]);
            accum_yaw[i] += m.rotVelZ;   // raw sums; units to be determined
            accum_pitch[i] += m.rotVelX;
        }
        samples++;

        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - last.QuadPart) / freq.QuadPart;
        if (dt >= 0.5) {
            for (int i = 0; i < n && i < STEAM_INPUT_MAX_COUNT; i++) {
                InputMotionData_t m;
                ((pfnSI_GetMotion)VT(si, SI_GetMotionData))(si, &m, handles[i]);
                int pad = ((pfnSI_GetGamepadIndex)VT(si, SI_GetGamepadIndexForController))(si, handles[i]);
                logf_("ctl%d slot%d vel=(%8.1f %8.1f %8.1f) acc=(%6.0f %6.0f %6.0f) quat=(%.3f %.3f %.3f %.3f) sumX=%.0f sumZ=%.0f",
                      i, pad, m.rotVelX, m.rotVelY, m.rotVelZ, m.posAccelX, m.posAccelY, m.posAccelZ,
                      m.rotQuatX, m.rotQuatY, m.rotQuatZ, m.rotQuatW, accum_pitch[i], accum_yaw[i]);
                accum_yaw[i] = accum_pitch[i] = 0;
            }
            for (int p = 0; p < 4; p++) {
                LONG c = InterlockedExchange(&g_getstate_calls[p], 0);
                LONG o = InterlockedExchange(&g_getstate_ok[p], 0);
                if (c) logf_("xinput%d: %ld calls (%ld ok) RS=(%d,%d) btn=%04x", p, c, o,
                             g_last_pad[p].sThumbRX, g_last_pad[p].sThumbRY, g_last_pad[p].wButtons);
            }
            logf_("poll rate: %.0f Hz", samples / dt);
            samples = 0;
            last = now;
        }
        Sleep(2);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        open_log(h);
        logf_("re6gyro probe loaded");
        CloseHandle(CreateThread(NULL, 0, probe_thread, NULL, 0, NULL));
    }
    return TRUE;
}
