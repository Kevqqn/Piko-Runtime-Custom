#include "CustomSong.hpp"

#include "Utils/FileUtil.hpp"
#include "Utils/Json.hpp"
#include "Utils/StrUtil.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace piko::filetypes::custom_song::parse
{
    using utils::json::Value;

    constexpr double samples_per_ms = 48.0;
    constexpr double scene_frames_per_sample = 30.0 / 48000.0;
    constexpr float base_speed_factor = 240.0f;
    constexpr double guard_chart_gap_ms = 10000.0;
    constexpr double fallback_tail_ms = 2000.0;
    constexpr std::uint32_t continuation_stub_samples = 64;
    constexpr double time_epsilon_ms = 0.001;
    constexpr float angle_epsilon_deg = 0.001f;

    constexpr std::array<std::string_view, 5> audio_extensions{ ".wav", ".flac", ".opus", ".ogg", ".mp3" };

    std::runtime_error error( std::string_view context, std::string_view message )
    {
        return std::runtime_error( std::string( context ) + ": " + std::string( message ) );
    }

    std::string label( std::string_view kind, std::size_t index )
    {
        return std::string( kind ) + " " + std::to_string( index );
    }

    double number_field( const Value &value, std::string_view field, double fallback )
    {
        const Value *entry = value.find( field );
        return entry != nullptr ? entry->number() : fallback;
    }

    std::string string_field( const Value &value, std::string_view field )
    {
        const Value *entry = value.find( field );
        return entry != nullptr ? entry->string() : std::string{};
    }

    const Value::Array *optional_array( const Value &value, std::string_view field )
    {
        const Value *entry = value.find( field );
        return entry != nullptr ? &entry->array() : nullptr;
    }

    void require_format( const Value &root, std::string_view format )
    {
        if( root.require( "format" ).string() != format )
        {
            throw std::runtime_error( "unsupported format, expected " + std::string( format ) );
        }

        if( root.require( "version" ).integer_or( 0 ) != 1 )
        {
            throw std::runtime_error( "unsupported version" );
        }
    }

    std::filesystem::path resolve_inside( const std::filesystem::path &folder, const std::string &relative )
    {
        const std::filesystem::path path = ( folder / utils::path_from_utf8( relative ) ).lexically_normal();
        const std::filesystem::path inside = path.lexically_relative( folder.lexically_normal() );

        if( relative.empty() || inside.empty() || *inside.begin() == ".." )
        {
            throw std::runtime_error( "path leaves the song folder: " + relative );
        }

        return path;
    }

    GuardButton guard_button( const std::string &name )
    {
        const std::string lower = utils::lowercase_ascii( name );

        if( lower == "square" )
        {
            return GuardButton::Square;
        }

        if( lower == "cross" )
        {
            return GuardButton::Cross;
        }

        if( lower == "circle" )
        {
            return GuardButton::Circle;
        }

        if( lower == "triangle" )
        {
            return GuardButton::Triangle;
        }

        throw std::runtime_error( "unknown guard button: " + name );
    }

    bool finite_non_negative( double value )
    {
        return std::isfinite( value ) && value >= 0.0;
    }

    double line_start( const Line &line )
    {
        return line.points.front().time_ms;
    }

    double line_end( const Line &line )
    {
        return line.points.back().time_ms;
    }

    bool inside_phrase( const Line &line, double time_ms )
    {
        for( const Phrase &phrase : line.phrases )
        {
            if( time_ms > phrase.time_ms + time_epsilon_ms &&
                time_ms < phrase.time_ms + phrase.length_ms - time_epsilon_ms )
            {
                return true;
            }
        }

        return false;
    }

    // Prefers a late point after a straight segment so the cut does not change the line shape.
    std::optional<std::size_t> find_split_point( const Line &line, double max_length_ms )
    {
        const double start = line_start( line );
        std::optional<std::size_t> latest_straight;
        std::optional<std::size_t> latest_legal;
        std::optional<std::size_t> earliest_beyond;

        for( std::size_t index = 1; index + 1 < line.points.size(); ++index )
        {
            const TracePoint &point = line.points[ index ];

            if( inside_phrase( line, point.time_ms ) )
            {
                continue;
            }

            const double offset = point.time_ms - start;

            if( offset > max_length_ms )
            {
                if( !earliest_beyond )
                {
                    earliest_beyond = index;
                }

                continue;
            }

            latest_legal = index;

            if( offset >= max_length_ms * 0.5 &&
                std::abs( line.points[ index - 1 ].angle_deg - point.angle_deg ) <= angle_epsilon_deg )
            {
                latest_straight = index;
            }
        }

        if( latest_straight )
        {
            return latest_straight;
        }

        return latest_legal ? latest_legal : earliest_beyond;
    }

    float stored_angle( float angle_deg )
    {
        constexpr float pi = std::numbers::pi_v<float>;
        return angle_deg * pi / 180.0f - pi;
    }

    std::uint32_t to_u32( std::int64_t value, std::string_view what )
    {
        if( value < 0 || value > static_cast<std::int64_t>( std::numeric_limits<std::uint32_t>::max() ) )
        {
            throw std::runtime_error( std::string( what ) + " is out of range" );
        }

        return static_cast<std::uint32_t>( value );
    }

    std::int32_t to_i32( std::int64_t value, std::string_view what )
    {
        if( value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max() )
        {
            throw std::runtime_error( std::string( what ) + " is out of range" );
        }

        return static_cast<std::int32_t>( value );
    }

    chc::EnergyDamageFactors energy_factors( const Health &health )
    {
        chc::EnergyDamageFactors factors{};
        factors.starting_energy = health.start;

        factors.attack_damage_press = health.hit_gain;

        factors.attack_damage_release = health.hold_gain;

        factors.attack_miss_damage = health.miss_damage;

        factors.guard_gain = health.guard_gain;

        factors.guard_miss_damage = health.guard_miss_damage;
        return factors;
    }

    TracePoint trace_point( const Value &value )
    {
        TracePoint point{};
        point.time_ms = value.require( "time_ms" ).number();

        point.angle_deg = static_cast<float>( value.require( "angle_deg" ).number() );

        if( const Value *curve = value.find( "curve" ) )
        {
            point.curve = curve->boolean_or( false );
        }

        return point;
    }

    Line line( const Value &value )
    {
        Line result{};

        if( const Value *continues = value.find( "continues_previous" ) )
        {
            result.continues_previous = continues->boolean_or( false );
        }

        for( const Value &entry : value.require( "points" ).array() )
        {
            result.points.push_back( trace_point( entry ) );
        }

        if( const Value::Array *phrases = optional_array( value, "phrases" ) )
        {
            for( const Value &entry : *phrases )
            {
                result.phrases.push_back(
                    Phrase{ entry.require( "time_ms" ).number(), entry.require( "length_ms" ).number() } );
            }
        }

        std::stable_sort( result.phrases.begin(),
            result.phrases.end(),
            []( const Phrase &left, const Phrase &right )
            {
                return left.time_ms < right.time_ms;
            } );

        return result;
    }
}

