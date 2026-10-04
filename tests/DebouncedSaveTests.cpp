#include "DebouncedSave.h"

#include <iostream>

int main()
{
    mic_daw::DebouncedSave save;
    const auto check = [](bool value, const char* message)
    {
        if (!value)
            std::cerr << message << '\n';
        return value;
    };
    if (!check(!save.due(100000.0), "Idle startup must not save")) return 1;
    save.changed(100.0);
    if (!check(!save.due(5099.0) && save.due(5100.0), "Wait five quiet seconds")) return 1;
    save.changed(5000.0);
    if (!check(!save.due(5100.0) && save.due(10000.0), "Editing postpones the deadline")) return 1;
    save.retry(10000.0);
    if (!check(save.pending() && !save.due(10999.0) && save.due(11000.0), "Failure stays pending")) return 1;
    save.changed(10500.0);
    if (!check(!save.due(11000.0) && save.due(15500.0), "Changes supersede retries")) return 1;
    save.saved();
    if (!check(!save.pending() && !save.due(1000000.0), "Success stops autosave until another change")) return 1;
    save.changed(1000001.0);
    save.saved();
    if (!check(!save.due(1005001.0), "Manual save cancels the pending timer")) return 1;
    std::cout << "DebouncedSaveTests: 7 checks passed\n";
    return 0;
}
