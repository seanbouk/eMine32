"""Intro map look-dev (truck -> yard -> Graveyard Shift fire exit -> Mine Shift), built headless in Blender.

    blender -b -P tools/intro_blockout.py -- [--render all|truck,wall,exit,staff,glass,top] [--samples N] [--flat]

Writes art/intro/intro_blockout.blend and renders to art/intro/renders/<view>.png (--flat: untextured grey boxes).
Textures come from art/intro/tex (python tools/intro_textures.py), already reduced to the game palette.
Built the way a Build map is: boxes with a texture per face (sectors and walls), billboards that turn to face you
(face sprites: barrel, fire, toilet, chair, bags, brick) and flat wall sprites (signs). No other geometry.

Units: 1 Blender unit = 512 Build units (one map-editor grid square). Plan x -> X, plan y (south, down) -> -Y.
Duke's eye is 38<<8 z = 608 xy units above the floor, so EYE = 1.19. Tiling textures are 32 texels a unit, Duke's
density at the default wall repeat.
"""
import bpy, bmesh, json, math, os, sys
from mathutils import Vector, Matrix

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'art', 'intro')
TEX = os.path.join(OUT, 'tex')
EYE = 608 / 512

argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
def arg(name, default=None):
    return argv[argv.index(name) + 1] if name in argv else default
RENDER = arg('--render', '')
SAMPLES = int(arg('--samples', '48'))
FLAT = '--flat' in argv
MANIFEST = json.load(open(os.path.join(TEX, 'manifest.json')))

# ---------------------------------------------------------------- scene
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
for name in ('Blockout', 'Sprites', 'Ceilings', 'Lights', 'Cameras'):
    scene.collection.children.link(bpy.data.collections.new(name))
COL = bpy.data.collections

def P(x, y, z=0.0):
    """plan coordinates (y down) -> Blender"""
    return Vector((x, -y, z))

# ---------------------------------------------------------------- materials
MATS, MODE = {}, {}
def _bsdf(m):
    b = m.node_tree.nodes['Principled BSDF']
    b.inputs['Roughness'].default_value = 1.0
    b.inputs['Specular IOR Level'].default_value = 0.0     # Build has no highlights
    return b

def mat(name, rgb, emit=None, strength=0.0):
    """flat colour (lamps, tubes, and everything in --flat)"""
    if name in MATS: return MATS[name]
    m = bpy.data.materials.new(name); m.use_nodes = True
    b = _bsdf(m)
    b.inputs['Base Color'].default_value = (*rgb, 1)
    if emit:
        b.inputs['Emission Color'].default_value = (*emit, 1)
        b.inputs['Emission Strength'].default_value = strength
    m.diffuse_color = (*rgb, 1)
    MATS[name] = m; MODE[name] = 'tile'
    return m

GREY = (0.3, 0.3, 0.3)
def T(name, strength=6.0):
    """a texture from the manifest: tile (repeats every 'bu'), fit (stretched over the face) or sprite (alpha)"""
    if name in MATS: return MATS[name]
    e = MANIFEST[name]
    if FLAT and e['mode'] != 'sprite' and not e.get('emit'):
        m = mat('flat_' + name, GREY); MATS[name] = m; return m
    m = bpy.data.materials.new(name); m.use_nodes = True
    nt = m.node_tree; b = _bsdf(m)
    uv = nt.nodes.new('ShaderNodeTexCoord')
    def image(fn):
        n = nt.nodes.new('ShaderNodeTexImage')
        n.image = bpy.data.images.load(os.path.join(TEX, fn + '.png'), check_existing=True)
        n.interpolation = 'Closest'                       # pixels, like the software renderer
        n.extension = 'REPEAT' if e['mode'] == 'tile' else 'EXTEND'
        if e['mode'] == 'tile':
            mp = nt.nodes.new('ShaderNodeMapping')
            mp.inputs['Scale'].default_value = (1 / e['bu'], 1 / e['bu'], 1)
            nt.links.new(uv.outputs['UV'], mp.inputs['Vector']); nt.links.new(mp.outputs['Vector'], n.inputs['Vector'])
        else:
            nt.links.new(uv.outputs['UV'], n.inputs['Vector'])
        return n
    img = image(name)
    nt.links.new(img.outputs['Color'], b.inputs['Base Color'])
    if e['mode'] == 'sprite':
        nt.links.new(img.outputs['Alpha'], b.inputs['Alpha'])
    em = e.get('emit')
    if em:
        src = img if em is True else image(em)
        nt.links.new(src.outputs['Color'], b.inputs['Emission Color'])
        b.inputs['Emission Strength'].default_value = strength
    MATS[name] = m; MODE[name] = e['mode']
    return m

