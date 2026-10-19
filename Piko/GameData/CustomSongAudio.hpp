#pragma once

#include "FileTypes/CustomSong/CustomSong.hpp"

#include <cstdint>
#include <filesystem>

namespace piko::gamedata
{
    struct PreparedCustomSongAudio
    {
        std::filesystem::path directory;
        std::int64_t frames = 0;
        std::uint16_t channels = 0;

        [[nodiscard]] double length_ms() const;
    };

    // Decodes the song audio to a cached 48 kHz WAV with a piko-audio manifest. Reuses the cache while the source is
    // unchanged.
    [[nodiscard]] PreparedCustomSongAudio prepare_custom_song_audio( const filetypes::custom_song::SongInfo &info,
        const std::filesystem::path &application_root );
}
