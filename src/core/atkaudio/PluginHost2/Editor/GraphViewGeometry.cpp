#include "GraphViewGeometry.h"

#include <cmath>

namespace GraphViewGeometry
{
double clampZoom(double zoom)
{
    return juce::jlimit(minimumZoom, maximumZoom, zoom);
}

juce::Point<double> normalisePosition(juce::Point<double> position)
{
    return position / juce::Point<double>(canvasWidth, canvasHeight);
}

juce::Point<double> canvasPosition(juce::Point<double> position)
{
    return position * juce::Point<double>(canvasWidth, canvasHeight);
}

juce::Point<int> availableArea(juce::Point<int> area, juce::Point<int> content, int scrollbarWidth)
{
    bool horizontal = false, vertical = false;

    for (int i = 0; i < 3; ++i)
    {
        horizontal = content.x > juce::jmax(0, area.x - (vertical ? scrollbarWidth : 0));
        vertical = content.y > juce::jmax(0, area.y - (horizontal ? scrollbarWidth : 0));
    }

    return {
        juce::jmax(0, area.x - (vertical ? scrollbarWidth : 0)),
        juce::jmax(0, area.y - (horizontal ? scrollbarWidth : 0))
    };
}

juce::Point<int> scaledSize(juce::Rectangle<int> bounds, double zoom)
{
    return {
        juce::jmax(1, (int)std::ceil(bounds.getWidth() * zoom)),
        juce::jmax(1, (int)std::ceil(bounds.getHeight() * zoom))
    };
}

double fitZoom(juce::Rectangle<int> content, juce::Rectangle<int> surface, juce::Point<int> area, int scrollbarWidth)
{
    auto fits = [&](double zoom)
    {
        juce::Point<int> available = availableArea(area, scaledSize(surface, zoom), scrollbarWidth);
        return content.getWidth() * zoom <= available.x && content.getHeight() * zoom <= available.y;
    };

    if (fits(maximumZoom))
        return maximumZoom;

    double lower = minimumZoom, upper = maximumZoom;

    for (int i = 0; i < 40; ++i)
    {
        double middle = (lower + upper) * 0.5;

        if (fits(middle))
            lower = middle;
        else
            upper = middle;
    }

    return lower;
}

double wheelZoom(double zoom, float delta)
{
    return clampZoom(zoom * std::pow(zoomStep, (double)delta * 4.0));
}
} // namespace GraphViewGeometry
