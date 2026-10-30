#include "GraphViewport.h"

#include "GraphEditorPanel.h"

namespace
{
void notifyCommandStatus(GraphEditorPanel& panel)
{
    panel.getCommandManager().commandStatusChanged();
}
} // namespace

GraphViewport::GraphViewport(GraphEditorPanel& graphPanel)
    : panel(graphPanel)
{
    addAndMakeVisible(viewport);
    surface.addAndMakeVisible(panel);
    viewport.setViewedComponent(&surface, false);
    viewport.setScrollBarsShown(true, true);
    viewport.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::never);
    viewport.setSingleStepSizes(20, 20);
    panel.setGraphViewport(this);
}

GraphViewport::~GraphViewport()
{
    panel.setGraphViewport(nullptr);
    viewport.setViewedComponent(nullptr, false);
    surface.removeChildComponent(&panel);
    panel.setTransform({});
}

double GraphViewport::getZoom() const
{
    return zoom;
}

juce::Viewport& GraphViewport::getViewport()
{
    return viewport;
}

void GraphViewport::paint(juce::Graphics& graphics)
{
    graphics.fillAll(findColour(juce::ResizableWindow::backgroundColourId));
}

GraphViewport::Surface::Surface(GraphViewport& graphViewport)
    : owner(graphViewport)
{
}

void GraphViewport::Surface::paint(juce::Graphics& graphics)
{
    owner.paint(graphics);
}

void GraphViewport::Surface::mouseDown(const juce::MouseEvent& event)
{
    owner.panel.mouseDown(event.getEventRelativeTo(&owner.panel));
}

void GraphViewport::Surface::mouseDrag(const juce::MouseEvent& event)
{
    owner.panel.mouseDrag(event.getEventRelativeTo(&owner.panel));
}

void GraphViewport::Surface::mouseUp(const juce::MouseEvent& event)
{
    owner.panel.mouseUp(event.getEventRelativeTo(&owner.panel));
}

void GraphViewport::Surface::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    owner.wheelMove(event, wheel);
}

juce::Point<float> GraphViewport::visibleCentre() const
{
    return {(float)viewport.getMaximumVisibleWidth() * 0.5f, (float)viewport.getMaximumVisibleHeight() * 0.5f};
}

juce::Point<double> GraphViewport::getLogicalPosition(juce::Point<float> viewPosition) const
{
    return (viewPosition.toDouble() + viewport.getViewPosition().toDouble() - panelOffset) / zoom
         - panel.getLayoutOrigin().toDouble();
}

juce::Point<double> GraphViewport::getLogicalCentre() const
{
    return getLogicalPosition(visibleCentre());
}

juce::Point<int> GraphViewport::getVisiblePanelCentre() const
{
    return (getLogicalCentre() + panel.getLayoutOrigin().toDouble()).roundToInt();
}

void GraphViewport::layoutContent()
{
    juce::Point<int> size = GraphViewGeometry::scaledSize(panel.getLocalBounds(), zoom);
    juce::Point<int> available = GraphViewGeometry::availableArea(
        {viewport.getWidth(), viewport.getHeight()},
        size,
        viewport.getScrollBarThickness()
    );
    panelOffset =
        juce::Point<int>(juce::jmax(0, available.x - size.x) / 2, juce::jmax(0, available.y - size.y) / 2).toDouble();
    if (zoom == 1.0)
    {
        panel.setTransform({});
        panel.setTopLeftPosition(panelOffset.roundToInt());
    }
    else
    {
        panel.setTopLeftPosition(0, 0);
        panel.setTransform(
            juce::AffineTransform::scale((float)zoom).translated((float)panelOffset.x, (float)panelOffset.y)
        );
    }
    surface.setTopLeftPosition(0, 0);
    surface.setSize(juce::jmax(size.x, available.x), juce::jmax(size.y, available.y));
    previousVisibleSize = {viewport.getMaximumVisibleWidth(), viewport.getMaximumVisibleHeight()};
}

void GraphViewport::placeAnchor(juce::Point<double> logicalPosition, juce::Point<float> viewPosition)
{
    juce::Point<double> position =
        (logicalPosition + panel.getLayoutOrigin().toDouble()) * zoom + panelOffset - viewPosition.toDouble();
    viewport.setViewPosition(position.roundToInt());
}

void GraphViewport::resized()
{
    juce::Point<double> centre =
        previousVisibleSize.x > 0 && previousVisibleSize.y > 0
            ? getLogicalCentre()
            : juce::Point<double>(GraphViewGeometry::canvasWidth * 0.5, GraphViewGeometry::canvasHeight * 0.5);
    viewport.setBounds(getLocalBounds());
    layoutContent();
    placeAnchor(centre, visibleCentre());
}

void GraphViewport::contentChanged(juce::Point<double> previousCentre)
{
    layoutContent();
    placeAnchor(previousCentre, visibleCentre());
}

void GraphViewport::setZoomAt(double newZoom, juce::Point<float> viewPosition)
{
    juce::Point<double> anchor = getLogicalPosition(viewPosition);
    zoom = GraphViewGeometry::clampZoom(newZoom);
    layoutContent();
    placeAnchor(anchor, viewPosition);
    notifyCommandStatus(panel);
}

void GraphViewport::setZoom(double newZoom)
{
    juce::Point<double> centre = getLogicalCentre();
    zoom = GraphViewGeometry::clampZoom(newZoom);
    layoutContent();
    placeAnchor(centre, visibleCentre());
    notifyCommandStatus(panel);
}

void GraphViewport::resetView()
{
    zoom = 1.0;
    layoutContent();
    placeAnchor({GraphViewGeometry::canvasWidth * 0.5, GraphViewGeometry::canvasHeight * 0.5}, visibleCentre());
    notifyCommandStatus(panel);
}

void GraphViewport::zoomToFit()
{
    juce::Rectangle<int> bounds = panel.getGraphVisualBounds();

    if (bounds.isEmpty())
    {
        resetView();
        return;
    }

    bounds = bounds.expanded(GraphViewGeometry::padding);
    zoom = GraphViewGeometry::fitZoom(
        bounds,
        panel.getLocalBounds(),
        {viewport.getWidth(), viewport.getHeight()},
        viewport.getScrollBarThickness()
    );
    layoutContent();
    placeAnchor(bounds.toDouble().getCentre(), visibleCentre());
    notifyCommandStatus(panel);
}

void GraphViewport::wheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (event.mods.isCommandDown())
    {
        setZoomAt(GraphViewGeometry::wheelZoom(zoom, wheel.deltaY), event.getEventRelativeTo(&viewport).position);
        return;
    }

    viewport.mouseWheelMove(event.getEventRelativeTo(&viewport), wheel);
}

void GraphViewport::beginPan(const juce::MouseEvent& event)
{
    panStartViewPosition = viewport.getViewPosition();
    panStartScreenPosition = event.eventComponent->localPointToGlobal(event.position);
    panel.setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    surface.setMouseCursor(juce::MouseCursor::DraggingHandCursor);
}

void GraphViewport::dragPan(const juce::MouseEvent& event)
{
    viewport.setViewPosition(
        panStartViewPosition
        - (event.eventComponent->localPointToGlobal(event.position) - panStartScreenPosition).roundToInt()
    );
}

void GraphViewport::endPan()
{
    panel.setMouseCursor(juce::MouseCursor::NormalCursor);
    surface.setMouseCursor(juce::MouseCursor::NormalCursor);
}
