#!/usr/bin/env python3
"""Procedural terrain tiles, in the spirit of the reference sheets in
assets/reference (world_map_tiles / _mountains / _edge_shapes).

Emits individual PNGs to assets/tiles/; the game loads whatever is there and
packs a spritesheet at startup (main.c, map_assets_load), falling back to
flat colors for terrains with no art. Fully deterministic: every image is
seeded by its name, so re-running reproduces identical pixels.

Ground is ONE PAINTING per terrain, <terrain>_ground.png: a 64x64 canvas
whose texture wraps toroidally (blotch clumps, wave dashes, dune lines all
continue across the wrap). The game cuts it into a 4x4 grid of windows and
shows window (x%4, y%4) at map position (x, y), so neighboring tiles
continue one texture -- no seams inside a terrain region, and the 4-tile
repeat hides in the noise.

On top of the ground live:
- overlays  <terrain>_overlay_<n>.png  pictographic art (tree clusters, rock
  masses): composed drawings with a silhouette, top-left light, and a seated
  dark bottom edge
- edges     <terrain>_edge_<n>.png     sparse art for region-border tiles
- masks     mask_<case>_<n>.png        terrain-AGNOSTIC boundary masks
  (white, alpha = keep): the game renders the spilling terrain's own ground
  through them at draw time on dual cells (points where four map tiles
  meet), so transitions are never baked into art. <case> is the 4-bit
  corner code from game/tiling.h (NW=1, NE=2, SW=4, SE=8); the 14
  non-trivial cases are rotations of 4 canonical shapes (corner, half,
  saddle, inner). Contours enter and exit at edge midpoints -- the wander is
  free in between -- so contours of neighboring dual cells connect.
"""

import math
import os
import random
from PIL import Image

SIZE = 16          # one tile
GROUND_GRID = 4    # ground torus is GROUND_GRID x GROUND_GRID tiles
GROUND = SIZE * GROUND_GRID
VARIANTS = 8       # overlay / edge variant count
MS_VARIANTS = 4    # boundary variants per marching-squares case
OUT_DIR = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "tiles"))
PREVIEW_SCALE = 6
TAU = 6.28318530718


# ------------------------------------------------------------------ helpers

def weighted_shade(rng, weights):
    r = rng.random()
    acc = 0.0
    for idx, w in enumerate(weights):
        acc += w
        if r < acc:
            return idx
    return len(weights) - 1


def majority_pass(grid):
    """Toroidal 3x3 majority filter; ties keep the center pixel."""
    h, w = len(grid), len(grid[0])
    out = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            counts = {}
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    v = grid[(y + dy) % h][(x + dx) % w]
                    counts[v] = counts.get(v, 0) + 1
            center = grid[y][x]
            best, best_n = center, counts.get(center, 0)
            for v, n in counts.items():
                if n > best_n:
                    best, best_n = v, n
            out[y][x] = best
    return out


def blotch_field(rng, ramp, weights, w, h, passes=2):
    grid = [[weighted_shade(rng, weights) for _ in range(w)] for _ in range(h)]
    for _ in range(passes):
        grid = majority_pass(grid)
    return [[ramp[grid[y][x]] for x in range(w)] for y in range(h)]


def put(px, x, y, color):
    if 0 <= x < len(px[0]) and 0 <= y < len(px):
        px[y][x] = color


def put_wrap(px, x, y, color):
    px[y % len(px)][x % len(px[0])] = color


def speckles(rng, px, count, color):
    h, w = len(px), len(px[0])
    for _ in range(count):
        put_wrap(px, rng.randrange(w), rng.randrange(h), color)


def dashes(rng, px, count, color, length=(2, 4)):
    h, w = len(px), len(px[0])
    for _ in range(count):
        n = rng.randrange(length[0], length[1] + 1)
        x = rng.randrange(w)
        y = rng.randrange(h)
        for i in range(n):
            put_wrap(px, x + i, y, color)


def area(px):
    return len(px) * len(px[0]) / (SIZE * SIZE)  # in tiles


# ------------------------------------------------------- ground painters
#
# Each paints a w x h canvas (16x16 for fringes, 64x64 for the ground torus)
# with wrap-safe techniques, so any canvas tiles against itself seamlessly.
# `decorated` sprinkles the rare accents (flowers, cacti, reeds, ...).

def paint_plains(rng, decorated, w=SIZE, h=SIZE):
    ramp = [(100, 136, 66), (120, 158, 84), (134, 172, 96)]
    px = blotch_field(rng, ramp, (0.25, 0.50, 0.25), w, h)
    speckles(rng, px, int(7 * area(px)), (88, 120, 58))
    dashes(rng, px, int(3 * area(px)), (144, 182, 104), length=(2, 3))
    if decorated:
        flower_colors = [(232, 232, 218), (224, 200, 90), (204, 122, 112)]
        for _ in range(max(2, int(0.5 * area(px)))):
            put_wrap(px, rng.randrange(w), rng.randrange(h), rng.choice(flower_colors))
    return px


def paint_forest(rng, decorated, w=SIZE, h=SIZE):
    ramp = [(46, 88, 50), (59, 107, 61), (72, 122, 72)]
    px = blotch_field(rng, ramp, (0.30, 0.50, 0.20), w, h)
    speckles(rng, px, int(8 * area(px)), (36, 72, 42))
    if decorated:
        for _ in range(max(1, int(0.3 * area(px)))):
            put_wrap(px, rng.randrange(w), rng.randrange(h), (186, 92, 74))  # mushroom cap
    return px


