#pragma once

namespace mic_daw
{
// Message-thread scheduling only. Clean sessions never request a snapshot.
class DebouncedSave final
{
public:
    void changed(double nowMs) noexcept { deadlineMs = nowMs + 5000.0; }
    void retry(double nowMs) noexcept { deadlineMs = nowMs + 1000.0; }
    void saved() noexcept { deadlineMs = -1.0; }
    [[nodiscard]] bool pending() const noexcept { return deadlineMs >= 0.0; }
    [[nodiscard]] bool due(double nowMs) const noexcept
    {
        return pending() && nowMs >= deadlineMs;
    }

private:
    double deadlineMs = -1.0;
};
} // namespace mic_daw
