#pragma once

#include <algorithm>

// Engine-independent so the boundary contract can be tested without an editor.
namespace AvenorRiverProfile
{
inline double SmoothUnit(double Value)
{
    const double T = std::max(0.0, std::min(1.0, Value));
    return T * T * (3.0 - 2.0 * T);
}

inline double BlendCarve(double TerrainHeight, double ChannelHeight,
    double BankAlpha, double MouthWeight)
{
    const double Weight = (1.0 - SmoothUnit(BankAlpha))
        * std::max(0.0, std::min(1.0, MouthWeight));
    // Fade the entire elevation difference, not just depth below water.
    // At the outside edge this must equal TerrainHeight, even on a hillside.
    return TerrainHeight - std::max(0.0, TerrainHeight - ChannelHeight) * Weight;
}
}