def paint_mountain(rng, decorated, w=SIZE, h=SIZE):
    del decorated  # rock is rock
    ramp = [(114, 110, 105), (138, 133, 128), (158, 153, 147)]
    px = blotch_field(rng, ramp, (0.30, 0.45, 0.25), w, h, passes=3)
    for _ in range(int(3 * area(px))):  # cracks: short dark diagonals
        x = rng.randrange(w)
        y = rng.randrange(h)
        step = rng.choice([1, -1])
        for i in range(rng.randrange(2, 4)):
            put_wrap(px, x + i, y + i * step, (92, 88, 84))
    speckles(rng, px, int(3 * area(px)), (196, 192, 186))  # sun-caught edges
    return px


def paint_desert(rng, decorated, w=SIZE, h=SIZE):
    ramp = [(188, 166, 108), (199, 179, 122), (212, 194, 140)]
    px = blotch_field(rng, ramp, (0.12, 0.62, 0.26), w, h)
    # dune lines: wavy, broken, frequency quantized to whole waves per canvas
    # so the pattern continues across the wrap
    waves = max(1, round(0.55 * w / TAU))
    freq = waves * TAU / w
    spacing = rng.randrange(4, 6)
    phase = rng.random() * TAU
    for row in range(rng.randrange(spacing), h, spacing):
        for x in range(w):
            if rng.random() < 0.82:
                y = row + int(round(1.1 * math.sin(phase + x * freq)))
                put_wrap(px, x, y, (170, 148, 94))
    if decorated:
        for _ in range(max(1, int(0.25 * area(px)))):
            x = rng.randrange(w)
            y = rng.randrange(h)
            if rng.random() < 0.5:  # cactus
                for i in range(3):
                    put_wrap(px, x, y - i, (74, 110, 64))
                put_wrap(px, x - 1, y - 1, (74, 110, 64))
                put_wrap(px, x + 1, y - 2, (74, 110, 64))
            else:  # bleached bones
                for i in range(rng.randrange(2, 4)):
                    put_wrap(px, x + i, y, (226, 220, 200))
    return px


def paint_water(rng, decorated, w=SIZE, h=SIZE):
    del decorated
    ramp = [(40, 72, 128), (46, 82, 140), (54, 92, 152)]
    px = blotch_field(rng, ramp, (0.30, 0.55, 0.15), w, h)
    dashes(rng, px, int(4 * area(px)), (92, 132, 188), length=(3, 5))
    speckles(rng, px, max(1, int(0.35 * area(px))), (180, 205, 235))  # sparkles
    return px


def paint_swamp(rng, decorated, w=SIZE, h=SIZE):
    ramp = [(62, 90, 76), (77, 107, 92), (90, 120, 102)]
    px = blotch_field(rng, ramp, (0.30, 0.45, 0.25), w, h)
    dashes(rng, px, int(3 * area(px)), (84, 118, 124), length=(2, 3))  # standing water glints
    speckles(rng, px, int(6 * area(px)), (48, 74, 62))
    if decorated:
        for _ in range(max(1, int(0.5 * area(px)))):  # reeds
            x = rng.randrange(w)
            y = rng.randrange(h)
            put_wrap(px, x, y, (122, 130, 72))
            put_wrap(px, x, y - 1, (122, 130, 72))
    return px


def paint_ocean(rng, decorated, w=SIZE, h=SIZE):
    del decorated
    ramp = [(24, 48, 96), (30, 56, 108), (36, 64, 118)]
    px = blotch_field(rng, ramp, (0.35, 0.50, 0.15), w, h)
    dashes(rng, px, int(2 * area(px)), (50, 80, 134), length=(3, 5))  # long, sparse swell
    return px


def paint_hills(rng, decorated, w=SIZE, h=SIZE):
    ramp = [(118, 124, 82), (132, 138, 92), (146, 152, 104)]
    px = blotch_field(rng, ramp, (0.30, 0.45, 0.25), w, h)
    dashes(rng, px, int(3 * area(px)), (106, 110, 72), length=(2, 4))  # slope shadows
    speckles(rng, px, int(3 * area(px)), (158, 160, 118))
    if decorated:
        for _ in range(max(1, int(0.3 * area(px)))):  # outcrop flecks
            x = rng.randrange(w)
            y = rng.randrange(h)
            put_wrap(px, x, y, (152, 148, 138))
            put_wrap(px, x + 1, y, (128, 124, 114))
    return px


def paint_badlands(rng, decorated, w=SIZE, h=SIZE):
    del decorated
    ramp = [(142, 88, 58), (160, 104, 68), (176, 120, 80)]
    px = blotch_field(rng, ramp, (0.30, 0.45, 0.25), w, h)
    # strata: broken horizontal terrace lines
    for row in range(rng.randrange(3), h, rng.randrange(3, 5)):
        for x in range(w):
            if rng.random() < 0.7:
                put_wrap(px, x, row, (118, 70, 46))
    speckles(rng, px, int(3 * area(px)), (196, 142, 100))
    return px


