#include "UiText.h"

#include <iostream>

int main()
{
    const auto actual = mic_daw::uiText(u8"한국어", "English");

#if MIC_VST_BRIDGE_ENGLISH
    const auto expected = juce::String::fromUTF8("English");
#else
    const auto expected = juce::String(u8"한국어");
#endif

    if (actual != expected)
    {
        std::cerr << "UI language selection returned the wrong string\n";
        return 1;
    }

    return 0;
}
