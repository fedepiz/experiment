#pragma once

////////////////////////////////
//~ fp: Shared Definitions
//
// This file holds the vocabularies that two or more modules must agree on.
// Each vocabulary is one X-macro list, declared one time. A module expands the
// list where it needs it.
//
// A list is not a namespace. It writes its entries into the enums that own
// those names already. One line of the feature list becomes a BD_Feature on
// the board, and a TH_IField, which is a column of the database. A new entry
// therefore appears in every module at the same time, and two lists that a
// person kept equal by hand cannot become different.
//
// Put a list here only when two or more modules must agree on it. A vocabulary
// with one reader belongs to that reader.
//
// This file uses nothing. Each module that expands one of its lists includes
// it.

////////////////////////////////
//~ fp: Features
//
// A feature is a line that the map draws over the terrain. The database holds
// it as a connection mask between tiles. Each feature is a BD_Feature on the
// board, a mask column in the database with the name <Name>Mask, and a prefix
// of the art files.

#define DF_FEATURE_LIST \
  X(River, "river")     \
  X(Road, "road")

////////////////////////////////
//~ fp: Sprites
//
// A sprite is how a thing looks. Each sprite is a GM_Sprite that the game
// gives to a thing, a name to show, and a prefix of the art files that the
// client reads at assets/tiles/site_<key>_N. The list does not hold the nil
// sprite, which is the ZII row for a thing with no art.

#define DF_SPRITE_LIST      \
  X(Band, "band")           \
  X(Village, "village")     \
  X(Palace, "palace")       \
  X(Herders, "herders")     \
  X(Wagon, "wagon")         \
  X(Tholos, "tholos")
