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
        instance = this;

        // Apply saved colors if they exist, otherwise use defaults
        if (customColorsSet)
            setColors(savedBackgroundColor, savedTextColor);
        else
            applyDefaultColors();
    }

    /**
     * Apply custom colors to the look and feel
     * @param background Background color
     * @param text Text/foreground color
     */
    void setColors(juce::Colour background, juce::Colour text)
    {
        // Save colors globally for future instances
        savedBackgroundColor = background;
        savedTextColor = text;
        customColorsSet = true;

        auto highlightColour = background.brighter(0.3f);

        this->setColour(juce::ResizableWindow::backgroundColourId, background);

        auto scheme = this->getCurrentColourScheme();
        scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::widgetBackground, background);
        scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::windowBackground, background);
        scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::menuBackground, background);
        scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::outline, text);
        scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::defaultText, text);
        scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::highlightedText, text);
        scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::menuText, text);
        scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::highlightedFill, highlightColour);
        this->setColourScheme(scheme);

        // Notify all existing components to refresh with new colors
        auto& desktop = juce::Desktop::getInstance();
        for (int i = 0; i < desktop.getNumComponents(); ++i)
            if (auto* comp = desktop.getComponent(i))
                comp->sendLookAndFeelChange();
    }

    /**
     * Static method to apply colors to the current instance
     */
    static void applyColorsToInstance(juce::Colour background, juce::Colour text)
    {
        if (instance != nullptr)
            instance->setColors(background, text);
        else
        {
            // Save colors for when instance is created
            savedBackgroundColor = background;
            savedTextColor = text;
            customColorsSet = true;
        }
    }

    ~LookAndFeel() override
    {
        if (instance == this)
            instance = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    }

private:
    // Static members for tracking instance and saved colors
    static inline LookAndFeel* instance = nullptr;
    static inline bool customColorsSet = false;
    static inline juce::Colour savedBackgroundColor;
    static inline juce::Colour savedTextColor;

    void applyDefaultColors()
    {
        auto bgColour = juce::Colour::fromString("ff272a33");
        auto r = bgColour.getRed();
        auto g = bgColour.getGreen();
        auto b = bgColour.getBlue();
        auto textColour = juce::Colour(255 - r, 255 - g, 255 - b);
        textColour = textColour.withBrightness(1.0f - bgColour.getBrightness());

        setColors(bgColour, textColour);
    }

public:
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

        auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(10);

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
        g.fillEllipse(juce::Rectangle<float>(thumbWidth, thumbWidth).withCentre(thumbPoint));
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

    void drawMenuBarBackground(Graphics& g, int width, int height, bool isMouseOverBar, MenuBarComponent&) override
    {
        g.fillAll(findColour(juce::ResizableWindow::backgroundColourId));
    }

private:
};
} // namespace atk
