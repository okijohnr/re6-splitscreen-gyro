#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 okijohnr
"""Analyse re6gyro_record.csv from the table-turn calibration.

usage: calibrate.py re6gyro_record.csv [turns=3]

Procedure the recording should contain, per controller:
  still (>=2 s) -> rotate flat on the table by exactly <turns> full turns -> still (>=2 s)
Reports Steam Input rotVel units per degree/second.
"""
import csv, math, sys
from collections import defaultdict

path = sys.argv[1]
turns = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0
target_deg = 360.0 * turns

rows = defaultdict(list)
with open(path) as f:
    for r in csv.DictReader(f):
        try:
            rows[int(r['ctl'])].append({k: float(v) for k, v in r.items()})
        except (ValueError, TypeError):
            pass  # truncated last line if the game exited while recording


def qmul(a, b):
    ax, ay, az, aw = a; bx, by, bz, bw = b
    return (aw*bx + ax*bw + ay*bz - az*by, aw*by - ax*bz + ay*bw + az*bx,
            aw*bz + ax*by - ay*bx + az*bw, aw*bw - ax*bx - ay*by - az*bz)


for ctl, d in sorted(rows.items()):
    t = [r['t'] for r in d]
    g = [(r['rx'], r['ry'], r['rz']) for r in d]
    port = int(d[0]['port'])
    dur = t[-1] - t[0]
    changes = sum(1 for i in range(1, len(g)) if g[i] != g[i-1])
    print(f'== controller {ctl} (xinput port {port}): {len(d)} samples over {dur:.1f}s, '
          f'~{changes/dur:.0f} new gyro reports/s')

    # gravity direction from the whole recording's still parts (table is the reference)
    mag = [math.sqrt(sum(x*x for x in v)) for v in g]
    # moving = smoothed |gyro| above threshold (raw units; bias is only ~20)
    win = 50
    sm = [sum(mag[max(0, i-win):i+win]) / len(mag[max(0, i-win):i+win]) for i in range(len(mag))]
    moving = [s > 150 for s in sm]
    # long still windows (>=1.5 s) separate calibration runs; short pauses
    # (re-gripping during the turns) stay inside a run.
    stills, i = [], 0
    while i < len(moving):
        if not moving[i]:
            j = i
            while j < len(moving) and not moving[j]: j += 1
            if t[j-1] - t[i] >= 1.5: stills.append((i, j))
            i = j
        else:
            i += 1
    segs = []
    for (s0, e0), (s1, e1) in zip(stills, stills[1:]):
        if any(moving[e0:s1]) and t[s1] - t[e0] > 1.0:
            segs.append((e0, s1))
    for (a, b) in segs:
        # bias from still windows just before / after the segment (up to 2 s each)
        pre = [k for k in range(a) if not moving[k] and t[a] - t[k] < 2.0]
        post = [k for k in range(b, len(d)) if not moving[k] and t[k] - t[b-1] < 2.0]
        still = pre + post
        if len(still) < 50:
            print(f'   segment {t[a]-t[0]:.1f}-{t[b-1]-t[0]:.1f}s: not enough still time around it'); continue
        bias = [sum(g[k][ax] for k in still) / len(still) for ax in range(3)]
        grav = [sum(d[k][c] for k in still) / len(still) for c in ('ax', 'ay', 'az')]
        gl = math.sqrt(sum(x*x for x in grav)); up = [x / gl for x in grav]
        # integrate (bias-corrected gyro) . up  from a little before to a little after
        a0 = pre[0] if pre else a
        b0 = post[-1] if post else b - 1
        integ = 0.0; full = 0.0
        for k in range(a0 + 1, b0 + 1):
            dt = t[k] - t[k-1]
            w = [g[k-1][ax] - bias[ax] for ax in range(3)]
            integ += sum(w[ax] * up[ax] for ax in range(3)) * dt
            full += math.sqrt(sum(x*x for x in w)) * dt
        cpd = abs(integ) / target_deg
        # quaternion cross-check: net rotation angle mod 360 between ends
        q0 = (d[a0]['qx'], d[a0]['qy'], d[a0]['qz'], d[a0]['qw'])
        q1 = (d[b0]['qx'], d[b0]['qy'], d[b0]['qz'], d[b0]['qw'])
        rel = qmul(q1, (-q0[0], -q0[1], -q0[2], q0[3]))
        qdeg = math.degrees(2 * math.acos(min(1.0, abs(rel[3]))))
        print(f'   turn {t[a]-t[0]:5.1f}-{t[b-1]-t[0]:5.1f}s  bias=({bias[0]:.1f},{bias[1]:.1f},{bias[2]:.1f}) '
              f'tilt={math.degrees(math.acos(min(1, abs(up[2])))):.1f}deg  '
              f'integral={integ:.0f} unit*s  -> {cpd:.3f} units per deg/s '
              f'(off-axis share {100*(1-abs(integ)/full):.1f}%, quat end error {qdeg:.1f}deg)')