def paint_beach(rng, decorated, w=SIZE, h=SIZE):
    ramp = [(206, 190, 146), (218, 202, 158), (228, 214, 172)]
    px = blotch_field(rng, ramp, (0.20, 0.55, 0.25), w, h)
    speckles(rng, px, int(5 * area(px)), (190, 172, 128))
    if decorated:
        for _ in range(max(1, int(0.2 * area(px)))):  # shell flecks
            put_wrap(px, rng.randrange(w), rng.randrange(h), (240, 234, 216))
    return px


def paint_ice(rng, decorated, w=SIZE, h=SIZE):
    del decorated
    ramp = [(196, 212, 228), (210, 224, 238), (224, 236, 248)]
    px = blotch_field(rng, ramp, (0.25, 0.50, 0.25), w, h)
    dashes(rng, px, int(2 * area(px)), (168, 190, 214), length=(3, 6))  # pressure cracks
    speckles(rng, px, int(2 * area(px)), (242, 248, 252))
    return px


def paint_snowcap(rng, decorated, w=SIZE, h=SIZE):
    del decorated
    ramp = [(206, 212, 224), (222, 228, 238), (238, 242, 250)]
    px = blotch_field(rng, ramp, (0.30, 0.45, 0.25), w, h, passes=3)
    for _ in range(int(2 * area(px))):  # rock showing through the snow
        x = rng.randrange(w)
        y = rng.randrange(h)
        step = rng.choice([1, -1])
        for i in range(rng.randrange(2, 4)):
            put_wrap(px, x + i, y + i * step, (150, 150, 156))
    return px


def paint_tundra(rng, decorated, w=SIZE, h=SIZE):
    ramp = [(148, 154, 136), (162, 168, 150), (176, 182, 162)]
    px = blotch_field(rng, ramp, (0.30, 0.45, 0.25), w, h)
    speckles(rng, px, int(5 * area(px)), (134, 140, 124))
    if decorated:
        for _ in range(max(1, int(0.4 * area(px)))):  # rust lichen
            put_wrap(px, rng.randrange(w), rng.randrange(h), (168, 138, 104))
    return px


def paint_taiga(rng, decorated, w=SIZE, h=SIZE):
    ramp = [(40, 68, 58), (50, 82, 68), (60, 94, 78)]
    px = blotch_field(rng, ramp, (0.30, 0.50, 0.20), w, h)
    speckles(rng, px, int(6 * area(px)), (32, 56, 48))
    if decorated:
        for _ in range(max(1, int(0.3 * area(px)))):  # snow patches
            put_wrap(px, rng.randrange(w), rng.randrange(h), (208, 218, 224))
    return px


def paint_jungle(rng, decorated, w=SIZE, h=SIZE):
    ramp = [(26, 88, 42), (34, 104, 50), (44, 120, 60)]
    px = blotch_field(rng, ramp, (0.30, 0.50, 0.20), w, h)
    speckles(rng, px, int(8 * area(px)), (20, 72, 36))
    if decorated:
        flower = [(214, 160, 62), (198, 84, 110)]
        for _ in range(max(1, int(0.4 * area(px)))):
            put_wrap(px, rng.randrange(w), rng.randrange(h), rng.choice(flower))
    return px


def paint_savanna(rng, decorated, w=SIZE, h=SIZE):
    ramp = [(148, 138, 78), (164, 152, 88), (178, 166, 98)]
    px = blotch_field(rng, ramp, (0.25, 0.50, 0.25), w, h)
    dashes(rng, px, int(4 * area(px)), (134, 124, 70), length=(2, 4))  # dry grass
    if decorated:
        for _ in range(max(1, int(0.3 * area(px)))):
            put_wrap(px, rng.randrange(w), rng.randrange(h), (110, 96, 58))
    return px


TERRAINS = {
    "water": paint_water,
    "plains": paint_plains,
    "forest": paint_forest,
    "mountain": paint_mountain,
    "desert": paint_desert,
    "swamp": paint_swamp,
    "ocean": paint_ocean,
    "hills": paint_hills,
    "badlands": paint_badlands,
    "beach": paint_beach,
    "ice": paint_ice,
    "snowcap": paint_snowcap,
    "tundra": paint_tundra,
    "taiga": paint_taiga,
    "jungle": paint_jungle,
    "savanna": paint_savanna,
}


# ------------------------------------------------------------- overlays
#
# Pictographic sprites drawn over the ground tiles -- the reference sheets'
# real character. These are not noise: each is a composed drawing with a
# silhouette, a consistent top-left light, a shadow side, and a dark edge
# where the shape meets the background. Transparent background; the game
# draws them on the terrain tile beneath.

def new_canvas():
    return [[None] * SIZE for _ in range(SIZE)]


def outline_silhouette(px, edge_color, bottom_color):
    """Darken shape pixels that border transparency; heaviest at the bottom,
    which is what seats a sprite on the ground instead of floating."""
    out = [row[:] for row in px]
    for y in range(SIZE):
        for x in range(SIZE):
            if px[y][x] is None:
                continue
            below = px[y + 1][x] if y + 1 < SIZE else None
            beside = ((px[y][x - 1] if x - 1 >= 0 else None),
                      (px[y][x + 1] if x + 1 < SIZE else None))
            if below is None:
                out[y][x] = bottom_color
            elif beside[0] is None or beside[1] is None:
                out[y][x] = edge_color
    return out


