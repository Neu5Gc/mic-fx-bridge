#include "StreamDiagnostics.h"

#include <iostream>
#include <stdexcept>

namespace
{
int checks = 0;

void require(bool condition, const char* message)
{
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace

int main()
{
    try
    {
        require(!mic_daw::shouldCountInputUnderrun(48000.0, 48000.0, 0, false),
                "Startup buffering must not count as an underrun");
        require(!mic_daw::shouldCountInputUnderrun(48000.0, 48000.0, 1, false),
                "One startup frame must remain intentional silence");
        require(mic_daw::shouldCountInputUnderrun(48000.0, 48000.0, 0, true),
                "A primed stream with no frames must count as an underrun");
        require(mic_daw::shouldCountInputUnderrun(48000.0, 48000.0, 1, true),
                "A primed stream with one frame cannot interpolate");
        require(!mic_daw::shouldCountInputUnderrun(48000.0, 48000.0, 2, true),
                "Two available frames are sufficient for interpolation");
        require(!mic_daw::shouldCountInputUnderrun(0.0, 48000.0, 0, true),
                "A closed input device must not count as a running underrun");
        require(!mic_daw::shouldCountInputUnderrun(48000.0, 0.0, 0, true),
                "A closed output device must not count as a running underrun");

        std::cout << "Stream diagnostics: " << checks << " checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Stream diagnostics failed: " << error.what() << '\n';
        return 1;
    }
}
