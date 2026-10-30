#include "TransportEditor.h"

struct TransportEditor::TempoEditor : public juce::TextEditor
{
    explicit TempoEditor(TransportEditor& editor)
        : owner(editor)
    {
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key.isKeyCode(juce::KeyPress::spaceKey) && !key.getModifiers().isAnyModifierKeyDown())
        {
            if (!spaceWasDown)
            {
                spaceWasDown = true;
                owner.tap();
            }
            return true;
        }
        return TextEditor::keyPressed(key);
    }

    bool keyStateChanged(bool isKeyDown) override
    {
        bool spaceIsDown = juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::spaceKey);
        bool consumed = spaceWasDown;
        if (!spaceIsDown)
        {
            spaceWasDown = false;
            owner.taps.release();
        }
        return spaceIsDown || consumed || TextEditor::keyStateChanged(isKeyDown);
    }

    void focusGained(FocusChangeType reason) override
    {
        TextEditor::focusGained(reason);
        owner.tapHint.setVisible(isEnabled());
    }

    void focusLost(FocusChangeType reason) override
    {
        owner.tapHint.setVisible(false);
        owner.resetTaps();
        spaceWasDown = false;
        TextEditor::focusLost(reason);
    }

    TransportEditor& owner;
    bool spaceWasDown = false;
};

