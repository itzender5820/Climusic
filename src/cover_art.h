#pragma once
#include <string>
#include <vector>

// Generate a placeholder ASCII art box (simulating album art)
// The real implementation would decode embedded JPEG/PNG using stb_image
// and convert to ASCII/braille, but for Termux compatibility we use
// a styled placeholder with the track info pattern.

static std::vector<std::string> generate_cover_art(
    const std::string& title,
    const std::string& artist,
    int width = 28,
    int height = 18)
{
    std::vector<std::string> lines;
    // Top border
    lines.push_back("." + std::string(width - 2, ':') + ".");

    auto gen_line = [&](int row) -> std::string {
        std::string s(width - 2, ' ');
        // Create a pattern that suggests a waveform/album art
        for (int c = 0; c < width - 2; ++c) {
            int pat = (row * 7 + c * 3) % 11;
            if (pat < 3) s[c] = '.';
            else if (pat < 5) s[c] = ',';
            else if (pat < 6) s[c] = '\'';
            else if (pat < 7) s[c] = '`';
            else if (pat < 8) s[c] = ':';
        }
        return "|" + s + "|";
    };

    for (int r = 1; r < height - 1; ++r)
        lines.push_back(gen_line(r));

    // Bottom border
    lines.push_back("'" + std::string(width - 2, '.') + "'");
    return lines;
}
