#include "CustomSongCatalog.hpp"

#include "Utils/FileUtil.hpp"
#include "Utils/StrUtil.hpp"

#include <algorithm>
#include <exception>
#include <iterator>
#include <system_error>

namespace piko::gamedata
{
    std::filesystem::path custom_songs_root( const GameDataRoot &root )
    {
        if( root.application_root().empty() )
        {
            return {};
        }

        return ( root.application_root() / "Songs" ).lexically_normal();
    }

    std::vector<PlayableStageEntry> custom_song_entries( const filetypes::custom_song::SongInfo &info )
    {
        std::vector<PlayableStageEntry> entries;
        entries.reserve( info.charts.size() );

        std::string detail = info.artist.empty() ? std::string( "Unknown artist" ) : info.artist;

        if( !info.charter.empty() )
        {
            detail += " | Charted by " + info.charter;
        }

        for( const filetypes::custom_song::ChartEntry &chart : info.charts )
        {
            PlayableStageEntry entry{};
            entry.mode = PlayMode::SinglePlayer;

            entry.stage_number = -1;

            entry.chc_path = chart.file;

            entry.chc_exists = true;

            entry.tempo = 120.0f;

            entry.play_immediately = true;

            entry.show_chart = true;

            entry.custom_song = true;

            entry.ruleset = StageRuleset::Free;

            entry.custom_song_id = utils::path_to_utf8( info.folder.filename() );

            entry.custom_song_folder = info.folder;

            entry.asset_stage_number = info.asset_stage;

            entry.chart_variant = chart.name;

            entry.display_name = info.title;

            entry.short_name = info.title + " - " + chart.name;

            entry.detail_line = detail;
            entries.push_back( std::move( entry ) );
        }

        return entries;
    }

    std::vector<PlayableStageEntry> load_custom_song_entries( const std::filesystem::path &folder )
    {
        return custom_song_entries( filetypes::custom_song::load_song_info( folder ) );
    }

    std::vector<PlayableStageEntry> load_custom_song_catalog( const GameDataRoot &root,
        std::vector<std::string> &errors )
    {
        std::vector<PlayableStageEntry> entries;
        const std::filesystem::path songs_root = custom_songs_root( root );

        if( songs_root.empty() || !utils::directory( songs_root ) )
        {
            return entries;
        }

        std::error_code ec{};

        for( const std::filesystem::directory_entry &item : std::filesystem::directory_iterator( songs_root, ec ) )
        {
            if( !item.is_directory( ec ) || !utils::regular_file( item.path() / "song.json" ) )
            {
                continue;
            }

            try
            {
                std::vector<PlayableStageEntry> song = load_custom_song_entries( item.path() );
                entries.insert( entries.end(),
                    std::make_move_iterator( song.begin() ),
                    std::make_move_iterator( song.end() ) );
            }
            catch( const std::exception &e )
            {
                errors.push_back( e.what() );
            }
        }

        // Stable keeps each song's charts in song.json order.
        std::stable_sort( entries.begin(),
            entries.end(),
            []( const PlayableStageEntry &left, const PlayableStageEntry &right )
            {
                const std::string left_title = utils::lowercase_ascii( left.display_name );
                const std::string right_title = utils::lowercase_ascii( right.display_name );

                if( left_title != right_title )
                {
                    return left_title < right_title;
                }

                return left.custom_song_id < right.custom_song_id;
            } );

        return entries;
    }
}
