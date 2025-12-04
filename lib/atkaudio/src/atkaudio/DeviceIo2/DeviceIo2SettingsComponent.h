#pragma once

#include "../LookAndFeel.h"
#include "../DeviceIo/AudioDeviceSelectorComponent.h"

#include <juce_audio_utils/juce_audio_utils.h>

using namespace juce;

class RoutingMatrixComponent : public Component
{
public:
    RoutingMatrixComponent(const String& title, int rows, int cols)
        : titleLabel("", title)
        , numRows(rows)
        , numCols(cols)
    {
        titleLabel.setFont(Font(14.0f, Font::bold));
        titleLabel.setJustificationType(Justification::centredLeft);
        addAndMakeVisible(titleLabel);

        // Create checkboxes for the matrix
        for (int row = 0; row < numRows; ++row)
        {
            for (int col = 0; col < numCols; ++col)
            {
                auto* checkbox = new ToggleButton();
                checkbox->setClickingTogglesState(true);
                checkbox->onClick = [this, row, col]() { onMatrixChanged(row, col); };
                addAndMakeVisible(checkbox);
                checkboxes.add(checkbox);
            }
        }
    }

    void setMatrix(const std::vector<std::vector<bool>>& matrix)
    {
        for (int row = 0; row < numRows && row < (int)matrix.size(); ++row)
        {
            for (int col = 0; col < numCols && col < (int)matrix[row].size(); ++col)
            {
                int index = row * numCols + col;
                if (index < checkboxes.size())
                    checkboxes[index]->setToggleState(matrix[row][col], dontSendNotification);
            }
        }
    }

    std::vector<std::vector<bool>> getMatrix() const
    {
        std::vector<std::vector<bool>> matrix(numRows);
        for (int row = 0; row < numRows; ++row)
        {
            matrix[row].resize(numCols);
            for (int col = 0; col < numCols; ++col)
            {
                int index = row * numCols + col;
                if (index < checkboxes.size())
                    matrix[row][col] = checkboxes[index]->getToggleState();
            }
        }
        return matrix;
    }

    std::function<void(int row, int col)> onMatrixChanged;

    void paint(Graphics& g) override
    {
        g.fillAll(getLookAndFeel().findColour(ResizableWindow::backgroundColourId).darker(0.1f));

        // Draw grid lines
        g.setColour(Colours::grey);
        auto r = getLocalBounds().withTop(titleLabel.getBottom() + 20);

        for (int row = 0; row <= numRows; ++row)
        {
            float y = r.getY() + row * cellHeight;
            g.drawLine((float)r.getX(), y, (float)r.getRight(), y, 0.5f);
        }

        for (int col = 0; col <= numCols; ++col)
        {
            float x = r.getX() + col * cellWidth;
            g.drawLine(x, (float)r.getY(), x, (float)r.getBottom(), 0.5f);
        }

        // Draw row/column labels
        g.setColour(Colours::white);
        g.setFont(10.0f);

        for (int row = 0; row < numRows; ++row)
        {
            float y = r.getY() + row * cellHeight;
            g.drawText(String(row + 1), r.getX() - 20, (int)y, 18, (int)cellHeight, Justification::centredRight);
        }

        for (int col = 0; col < numCols; ++col)
        {
            float x = r.getX() + col * cellWidth;
            g.drawText(String(col + 1), (int)x, r.getY() - 20, (int)cellWidth, 18, Justification::centred);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        titleLabel.setBounds(r.removeFromTop(30).reduced(5));

        r.removeFromTop(20);  // Space for column labels
        r.removeFromLeft(20); // Space for row labels

        cellWidth = r.getWidth() / (float)numCols;
        cellHeight = r.getHeight() / (float)numRows;

        for (int row = 0; row < numRows; ++row)
        {
            for (int col = 0; col < numCols; ++col)
            {
                int index = row * numCols + col;
                if (index < checkboxes.size())
                {
                    float x = r.getX() + col * cellWidth;
                    float y = r.getY() + row * cellHeight;
                    checkboxes[index]->setBounds(
                        (int)(x + cellWidth / 4),
                        (int)(y + cellHeight / 4),
                        (int)(cellWidth / 2),
                        (int)(cellHeight / 2)
                    );
                }
            }
        }
    }

private:
    Label titleLabel;
    int numRows;
    int numCols;
    float cellWidth = 30.0f;
    float cellHeight = 30.0f;
    OwnedArray<ToggleButton> checkboxes;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RoutingMatrixComponent)
};

class DeviceIo2SettingsComponent : public Component
{
public:
    DeviceIo2SettingsComponent(
        AudioDeviceManager& deviceManagerToUse,
        int maxAudioInputChannels,
        int maxAudioOutputChannels
    )
        : deviceSelector(
              deviceManagerToUse,
              0,
              maxAudioInputChannels,
              0,
              maxAudioOutputChannels,
              false,
              false,
              false,
              true
          )
        , inputRoutingMatrix("Input Routing Matrix", 8, 8)
        , outputRoutingMatrix("Output Routing Matrix", 8, 8)
    {
        setOpaque(true);

        addAndMakeVisible(deviceSelector);
        addAndMakeVisible(inputRoutingMatrix);
        addAndMakeVisible(outputRoutingMatrix);

        // Initialize with diagonal pass-through
        std::vector<std::vector<bool>> defaultMatrix(8);
        for (int i = 0; i < 8; ++i)
        {
            defaultMatrix[i].resize(8, false);
            defaultMatrix[i][i] = true;
        }
        inputRoutingMatrix.setMatrix(defaultMatrix);
        outputRoutingMatrix.setMatrix(defaultMatrix);
    }

    void paint(Graphics& g) override
    {
        g.fillAll(getLookAndFeel().findColour(ResizableWindow::backgroundColourId));
    }

    void resized() override
    {
        const ScopedValueSetter<bool> scope(isResizing, true);

        auto r = getLocalBounds();

        // Device selector at the top
        auto deviceSelectorBounds = r.removeFromTop(250);
        deviceSelector.setBounds(deviceSelectorBounds);

        // Split remaining area for two routing matrices
        auto matrixArea = r.reduced(5);
        auto inputMatrixBounds = matrixArea.removeFromTop(matrixArea.getHeight() / 2).reduced(5);
        auto outputMatrixBounds = matrixArea.reduced(5);

        inputRoutingMatrix.setBounds(inputMatrixBounds);
        outputRoutingMatrix.setBounds(outputMatrixBounds);
    }

    void childBoundsChanged(Component* childComp) override
    {
        if (!isResizing && childComp == &deviceSelector)
            setToRecommendedSize();
    }

    void setToRecommendedSize()
    {
        setSize(700, 600);
    }

    std::vector<std::vector<bool>> getInputRoutingMatrix() const
    {
        return inputRoutingMatrix.getMatrix();
    }

    std::vector<std::vector<bool>> getOutputRoutingMatrix() const
    {
        return outputRoutingMatrix.getMatrix();
    }

    void setInputRoutingMatrix(const std::vector<std::vector<bool>>& matrix)
    {
        inputRoutingMatrix.setMatrix(matrix);
    }

    void setOutputRoutingMatrix(const std::vector<std::vector<bool>>& matrix)
    {
        outputRoutingMatrix.setMatrix(matrix);
    }

private:
    atk::AudioDeviceSelectorComponent deviceSelector;
    RoutingMatrixComponent inputRoutingMatrix;
    RoutingMatrixComponent outputRoutingMatrix;
    bool isResizing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DeviceIo2SettingsComponent)
};
