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


TERRAINS = {
    "water": paint_water,
    "plains": paint_plains,
    "forest": paint_forest,
    "mountain": paint_mountain,
    "desert": paint_desert,
    "swamp": paint_swamp,
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


def _draw_canopies(px, rng, canopies):
    dark, mid, light = (44, 82, 48), (58, 106, 60), (88, 136, 74)
    trunk = (82, 62, 42)
    canopies = sorted(canopies, key=lambda c: c[1])  # back (high) to front (low)
    for cx, cy, r in canopies:
        tx = int(round(cx))
        for ty in range(int(cy), min(int(cy + r) + 3, SIZE - 1)):
            put(px, tx, ty, trunk)  # trunk first; canopy overwrites its top
        for y in range(SIZE):
            for x in range(SIZE):
                dx, dy = x - cx, y - cy
                if dx * dx + dy * dy > r * r:
                    continue
                lit = (-dx - dy) / max(r, 1e-3)  # top-left light
                lit += rng.uniform(-0.25, 0.25)  # dithered shade boundary
                if lit > 0.45:
                    px[y][x] = light
                elif lit < -0.55:
                    px[y][x] = dark
                else:
                    px[y][x] = mid


def paint_tree_cluster(rng, decorated):
    """2-4 round canopies with trunks, back row first so the front overlaps."""
    del decorated
    px = new_canvas()
    canopies = []
    for _ in range(rng.randrange(2, 5)):
        r = rng.uniform(2.6, 3.6)
        canopies.append((rng.uniform(r + 0.5, SIZE - r - 0.5), rng.uniform(4.0, 9.5), r))
    _draw_canopies(px, rng, canopies)
    return outline_silhouette(px, (38, 70, 42), (30, 56, 34))


def paint_tree_sparse(rng, decorated):
    """1-2 smaller trees for region edges: forests thin out at their border
    instead of stopping like a wall."""
    del decorated
    px = new_canvas()
    canopies = []
    for _ in range(rng.randrange(1, 3)):
        r = rng.uniform(2.0, 2.8)
        canopies.append((rng.uniform(r + 0.5, SIZE - r - 0.5), rng.uniform(5.0, 10.0), r))
    _draw_canopies(px, rng, canopies)
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


OVERLAYS = {
    "forest": paint_tree_cluster,
    "mountain": paint_rock_mass,
}

# sparser art for tiles on a region's border, so forests thin out and
# massifs crumble into foothills the way the reference map reads
EDGE_OVERLAYS = {
    "forest": paint_tree_sparse,
    "mountain": paint_rock_scatter,
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