namespace piko::filetypes::custom_song
{
    SongInfo load_song_info( const std::filesystem::path &folder )
    {
        const std::filesystem::path path = folder / "song.json";
        const std::string context = utils::path_to_utf8( path );

        try
        {
            const utils::json::Value root = utils::json::load_file( path );
            parse::require_format( root, "piko-custom-song" );

            SongInfo info{};
            info.folder = folder.lexically_normal();

            info.title = root.require( "title" ).string();

            info.artist = parse::string_field( root, "artist" );

            info.charter = parse::string_field( root, "charter" );

            if( info.title.empty() )
            {
                throw std::runtime_error( "title is empty" );
            }

            info.audio_path = parse::resolve_inside( info.folder, root.require( "audio" ).string() );

            const std::string extension = utils::lowercase_ascii( info.audio_path.extension().string() );

            if( std::find( parse::audio_extensions.begin(), parse::audio_extensions.end(), extension ) ==
                parse::audio_extensions.end() )
            {
                throw std::runtime_error( "unsupported audio type: " + extension );
            }

            if( !utils::regular_file( info.audio_path ) )
            {
                throw std::runtime_error( "audio file is missing: " + utils::path_to_utf8( info.audio_path ) );
            }

            info.audio_offset_ms = parse::number_field( root, "audio_offset_ms", 0.0 );

            if( !std::isfinite( info.audio_offset_ms ) )
            {
                throw std::runtime_error( "audio_offset_ms is not a number" );
            }

            if( const utils::json::Value *preview = root.find( "preview_start_ms" ) )
            {
                info.preview_start_ms = preview->number();
            }

            if( const utils::json::Value *asset_stage = root.find( "asset_stage" ) )
            {
                info.asset_stage = static_cast<std::int32_t>( asset_stage->integer_or( 1 ) );
            }

            if( info.asset_stage < 1 || info.asset_stage > 10 )
            {
                throw std::runtime_error( "asset_stage must be between 1 and 10" );
            }

            if( const std::string ruleset = parse::string_field( root, "ruleset" ); !ruleset.empty() )
            {
                info.ruleset = ruleset;
            }

            if( info.ruleset != "free" )
            {
                throw std::runtime_error( "unsupported ruleset: " + info.ruleset );
            }

            for( const utils::json::Value &entry : root.require( "charts" ).array() )
            {
                ChartEntry chart{};
                chart.file = parse::resolve_inside( info.folder, entry.require( "file" ).string() );

                chart.name = parse::string_field( entry, "name" );

                if( chart.name.empty() )
                {
                    chart.name = utils::path_to_utf8( chart.file.stem() );
                }

                if( !utils::regular_file( chart.file ) )
                {
                    throw std::runtime_error( "chart file is missing: " + utils::path_to_utf8( chart.file ) );
                }

                info.charts.push_back( std::move( chart ) );
            }

            if( info.charts.empty() )
            {
                throw std::runtime_error( "song has no charts" );
            }

            return info;
        }
        catch( const std::exception &e )
        {
            throw parse::error( context, e.what() );
        }
    }

