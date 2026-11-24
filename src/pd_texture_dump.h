// Dump All Textures (Settings > Graphics > Experimental; for texture pack makers):
// every texture of the game's DataFiles\Textures.raw as
// resources\userdata\texture-dump\ext_tex\xbla\<id>.png (the base level, as the game's
// data has it), and, with an N64 ROM imported, every N64 picture (ids 0-3502 and the
// textures N64 model files keep inside themselves) as ...\ext_tex\<id>.png in the N64's
// own colours. The PC-port pack layout (pd_assets_import.h): ids in four hex digits,
// pictures right way up, so ext_tex is itself a pack. On a thread of its own; files
// already there are left alone. The as-drawn dump (pd_texture_dump) still exists for
// seeing which textures a scene uses.
#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace pd::texdump {

// Starts the dump; `notice` is called with a start and a finish message (from the dump
// thread). False if a dump is already running.
bool StartDumpAll(std::function<void(const std::string&)> notice);
bool Running();

// Dump Game Textures... (Texture Packs > Texture Tools, HANDOVER 4du): one texture source.
// "Remastered": the Textures.raw dump above (ext_tex\xbla), "N64": the N64 pictures (ext_tex,
// the ROM loaded first if it isn't), anything else an installed pack's name: what the game
// takes from that pack (its pictures by texture number after its naming rules and
// pd-pack.txt's keep_original, each as the game turns it), into
// texture-dump\packs\<pack>\ (top folder and xbla\, the dump's naming and orientation, so the
// folder is itself a pack) with dump-summary.txt saying what was left out and why. Read
// from the installed files, whichever pack is shown. Files already there are left alone.
bool StartDump(const std::string& source, std::function<void(const std::string&)> notice);
// The folder a source's dump goes to.
std::filesystem::path SourceFolder(const std::string& source);

}  // namespace pd::texdump
