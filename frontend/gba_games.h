#pragma once
// The GBA games in the all-games list (launcher.h), from Pokemon GBA Widescreen built in
// (embed.h, SSR_GBA): each as a Launcher::Game of the GBA section, with its box art.
#ifdef SSR_GBA
#include <map>
#include <string>
#include <vector>

#include "embed.h"
#include "launcher.h"

namespace GbaGames
{

// The game code the list goes by (Launcher::Order, a desktop shortcut's --game): the cartridge's
// own, except Ultra Violet's (a FireRed hack: BPRE too).
inline std::string CodeOf(const std::string& key)
{
    if (key == "emerald") return "BPEE";
    if (key == "ultraviolet") return "UV22";
    if (key == "firered") return "BPRE";
    if (key == "leafgreen") return "BPGE";
    return "GBA-" + key;
}

// Adds the GBA games in roms (saves in saves) to games; their pictures read once (artCache, by
// the ROM's path).
inline void Add(const std::string& roms, const std::string& saves, std::vector<Launcher::Game>& games,
                std::map<std::string, std::vector<uint32_t>>& artCache)
{
    for (const GbaWide::ListGame& l : GbaWide::Find(roms, saves))
    {
        Launcher::Game g;
        g.platform = Launcher::GBA;
        g.rom = l.file;
        g.folder = saves;
        g.code = CodeOf(l.key);
        g.name = l.name;
        g.saveName = l.key + ".sav";
        g.known = true; // (any GBA game plays: with widescreen when it is one of these)
        g.hasSave = l.hasSave;
        auto it = artCache.find(g.rom);
        if (it == artCache.end())
        {
            const unsigned char* jpeg = nullptr;
            size_t size = 0;
            if (!l.art.empty() && GbaWide::Art(l.art, jpeg, size)) Launcher::LoadArt(jpeg, size, g.art);
            it = artCache.emplace(g.rom, g.art).first;
        }
        g.art = it->second;
        games.push_back(std::move(g));
    }
}

}
#endif