    Chart load_chart_file( const std::filesystem::path &path )
    {
        const std::string context = utils::path_to_utf8( path );

        try
        {
            const utils::json::Value root = utils::json::load_file( path );
            parse::require_format( root, "piko-custom-chart" );

            Chart chart{};
            chart.scroll_speed = static_cast<float>( parse::number_field( root, "scroll_speed", 1.0 ) );

            if( const utils::json::Value *health = root.find( "health" ) )
            {
                const Health defaults{};
                chart.health.start = static_cast<float>( parse::number_field( *health, "start", defaults.start ) );

                chart.health.hit_gain =
                    static_cast<float>( parse::number_field( *health, "hit_gain", defaults.hit_gain ) );

                chart.health.hold_gain =
                    static_cast<float>( parse::number_field( *health, "hold_gain", defaults.hold_gain ) );

                chart.health.miss_damage =
                    static_cast<float>( parse::number_field( *health, "miss_damage", defaults.miss_damage ) );

                chart.health.guard_gain =
                    static_cast<float>( parse::number_field( *health, "guard_gain", defaults.guard_gain ) );

                chart.health.guard_miss_damage = static_cast<float>(
                    parse::number_field( *health, "guard_miss_damage", defaults.guard_miss_damage ) );
            }

            if( const utils::json::Value *end = root.find( "end_ms" ) )
            {
                chart.end_ms = end->number();
            }

            for( const utils::json::Value &entry : root.require( "lines" ).array() )
            {
                chart.lines.push_back( parse::line( entry ) );
            }

            if( const utils::json::Value::Array *guards = parse::optional_array( root, "guards" ) )
            {
                for( const utils::json::Value &entry : *guards )
                {
                    chart.guards.push_back( Guard{ entry.require( "time_ms" ).number(),
                        parse::guard_button( entry.require( "button" ).string() ) } );
                }
            }

            if( const utils::json::Value::Array *sections = parse::optional_array( root, "sections" ) )
            {
                for( const utils::json::Value &entry : *sections )
                {
                    chart.sections.push_back(
                        SectionMarker{ entry.require( "time_ms" ).number(), parse::string_field( entry, "name" ) } );
                }
            }

            if( const utils::json::Value::Array *tempo = parse::optional_array( root, "tempo" ) )
            {
                for( const utils::json::Value &entry : *tempo )
                {
                    chart.tempo.push_back(
                        TempoPoint{ entry.require( "time_ms" ).number(), entry.require( "bpm" ).number() } );
                }
            }

            std::stable_sort( chart.lines.begin(),
                chart.lines.end(),
                []( const Line &left, const Line &right )
                {
                    const double left_start = left.points.empty() ? 0.0 : left.points.front().time_ms;
                    const double right_start = right.points.empty() ? 0.0 : right.points.front().time_ms;
                    return left_start < right_start;
                } );

            const auto by_time = []( const auto &left, const auto &right )
            {
                return left.time_ms < right.time_ms;
            };

            std::stable_sort( chart.guards.begin(), chart.guards.end(), by_time );
            std::stable_sort( chart.sections.begin(), chart.sections.end(), by_time );
            std::stable_sort( chart.tempo.begin(), chart.tempo.end(), by_time );
            return chart;
        }
        catch( const std::exception &e )
        {
            throw parse::error( context, e.what() );
        }
    }

