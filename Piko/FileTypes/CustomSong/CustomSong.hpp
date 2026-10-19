#pragma once

#include "FileTypes/CHC/CHC.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace piko::filetypes::custom_song
{
    constexpr double editor_line_length_target_ms = 30000.0;
    constexpr double runtime_line_length_limit_ms = 60000.0;
    constexpr std::string_view audio_cue_name = "SONG";

    enum class GuardButton : std::uint32_t
    {
        Square = 0,
        Cross = 1,
        Circle = 2,
        Triangle = 3
    };

    struct ChartEntry
    {
        std::string name;
        std::filesystem::path file;
    };

    struct SongInfo
    {
        std::filesystem::path folder;
        std::string title;
        std::string artist;
        std::string charter;
        std::filesystem::path audio_path;
        double audio_offset_ms = 0.0;
        std::optional<double> preview_start_ms;
        std::int32_t asset_stage = 1;
        std::string ruleset = "free";
        std::vector<ChartEntry> charts;
    };

    struct TracePoint
    {
        double time_ms = 0.0;
        float angle_deg = 0.0f;
        bool curve = false;
    };

    struct Phrase
    {
        double time_ms = 0.0;
        double length_ms = 0.0;
    };

    struct Line
    {
        bool continues_previous = false;
        std::vector<TracePoint> points;
        std::vector<Phrase> phrases;
    };

    struct Guard
    {
        double time_ms = 0.0;
        GuardButton button = GuardButton::Square;
    };

    struct SectionMarker
    {
        double time_ms = 0.0;
        std::string name;
    };

    struct TempoPoint
    {
        double time_ms = 0.0;
        double bpm = 120.0;
    };

    struct Health
    {
        float start = 0.5f;
        float hit_gain = 0.012f;
        float hold_gain = 0.01f;
        float miss_damage = 0.05f;
        float guard_gain = 0.005f;
        float guard_miss_damage = 0.05f;
    };

    struct Chart
    {
        float scroll_speed = 1.0f;
        Health health{};
        std::optional<double> end_ms;
        std::vector<Line> lines;
        std::vector<Guard> guards;
        std::vector<SectionMarker> sections;
        std::vector<TempoPoint> tempo;
    };

    [[nodiscard]] SongInfo load_song_info( const std::filesystem::path &folder );
    [[nodiscard]] Chart load_chart_file( const std::filesystem::path &path );

    // Throws on errors, returns warnings.
    [[nodiscard]] std::vector<std::string> validate_chart( const Chart &chart );

    // Splits lines longer than max_length_ms at point boundaries outside phrases. Shared with the editor.
    std::size_t split_long_lines( Chart &chart, double max_length_ms, std::vector<std::string> &warnings );

    [[nodiscard]] chc::Song build_song( const Chart &chart,
        const SongInfo &info,
        std::optional<double> audio_length_ms = std::nullopt );

    [[nodiscard]] chc::Song load_song( const SongInfo &info,
        const ChartEntry &entry,
        std::vector<std::string> &warnings,
        std::optional<double> audio_length_ms = std::nullopt );
}
