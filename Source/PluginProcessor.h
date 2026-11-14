#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PhaseAlignCore.h"
#include "ScopeBuffer.h"
#include <atomic>
#include <vector>

class PsyPhaseLockAudioProcessor : public juce::AudioProcessor
{
public:
    PsyPhaseLockAudioProcessor();
    ~PsyPhaseLockAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "DaniSan PsyPhase Lock"; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;
    psy::PhaseAligner aligner;
    psy::ScopeBuffer  scope;                       // beat-synchronous waveform capture (for the editor)
    std::atomic<bool>  scopeActive { false };      // set by the editor: no capture cost while it is closed
    std::atomic<float> hostBpm { 0.0f };

private:
    std::vector<float> tapK, tapBi, tapBo;
    double freeRunPpq = 0.0, lastBpm = 0.0;
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PsyPhaseLockAudioProcessor)
};
