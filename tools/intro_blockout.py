"""Intro map blockout (truck -> yard -> Graveyard Shift fire exit -> Mine Shift), built headless in Blender.

    blender -b -P tools/intro_blockout.py -- [--render all|truck,wall,exit,staff,top] [--samples N]

Writes art/intro/intro_blockout.blend and renders to art/intro/renders/<view>.png.
Units: 1 Blender unit = 512 Build units (one map-editor grid square). Plan x -> X, plan y (south, down) -> -Y.
Duke's eye is 38<<8 z = 608 xy units above the floor, so EYE = 1.19.
"""
import bpy, bmesh, math, os, sys
from mathutils import Vector, Matrix

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'art', 'intro')
EYE = 608 / 512

argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
def arg(name, default=None):
    return argv[argv.index(name) + 1] if name in argv else default
RENDER = arg('--render', '')
SAMPLES = int(arg('--samples', '48'))

# ---------------------------------------------------------------- scene
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
for name in ('Blockout', 'Ceilings', 'Lights', 'Cameras'):
    scene.collection.children.link(bpy.data.collections.new(name))
COL = bpy.data.collections

def P(x, y, z=0.0):
    """plan coordinates (y down) -> Blender"""
    return Vector((x, -y, z))

# ---------------------------------------------------------------- materials
MATS = {}
def mat(name, rgb, rough=0.8, emit=None, strength=0.0, glass=False, metal=0.0):
    if name in MATS: return MATS[name]
    m = bpy.data.materials.new(name); m.use_nodes = True
    b = m.node_tree.nodes['Principled BSDF']
    b.inputs['Base Color'].default_value = (*rgb, 1)
    b.inputs['Roughness'].default_value = rough
    b.inputs['Metallic'].default_value = metal
    if emit:
        b.inputs['Emission Color'].default_value = (*emit, 1)
        b.inputs['Emission Strength'].default_value = strength
    if glass:
        b.inputs['Transmission Weight'].default_value = 1.0
        b.inputs['IOR'].default_value = 1.45
    m.diffuse_color = (*rgb, 1)
    MATS[name] = m
    return m

M = dict(
    asphalt=mat('asphalt', (0.06, 0.06, 0.065), 0.9),
    brick=mat('brick_dark', (0.20, 0.11, 0.08)),
    pale=mat('brick_pale', (0.62, 0.58, 0.52)),            # the wall the truck faces
    render=mat('building_render', (0.22, 0.22, 0.24)),
    steel=mat('steel_door', (0.30, 0.32, 0.30), 0.4, metal=0.6),
    shutter=mat('shutter', (0.25, 0.27, 0.30), 0.5, metal=0.5),
    bin=mat('bin_green', (0.05, 0.14, 0.07), 0.6),
    wood=mat('pallet_wood', (0.30, 0.20, 0.11)),
    truck=mat('truck_white', (0.55, 0.55, 0.55), 0.5),
    truckin=mat('truck_inside', (0.18, 0.18, 0.19), 0.7),
    sheet=mat('dust_sheet', (0.28, 0.27, 0.26), 0.95),
    wallin=mat('wall_inside', (0.28, 0.27, 0.25)),
    lino=mat('corridor_lino', (0.16, 0.17, 0.15), 0.6),
    carpet=mat('arcade_carpet', (0.06, 0.03, 0.09), 1.0),
    ceiling=mat('ceiling_tiles', (0.30, 0.30, 0.30)),
    cab=mat('cabinet', (0.04, 0.04, 0.05), 0.5),
    screen=mat('dead_screen', (0.01, 0.01, 0.012), 0.15),
    pin=mat('pinball', (0.08, 0.05, 0.05), 0.4),
    claw=mat('claw_glass', (0.6, 0.65, 0.7), 0.1, glass=True),
    frost=mat('frosted_glass', (0.9, 0.95, 1.0), 0.35, glass=True),
    mullion=mat('mullion', (0.05, 0.05, 0.05), 0.4, metal=0.8),
    porcelain=mat('porcelain', (0.8, 0.8, 0.78), 0.2),
    desk=mat('desk', (0.15, 0.1, 0.06)),
    fire=mat('fire', (0, 0, 0), emit=(1.0, 0.35, 0.08), strength=30),
    exit=mat('exit_sign', (0, 0, 0), emit=(0.15, 1.0, 0.45), strength=25),
    tube=mat('tube_on', (0.9, 0.9, 0.9), emit=(0.9, 0.95, 1.0), strength=30),
    tubedim=mat('tube_flicker', (0.9, 0.9, 0.9), emit=(0.9, 0.95, 1.0), strength=6),
    tubedead=mat('tube_dead', (0.5, 0.5, 0.5), 0.3),
    sodium=mat('sodium_lamp', (0, 0, 0), emit=(1.0, 0.6, 0.2), strength=60),
    mine=mat('mineshift_screen', (0, 0, 0), emit=(0.2, 1.0, 0.8), strength=14),
    marquee=mat('mineshift_marquee', (0, 0, 0), emit=(0.4, 1.0, 0.5), strength=10),
    red=mat('big_red_button', (0, 0, 0), emit=(1.0, 0.05, 0.03), strength=25),
    neon=mat('neon_pink', (0, 0, 0), emit=(1.0, 0.18, 0.55), strength=60),
    bulb=mat('caged_bulb', (0, 0, 0), emit=(1.0, 0.9, 0.7), strength=40),
)

