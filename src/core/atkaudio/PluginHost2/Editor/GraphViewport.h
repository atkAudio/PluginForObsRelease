#pragma once

#include "GraphViewGeometry.h"

#include <juce_gui_basics/juce_gui_basics.h>

class GraphEditorPanel;

class GraphViewport final : public juce::Component
{
public:
    explicit GraphViewport(GraphEditorPanel&);
    ~GraphViewport() override;

    void resized() override;
    void paint(juce::Graphics&) override;

    double getZoom() const;
    void setZoom(double);
    void resetView();
    void zoomToFit();
    void contentChanged(juce::Point<double> previousCentre);
    juce::Point<double> getLogicalCentre() const;
    juce::Point<double> getLogicalPosition(juce::Point<float> viewPosition) const;
    juce::Point<int> getVisiblePanelCentre() const;
    juce::Viewport& getViewport();

    void wheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&);
    void beginPan(const juce::MouseEvent&);
    void dragPan(const juce::MouseEvent&);
    void endPan();

private:
    struct Surface final : public juce::Component
    {
        explicit Surface(GraphViewport&);
        void paint(juce::Graphics&) override;
        void mouseDown(const juce::MouseEvent&) override;
        void mouseDrag(const juce::MouseEvent&) override;
        void mouseUp(const juce::MouseEvent&) override;
        void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        GraphViewport& owner;
    };

    GraphEditorPanel& panel;
    juce::Viewport viewport;
    Surface surface{*this};
    double zoom = 1.0;
    juce::Point<double> panelOffset;
    juce::Point<int> previousVisibleSize;
    juce::Point<int> panStartViewPosition;
    juce::Point<float> panStartScreenPosition;

    void layoutContent();
    void placeAnchor(juce::Point<double> logicalPosition, juce::Point<float> viewPosition);
    void setZoomAt(double, juce::Point<float> viewPosition);
    juce::Point<float> visibleCentre() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GraphViewport)
};
