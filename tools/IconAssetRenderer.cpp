#include <juce_build_tools/juce_build_tools.h>

#include <algorithm>
#include <array>
#include <iostream>

namespace
{
constexpr int canvasSize = 1024;
using BrandPalette = std::array<juce::Colour, 3>;

const juce::Colour tileColour { 0xff101b2c };
const juce::Colour defaultAccentColour { 0xff4d6a87 };
const juce::Colour capsuleColour { 0xfff3f6fb };

int colourDistanceSquared (juce::Colour left, juce::Colour right)
{
    const auto red = static_cast<int> (left.getRed()) - static_cast<int> (right.getRed());
    const auto green = static_cast<int> (left.getGreen()) - static_cast<int> (right.getGreen());
    const auto blue = static_cast<int> (left.getBlue()) - static_cast<int> (right.getBlue());
    return red * red + green * green + blue * blue;
}

juce::Colour nearestPaletteColour (juce::Colour colour, const BrandPalette& palette)
{
    auto nearest = palette.front();
    auto nearestDistance = colourDistanceSquared (colour, nearest);

    for (const auto candidate : palette)
    {
        const auto distance = colourDistanceSquared (colour, candidate);
        if (distance < nearestDistance)
        {
            nearest = candidate;
            nearestDistance = distance;
        }
    }

    return nearest;
}

void quantiseToBrandPalette (juce::Image& image, const BrandPalette& palette)
{
    for (int y = 0; y < image.getHeight(); ++y)
    {
        for (int x = 0; x < image.getWidth(); ++x)
        {
            const auto source = image.getPixelAt (x, y);
            if (source.getAlpha() == 0)
            {
                image.setPixelAt (x, y, juce::Colours::transparentBlack);
                continue;
            }

            const auto nearest = nearestPaletteColour (source, palette);
            image.setPixelAt (x, y, nearest);
        }
    }
}

bool hasExactPaletteAndTransparency (const juce::Image& image, const BrandPalette& palette)
{
    std::array<bool, 3> paletteSeen {};
    auto transparentPixelSeen = false;

    for (int y = 0; y < image.getHeight(); ++y)
    {
        for (int x = 0; x < image.getWidth(); ++x)
        {
            const auto colour = image.getPixelAt (x, y);
            if (colour.getAlpha() == 0)
            {
                transparentPixelSeen = true;
                continue;
            }

            auto matched = false;
            for (size_t index = 0; index < palette.size(); ++index)
            {
                if (colour.withAlpha (1.0f) == palette[index])
                {
                    paletteSeen[index] = true;
                    matched = true;
                    break;
                }
            }

            if (! matched)
                return false;
        }
    }

    return transparentPixelSeen
        && std::all_of (paletteSeen.begin(), paletteSeen.end(), [] (bool seen) { return seen; });
}

bool parseRgbColour (const char* argument, juce::Colour& result)
{
    auto text = juce::String::fromUTF8 (argument).trim();
    if (text.startsWithChar ('#'))
        text = text.substring (1);

    if (text.length() != 6 || ! text.containsOnly ("0123456789abcdefABCDEF"))
        return false;

    result = juce::Colour { static_cast<juce::uint32> (0xff000000u
                                                       | static_cast<juce::uint32> (text.getHexValue32())) };
    return true;
}
}

int main (int argc, char* argv[])
{
    if (argc < 3 || argc > 5)
    {
        std::cerr << "Usage: IconAssetRenderer <source.svg> <output.png> [output.ico] [accent-rgb]\n";
        return 2;
    }

    auto accentColour = defaultAccentColour;
    if (argc == 5 && ! parseRgbColour (argv[4], accentColour))
    {
        std::cerr << "Accent colour must be a six-digit RGB value.\n";
        return 3;
    }

    const BrandPalette palette { tileColour, accentColour, capsuleColour };

    const juce::File sourceSvg { juce::String::fromUTF8 (argv[1]) };
    const juce::File outputPng { juce::String::fromUTF8 (argv[2]) };
    if (! sourceSvg.existsAsFile())
    {
        std::cerr << "SVG source was not found.\n";
        return 4;
    }

    auto drawable = juce::Drawable::createFromSVGFile (sourceSvg);
    if (drawable == nullptr)
    {
        std::cerr << "SVG source could not be parsed.\n";
        return 5;
    }

    static_cast<void> (drawable->replaceColour (defaultAccentColour, accentColour));

    juce::Image image { juce::Image::ARGB, canvasSize, canvasSize, true };
    {
        juce::Graphics graphics { image };
        drawable->draw (graphics, 1.0f);
    }

    quantiseToBrandPalette (image, palette);
    if (! hasExactPaletteAndTransparency (image, palette))
    {
        std::cerr << "Rendered icon did not retain the exact three-colour palette and transparency.\n";
        return 6;
    }

    outputPng.getParentDirectory().createDirectory();
    if (outputPng.existsAsFile() && ! outputPng.deleteFile())
    {
        std::cerr << "Existing PNG output could not be replaced.\n";
        return 7;
    }

    auto output = outputPng.createOutputStream();
    if (output == nullptr)
    {
        std::cerr << "PNG output could not be opened.\n";
        return 8;
    }

    juce::PNGImageFormat png;
    if (! png.writeImageToStream (image, *output))
    {
        std::cerr << "PNG output could not be written.\n";
        return 9;
    }

    output->flush();

    if (argc >= 4)
    {
        output.reset();
        const juce::File outputIco { juce::String::fromUTF8 (argv[3]) };
        const auto icons = juce::build_tools::Icons::fromFilesSmallAndBig (outputPng, outputPng);
        juce::build_tools::writeWinIcon (icons, outputIco);

        if (! outputIco.existsAsFile() || outputIco.getSize() == 0)
        {
            std::cerr << "Windows icon output could not be written.\n";
            return 10;
        }
    }

    return 0;
}
