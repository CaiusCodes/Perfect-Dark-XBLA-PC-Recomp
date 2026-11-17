// The game's own import code for the experimental extras (--n64-rom,
// --texture-pack): the N64 texture decoder (for the ROM check and .zip
// reading) and the importer. Our code (The Unlicense), compiled in here so the
// launcher and the game's Settings > Graphics > Experimental do the same thing.
#include "../../src/pd_n64_decode.cpp"
#include "../../src/pd_assets_import.cpp"
