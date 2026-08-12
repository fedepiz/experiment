package defs

////////////////////////////////
//~ fp: Shared Definitions
//
// This package holds the vocabularies that two or more packages must agree
// on, and that cannot live in one of them without a wrong dependency.
//
// The shared enum lives here, and each dependent names it directly: a
// feature is a defs.Feature on the board, a <Name>_Mask column of the thing
// database, and a prefix of the art files. The exhaustive switch in
// game.feature_ifield holds the enum and the columns together: a new member
// breaks its compile
// until the column exists.
//
// Put a vocabulary here only when the packages that share it must not depend
// on each other. A vocabulary with one owner belongs to that owner: the
// sprites live in the game package, because each of their readers imports it.
//
// This package uses nothing.

////////////////////////////////
//~ fp: Features
//
// A feature is a line that the map draws over the terrain. The database holds
// it as a connection mask between tiles. The board, the database and the game
// all read this enum, and the board must not know the database.

Feature :: enum {
	River,
	Road,
}

// the name of each feature to show, and its art prefix
FEATURE_KEYS :: [Feature]string{
	.River = "river",
	.Road = "road",
}