    std::vector<std::string> validate_chart( const Chart &chart )
    {
        std::vector<std::string> warnings;

        if( !std::isfinite( chart.scroll_speed ) || chart.scroll_speed <= 0.0f )
        {
            throw std::runtime_error( "scroll_speed must be greater than 0" );
        }

        const Health &health = chart.health;

        for( const float value : { health.hit_gain,
                 health.hold_gain,
                 health.miss_damage,
                 health.guard_gain,
                 health.guard_miss_damage } )
        {
            if( !std::isfinite( value ) || value < 0.0f )
            {
                throw std::runtime_error( "health values must be 0 or greater" );
            }
        }

        if( !std::isfinite( health.start ) || health.start <= 0.0f || health.start > 1.0f )
        {
            throw std::runtime_error( "health start must be greater than 0 and at most 1" );
        }

        if( chart.end_ms && ( !std::isfinite( *chart.end_ms ) || *chart.end_ms <= 0.0 ) )
        {
            throw std::runtime_error( "end_ms must be greater than 0" );
        }

        if( chart.lines.empty() && chart.guards.empty() )
        {
            throw std::runtime_error( "chart has no lines or guards" );
        }

        for( std::size_t tempo_index = 0; tempo_index < chart.tempo.size(); ++tempo_index )
        {
            const TempoPoint &tempo = chart.tempo[ tempo_index ];

            if( !parse::finite_non_negative( tempo.time_ms ) || !std::isfinite( tempo.bpm ) || tempo.bpm <= 0.0 )
            {
                throw std::runtime_error( parse::label( "tempo", tempo_index ) + ": invalid time or bpm" );
            }
        }

        for( std::size_t line_index = 0; line_index < chart.lines.size(); ++line_index )
        {
            const Line &line = chart.lines[ line_index ];
            const std::string name = parse::label( "line", line_index );

            if( line.points.size() < 2 )
            {
                throw std::runtime_error( name + ": needs at least 2 points" );
            }

            for( std::size_t point_index = 0; point_index < line.points.size(); ++point_index )
            {
                const TracePoint &point = line.points[ point_index ];

                if( !parse::finite_non_negative( point.time_ms ) || !std::isfinite( point.angle_deg ) )
                {
                    throw std::runtime_error( name + ", " + parse::label( "point", point_index ) +
                                              ": invalid time or angle" );
                }

                if( point_index > 0 && point.time_ms <= line.points[ point_index - 1 ].time_ms )
                {
                    throw std::runtime_error( name + ", " + parse::label( "point", point_index ) +
                                              ": times must increase" );
                }
            }

            double previous_phrase_end = -1.0;

            for( std::size_t phrase_index = 0; phrase_index < line.phrases.size(); ++phrase_index )
            {
                const Phrase &phrase = line.phrases[ phrase_index ];
                const std::string phrase_name = name + ", " + parse::label( "phrase", phrase_index );

                if( !parse::finite_non_negative( phrase.time_ms ) || !std::isfinite( phrase.length_ms ) ||
                    phrase.length_ms <= 0.0 )
                {
                    throw std::runtime_error( phrase_name + ": invalid time or length" );
                }

                if( phrase.time_ms < parse::line_start( line ) - parse::time_epsilon_ms ||
                    phrase.time_ms + phrase.length_ms > parse::line_end( line ) + parse::time_epsilon_ms )
                {
                    throw std::runtime_error( phrase_name + ": lies outside its line" );
                }

                if( phrase.time_ms < previous_phrase_end - parse::time_epsilon_ms )
                {
                    throw std::runtime_error( phrase_name + ": overlaps the previous phrase" );
                }

                previous_phrase_end = phrase.time_ms + phrase.length_ms;
            }

            if( line_index == 0 )
            {
                if( line.continues_previous )
                {
                    throw std::runtime_error( name + ": continues_previous has no previous line" );
                }

                continue;
            }

            const Line &previous = chart.lines[ line_index - 1 ];

            if( parse::line_start( line ) < parse::line_end( previous ) - parse::time_epsilon_ms )
            {
                throw std::runtime_error( name + ": overlaps the previous line" );
            }

            if( line.continues_previous &&
                ( std::abs( parse::line_start( line ) - parse::line_end( previous ) ) > parse::time_epsilon_ms ||
                    std::abs( line.points.front().angle_deg - previous.points.back().angle_deg ) >
                        parse::angle_epsilon_deg ) )
            {
                throw std::runtime_error( name + ": continues_previous must start where the previous line ends" );
            }
        }

        for( std::size_t guard_index = 0; guard_index < chart.guards.size(); ++guard_index )
        {
            const Guard &guard = chart.guards[ guard_index ];

            if( !parse::finite_non_negative( guard.time_ms ) )
            {
                throw std::runtime_error( parse::label( "guard", guard_index ) + ": invalid time" );
            }

            for( const Line &line : chart.lines )
            {
                if( parse::inside_phrase( line, guard.time_ms ) )
                {
                    warnings.push_back( parse::label( "guard", guard_index ) + ": lies inside a phrase" );
                    break;
                }
            }
        }

        return warnings;
    }

