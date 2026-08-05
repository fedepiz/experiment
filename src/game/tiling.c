#include "game/tiling.h"

////////////////////////////////
//~ fp: Registration

internal TL_Class* tl__class(TL_Config* config, U32 klass) {
  TL_Class* result = 0;
  if(klass < TL_CLASS_CAP) {
    config->class_count = Max(config->class_count, klass + 1);
    result = &config->classes[klass];
  }
  return result;
}

internal void tl_class_set(TL_Config* config, U32 klass, U8 rank, U32 overlay_density) {
  TL_Class* c = tl__class(config, klass);
  if(c != 0) {
    c->rank = rank;
    c->overlay_density = overlay_density;
  }
}

internal void tl_push_ground(TL_Config* config, U32 klass, U32 gx, U32 gy, U32 id) {
  TL_Class* c = tl__class(config, klass);
  if(c != 0 && gx < TL_TORUS_GRID && gy < TL_TORUS_GRID) {
    c->ground_ids[gy][gx] = id;
  }
}

internal void tl_push_overlay(TL_Config* config, U32 klass, U32 id) {
  TL_Class* c = tl__class(config, klass);
  if(c != 0 && c->overlay_count < TL_VARIANT_CAP) {
    c->overlay_ids[c->overlay_count] = id;
    c->overlay_count += 1;
  }
}

internal void tl_push_edge_overlay(TL_Config* config, U32 klass, U32 id) {
  TL_Class* c = tl__class(config, klass);
  if(c != 0 && c->edge_overlay_count < TL_VARIANT_CAP) {
    c->edge_overlay_ids[c->edge_overlay_count] = id;
    c->edge_overlay_count += 1;
  }
}

internal void tl_push_mask(TL_Config* config, U32 code, U32 id) {
  if(code < 16 && config->mask_counts[code] < TL_VARIANT_CAP) {
    config->mask_ids[code][config->mask_counts[code]] = id;
    config->mask_counts[code] += 1;
  }
}

////////////////////////////////
//~ fp: Cell Query

// position-keyed noise: pure hash of (seed, position, stream), so appearance
// never flickers frame to frame. `stream` decorrelates the module's separate
// random decisions at one position.
internal U32 tl__noise(U64 seed, V2I p, U32 stream) {
  U64 h = seed ^ ((U64)(U32)p.x * 374761393u) ^ ((U64)(U32)p.y * 668265263u) ^
          ((U64)stream * 0x9E3779B97F4A7C15ull);
  h ^= h >> 30;
  h *= 0xBF58476D1CE4E5B9ull;
  h ^= h >> 27;
  h *= 0x94D049BB133111EBull;
  h ^= h >> 31;
  return (U32)h;
}

// the total covering order: rank decides, class id breaks ties
internal U32 tl__order(TL_Config* config, U32 klass) {
  return ((U32)config->classes[klass].rank << 16) | klass;
}

internal TL_Cell tl_cell(TL_Config* config, U32 neighborhood[9], V2I p) {
  TL_Cell result = {0};

  // bad ids read as class 0 -- the caller's diagnostic path (loud nil color)
  U32 nb[9];
  for(U32 i = 0; i < 9; i += 1) {
    nb[i] = neighborhood[i] < config->class_count ? neighborhood[i] : 0;
  }
  U32 klass = nb[4];
  TL_Class* def = &config->classes[klass];

  //- ground: a world-space window of the class's torus, so neighboring
  // cells continue one seamless texture
  {
    TL_Piece* piece = &result.pieces[result.count];
    result.count += 1;
    piece->klass = klass;
    piece->layer = TL_Layer_Surface;
    piece->id = def->ground_ids[p.y & (TL_TORUS_GRID - 1)][p.x & (TL_TORUS_GRID - 1)];
  }

  //- boundary layers at the NW dual cell (corners: NW, N, W, self)
  {
    U32 corners[4] = {nb[0], nb[1], nb[3], nb[4]}; // TL_Corner_* bit order
    U32 present[4];
    U32 present_count = 0;
    for(U32 i = 0; i < 4; i += 1) {
      B32 seen = 0;
      for(U32 j = 0; j < present_count; j += 1) { seen |= present[j] == corners[i]; }
      if(!seen) {
        present[present_count] = corners[i];
        present_count += 1;
      }
    }
    // ascending covering order; [0] is the background and emits no layer
    for(U32 i = 1; i < present_count; i += 1) {
      for(U32 j = i; j > 0 && tl__order(config, present[j]) < tl__order(config, present[j - 1]); j -= 1) {
        Swap(U32, present[j], present[j - 1]);
      }
    }
    for(U32 i = 1; i < present_count; i += 1) {
      U32 layer_klass = present[i];
      TL_Class* layer_def = &config->classes[layer_klass];
      U32 code = 0;
      for(U32 c = 0; c < 4; c += 1) {
        if(tl__order(config, corners[c]) >= tl__order(config, layer_klass)) { code |= 1u << c; }
      }
      if(config->mask_counts[code] == 0) { continue; }
      // the spill is the class's own ground through the case mask; a class
      // with no ground has nothing to spill. The window is hash-picked, not
      // world-aligned -- tongue noise over tile noise reads fine.
      U32 window = tl__noise(config->seed, p, 2 + layer_klass);
      U32 ground_id = layer_def->ground_ids[(window >> 2) & (TL_TORUS_GRID - 1)][window & (TL_TORUS_GRID - 1)];
      if(ground_id == 0) { continue; }
      TL_Piece* piece = &result.pieces[result.count];
      result.count += 1;
      piece->id = ground_id;
      piece->mask_id = config->mask_ids[code][tl__noise(config->seed, p, 6 + layer_klass) % config->mask_counts[code]];
      piece->klass = layer_klass;
      piece->layer = TL_Layer_Surface;
      piece->offset = (V2){-0.5f, -0.5f};
    }
  }

  //- overlay art tapers at the region border: sparse edge art there, full
  // art inside -- with bare gaps so a massif or forest reads as scattered
  // shapes, not a carpet of one tile art per cell
  {
    B32 on_border = 0;
    for(U32 i = 0; i < 9; i += 1) { on_border |= nb[i] != klass; }
    U32* ids = 0;
    U32 variants = 0;
    if(on_border && def->edge_overlay_count > 0) {
      ids = def->edge_overlay_ids;
      variants = def->edge_overlay_count;
    } else if(def->overlay_count > 0 &&
              tl__noise(config->seed, p, 1) % 100 < def->overlay_density) {
      ids = def->overlay_ids;
      variants = def->overlay_count;
    }
    if(ids != 0) {
      TL_Piece* piece = &result.pieces[result.count];
      result.count += 1;
      piece->id = ids[tl__noise(config->seed, p, 0) % variants];
      piece->klass = klass;
      piece->layer = TL_Layer_Object;
    }
  }

  return result;
}
