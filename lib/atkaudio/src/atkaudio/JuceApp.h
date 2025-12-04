#include "atkaudio.h"
#include <config.h>

class Application : public juce::JUCEApplication
{
public:
    Application() = default;

    const juce::String getApplicationName() override
    {
        return PLUGIN_DISPLAY_NAME;
    }

    const juce::String getApplicationVersion() override
    {
        return PLUGIN_VERSION;
    }

    void initialise(const juce::String&) override
    {
    }

    void shutdown() override
    {
    }
};
