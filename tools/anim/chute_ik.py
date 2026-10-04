"""Rico's chute flight pose, solved offline for a grid of chute pitch x bank. JC2 poses Rico by
time-blending his own rico_speed_TB / rico_turn_TB clips (kc001_base.asb, ANIM_PARACHUTE) at the
same frames as the canopy's chute_speed_TB / chute_turn_TB (mapped like skse/src/visuals.cpp
poses the canopy), so the whole body swings with W/S and A/D. After the retarget the hands land
2-14 units off Para_LeftHandle / Para_RightHandle; two-bone IK on both arms closes that.
build_anims.py bakes the grid to rico_chute.bin."""
import os
import numpy as np

import paths
from jc2havok import read_skeleton, read_animation
from retarget import fk, q2m, m2q, C, UNITS_PER_M

RIG = os.path.join(paths.EXTRACTED, "5b8827ed")
ARMS = [("NPC L UpperArm [LUar]", "NPC L Forearm [LLar]", "NPC L Hand [LHnd]", "Para_LeftHandle"),
        ("NPC R UpperArm [RUar]", "NPC R Forearm [RLar]", "NPC R Hand [RHnd]", "Para_RightHandle")]


def _sky_q(q):
    v = C @ np.asarray(q[:3], float)
    return np.array([v[0], v[1], v[2], q[3]])


def _qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return np.array([aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
                     aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz])


def _sample(anim, track, skel, frame):
    """Per bone (t, q) in Skyrim axes and units at a fractional frame."""
    f0 = int(np.floor(frame))
    f1 = min(f0 + 1, len(anim.frames) - 1)
    u = frame - f0
    out = []
    for i, n in enumerate(skel.names):
        if n in track:
            a, b = anim.frames[f0, track[n]], anim.frames[f1, track[n]]
            qa, qb = a[3:7].astype(float), b[3:7].astype(float)
            if np.dot(qa, qb) < 0:
                qb = -qb
            q = qa + (qb - qa) * u
            src_t, src_q = a[0:3] + (b[0:3] - a[0:3]) * u, q / np.linalg.norm(q)
        else:
            src_t, src_q = skel.ref[i, 0:3], skel.ref[i, 3:7]
        out.append((C @ src_t * UNITS_PER_M, _sky_q(src_q)))
    return out


def _nlerp_frames(a, b, u):
    """Blend two (n, 10) Skyrim pose frames: positions lerped, rotations nlerped."""
    b = b.copy()
    b[:, 3:7] *= np.sign(np.sum(a[:, 3:7] * b[:, 3:7], axis=1))[:, None]
    out = a + (b - a) * u
    out[:, 3:7] /= np.linalg.norm(out[:, 3:7], axis=1)[:, None]
    return out


def _frame_at(frames, f):
    f = float(np.clip(f, 0, len(frames) - 1))
    f0 = int(np.floor(f))
    return _nlerp_frames(frames[f0], frames[min(f0 + 1, len(frames) - 1)], f - f0)


def rico_pose(speed, turn, pitch_norm, bank_norm):
    """speed / turn: retargeted rico_speed_TB_UP / rico_turn_TB_UP frames -> one pose frame."""
    return _nlerp_frames(_frame_at(speed, 15 + 15 * pitch_norm), _frame_at(turn, 15 - 15 * bank_norm),
                         turn_weight(pitch_norm, bank_norm))


def turn_weight(p, b):
    """Share of the turn clip in the canopy pose. Speed and turn are blended, not added (added,
    they pull a handle below the hips). Same rule as visuals.cpp."""
    s = abs(p) + abs(b)
    return abs(b) / s if s > 1e-6 else 0.0


class Canopy:
    def __init__(self):
        self.skel = read_skeleton(os.path.join(RIG, "Parachute.bsk"))
        self.speed = read_animation(os.path.join(RIG, "chute_speed_TB_UP.ban"))
        self.turn = read_animation(os.path.join(RIG, "chute_turn_TB_UP.ban"))
        self.ts = {t: i for i, t in enumerate(self.speed.track_names)}
        self.tt = {t: i for i, t in enumerate(self.turn.track_names)}

    def handles(self, pitch_norm, bank_norm):
        """World (actor space) handle positions for a chute pitch / bank in -1..1."""
        fs, ft = 15 + 15 * pitch_norm, 15 - 15 * bank_norm
        w = turn_weight(pitch_norm, bank_norm)
        S = _sample(self.speed, self.ts, self.skel, fs)
        T = _sample(self.turn, self.tt, self.skel, ft)
        lr, lt = [], []
        for (st, sq), (tt, tq) in zip(S, T):
            if np.dot(sq, tq) < 0:
                tq = -tq
            q = sq + (tq - sq) * w
            lr.append(q2m(q / np.linalg.norm(q)))
            lt.append(st + (tt - st) * w)
        _, P = fk(self.skel.parents, np.array(lr), np.array(lt))
        return {n: P[self.skel.index[n]] for n in ("Para_LeftHandle", "Para_RightHandle")}


def _axis_angle(axis, angle):
    n = np.linalg.norm(axis)
    if n < 1e-9 or abs(angle) < 1e-9:
        return np.eye(3)
    x, y, z = axis / n
    c, s, t = np.cos(angle), np.sin(angle), 1 - np.cos(angle)
    return np.array([[t * x * x + c, t * x * y - s * z, t * x * z + s * y],
                     [t * x * y + s * z, t * y * y + c, t * y * z - s * x],
                     [t * x * z - s * y, t * y * z + s * x, t * z * z + c]])


def _angle(a, b):
    return np.arccos(np.clip(np.dot(a, b) / max(np.linalg.norm(a) * np.linalg.norm(b), 1e-9), -1, 1))


def solve_arms(skel, frame, targets):
    """frame: (n, 10) Skyrim locals -> copy with both arms reaching their handle targets."""
    out = frame.copy()
    par = skel.parents
    for upper, fore, hand, handle in ARMS:
        u, f, h = skel.index[upper], skel.index[fore], skel.index[hand]
        LR = np.array([q2m(q) for q in out[:, 3:7]])
        LT = out[:, 0:3].astype(float)
        R, P = fk(par, LR, LT)
        hand_world = R[h].copy()
        s, e, hp, t = P[u], P[f], P[h], targets[handle]
        a, b = np.linalg.norm(e - s), np.linalg.norm(hp - e)
        d = np.clip(np.linalg.norm(t - s), abs(a - b) + 0.5, a + b - 0.5)
        axis = np.cross(s - e, hp - e)
        if np.linalg.norm(axis) < 1e-6:
            axis = np.cross(hp - s, [0.0, 0.0, 1.0])
        want = np.arccos(np.clip((a * a + b * b - d * d) / (2 * a * b), -1, 1))
        R[f] = _axis_angle(axis, want - _angle(s - e, hp - e)) @ R[f]
        LR[f] = R[u].T @ R[f]
        R, P = fk(par, LR, LT)
        swing = _axis_angle(np.cross(P[h] - s, t - s), _angle(P[h] - s, t - s))
        LR[u] = R[par[u]].T @ (swing @ R[u])
        R, P = fk(par, LR, LT)
        LR[h] = R[f].T @ hand_world  # keep the grip orientation
        for k in (u, f, h):
            out[k, 3:7] = m2q(LR[k])
    return out
