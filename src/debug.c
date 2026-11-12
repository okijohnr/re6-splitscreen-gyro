// [advanced] debug=1: log per-player camera state changes and pauses in the
// camera hook, so bug reports show which camera mode a situation uses.
//
// uCameraQFPS::update (0x625320) picks one of two parameter blocks
// (cam+0x2c0 / cam+0x2e8, chosen by byte cam+0x33d) and passes block fields
// +0x0c (mode) and +0x08 (state) to the angle update (0x622890), which
// switches on them:
//   mode 1: state 0 = stick, state 1 = copy the player's own aim angles (no hook)
//   mode 2: no camera input             mode 3: none / angles from player facing
//   mode 4: pitch only / relative pitch mode 5: yaw only / relative yaw
//   mode 6: relative, yaw clamped       other: stick / relative
// It is skipped entirely when cam+0x80 (target) isn't a player.
#include "common.h"
#include <string.h>

hook_seen_t g_seen[MAX_PORTS];

typedef struct {
    uint32_t tgt;
    uint8_t  sel;
    uint32_t a[10], b[10];
    uint32_t f1, f2;
} camsnap_t;

static int rd(const void *addr, void *out, SIZE_T n)
{
    SIZE_T got = 0;
    return addr && ReadProcessMemory(GetCurrentProcess(), addr, out, n, &got) && got == n;
}

static const char *site_name(DWORD ret)
{
    switch (ret) {
    case 0x622c85: return "mode1/state0 stick";
    case 0x622d6c: return "mode4/state0 pitch-only";
    case 0x622dbc: return "mode4/state1 relative pitch";
    case 0x622e14: return "mode5/state0 yaw-only";
    case 0x622e58: return "mode5/state1 relative yaw";
    case 0x622ec5: return "mode6/state1 relative clamped";
    case 0x622f3e: return "default/state0 stick";
    case 0x622f84: return "default/state1 relative";
    case 0x622890: return "mode2 no-input: gyro applied to camera directly";
    case 0x622891: return "mode1/state1 player aim: gyro sent as right stick";
    default:       return "unknown caller";
    }
}

static void blk(char *out, size_t n, const uint32_t *b)
{
    (void)n;
    wsprintfA(out, "mode=%d state=%d idx=%d", (int)b[3], (int)b[2], (int)b[7]);
}

void debug_poll(void)
{
    static camsnap_t last[MAX_PORTS];
    static void *last_cam[MAX_PORTS];
    static DWORD last_ret[MAX_PORTS];
    static DWORD gap_start[MAX_PORTS];
    if (!g_cfg.debug) return;
    DWORD now = GetTickCount();

    for (int p = 0; p < MAX_PORTS; p++) {
        uint8_t *cam = g_seen[p].cam;
        if (!cam) continue;

        // pauses in hook calls (camera not reading stick input through the hook)
        DWORD lc = g_seen[p].last_ms;
        if (now - lc > 60) {
            if (!gap_start[p]) gap_start[p] = lc;
        } else if (gap_start[p]) {
            logf_("dbg port%d: hook paused %lu ms", p, (unsigned long)(lc - gap_start[p]));
            gap_start[p] = 0;
        }

        DWORD ret = g_seen[p].ret;
        if (ret != last_ret[p]) {
            logf_("dbg port%d: hook site %#lx (%s)", p, (unsigned long)ret, site_name(ret));
            last_ret[p] = ret;
        }

        camsnap_t s;
        memset(&s, 0, sizeof s);
        if (!rd(cam + 0x80, &s.tgt, 4) || !rd(cam + 0x33d, &s.sel, 1) ||
            !rd(cam + 0x2c0, s.a, sizeof s.a) || !rd(cam + 0x2e8, s.b, sizeof s.b) ||
            !rd(cam + 0x1a5c, &s.f1, 4) || !rd(cam + 0x1a68, &s.f2, 4))
            continue;
        // compare only the logged fields (the blocks also hold floats that may tick)
        for (int k = 0; k < 10; k++) if (k != 2 && k != 3 && k != 7) { s.a[k] = 0; s.b[k] = 0; }
        s.a[9] = 0; s.b[9] = 0;
        if (cam != last_cam[p] || memcmp(&s, &last[p], sizeof s) != 0) {
            static DWORD win[MAX_PORTS], cnt[MAX_PORTS];
            if (now - win[p] > 1000) { win[p] = now; cnt[p] = 0; }
            if (++cnt[p] > 20) { last[p] = s; last_cam[p] = cam; continue; } // rate limit
            char ba[96], bb[96];
            blk(ba, sizeof ba, s.a);
            blk(bb, sizeof bb, s.b);
            float pa[2] = {0, 0};
            if (s.tgt) rd((uint8_t *)(uintptr_t)s.tgt + 0x4ed4, pa, sizeof pa);
            logf_("dbg port%d cam=%p tgt=%08lx use=%s | A: %s | B: %s | flags=%08lx %08lx | plaim=%.3f %.3f%s",
                  p, cam, (unsigned long)s.tgt, s.sel ? "B" : "A", ba, bb,
                  (unsigned long)s.f1, (unsigned long)s.f2, pa[0], pa[1],
                  gap_start[p] ? " (hook not running)" : "");
            last[p] = s;
            last_cam[p] = cam;
        }
    }
}
