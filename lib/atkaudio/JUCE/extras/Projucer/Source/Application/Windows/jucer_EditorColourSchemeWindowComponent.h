/*
  ==============================================================================

   This file is part of the JUCE framework.
   Copyright (c) Raw Material Software Limited

   JUCE is an open source framework subject to commercial or open source
   licensing.

   By downloading, installing, or using the JUCE framework, or combining the
   JUCE framework with any other source code, object code, content or any other
   copyrightable work, you agree to the terms of the JUCE End User Licence
   Agreement, and all incorporated terms including the JUCE Privacy Policy and
   the JUCE Website Terms of Service, as applicable, which will bind you. If you
   do not agree to the terms of these agreements, we will not license the JUCE
   framework to you, and you must discontinue the installation or download
   process and cease use of the JUCE framework.

   JUCE End User Licence Agreement: https://juce.com/legal/juce-8-licence/
   JUCE Privacy Policy: https://juce.com/juce-privacy-policy
   JUCE Website Terms of Service: https://juce.com/juce-website-terms-of-service/

   Or:

   You may also use this code under the terms of the AGPLv3:
   https://www.gnu.org/licenses/agpl-3.0.en.html

   THE JUCE FRAMEWORK IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL
   WARRANTIES, WHETHER EXPRESSED OR IMPLIED, INCLUDING WARRANTY OF
   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.

  ==============================================================================
*/

#pragma once

#include "../../Utility/UI/PropertyComponents/jucer_ColourPropertyComponent.h"

//==============================================================================
class EditorColourSchemeWindowComponent final : public Component
{
public:
    EditorColourSchemeWindowComponent()
    {
        if (getAppSettings().monospacedFontNames.size() == 0)
            changeContent (new AppearanceEditor::FontScanPanel());
        else
            changeContent (new AppearanceEditor::EditorPanel());
    }

    void paint (Graphics& g) override
    {
        g.fillAll (findColour (backgroundColourId));
    }

    void resized() override
    {
       content->setBounds (getLocalBounds());
    }

    void changeContent (Component* newContent)
    {
        content.reset (newContent);
        addAndMakeVisible (newContent);
        content->setBounds (getLocalBounds().reduced (10));
    }

private:
    std::unique_ptr<Component> content;

    //==============================================================================
    struct AppearanceEditor
    {
        struct FontScanPanel final : public Component,
                                     private Timer
        {
            FontScanPanel()
            {
                fontsToScan = Font::findAllTypefaceNames();
                startTimer (1);
            }

            void paint (Graphics& g) override
            {
                g.fillAll (findColour (backgroundColourId));

                g.setFont (14.0f);
                g.setColour (findColour (defaultTextColourId));
                g.drawFittedText ("Scanning for fonts..", getLocalBounds(), Justification::centred, 2);

                const auto size = 30;
                getLookAndFeel().drawSpinningWaitAnimation (g, Colours::white, (getWidth() - size) / 2, getHeight() / 2 - 50, size, size);
            }

            void timerCallback() override
            {
                repaint();

                if (fontsToScan.size() == 0)
                {
                    getAppSettings().monospacedFontNames = fontsFound;

                    if (auto* owner = findParentComponentOfClass<EditorColourSchemeWindowComponent>())
                        owner->changeContent (new EditorPanel());
                }
                else
                {
                    if (isMonospacedTypeface (fontsToScan[0]))
                        fontsFound.add (fontsToScan[0]);

                    fontsToScan.remove (0);
                }
            }

            // A rather hacky trick to select only the fixed-pitch fonts..
            // This is unfortunately a bit slow, but will work on all platforms.
            static bool isMonospacedTypeface (const String& name)
            {
                const Font font = FontOptions (name, 20.0f, Font::plain);

                const auto width = GlyphArrangement::getStringWidthInt (font, "....");

                return width == GlyphArrangement::getStringWidthInt (font, "WWWW")
                    && width == GlyphArrangement::getStringWidthInt (font, "0000")
                    && width == GlyphArrangement::getStringWidthInt (font, "1111")
                    && width == GlyphArrangement::getStringWidthInt (font, "iiii");
            }

            StringArray fontsToScan, fontsFound;
        };

        //==============================================================================
        struct EditorPanel final : public Component
        {
            EditorPanel()
                : loadButton ("Load Scheme..."),
                  saveButton ("Save Scheme...")
            {
                rebuildProperties();
                addAndMakeVisible (panel);

                addAndMakeVisible (loadButton);
                addAndMakeVisible (saveButton);

                loadButton.onClick = [this] { loadScheme(); };
                saveButton.onClick = [this] { saveScheme (false); };

                lookAndFeelChanged();

                saveSchemeState();
            }

            ~EditorPanel() override
            {
                if (hasSchemeBeenModifiedSinceSave())
                    saveScheme (true);
            }

            void rebuildProperties()
            {
                auto& scheme = getAppSettings().appearance;

                Array<PropertyComponent*> props;
                auto fontValue = scheme.getCodeFontValue();
                props.add (FontNameValueSource::createProperty ("Code Editor Font", fontValue));
                props.add (FontSizeValueSource::createProperty ("Font Size", fontValue));

                const auto colourNames = scheme.getColourNames();

                for (int i = 0; i < colourNames.size(); ++i)
                    props.add (new ColourPropertyComponent (nullptr, colourNames[i],
                                                            scheme.getColourValue (colourNames[i]),
                                                            Colours::white, false));

                panel.clear();
                panel.addProperties (props);
            }

