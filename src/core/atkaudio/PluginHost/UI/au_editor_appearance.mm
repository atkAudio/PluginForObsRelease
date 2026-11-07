#include "au_editor_appearance.h"

#include <juce_gui_extra/juce_gui_extra.h>

#import <AppKit/AppKit.h>

namespace atk
{
void ApplyGenericAudioUnitEditorAppearance(juce::Component* editor)
{
    if (editor == nullptr)
        return;

    if (auto* wrapper = dynamic_cast<juce::NSViewComponent*>(editor))
    {
        NSView* view = static_cast<NSView*>(wrapper->getView());
        if ([view isKindOfClass:NSClassFromString(@"AUGenericView")])
            [view setAppearance:[NSAppearance appearanceNamed:NSAppearanceNameAqua]];
    }

    for (int index = 0; index < editor->getNumChildComponents(); ++index)
        ApplyGenericAudioUnitEditorAppearance(editor->getChildComponent(index));
}
}
