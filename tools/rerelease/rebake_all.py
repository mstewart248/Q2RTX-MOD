"""Rebake Q2RTX's PBR maps into the MD5 UV layout for every model that needs it.

    python rebake_all.py --list     show the work list and stop
    python rebake_all.py --go       do it

Only models that are ACTUALLY USED are touched: at the default cl_md5_models 1
the rerelease model is only loaded where Q2RTX has no remastered .md3 of its
own, which in practice means the monsters. The 43 models Q2RTX replaces are
skipped - rebaking them would be wasted work.

For each skin it runs two passes:

    <stem>_n      -> md5/<stem>_n.tga        (normal, re-based into the new
                                              tangent frame)
                     md5/<stem>_metallic.tga (the source normal map's alpha)
    <stem>        -> md5/<stem>_rough.tga    (the base texture's alpha)

The albedo the second pass produces is DELETED again: the rerelease's own
md5/<stem>.png is authored for that unwrap and beats a resample. That pass is
only run to harvest the roughness hiding in the alpha channel.
"""

import os
import struct
import subprocess
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from q2bsp import ROOT, pak_list, pak_extract

PAK = r"D:/SteamLibrary/steamapps/common/Quake 2/rerelease/baseq2/pak0.pak"
HERE = os.path.dirname(os.path.abspath(__file__))


def find_asset(rel):
    for base in ("rerelease", "baseq2"):
        p = os.path.join(ROOT, base, rel.replace("/", os.sep))
        if os.path.exists(p):
            return p
    return None


def build_worklist():
    # md5 models present as loose files
    md5dirs = []
    for dp, dn, fn in os.walk(os.path.join(ROOT, "rerelease")):
        if "tris.md5mesh" in fn and os.path.basename(dp).lower() == "md5":
            rel = os.path.relpath(os.path.dirname(dp),
                                  os.path.join(ROOT, "rerelease")).replace("\\", "/")
            md5dirs.append(rel)

    # models Q2RTX replaces with its own .md3 are not loaded at cl_md5_models 1
    md3 = set()
    pkz = os.path.join(ROOT, "baseq2", "q2rtx_media.pkz")
    if os.path.exists(pkz):
        for n in zipfile.ZipFile(pkz).namelist():
            if n.lower().endswith(".md3"):
                md3.add(os.path.dirname(n))

    names = [e[0] for e in pak_list(PAK)]
    nameset = {n.lower(): n for n in names}
    tmp = os.path.join(HERE, "__pycache__")
    os.makedirs(tmp, exist_ok=True)

    work = []
    for d in sorted(md5dirs):
        if d in md3:
            continue
        md2 = nameset.get((d + "/tris.md2").lower()) or nameset.get((d + "/tris.MD2").lower())
        if not md2:
            continue
        p = os.path.join(tmp, "wl.md2")
        pak_extract(PAK, md2, p)
        raw = open(p, "rb").read()
        nsk = struct.unpack("<i", raw[20:24])[0]
        ofs = struct.unpack("<i", raw[44:48])[0]
        for i in range(nsk):
            s = raw[ofs + i * 64: ofs + i * 64 + 64].split(b"\0")[0]
            s = s.decode("latin-1").replace("\\", "/")
            stem_path = s.rsplit(".", 1)[0]
            stem = os.path.basename(stem_path)
            if not find_asset(stem_path + "_n.tga"):
                continue                        # no normal map to rebake from
            out = os.path.join(ROOT, "rerelease", d.replace("/", os.sep),
                               "md5", stem + "_n.tga")
            if os.path.exists(out):
                continue                        # already done
            if (d, stem) not in work:
                work.append((d, stem))
    return work


def run(model_dir, stem, extra):
    cmd = [sys.executable, os.path.join(HERE, "rebake.py"), model_dir, stem,
           "--size", "512"] + extra
    r = subprocess.run(cmd, capture_output=True, text=True)
    ok = r.returncode == 0 and "wrote" in r.stdout
    return ok, (r.stdout + r.stderr).strip().splitlines()[-1:] or [""]


def main():
    if "--list" not in sys.argv and "--go" not in sys.argv:
        print(__doc__)
        return
    work = build_worklist()
    print("%d skin(s) to rebake" % len(work))
    for d, stem in work:
        print("   %-42s %s" % (d, stem))
    if "--list" in sys.argv:
        return

    okn = okr = fail = 0
    for i, (d, stem) in enumerate(work, 1):
        print("[%d/%d] %s %s" % (i, len(work), d, stem), flush=True)

        # one bad skin must never take the batch down with it
        try:
            good, msg = run(d, stem, ["--normal"])
            if good:
                okn += 1
            else:
                fail += 1
                print("        normal FAILED: %s" % msg[-1], flush=True)
        except Exception as e:
            fail += 1
            print("        normal ERROR: %s" % e, flush=True)

        # second pass purely to harvest roughness from the base texture's alpha
        try:
            # This pass also writes an albedo we do not want. Only ever delete
            # it if we created it: one model (shambler) ships its base skin as
            # md5/skin.tga, and deleting that blindly would destroy a genuine
            # rerelease asset.
            stray = os.path.join(ROOT, "rerelease", d.replace("/", os.sep),
                                 "md5", stem + ".tga")
            preexisting = os.path.exists(stray)

            good, msg = run(d, stem, [])
            if good:
                okr += 1
                if not preexisting:
                    try:
                        os.remove(stray)   # never shadow the rerelease's own art
                    except OSError:
                        pass               # it was not produced; nothing to undo
            else:
                print("        rough  FAILED: %s" % msg[-1], flush=True)
        except Exception as e:
            print("        rough  ERROR: %s" % e, flush=True)

    print("done: %d normal+metallic, %d roughness, %d failures" % (okn, okr, fail))


if __name__ == "__main__":
    main()
