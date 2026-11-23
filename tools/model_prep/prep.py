# reforce extension (PLAN 5R.5, docs/model-design.md "Prep script"): turns a Blender scene into a .glb that reforce's
# model Objects can draw in real time.
#
#   blender -b IN.blend -P tools/model_prep/prep.py -- OUT.glb [--map MAP.json] [--drop NAME ...] [--tex 2048]
#                                                       [--front -y] [--decimate 0.5]
#
# 1. Applies every modifier (Mirror, Edge Split, Subdivision, ...) and removes cameras and lights; `--drop` removes
#    objects by name (fnmatch patterns, e.g. "Interior*"), say interiors that only show through windows.
# 2. Rewrites each material as one Principled BSDF that the glTF exporter understands:
#      - MAP.json names the images per material (renderer-specific node trees can't be converted automatically):
#          {"Hull": {"base": "HullColor.png", "normal": "HullNormal.png", "emissive": "HullLights.png",
#                    "roughness": 0.45, "metallic": 0.2, "emissiveStrength": 1.0}}
#        Material names may be fnmatch patterns; image names are Blender image names (packed or not).
#      - otherwise: a material that already has a Principled BSDF is kept; else its image textures are guessed by
#        name (normal / emiss|glow|light|window / anything else = base color), its first diffuse color is used,
#        and an Emission shader becomes the emissive color.
# 3. Halves textures until they fit `--tex` pixels a side (default 2048).
# 4. Optional `--decimate R` (0..1) keeps that share of the triangles.
# 5. Exports .glb with tangents and prints a report, including the Front="..." line for the scene file: `--front` is
#    the model's forward axis in Blender coordinates (default -y, Blender's usual front), converted to glTF axes.
#
# The model and MAP.json stay in your model folder (reforce.Model.Dir); never commit them.
import bpy
import mathutils
import fnmatch
import json
import sys


def args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if not argv:
        sys.exit("usage: blender -b IN.blend -P prep.py -- OUT.glb [--map MAP.json] [--drop NAME ...] [--tex N] "
                 "[--front AXIS] [--decimate R]")
    out = {"out": argv[0], "map": None, "drop": [], "tex": 2048, "front": "-y", "decimate": None}
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--map":
            out["map"] = argv[i + 1]; i += 2
        elif a == "--tex":
            out["tex"] = int(argv[i + 1]); i += 2
        elif a == "--front":
            out["front"] = argv[i + 1]; i += 2
        elif a == "--decimate":
            out["decimate"] = float(argv[i + 1]); i += 2
        elif a == "--drop":
            i += 1
            while i < len(argv) and not argv[i].startswith("--"):
                out["drop"].append(argv[i]); i += 1
        else:
            sys.exit("unknown option " + a)
    return out


def apply_modifiers(decimate):
    dg = bpy.context.evaluated_depsgraph_get()
    for ob in [o for o in bpy.data.objects if o.type == "MESH"]:
        if decimate:
            m = ob.modifiers.new("reforce decimate", "DECIMATE")
            m.ratio = decimate
            dg.update()
        if not ob.modifiers:
            continue
        ev = ob.evaluated_get(dg)
        me = bpy.data.meshes.new_from_object(ev, preserve_all_data_layers=True, depsgraph=dg)
        ob.modifiers.clear()
        ob.data = me


def drop_objects(patterns):
    gone = []
    for ob in list(bpy.data.objects):
        if ob.type in ("CAMERA", "LIGHT") or any(fnmatch.fnmatchcase(ob.name, p) for p in patterns):
            gone.append(ob.name)
            bpy.data.objects.remove(ob, do_unlink=True)
    return gone


def principled(mat):
    for n in mat.node_tree.nodes:
        if n.type == "BSDF_PRINCIPLED":
            return n
    return None


def rebuild(mat, spec):
    """Replaces mat's node tree with a Principled BSDF fed by spec's images."""
    nt = mat.node_tree
    old_color, old_emit, old_strength = (0.8, 0.8, 0.8, 1.0), None, 1.0
    for n in nt.nodes:
        if n.type in ("BSDF_DIFFUSE", "BSDF_PRINCIPLED") and "Color" in n.inputs and not n.inputs["Color"].is_linked:
            old_color = tuple(n.inputs["Color"].default_value)
        if n.type == "BSDF_PRINCIPLED" and not n.inputs["Base Color"].is_linked:
            old_color = tuple(n.inputs["Base Color"].default_value)
        if n.type == "EMISSION":
            old_emit = tuple(n.inputs["Color"].default_value)
            old_strength = n.inputs["Strength"].default_value
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    bsdf.inputs["Base Color"].default_value = old_color
    bsdf.inputs["Roughness"].default_value = float(spec.get("roughness", 0.5))
    bsdf.inputs["Metallic"].default_value = float(spec.get("metallic", 0.0))

    def image(name, color):
        if not name:
            return None
        img = bpy.data.images.get(name)
        if img is None:
            print("  ! image not found:", name)
            return None
        img.colorspace_settings.name = "sRGB" if color else "Non-Color"
        node = nt.nodes.new("ShaderNodeTexImage")
        node.image = img
        return node

    base = image(spec.get("base"), True)
    if base:
        nt.links.new(base.outputs["Color"], bsdf.inputs["Base Color"])
    normal = image(spec.get("normal"), False)
    if normal:
        nm = nt.nodes.new("ShaderNodeNormalMap")
        nt.links.new(normal.outputs["Color"], nm.inputs["Color"])
        nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
    emissive = image(spec.get("emissive"), True)
    if emissive:
        nt.links.new(emissive.outputs["Color"], bsdf.inputs["Emission Color"])
        bsdf.inputs["Emission Strength"].default_value = float(spec.get("emissiveStrength", 1.0))
    elif old_emit is not None and spec.get("keepEmission", True):
        bsdf.inputs["Emission Color"].default_value = old_emit
        bsdf.inputs["Emission Strength"].default_value = min(float(old_strength), 10.0)


