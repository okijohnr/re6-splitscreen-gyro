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
//      mode 3, state 1  - e.g. swimming: the camera is rebuilt each frame from the
//                         character facing; we turn the body (turn_body).
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

// The player's PortNo is the game's player slot, not the controller. Slot 0 is
// always the primary character, so when player 1 picks the secondary character
// the slots and controllers swap. sPad (*0x17c1384) holds a per-slot bitmask of
// assigned input devices at +0x364; each device lives in the device manager
// (*0x186e8e8) at +0x40 + i*0x2d8, with type at +0x3 (4 = XInput) and its
// XInput index at +0xc. Returns the XInput index, or the slot if none is found.
#define SPAD_PTR        0x17c1384
#define SPAD_DEVMASK    0x364
#define DEVMGR_PTR      0x186e8e8
#define DEV_BASE        0x40
#define DEV_STRIDE      0x2d8
#define DEV_TYPE        0x3
#define DEV_TYPE_XINPUT 4
#define DEV_XINPUT_IDX  0xc
#define MAX_DEVICES     16

static int pad_xinput(int slot)
{
    static int last[MAX_PORTS] = { -1, -1, -1, -1 };
    int idx = slot;
    uint8_t *spad = *(uint8_t **)SPAD_PTR;
    uint8_t *mgr = *(uint8_t **)DEVMGR_PTR;
    if (spad && mgr) {
        uint32_t mask = *(uint32_t *)(spad + SPAD_DEVMASK + slot * 4);
        for (int i = 0; i < MAX_DEVICES; i++) {
            if (!(mask & (1u << i))) continue;
            uint8_t *dev = mgr + DEV_BASE + i * DEV_STRIDE;
            if (dev[DEV_TYPE] != DEV_TYPE_XINPUT) continue;
            uint32_t x = *(uint32_t *)(dev + DEV_XINPUT_IDX);
            if (x < MAX_PORTS) { idx = (int)x; break; }
        }
    }
    if (idx != last[slot]) {
        logf_("player slot %d uses controller %d", slot, idx);
        last[slot] = idx;
    }
    return idx;
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
    g_dbg.cam_ms[port] = g_seen[port].last_ms;
    g_dbg.pl[port] = pl;
}

static char __fastcall hook_pad(void *self, void *edx, void *pl, float dt, float *pitch, float *yaw, int flag)
{
    char applied = g_orig_pad(self, edx, pl, dt, pitch, yaw, flag);

    int port = player_port(pl);
    if (port < 0) { InterlockedIncrement(&g_hook_calls_other); return applied; }
    InterlockedIncrement(&g_hook_calls[port]);
    seen(port, self, pl, (DWORD)(uintptr_t)__builtin_return_address(0));

    float dyaw, dpitch;
    gyro_take(pad_xinput(port), &dyaw, &dpitch);
    if (dyaw == 0.0f && dpitch == 0.0f) return applied;
    apply(pitch, yaw, dpitch, dyaw);
    return 1; // tell the camera the player moved it (suppresses auto-recentre)
}

static void __fastcall hook_aim(void *pad, void *edx, void *pl, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6)
{
    uint32_t port = pad ? *(uint32_t *)((uint8_t *)pad + PAD_PORT) : 0xffffffff;
    if (port < MAX_PORTS) { g_dbg.aim_ms[port] = GetTickCount(); g_dbg.pad[port] = pad; }
    // Lying down: the camera copies the character's aim (mode 1/state 1).
    // Mounted guns in vehicles: no player camera runs at all and the gun
    // follows the character's aim, so add gyro whenever the camera is idle.
    DWORD idle = port < MAX_PORTS ? GetTickCount() - g_seen[port].last_ms : 0;
    if (port < MAX_PORTS &&
        ((g_seen[port].mode == 1 && g_seen[port].state == 1 && idle < 200) || idle > 300)) {
        float dyaw, dpitch;
        gyro_take(pad_xinput((int)port), &dyaw, &dpitch);
        // camera yaw = body + offset (deg); camera pitch = -aim pitch
        *(float *)((uint8_t *)pad + PAD_AIM_YAW_DEG) += dyaw * 57.29578f;
        *(float *)((uint8_t *)pad + PAD_AIM_PITCH) -= dpitch;
    }
    g_orig_aim(pad, edx, pl, a2, a3, a4, a5, a6);
}

