#include <iostream>
#include <juce_audio_processors/juce_audio_processors.h>

int main(int argc, const char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " <plugin-identifier>" << std::endl;
        return 1;
    }

    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::AudioPluginFormatManager formatManager;
    juce::addDefaultFormatsToManager(formatManager);

    const juce::String identifier(argv[1]);

    // Find matching format
    juce::AudioPluginFormat* format = nullptr;
    for (int i = 0; i < formatManager.getNumFormats(); ++i)
    {
        if (formatManager.getFormat(i)->fileMightContainThisPluginType(identifier))
        {
            format = formatManager.getFormat(i);
            break;
        }
    }

    juce::XmlElement xml("SCANRESULT");

    if (!format)
    {
        xml.setAttribute("success", false);
        xml.setAttribute("error", "Unknown format: " + identifier);
    }
    else
    {
        juce::OwnedArray<juce::PluginDescription> descriptions;
        format->findAllTypesForFile(descriptions, identifier);

        if (descriptions.isEmpty())
        {
            xml.setAttribute("success", false);
            xml.setAttribute("error", "No plugins found: " + identifier);
        }
        else
        {
            xml.setAttribute("success", true);
            xml.setAttribute("identifier", identifier);
            xml.setAttribute("format", format->getName());

            for (auto* desc : descriptions)
                xml.addChildElement(desc->createXml().release());
        }
    }

    std::cout << xml.toString().toStdString() << std::endl;
    return 0;
}
