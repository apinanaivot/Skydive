"""Retargets JC2 (Rico, biped.bsk) animations onto the Skyrim character skeleton.

Rico's bind pose is a T-pose with identity bone rotations, so a JC2 bone's world rotation in an
animation is directly its rotation away from the T-pose. The Skyrim skeleton is first bent into
the same T-pose (each mapped bone swung so it points at its child the way Rico's does), which
gives every Skyrim bone a T-pose world rotation O. Then per frame:
    Skyrim world rotation = C * JC2 world rotation * C^T * O
with C the axis change JC2 (Y up, facing -Z, meters) -> Skyrim (Z up, facing +Y, 70 units/m).
Bones without a JC2 partner keep their bind rotation relative to their parent. Only the COM
gets a translation (Rico's hips, scaled by the hip height ratio); bone lengths stay Skyrim's."""
import numpy as np

UNITS_PER_M = 70.0
C = np.array([[1.0, 0.0, 0.0], [0.0, 0.0, -1.0], [0.0, 1.0, 0.0]])  # skyrim = C @ jc2

# Skyrim bone -> JC2 bone. Skyrim has three spine bones to Rico's two and separate twist bones.
MAP = {
    "NPC COM [COM ]": "Hips",
    "NPC Pelvis [Pelv]": "Hips",
    "NPC Spine [Spn0]": "Spine",
    "NPC Spine1 [Spn1]": "Spine1",
    "NPC Spine2 [Spn2]": "Spine1",
    "NPC Neck [Neck]": "Neck",
    "NPC Head [Head]": "Head",
}
for side, s in (("L", "Left"), ("R", "Right")):
    MAP.update({
        "NPC %s Clavicle [%sClv]" % (side, side): s + "Shoulder",
        "NPC %s UpperArm [%sUar]" % (side, side): s + "Arm",
        "NPC %s UpperarmTwist1 [%sUt1]" % (side, side): s + "Arm",
        "NPC %s UpperarmTwist2 [%sUt2]" % (side, side): s + "ArmRoll",
        "NPC %s Forearm [%sLar]" % (side, side): s + "ForeArm",
        "NPC %s ForearmTwist1 [%sLt1]" % (side, side): s + "ForeArmRoll",
        "NPC %s ForearmTwist2 [%sLt2]" % (side, side): s + "ForeArmRoll",
        "NPC %s Hand [%sHnd]" % (side, side): s + "Hand",
        "NPC %s Thigh [%sThg]" % (side, side): s + "UpLeg",
        "NPC %s Calf [%sClf]" % (side, side): s + "Leg",
        "NPC %s Foot [%sft ]" % (side, side): s + "Foot",
        "NPC %s Toe0 [%sToe]" % (side, side): s + "ToeBase",
    })
    for k, finger in enumerate(("Thumb", "Index", "Middle", "Ring", "Pinky")):
        for j in range(3):
            if finger != "Pinky" or j < 2:  # Rico has two pinky bones
                MAP["NPC %s Finger%d%d [%sF%d%d]" % (side, k, j, side, k, j)] = "%sHand%s%d" % (s, finger, j + 1)

# Direction pairs used to bend Skyrim's bind pose into Rico's T-pose: bone -> child it points at.
AIM = {}
for side in "LR":
    AIM.update({
        "NPC %s Clavicle [%sClv]" % (side, side): "NPC %s UpperArm [%sUar]" % (side, side),
        "NPC %s UpperArm [%sUar]" % (side, side): "NPC %s Forearm [%sLar]" % (side, side),
        "NPC %s Forearm [%sLar]" % (side, side): "NPC %s Hand [%sHnd]" % (side, side),
        "NPC %s Hand [%sHnd]" % (side, side): "NPC %s Finger20 [%sF20]" % (side, side),
        "NPC %s Thigh [%sThg]" % (side, side): "NPC %s Calf [%sClf]" % (side, side),
        "NPC %s Calf [%sClf]" % (side, side): "NPC %s Foot [%sft ]" % (side, side),
        "NPC %s Foot [%sft ]" % (side, side): "NPC %s Toe0 [%sToe]" % (side, side),
    })
    for k in range(5):
        for j in range(2):
            AIM["NPC %s Finger%d%d [%sF%d%d]" % (side, k, j, side, k, j)] = \
                "NPC %s Finger%d%d [%sF%d%d]" % (side, k, j + 1, side, k, j + 1)
AIM.update({
    "NPC Spine [Spn0]": "NPC Spine1 [Spn1]",
    "NPC Spine1 [Spn1]": "NPC Spine2 [Spn2]",
    "NPC Spine2 [Spn2]": "NPC Neck [Neck]",
    "NPC Neck [Neck]": "NPC Head [Head]",
})


# --- quaternion / matrix helpers (x, y, z, w) ---------------------------------------------------

def q2m(q):
    x, y, z, w = q
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ])