    std::size_t split_long_lines( Chart &chart, double max_length_ms, std::vector<std::string> &warnings )
    {
        std::size_t splits = 0;
        std::vector<Line> result;
        result.reserve( chart.lines.size() );

        for( Line &source : chart.lines )
        {
            Line current = std::move( source );

            while( current.points.size() >= 2 &&
                   parse::line_end( current ) - parse::line_start( current ) > max_length_ms )
            {
                const std::optional<std::size_t> split = parse::find_split_point( current, max_length_ms );

                if( !split )
                {
                    warnings.push_back( "line at " + std::to_string( parse::line_start( current ) ) +
                                        " ms is too long and has no point outside a phrase to split at" );
                    break;
                }

                const double split_ms = current.points[ *split ].time_ms;
                const auto split_point = current.points.begin() + static_cast<std::ptrdiff_t>( *split );

                Line head{};
                head.continues_previous = current.continues_previous;

                head.points.assign( current.points.begin(), split_point + 1 );

                Line tail{};
                tail.continues_previous = true;

                tail.points.assign( split_point, current.points.end() );

                for( const Phrase &phrase : current.phrases )
                {
                    ( phrase.time_ms < split_ms ? head : tail ).phrases.push_back( phrase );
                }

                result.push_back( std::move( head ) );
                current = std::move( tail );
                ++splits;
            }

            result.push_back( std::move( current ) );
        }

        chart.lines = std::move( result );
        return splits;
    }

