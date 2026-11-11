#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

config_t g_cfg;
static char g_ini[MAX_PATH];
static FILETIME g_ini_mtime;
static DWORD g_last_check;

static const char k_default_ini[] =
    "; RE6 splitscreen gyro aiming - settings are re-read live while the game runs.\r\n"
    "[gyro]\r\n"
    "enabled=1\r\n"
    "; camera degrees per degree of controller rotation\r\n"
    "sensitivity=2.0\r\n"
    "; vertical sensitivity relative to horizontal\r\n"
    "sensitivity_pitch=1.0\r\n"
    "invert_yaw=0\r\n"
    "invert_pitch=0\r\n"
    "; 1 = player space (turn the controller like a steering wheel OR yaw it; recommended)\r\n"
    "; 0 = local space (yaw only around the controller's own vertical axis)\r\n"
    "player_space=1\r\n"
    "; hold this button to pause gyro (e.g. to re-centre your hands). none, a, b, x, y, lb, rb, ls, rs, back, start\r\n"
    "disable_button=none\r\n"
    "\r\n"
    "[advanced]\r\n"
    "counts_per_dps=16.384\r\n"
    "deadzone_dps=0.0\r\n"
    "still_dps=2.5\r\n"
    "log_level=1\r\n"
    "; 1 = write raw gyro samples to re6gyro_record.csv (for calibration)\r\n"
    "record=0\r\n";

static float ini_float(const char *sec, const char *key, float def)
{
    char buf[64], d[32];
    snprintf(d, sizeof d, "%g", def);
    GetPrivateProfileStringA(sec, key, d, buf, sizeof buf, g_ini);
    return (float)atof(buf);
}

static WORD button_mask(const char *s)
{
    static const struct { const char *n; WORD m; } t[] = {
        {"a", 0x1000}, {"b", 0x2000}, {"x", 0x4000}, {"y", 0x8000},
        {"lb", 0x0100}, {"rb", 0x0200}, {"ls", 0x0040}, {"rs", 0x0080},
        {"back", 0x0020}, {"start", 0x0010},
        {"up", 0x0001}, {"down", 0x0002}, {"left", 0x0004}, {"right", 0x0008},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!_stricmp(s, t[i].n)) return t[i].m;
    return 0;
}

static void load(void)
{
    config_t c;
    c.enabled = GetPrivateProfileIntA("gyro", "enabled", 1, g_ini);
    c.sensitivity = ini_float("gyro", "sensitivity", 2.0f);
    c.sensitivity_pitch = ini_float("gyro", "sensitivity_pitch", 1.0f);
    c.invert_yaw = GetPrivateProfileIntA("gyro", "invert_yaw", 0, g_ini);
    c.invert_pitch = GetPrivateProfileIntA("gyro", "invert_pitch", 0, g_ini);
    c.player_space = GetPrivateProfileIntA("gyro", "player_space", 1, g_ini);
    char b[32];
    GetPrivateProfileStringA("gyro", "disable_button", "none", b, sizeof b, g_ini);
    c.disable_button = button_mask(b);
    c.counts_per_dps = ini_float("advanced", "counts_per_dps", 16.384f);
    if (c.counts_per_dps <= 0.01f) c.counts_per_dps = 16.384f;
    c.deadzone_dps = ini_float("advanced", "deadzone_dps", 0.0f);
    c.still_dps = ini_float("advanced", "still_dps", 2.5f);
    c.log_level = GetPrivateProfileIntA("advanced", "log_level", 1, g_ini);
    c.record = GetPrivateProfileIntA("advanced", "record", 0, g_ini);
    g_cfg = c;
    logf_("config: enabled=%d sens=%.2f pitch=%.2f inv=%d/%d player_space=%d disable=0x%04x cpd=%.3f record=%d",
          c.enabled, c.sensitivity, c.sensitivity_pitch, c.invert_yaw, c.invert_pitch,
          c.player_space, c.disable_button, c.counts_per_dps, c.record);
}

static int mtime(FILETIME *out)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(g_ini, GetFileExInfoStandard, &a)) return 0;
    *out = a.ftLastWriteTime;
    return 1;
}

void config_load(void)
{
    extern HMODULE g_self;
    GetModuleFileNameA(g_self, g_ini, MAX_PATH);
    char *slash = strrchr(g_ini, '\\');
    if (slash) slash[1] = 0;
    strcat(g_ini, "re6gyro.ini");
    if (GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileA(g_ini, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(h, k_default_ini, sizeof k_default_ini - 1, &w, NULL);
            CloseHandle(h);
        }
    }
    mtime(&g_ini_mtime);
    load();
}

void config_maybe_reload(void)
{
    DWORD t = GetTickCount();
    if (t - g_last_check < 1000) return;
    g_last_check = t;
    FILETIME m;
    if (mtime(&m) && CompareFileTime(&m, &g_ini_mtime) != 0) {
        g_ini_mtime = m;
        load();
    }
}