def m2q(m):
    t = np.trace(m)
    if t > 0:
        s = np.sqrt(t + 1.0) * 2
        q = [(m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, 0.25 * s]
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = np.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        q = [0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s, (m[2, 1] - m[1, 2]) / s]
    elif m[1, 1] > m[2, 2]:
        s = np.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        q = [(m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s, (m[0, 2] - m[2, 0]) / s]
    else:
        s = np.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        q = [(m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s, (m[1, 0] - m[0, 1]) / s]
    q = np.array(q)
    return q / np.linalg.norm(q)


def swing(a, b):
    """Rotation matrix taking direction a onto direction b."""
    a, b = a / np.linalg.norm(a), b / np.linalg.norm(b)
    v, c = np.cross(a, b), np.dot(a, b)
    if c < -0.999999:
        axis = np.cross(a, [1.0, 0.0, 0.0])
        if np.linalg.norm(axis) < 1e-6:
            axis = np.cross(a, [0.0, 1.0, 0.0])
        axis /= np.linalg.norm(axis)
        return 2 * np.outer(axis, axis) - np.eye(3)
    k = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + k + k @ k / (1 + c)


def fk(parents, local_r, local_t):
    """World rotations (n, 3, 3) and positions (n, 3) from local ones (parents precede children)."""
    n = len(parents)
    R, P = np.zeros((n, 3, 3)), np.zeros((n, 3))
    for i in range(n):
        p = parents[i]
        if p < 0:
            R[i], P[i] = local_r[i], local_t[i]
        else:
            R[i] = R[p] @ local_r[i]
            P[i] = P[p] + R[p] @ local_t[i]
    return R, P


class Retargeter:
    def __init__(self, jc2_skel, sky_skel):
        self.j, self.s = jc2_skel, sky_skel
        sp = sky_skel.parents
        self.sky_local_r = np.array([q2m(q) for q in sky_skel.ref[:, 3:7]])
        self.sky_local_t = sky_skel.ref[:, 0:3].astype(np.float64)
        jr = np.array([q2m(q) for q in jc2_skel.ref[:, 3:7]])
        _, jp = fk(jc2_skel.parents, jr, jc2_skel.ref[:, 0:3].astype(np.float64))
        self.jc2_rest_pos = jp

        # Bend Skyrim's bind pose into Rico's T-pose, parents first.
        local_r = self.sky_local_r.copy()
        for i, name in enumerate(sky_skel.names):
            child = AIM.get(name)
            if child is None or name not in MAP or child not in MAP:
                continue
            R, P = fk(sp, local_r, self.sky_local_t)
            c = sky_skel.index[child]
            want = C @ (jp[jc2_skel.index[MAP[child]]] - jp[jc2_skel.index[MAP[name]]])
            have = P[c] - P[i]
            if np.linalg.norm(want) < 1e-6 or np.linalg.norm(have) < 1e-6:
                continue
            Rw = swing(have, want) @ R[i]
            parent_r = R[sp[i]] if sp[i] >= 0 else np.eye(3)
            local_r[i] = parent_r.T @ Rw
        self.tpose_r, self.tpose_p = fk(sp, local_r, self.sky_local_t)
        com = sky_skel.index["NPC COM [COM ]"]
        self.com = com
        self.height_ratio = self.tpose_p[com][2] / (jp[jc2_skel.index["Hips"]][1] * UNITS_PER_M)

    def convert(self, anim):
        """JC2 Animation -> (F, n_sky, 10) Skyrim local transforms."""
        j, s = self.j, self.s
        track = {n: i for i, n in enumerate(anim.track_names)}
        F, n = len(anim.frames), len(s.names)
        out = np.zeros((F, n, 10), np.float32)
        jlocal_t = j.ref[:, 0:3].astype(np.float64)
        for f in range(F):
            # JC2 world rotations for this frame (bones without a track stay at bind).
            lr = np.array([q2m(q) for q in j.ref[:, 3:7]])
            lt = jlocal_t.copy()
            for name, ti in track.items():
                bi = j.index.get(name)
                if bi is not None:
                    lr[bi] = q2m(anim.frames[f, ti, 3:7])
                    lt[bi] = anim.frames[f, ti, 0:3]
            JR, JP = fk(j.parents, lr, lt)
            # Skyrim world rotations, then locals.
            SR = np.zeros((n, 3, 3))
            for i, name in enumerate(s.names):
                p = s.parents[i]
                if name in MAP and MAP[name] in j.index:
                    SR[i] = C @ JR[j.index[MAP[name]]] @ C.T @ self.tpose_r[i]
                else:
                    SR[i] = (SR[p] if p >= 0 else np.eye(3)) @ self.sky_local_r[i]
            for i in range(n):
                p = s.parents[i]
                local = SR[i] if p < 0 else SR[p].T @ SR[i]
                out[f, i, 0:3] = self.sky_local_t[i]
                out[f, i, 3:7] = m2q(local)
                out[f, i, 7:10] = 1.0
            # The COM carries Rico's hip position (relative to the JC2 reference bone).
            hips = JP[j.index["Hips"]] - JP[j.index["Reference"]]
            out[f, self.com, 0:3] = C @ hips * UNITS_PER_M * self.height_ratio
        return out
