// Hook of uCameraQFPS's pad-angle update (BH6.exe 0x615c40).
//
//   bool __thiscall uCameraQFPS::UpdatePadAngle(uPlayer *pl, float dt, float *pitch, float *yaw, bool flag)
//
// The game calls this once per camera update with pointers to whichever angle
// pair the current camera mode accumulates stick input into. It adds stick
// input, clamps pitch to [-1,1] and wraps yaw to [-pi,pi). We call the original
// and then add our gyro delta the same way.
#include "common.h"
#include <math.h>
#include <string.h>

#define FN_UPDATE_PAD_ANGLE 0x615c40
#define PLAYER_INTEL_OFS    0x4e50  // uPlayer -> uPlIntel*
#define INTEL_PORT_OFS      0xcec   // uPlIntel -> cPadEx(+0x480) -> cPad(+0x860) -> PortNo(+0xc)

static const uint8_t k_prologue[7] = { 0x53, 0x56, 0x57, 0x8b, 0x7c, 0x24, 0x10 };

typedef char (__fastcall *update_fn)(void *self, void *edx, void *pl, float dt, float *pitch, float *yaw, int flag);
static update_fn g_orig;

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

static char __fastcall hook_update(void *self, void *edx, void *pl, float dt, float *pitch, float *yaw, int flag)
{
    char applied = g_orig(self, edx, pl, dt, pitch, yaw, flag);

    int port = player_port(pl);
    if (port < 0) { InterlockedIncrement(&g_hook_calls_other); return applied; }
    InterlockedIncrement(&g_hook_calls[port]);

    float dyaw, dpitch;
    gyro_take(port, &dyaw, &dpitch);
    if (dyaw == 0.0f && dpitch == 0.0f) return applied;

    if (yaw) *yaw = wrap_pi(*yaw + dyaw);
    if (pitch) {
        float p = *pitch + dpitch;
        if (p < -1.0f) p = -1.0f;
        if (p > 1.0f) p = 1.0f;
        *pitch = p;
    }
    return 1; // tell the camera the player moved it (suppresses auto-recentre)
}

int hook_install(void)
{
    uint8_t *target = (uint8_t *)FN_UPDATE_PAD_ANGLE;
    if (IsBadReadPtr(target, sizeof k_prologue) || memcmp(target, k_prologue, sizeof k_prologue) != 0) {
        logf_("ERROR: unexpected bytes at %p - unsupported BH6.exe version, camera hook NOT installed", target);
        return 0;
    }
    // trampoline: original 7 bytes + jmp back
    uint8_t *tramp = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) { logf_("ERROR: VirtualAlloc failed"); return 0; }
    memcpy(tramp, k_prologue, sizeof k_prologue);
    tramp[7] = 0xE9;
    *(int32_t *)(tramp + 8) = (int32_t)((target + 7) - (tramp + 12));
    g_orig = (update_fn)tramp;

    DWORD old;
    if (!VirtualProtect(target, 7, PAGE_EXECUTE_READWRITE, &old)) { logf_("ERROR: VirtualProtect failed"); return 0; }
    uint8_t patch[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
    *(int32_t *)(patch + 1) = (int32_t)((uint8_t *)hook_update - (target + 5));
    memcpy(target, patch, sizeof patch);
    VirtualProtect(target, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 7);
    logf_("camera hook installed at %p (trampoline %p)", target, tramp);
    return 1;
}
