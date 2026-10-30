#pragma once

#include <juce_graphics/juce_graphics.h>

namespace GraphViewGeometry
{
constexpr int canvasWidth = 800;
constexpr int canvasHeight = 600;
constexpr int padding = 24;
constexpr double minimumZoom = 0.25;
constexpr double maximumZoom = 4.0;
constexpr double zoomStep = 1.25;

double clampZoom(double zoom);
juce::Point<double> normalisePosition(juce::Point<double> position);
juce::Point<double> canvasPosition(juce::Point<double> position);
juce::Point<int> availableArea(juce::Point<int> area, juce::Point<int> content, int scrollbarWidth);
juce::Point<int> scaledSize(juce::Rectangle<int> bounds, double zoom);
double fitZoom(juce::Rectangle<int> content, juce::Rectangle<int> surface, juce::Point<int> area, int scrollbarWidth);
double wheelZoom(double zoom, float delta);
} // namespace GraphViewGeometry