def guess(mat):
    """A spec for a material with no MAP.json entry, from its image names."""
    spec = {}
    for n in mat.node_tree.nodes:
        if n.type != "TEX_IMAGE" or not n.image:
            continue
        name = n.image.name
        low = name.lower()
        if "normal" in low or low.endswith("_n.png") or "nrm" in low:
            spec.setdefault("normal", name)
        elif any(w in low for w in ("emiss", "glow", "light", "window")):
            spec.setdefault("emissive", name)
        else:
            spec.setdefault("base", name)
    return spec


def materials(mapping):
    report = []
    for mat in bpy.data.materials:
        if not mat.use_nodes or not mat.users:
            continue
        spec = None
        for pat, s in (mapping or {}).items():
            if fnmatch.fnmatchcase(mat.name, pat):
                spec = s
                break
        if spec is not None:
            rebuild(mat, spec)
            report.append((mat.name, "mapped", spec))
        elif principled(mat) is not None and len(mat.node_tree.nodes) <= 12:
            report.append((mat.name, "kept", {}))
        else:
            spec = guess(mat)
            rebuild(mat, spec)
            report.append((mat.name, "guessed", spec))
    return report


def shrink_textures(limit):
    done = []
    for img in bpy.data.images:
        w, h = img.size[:]
        if w == 0 or h == 0 or img.source not in ("FILE", "GENERATED"):
            continue
        nw, nh = w, h
        while nw > limit or nh > limit:
            nw, nh = max(1, nw // 2), max(1, nh // 2)
        if (nw, nh) != (w, h):
            img.scale(nw, nh)
            done.append((img.name, w, h, nw, nh))
    return done


def gltf_axis(blender_axis):
    """A Blender axis ("-y") as the exported glTF axis (glTF x = x, y = Blender z, z = -Blender y)."""
    s = blender_axis.strip().lower()
    sign = -1 if s.startswith("-") else 1
    a = s.lstrip("+-")
    table = {"x": ("x", 1), "y": ("z", -1), "z": ("y", 1)}
    if a not in table:
        sys.exit("bad --front " + blender_axis)
    axis, flip = table[a]
    return ("-" if sign * flip < 0 else "+") + axis


def main():
    a = args()
    mapping = None
    if a["map"]:
        with open(a["map"]) as f:
            mapping = json.load(f)
    if bpy.context.object and bpy.context.object.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")
    gone = drop_objects(a["drop"])
    apply_modifiers(a["decimate"])
    mats = materials(mapping)
    shrunk = shrink_textures(a["tex"])

    kw = dict(filepath=a["out"], export_format="GLB", export_tangents=True, export_apply=True,
              export_cameras=False, export_lights=False, use_selection=False)
    try:
        bpy.ops.export_scene.gltf(**kw)
    except TypeError:
        kw.pop("export_lights", None)
        bpy.ops.export_scene.gltf(**kw)

    tris = 0
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    for o in meshes:
        o.data.calc_loop_triangles()
        tris += len(o.data.loop_triangles)
    print("=== reforce model prep:", a["out"])
    print("  objects: %d, triangles: %d" % (len(meshes), tris))
    if gone:
        print("  removed:", ", ".join(gone))
    for name, how, spec in mats:
        print("  material %-28s %-8s %s" % (name, how, ", ".join("%s=%s" % kv for kv in spec.items())))
    for name, w, h, nw, nh in shrunk:
        print("  texture %s: %dx%d -> %dx%d" % (name, w, h, nw, nh))
    lo = [min(v[k] for o in meshes for v in (o.matrix_world @ c for c in [mathutils.Vector(b) for b in o.bound_box])) for k in range(3)]
    hi = [max(v[k] for o in meshes for v in (o.matrix_world @ c for c in [mathutils.Vector(b) for b in o.bound_box])) for k in range(3)]
    print("  size in Blender x, y, z: %.2f x %.2f x %.2f (the front is usually along the longest)" %
          (hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]))
    print('  scene file: Front="%s"   (assuming the model faces Blender %s; --front to change)' % (gltf_axis(a["front"]), a["front"]))


main()