// Mode 3/state 1 (e.g. swimming): the game rebuilds the camera every frame
// with pitch 0 and yaw along the character's facing, and the right stick turns
// the character's body instead (yaw, and a dive tilt the game levels out again).
// We do the same with gyro: rotate the body orientation quaternion (pl+0x60,
// x y z w) about world up for yaw, and about the body's horizontal side axis
// for pitch. The game keeps the result, and the camera follows next frame.
#define PL_QUAT        0x60
#define SWIM_MAX_PITCH 0.866f   // sin(60 deg): the game's own dive-tilt limit

static void qmul(const float a[4], const float b[4], float o[4])
{
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

static float cap(float v, float lim) { return v > lim ? lim : v < -lim ? -lim : v; }

static void turn_body(int port, void *pl)
{
    // frame time, for the turn-speed cap
    static LARGE_INTEGER freq, last[MAX_PORTS];
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    float dt = last[port].QuadPart ? (float)(now.QuadPart - last[port].QuadPart) / (float)freq.QuadPart : 0.0f;
    last[port] = now;
    if (dt <= 0.0f || dt > 0.1f) dt = 1.0f / 60;

    float dyaw, dpitch;
    gyro_take(pad_xinput(port), &dyaw, &dpitch);
    if (dyaw == 0.0f && dpitch == 0.0f) return;
    dyaw *= g_cfg.sensitivity_swim;
    dpitch *= g_cfg.sensitivity_swim;
    // like the right stick (and the mouse), turning in water has a top speed;
    // anything faster is dropped, not saved up for later
    if (g_cfg.swim_max_dps > 0.0f) {
        float lim = g_cfg.swim_max_dps * 0.017453293f * dt;
        dyaw = cap(dyaw, lim);
        dpitch = cap(dpitch, lim);
    }

    float *q = (float *)((uint8_t *)pl + PL_QUAT);
    float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (!(n > 0.9f && n < 1.1f)) return; // not a sane rotation: leave it alone

    // yaw: world-space rotation about +Y (positive = turn left, as the camera)
    float ry[4] = { 0, sinf(dyaw * 0.5f), 0, cosf(dyaw * 0.5f) }, t[4];
    qmul(ry, q, t);

    // pitch: rotate about axis = forward x up, which raises the nose for +dpitch
    float x = t[0], y = t[1], z = t[2], w = t[3];
    float fx = 2 * (x * z + w * y), fy = 2 * (y * z - w * x), fz = 1 - 2 * (x * x + y * y);
    float ax = -fz, az = fx, al = sqrtf(ax * ax + az * az);  // (f x up) with up = +Y
    float np = fy + dpitch * sqrtf(fmaxf(0.0f, 1 - fy * fy));
    if (al > 1e-3f && fabsf(np) < SWIM_MAX_PITCH) {
        float s = sinf(dpitch * 0.5f) / al;
        float rp[4] = { ax * s, 0, az * s, cosf(dpitch * 0.5f) };
        qmul(rp, t, q);
    } else {
        memcpy(q, t, sizeof t);
    }
    n = 1.0f / sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; i++) q[i] *= n;
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
        gyro_take(pad_xinput(port), &dyaw, &dpitch);
        apply((float *)((uint8_t *)self + CAM_PITCH), (float *)((uint8_t *)self + CAM_YAW), dpitch, dyaw);
    } else if (mode == 1 && state == 1) {
        seen(port, self, pl, 0x622891); // gyro is applied in hook_aim
    } else if (mode == 3 && state == 1) {
        seen(port, self, pl, 0x622892);
        turn_body(port, pl);
    }
}

