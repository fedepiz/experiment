#pragma once
#include "base/core.h"

////////////////////////////////
//~ fp: temp: Worldgen Report
//
// `app worlds N [first_seed]`: generate N worlds headless and print one CSV
// row of metrics per world to stdout, for offline analysis.

internal void report_worldgen(I32 world_count, U64 first_seed);