    chc::Song build_song( const Chart &chart, const SongInfo &info, std::optional<double> audio_length_ms )
    {
        const auto sample_at = [ & ]( double chart_ms ) -> std::int64_t
        {
            const std::int64_t sample = std::llround( ( chart_ms + info.audio_offset_ms ) * parse::samples_per_ms );

            if( sample < 0 )
            {
                throw std::runtime_error( "a note lands before the audio starts after audio_offset_ms" );
            }

            return sample;
        };

        double last_event_ms = 0.0;

        for( const Line &line : chart.lines )
        {
            last_event_ms = std::max( last_event_ms, parse::line_end( line ) );
        }

        for( const Guard &guard : chart.guards )
        {
            last_event_ms = std::max( last_event_ms, guard.time_ms );
        }

        const std::int64_t last_event_sample = sample_at( last_event_ms );
        std::int64_t end_sample = sample_at( last_event_ms + parse::fallback_tail_ms );

        if( chart.end_ms )
        {
            end_sample = sample_at( *chart.end_ms );
        }
        else if( audio_length_ms )
        {
            end_sample = std::llround( *audio_length_ms * parse::samples_per_ms );
        }

        if( end_sample < last_event_sample )
        {
            throw std::runtime_error( "notes extend past the end of the song" );
        }

        std::vector<chc::Chart> charts;
        charts.reserve( chart.lines.size() + 1 );

        for( std::size_t line_index = 0; line_index < chart.lines.size(); ++line_index )
        {
            const Line &line = chart.lines[ line_index ];
            const std::int64_t start = sample_at( parse::line_start( line ) );
            const bool continued =
                line_index + 1 < chart.lines.size() && chart.lines[ line_index + 1 ].continues_previous;

            chc::Chart out{};
            out.start_sample = parse::to_u32( start, "line start" );

            out.end_sample = parse::to_u32( sample_at( parse::line_end( line ) ), "line end" );

            out.continues_previous = line.continues_previous;

            for( std::size_t index = 0; index < line.points.size(); ++index )
            {
                const TracePoint &point = line.points[ index ];
                const std::int64_t point_sample = sample_at( point.time_ms );
                const std::int64_t next_sample =
                    index + 1 < line.points.size() ? sample_at( line.points[ index + 1 ].time_ms ) : point_sample;

                chc::TraceFragment fragment{};
                fragment.start_sample = parse::to_i32( point_sample - start, "trace point" );

                fragment.duration_samples = parse::to_u32( next_sample - point_sample, "trace segment" );

                fragment.stored_angle = parse::stored_angle( point.angle_deg );

                fragment.curve = point.curve ? 1u : 0u;
                out.traces.push_back( fragment );
            }

            // The last drawn segment of a chart never turns. A short stub past the cut keeps the real last segment
            // turning toward the next point, so a split line keeps its shape.
            if( continued )
            {
                chc::TraceFragment &cut = out.traces.back();
                cut.duration_samples = parse::continuation_stub_samples;

                chc::TraceFragment terminator = cut;
                terminator.start_sample += static_cast<std::int32_t>( parse::continuation_stub_samples );

                terminator.duration_samples = 0;
                out.traces.push_back( terminator );
            }

            for( const Phrase &phrase : line.phrases )
            {
                chc::PhraseFragment fragment{};
                fragment.start_sample = parse::to_i32( sample_at( phrase.time_ms ) - start, "phrase" );

                fragment.duration_samples = parse::to_u32(
                    std::max<std::int64_t>( 1, std::llround( phrase.length_ms * parse::samples_per_ms ) ),
                    "phrase length" );

                fragment.phrase_start = 1;

                fragment.phrase_end = 1;
                out.phrases.push_back( fragment );
            }

            charts.push_back( std::move( out ) );
        }

        const std::size_t line_chart_count = charts.size();
        std::optional<std::size_t> guard_chart;
        std::int64_t guard_chart_last_sample = 0;

        for( const Guard &guard : chart.guards )
        {
            const std::int64_t sample = sample_at( guard.time_ms );

            chc::GuardFragment fragment{};
            fragment.button = static_cast<std::uint32_t>( guard.button );

            const auto covering = std::find_if( charts.begin(),
                charts.begin() + static_cast<std::ptrdiff_t>( line_chart_count ),
                [ & ]( const chc::Chart &candidate )
                {
                    return sample >= candidate.start_sample && sample <= candidate.end_sample;
                } );

            if( covering != charts.begin() + static_cast<std::ptrdiff_t>( line_chart_count ) )
            {
                fragment.start_sample = parse::to_i32( sample - covering->start_sample, "guard" );
                covering->guards.push_back( fragment );
                guard_chart.reset();
                continue;
            }

            // Like retail, guards before a line belong to that line's chart with a negative offset. A separate guard
            // chart spanning a line would fade the player stem while its phrases are held.
            const auto next_line = std::find_if( charts.begin(),
                charts.begin() + static_cast<std::ptrdiff_t>( line_chart_count ),
                [ & ]( const chc::Chart &candidate )
                {
                    return candidate.start_sample > sample;
                } );

            if( next_line != charts.begin() + static_cast<std::ptrdiff_t>( line_chart_count ) )
            {
                fragment.start_sample = parse::to_i32( sample - next_line->start_sample, "guard" );
                next_line->guards.push_back( fragment );
                guard_chart.reset();
                continue;
            }

            const bool start_new_chart =
                !guard_chart ||
                sample - guard_chart_last_sample > std::llround( parse::guard_chart_gap_ms * parse::samples_per_ms );

            if( start_new_chart )
            {
                chc::Chart out{};
                out.start_sample = parse::to_u32( sample, "guard" );

                out.end_sample = out.start_sample;
                charts.push_back( std::move( out ) );
                guard_chart = charts.size() - 1;
            }

            chc::Chart &target = charts[ *guard_chart ];
            target.end_sample = parse::to_u32( sample, "guard" );

            fragment.start_sample = parse::to_i32( sample - target.start_sample, "guard" );
            target.guards.push_back( fragment );
            guard_chart_last_sample = sample;
        }

        std::stable_sort( charts.begin(),
            charts.end(),
            []( const chc::Chart &left, const chc::Chart &right )
            {
                return left.start_sample < right.start_sample;
            } );

        chc::SongSection section{};
        section.battle_phase = static_cast<std::uint32_t>( chc::BattlePhase::Battle );

        section.tempo = chart.tempo.empty() ? 120.0f : static_cast<float>( chart.tempo.front().bpm );

        section.duration_samples = parse::to_u32( end_sample, "song length" );

        section.player_count = 1;

        section.charts_per_player = parse::to_u32( static_cast<std::int64_t>( charts.size() ), "chart count" );

        for( std::size_t index = 1; index < chart.tempo.size(); ++index )
        {
            chc::TempoChange change{};
            change.start_sample = parse::to_u32(
                std::max<std::int64_t>( 0,
                    std::llround( ( chart.tempo[ index ].time_ms + info.audio_offset_ms ) * parse::samples_per_ms ) ),
                "tempo change" );

            change.tempo = static_cast<float>( chart.tempo[ index ].bpm );
            section.tempo_changes.push_back( change );
        }

        section.charts = std::move( charts );

        chc::Cue cue{};
        cue.fallback_name = std::string( audio_cue_name );

        cue.audio_section_name = std::string( audio_cue_name );

        cue.last_frame = static_cast<float>( static_cast<double>( end_sample ) * parse::scene_frames_per_sample );

        cue.timeline_advance_samples = section.duration_samples;

        chc::Song song{};
        song.imc_path = utils::path_to_utf8( info.audio_path.filename() );

        song.gameplay_speed_factor = parse::base_speed_factor / chart.scroll_speed;

        song.cues.push_back( std::move( cue ) );
        song.sections.push_back( std::move( section ) );

        const chc::EnergyDamageFactors factors = parse::energy_factors( chart.health );
        chc::PlayerEnergyDamageFactors &player = song.energy_damage.players[ 0 ];
        player.intro = factors;

        player.charge = factors;

        player.battle = factors;

        player.harmony = factors;

        player.end = factors;
        return song;
    }

    chc::Song load_song( const SongInfo &info,
        const ChartEntry &entry,
        std::vector<std::string> &warnings,
        std::optional<double> audio_length_ms )
    {
        Chart chart = load_chart_file( entry.file );
        const std::string context = utils::path_to_utf8( entry.file );

        try
        {
            for( std::string &warning : validate_chart( chart ) )
            {
                warnings.push_back( context + ": " + std::move( warning ) );
            }

            std::vector<std::string> split_warnings;
            split_long_lines( chart, runtime_line_length_limit_ms, split_warnings );

            for( std::string &warning : split_warnings )
            {
                warnings.push_back( context + ": " + std::move( warning ) );
            }

            return build_song( chart, info, audio_length_ms );
        }
        catch( const std::exception &e )
        {
            throw parse::error( context, e.what() );
        }
    }
}
