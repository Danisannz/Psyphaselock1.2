#include "PluginEditor.h"
#include "LogoData.h"
#include <cmath>

namespace
{
    const float kTwoPi = juce::MathConstants<float>::twoPi;
    const juce::Colour cMagenta (0xffff2bd6), cCyan (0xff00f5d4), cViolet (0xff7b2cff),
                       cOrange (0xffff9f1c), cDeep (0xff08030f);

    juce::String degStr (float v)
    {
        return juce::String ((int) std::lround (v)) + juce::String (juce::CharPointer_UTF8 ("\xc2\xb0"));
    }
}

//==============================================================================
PsyLookAndFeel::PsyLookAndFeel()
{
    setColour (juce::Label::textColourId, juce::Colour (0xffd9b8ff));
    setColour (juce::Slider::textBoxTextColourId, juce::Colours::white.withAlpha (0.9f));
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
}

void PsyLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                       float startAngle, float endAngle, juce::Slider& s)
{
    auto b = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (8.0f);
    const float r = std::min (b.getWidth(), b.getHeight()) * 0.5f;
    const auto  c = b.getCentre();
    const float a = startAngle + pos * (endAngle - startAngle);
    const bool  on = s.isEnabled();
    const float alpha = on ? 1.0f : 0.35f;

    // knob body
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3a1466).withAlpha (alpha), c.x, c.y - r,
                                             juce::Colour (0xff0b0418).withAlpha (alpha), c.x, c.y + r, false));
    g.fillEllipse (c.x - r * 0.74f, c.y - r * 0.74f, r * 1.48f, r * 1.48f);
    g.setColour (juce::Colours::white.withAlpha (0.10f * alpha));
    g.drawEllipse (c.x - r * 0.74f, c.y - r * 0.74f, r * 1.48f, r * 1.48f, 1.0f);

    // track
    juce::Path track;
    track.addCentredArc (c.x, c.y, r, r, 0.0f, startAngle, endAngle, true);
    g.setColour (juce::Colours::white.withAlpha (0.12f * alpha));
    g.strokePath (track, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // value arc (glow + core)
    juce::Path val;
    val.addCentredArc (c.x, c.y, r, r, 0.0f, startAngle, a, true);
    g.setColour (cMagenta.withAlpha (0.22f * alpha));
    g.strokePath (val, juce::PathStrokeType (9.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setGradientFill (juce::ColourGradient (cCyan.withAlpha (alpha), c.x - r, c.y,
                                             cMagenta.withAlpha (alpha), c.x + r, c.y, false));
    g.strokePath (val, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // pointer
    const float s1 = std::sin (a), c1 = std::cos (a);
    g.setColour (juce::Colours::white.withAlpha (alpha));
    g.drawLine (c.x + r * 0.25f * s1, c.y - r * 0.25f * c1, c.x + r * 0.62f * s1, c.y - r * 0.62f * c1, 2.5f);
}

void PsyLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& t, bool highlighted, bool)
{
    auto b = t.getLocalBounds().toFloat().reduced (2.0f);
    const bool on = t.getToggleState();
    if (on)
    {
        g.setColour (cMagenta.withAlpha (0.25f));
        g.fillRoundedRectangle (b.expanded (2.0f), b.getHeight() * 0.5f);
        g.setGradientFill (juce::ColourGradient (cMagenta, b.getX(), b.getY(), cViolet, b.getRight(), b.getBottom(), false));
    }
    else
        g.setColour (juce::Colour (0xff1a0b2e));
    g.fillRoundedRectangle (b, b.getHeight() * 0.5f);
    g.setColour (juce::Colours::white.withAlpha (highlighted ? 0.7f : 0.35f));
    g.drawRoundedRectangle (b, b.getHeight() * 0.5f, 1.2f);
    g.setColour (juce::Colours::white.withAlpha (on ? 1.0f : 0.6f));
    g.setFont (13.0f);
    g.drawText (t.getButtonText(), b.toNearestInt(), juce::Justification::centred);
}

//==============================================================================
ScopeComponent::ScopeComponent (PsyPhaseLockAudioProcessor& p) : proc (p)
{
    snap.assign ((size_t) (S * 3 * C * 2), 1.0e9f);
    setOpaque (false);
    setInterceptsMouseClicks (false, false);
    startTimerHz (24);
}

void ScopeComponent::takeSnapshot()
{
    snapCur = proc.scope.currentSlice();
    float peak = 0.0f;
    bool any = false;
    for (int s = 0; s < S; ++s)
        for (int g = 0; g < 3; ++g)
            for (int c = 0; c < C; ++c)
            {
                const float mn = proc.scope.get (s, g, c, 0), mx = proc.scope.get (s, g, c, 1);
                snap[(size_t) idx (s, g, c, 0)] = mn;
                snap[(size_t) idx (s, g, c, 1)] = mx;
                if (mn <= mx)
                {
                    any = true;
                    if (g != 1 || showBefore) peak = std::max (peak, std::max (std::abs (mn), std::abs (mx)));
                }
            }
    hasData = any;
    const float target = std::max (peak, 0.02f);
    scaleSm += (target - scaleSm) * (target > scaleSm ? 0.5f : 0.05f);
}

void ScopeComponent::timerCallback()
{
    if (frozen) return;
    takeSnapshot();
    repaint();
}

void ScopeComponent::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();

    // panel
    g.setColour (juce::Colour (0xe004020a));
    g.fillRoundedRectangle (b, 10.0f);
    g.setGradientFill (juce::ColourGradient (cMagenta.withAlpha (0.85f), b.getX(), b.getY(),
                                             cCyan.withAlpha (0.85f), b.getRight(), b.getBottom(), false));
    g.drawRoundedRectangle (b.reduced (0.5f), 10.0f, 1.4f);

    const auto  plot = b.reduced (12.0f, 0.0f).withTrimmedTop (26.0f).withTrimmedBottom (10.0f);
    const float cy   = plot.getCentreY();
    const float scale = (plot.getHeight() * 0.5f * 0.92f) / scaleSm;

    // grid: beat start, and the four 16ths  (1 e & a)
    static const char* names[4] = { "1", "e", "&", "a" };
    for (int q = 0; q <= 4; ++q)
    {
        const float x = plot.getX() + plot.getWidth() * (float) q / 4.0f;
        g.setColour (juce::Colours::white.withAlpha ((q == 0 || q == 4) ? 0.30f : 0.14f));
        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
        if (q < 4)
        {
            g.setColour (juce::Colour (0xffb79cff));
            g.setFont (12.0f);
            g.drawText (names[q], (int) x + 4, (int) b.getY() + 6, 20, 14, juce::Justification::left);
        }
    }
    g.setColour (juce::Colours::white.withAlpha (0.12f));
    g.drawHorizontalLine ((int) cy, plot.getX(), plot.getRight());

    // traces: oldest beat first, newest on top
    auto trace = [&] (int s, int sig)
    {
        juce::Path p;
        bool pen = false;
        for (int c = 0; c < C; ++c)
        {
            const float mn = snap[(size_t) idx (s, sig, c, 0)], mx = snap[(size_t) idx (s, sig, c, 1)];
            if (mn > mx) { pen = false; continue; }
            const float x = plot.getX() + plot.getWidth() * ((float) c + 0.5f) / (float) C;
            const float y = cy - 0.5f * (mn + mx) * scale;
            if (! pen) { p.startNewSubPath (x, y); pen = true; } else p.lineTo (x, y);
        }
        return p;
    };

    for (int age = S - 1; age >= 0; --age)
    {
        const int   s  = (snapCur - age + S) % S;
        const float a  = age == 0 ? 1.0f : 0.10f + 0.55f * (1.0f - (float) age / (float) S);
        const float th = age == 0 ? 1.8f : 1.1f;
        if (showBefore)
        {
            g.setColour (cOrange.withAlpha (a * 0.7f));
            g.strokePath (trace (s, 1), juce::PathStrokeType (th));
        }
        g.setColour (cMagenta.withAlpha (a));
        g.strokePath (trace (s, 0), juce::PathStrokeType (th));
        const auto bp = trace (s, 2);
        if (age == 0)
        {
            g.setColour (cCyan.withAlpha (0.18f));
            g.strokePath (bp, juce::PathStrokeType (6.0f));
        }
        g.setColour (cCyan.withAlpha (a));
        g.strokePath (bp, juce::PathStrokeType (th));
    }

    // status line
    auto& m = proc.aligner.meters;
    const int   lk    = m.locked.load();
    const float delta = m.deltaDeg.load(), applied = m.appliedDeg.load();
    juce::String st = lk == 1 ? "LOCK" : lk == 2 ? "LOCK (short note)" : "WAIT";
    g.setColour (lk == 1 ? juce::Colour (0xffb8ff5a) : lk == 2 ? cOrange : juce::Colours::white.withAlpha (0.6f));
    g.setFont (13.0f);
    g.drawText (st + "   " + degStr (delta) + "  ->  " + degStr (delta - applied) + "     "
                  + juce::String (proc.hostBpm.load(), 1) + " BPM",
                (int) b.getX() + 60, (int) b.getY() + 5, (int) b.getWidth() - 72, 16, juce::Justification::right);

    g.setFont (12.0f);
    g.setColour (cMagenta);  g.drawText ("KICK", (int) b.getRight() - 300, (int) b.getBottom() - 18, 50, 14, juce::Justification::left);
    g.setColour (cCyan);     g.drawText ("BASS", (int) b.getRight() - 250, (int) b.getBottom() - 18, 50, 14, juce::Justification::left);
    if (showBefore)
    {
        g.setColour (cOrange);
        g.drawText ("BASS (before)", (int) b.getRight() - 200, (int) b.getBottom() - 18, 100, 14, juce::Justification::left);
    }

    // groove detector: STRAIGHT / TENSION / FILL, the 4 sixteenth slots of the detected pattern, % of beats running in 16ths
    {
        const int rs = m.rhythmState.load(), pct = m.sixteenthPct.load(), mask = m.patternMask.load();
        const juce::String name = rs == 1 ? "STRAIGHT" : rs == 2 ? "TENSION" : rs == 3 ? "FILL" : "LISTENING";
        const juce::Colour col  = rs == 1 ? juce::Colour (0xffb8ff5a) : rs == 2 ? cOrange : rs == 3 ? cMagenta
                                                                                                   : juce::Colours::white.withAlpha (0.5f);
        const int gx = (int) b.getX() + 14, gy = (int) b.getBottom() - 19;
        g.setFont (13.0f);
        g.setColour (col);
        g.drawText (name, gx, gy, 84, 14, juce::Justification::left);
        for (int q = 0; q < 4; ++q)
        {
            const float sx = (float) (gx + 88 + q * 15);
            g.setColour (((mask >> q) & 1) != 0 && rs != 0 ? col : juce::Colours::white.withAlpha (0.14f));
            g.fillRoundedRectangle (sx, (float) gy + 2.0f, 11.0f, 11.0f, 3.0f);
        }
        g.setColour (juce::Colours::white.withAlpha (0.75f));
        g.setFont (12.0f);
        g.drawText (rs == 0 ? juce::String() : juce::String (pct) + "% 16ths" + (pct >= 60 ? "  RUNNING" : ""),
                    gx + 154, gy, 130, 14, juce::Justification::left);
    }

    // hints
    g.setColour (juce::Colours::white.withAlpha (0.55f));
    g.setFont (14.0f);
    if (proc.aligner.meters.kickDb.load() < -80.0f)
        g.drawText ("connect a copy of the kick to the plugin's sidechain input", plot.toNearestInt(), juce::Justification::centred);
    else if (! hasData)
        g.drawText ("play the transport", plot.toNearestInt(), juce::Justification::centred);
}

//==============================================================================
PsyPhaseLockAudioProcessorEditor::PsyPhaseLockAudioProcessorEditor (PsyPhaseLockAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p), scope (p)
{
    setLookAndFeel (&laf);
    logoImg = juce::ImageFileFormat::loadFrom (logo::data, (size_t) logo::size);
    setSize (780, 470);

    initKnob (kFocus,  "focus",  "FOCUS",     " Hz");
    initKnob (kTarget, "target", "TARGET",    juce::String (juce::CharPointer_UTF8 (" \xc2\xb0")));
    initKnob (kPhase,  "phase",  "MANUAL",    juce::String (juce::CharPointer_UTF8 (" \xc2\xb0")));
    initKnob (kThresh, "thresh", "THRESHOLD", " dB");
    initKnob (kSettle, "settle", "SETTLE",    " ms");
    initKnob (kWet,    "wet",    "MIX",       " %");
    initKnob (kTrim,   "trim",   "KICK TRIM", " dB");

    for (auto* t : { &bGroup, &bAuto, &bGhost, &bStab, &bTrim, &bBefore, &bFreeze }) addAndMakeVisible (*t);
    aGroup = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "group", bGroup);
    aAuto  = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "auto",  bAuto);
    aGhost = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "ghost", bGhost);
    aStab  = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "stab",  bStab);
    aTrim  = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "atrim", bTrim);
    bAuto.onClick   = [this] { updateEnabledStates(); };
    bGroup.onClick  = [this] { updateEnabledStates(); };
    bBefore.onClick = [this] { scope.setShowBefore (bBefore.getToggleState()); };
    bFreeze.onClick = [this] { scope.setFrozen (bFreeze.getToggleState()); };
    updateEnabledStates();

    addAndMakeVisible (scope);
    proc.scopeActive.store (true);            // start capturing waveforms only while the editor is open
}

