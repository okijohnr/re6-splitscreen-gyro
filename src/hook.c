// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 okijohnr
// Camera hooks in BH6.exe.
//
// 1) uCameraQFPS::UpdatePadAngle (0x615c40)
//      bool __thiscall (uPlayer *pl, float dt, float *pitch, float *yaw, bool flag)
//    Called with pointers to whichever angle pair the current camera mode
//    feeds stick input into. It adds stick input, clamps pitch to [-1,1] and
//    wraps yaw to [-pi,pi). We call the original and add our gyro delta the same way.
//
// 2) uCameraQFPS angle update (0x622890), the only caller of (1)
//      void __thiscall (uPlayer *pl, int mode, int state, int idx, float lim, float dt, bool flag)
//    Runs every camera frame and switches on mode/state. Some modes don't read
//    camera input at all, so (1) never runs there:
//      mode 2           - e.g. the ~275 ms aim-in transition: no camera input.
//                         We add gyro straight to the camera angles (+0x290/+0x294).
//      mode 1, state 1  - e.g. lying on the ground: the camera copies the
//                         character's own aim (see 3).
//
// 3) cPadEx aim update (0xabaa40)
//      void __thiscall cPadEx(uPlayer *pl, ... 5 more)
//    Runs every frame and moves the character's own aim from the right stick:
//      pad+0x848  yaw offset in degrees, clamped to +-10; the excess goes to
//                 pad+0x834 and turns the body
//      pad+0x84c  pitch, 1.0 = 90 deg for the aim, used as radians by the camera
//    The game copies these to the player (0xa45da0) and, lying down, the camera
//    follows them. While that player's camera is in mode 1/state 1 we add gyro to
//    both before the update runs, so the game's own limits and body turn apply.
#include "common.h"
#include <math.h>
#include <string.h>

#define FN_UPDATE_PAD_ANGLE 0x615c40
#define FN_UPDATE_ANGLE     0x622890
#define PLAYER_INTEL_OFS    0x4e50  // uPlayer -> uPlIntel*
#define INTEL_PORT_OFS      0xcec   // uPlIntel -> cPadEx(+0x480) -> cPad(+0x860) -> PortNo(+0xc)
#define CAM_PITCH           0x290
#define CAM_YAW             0x294
#define FN_PAD_AIM          0xabaa40
#define PAD_PORT            0x86c   // cPadEx -> cPad(+0x860) -> PortNo(+0xc)
#define PAD_AIM_YAW_DEG     0x848
#define PAD_AIM_PITCH       0x84c

static const uint8_t k_pro_pad[7]   = { 0x53, 0x56, 0x57, 0x8b, 0x7c, 0x24, 0x10 };
static const uint8_t k_pro_angle[6] = { 0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0 };

typedef char (__fastcall *pad_fn)(void *self, void *edx, void *pl, float dt, float *pitch, float *yaw, int flag);
typedef void (__fastcall *angle_fn)(void *self, void *edx, void *pl, int mode, int state, int idx, float lim, float dt, int flag);
typedef void (__fastcall *aim_fn)(void *pad, void *edx, void *pl, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6);
static pad_fn g_orig_pad;
static angle_fn g_orig_angle;
static aim_fn g_orig_aim;

volatile LONG g_hook_calls[MAX_PORTS];
volatile LONG g_hook_calls_other;

static int player_port(void *pl)
{
    if (!pl) return -1;
    uint8_t *intel = *(uint8_t **)((uint8_t *)pl + PLAYER_INTEL_OFS);
    if (!intel) return -1;
    uint32_t port = *(uint32_t *)(intel + INTEL_PORT_OFS);
    return port < MAX_PORTS ? (int)port : -1;
}

static float wrap_pi(float a)
{
    const float twopi = 6.2831853f, pi = 3.1415927f;
    a = fmodf(a, twopi);
    if (a < -pi) a += twopi;
    if (a >= pi) a -= twopi;
    return a;
}

static void apply(float *pitch, float *yaw, float dpitch, float dyaw)
{
    if (yaw) *yaw = wrap_pi(*yaw + dyaw);
    if (pitch) {
        float p = *pitch + dpitch;
        if (p < -1.0f) p = -1.0f;
        if (p > 1.0f) p = 1.0f;
        *pitch = p;
    }
}

static void seen(int port, void *self, void *pl, DWORD ret)
{
    g_seen[port].cam = self;
    g_seen[port].pl = pl;
    g_seen[port].ret = ret;
    g_seen[port].last_ms = GetTickCount();
}

