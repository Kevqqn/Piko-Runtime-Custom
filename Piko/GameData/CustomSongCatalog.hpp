#pragma once

#include "FileTypes/CustomSong/CustomSong.hpp"
#include "GameData/GameDataRoot.hpp"
#include "GameData/StageCatalog.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace piko::gamedata
{
    [[nodiscard]] std::filesystem::path custom_songs_root( const GameDataRoot &root );

    // One entry per chart of the song.
    [[nodiscard]] std::vector<PlayableStageEntry> custom_song_entries( const filetypes::custom_song::SongInfo &info );

    // Throws when the song folder is invalid.
    [[nodiscard]] std::vector<PlayableStageEntry> load_custom_song_entries( const std::filesystem::path &folder );

    // Skips invalid song folders and reports them in errors.
    [[nodiscard]] std::vector<PlayableStageEntry> load_custom_song_catalog( const GameDataRoot &root,
        std::vector<std::string> &errors );
}