CANOPY_TEMPERATE = ((44, 82, 48), (58, 106, 60), (88, 136, 74))
CANOPY_JUNGLE = ((20, 76, 34), (30, 96, 44), (64, 132, 58))


def _canopy_mass(px, rng, blobs, palette=CANOPY_TEMPERATE):
    """A lumpy connected canopy: the union of overlapping discs, shaded as
    one mass by a top-left light with a dithered boundary. Aggregate, not
    tree portraits -- at world scale a tile is a whole woodland."""
    dark, mid, light = palette
    for y in range(SIZE):
        for x in range(SIZE):
            best = None
            for cx, cy, r in blobs:
                d2 = (x - cx) ** 2 + (y - cy) ** 2
                if d2 <= r * r and (best is None or d2 / (r * r) < best[0]):
                    best = (d2 / (r * r), cx, cy, r)
            if best is None:
                continue
            _, cx, cy, r = best
            lit = (-(x - cx) - (y - cy)) / max(r, 1e-3)
            lit += rng.uniform(-0.3, 0.3)
            px[y][x] = light if lit > 0.5 else dark if lit < -0.6 else mid


def _broadleaf_trees(px, rng, trees):
    """Round-crowned broadleaf silhouettes -- trunk under a lumpy crown --
    side-on like the conifers and umbrella trees, lit from the top-left.
    Trees sort by crown height so nearer (lower) ones overlap farther."""
    trunk = (86, 64, 40)
    dark, mid, light = CANOPY_TEMPERATE
    for cx, cy, r in sorted(trees, key=lambda t: t[1]):
        tx = int(round(cx))
        for ty in range(int(cy), min(int(cy + r + 3.0), SIZE - 1) + 1):
            put(px, tx, ty, trunk)
        # crown: the main disc plus a couple of offset lobes, so the outline
        # scallops like foliage instead of reading as a lollipop
        lobes = [(cx, cy, r)]
        for _ in range(rng.randrange(2, 4)):
            a = rng.uniform(0.0, TAU)
            lobes.append((cx + math.cos(a) * r * 0.6, cy + math.sin(a) * r * 0.4,
                          r * rng.uniform(0.45, 0.7)))
        for y in range(SIZE):
            for x in range(SIZE):
                best = None
                for lx, ly, lr in lobes:
                    d2 = (x - lx) ** 2 + (y - ly) ** 2
                    if d2 <= lr * lr and (best is None or d2 / (lr * lr) < best[0]):
                        best = (d2 / (lr * lr), lx, ly, lr)
                if best is None:
                    continue
                _, lx, ly, lr = best
                lit = (-(x - lx) - (y - ly)) / max(lr, 1e-3)
                lit += rng.uniform(-0.3, 0.3)
                px[y][x] = light if lit > 0.5 else dark if lit < -0.6 else mid


def paint_forest_mass(rng, decorated):
    """A clustered stand of broadleaf trees covering most of the tile."""
    del decorated
    px = new_canvas()
    trees = []
    for _ in range(rng.randrange(3, 6)):
        r = rng.uniform(2.2, 3.2)
        trees.append((rng.uniform(r + 0.5, SIZE - r - 0.5),
                      rng.uniform(r + 1.0, SIZE - r - 4.0), r))
    _broadleaf_trees(px, rng, trees)
    return outline_silhouette(px, (38, 70, 42), (30, 56, 34))


def paint_forest_fringe(rng, decorated):
    """Sparser lone trees for region borders: the woodland thins out."""
    del decorated
    px = new_canvas()
    trees = []
    for _ in range(rng.randrange(1, 3)):
        r = rng.uniform(1.8, 2.6)
        trees.append((rng.uniform(r + 0.5, SIZE - r - 0.5),
                      rng.uniform(r + 1.0, SIZE - r - 4.0), r))
    _broadleaf_trees(px, rng, trees)
    return outline_silhouette(px, (38, 70, 42), (30, 56, 34))


#        crack       dark        mid          lit          top
ROCK_RAMP = [(84, 81, 74), (110, 106, 98), (139, 135, 126), (170, 166, 156), (199, 195, 185)]


def _rock(px, rng, cx, cy, base_r, r_jitter, n_facets):
    """One irregular faceted rock, DF-style (world_map_mountains.png): a
    blobby angular silhouette partitioned into Voronoi facets, each facet
    shaded as one flat face by its position against a top-left light,
    quantized to a hard ramp so facet boundaries read as rock edges."""
    controls = [max(1.2, base_r + rng.uniform(-r_jitter, r_jitter)) for _ in range(6)]
    base_y = min(SIZE - 2, int(cy + base_r + 0.5))

    # facet seeds, then rank-assigned values: the topmost-lit facet is always
    # ramp 4 and the bottom crevice always ramp 1, so no rock can collapse
    # into a single flat grey blob
    seeds = []
    for _ in range(n_facets):
        a = rng.uniform(0.0, TAU)
        d = rng.uniform(0.0, base_r * 0.85)
        sx, sy = cx + math.cos(a) * d, cy + math.sin(a) * d
        t = (-(sx - cx) * 0.7 - (sy - cy) * 1.5) / max(base_r, 1e-3) + rng.uniform(-0.4, 0.4)
        seeds.append([sx, sy, t, 2])
    order = sorted(range(n_facets), key=lambda i: seeds[i][2])
    for rank, i in enumerate(order):
        seeds[i][3] = 1 + int(round(3.0 * rank / max(n_facets - 1, 1)))

    for y in range(SIZE):
        for x in range(SIZE):
            if y > base_y:
                continue
            dx, dy = x - cx, y - cy
            ang = math.atan2(dy, dx) % TAU
            k = ang / TAU * 6
            lo = int(k) % 6
            frac = k - int(k)
            r = controls[lo] * (1 - frac) + controls[(lo + 1) % 6] * frac
            if dx * dx + dy * dy > r * r:
                continue
            best, best_d = 2, 1e9
            for sx, sy, _, v in seeds:
                d2 = (x - sx) * (x - sx) + (y - sy) * (y - sy)
                if d2 < best_d:
                    best_d, best = d2, v
            px[y][x] = ROCK_RAMP[best]