M = dict(
    sodium=mat('sodium_lamp', (0, 0, 0), emit=(1.0, 0.6, 0.2), strength=60),
    tube=mat('tube_on', (0.9, 0.9, 0.9), emit=(0.9, 0.95, 1.0), strength=30),
    tubedim=mat('tube_flicker', (0.9, 0.9, 0.9), emit=(0.9, 0.95, 1.0), strength=6),
    tubedead=mat('tube_dead', (0.5, 0.5, 0.5)),
    bulb=mat('caged_bulb', (0, 0, 0), emit=(1.0, 0.9, 0.7), strength=40),
    mullion=mat('mullion', (0.04, 0.04, 0.04)),
)

# ---------------------------------------------------------------- geometry
FACES = {  # key: (local outward normal, the four corners as signs of (hx, hy, hz), ordered BL BR TR TL seen from outside)
    'e':   ((1, 0, 0),  [(1, -1, -1), (1, 1, -1), (1, 1, 1), (1, -1, 1)]),
    'w':   ((-1, 0, 0), [(-1, 1, -1), (-1, -1, -1), (-1, -1, 1), (-1, 1, 1)]),
    'n':   ((0, 1, 0),  [(1, 1, -1), (-1, 1, -1), (-1, 1, 1), (1, 1, 1)]),
    's':   ((0, -1, 0), [(-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1)]),
    'top': ((0, 0, 1),  [(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]),
    'bot': ((0, 0, -1), [(-1, 1, -1), (1, 1, -1), (1, -1, -1), (-1, -1, -1)]),
}
DIRS = {'e': Vector((1, 0, 0)), 'w': Vector((-1, 0, 0)), 'n': Vector((0, 1, 0)), 's': Vector((0, -1, 0))}

def box(name, x0, y0, z0, x1, y1, z1, m, faces=None, col='Blockout', rotz=0.0, pivot=None, top_front=None):
    """a box from plan corners (x0,y0)-(x1,y1), z0..z1. m: material for every face; faces: {'e'|'w'|'n'|'s'|'top'|'bot': material}
    (n = plan north). Tile textures line up in world space; fit textures stretch over their face, upright as seen from
    outside; a fit texture on top reads from the top_front side."""
    faces = faces or {}
    hx, hy, hz = abs(x1 - x0) / 2, abs(y1 - y0) / 2, abs(z1 - z0) / 2
    centre = P((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2)
    if rotz:
        pv = P(*pivot)
        centre = pv + Matrix.Rotation(math.radians(rotz), 3, 'Z') @ (centre - pv)
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    uvl = bm.loops.layers.uv.new()
    mats = []
    for key, (nrm, corners) in FACES.items():
        mm = faces.get(key, m)
        if mm not in mats: mats.append(mm)
        vs = [bm.verts.new((sx * hx, sy * hy, sz * hz)) for sx, sy, sz in corners]
        f = bm.faces.new(vs)
        f.material_index = mats.index(mm)
        mode = MODE[mm.name]
        n = Vector(nrm)
        for loop, (i, co) in zip(f.loops, enumerate(v.co.copy() for v in vs)):
            w = co + centre
            if key in ('top', 'bot'):
                if mode == 'fit' and top_front:
                    fr = DIRS[top_front]
                    right = (-fr).cross(Vector((0, 0, 1)))
                    u = (co.dot(right) / (abs(right.x) * hx + abs(right.y) * hy) + 1) / 2
                    v = (-co.dot(fr) / (abs(fr.x) * hx + abs(fr.y) * hy) + 1) / 2
                    loop[uvl].uv = (u, v)
                elif mode == 'fit':
                    loop[uvl].uv = ((co.x / hx + 1) / 2, (co.y / hy + 1) / 2)
                else:
                    loop[uvl].uv = (w.x, w.y)
            else:
                if mode == 'fit':
                    loop[uvl].uv = [(0, 0), (1, 0), (1, 1), (0, 1)][i]
                else:
                    right = (-n).cross(Vector((0, 0, 1)))
                    loop[uvl].uv = (w.dot(right), w.z)
    bm.to_mesh(me); bm.free()
    for mm in mats: me.materials.append(mm)
    ob = bpy.data.objects.new(name, me)
    ob.location = centre
    ob.rotation_euler.z = math.radians(rotz)
    COL[col].objects.link(ob)
    return ob

def cyl(name, x, y, z0, z1, r, m, col='Blockout'):
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, segments=16, radius1=r, radius2=r, depth=z1 - z0)
    bm.to_mesh(me); bm.free()
    ob = bpy.data.objects.new(name, me)
    ob.location = P(x, y, (z0 + z1) / 2)
    me.materials.append(m)
    COL[col].objects.link(ob)
    return ob

def wall(name, x0, y0, x1, y1, z0, z1, m, t, faces=None):
    """wall along a plan line, thickness t"""
    if x0 == x1: return box(name, x0 - t / 2, y0, z0, x0 + t / 2, y1, z1, m, faces)
    return box(name, x0, y0 - t / 2, z0, x1, y0 + t / 2, z1, m, faces)

SPRITES = []
def sprite(name, x, y, z0, tex, h=None, facing=None, strength=6.0):
    """a Build sprite: a picture h units tall standing at plan (x, y). facing None = face sprite (turns to the camera
    about Z); 'e'/'w'/'n'/'s' = wall sprite, flat against a wall and facing that way."""
    e = MANIFEST[tex]
    h = h or e['h']; w = h * e['w'] / e['h']
    me = bpy.data.meshes.new(name)
    bm = bmesh.new(); uvl = bm.loops.layers.uv.new()
    f = bm.faces.new([bm.verts.new(c) for c in [(-w / 2, 0, 0), (w / 2, 0, 0), (w / 2, 0, h), (-w / 2, 0, h)]])
    for loop, uv in zip(f.loops, [(0, 0), (1, 0), (1, 1), (0, 1)]): loop[uvl].uv = uv
    bm.to_mesh(me); bm.free()
    me.materials.append(T(tex, strength))
    ob = bpy.data.objects.new(name, me)
    ob.location = P(x, y, z0)
    if facing:
        ob.rotation_euler.z = {'s': 0, 'e': math.pi / 2, 'n': math.pi, 'w': -math.pi / 2}[facing]
    else:
        c = ob.constraints.new('LOCKED_TRACK')
        c.track_axis = 'TRACK_NEGATIVE_Y'; c.lock_axis = 'LOCK_Z'
        SPRITES.append(c)
    COL['Sprites'].objects.link(ob)
    return ob

def floor_paint(name, tex):
    """a floor sprite: paint on the ground over the plan rect stored with the texture"""
    x0, y0, x1, y1 = MANIFEST['_floor'][tex]
    me = bpy.data.meshes.new(name)
    bm = bmesh.new(); uvl = bm.loops.layers.uv.new()
    f = bm.faces.new([bm.verts.new(c) for c in [(x0, -y1, 0.004), (x1, -y1, 0.004), (x1, -y0, 0.004), (x0, -y0, 0.004)]])
    for loop, uv in zip(f.loops, [(0, 0), (1, 0), (1, 1), (0, 1)]): loop[uvl].uv = uv
    bm.to_mesh(me); bm.free()
    me.materials.append(T(tex))
    ob = bpy.data.objects.new(name, me)
    COL['Sprites'].objects.link(ob)
    return ob

def light(name, kind, at, rgb, power, size=None, aim=None, spot=None, col='Lights'):
    d = bpy.data.lights.new(name, kind)
    d.color = rgb; d.energy = power
    if kind == 'AREA' and size:
        d.shape = 'RECTANGLE'; d.size, d.size_y = size
    if kind == 'POINT' and size: d.shadow_soft_size = size
    if kind == 'SPOT' and spot:
        d.spot_size = math.radians(spot); d.spot_blend = 0.6; d.shadow_soft_size = 0.1
    ob = bpy.data.objects.new(name, d)
    ob.location = at
    if aim is not None:
        ob.rotation_euler = (aim - at).to_track_quat('-Z', 'Y').to_euler()
    COL[col].objects.link(ob)
    return ob

# ---------------------------------------------------------------- yard
YARD_H, BLD_H, T_OUT, T_IN = 6.0, 7.0, 0.35, 0.18
box('yard_ground', 0, 0, -0.1, 19, 24, 0, T('asphalt'))
box('street_ground', 0, 23, -0.1, 47, 30, 0, T('asphalt'))
box('street_ground_e', 41, 0, -0.1, 47, 23, 0, T('asphalt'))
wall('yard_n_pale', 1, 1, 10.5, 1, 0, YARD_H, T('brick'), T_OUT, {'s': T('brick_pale')})
wall('yard_n', 10.5, 1, 19, 1, 0, YARD_H, T('brick'), T_OUT)
wall('yard_w', 1, 1, 1, 23, 0, YARD_H, T('brick'), T_OUT)
wall('yard_s_a', 1, 23, 3.5, 23, 0, YARD_H, T('brick'), T_OUT)
wall('yard_s_b', 7.5, 23, 19, 23, 0, YARD_H, T('brick'), T_OUT)
box('gate_shutter', 3.5, 22.9, 0.22, 7.5, 23.1, 4.0, T('shutter'))     # gap underneath leaks street light
box('gate_lintel', 3.5, 22.8, 4.0, 7.5, 23.2, YARD_H, T('brick'))
sprite('sign_noentry', 5.5, 22.88, 1.5, 'sign_noentry', facing='n')
floor_paint('parking_bays', 'paint_bays')
floor_paint('fire_exit_keep_clear', 'paint_keepclear')
light('street_gate', 'AREA', P(5.5, 24.2, 0.6), (0.45, 0.65, 1.0), 400, size=(4, 1), aim=P(5.5, 20, 0))

# bins, bags, pallets, skip, barrel fire
DUMP = {k: T('dumpster_front') for k in 'nsew'} | {'top': T('rubber')}
box('bin_a', 10.8, 1.2, 0, 14.0, 2.7, 1.3, T('dumpster_steel'), DUMP)
box('bin_b', 14.6, 1.2, 0, 16.4, 2.6, 1.2, T('dumpster_steel'), DUMP)
box('bin_w', 1.2, 14.5, 0, 2.7, 17.7, 1.3, T('dumpster_steel'), DUMP)
sprite('bags_a', 16.9, 1.9, 0, 'spr_bags'); sprite('bags_b', 2.0, 18.5, 0, 'spr_bags')
for i, (px, py, h) in enumerate([(10, 14.2, 0.9), (11.3, 14.2, 0.6), (10, 15.5, 0.3)]):
    box(f'pallets_{i}', px, py, 0, px + 1.2, py + 1.2, h, T('plywood'))
box('skip', 15.5, 18.5, 0, 18.1, 19.9, 1.2, T('dumpster_steel'))
sprite('barrel', 12.6, 7.8, 0, 'spr_barrel')
sprite('barrel_fire', 12.6, 7.8, 0.72, 'spr_fire', h=0.95, strength=8)
light('barrel_fire', 'POINT', P(12.6, 7.8, 1.4), (1.0, 0.45, 0.15), 90, size=0.3)

# sodium wall-pack washing the pale wall the truck faces
box('wallpack', 5.2, 1.18, 4.3, 5.9, 1.5, 4.6, M['sodium'])
light('sodium_wash', 'SPOT', P(5.55, 2.0, 4.7), (1.0, 0.62, 0.28), 450, spot=60, aim=P(5.55, 3.4, 0.0))
light('sodium_wallgraze', 'AREA', P(5.55, 2.0, 4.4), (1.0, 0.62, 0.28), 650, size=(8.5, 0.4), aim=P(5.55, 1.15, 1.2))

# ---------------------------------------------------------------- truck (crooked: +11 deg about plan (5.3,16))
TR = dict(rotz=11, pivot=(5.3, 16))
FLOOR, TOP = 0.8, 3.1
box('truck_floor', 4, 12, FLOOR - 0.15, 6.6, 19.5, FLOOR, T('truck_white'), {'top': T('plywood')}, **TR)
box('truck_wall_l', 4, 12, FLOOR, 4.08, 19.5, TOP, T('truck_white'), {'w': T('truck_side'), 'e': T('shutter')}, **TR)
box('truck_wall_r', 6.52, 12, FLOOR, 6.6, 19.5, TOP, T('truck_white'), {'e': T('truck_side'), 'w': T('shutter')}, **TR)
box('truck_wall_front', 4, 19.42, FLOOR, 6.6, 19.5, TOP, T('truck_white'), {'n': T('shutter')}, **TR)
box('truck_roof', 4, 12, TOP, 6.6, 19.5, TOP + 0.08, T('truck_white'), **TR)
box('truck_chassis', 4.2, 12.3, 0.25, 6.4, 22, FLOOR - 0.15, T('rubber'), **TR)
box('truck_cab', 4.15, 19.6, 0.4, 6.45, 22.1, 2.8, T('truck_white'), {'w': T('truck_cab'), 'e': T('truck_cab')}, **TR)
for i, (wx, wy) in enumerate([(4.0, 13.2), (6.3, 13.2), (4.0, 20.8), (6.3, 20.8)]):
    box(f'truck_wheel_{i}', wx, wy - 0.5, 0, wx + 0.3, wy + 0.5, 0.9, T('rubber'), **TR)
for i, (hx, sgn) in enumerate([(4.0, -1), (6.6, 1)]):     # rear doors swung open (~135 deg)
    d = box(f'truck_door_{i}', hx - 1.3 * (sgn < 0), 11.96, FLOOR, hx + 1.3 * (sgn > 0), 12.0, TOP, T('truck_white'))
    hinge = P(hx, 12)
    rot = Matrix.Rotation(math.radians(-sgn * 135), 3, 'Z')
    d.location = hinge + rot @ (d.location - hinge)
    d.rotation_euler.z = math.radians(-sgn * 135)
    hv = P(*TR['pivot']); rt = Matrix.Rotation(math.radians(TR['rotz']), 3, 'Z')
    d.location = hv + rt @ (d.location - hv)
    d.rotation_euler.z += math.radians(TR['rotz'])
box('truck_cargo_sheeted_cabinet', 4.2, 17.6, FLOOR, 5.1, 18.5, FLOOR + 1.9, T('dustsheet'), **TR)
box('truck_cargo_crate', 5.6, 18.0, FLOOR, 6.45, 19.4, FLOOR + 1.1, T('plywood'), **TR)

# ---------------------------------------------------------------- building shell
box('bld_floor_corr', 19, 1, -0.1, 33, 7.5, 0.0, T('concrete'))
box('bld_floor_prize', 33, 1, -0.1, 41, 7.5, 0.0, T('carpet'))
box('bld_floor_arcade', 19, 7.5, -0.1, 41, 23, 0.0, T('carpet'))
wall('bld_w_a', 19, 1, 19, 5, 0, BLD_H, T('render'), T_OUT, {'e': T('cinder_pale')})
wall('bld_w_b', 19, 6.5, 19, 23, 0, BLD_H, T('render'), T_OUT, {'e': T('arcade_wall')})
box('bld_w_lintel', 19 - T_OUT / 2, 5, 2.3, 19 + T_OUT / 2, 6.5, BLD_H, T('render'), {'e': T('cinder_pale')})
wall('bld_n', 19, 1, 41.2, 1, 0, BLD_H, T('render'), T_OUT, {'s': T('cinder')})
wall('bld_s', 19, 23, 41.2, 23, 0, BLD_H, T('render'), T_OUT, {'n': T('arcade_wall')})
box('bld_e_band', 40.8, 1, 3.6, 41.2, 23, BLD_H, T('render'), {'w': T('arcade_wall')})
box('bld_roof', 19, 1, BLD_H, 41.2, 23, BLD_H + 0.2, T('render'), col='Ceilings')

# fire exit: hinged on the north jamb, swung ~70 deg out into the yard, propped with a brick
fx = box('fire_exit_door', 18.97, 5, 0, 19.03, 6.5, 2.3, T('steel'), {'w': T('fire_door'), 'e': T('fire_door_inside')})
hinge = P(19, 5)
r = Matrix.Rotation(math.radians(-70), 3, 'Z')
fx.location = hinge + r @ (fx.location - hinge); fx.rotation_euler.z = math.radians(-70)
sprite('sign_trade', 18.81, 10.0, 2.3, 'sign_trade', facing='w')
sprite('brick_wedge', 18.5, 6.65, 0, 'spr_brick')
sprite('sign_exit', 18.81, 5.75, 2.4, 'sign_exit', facing='w', strength=8)
light('exit_glow', 'POINT', P(18.5, 5.75, 2.6), (0.2, 1.0, 0.5), 60, size=0.15)
cyl('caged_bulb', 18.75, 4.6, 2.85, 3.0, 0.08, M['bulb'])
light('caged_bulb', 'POINT', P(18.6, 4.6, 2.9), (1.0, 0.9, 0.7), 70, size=0.08)
light('door_spill', 'AREA', P(19.8, 5.75, 2.2), (0.85, 0.9, 1.0), 15, size=(0.8, 1.4), aim=P(17, 5.75, 0))

# interior walls: corridor side cinder block, arcade side dark paint
CORR_CEIL, ARC_CEIL = 3.0, 4.0
for i, (x0, x1) in enumerate([(19, 21.5), (22.5, 25), (25.8, 29), (30, 33)]):
    wall(f'corr_n_{i}', x0, 4, x1, 4, 0, CORR_CEIL, T('cinder'), T_IN, {'s': T('cinder_pale')})
for i, x in enumerate([24, 27]):
    wall(f'room_div_{i}', x, 1, x, 4, 0, CORR_CEIL, T('cinder'), T_IN)
wall('corr_e', 33, 1, 33, 7.5, 0, ARC_CEIL, T('cinder'), T_IN, {'e': T('arcade_wall'), 'w': T('wall_twotone')})
wall('corr_s_a', 19, 7.5, 29.2, 7.5, 0, ARC_CEIL, T('wall_twotone'), T_IN, {'s': T('arcade_wall')})
wall('corr_s_b', 30.8, 7.5, 33, 7.5, 0, ARC_CEIL, T('wall_twotone'), T_IN, {'s': T('arcade_wall')})
box('staff_door_lintel', 29.2, 7.41, 2.2, 30.8, 7.59, ARC_CEIL, T('wall_twotone'), {'s': T('arcade_wall')})
sprite('sign_staff', 28.55, 7.4, 1.3, 'sign_staff', facing='n')
box('corr_ceiling', 19, 1, CORR_CEIL, 33, 7.5, CORR_CEIL + 0.1, T('pipes'), col='Ceilings')
box('arcade_ceiling', 19, 7.5, ARC_CEIL, 41, 23, ARC_CEIL + 0.1, T('paint_black'), col='Ceilings')
box('prize_ceiling', 33, 1, ARC_CEIL, 41, 7.5, ARC_CEIL + 0.1, T('paint_black'), col='Ceilings')
for i, (hx, sgn) in enumerate([(29.2, 1), (30.8, -1)]):     # staff swing doors, ajar into the arcade
    d = box(f'staff_door_{i}', hx if sgn > 0 else hx - 0.8, 7.47, 0, hx + 0.8 if sgn > 0 else hx, 7.53, 2.2, T('steel'))
    h = P(hx, 7.5); r = Matrix.Rotation(math.radians(-sgn * 55), 3, 'Z')
    d.location = h + r @ (d.location - h); d.rotation_euler.z = math.radians(-sgn * 55)

# fluorescent tubes: dead, flicker, dead, on (the best one at the far end pulls you along)
for i, (x0, state) in enumerate([(20.6, 'dead'), (23.5, 'dim'), (26.6, 'dead'), (29.7, 'on')]):
    box(f'tube_{i}_{state}', x0, 5.65, CORR_CEIL - 0.06, x0 + 1.4, 5.85, CORR_CEIL, M['tube' if state == 'on' else 'tubedim' if state == 'dim' else 'tubedead'])
    if state != 'dead':
        light(f'tube_{i}', 'AREA', P(x0 + 0.7, 5.75, CORR_CEIL - 0.08), (0.9, 0.95, 1.0), 70 if state == 'on' else 14, size=(1.4, 0.2), aim=P(x0 + 0.7, 5.75, 0))

# a drinks machine against the two-tone wall, a chunk of wall sticking out beside it, an extinguisher beyond
box('pilaster', 25.3, 6.95, 0, 25.75, 7.41, CORR_CEIL, T('wall_twotone'))
box('vending', 24.2, 6.62, 0, 25.25, 7.41, 2.0, T('paint_black'), {'n': T('vending_front', 1.5)})
light('vending_glow', 'AREA', P(24.72, 6.4, 1.2), (1.0, 0.92, 0.85), 12, size=(0.9, 1.6), aim=P(24.72, 4, 1.0))
sprite('extinguisher', 26.4, 7.39, 0.45, 'spr_extinguisher', facing='n')

# rooms off the corridor
box('office_desk', 19.3, 1.3, 0, 21.3, 2.3, 0.75, T('plywood'))
sprite('office_chair', 20.4, 2.8, 0, 'spr_chair')
sprite('wc_toilet', 25.5, 1.55, 0, 'spr_toilet')
box('store_shelves', 27.3, 1.2, 0, 32.7, 2.0, 2.2, T('plywood'))
box('store_part_a', 27.3, 2.4, 0, 28.5, 3.6, 1.0, T('dustsheet'))
box('store_part_b', 29, 2.4, 0, 30.2, 3.6, 1.6, T('dustsheet'))

# prize counter and lobby
box('prize_counter', 34, 5.4, 0, 39.6, 6.2, 1.05, T('laminate'))
box('prize_shelves', 34, 1.15, 0, 39.6, 1.85, 2.6, T('plywood'))
box('change_machine', 39.4, 8.4, 0, 40.5, 9.6, 1.8, T('steel'))
box('air_hockey', 38.6, 17.6, 0, 40.6, 21.2, 0.8, T('laminate'))
box('lobby_claw', 39.2, 15.0, 0, 40.4, 16.2, 2.05, T('paint_black'), {'w': T('claw_front')})

# ---------------------------------------------------------------- arcade machines
AXIS = {'e': (1, 0), 'w': (-1, 0), 'n': (0, -1), 's': (0, 1)}     # plan direction each way faces
def footprint(cx, cy, face, a0, a1, width):
    """plan rect from a0..a1 along the facing axis (0 = centre) and +-width/2 across it"""
    dx, dy = AXIS[face]
    if dx: xs, ys = sorted((cx + dx * a0, cx + dx * a1)), (cy - width / 2, cy + width / 2)
    else:  xs, ys = (cx - width / 2, cx + width / 2), sorted((cy + dy * a0, cy + dy * a1))
    return xs[0], ys[0], xs[1], ys[1]

def cabinet(name, cx, cy, face, tex, width=0.95, depth=0.9, ledge=0.3, panel_h=1.0, height=1.9, strength=0):
    """a Build-style upright cabinet: a tall body (marquee and screen on its face) and a lower ledge in front of it
    (coin door on its face, the control panel on top). Two boxes, everything else is the painting."""
    up, lo, pa = T(tex + '_upper', strength), T(tex + '_lower', strength), T(tex + '_panel', strength)
    side = T('laminate')
    x0, y0, x1, y1 = footprint(cx, cy, face, -depth / 2, depth / 2 - ledge, width)
    box(name + '_base', x0, y0, 0, x1, y1, panel_h, side)
    box(name + '_head', x0, y0, panel_h, x1, y1, height, side, {face: up})
    x0, y0, x1, y1 = footprint(cx, cy, face, depth / 2 - ledge, depth / 2, width)
    box(name + '_ledge', x0, y0, 0, x1, y1, panel_h, side, {face: lo, 'top': pa}, top_front=face)

def sheeted(name, cx, cy, face, width=0.95, depth=0.9):
    x0, y0, x1, y1 = footprint(cx, cy, face, -depth / 2, depth / 2, width)
    box(name, x0 - 0.04, y0 - 0.04, 0, x1 + 0.04, y1 + 0.04, 1.95, T('dustsheet'))

k = 0
for rx, face in [(19.2, 'e'), (23.5, 'w'), (24.4, 'e'), (26.9, 'e'), (32.2, 'w'), (35.3, 'w'), (36.2, 'e')]:
    y = 9.8
    while y < 18.5:
        cx, cy = rx + 0.45, y + 0.475
        if (k * 7 + 3) % 5 == 0:
            sheeted(f'cab_{k}', cx, cy, face)
        else:
            cabinet(f'cab_{k}', cx, cy, face, f'cab{k % 3}')
            if k % 9 == 4:   # the odd dead one
                sprite(f'out_of_order_{k}', cx + AXIS[face][0] * 0.46, cy, 1.25, 'sign_outoforder', h=0.4, facing=face)
        k += 1; y += 1.15

def pinball(x):
    box(f'pinball_{x:.1f}', x, 21.45, 0, x + 0.7, 22.85, 0.95, T('laminate'), {'n': T('pinball_front'), 'top': T('pinball_top')}, top_front='n')
    box(f'pinball_back_{x:.1f}', x, 22.65, 0.95, x + 0.7, 22.85, 1.85, T('laminate'), {'n': T('pinball_back')})
def claw(x):
    box(f'claw_{x:.1f}', x, 21.75, 0, x + 1.1, 22.85, 2.05, T('paint_black'), {'n': T('claw_front')})
def backwall(seq, x):
    for t in seq:
        if t == 'P': pinball(x); x += 0.85
        else: claw(x); x += 1.25
backwall('PPCPPCPP', 19.4)
backwall('PPCPPCP', 31.3)

# Mine Shift: the one live cabinet (marquee, screen and big red button are fullbright)
cabinet('mineshift', 30.0, 22.475, 'n', 'mineshift', width=1.0, depth=0.85, strength=10)
light('mineshift_glow', 'AREA', P(30, 21.9, 1.4), (0.25, 1.0, 0.8), 110, size=(0.8, 0.5), aim=P(30, 18, 0.6))

# ---------------------------------------------------------------- the frosted front: painted, backlit (fullbright), the neon backwards
box('front_glass', 40.97, 1, 0, 41.03, 23, 3.6, M['mullion'], {'w': T('frost_front', 2.2)})
for i, y0 in enumerate([1, 5.4, 9.8, 14.2, 18.6, 23]):
    box(f'front_mullion_{i}', 40.9, y0 - 0.06, 0, 40.96, y0 + 0.06, 3.6, M['mullion'])
box('front_door_split', 40.92, 11.97, 0, 40.96, 12.03, 3.6, M['mullion'])
box('front_transom', 40.9, 1, 3.55, 40.96, 23, 3.65, M['mullion'])
cyl('front_door_chain', 40.85, 12, 1.0, 1.4, 0.05, T('steel'))
light('front_fill_inside', 'AREA', P(40.6, 12, 1.8), (0.75, 0.85, 1.0), 35, size=(3.4, 21), aim=P(30, 12, 1.0))

# ---------------------------------------------------------------- world, render
world = bpy.data.worlds.new('night'); scene.world = world
world.use_nodes = True
bg = world.node_tree.nodes['Background']
bg.inputs['Color'].default_value = (0.010, 0.014, 0.03, 1); bg.inputs['Strength'].default_value = 0.6

scene.render.engine = 'CYCLES'
scene.cycles.samples = SAMPLES
scene.cycles.use_denoising = True
scene.cycles.max_bounces = 6
scene.render.resolution_x, scene.render.resolution_y = 1280, 800
scene.view_settings.view_transform = 'AgX'
scene.view_settings.exposure = 0.5
try:
    prefs = bpy.context.preferences.addons['cycles'].preferences
    for dev in ('OPTIX', 'CUDA', 'HIP', 'ONEAPI'):
        try:
            prefs.compute_device_type = dev; prefs.get_devices()
            if any(d.type == dev for d in prefs.devices):
                for d in prefs.devices: d.use = d.type == dev
                scene.cycles.device = 'GPU'; print('intro: rendering on', dev); break
        except TypeError:
            pass
except Exception as e:
    print('intro: CPU render', e)

# ---------------------------------------------------------------- cameras (Duke's eye height, ~95 deg wide at 16:10)
def camera(name, at, look):
    c = bpy.data.cameras.new(name); c.lens_unit = 'FOV'; c.angle = math.radians(95); c.sensor_fit = 'HORIZONTAL'
    c.clip_start = 0.05
    ob = bpy.data.objects.new(name, c); ob.location = at
    ob.rotation_euler = (look - at).to_track_quat('-Z', 'Y').to_euler()
    COL['Cameras'].objects.link(ob)
    return ob

tp = P(*TR['pivot']); rt = Matrix.Rotation(math.radians(TR['rotz']), 3, 'Z')
def truckpt(x, y, z): return tp + rt @ (P(x, y, z) - tp)
CAMS = {
    'truck': camera('cam_1_truck', truckpt(5.3, 16.4, FLOOR + EYE), truckpt(5.3, 8, FLOOR + EYE - 0.4)),
    'wall':  camera('cam_2_pale_wall', P(6.5, 4.8, EYE), P(18, 6.6, EYE - 0.2)),
    'exit':  camera('cam_3_fire_exit', P(19.8, 5.75, EYE), P(32, 5.75, EYE - 0.1)),
    'glass': camera('cam_5_front_glass', P(31.2, 8.5, EYE), P(41, 11.5, EYE + 0.3)),
    'yard':  camera('cam_6_yard_back', P(16.5, 9.5, EYE), P(4, 16, EYE - 0.6)),
    'corridor': camera('cam_7_corridor_back', P(32.2, 5.2, EYE), P(19, 6.0, EYE - 0.15)),
    'staff': camera('cam_4_staff_doors', P(30, 8.3, EYE), P(33.0, 22, EYE - 0.35)),
}
top = bpy.data.cameras.new('top'); top.type = 'ORTHO'; top.ortho_scale = 44
topob = bpy.data.objects.new('cam_top', top); topob.location = P(21, 12, 40)
COL['Cameras'].objects.link(topob)
CAMS['top'] = topob

def use_camera(cam):
    scene.camera = cam
    for c in SPRITES: c.target = cam      # face sprites turn to whoever is looking

use_camera(CAMS['truck'])
os.makedirs(os.path.join(OUT, 'renders'), exist_ok=True)
blend = os.path.join(OUT, 'intro_blockout.blend')
bpy.ops.wm.save_as_mainfile(filepath=blend)
print('intro: saved', blend)

if RENDER:
    views = list(CAMS) if RENDER == 'all' else RENDER.split(',')
    for v in views:
        use_camera(CAMS[v])
        hide = v == 'top'
        COL['Ceilings'].hide_render = hide
        scene.render.resolution_x, scene.render.resolution_y = (1400, 800) if hide else (1280, 800)
        scene.render.filepath = os.path.join(OUT, 'renders', ('flat_' if FLAT else '') + ('overhead' if v == 'top' else v) + '.png')
        bpy.ops.render.render(write_still=True)
        print('intro: rendered', scene.render.filepath)
    COL['Ceilings'].hide_render = False
    use_camera(CAMS['truck'])
