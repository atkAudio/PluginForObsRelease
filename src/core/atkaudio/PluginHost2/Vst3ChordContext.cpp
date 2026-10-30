#include "Vst3ChordContext.h"
#include <juce_audio_processors_headless/format_types/VST3_SDK/pluginterfaces/vst/ivstprocesscontext.h>

Vst3ChordContext::Vst3ChordContext(std::shared_ptr<chord_analyzer::PublishedChord> context)
    : published(std::move(context))
{
}

void Vst3ChordContext::beginBlock()
{
    snapshot.store(published->read().encode(), std::memory_order_release);
}

void Vst3ChordContext::reset()
{
    snapshot.store(0, std::memory_order_release);
}

void Vst3ChordContext::updateProcessContext(Steinberg::Vst::ProcessContext& context)
{
    auto chord = chord_analyzer::Result::decode(snapshot.load(std::memory_order_acquire));
    context.state &= ~Steinberg::Vst::ProcessContext::kChordValid;
    context.chord = {};
    if (chord.status == chord_analyzer::recognized)
    {
        context.chord.keyNote = chord.root;
        context.chord.rootNote = chord.bass % 12;
        context.chord.chordMask = chord.vst3Mask();
        context.state |= Steinberg::Vst::ProcessContext::kChordValid;
    }
}
