#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorValueTreeState::ParameterLayout PsyPhaseLockAudioProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "group", 1 }, "Group Mode", true));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "auto", 1 },  "Auto Align", true));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "ghost", 1 }, "Ghost Kick", true));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "stab", 1 },  "Stabilize", true));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "atrim", 1 }, "Auto Kick Trim", true));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "focus", 1 }, "Focus",
                 NormalisableRange<float> (40.0f, 100.0f, 0.1f), 55.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "phase", 1 }, "Manual Phase",
                 NormalisableRange<float> (0.0f, 360.0f, 0.5f), 0.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "target", 1 }, "Target",
                 NormalisableRange<float> (-90.0f, 90.0f, 0.5f), 0.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "thresh", 1 }, "Threshold",
                 NormalisableRange<float> (-70.0f, -20.0f, 0.5f), -45.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "settle", 1 }, "Settle",
                 NormalisableRange<float> (0.0f, 40.0f, 0.5f), 0.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "wet", 1 }, "Mix",
                 NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "trim", 1 }, "Kick Trim",
                 NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f));

    return { p.begin(), p.end() };
}

PsyPhaseLockAudioProcessor::PsyPhaseLockAudioProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",          juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output",         juce::AudioChannelSet::stereo(), true)
                        .withInput  ("Kick Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
}

bool PsyPhaseLockAudioProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto& out = l.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    if (out != l.getMainInputChannelSet()) return false;
    const auto& sc = l.getChannelSet (true, 1);
    return sc.isDisabled() || sc == juce::AudioChannelSet::mono() || sc == juce::AudioChannelSet::stereo();
}

void PsyPhaseLockAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const size_t cap = (size_t) std::max (samplesPerBlock, 8192);
    tapK.assign (cap, 0.0f); tapBi.assign (cap, 0.0f); tapBo.assign (cap, 0.0f);
    scope.reset();
    freeRunPpq = 0.0;
    aligner.prepare (sampleRate);
    setLatencySamples (aligner.getLatencySamples());   // 30 ms look-ahead, host compensates (PDC)
}

void PsyPhaseLockAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    auto mainBuf = getBusBuffer (buffer, true, 0);

    const auto* scBus = getBus (true, 1);
    const bool scOn = scBus != nullptr && scBus->isEnabled() && scBus->getNumberOfChannels() > 0;
    auto scBuf = scOn ? getBusBuffer (buffer, true, 1) : juce::AudioBuffer<float>();

    auto get = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };

    psy::Params p;
    p.groupMode   = get ("group") > 0.5f;
    p.autoAlign   = get ("auto")  > 0.5f;
    p.focusHz     = get ("focus");
    p.manualDeg   = get ("phase");
    p.targetDeg   = get ("target");
    p.thresholdDb = get ("thresh");
    p.settleMs    = get ("settle");
    p.wet         = get ("wet") * 0.01f;
    p.kickTrimDb  = get ("trim");

    p.ghostRef    = get ("ghost") > 0.5f;
    p.stabilize   = get ("stab")  > 0.5f;
    p.autoTrim    = get ("atrim") > 0.5f;
    p.hostBpm     = 0.0f;                     // tempo is detected from the kick itself

    const int n = mainBuf.getNumSamples();
    const bool wantScope = scopeActive.load (std::memory_order_relaxed) && n <= (int) tapK.size();
    psy::PhaseAligner::Tap tap;
    if (wantScope) { tap.kick = tapK.data(); tap.bassIn = tapBi.data(); tap.bassOut = tapBo.data(); }

    aligner.process (mainBuf.getArrayOfWritePointers(), mainBuf.getNumChannels(),
                     scOn ? scBuf.getArrayOfReadPointers() : nullptr, scOn ? scBuf.getNumChannels() : 0,
                     n, p, wantScope ? &tap : nullptr);

    if (wantScope)
    {
        // musical position of this block (host PPQ when the transport runs, free-running otherwise)
        double bpm = 0.0, ppqStart = freeRunPpq;
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
            {
                if (auto b = pos->getBpm()) bpm = *b;
                if (pos->getIsPlaying())
                    if (auto q = pos->getPpqPosition()) ppqStart = *q;
            }
        if (bpm < 20.0) bpm = lastBpm > 0.0 ? lastBpm : 174.0;
        lastBpm = bpm;
        hostBpm.store ((float) bpm);

        const double pps = bpm / (60.0 * getSampleRate());
        freeRunPpq = ppqStart + (double) n * pps;
        // the tapped signals are delayed by the look-ahead, so shift their timeline back by the latency
        scope.write (ppqStart - (double) aligner.getLatencySamples() * pps, pps, n,
                     tapK.data(), tapBi.data(), tapBo.data());
    }
}

juce::AudioProcessorEditor* PsyPhaseLockAudioProcessor::createEditor()
{
    return new PsyPhaseLockAudioProcessorEditor (*this);
}

void PsyPhaseLockAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void PsyPhaseLockAudioProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PsyPhaseLockAudioProcessor();
}