def paint_rock_mass(rng, decorated):
    """A mountain here is a boulder the size of a county, sometimes with a
    companion at its foot."""
    del decorated
    px = new_canvas()
    _rock(px, rng, rng.uniform(7.2, 8.8), rng.uniform(7.0, 7.8), rng.uniform(6.4, 7.2),
          2.0, rng.randrange(5, 8))
    if rng.random() < 0.45:
        side = rng.choice([rng.uniform(2.0, 3.0), rng.uniform(13.0, 14.0)])
        _rock(px, rng, side, rng.uniform(11.5, 12.5), rng.uniform(1.8, 2.4), 0.8, 3)
    return outline_silhouette(px, (84, 81, 74), (66, 63, 57))


def paint_rock_scatter(rng, decorated):
    """2-4 small boulders for region edges: a massif crumbles into foothills
    instead of ending in a cliff of full-tile masses."""
    del decorated
    px = new_canvas()
    for _ in range(rng.randrange(2, 5)):
        r = rng.uniform(1.5, 2.6)
        _rock(px, rng, rng.uniform(r + 1.0, SIZE - r - 1.0), rng.uniform(5.5, 12.0),
              r, 0.9, 3)
    return outline_silhouette(px, (84, 81, 74), (66, 63, 57))


def paint_hill_mounds(rng, decorated):
    """Rounded flat-bottomed domes: foothills as a mass, like the canopy."""
    del decorated
    px = new_canvas()
    dark, mid, light = (104, 106, 72), (126, 128, 88), (152, 152, 108)
    mounds = []
    for _ in range(rng.randrange(2, 4)):
        r = rng.uniform(2.8, 4.2)
        mounds.append((rng.uniform(r, SIZE - r), rng.uniform(5.0, SIZE - 3.0), r))
    for cx, cy, r in sorted(mounds, key=lambda m: m[1]):
        for y in range(SIZE):
            for x in range(SIZE):
                dx, dy = x - cx, y - cy
                if dx * dx + dy * dy > r * r or y > cy + r * 0.55:
                    continue
                lit = (-dx - dy) / max(r, 1e-3) + rng.uniform(-0.25, 0.25)
                px[y][x] = light if lit > 0.45 else dark if lit < -0.55 else mid
    return outline_silhouette(px, (92, 94, 64), (76, 78, 52))


def paint_hill_knolls(rng, decorated):
    """1-2 small domes for region borders: the foothills flatten out."""
    del decorated
    px = new_canvas()
    dark, mid, light = (104, 106, 72), (126, 128, 88), (152, 152, 108)
    for _ in range(rng.randrange(1, 3)):
        r = rng.uniform(1.8, 2.6)
        cx = rng.uniform(r, SIZE - r)
        cy = rng.uniform(6.0, SIZE - 3.0)
        for y in range(SIZE):
            for x in range(SIZE):
                dx, dy = x - cx, y - cy
                if dx * dx + dy * dy > r * r or y > cy + r * 0.55:
                    continue
                lit = (-dx - dy) / max(r, 1e-3) + rng.uniform(-0.25, 0.25)
                px[y][x] = light if lit > 0.45 else dark if lit < -0.55 else mid
    return outline_silhouette(px, (92, 94, 64), (76, 78, 52))


def _conifer_spires(px, rng, spires):
    """Pointed conifer silhouettes -- the cold-forest read -- lit from the left."""
    dark, mid, light = (26, 52, 44), (36, 68, 54), (56, 92, 68)
    for cx, top, half_base, height in sorted(spires, key=lambda s: s[1]):
        base = min(top + height, SIZE - 1)
        for y in range(int(top), int(base) + 1):
            f = (y - top) / max(height, 1e-3)
            hw = half_base * f
            for x in range(int(cx - hw), int(cx + hw) + 1):
                if not 0 <= x < SIZE:
                    continue
                lit = (cx - x) / max(half_base, 1e-3) + rng.uniform(-0.3, 0.3)
                px[y][x] = light if lit > 0.5 else dark if lit < -0.5 else mid


def paint_taiga_mass(rng, decorated):
    del decorated
    px = new_canvas()
    spires = []
    for _ in range(rng.randrange(3, 6)):
        half = rng.uniform(1.6, 2.6)
        height = rng.uniform(7.0, 11.0)
        spires.append((rng.uniform(half + 0.5, SIZE - half - 0.5),
                       rng.uniform(1.0, SIZE - height - 1.0), half, height))
    _conifer_spires(px, rng, spires)
    return outline_silhouette(px, (22, 44, 38), (16, 34, 30))