            void resized() override
            {
                auto r = getLocalBounds();
                panel.setBounds (r.removeFromTop (getHeight() - 28).reduced (10, 2));
                loadButton.setBounds (r.removeFromLeft (getWidth() / 2).reduced (10, 1));
                saveButton.setBounds (r.reduced (10, 1));
            }

        private:
            PropertyPanel panel;
            TextButton loadButton, saveButton;

            Font codeFont { FontOptions{} };
            Array<var> colourValues;

            void saveScheme (bool isExit)
            {
                chooser = std::make_unique<FileChooser> ("Select a file in which to save this colour-scheme...",
                                                         getAppSettings().appearance.getSchemesFolder()
                                                         .getNonexistentChildFile ("Scheme", AppearanceSettings::getSchemeFileSuffix()),
                                                         AppearanceSettings::getSchemeFileWildCard());
                auto chooserFlags = FileBrowserComponent::saveMode
                                  | FileBrowserComponent::canSelectFiles
                                  | FileBrowserComponent::warnAboutOverwriting;

                chooser->launchAsync (chooserFlags, [this, isExit] (const FileChooser& fc)
                {
                    if (fc.getResult() == File{})
                    {
                        if (isExit)
                            restorePreviousScheme();

                        return;
                    }

                    File file (fc.getResult().withFileExtension (AppearanceSettings::getSchemeFileSuffix()));
                    getAppSettings().appearance.writeToFile (file);
                    getAppSettings().appearance.refreshPresetSchemeList();

                    saveSchemeState();
                    ProjucerApplication::getApp().selectEditorColourSchemeWithName (file.getFileNameWithoutExtension());
                });
            }

            void loadScheme()
            {
                chooser = std::make_unique<FileChooser> ("Please select a colour-scheme file to load...",
                                                         getAppSettings().appearance.getSchemesFolder(),
                                                         AppearanceSettings::getSchemeFileWildCard());
                auto chooserFlags = FileBrowserComponent::openMode
                                  | FileBrowserComponent::canSelectFiles;

                chooser->launchAsync (chooserFlags, [this] (const FileChooser& fc)
                {
                    if (fc.getResult() == File{})
                        return;

                    if (getAppSettings().appearance.readFromFile (fc.getResult()))
                    {
                        rebuildProperties();
                        saveSchemeState();
                    }
                });
            }

            void lookAndFeelChanged() override
            {
                loadButton.setColour (TextButton::buttonColourId,
                                      findColour (secondaryButtonBackgroundColourId));
            }

            void saveSchemeState()
            {
                auto& appearance = getAppSettings().appearance;
                const auto colourNames = appearance.getColourNames();

                codeFont = appearance.getCodeFont();

                colourValues.clear();
                for (int i = 0; i < colourNames.size(); ++i)
                    colourValues.add (appearance.getColourValue (colourNames[i]).getValue());
            }

            bool hasSchemeBeenModifiedSinceSave()
            {
                auto& appearance = getAppSettings().appearance;
                const auto colourNames = appearance.getColourNames();

                if (codeFont != appearance.getCodeFont())
                    return true;

                for (int i = 0; i < colourNames.size(); ++i)
                    if (colourValues[i] != appearance.getColourValue (colourNames[i]).getValue())
                        return true;

                return false;
            }

            void restorePreviousScheme()
            {
                auto& appearance = getAppSettings().appearance;
                const auto colourNames = appearance.getColourNames();

                appearance.getCodeFontValue().setValue (codeFont.toString());

                for (int i = 0; i < colourNames.size(); ++i)
                    appearance.getColourValue (colourNames[i]).setValue (colourValues[i]);
            }

            std::unique_ptr<FileChooser> chooser;

            JUCE_DECLARE_NON_COPYABLE (EditorPanel)
        };

        //==============================================================================
        struct FontNameValueSource final : public ValueSourceFilter
        {
            FontNameValueSource (const Value& source)  : ValueSourceFilter (source) {}

            var getValue() const override
            {
                return Font::fromString (sourceValue.toString()).getTypefaceName();
            }

            void setValue (const var& newValue) override
            {
                auto font = Font::fromString (sourceValue.toString());
                font.setTypefaceName (newValue.toString().isEmpty() ? Font::getDefaultMonospacedFontName()
                                      : newValue.toString());
                sourceValue = font.toString();
            }

            static ChoicePropertyComponent* createProperty (const String& title, const Value& value)
            {
                auto fontNames = getAppSettings().monospacedFontNames;

                Array<var> values;
                values.add (Font::getDefaultMonospacedFontName());
                values.add (var());

                for (int i = 0; i < fontNames.size(); ++i)
                    values.add (fontNames[i]);

                StringArray names;
                names.add ("<Default Monospaced>");
                names.add (String());
                names.addArray (getAppSettings().monospacedFontNames);

                return new ChoicePropertyComponent (Value (new FontNameValueSource (value)),
                                                    title, names, values);
            }
        };

        //==============================================================================
        struct FontSizeValueSource final : public ValueSourceFilter
        {
            FontSizeValueSource (const Value& source)  : ValueSourceFilter (source) {}

            var getValue() const override
            {
                return Font::fromString (sourceValue.toString()).getHeight();
            }

            void setValue (const var& newValue) override
            {
                sourceValue = Font::fromString (sourceValue.toString()).withHeight (newValue).toString();
            }

            static PropertyComponent* createProperty (const String& title, const Value& value)
            {
                return new SliderPropertyComponent (Value (new FontSizeValueSource (value)),
                                                    title, 5.0, 40.0, 0.1, 0.5);
            }
        };
    };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditorColourSchemeWindowComponent)
};
