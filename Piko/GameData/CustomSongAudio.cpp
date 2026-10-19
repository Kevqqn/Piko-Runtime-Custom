#include "CustomSongAudio.hpp"

#include "Audio/PspAtracConverter.hpp"
#include "Utils/FileUtil.hpp"
#include "Utils/Json.hpp"
#include "Utils/StrUtil.hpp"

#include <array>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace piko::gamedata::custom_song_audio
{
    constexpr std::uint32_t sample_rate = 48000;
    constexpr std::int64_t manifest_version = 1;

    struct WavInfo
    {
        std::uint16_t channels = 0;
        std::uint32_t sample_rate = 0;
        std::uint16_t bits_per_sample = 0;
        std::uint64_t data_size = 0;
    };

    std::uint16_t read_u16( const std::uint8_t *bytes )
    {
        return static_cast<std::uint16_t>( bytes[ 0 ] | ( bytes[ 1 ] << 8 ) );
    }

    std::uint32_t read_u32( const std::uint8_t *bytes )
    {
        return static_cast<std::uint32_t>( bytes[ 0 ] ) | ( static_cast<std::uint32_t>( bytes[ 1 ] ) << 8 ) |
               ( static_cast<std::uint32_t>( bytes[ 2 ] ) << 16 ) | ( static_cast<std::uint32_t>( bytes[ 3 ] ) << 24 );
    }

    // Reads chunk headers only, the sample data is skipped.
    WavInfo read_wav_info( const std::filesystem::path &path )
    {
        std::ifstream file( path, std::ios::binary );
        std::array<std::uint8_t, 12> riff{};

        if( !file.read( reinterpret_cast<char *>( riff.data() ), riff.size() ) ||
            std::memcmp( riff.data(), "RIFF", 4 ) != 0 || std::memcmp( riff.data() + 8, "WAVE", 4 ) != 0 )
        {
            throw std::runtime_error( "decoded audio is not a WAV file" );
        }

        WavInfo info{};
        bool found_format = false;
        std::array<std::uint8_t, 8> header{};

        while( file.read( reinterpret_cast<char *>( header.data() ), header.size() ) )
        {
            const std::uint32_t size = read_u32( header.data() + 4 );

            if( std::memcmp( header.data(), "fmt ", 4 ) == 0 )
            {
                std::array<std::uint8_t, 16> format{};

                if( size < format.size() || !file.read( reinterpret_cast<char *>( format.data() ), format.size() ) )
                {
                    throw std::runtime_error( "decoded WAV has an invalid format chunk" );
                }

                info.channels = read_u16( format.data() + 2 );

                info.sample_rate = read_u32( format.data() + 4 );

                info.bits_per_sample = read_u16( format.data() + 14 );

                found_format = true;
                file.seekg( static_cast<std::streamoff>( size - format.size() + ( size & 1u ) ), std::ios::cur );
                continue;
            }

            if( std::memcmp( header.data(), "data", 4 ) == 0 )
            {
                if( !found_format )
                {
                    throw std::runtime_error( "decoded WAV has data before its format chunk" );
                }

                info.data_size = size;
                return info;
            }

            file.seekg( static_cast<std::streamoff>( size + ( size & 1u ) ), std::ios::cur );
        }

        throw std::runtime_error( "decoded WAV has no data chunk" );
    }

    std::string source_stamp( const std::filesystem::path &path )
    {
        std::error_code ec{};
        const auto size = std::filesystem::file_size( path, ec );
        const auto time = std::filesystem::last_write_time( path, ec ).time_since_epoch().count();

        std::ostringstream output;
        output << size << '-' << time << '-' << utils::path_to_utf8( path.lexically_normal() );
        return output.str();
    }

    std::string stamp_hash( const std::string &stamp )
    {
        std::ostringstream output;
        output << std::hex << std::setw( 16 ) << std::setfill( '0' ) << std::hash<std::string>{}( stamp );
        return output.str();
    }

    std::filesystem::path cache_directory( const filetypes::custom_song::SongInfo &info,
        const std::filesystem::path &application_root )
    {
        const std::string folder = utils::path_to_utf8( info.folder.filename() );
        const std::string name = utils::safe_ascii_stem( folder, "song", true ) + "_" +
                                 stamp_hash( utils::path_to_utf8( info.folder.lexically_normal() ) );

        return ( application_root / "Cache" / "CustomSongs" / name ).lexically_normal();
    }

    bool cache_matches( const std::filesystem::path &directory, const std::string &stamp )
    {
        try
        {
            const utils::json::Value root = utils::json::load_file( directory / "audio.json" );
            const utils::json::Value *source = root.find( "source_stamp" );

            return source != nullptr && source->string() == stamp_hash( stamp ) &&
                   root.require( "version" ).integer_or( 0 ) == manifest_version &&
                   utils::regular_file( directory / "SONG.wav" );
        }
        catch( const std::exception & )
        {
            return false;
        }
    }

    void write_manifest( const std::filesystem::path &directory,
        const std::string &stamp,
        std::uint16_t channels,
        std::int64_t frames )
    {
        const std::filesystem::path temp = directory / "audio.json.tmp";

        {
            std::ofstream output( temp, std::ios::binary | std::ios::trunc );
            output << "{\n"
                   << "  \"format\": \"piko-audio\",\n"
                   << "  \"version\": " << manifest_version << ",\n"
                   << "  \"layout\": \"interleaved-voices\",\n"
                   << "  \"source_stamp\": \"" << stamp_hash( stamp ) << "\",\n"
                   << "  \"cues\": [\n"
                   << "    {\n"
                   << "      \"index\": 0,\n"
                   << "      \"name\": \"" << filetypes::custom_song::audio_cue_name << "\",\n"
                   << "      \"file\": \"SONG.wav\",\n"
                   << "      \"codec\": \"wav\",\n"
                   << "      \"sample_rate\": " << sample_rate << ",\n"
                   << "      \"channels\": " << channels << ",\n"
                   << "      \"frames\": " << frames << ",\n"
                   << "      \"loop_start_frame\": -1,\n"
                   << "      \"loop_end_frame\": -1\n"
                   << "    }\n"
                   << "  ]\n"
                   << "}\n";

            if( !output )
            {
                throw std::runtime_error( "could not write the custom song audio manifest" );
            }
        }

        std::filesystem::rename( temp, directory / "audio.json" );
    }
}