TransportEditor::TransportEditor(TransportProcessor& processor)
    : AudioProcessorEditor(processor)
    , transport(processor)
    , bpm(std::make_unique<TempoEditor>(*this))
{
    bpmLabel.setText("BPM", juce::dontSendNotification);
    signatureLabel.setText("TSig", juce::dontSendNotification);
    slash.setText("/", juce::dontSendNotification);
    positionLabel.setText("Position", juce::dontSendNotification);
    inputLabel.setText("MIDI Input", juce::dontSendNotification);
    outputLabel.setText("MIDI Output", juce::dontSendNotification);
    for (auto* label : {&bpmLabel, &signatureLabel, &slash, &positionLabel, &inputLabel, &outputLabel, &status})
    {
        label->setFont(juce::FontOptions(14.0f));
        addAndMakeVisible(*label);
    }
    tapHint.setComponentID("transport-tap-hint");
    tapHint.setText("Space to tap", juce::dontSendNotification);
    tapHint.setFont(juce::FontOptions(14.0f));
    tapHint.setInterceptsMouseClicks(false, false);
    addChildComponent(tapHint);
    bpm->setComponentID("transport-bpm");
    bpm->setTitle("Tempo in beats per minute");
    bpm->setTooltip("Tempo (BPM). Space: tap tempo");
    bpm->setInputRestrictions(6, "0123456789.");
    numerator.setComponentID("transport-numerator");
    numerator.setTitle("Time signature numerator");
    numerator.setInputRestrictions(2, "0123456789");
    position.setComponentID("transport-position");
    position.setTitle("Position in bars, beats and ticks");
    position.setTooltip("Bars:beats:ticks (960 ticks per quarter note)");
    position.setInputRestrictions(24, "0123456789:");
    for (auto* editor : {static_cast<juce::TextEditor*>(bpm.get()), &numerator, &position})
    {
        editor->setFont(juce::FontOptions(editor == &position ? 20.0f : 15.0f));
        editor->setJustification(juce::Justification::centred);
        editor->setSelectAllWhenFocused(true);
        addAndMakeVisible(*editor);
    }
    denominator.setComponentID("transport-denominator");
    denominator.setTitle("Time signature denominator");
    for (int value = 1; value <= 64; value *= 2)
        denominator.addItem(juce::String(value), value);
    addAndMakeVisible(denominator);

    std::array<juce::String, 5> names{"Previous bar", "Start", "Pause", "Stop", "Next bar"};
    std::array<TransportOperation, 5>
        operations{transportPreviousBar, transportPlay, transportPause, transportStop, transportNextBar};
    auto normal = findColour(juce::Label::textColourId);
    auto active = findColour(juce::TextEditor::highlightColourId);
    for (int index = 0; index < 5; ++index)
    {
        auto button = std::make_unique<juce::ShapeButton>(names[index], normal, active, active.brighter());
        button->setTooltip(names[index]);
        button->setComponentID("transport-" + juce::String(index));
        juce::Path icon;
        if (index == 0 || index == 4)
        {
            bool previous = index == 0;
            icon.addTriangle(
                previous ? 20.0f : 4.0f,
                3.0f,
                previous ? 5.0f : 19.0f,
                12.0f,
                previous ? 20.0f : 4.0f,
                21.0f
            );
            icon.addRectangle(previous ? 2.0f : 20.0f, 3.0f, 3.0f, 18.0f);
        }
        else if (index == 1)
            icon.addTriangle(4.0f, 3.0f, 21.0f, 12.0f, 4.0f, 21.0f);
        else if (index == 2)
        {
            icon.addRectangle(4.0f, 3.0f, 5.0f, 18.0f);
            icon.addRectangle(15.0f, 3.0f, 5.0f, 18.0f);
        }
        else
            icon.addRectangle(4.0f, 4.0f, 16.0f, 16.0f);
        button->setShape(icon, false, true, false);
        button->onClick = [this, operation = operations[index]]
        {
            transport.command(operation);
        };
        addAndMakeVisible(*button);
        buttons[index] = std::move(button);
    }

    std::array<juce::String, 7> labels{
        "Follow Clock",
        "Follow Song Position",
        "Follow Start / Continue / Stop",
        "Send Clock",
        "Send Song Position",
        "Send Start / Continue / Stop",
        "Clock while stopped"
    };
    std::array<int, 7>
        flags{followClock, followPosition, followPlayback, sendClock, sendPosition, sendPlayback, continuousClock};
    for (int index = 0; index < 7; ++index)
    {
        switches[index].setButtonText(labels[index]);
        switches[index].setComponentID("transport-mode-" + juce::String(flags[index]));
        switches[index].onClick = [this, flag = flags[index], index]
        {
            int modes = transport.getModes();
            transport.setModes(switches[index].getToggleState() ? modes | flag : modes & ~flag);
            resetTaps();
            refresh(true);
        };
        addAndMakeVisible(switches[index]);
    }
    bpm->onReturnKey = [this]
    {
        commitTempo();
    };
    bpm->onFocusLost = [this]
    {
        commitTempo();
    };
    bpm->onEscapeKey = [this]
    {
        resetTaps();
        refresh(true);
    };
    numerator.onReturnKey = [this]
    {
        commitNumerator();
    };
    numerator.onFocusLost = [this]
    {
        commitNumerator();
    };
    numerator.onEscapeKey = [this]
    {
        refresh(true);
    };
    denominator.onChange = [this]
    {
        auto settings = transport.getSettings();
        settings.denominator = denominator.getSelectedId();
        transport.setSettings(settings);
        resetTaps();
        refresh(true);
    };
    position.onReturnKey = [this]
    {
        transport.seekPositionText(position.getText());
        refresh(true);
    };
    position.onFocusLost = position.onReturnKey;
    position.onEscapeKey = [this]
    {
        refresh(true);
    };
    setResizable(true, false);
    setResizeLimits(520, 330, 1000, 600);
    setSize(560, 350);
    refresh(true);
    startTimer(50);
}

TransportEditor::~TransportEditor()
{
    stopTimer();
    bpm->onFocusLost = nullptr;
    numerator.onFocusLost = nullptr;
    position.onFocusLost = nullptr;
    removeAllChildren();
}

void TransportEditor::refresh(bool force)
{
    auto settings = transport.getSettings();
    int modes = transport.getModes();
    if (settings.encode() != observedSettings || modes != observedModes)
        resetTaps();
    observedSettings = settings.encode();
    observedModes = modes;
    bool following = (modes & followClock) != 0;
    bpm->setEnabled(!following);
    if (following)
        tapHint.setVisible(false);
    if (force || !bpm->hasKeyboardFocus(true))
        bpm->setText(
            juce::String((following ? transport.getEffectiveTempo() : settings.tempoHundredths) / 100.0, 2),
            false
        );
    if (force || !numerator.hasKeyboardFocus(true))
        numerator.setText(juce::String(settings.numerator), false);
    denominator.setSelectedId(settings.denominator, juce::dontSendNotification);
    if (force || !position.hasKeyboardFocus(true))
        position.setText(transport.getPositionText(), false);
    std::array<int, 7>
        flags{followClock, followPosition, followPlayback, sendClock, sendPosition, sendPlayback, continuousClock};
    for (int index = 0; index < 7; ++index)
        switches[index].setToggleState((modes & flags[index]) != 0, juce::dontSendNotification);
    switches[6].setEnabled((modes & sendClock) != 0);
    juce::String state = transport.isPlaying() ? "Playing" : transport.isPaused() ? "Paused" : "Stopped";
    if (following)
    {
        std::array<juce::String, 4> sync{"Acquiring", "Acquiring", "Clock locked", "Clock lost - free-running"};
        state += " | " + sync[juce::jlimit(0, 3, transport.getSyncStatus())];
    }
    if (transport.getRejectedCommands() != 0)
        state += " | MIDI warning";
    status.setText(state, juce::dontSendNotification);
}

