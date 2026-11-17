// Dump All Textures (Settings > Graphics > Experimental; for texture pack makers):
// every texture of the game's DataFiles\Textures.raw as
// resources\userdata\texture-dump\remastered\<id>.png (the base level, as the game's
// data has it), and, with an N64 ROM imported, every N64 picture (ids 0-3502 and the
// textures N64 model files keep inside themselves) as ...\n64\<id>.png in the N64's own
// colours. On a thread of its own; files already there are left alone. The as-drawn
// dump (pd_texture_dump) still exists for seeing which textures a scene uses.
#pragma once

#include <functional>
#include <string>

namespace pd::texdump {

// Starts the dump; `notice` is called with a start and a finish message (from the dump
// thread). False if a dump is already running.
bool StartDumpAll(std::function<void(const std::string&)> notice);
bool Running();

}  // namespace pd::texdump
