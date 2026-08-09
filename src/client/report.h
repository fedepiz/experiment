#pragma once
#include "base/core.h"

////////////////////////////////
//~ fp: temp: Worldgen Report
//
// `app worlds N [first_seed]` generates N worlds with no window, and writes one
// CSV row of measurements for each world to stdout. Read those rows with
// another program.

internal void report_worldgen(I32 world_count, U64 first_seed);