void TransportEditor::resetTaps()
{
    taps.reset();
    tapHasEdit = false;
}

void TransportEditor::tap()
{
    if ((transport.getModes() & followClock) != 0)
        return;
    double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    if (taps.expired(now) || observedSettings != transport.getSettings().encode())
        resetTaps();
    auto settings = transport.getSettings();
    if (taps.press(now, settings.tempoHundredths))
    {
        bool changed = settings.encode() != transport.getSettings().encode();
        transport.setSettings(settings, tapHasEdit);
        tapHasEdit = tapHasEdit || changed;
        observedSettings = settings.encode();
        refresh(true);
    }
}

void TransportEditor::commitTempo()
{
    resetTaps();
    auto settings = transport.getSettings();
    auto text = bpm->getText().trim();
    if ((transport.getModes() & followClock) == 0 && text.isNotEmpty() && text.containsOnly("0123456789."))
    {
        auto end = text.getCharPointer();
        double value = juce::CharacterFunctions::readDoubleValue(end);
        if (end.isEmpty() && value >= 1.0 && value <= 999.0)
        {
            settings.tempoHundredths = juce::roundToInt(value * 100.0);
            transport.setSettings(settings);
        }
    }
    refresh(true);
}

void TransportEditor::commitNumerator()
{
    auto settings = transport.getSettings();
    settings.numerator = numerator.getText().getIntValue();
    transport.setSettings(settings);
    refresh(true);
}

void TransportEditor::timerCallback()
{
    refresh();
}

void TransportEditor::paint(juce::Graphics& graphics)
{
    graphics.fillAll(findColour(juce::ResizableWindow::backgroundColourId));
}

void TransportEditor::resized()
{
    auto area = getLocalBounds().reduced(16);
    auto positionRow = area.removeFromTop(40);
    positionLabel.setBounds(positionRow.removeFromLeft(70));
    position.setBounds(positionRow.removeFromLeft(260));
    area.removeFromTop(12);
    auto buttonRow = area.removeFromTop(34);
    for (auto& button : buttons)
    {
        button->setBounds(buttonRow.removeFromLeft(36));
        buttonRow.removeFromLeft(12);
    }
    area.removeFromTop(16);
    auto settingsRow = area.removeFromTop(30);
    bpmLabel.setBounds(settingsRow.removeFromLeft(42));
    bpm->setBounds(settingsRow.removeFromLeft(80));
    settingsRow.removeFromLeft(24);
    signatureLabel.setBounds(settingsRow.removeFromLeft(42));
    numerator.setBounds(settingsRow.removeFromLeft(40));
    slash.setBounds(settingsRow.removeFromLeft(16));
    denominator.setBounds(settingsRow.removeFromLeft(64));
    settingsRow.removeFromLeft(12);
    tapHint.setBounds(settingsRow);
    area.removeFromTop(18);
    auto headings = area.removeFromTop(24);
    int columnWidth = area.getWidth() / 2;
    inputLabel.setBounds(headings.removeFromLeft(columnWidth));
    outputLabel.setBounds(headings);
    auto input = area.removeFromLeft(columnWidth);
    for (int index = 0; index < 3; ++index)
        switches[index].setBounds(input.removeFromTop(26));
    for (int index = 3; index < 7; ++index)
        switches[index].setBounds(area.removeFromTop(26));
    status.setBounds(16, getHeight() - 28, getWidth() - 32, 24);
}
