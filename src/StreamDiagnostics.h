#pragma once

#include <cstddef>

namespace mic_daw
{
// Startup buffering is intentional silence. Running out of the two frames
// required for interpolation after priming is an actual input underrun.
[[nodiscard]] constexpr bool shouldCountInputUnderrun(double inputRate,
                                                      double outputRate,
                                                      std::size_t availableFrames,
                                                      bool inputPrimed) noexcept
{
    return inputPrimed && inputRate > 0.0 && outputRate > 0.0 && availableFrames < 2;
}
} // namespace mic_daw
