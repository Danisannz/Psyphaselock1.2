#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include "PluginProcessor.h"

//==============================================================================
class PsyLookAndFeel : public juce::LookAndFeel_V4
{
public:
    PsyLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
};

//==============================================================================
// Beat-synchronous oscilloscope.  One beat is shown on the x axis (1 e & a = the four 16ths);
// the last 8 beats are overlaid.  If every bass note is phase-locked to the kick, the traces stack.
//   magenta = kick (sidechain)   cyan = bass AFTER the plugin   orange = bass BEFORE (optional)
class ScopeComponent : public juce::Component, private juce::Timer
{
public:
    explicit ScopeComponent (PsyPhaseLockAudioProcessor& p);
    void paint (juce::Graphics&) override;
    void setFrozen (bool f) { frozen = f; }
    void setShowBefore (bool b) { showBefore = b; }

private:
    static constexpr int C = psy::ScopeBuffer::kCols;
    static constexpr int S = psy::ScopeBuffer::kSlices;
    static int idx (int s, int g, int c, int k) { return (((s * 3 + g) * C) + c) * 2 + k; }

    void timerCallback() override;
    void takeSnapshot();

    PsyPhaseLockAudioProcessor& proc;
    std::vector<float> snap;
    int   snapCur = 0;
    float scaleSm = 0.5f;
    bool  frozen = false, showBefore = false, hasData = false;
};

//==============================================================================
class PsyPhaseLockAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit PsyPhaseLockAudioProcessorEditor (PsyPhaseLockAudioProcessor&);
    ~PsyPhaseLockAudioProcessorEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Knob
    {
        juce::Slider slider;
        juce::Label  label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
    };
    void initKnob (Knob&, const juce::String& id, const juce::String& text, const juce::String& suffix);
    void updateEnabledStates();

    PsyPhaseLockAudioProcessor& proc;
    PsyLookAndFeel laf;
    ScopeComponent scope;
    Knob kFocus, kTarget, kPhase, kThresh, kSettle, kWet, kTrim;
    juce::ToggleButton bGroup { "GROUP" }, bAuto { "AUTO" }, bGhost { "GHOST" }, bStab { "STABLE" },
                       bTrim { "AUTO TRIM" }, bBefore { "BEFORE" }, bFreeze { "FREEZE" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> aGroup, aAuto, aGhost, aStab, aTrim;
    juce::Image bg, logoImg;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PsyPhaseLockAudioProcessorEditor)
};
