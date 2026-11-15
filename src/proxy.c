// RE6 splitscreen gyro aiming.
// Proxy xinput1_3.dll: forwards XInput to the real system DLL, records button
// state per port, installs the camera hook and starts the Steam Input gyro reader.
#include "common.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

HMODULE g_self;
volatile WORD g_buttons[MAX_PORTS];

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

void logf_(const char *fmt, ...)
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

static void open_log(void)
{
    char path[MAX_PATH];
    GetModuleFileNameA(g_self, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    if (slash) slash[1] = 0;
    strcat(path, "re6gyro_log.txt");
    InitializeCriticalSection(&g_logcs);
    g_log = fopen(path, "w");
}

// ---------------------------------------------------------------- real xinput
#define GPA(T, name) (T)(void *)GetProcAddress(g_real, name)

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
        if (g_real) logf_("real xinput: %s", path);
    }
    if (!g_real) { logf_("ERROR: could not load a real xinput dll"); return; }
    r_GetState     = GPA(pfnGetState, "XInputGetState");
    r_GetStateEx   = GPA(pfnGetState, (LPCSTR)100);
    r_SetState     = GPA(pfnGeneric2, "XInputSetState");
    r_GetCaps      = GPA(pfnGeneric3, "XInputGetCapabilities");
    r_GetKeystroke = GPA(pfnGeneric3, "XInputGetKeystroke");
    r_Enable       = GPA(pfnEnable, "XInputEnable");
    r_DSound       = GPA(pfnDSound, "XInputGetDSoundAudioDeviceGuids");
    r_Battery      = GPA(pfnBattery, "XInputGetBatteryInformation");
}

static void note_state(DWORD idx, DWORD rc, XINPUT_STATE *s)
{
    if (idx < MAX_PORTS) g_buttons[idx] = (rc == 0 && s) ? s->Gamepad.wButtons : 0;
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

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        open_log();
        logf_("re6gyro v0.6-dev loaded");
        config_load();
        hook_install();
        gyro_start();
    }
    return TRUE;
}