// 4) uWpTurret aim update (0xcdd190, mounted guns)
//      void __thiscall uWpTurret()
//    The turret has its own camera and never runs (1)-(3). Its operator is at
//    +0xecc; each frame it reads that player's right stick and moves its own
//    aim angles, +0x3260 (pitch, up = smaller) and +0x3264 (yaw, right = larger),
//    through setters that clamp them to the turret's limits (0x4a6620/0x4a6670).
//    After the update we add gyro through the same setters.
#define FN_TURRET_AIM     0xcdd190
#define FN_TURRET_PITCH   0x4a6620
#define FN_TURRET_YAW     0x4a6670
#define TURRET_OPERATOR   0xecc
#define TURRET_PITCH      0x3260
#define TURRET_YAW        0x3264
#define PLAYER_LIST_PTR   0x17d0468  // +0x24: 8 player slots (as the game's own check, 0x590f40)
#define PLAYER_STATE      0x4e90     // the turret ignores input when this is 2

static const uint8_t k_pro_turret[5] = { 0x83, 0xec, 0x3c, 0x53, 0x56 };
typedef void (__fastcall *turret_fn)(void *self, void *edx);
typedef void (__fastcall *turret_set_fn)(void *self, void *edx, float v);
static turret_fn g_orig_turret;

static int is_player(void *p)
{
    uint8_t *list = *(uint8_t **)PLAYER_LIST_PTR;
    if (!p || !list) return 0;
    for (int i = 0; i < 8; i++)
        if (*(void **)(list + 0x24 + i * 4) == p) return 1;
    return 0;
}

static void __fastcall hook_turret(void *self, void *edx)
{
    g_orig_turret(self, edx);

    void *pl = *(void **)((uint8_t *)self + TURRET_OPERATOR);
    if (!is_player(pl) || *(int *)((uint8_t *)pl + PLAYER_STATE) == 2) return;
    int port = player_port(pl);
    if (port < 0) return;
    g_seen[port].ret = FN_TURRET_AIM;
    g_seen[port].last_ms = GetTickCount();

    float dyaw, dpitch;
    gyro_take(pad_xinput(port), &dyaw, &dpitch);
    if (dyaw == 0.0f && dpitch == 0.0f) return;
    float *a = (float *)((uint8_t *)self + TURRET_PITCH);
    ((turret_set_fn)FN_TURRET_PITCH)(self, NULL, a[0] - dpitch); // gyro +pitch = up
    ((turret_set_fn)FN_TURRET_YAW)(self, NULL, a[1] - dyaw);     // gyro +yaw = left
}

// debug=1 only: record who reads a player's right stick (0xabc8d0, cdecl,
// arg 1 = uPlayer*), so a log shows which code a new situation uses.
#define FN_GET_STICK 0xabc8d0
static const uint8_t k_pro_stick[8] = { 0x83, 0xec, 0x18, 0x56, 0x8b, 0x74, 0x24, 0x20 }; // sub esp,18; push esi; mov esi,[esp+20]
void *g_orig_stick;
stick_site_t g_stick_sites[MAX_STICK_SITES];

void __cdecl note_stick_caller(DWORD ret, void *pl)
{
    int port = player_port(pl);
    DWORD now = GetTickCount();
    for (int i = 0; i < MAX_STICK_SITES; i++) {
        stick_site_t *s = &g_stick_sites[i];
        if (s->ret == ret && s->port == port) { s->last_ms = now; s->calls++; return; }
        if (!s->ret) { s->port = port; s->last_ms = now; s->calls = 1; s->ret = ret; return; }
    }
}

// naked stub: note the caller, then continue into the original function
__asm__(
    ".globl _hook_stick_stub\n"
    "_hook_stick_stub:\n"
    "    pushal\n"
    "    pushl 36(%esp)\n"      // arg 1 (player)
    "    pushl 36(%esp)\n"      // return address
    "    call _note_stick_caller\n"
    "    addl $8, %esp\n"
    "    popal\n"
    "    jmp *_g_orig_stick\n");
void hook_stick_stub(void);

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
    if (!g_orig_aim) return 0;
    // independent of the camera hooks: without it only mounted guns lack gyro
    g_orig_turret = (turret_fn)install(FN_TURRET_AIM, k_pro_turret, sizeof k_pro_turret, (void *)hook_turret, "turret-aim");
    if (g_cfg.debug)
        g_orig_stick = install(FN_GET_STICK, k_pro_stick, sizeof k_pro_stick, (void *)hook_stick_stub, "debug stick-reader");
    return 1;
}