def paint_taiga_sparse(rng, decorated):
    del decorated
    px = new_canvas()
    spires = []
    for _ in range(rng.randrange(1, 3)):
        half = rng.uniform(1.4, 2.0)
        height = rng.uniform(6.0, 9.0)
        spires.append((rng.uniform(half + 0.5, SIZE - half - 0.5),
                       rng.uniform(2.0, SIZE - height - 1.0), half, height))
    _conifer_spires(px, rng, spires)
    return outline_silhouette(px, (22, 44, 38), (16, 34, 30))


def _jungle_canopy(px, rng, crowns):
    """Tiered tropical canopy: flattened crowns stacked at varying heights,
    nearer layers overlapping farther ones so trunks show only in glimpses.
    Denser and darker than the broadleaf stands; lit from the top-left."""
    trunk = (66, 50, 32)
    dark, mid, light = CANOPY_JUNGLE
    for cx, cy, rx, ry in sorted(crowns, key=lambda c: c[1]):
        tx = int(round(cx))
        for ty in range(int(cy), min(int(cy + ry + 4.0), SIZE - 1) + 1):
            put(px, tx, ty, trunk)
        for y in range(SIZE):
            for x in range(SIZE):
                dx = (x - cx) / rx
                dy = (y - cy) / ry
                if dx * dx + dy * dy > 1.0:
                    continue
                lit = (-dx - dy) * 0.7 + rng.uniform(-0.25, 0.25)
                px[y][x] = light if lit > 0.4 else dark if lit < -0.5 else mid


def paint_jungle_mass(rng, decorated):
    """Layered crowns filling most of the tile: canopy with emergents."""
    del decorated
    px = new_canvas()
    crowns = []
    for _ in range(rng.randrange(4, 7)):
        rx = rng.uniform(2.6, 3.6)
        ry = rx * rng.uniform(0.55, 0.8)
        crowns.append((rng.uniform(rx + 0.5, SIZE - rx - 0.5),
                       rng.uniform(ry + 1.0, SIZE - ry - 5.0), rx, ry))
    _jungle_canopy(px, rng, crowns)
    return outline_silhouette(px, (16, 60, 28), (12, 46, 22))


def paint_jungle_sparse(rng, decorated):
    """A lone crown or two for region borders: the jungle opens up."""
    del decorated
    px = new_canvas()
    crowns = []
    for _ in range(rng.randrange(1, 3)):
        rx = rng.uniform(2.0, 2.8)
        ry = rx * rng.uniform(0.55, 0.8)
        crowns.append((rng.uniform(rx + 0.5, SIZE - rx - 0.5),
                       rng.uniform(ry + 1.0, SIZE - ry - 5.0), rx, ry))
    _jungle_canopy(px, rng, crowns)
    return outline_silhouette(px, (16, 60, 28), (12, 46, 22))


def paint_savanna_trees(rng, decorated):
    """1-2 flat-topped umbrella trees over open grass."""
    del decorated
    px = new_canvas()
    trunk = (96, 74, 46)
    dark, mid, light = (74, 96, 44), (92, 116, 54), (116, 140, 66)
    for _ in range(rng.randrange(1, 3)):
        cx = rng.uniform(3.5, SIZE - 3.5)
        cy = rng.uniform(4.0, 9.0)
        rx = rng.uniform(3.0, 4.5)
        ry = rng.uniform(1.2, 1.9)
        tx = int(round(cx))
        for ty in range(int(cy), min(int(cy) + 6, SIZE - 1)):
            put(px, tx, ty, trunk)
        for y in range(SIZE):
            for x in range(SIZE):
                dx = (x - cx) / rx
                dy = (y - cy) / ry
                if dx * dx + dy * dy > 1.0:
                    continue
                lit = (-dx - dy) * 0.7 + rng.uniform(-0.25, 0.25)
                px[y][x] = light if lit > 0.4 else dark if lit < -0.5 else mid
    return outline_silhouette(px, (60, 78, 36), (48, 62, 30))


OVERLAYS = {
    "forest": paint_forest_mass,
    "mountain": paint_rock_mass,
    "hills": paint_hill_mounds,
    "taiga": paint_taiga_mass,
    "jungle": paint_jungle_mass,
    "savanna": paint_savanna_trees,
}

# sparser art for tiles on a region's border, so forests thin out and
# massifs crumble into foothills the way the reference map reads
EDGE_OVERLAYS = {
    "forest": paint_forest_fringe,
    "mountain": paint_rock_scatter,
    "hills": paint_hill_knolls,
    "taiga": paint_taiga_sparse,
    "jungle": paint_jungle_sparse,
}


# ------------------------------------------------------------- boundaries
#
# Dual-grid marching-squares masks (see game/tiling.h for the taxonomy): a
# boundary sprite is one terrain's ground cut by the contour mask of a 4-bit
# corner case, drawn on dual cells -- the points where four map tiles meet.
# The 14 non-trivial cases compose from 4 canonical shapes: corner arcs,
# half planes, a connected saddle, and inner corners (arc complements).
# Contours are pinned to the edge MIDPOINTS and wander freely in between,
# which is what makes contours of neighboring dual cells connect.