PsyPhaseLockAudioProcessorEditor::~PsyPhaseLockAudioProcessorEditor()
{
    proc.scopeActive.store (false);
    setLookAndFeel (nullptr);
}

void PsyPhaseLockAudioProcessorEditor::initKnob (Knob& k, const juce::String& id, const juce::String& text,
                                                 const juce::String& suffix)
{
    k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 78, 16);
    k.slider.setTextValueSuffix (suffix);
    addAndMakeVisible (k.slider);
    k.label.setText (text, juce::dontSendNotification);
    k.label.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (k.label);
    k.att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id, k.slider);
}

void PsyPhaseLockAudioProcessorEditor::updateEnabledStates()
{
    const bool autoOn = bAuto.getToggleState();
    kPhase.slider.setEnabled (! autoOn);            // manual phase only when AUTO is off
    kTarget.slider.setEnabled (autoOn);
    kSettle.slider.setEnabled (autoOn);
    kThresh.slider.setEnabled (autoOn);
    bGhost.setEnabled (autoOn);
    bStab.setEnabled (autoOn);
    bTrim.setEnabled (bGroup.getToggleState());
    kTrim.slider.setEnabled (bGroup.getToggleState());
}

void PsyPhaseLockAudioProcessorEditor::resized()
{
    const int W = getWidth(), H = getHeight();

    // cached static background: gradient + mandala + title
    bg = juce::Image (juce::Image::ARGB, W, H, true);
    {
        juce::Graphics g (bg);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1c0638), 0.0f, 0.0f, cDeep, (float) W, (float) H, false));
        g.fillAll();
        const float cx = W * 0.5f, cy = H * 0.55f;
        for (int layer = 0; layer < 7; ++layer)
        {
            const int petals = 12;
            for (int i = 0; i < petals; ++i)
            {
                juce::Path pth;
                const float len = 60.0f + 38.0f * (float) layer;
                pth.addEllipse (-len * 0.16f, -len, len * 0.32f, len);
                pth.applyTransform (juce::AffineTransform::rotation (kTwoPi * (float) i / (float) petals + 0.15f * (float) layer)
                                        .translated (cx, cy));
                g.setColour (juce::Colour::fromHSV (std::fmod (0.75f + 0.07f * (float) layer + 0.02f * (float) i, 1.0f), 0.8f, 1.0f, 0.08f));
                g.strokePath (pth, juce::PathStrokeType (1.1f));
            }
        }
        g.setGradientFill (juce::ColourGradient (cMagenta, 0.0f, 0.0f, cCyan, (float) W, 0.0f, false));
        g.setFont (28.0f);
        g.drawText ("PSYPHASE  LOCK", 0, 8, W, 34, juce::Justification::centred);
    }

    // buttons row
    const int bw = 84, bh = 26, gap = 8;                    // starts right of the logo (x > 100)
    int bx = 108;
    for (auto* t : { &bGroup, &bAuto, &bGhost, &bStab, &bTrim, &bBefore, &bFreeze })
    {
        t->setBounds (bx, 48, bw, bh);
        bx += bw + gap;
    }

    // oscilloscope
    scope.setBounds (20, 84, W - 40, 220);

    // knob row
    const int kw = 104, lh = 16, kh = 112, y0 = 316;
    int kx = (W - (7 * kw + 6 * 2)) / 2;
    for (auto* k : { &kFocus, &kTarget, &kPhase, &kThresh, &kSettle, &kWet, &kTrim })
    {
        k->label.setBounds (kx, y0, kw, lh);
        k->slider.setBounds (kx, y0 + lh, kw, kh);
        kx += kw + 2;
    }
}

void PsyPhaseLockAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.drawImageAt (bg, 0, 0);

    // Dani San logo, top-left corner (aspect ratio kept)
    if (logoImg.isValid())
    {
        const juce::Rectangle<float> area (14.0f, 4.0f, 82.0f, 78.0f);
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.drawImage (logoImg, area, juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize);
    }

    g.setColour (juce::Colours::white.withAlpha (0.45f));
    g.setFont (12.0f);
    g.drawText ("v1.2  |  Dani San", getWidth() - 160, 10, 146, 14, juce::Justification::right);
}