static char __fastcall hook_pad(void *self, void *edx, void *pl, float dt, float *pitch, float *yaw, int flag)
{
    char applied = g_orig_pad(self, edx, pl, dt, pitch, yaw, flag);

    int port = player_port(pl);
    if (port < 0) { InterlockedIncrement(&g_hook_calls_other); return applied; }
    InterlockedIncrement(&g_hook_calls[port]);
    seen(port, self, pl, (DWORD)(uintptr_t)__builtin_return_address(0));

    float dyaw, dpitch;
    gyro_take(port, &dyaw, &dpitch);
    if (dyaw == 0.0f && dpitch == 0.0f) return applied;
    apply(pitch, yaw, dpitch, dyaw);
    return 1; // tell the camera the player moved it (suppresses auto-recentre)
}

static void __fastcall hook_aim(void *pad, void *edx, void *pl, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6)
{
    uint32_t port = pad ? *(uint32_t *)((uint8_t *)pad + PAD_PORT) : 0xffffffff;
    if (port < MAX_PORTS && g_seen[port].mode == 1 && g_seen[port].state == 1 &&
        GetTickCount() - g_seen[port].last_ms < 200) {
        float dyaw, dpitch;
        gyro_take((int)port, &dyaw, &dpitch);
        // camera yaw = body + offset (deg); camera pitch = -aim pitch
        *(float *)((uint8_t *)pad + PAD_AIM_YAW_DEG) += dyaw * 57.29578f;
        *(float *)((uint8_t *)pad + PAD_AIM_PITCH) -= dpitch;
    }
    g_orig_aim(pad, edx, pl, a2, a3, a4, a5, a6);
}

static void __fastcall hook_angle(void *self, void *edx, void *pl, int mode, int state, int idx, float lim, float dt, int flag)
{
    g_orig_angle(self, edx, pl, mode, state, idx, lim, dt, flag);

    int port = player_port(pl);
    if (port < 0) return;
    g_seen[port].mode = mode;
    g_seen[port].state = state;

    if (mode == 2) {
        seen(port, self, pl, 0x622890);
        float dyaw, dpitch;
        gyro_take(port, &dyaw, &dpitch);
        apply((float *)((uint8_t *)self + CAM_PITCH), (float *)((uint8_t *)self + CAM_YAW), dpitch, dyaw);
    } else if (mode == 1 && state == 1) {
        seen(port, self, pl, 0x622891); // gyro is applied in hook_aim
    }
}

static void *install(uintptr_t addr, const uint8_t *pro, int n, void *hook, const char *name)
{
    uint8_t *target = (uint8_t *)addr;
    if (IsBadReadPtr(target, n) || memcmp(target, pro, n) != 0) {
        logf_("ERROR: unexpected bytes at %p (%s) - unsupported BH6.exe version, hook NOT installed", target, name);
        return NULL;
    }
    uint8_t *tramp = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) { logf_("ERROR: VirtualAlloc failed"); return NULL; }
    memcpy(tramp, pro, n);
    tramp[n] = 0xE9;
    *(int32_t *)(tramp + n + 1) = (int32_t)((target + n) - (tramp + n + 5));

    DWORD old;
    if (!VirtualProtect(target, n, PAGE_EXECUTE_READWRITE, &old)) { logf_("ERROR: VirtualProtect failed"); return NULL; }
    uint8_t patch[8];
    memset(patch, 0x90, sizeof patch);
    patch[0] = 0xE9;
    *(int32_t *)(patch + 1) = (int32_t)((uint8_t *)hook - (target + 5));
    memcpy(target, patch, n);
    VirtualProtect(target, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, n);
    logf_("%s hook installed at %p", name, target);
    return tramp;
}

int hook_install(void)
{
    // Install the outer hook only if the inner one succeeds, so a partial
    // version mismatch never leaves just one of them active.
    g_orig_pad = (pad_fn)install(FN_UPDATE_PAD_ANGLE, k_pro_pad, sizeof k_pro_pad, (void *)hook_pad, "pad-angle");
    if (!g_orig_pad) return 0;
    g_orig_angle = (angle_fn)install(FN_UPDATE_ANGLE, k_pro_angle, sizeof k_pro_angle, (void *)hook_angle, "camera-angle");
    if (!g_orig_angle) return 0;
    g_orig_aim = (aim_fn)install(FN_PAD_AIM, k_pro_angle, sizeof k_pro_angle, (void *)hook_aim, "character-aim");
    return g_orig_aim != NULL;
}