CORNER_XY = ((0.0, 0.0), (SIZE, 0.0), (0.0, SIZE), (SIZE, SIZE))  # NW NE SW SE, tiling.h bit order


def wander(rng, amp=1.7):
    """Wavy offset over t in [-SIZE/2, SIZE/2], pinned to zero at both ends
    (the edge midpoints), free in between."""
    k = rng.randrange(1, 3)
    j_amp = rng.uniform(0.4, 0.9)
    j_phase = rng.uniform(0.0, TAU)

    def f(t):
        s = (t + SIZE / 2.0) / SIZE  # 0..1 along the contour
        pin = math.sin(math.pi * s)
        return pin * (amp * math.sin(math.pi * s * k) + j_amp * math.sin(j_phase + s * 11.0))
    return f


def corner_arc(rng, ci):
    """Quarter arc around one corner, midpoint to midpoint of its two edges."""
    cx0, cy0 = CORNER_XY[ci]
    w = wander(rng)

    def f(x, y):
        dx = abs(x + 0.5 - cx0)
        dy = abs(y + 0.5 - cy0)
        return dx + dy < SIZE / 2.0 + w(dx - dy)
    return f


def half_plane(rng, side):
    """One half of the cell, split midpoint to midpoint."""
    w = wander(rng)
    half = SIZE / 2.0

    def f(x, y):
        px, py = x + 0.5, y + 0.5
        if side == "n":
            return py < half + w(px - half)
        if side == "s":
            return py > half + w(px - half)
        if side == "w":
            return px < half + w(py - half)
        return px > half + w(py - half)
    return f


def saddle_neck(rng, main_diag):
    """The band joining two opposite corner arcs through the cell center."""
    width = rng.uniform(1.6, 2.4)
    j_amp = rng.uniform(0.0, 0.8)
    j_phase = rng.uniform(0.0, TAU)

    def f(x, y):
        px, py = x + 0.5, y + 0.5
        d = (px - py) if main_diag else (px + py - SIZE)
        t = (px + py - SIZE) if main_diag else (px - py)  # along the neck
        return abs(d) < width + j_amp * math.sin(j_phase + t * 0.5)
    return f


def ms_mask(rng, code):
    """True where the case's covered region keeps its pixels. `code` is the
    4-bit corner code (NW=1, NE=2, SW=4, SE=8)."""
    on = [i for i in range(4) if (code >> i) & 1]
    if len(on) == 1:
        pred = corner_arc(rng, on[0])
    elif len(on) == 3:
        off = [i for i in range(4) if i not in on][0]
        arc = corner_arc(rng, off)
        pred = lambda x, y: not arc(x, y)
    elif set(on) in ({0, 3}, {1, 2}):  # opposite corners: connected saddle
        a = corner_arc(rng, on[0])
        b = corner_arc(rng, on[1])
        neck = saddle_neck(rng, set(on) == {0, 3})
        pred = lambda x, y: a(x, y) or b(x, y) or neck(x, y)
    else:  # adjacent corners: half plane
        side = {frozenset({0, 1}): "n", frozenset({2, 3}): "s",
                frozenset({0, 2}): "w", frozenset({1, 3}): "e"}[frozenset(on)]
        pred = half_plane(rng, side)
    return [[pred(x, y) for x in range(SIZE)] for y in range(SIZE)]


# ------------------------------------------------------------- networks
#
# Rivers and roads: finished pieces per 4-bit connection case (N=1, E=2,
# S=4, W=8, matching BD_Dir). A channel wanders from each connected edge
# midpoint to the center; the wander vanishes at the edge, so pieces
# connect across tiles.

NET_VARIANTS = 4
NET_ENDS = ((SIZE / 2, 0.0), (SIZE, SIZE / 2), (SIZE / 2, SIZE), (0.0, SIZE / 2))  # N E S W


def channel_mask(rng, code, width):
    keep = [[False] * SIZE for _ in range(SIZE)]
    cx = SIZE / 2 + rng.uniform(-1.2, 1.2)
    cy = SIZE / 2 + rng.uniform(-1.2, 1.2)
    r = width / 2.0

    def stamp(sx, sy, sr):
        for y in range(max(0, int(sy - sr - 1)), min(SIZE, int(sy + sr + 2))):
            for x in range(max(0, int(sx - sr - 1)), min(SIZE, int(sx + sr + 2))):
                if (x + 0.5 - sx) ** 2 + (y + 0.5 - sy) ** 2 <= sr * sr:
                    keep[y][x] = True

    for bit in range(4):
        if not (code >> bit) & 1:
            continue
        ex, ey = NET_ENDS[bit]
        dx, dy = cx - ex, cy - ey
        length = math.hypot(dx, dy)
        nx, ny = -dy / length, dx / length  # perpendicular carries the wander
        amp = rng.uniform(0.8, 1.8)
        phase = rng.uniform(0.0, TAU)
        for i in range(13):
            t = i / 12.0
            w = amp * math.sin(math.pi * t) * math.sin(phase + t * 5.0)
            stamp(ex + dx * t + nx * w, ey + dy * t + ny * w, r)
    if bin(code).count("1") == 1:  # stub: a head where the flow peters out
        stamp(cx, cy, r + 0.8)
    return keep


