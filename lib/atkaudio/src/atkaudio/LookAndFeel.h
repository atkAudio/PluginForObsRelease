#pragma once
#include <juce_audio_utils/juce_audio_utils.h>

using namespace juce;

namespace atk
{
class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel()
    {
        juce::LookAndFeel::setDefaultLookAndFeel(this);

        auto bgColour = findColour(juce::ResizableWindow::backgroundColourId);
        // bgColour = juce::Colours::black.withBrightness(0.1f);
        bgColour = juce::Colour::fromString("ff272a33");
        auto highlightColour = juce::Colour::fromString("ff464b69");
        auto r = bgColour.getRed();
        auto g = bgColour.getGreen();
        auto b = bgColour.getBlue();
        auto inverseColour = juce::Colour(255 - r, 255 - g, 255 - b);
        inverseColour = inverseColour.withBrightness(1.0f - bgColour.getBrightness());

        this->setColour(juce::ResizableWindow::backgroundColourId, bgColour);

        auto scheme = this->getCurrentColourScheme();
        scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::widgetBackground, bgColour);
        scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::windowBackground, bgColour);
        scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::menuBackground, bgColour);
        scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::outline, inverseColour);
        scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::defaultText, inverseColour);
        scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::highlightedText, inverseColour);
        scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::menuText, inverseColour);

        // scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::defaultFill, highlightColour);
        scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::highlightedFill, highlightColour);
        this->setColourScheme(scheme);
    }

    ~LookAndFeel() override

    {
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    }

    Component* getParentComponentForMenuOptions(const PopupMenu::Options& options) override
    {
        if (auto* target = options.getTopLevelTargetComponent())
            return target->findParentComponentOfClass<AudioProcessorEditor>();

        return nullptr;
    }

    void drawRotarySlider(
        Graphics& g,
        int x,
        int y,
        int width,
        int height,
        float sliderPos,
        const float rotaryStartAngle,
        const float rotaryEndAngle,
        Slider& slider
    ) override
    {
        auto outline = slider.findColour(Slider::rotarySliderOutlineColourId);
        auto fill = slider.findColour(Slider::rotarySliderFillColourId);

        auto bounds = Rectangle<int>(x, y, width, height).toFloat().reduced(10);

        auto radius = jmin(bounds.getWidth(), bounds.getHeight()) / 2.0f;
        auto toAngle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
        auto lineW = jmin(8.0f, radius * 0.5f);
        auto arcRadius = radius - lineW * 0.5f;

        Path backgroundArc;
        backgroundArc.addCentredArc(
            bounds.getCentreX(),
            bounds.getCentreY(),
            arcRadius,
            arcRadius,
            0.0f,
            rotaryStartAngle,
            rotaryEndAngle,
            true
        );

        g.setColour(outline);
        g.strokePath(backgroundArc, PathStrokeType(lineW, PathStrokeType::curved, PathStrokeType::rounded));

        if (slider.isEnabled())
        {
            Path valueArc;
            valueArc.addCentredArc(
                bounds.getCentreX(),
                bounds.getCentreY(),
                arcRadius,
                arcRadius,
                0.0f,
                rotaryStartAngle,
                toAngle,
                true
            );

            g.setColour(fill);
            g.strokePath(valueArc, PathStrokeType(lineW, PathStrokeType::curved, PathStrokeType::rounded));
        }

        auto thumbWidth = lineW * 1.0f;
        Point<float> thumbPoint(
            bounds.getCentreX() + arcRadius * std::cos(toAngle - MathConstants<float>::halfPi),
            bounds.getCentreY() + arcRadius * std::sin(toAngle - MathConstants<float>::halfPi)
        );

        g.setColour(slider.findColour(Slider::thumbColourId));
        g.fillEllipse(Rectangle<float>(thumbWidth, thumbWidth).withCentre(thumbPoint));
    }

    void drawMenuBarItem(
        Graphics& g,
        int width,
        int height,
        int itemIndex,
        const String& itemText,
        bool isMouseOverItem,
        bool isMenuOpen,
        bool /*isMouseOverBar*/,
        MenuBarComponent& menuBar
    ) override
    {
        if (!menuBar.isEnabled())
        {
            g.setColour(menuBar.findColour(TextButton::textColourOffId).withMultipliedAlpha(0.5f));
        }
        else if (isMenuOpen || isMouseOverItem)
        {
            juce::Path path;

            path.addRoundedRectangle(juce::Rectangle(width, height), 3.0f);
            g.reduceClipRegion(path);

            auto colour = juce::Colours::black.withAlpha(0.5f);
            g.fillAll(colour);
            g.setColour(menuBar.findColour(TextButton::textColourOnId));
        }
        else
        {
            g.setColour(menuBar.findColour(TextButton::textColourOffId));
        }

        g.setFont(getMenuBarFont(menuBar, itemIndex, itemText));
        g.drawFittedText(itemText, 0, 0, width, height, Justification::centred, 1);
    }

private:
};
} // namespace atk
