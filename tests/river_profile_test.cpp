#include "../AvenorClient/Source/AvenorEditor/AvenorRiverProfile.h"
#include <cassert>
#include <cmath>
#include <iostream>

int main()
{
    using AvenorRiverProfile::BlendCarve;
    // Small stream and deep trunk river, including water far below hillside.
    for (double Terrain : {100.0, 1000.0, 20000.0})
    for (double Depth : {25.0, 150.0, 1000.0})
    {
        const double Target = 100.0 - Depth;
        assert(BlendCarve(Terrain, Target, 0.0, 1.0) == Target);
        assert(BlendCarve(Terrain, Target, 1.0, 1.0) == Terrain);
        assert(BlendCarve(Terrain, Target, 0.0, 0.0) == Terrain);
        double Previous = Target;
        for (int I = 0; I <= 1000; ++I)
        {
            const double H = BlendCarve(Terrain, Target, I / 1000.0, 1.0);
            assert(H >= Previous - 1e-9 && H <= Terrain);
            Previous = H;
        }
        // Zero-slope return at bank boundary: no water-datum cliff.
        assert(std::abs(BlendCarve(Terrain, Target, 1.0 - 1e-6, 1.0)
            - Terrain) < 1e-6);
    }
    // Never fill a pre-existing deeper lake/channel.
    assert(BlendCarve(-200.0, -100.0, 0.0, 1.0) == -200.0);
    // Overlapping lower-only profiles remain independent of evaluation order.
    const double A = BlendCarve(1000.0, -100.0, 0.3, 1.0);
    const double B = BlendCarve(1000.0, -200.0, 0.6, 1.0);
    assert(std::min(A, B) == std::min(B, A));
    std::cout << "River profile continuity tests passed\n";
}