def paint_network(rng, code, fill, bank, width, sparkle=None):
    px = new_canvas()
    keep = channel_mask(rng, code, width)
    for y in range(SIZE):
        for x in range(SIZE):
            if keep[y][x]:
                px[y][x] = rng.choice(fill)
    # banks: channel pixels beside open ground darken; the tile border stays
    # open, so the channel flows through
    out = [row[:] for row in px]
    for y in range(SIZE):
        for x in range(SIZE):
            if px[y][x] is None:
                continue
            for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                if 0 <= y + dy < SIZE and 0 <= x + dx < SIZE and px[y + dy][x + dx] is None:
                    out[y][x] = bank
    if sparkle:
        for y in range(SIZE):
            for x in range(SIZE):
                if out[y][x] is not None and out[y][x] != bank and rng.random() < 0.05:
                    out[y][x] = sparkle
    return out


# ------------------------------------------------------------------ output

def to_image(px):
    h, w = len(px), len(px[0])
    img = Image.new("RGBA", (w, h))
    img.putdata([(0, 0, 0, 0) if px[y][x] is None else (*px[y][x], 255)
                 for y in range(h) for x in range(w)])
    return img


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    sheet_rows = []
    grounds = {}

    # ground: one seamless torus painting per terrain, saved whole; the game
    # cuts the windows. The contact sheet shows it sliced, since its rows
    # are tile-sized.
    for name, painter in TERRAINS.items():
        rng = random.Random(f"{name}-ground")
        torus = to_image(painter(rng, True, GROUND, GROUND))
        torus.save(os.path.join(OUT_DIR, f"{name}_ground.png"))
        row = []
        for gy in range(GROUND_GRID):
            for gx in range(GROUND_GRID):
                row.append(torus.crop((gx * SIZE, gy * SIZE, (gx + 1) * SIZE, (gy + 1) * SIZE)))
        grounds[name] = row[0]
        sheet_rows.append((name, row))
        print(f"{name}: {GROUND}x{GROUND} ground painting")

    # boundary masks: terrain-agnostic white-with-alpha shapes; the game
    # draws any spilling terrain's ground through them at draw time
    mask_row = []
    for code in range(1, 15):
        for variant in range(MS_VARIANTS):
            rng = random.Random(f"mask-{code}:{variant}")
            keep = ms_mask(rng, code)
            img = Image.new("RGBA", (SIZE, SIZE))
            img.putdata([(255, 255, 255, 255) if keep[y][x] else (0, 0, 0, 0)
                         for y in range(SIZE) for x in range(SIZE)])
            img.save(os.path.join(OUT_DIR, f"mask_{code}_{variant}.png"))
            if variant == 0:
                mask_row.append(img)
    sheet_rows.append(("masks", mask_row))
    print(f"masks: 14 cases x {MS_VARIANTS} variants")

    # network pieces: rivers and roads by connection case
    for name, fill, bank, width, sparkle in (
        ("river", [(46, 82, 140), (54, 92, 152), (64, 104, 164)], (28, 52, 94), 5.0, (150, 190, 225)),
        ("road", [(168, 140, 96), (178, 150, 106)], (112, 88, 60), 4.2, None),
    ):
        net_row = []
        for code in range(1, 16):
            for variant in range(NET_VARIANTS):
                rng = random.Random(f"{name}-{code}:{variant}")
                img = to_image(paint_network(rng, code, fill, bank, width, sparkle))
                img.save(os.path.join(OUT_DIR, f"{name}_{code}_{variant}.png"))
                if variant == 0:
                    net_row.append(img)
        sheet_rows.append((name, net_row))
        print(f"{name}: 15 cases x {NET_VARIANTS} variants")

    for suffix, painters in (("overlay", OVERLAYS), ("edge", EDGE_OVERLAYS)):
        for name, painter in painters.items():
            row = []
            for variant in range(VARIANTS):
                rng = random.Random(f"{name}-{suffix}:{variant}")
                px = painter(rng, False)
                img = to_image(px)
                img.save(os.path.join(OUT_DIR, f"{name}_{suffix}_{variant}.png"))
                # preview composited on that terrain's ground so the sheet
                # shows what the game will actually draw
                composed = grounds[name].copy()
                composed.alpha_composite(img)
                row.append(composed)
            sheet_rows.append((f"{name} {suffix}", row))
            print(f"{name} {suffix}: {VARIANTS} variants")

    # contact sheet for eyeballing, next to the tiles but not loaded by the game
    pad = 2
    cols = max(len(row) for _, row in sheet_rows)
    sheet_w = (SIZE + pad) * cols + pad
    sheet_h = (SIZE + pad) * len(sheet_rows) + pad
    sheet = Image.new("RGBA", (sheet_w, sheet_h), (24, 26, 28, 255))
    for row_idx, (_, row) in enumerate(sheet_rows):
        for col_idx, img in enumerate(row):
            sheet.paste(img, (pad + col_idx * (SIZE + pad), pad + row_idx * (SIZE + pad)))
    sheet = sheet.resize((sheet_w * PREVIEW_SCALE, sheet_h * PREVIEW_SCALE), Image.NEAREST)
    sheet.save(os.path.join(OUT_DIR, "_contact_sheet.png"))
    print(f"contact sheet -> {os.path.join(OUT_DIR, '_contact_sheet.png')}")


if __name__ == "__main__":
    main()
