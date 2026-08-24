"""Rebake a texture from a model's classic .md2 UV layout into its rerelease
.md5 UV layout.

    python rebake.py <model-dir> <suffix> [--normal] [--size N]

    python rebake.py models/monsters/soldier _n --normal
    python rebake.py models/monsters/soldier ""            (albedo)

Why this is possible at all: the rerelease's MD5 meshes are fully re-unwrapped -
their texture coordinates share nothing with the MD2's (measured mean |duv| of
0.57-0.75 across seven models, where unrelated layouts average ~0.67). But the
ARTWORK is the same painting on a different unwrap, so Q2RTX's hand-authored HD
maps can be resampled into the new layout by going through 3D.

For every texel of the output:

    md5 texel -> (u,v) in md5 layout -> 3D point on the md5 mesh, posed to
    animation frame 0 -> closest point on the md2 mesh at frame 0 -> that
    point's (s,t) in the classic layout -> sample the source texture

Frame 0 is the common pose: the md5anim frame count matches the md2 frame count
one for one, which is what makes the two meshes comparable at all.

NORMAL MAPS need more than a colour copy. A tangent-space normal is relative to
its own mesh's tangent frame, and the two meshes have different UV layouts and
therefore different tangents. So the source normal is decoded with the MD2's
tangent frame at the sampled point and re-encoded with the MD5's frame at the
target texel. Pass --normal to do that; without it the pixels are copied as-is,
which is what you want for albedo and emissive.

LIMITATIONS, because this is a starting point to paint over and not a finished
asset: the two meshes are only within ~2 units of each other, so detail smears
wherever they disagree, and anywhere the MD5 has geometry the MD2 never had
there is nothing correct to sample. Check the coverage report it prints.
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from q2bsp import ROOT

try:
    from PIL import Image
except ImportError:
    sys.exit("this tool needs Pillow")


# --------------------------------------------------------------------------
# parsing (kept here so the tool is self-contained)
# --------------------------------------------------------------------------

def _tokens(text):
    import re
    text = re.sub(r"//[^\n]*", " ", text)
    for ch in "(){}":
        text = text.replace(ch, " %s " % ch)
    return re.findall(r'"[^"]*"|\S+', text)


def _quat_w(x, y, z):
    t = 1.0 - (x * x + y * y + z * z)
    return -np.sqrt(t) if t > 0 else 0.0


def _mat34(q, t):
    x, y, z, w = q
    xx, yy, zz = 2 * x * x, 2 * y * y, 2 * z * z
    xy, xz, yz = 2 * x * y, 2 * x * z, 2 * y * z
    wx, wy, wz = 2 * w * x, 2 * w * y, 2 * w * z
    return np.array([[1 - (yy + zz), xy - wz, xz + wy, t[0]],
                     [xy + wz, 1 - (xx + zz), yz - wx, t[1]],
                     [xz - wy, yz + wx, 1 - (xx + yy), t[2]]], dtype=np.float64)


def _mul34(a, b):
    out = np.zeros((3, 4))
    out[:, :3] = a[:, :3] @ b[:, :3]
    out[:, 3] = a[:, :3] @ b[:, 3] + a[:, 3]
    return out


def parse_md5mesh(path):
    t = _tokens(open(path, "r", errors="ignore").read())
    i = 0
    joints, meshes = [], []
    while i < len(t):
        if t[i] == "joints":
            i += 2
            while t[i] != "}":
                parent = int(t[i + 1])
                pos = [float(t[i + 3]), float(t[i + 4]), float(t[i + 5])]
                qx, qy, qz = float(t[i + 8]), float(t[i + 9]), float(t[i + 10])
                joints.append((parent, pos, (qx, qy, qz, _quat_w(qx, qy, qz))))
                i += 12
            i += 1
        elif t[i] == "mesh":
            i += 2
            verts, tris, weights = [], [], []
            while t[i] != "}":
                if t[i] == "vert":
                    verts.append(((float(t[i + 3]), float(t[i + 4])),
                                  int(t[i + 6]), int(t[i + 7])))
                    i += 8
                elif t[i] == "tri":
                    tris.append((int(t[i + 2]), int(t[i + 3]), int(t[i + 4])))
                    i += 5
                elif t[i] == "weight":
                    weights.append((int(t[i + 2]), float(t[i + 3]),
                                    [float(t[i + 5]), float(t[i + 6]), float(t[i + 7])]))
                    i += 9
                else:
                    i += 1
            meshes.append((verts, tris, weights))
            i += 1
        else:
            i += 1
    return joints, meshes


def parse_md5anim_frame0(path):
    t = _tokens(open(path, "r", errors="ignore").read())
    i = 0
    hier, base, frame0 = [], [], None
    while i < len(t):
        if t[i] == "hierarchy":
            i += 2
            while t[i] != "}":
                hier.append((int(t[i + 1]), int(t[i + 2]), int(t[i + 3])))
                i += 4
            i += 1
        elif t[i] == "baseframe":
            i += 2
            while t[i] != "}":
                base.append(([float(t[i + 1]), float(t[i + 2]), float(t[i + 3])],
                             [float(t[i + 6]), float(t[i + 7]), float(t[i + 8])]))
                i += 10
            i += 1
        elif t[i] == "bounds":
            i += 2
            while t[i] != "}":
                i += 1
            i += 1
        elif t[i] == "frame":
            if int(t[i + 1]) != 0:
                i += 2
                continue
            i += 3
            vals = []
            while t[i] != "}":
                vals.append(float(t[i]))
                i += 1
            frame0 = vals
            break
        else:
            i += 1
    return hier, base, frame0


def md5_frame0_positions(mesh_path, anim_path):
    """Skinned vertex positions, UVs and triangles for animation frame 0."""
    joints, meshes = parse_md5mesh(mesh_path)
    hier, base, vals = parse_md5anim_frame0(anim_path)

    bind = [_mat34(j[2], j[1]) for j in joints]

    anim = []
    for j, (parent, flags, off) in enumerate(hier):
        pos = list(base[j][0])
        q = list(base[j][1])
        k = off
        for bit, arr, idx in ((1, pos, 0), (2, pos, 1), (4, pos, 2),
                              (8, q, 0), (16, q, 1), (32, q, 2)):
            if flags & bit:
                arr[idx] = vals[k]
                k += 1
        m = _mat34((q[0], q[1], q[2], _quat_w(*q)), pos)
        anim.append(_mul34(anim[parent], m) if parent >= 0 else m)

    P, UV, TRI = [], [], []
    base_v = 0
    for verts, tris, weights in meshes:
        for (uv, w0, wn) in verts:
            p = np.zeros(3)
            for w in range(w0, w0 + wn):
                ji, bias, wp = weights[w]
                wp = np.array(wp)
                p += bias * (anim[ji][:, :3] @ wp + anim[ji][:, 3])
            P.append(p)
            UV.append(uv)
        for a, b, c in tris:
            TRI.append((a + base_v, b + base_v, c + base_v))
        base_v += len(verts)
    return np.array(P), np.array(UV, dtype=np.float64), np.array(TRI, dtype=np.int32)


def md2_frame0(path):
    import struct
    d = open(path, "rb").read()
    (_, _, skinw, skinh, framesize, _, nxyz, nst, ntris,
     _, _, _, ofs_st, ofs_tris, ofs_frames, _, _) = struct.unpack("<17i", d[:68])
    st = np.array([struct.unpack("<hh", d[ofs_st + i * 4: ofs_st + i * 4 + 4])
                   for i in range(nst)], dtype=np.float64)
    st[:, 0] /= skinw
    st[:, 1] /= skinh
    tris = np.array([struct.unpack("<6H", d[ofs_tris + i * 12: ofs_tris + i * 12 + 12])
                     for i in range(ntris)], dtype=np.int32)
    o = ofs_frames
    sc = np.array(struct.unpack("<3f", d[o:o + 12]))
    tr = np.array(struct.unpack("<3f", d[o + 12:o + 24]))
    verts = np.frombuffer(d[o + 40: o + 40 + nxyz * 4], dtype=np.uint8).reshape(nxyz, 4)
    P = verts[:, :3].astype(np.float64) * sc + tr

    # expand to per-corner so position and uv share an index
    corner_p = P[tris[:, 0:3].reshape(-1)]
    corner_uv = st[tris[:, 3:6].reshape(-1)]
    weld = tris[:, 0:3].reshape(-1).astype(np.int32)   # shared xyz index
    T = np.arange(len(corner_p), dtype=np.int32).reshape(-1, 3)
    return corner_p, corner_uv, T, weld


# --------------------------------------------------------------------------
# geometry
# --------------------------------------------------------------------------

def tangent_frames(P, UV, TRI, weld=None):
    """Per-VERTEX (T, N), accumulated over incident triangles.

    Per-triangle frames would make each triangle re-encode the same world normal
    into a different basis, which reads as hard faceting. The renderer
    interpolates per-vertex tangents, so this does too. (weld) optionally maps
    each vertex onto a shared index so that duplicates split by a UV seam still
    average to one smooth normal.
    """
    p0, p1, p2 = P[TRI[:, 0]], P[TRI[:, 1]], P[TRI[:, 2]]
    t0, t1, t2 = UV[TRI[:, 0]], UV[TRI[:, 1]], UV[TRI[:, 2]]
    e1, e2 = p1 - p0, p2 - p0
    d1, d2 = t1 - t0, t2 - t0

    det = d1[:, 0] * d2[:, 1] - d2[:, 0] * d1[:, 1]
    det = np.where(np.abs(det) < 1e-12, 1e-12, det)
    r = (1.0 / det)[:, None]
    face_T = (e1 * d2[:, 1:2] - e2 * d1[:, 1:2]) * r

    # outward normal, same convention the md2/md5 loaders use
    face_N = np.cross(e2, e1)

    nv = len(P)
    accT = np.zeros((nv, 3))
    accN = np.zeros((nv, 3))
    for k in range(3):
        np.add.at(accT, TRI[:, k], face_T)
        np.add.at(accN, TRI[:, k], face_N)

    if weld is not None:
        nw = int(weld.max()) + 1
        wT = np.zeros((nw, 3))
        wN = np.zeros((nw, 3))
        np.add.at(wT, weld, accT)
        np.add.at(wN, weld, accN)
        accT, accN = wT[weld], wN[weld]

    N = accN / np.maximum(np.linalg.norm(accN, axis=1, keepdims=True), 1e-20)
    T = accT - N * np.sum(accT * N, axis=1, keepdims=True)
    T /= np.maximum(np.linalg.norm(T, axis=1, keepdims=True), 1e-20)
    B = np.cross(N, T)
    return T, B, N


def closest_point_on_triangles(pts, a, b, c):
    """Closest point on ONE triangle (a,b,c) for many pts. Returns (dist2, bary)."""
    ab, ac = b - a, c - a
    ap = pts - a
    d1 = ap @ ab
    d2 = ap @ ac

    bp = pts - b
    d3 = bp @ ab
    d4 = bp @ ac

    cp = pts - c
    d5 = cp @ ab
    d6 = cp @ ac

    vc = d1 * d4 - d3 * d2
    vb = d5 * d2 - d1 * d6
    va = d3 * d6 - d5 * d4
    denom = va + vb + vc
    denom = np.where(np.abs(denom) < 1e-20, 1e-20, denom)

    v = vb / denom
    w = vc / denom

    # clamp to the triangle by regions
    v = np.clip(v, 0.0, 1.0)
    w = np.clip(w, 0.0, 1.0)
    over = v + w > 1.0
    if np.any(over):
        s = v[over] + w[over]
        v[over] /= s
        w[over] /= s
    u = 1.0 - v - w

    q = a + v[:, None] * ab + w[:, None] * ac
    d = q - pts
    return np.sum(d * d, axis=1), np.stack([u, v, w], axis=1)


def rasterize_uv(UV, TRI, size, pad=1.0):
    """For each texel covered by a UV triangle, return (texel index, tri index, bary)."""
    idx_list, tri_list, bary_list = [], [], []
    uvpx = UV * size
    for ti in range(len(TRI)):
        t = TRI[ti]
        p = uvpx[t]
        x0 = max(int(np.floor(p[:, 0].min() - pad)), 0)
        x1 = min(int(np.ceil(p[:, 0].max() + pad)), size - 1)
        y0 = max(int(np.floor(p[:, 1].min() - pad)), 0)
        y1 = min(int(np.ceil(p[:, 1].max() + pad)), size - 1)
        if x1 < x0 or y1 < y0:
            continue
        xs, ys = np.meshgrid(np.arange(x0, x1 + 1), np.arange(y0, y1 + 1))
        px = xs.reshape(-1) + 0.5
        py = ys.reshape(-1) + 0.5

        v0 = p[1] - p[0]
        v1 = p[2] - p[0]
        den = v0[0] * v1[1] - v1[0] * v0[1]
        if abs(den) < 1e-12:
            continue
        v2x = px - p[0, 0]
        v2y = py - p[0, 1]
        b1 = (v2x * v1[1] - v1[0] * v2y) / den
        b2 = (v0[0] * v2y - v2x * v0[1]) / den
        b0 = 1.0 - b1 - b2
        eps = -0.35 / max(size, 1) * size  # allow a little bleed into the gutter
        inside = (b0 >= eps) & (b1 >= eps) & (b2 >= eps)
        if not np.any(inside):
            continue
        sel = np.where(inside)[0]
        idx_list.append((ys.reshape(-1)[sel] * size + xs.reshape(-1)[sel]).astype(np.int64))
        tri_list.append(np.full(len(sel), ti, dtype=np.int32))
        bary_list.append(np.stack([b0[sel], b1[sel], b2[sel]], axis=1))
    if not idx_list:
        return None
    return (np.concatenate(idx_list), np.concatenate(tri_list), np.concatenate(bary_list))


def dilate(img, mask, iterations=8):
    """Push colour outward into unwritten texels so mip-mapping does not eat edges."""
    h, w = mask.shape
    out = img.copy()
    m = mask.copy()
    for _ in range(iterations):
        if m.all():
            break
        acc = np.zeros_like(out, dtype=np.float64)
        cnt = np.zeros((h, w), dtype=np.float64)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (-1, 1), (1, -1), (1, 1)):
            sm = np.roll(np.roll(m, dy, axis=0), dx, axis=1)
            si = np.roll(np.roll(out, dy, axis=0), dx, axis=1)
            acc += si * sm[:, :, None]
            cnt += sm
        fill = (~m) & (cnt > 0)
        out[fill] = acc[fill] / cnt[fill][:, None]
        m = m | fill
    return out


# --------------------------------------------------------------------------

def main():
    args = [a for a in sys.argv[1:]]
    if not args:
        print(__doc__)
        return
    is_normal = "--normal" in args
    if is_normal:
        args.remove("--normal")
    size = 512
    if "--size" in args:
        i = args.index("--size")
        size = int(args[i + 1])
        del args[i:i + 2]

    model_dir = args[0].replace("\\", "/").rstrip("/")
    stem = args[1] if len(args) > 1 else "skin"
    # --normal means "rebake this stem's normal map", i.e. <stem>_n
    src_stem = stem + "_n" if is_normal else stem

    base = os.path.join(ROOT, "rerelease", model_dir.replace("/", os.sep))
    md5dir = os.path.join(base, "md5")
    mesh = os.path.join(md5dir, "tris.md5mesh")
    anim = os.path.join(md5dir, "tris.md5anim")

    md2 = None
    for cand in ("tris.md2", "tris.MD2"):
        if os.path.exists(os.path.join(base, cand)):
            md2 = os.path.join(base, cand)
            break
    if not md2:
        # the classic model is normally only inside a pak
        from q2bsp import pak_list, pak_extract
        tmp = os.path.join(ROOT, "tools", "rerelease", "__pycache__")
        os.makedirs(tmp, exist_ok=True)
        for pak in (os.path.join(ROOT, "baseq2", "pak0.pak"),
                    "D:/SteamLibrary/steamapps/common/Quake 2/rerelease/baseq2/pak0.pak"):
            if not os.path.exists(pak):
                continue
            names = {e[0].lower(): e[0] for e in pak_list(pak)}
            for cand in ("%s/tris.md2" % model_dir, "%s/tris.MD2" % model_dir):
                real = names.get(cand.lower())
                if real:
                    md2 = os.path.join(tmp, "rebake_src.md2")
                    pak_extract(pak, real, md2)
                    print("md2 from  %s : %s" % (os.path.basename(pak), real))
                    break
            if md2:
                break
    for p, what in ((mesh, "md5mesh"), (anim, "md5anim")):
        if not os.path.exists(p):
            sys.exit("missing %s: %s" % (what, p))
    if not md2:
        sys.exit("no .md2 beside %s" % base)

    # the source texture, in the classic layout
    src_path = None
    for ext in (".tga", ".png", ".pcx"):
        cand = os.path.join(base, src_stem + ext)
        if os.path.exists(cand):
            src_path = cand
            break
    if not src_path:
        sys.exit("no source texture %s.* in %s" % (src_stem, base))

    src_img = Image.open(src_path)
    had_alpha = src_img.mode in ("RGBA", "LA")
    src_rgba = np.asarray(src_img.convert("RGBA"), dtype=np.float64) / 255.0
    src = src_rgba[:, :, :3]
    src_a = src_rgba[:, :, 3]
    print("source     %s  %dx%d  alpha=%s"
          % (os.path.basename(src_path), src.shape[1], src.shape[0],
             "yes (mean %.2f)" % src_a.mean() if had_alpha else "none"))

    P5, UV5, TRI5 = md5_frame0_positions(mesh, anim)
    P2, UV2, TRI2, WELD2 = md2_frame0(md2)
    print("md5 %d verts / %d tris     md2 %d tris" % (len(P5), len(TRI5), len(TRI2)))

    raster = rasterize_uv(UV5, TRI5, size)
    if raster is None:
        sys.exit("nothing rasterized")
    texel, tri, bary = raster
    print("texels covered: %d of %d (%.1f%%)"
          % (len(texel), size * size, 100.0 * len(np.unique(texel)) / (size * size)))

    # 3D point of every covered texel, in the frame-0 pose
    tv = TRI5[tri]
    pts = (P5[tv[:, 0]] * bary[:, 0:1] + P5[tv[:, 1]] * bary[:, 1:2] + P5[tv[:, 2]] * bary[:, 2:3])

    # closest point on the md2 surface, brute force over its few hundred triangles
    a = P2[TRI2[:, 0]]
    b = P2[TRI2[:, 1]]
    c = P2[TRI2[:, 2]]
    best_d2 = np.full(len(pts), np.inf)
    best_tri = np.zeros(len(pts), dtype=np.int32)
    best_bary = np.zeros((len(pts), 3))
    for ti in range(len(TRI2)):
        d2, bary2 = closest_point_on_triangles(pts, a[ti], b[ti], c[ti])
        upd = d2 < best_d2
        if np.any(upd):
            best_d2[upd] = d2[upd]
            best_tri[upd] = ti
            best_bary[upd] = bary2[upd]
    dist = np.sqrt(best_d2)
    print("md5->md2 surface distance: mean %.2f  median %.2f  p95 %.2f units"
          % (dist.mean(), np.median(dist), np.percentile(dist, 95)))

    # the classic (s,t) at that point
    t2 = TRI2[best_tri]
    st = (UV2[t2[:, 0]] * best_bary[:, 0:1] + UV2[t2[:, 1]] * best_bary[:, 1:2]
          + UV2[t2[:, 2]] * best_bary[:, 2:3])

    sh, sw = src.shape[0], src.shape[1]
    sx = np.clip((st[:, 0] * sw).astype(np.int32), 0, sw - 1)
    sy = np.clip((st[:, 1] * sh).astype(np.int32), 0, sh - 1)
    sample = src[sy, sx]
    sample_a = src_a[sy, sx]

    if is_normal:
        T2, B2, N2 = tangent_frames(P2, UV2, TRI2, WELD2)
        T5, B5, N5 = tangent_frames(P5, UV5, TRI5)

        def lerp_frame(F, TRIx, idx, bary3):
            t = TRIx[idx]
            v = (F[t[:, 0]] * bary3[:, 0:1] + F[t[:, 1]] * bary3[:, 1:2]
                 + F[t[:, 2]] * bary3[:, 2:3])
            return v / np.maximum(np.linalg.norm(v, axis=1, keepdims=True), 1e-20)

        # frames interpolated across the triangle, as the shader does
        t2i = lerp_frame(T2, TRI2, best_tri, best_bary)
        n2i = lerp_frame(N2, TRI2, best_tri, best_bary)
        b2i = np.cross(n2i, t2i)
        t5i = lerp_frame(T5, TRI5, tri, bary)
        n5i = lerp_frame(N5, TRI5, tri, bary)
        b5i = np.cross(n5i, t5i)

        n_tan = sample * 2.0 - 1.0
        n_tan[:, 2] = np.abs(n_tan[:, 2])
        # decode with the md2 frame at the sampled point...
        world = (t2i * n_tan[:, 0:1] + b2i * n_tan[:, 1:2] + n2i * n_tan[:, 2:3])
        world /= np.maximum(np.linalg.norm(world, axis=1, keepdims=True), 1e-20)
        # ...and re-encode with the md5 frame at the target texel
        enc = np.stack([np.sum(world * t5i, axis=1),
                        np.sum(world * b5i, axis=1),
                        np.sum(world * n5i, axis=1)], axis=1)
        enc /= np.maximum(np.linalg.norm(enc, axis=1, keepdims=True), 1e-20)
        enc[:, 2] = np.abs(enc[:, 2])
        sample = enc * 0.5 + 0.5

    img = np.zeros((size, size, 3), dtype=np.float64)
    mask = np.zeros(size * size, dtype=bool)
    flat = img.reshape(-1, 3)
    # nearest-surface wins where two triangles claim a texel
    order = np.argsort(-dist)
    flat[texel[order]] = sample[order]
    mask[texel] = True

    img = dilate(img, mask.reshape(size, size))

    out_path = os.path.join(md5dir, src_stem + ".tga")
    Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8)).save(out_path)
    print("wrote %s  (%dx%d)" % (out_path, size, size))

    # The alpha of a Q2RTX texture is not transparency: the base texture's alpha
    # is roughness and the normal map's alpha is metallic. A rebake that drops
    # them leaves alpha at 255, which reads as fully metallic and turns the
    # model into dark metal. Write them out as the separate maps the material
    # system now takes, which is also just far easier to edit.
    if had_alpha:
        chan = np.zeros(size * size, dtype=np.float64)
        chan[texel[order]] = sample_a[order]
        chan_img = dilate(np.repeat(chan.reshape(size, size, 1), 3, axis=2),
                          mask.reshape(size, size))
        side = "_metallic" if is_normal else "_rough"
        side_path = os.path.join(md5dir, stem + side + ".tga")
        Image.fromarray((np.clip(chan_img[:, :, 0], 0, 1) * 255).astype(np.uint8)).save(side_path)
        print("wrote %s  (was the alpha of the source)" % side_path)


if __name__ == "__main__":
    main()