namespace piko::gamedata
{
    double PreparedCustomSongAudio::length_ms() const
    {
        return static_cast<double>( frames ) * 1000.0 / custom_song_audio::sample_rate;
    }

    PreparedCustomSongAudio prepare_custom_song_audio( const filetypes::custom_song::SongInfo &info,
        const std::filesystem::path &application_root )
    {
        const std::filesystem::path directory = custom_song_audio::cache_directory( info, application_root );
        const std::filesystem::path wav_path = directory / "SONG.wav";
        const std::string stamp = custom_song_audio::source_stamp( info.audio_path );

        if( !custom_song_audio::cache_matches( directory, stamp ) )
        {
            std::filesystem::path ffmpeg;

            try
            {
                ffmpeg = convert::find_ffmpeg_executable( application_root );
            }
            catch( const std::exception & )
            {
                throw std::runtime_error( "FFmpeg is required to decode custom song audio" );
            }

            std::filesystem::create_directories( directory );
            std::filesystem::remove( directory / "audio.json" );
            convert::decode_audio_to_wav( info.audio_path, wav_path, ffmpeg, custom_song_audio::sample_rate );

            const custom_song_audio::WavInfo decoded = custom_song_audio::read_wav_info( wav_path );

            // Plain songs are stereo, 8 channels is the retail stem layout. Anything else is folded to stereo.
            if( decoded.channels != 2 && decoded.channels != 8 )
            {
                convert::decode_audio_to_wav( info.audio_path, wav_path, ffmpeg, custom_song_audio::sample_rate, 2 );
            }
        }

        const custom_song_audio::WavInfo wav = custom_song_audio::read_wav_info( wav_path );

        if( wav.sample_rate != custom_song_audio::sample_rate || wav.bits_per_sample != 16 || wav.channels == 0 )
        {
            throw std::runtime_error( "decoded custom song audio is not 16-bit 48 kHz PCM" );
        }

        PreparedCustomSongAudio prepared{};
        prepared.directory = directory;

        prepared.channels = wav.channels;

        prepared.frames = static_cast<std::int64_t>( wav.data_size / ( wav.channels * 2u ) );

        if( !custom_song_audio::cache_matches( directory, stamp ) )
        {
            custom_song_audio::write_manifest( directory, stamp, prepared.channels, prepared.frames );
        }

        return prepared;
    }
}