# ---------------------------------------------------------------- geometry helpers
def box(name, x0, y0, z0, x1, y1, z1, m, col='Blockout', rotz=0.0, pivot=None):
    """axis-aligned box from plan corners (x0,y0)-(x1,y1), z0..z1; optional rotation about a plan pivot"""
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    bm.to_mesh(me); bm.free()
    ob = bpy.data.objects.new(name, me)
    ob.scale = (abs(x1 - x0), abs(y1 - y0), abs(z1 - z0))
    c = P((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2)
    if rotz:
        pv = P(*pivot)
        c = pv + Matrix.Rotation(math.radians(rotz), 3, 'Z') @ (c - pv)
        ob.rotation_euler.z = math.radians(rotz)
    ob.location = c
    me.materials.append(m)
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

def wall(name, x0, y0, x1, y1, z0, z1, m, t):
    """wall along a plan line, thickness t"""
    if x0 == x1: return box(name, x0 - t / 2, y0, z0, x0 + t / 2, y1, z1, m)
    return box(name, x0, y0 - t / 2, z0, x1, y0 + t / 2, z1, m)

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
box('yard_ground', 0, 0, -0.1, 19, 24, 0, M['asphalt'])
box('street_ground', 0, 23, -0.1, 47, 30, 0, M['asphalt'])
box('street_ground_e', 41, 0, -0.1, 47, 23, 0, M['asphalt'])
wall('yard_n_pale', 1, 1, 10.5, 1, 0, YARD_H, M['pale'], T_OUT)
wall('yard_n', 10.5, 1, 19, 1, 0, YARD_H, M['brick'], T_OUT)
wall('yard_w', 1, 1, 1, 23, 0, YARD_H, M['brick'], T_OUT)
wall('yard_s_a', 1, 23, 3.5, 23, 0, YARD_H, M['brick'], T_OUT)
wall('yard_s_b', 7.5, 23, 19, 23, 0, YARD_H, M['brick'], T_OUT)
box('gate_shutter', 3.5, 22.9, 0.22, 7.5, 23.1, 4.0, M['shutter'])     # gap underneath leaks street light
box('gate_lintel', 3.5, 22.8, 4.0, 7.5, 23.2, YARD_H, M['brick'])
light('street_gate', 'AREA', P(5.5, 24.2, 0.6), (0.45, 0.65, 1.0), 400, size=(4, 1), aim=P(5.5, 20, 0))

# bins, pallets, skip, barrel
box('bin_a', 10.8, 1.2, 0, 14.0, 2.7, 1.3, M['bin'])
box('bin_b', 14.6, 1.2, 0, 16.4, 2.6, 1.2, M['bin'])
box('bin_w', 1.2, 14.5, 0, 2.7, 17.7, 1.3, M['bin'])
for i, (px, py, h) in enumerate([(10, 14.2, 0.9), (11.3, 14.2, 0.6), (10, 15.5, 0.3)]):
    box(f'pallets_{i}', px, py, 0, px + 1.2, py + 1.2, h, M['wood'])
box('skip', 15.5, 18.5, 0, 18.1, 19.9, 1.2, M['shutter'])
cyl('barrel', 12.6, 7.8, 0, 0.9, 0.3, M['steel'])
cyl('barrel_fire', 12.6, 7.8, 0.9, 1.05, 0.26, M['fire'])
light('barrel_fire', 'POINT', P(12.6, 7.8, 1.4), (1.0, 0.45, 0.15), 90, size=0.3)

# sodium wall-pack washing the pale wall the truck faces
box('wallpack', 5.2, 1.18, 4.3, 5.9, 1.5, 4.6, M['sodium'])
light('sodium_wash', 'SPOT', P(5.55, 2.0, 4.7), (1.0, 0.62, 0.28), 450, spot=60, aim=P(5.55, 3.4, 0.0))
light('sodium_wallgraze', 'AREA', P(5.55, 2.0, 4.4), (1.0, 0.62, 0.28), 650, size=(8.5, 0.4), aim=P(5.55, 1.15, 1.2))

# ---------------------------------------------------------------- truck (crooked: +11 deg about plan (5.3,16))
TR = dict(rotz=11, pivot=(5.3, 16))
FLOOR, TOP = 0.8, 3.1
box('truck_floor', 4, 12, FLOOR - 0.15, 6.6, 19.5, FLOOR, M['truckin'], **TR)
box('truck_wall_l', 4, 12, FLOOR, 4.08, 19.5, TOP, M['truck'], **TR)
box('truck_wall_r', 6.52, 12, FLOOR, 6.6, 19.5, TOP, M['truck'], **TR)
box('truck_wall_front', 4, 19.42, FLOOR, 6.6, 19.5, TOP, M['truck'], **TR)
box('truck_roof', 4, 12, TOP, 6.6, 19.5, TOP + 0.08, M['truck'], **TR)
box('truck_chassis', 4.2, 12.3, 0.25, 6.4, 22, FLOOR - 0.15, M['steel'], **TR)
box('truck_cab', 4.15, 19.6, 0.4, 6.45, 22.1, 2.8, M['truck'], **TR)
for i, (wx, wy) in enumerate([(4.0, 13.2), (6.3, 13.2), (4.0, 20.8), (6.3, 20.8)]):
    box(f'truck_wheel_{i}', wx, wy - 0.5, 0, wx + 0.3, wy + 0.5, 0.9, M['cab'], **TR)
# rear doors swung open (~135 deg)
for i, (hx, sgn) in enumerate([(4.0, -1), (6.6, 1)]):
    d = box(f'truck_door_{i}', hx - 1.3 * (sgn < 0), 11.96, FLOOR, hx + 1.3 * (sgn > 0), 12.0, TOP, M['truck'])
    hinge = P(hx, 12)
    rot = Matrix.Rotation(math.radians(-sgn * 135), 3, 'Z')
    d.location = hinge + rot @ (d.location - hinge)
    d.rotation_euler.z = math.radians(-sgn * 135)
    hv = P(*TR['pivot'])
    rt = Matrix.Rotation(math.radians(TR['rotz']), 3, 'Z')
    d.location = hv + rt @ (d.location - hv)
    d.rotation_euler.z += math.radians(TR['rotz'])
box('truck_cargo_sheeted_cabinet', 4.2, 17.6, FLOOR, 5.1, 18.5, FLOOR + 1.9, M['sheet'], **TR)
box('truck_cargo_crate', 5.6, 18.0, FLOOR, 6.45, 19.4, FLOOR + 1.1, M['wood'], **TR)

# ---------------------------------------------------------------- building shell
box('bld_floor_corr', 19, 1, -0.1, 41, 7.5, 0.0, M['lino'])
box('bld_floor_arcade', 19, 7.5, -0.1, 41, 23, 0.0, M['carpet'])
wall('bld_w_a', 19, 1, 19, 5, 0, BLD_H, M['render'], T_OUT)
wall('bld_w_b', 19, 6.5, 19, 23, 0, BLD_H, M['render'], T_OUT)
box('bld_w_lintel', 19 - T_OUT / 2, 5, 2.3, 19 + T_OUT / 2, 6.5, BLD_H, M['render'])
wall('bld_n', 19, 1, 41.2, 1, 0, BLD_H, M['render'], T_OUT)
wall('bld_s', 19, 23, 41.2, 23, 0, BLD_H, M['render'], T_OUT)
box('bld_e_band', 40.8, 1, 3.6, 41.2, 23, BLD_H, M['render'])     # above the glass
box('bld_roof', 19, 1, BLD_H, 41.2, 23, BLD_H + 0.2, M['render'], col='Ceilings')

# fire exit: hinged on the north jamb, swung ~70 deg out into the yard, propped with a brick
fx = box('fire_exit_door', 18.97, 5, 0, 19.03, 6.5, 2.3, M['steel'])
hinge = P(19, 5)
r = Matrix.Rotation(math.radians(-70), 3, 'Z')     # swings toward the yard (south-west)
fx.location = hinge + r @ (fx.location - hinge); fx.rotation_euler.z = math.radians(-70)
box('brick_wedge', 18.35, 6.55, 0, 18.7, 6.8, 0.12, M['brick'])
box('exit_sign', 18.7, 5.4, 2.5, 18.8, 6.1, 2.75, M['exit'])
light('exit_glow', 'POINT', P(18.5, 5.75, 2.6), (0.2, 1.0, 0.5), 60, size=0.15)
cyl('caged_bulb', 18.75, 4.6, 2.85, 3.0, 0.08, M['bulb'])
light('caged_bulb', 'POINT', P(18.6, 4.6, 2.9), (1.0, 0.9, 0.7), 70, size=0.08)
light('door_spill', 'AREA', P(19.8, 5.75, 2.2), (0.85, 0.9, 1.0), 15, size=(0.8, 1.4), aim=P(17, 5.75, 0))

# interior walls
CORR_CEIL, ARC_CEIL = 2.6, 4.0
for i, (x0, x1) in enumerate([(19, 21.5), (22.5, 25), (25.8, 29), (30, 33)]):
    wall(f'corr_n_{i}', x0, 4, x1, 4, 0, CORR_CEIL, M['wallin'], T_IN)
for i, x in enumerate([24, 27]):
    wall(f'room_div_{i}', x, 1, x, 4, 0, CORR_CEIL, M['wallin'], T_IN)
wall('corr_e', 33, 1, 33, 7.5, 0, ARC_CEIL, M['wallin'], T_IN)
wall('corr_s_a', 19, 7.5, 29.2, 7.5, 0, ARC_CEIL, M['wallin'], T_IN)
wall('corr_s_b', 30.8, 7.5, 33, 7.5, 0, ARC_CEIL, M['wallin'], T_IN)
box('staff_door_lintel', 29.2, 7.41, 2.2, 30.8, 7.59, ARC_CEIL, M['wallin'])
box('corr_ceiling', 19, 1, CORR_CEIL, 33, 7.5, CORR_CEIL + 0.1, M['ceiling'], col='Ceilings')
box('arcade_ceiling', 19, 7.5, ARC_CEIL, 41, 23, ARC_CEIL + 0.1, M['ceiling'], col='Ceilings')
box('prize_ceiling', 33, 1, ARC_CEIL, 41, 7.5, ARC_CEIL + 0.1, M['ceiling'], col='Ceilings')
# staff swing doors, ajar
for i, (hx, sgn) in enumerate([(29.2, 1), (30.8, -1)]):
    d = box(f'staff_door_{i}', hx if sgn > 0 else hx - 0.8, 7.47, 0, hx + 0.8 if sgn > 0 else hx, 7.53, 2.2, M['wallin'])
    h = P(hx, 7.5); r = Matrix.Rotation(math.radians(-sgn * 55), 3, 'Z')
    d.location = h + r @ (d.location - h); d.rotation_euler.z = math.radians(-sgn * 55)

# fluorescent tubes: dead, flicker, dead, on (the best one at the far end pulls you along)
for i, (x0, state) in enumerate([(20.6, 'dead'), (23.5, 'dim'), (26.6, 'dead'), (29.7, 'on')]):
    box(f'tube_{i}_{state}', x0, 5.65, CORR_CEIL - 0.06, x0 + 1.4, 5.85, CORR_CEIL, M['tube' if state == 'on' else 'tubedim' if state == 'dim' else 'tubedead'])
    if state != 'dead':
        light(f'tube_{i}', 'AREA', P(x0 + 0.7, 5.75, CORR_CEIL - 0.08), (0.9, 0.95, 1.0), 70 if state == 'on' else 14, size=(1.4, 0.2), aim=P(x0 + 0.7, 5.75, 0))

# rooms off the corridor
box('office_desk', 19.3, 1.3, 0, 21.3, 2.3, 0.75, M['desk'])
cyl('wc_toilet', 25.9, 1.7, 0, 0.45, 0.3, M['porcelain'])
box('wc_cistern', 25.55, 1.1, 0.45, 26.25, 1.35, 0.95, M['porcelain'])
box('store_shelves', 27.3, 1.2, 0, 32.7, 2.0, 2.2, M['wood'])
box('store_part_a', 27.3, 2.4, 0, 28.5, 3.6, 1.0, M['sheet'])
box('store_part_b', 29, 2.4, 0, 30.2, 3.6, 1.6, M['sheet'])

# prize counter and lobby
box('prize_counter', 34, 5.4, 0, 39.6, 6.2, 1.05, M['desk'])
box('prize_shelves', 34, 1.15, 0, 39.6, 1.85, 2.6, M['wood'])
box('change_machine', 39.4, 8.4, 0, 40.5, 9.6, 1.8, M['steel'])
box('air_hockey', 38.6, 17.6, 0, 40.6, 21.2, 0.8, M['pin'])
box('lobby_claw', 39.2, 12.6, 0, 40.4, 13.8, 2.0, M['cab'])

# ---------------------------------------------------------------- arcade cabinets
def cabinet(name, x, y, facing, sheeted=False):
    """upright cabinet, 0.9 x 0.95 footprint, screen on the facing side (+1 east / -1 west, plan)"""
    if sheeted:
        box(name + '_sheet', x - 0.05, y - 0.05, 0, x + 0.95, y + 1.0, 1.95, M['sheet']); return
    box(name, x, y, 0, x + 0.9, y + 0.95, 1.9, M['cab'])
    sx = x + 0.9 if facing > 0 else x - 0.02
    box(name + '_screen', sx, y + 0.12, 1.15, sx + 0.02, y + 0.83, 1.6, M['screen'])

k = 0
for rx, face in [(19.2, 1), (23.5, -1), (24.4, 1), (26.9, 1), (32.2, -1), (35.3, -1), (36.2, 1)]:
    y = 9.8
    while y < 18.5:
        cabinet(f'cab_{k}', rx, y, face, sheeted=(k * 7 + 3) % 5 == 0)
        k += 1; y += 1.15

def backwall(seq, x):
    for t in seq:
        if t == 'P':
            box(f'pinball_{x:.1f}', x, 21.45, 0, x + 0.7, 22.85, 0.95, M['pin'])
            box(f'pinball_back_{x:.1f}', x, 22.65, 0.95, x + 0.7, 22.85, 1.85, M['pin'])
            x += 0.85
        else:
            box(f'claw_{x:.1f}', x, 21.75, 0, x + 1.1, 22.85, 0.9, M['cab'])
            box(f'claw_glass_{x:.1f}', x + 0.05, 21.8, 0.9, x + 1.05, 22.8, 1.8, M['claw'])
            box(f'claw_top_{x:.1f}', x, 21.75, 1.8, x + 1.1, 22.85, 2.05, M['cab'])
            x += 1.25
backwall('PPCPPCPP', 19.4)
backwall('PPCPPCP', 31.3)

# Mine Shift: the one live cabinet
box('mineshift', 29.5, 22.05, 0, 30.5, 22.9, 1.9, M['cab'])
box('mineshift_screen', 29.6, 22.03, 1.15, 30.4, 22.05, 1.65, M['mine'])
box('mineshift_marquee', 29.5, 22.0, 1.72, 30.5, 22.05, 1.9, M['marquee'])
box('mineshift_panel', 29.5, 21.75, 0.95, 30.5, 22.05, 1.05, M['cab'])
cyl('mineshift_big_red_button', 30.0, 21.9, 1.05, 1.1, 0.07, M['red'])
light('mineshift_glow', 'AREA', P(30, 21.9, 1.4), (0.25, 1.0, 0.8), 70, size=(0.8, 0.5), aim=P(30, 18, 0.6))

# ---------------------------------------------------------------- frosted glass front + neon outside
for i, (y0, y1) in enumerate([(1, 5.4), (5.4, 9.8), (9.8, 14.2), (14.2, 18.6), (18.6, 23)]):
    box(f'front_glass_{i}', 40.97, y0, 0, 41.03, y1, 3.6, M['frost'])
    box(f'front_mullion_{i}', 40.9, y0 - 0.06, 0, 41.1, y0 + 0.06, 3.6, M['mullion'])
box('front_mullion_end', 40.9, 22.94, 0, 41.1, 23.06, 3.6, M['mullion'])
box('front_door_split', 40.92, 11.97, 0, 41.08, 12.03, 3.6, M['mullion'])
box('front_transom', 40.9, 1, 3.55, 41.1, 23, 3.65, M['mullion'])
cyl('front_door_chain', 40.85, 12, 1.0, 1.4, 0.05, M['steel'])

curve = bpy.data.curves.new('neon_sign', 'FONT')
curve.body = 'GRAVEYARD SHIFT'; curve.align_x = 'CENTER'; curve.align_y = 'CENTER'
curve.size = 0.75; curve.extrude = 0.02
neon = bpy.data.objects.new('neon_sign', curve)
neon.location = P(41.6, 12, 2.4)
neon.rotation_euler = (math.radians(90), 0, math.radians(90))   # faces the street, so it reads backwards inside
curve.materials.append(M['neon'])
COL['Blockout'].objects.link(neon)
light('street_front', 'AREA', P(44, 12, 3.0), (0.75, 0.85, 1.0), 2200, size=(1, 22), aim=P(41, 12, 1.5))
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
    'staff': camera('cam_4_staff_doors', P(30, 8.3, EYE), P(33.0, 22, EYE - 0.35)),
}
top = bpy.data.cameras.new('top'); top.type = 'ORTHO'; top.ortho_scale = 44
topob = bpy.data.objects.new('cam_top', top); topob.location = P(21, 12, 40)
COL['Cameras'].objects.link(topob)
CAMS['top'] = topob
scene.camera = CAMS['truck']

os.makedirs(os.path.join(OUT, 'renders'), exist_ok=True)
blend = os.path.join(OUT, 'intro_blockout.blend')
bpy.ops.wm.save_as_mainfile(filepath=blend)
print('intro: saved', blend)

if RENDER:
    views = list(CAMS) if RENDER == 'all' else RENDER.split(',')
    for v in views:
        scene.camera = CAMS[v]
        hide = v == 'top'
        COL['Ceilings'].hide_render = hide
        if hide:
            scene.render.resolution_x, scene.render.resolution_y = 1400, 800
        else:
            scene.render.resolution_x, scene.render.resolution_y = 1280, 800
        scene.render.filepath = os.path.join(OUT, 'renders', v + '.png')
        bpy.ops.render.render(write_still=True)
        print('intro: rendered', scene.render.filepath)
    COL['Ceilings'].hide_render = False
    scene.camera = CAMS['truck']
