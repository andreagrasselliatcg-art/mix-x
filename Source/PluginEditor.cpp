#include "PluginEditor.h"

namespace mxui
{
juce::Colour categoryColour (mx::Category c)
{
    switch (c)
    {
        case mx::Category::EQ:          return juce::Colour (0xff4fa3ff);
        case mx::Category::Compression: return juce::Colour (0xffffb020);
        case mx::Category::Sidechain:   return juce::Colour (0xffff4f8b);
        case mx::Category::Saturation:  return juce::Colour (0xffff7a30);
        case mx::Category::Stereo:      return juce::Colour (0xffa875ff);
        case mx::Category::Loudness:    return juce::Colour (0xff3fd1c0);
        case mx::Category::Info:        return juce::Colour (0xff8a90a2);
    }
    return col::dim;
}

juce::Colour severityColour (int s)
{
    return s >= 3 ? juce::Colour (0xffff4a4a) : s == 2 ? juce::Colour (0xffff9a3d) : s == 1 ? juce::Colour (0xfff2d24b) : col::accent;
}

//==============================================================================
LookAndFeel::LookAndFeel()
{
    setColour (juce::ComboBox::backgroundColourId, col::panel2);
    setColour (juce::ComboBox::textColourId, col::text);
    setColour (juce::ComboBox::arrowColourId, col::accent);
    setColour (juce::PopupMenu::backgroundColourId, col::panel2);
    setColour (juce::PopupMenu::textColourId, col::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, col::accent);
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::black);
    setColour (juce::TextButton::buttonColourId, col::panel2);
    setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffe8484a));
    setColour (juce::TextButton::textColourOffId, col::text);
    setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    setColour (juce::Label::textColourId, col::dim);
    setColour (juce::ScrollBar::thumbColourId, col::dim.withAlpha (0.5f));
}

void LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& c, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    auto colour = c;
    if (over) colour = colour.brighter (0.1f);
    if (down) colour = colour.darker (0.1f);
    g.setColour (colour);
    g.fillRoundedRectangle (r, 6.0f);
}

void LookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox&)
{
    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h);
    g.setColour (col::panel2);
    g.fillRoundedRectangle (r, 6.0f);
    juce::Path arrow;
    const float ax = (float) w - 14.0f, ay = (float) h * 0.5f;
    arrow.addTriangle (ax - 4, ay - 2, ax + 4, ay - 2, ax, ay + 3);
    g.setColour (col::accent);
    g.fillPath (arrow);
}

//==============================================================================
SpectrumView::SpectrumView (MixXProcessor& p) : proc (p)
{
    startTimerHz (30);
}

// 1/3-octave-wide energy around each display frequency (same scale as the targets)
void SpectrumView::bandCurve (const std::vector<float>& binDb, double sr, int points, std::vector<float>& out)
{
    out.assign ((size_t) points, -120.0f);
    if (binDb.empty()) return;
    std::vector<double> prefix (binDb.size() + 1, 0.0);
    for (size_t i = 0; i < binDb.size(); ++i)
        prefix[i + 1] = prefix[i] + std::pow (10.0, binDb[i] / 10.0);
    const double binHz = sr / (double) ((binDb.size() - 1) * 2);
    for (int i = 0; i < points; ++i)
    {
        const double f = 20.0 * std::pow (1000.0, (double) i / (points - 1));
        int lo = (int) (f / std::pow (2.0, 1.0 / 6.0) / binHz), hi = (int) (f * std::pow (2.0, 1.0 / 6.0) / binHz);
        lo = juce::jlimit (1, (int) binDb.size() - 1, lo);
        hi = juce::jlimit (lo, (int) binDb.size() - 1, hi);
        out[(size_t) i] = (float) (10.0 * std::log10 (prefix[(size_t) hi + 1] - prefix[(size_t) lo] + 1e-20));
    }
}

void SpectrumView::timerCallback()
{
    proc.analyzer.getLiveSpectrum (live);
    const double sr = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;
    std::vector<float> fresh;
    bandCurve (live, sr, 240, fresh);
    if (liveCurve.size() != fresh.size()) liveCurve = fresh;
    for (size_t i = 0; i < fresh.size(); ++i)
        liveCurve[i] = 0.6f * liveCurve[i] + 0.4f * fresh[i];

    if (learnedGen != proc.getDiagnosisGeneration())
    {
        learnedGen = proc.getDiagnosisGeneration();
        const auto& r = proc.getReport();
        if (r.valid) bandCurve (r.spectrumDb, r.sampleRate, 240, learnedCurve);
        else learnedCurve.clear();

        const auto& ref = proc.getReferenceReport();
        if (ref.valid)
        {
            bandCurve (ref.spectrumDb, ref.sampleRate, 240, refCurve);
            refName = proc.getReferenceFile().getFileNameWithoutExtension() + "  (" + juce::String (ref.integratedLufs, 1) + " LUFS)";
        }
        else
        {
            refCurve.clear();
            refName.clear();
        }
    }
    repaint();
}

float SpectrumView::xForFreq (float f) const
{
    return 50.0f + (float) (getWidth() - 62) * std::log10 (f / 20.0f) / 3.0f;
}

float SpectrumView::yForDb (float d) const
{
    const float top = 12.0f, bottom = (float) getHeight() - 24.0f;
    return juce::jmap (juce::jlimit (-84.0f, 0.0f, d), 0.0f, -84.0f, top, bottom);
}

void SpectrumView::paint (juce::Graphics& g)
{
    g.setColour (col::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 10.0f);

    g.setFont (juce::FontOptions (10.0f));
    for (float f : { 20.f, 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f, 20000.f })
    {
        const float x = xForFreq (f);
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.drawVerticalLine ((int) x, 12.0f, (float) getHeight() - 24.0f);
        g.setColour (col::dim);
        g.drawText (f >= 1000 ? juce::String ((int) (f / 1000)) + "k" : juce::String ((int) f), (int) x - 20, getHeight() - 20, 40, 14, juce::Justification::centred);
    }
    for (int d = 0; d >= -84; d -= 12)
    {
        const float y = yForDb ((float) d);
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.drawHorizontalLine ((int) y, 50.0f, (float) getWidth() - 12.0f);
        g.setColour (col::dim);
        g.drawText (juce::String (d), 8, (int) y - 7, 36, 14, juce::Justification::centredRight);
    }

    auto curvePath = [&] (const std::vector<float>& c, bool closed)
    {
        juce::Path p;
        for (size_t i = 0; i < c.size(); ++i)
        {
            const float f = 20.0f * std::pow (1000.0f, (float) i / (float) (c.size() - 1));
            const float x = xForFreq (f), y = yForDb (c[i]);
            if (i == 0) p.startNewSubPath (x, closed ? yForDb (-200) : y);
            if (i == 0 && closed) p.lineTo (x, y); else p.lineTo (x, y);
        }
        if (closed) { p.lineTo (xForFreq (20000), yForDb (-200)); p.closeSubPath(); }
        return p;
    };

    // reference track: red, level-matched to the song so you compare tonal balance
    if (! refCurve.empty())
    {
        const auto& songCurve = ! learnedCurve.empty() ? learnedCurve : liveCurve;
        auto meanIn = [] (const std::vector<float>& c, float lo, float hi)
        {
            double s = 0; int n = 0;
            for (size_t i = 0; i < c.size(); ++i)
            {
                const float f = 20.0f * std::pow (1000.0f, (float) i / (float) (c.size() - 1));
                if (f >= lo && f <= hi && c[i] > -110.0f) { s += c[i]; ++n; }
            }
            return n > 0 ? (float) (s / n) : 0.0f;
        };
        const float songMean = songCurve.empty() ? 0.0f : meanIn (songCurve, 100, 5000);
        const float shift = (songCurve.empty() || songMean < -100.0f) ? 0.0f : songMean - meanIn (refCurve, 100, 5000);
        std::vector<float> shifted (refCurve);
        for (auto& v : shifted) v += shift;

        const juce::Colour red (0xffff3b3b);
        g.setGradientFill (juce::ColourGradient (red.withAlpha (0.30f), 0, 12, red.withAlpha (0.04f), 0, (float) getHeight(), false));
        g.fillPath (curvePath (shifted, true));
        g.setColour (red.withAlpha (0.9f));
        g.strokePath (curvePath (shifted, false), juce::PathStrokeType (1.8f));
    }

    if (! liveCurve.empty())
    {
        g.setGradientFill (juce::ColourGradient (col::accent.withAlpha (0.35f), 0, 12, col::accent.withAlpha (0.03f), 0, (float) getHeight(), false));
        g.fillPath (curvePath (liveCurve, true));
        g.setColour (col::accent.withAlpha (0.7f));
        g.strokePath (curvePath (liveCurve, false), juce::PathStrokeType (1.2f));
    }

    const auto& d = proc.getDiagnosis();
    if (d.valid && ! d.usedReference)
    {
        // target (aligned to measured level)
        juce::Path t;
        const auto& fc = mx::thirdOctaveCentres();
        for (int i = 0; i < mx::kThirds; ++i)
        {
            const float x = xForFreq (fc[(size_t) i]), y = yForDb (d.targetThirdsDb[(size_t) i]);
            if (i == 0) t.startNewSubPath (x, y); else t.lineTo (x, y);
        }
        juce::Path dashed;
        const float dashes[] = { 6.0f, 5.0f };
        juce::PathStrokeType (1.6f).createDashedStroke (dashed, t, dashes, 2);
        g.setColour (juce::Colours::white.withAlpha (0.65f));
        g.fillPath (dashed);
    }

    if (d.valid)
    {
        if (! learnedCurve.empty())
        {
            g.setColour (col::orange);
            g.strokePath (curvePath (learnedCurve, false), juce::PathStrokeType (2.2f));
        }

        for (const auto& is : d.issues)
            if (is.freqHz > 0.0f)
            {
                const float x = xForFreq (is.freqHz);
                g.setColour (severityColour (is.severity).withAlpha (0.8f));
                g.fillEllipse (x - 5, (float) getHeight() - 38.0f, 10, 10);
                g.drawVerticalLine ((int) x, 30.0f, (float) getHeight() - 38.0f);
            }
    }

    // legend
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    auto leg = [&] (int x, juce::Colour c, const char* t) { g.setColour (c); g.fillRect (x, 18, 14, 3); g.setColour (col::dim); g.drawText (t, x + 18, 12, 140, 14, juce::Justification::centredLeft); };
    leg (70, col::accent, "LIVE");
    leg (140, col::orange, "ANALYSIS (avg)");
    if (refName.isEmpty())
        leg (270, juce::Colours::white.withAlpha (0.65f), "TARGET");
    else
    {
        g.setColour (juce::Colour (0xffff3b3b)); g.fillRect (270, 18, 14, 3);
        g.setColour (col::text);
        g.drawText ("REFERENCE: " + refName, 288, 12, 520, 14, juce::Justification::centredLeft);
    }
}

//==============================================================================
void IssueList::setIssues (const std::vector<mx::Issue>& issues, int width)
{
    items = issues;
    blocks.clear();
    const float textW = (float) width - 40.0f;
    float y = 0;

    for (auto& is : items)
    {
        Block b;
        b.issue = &is;
        juce::AttributedString t;
        t.append (is.title, juce::Font (juce::FontOptions (15.5f, juce::Font::bold)), col::text);
        t.setWordWrap (juce::AttributedString::byWord);
        b.title.createLayout (t, textW - 150.0f);

        juce::AttributedString w;
        w.append (is.why, juce::Font (juce::FontOptions (13.0f)), col::dim);
        b.why.createLayout (w, textW);

        float h = 14 + juce::jmax (22.0f, b.title.getHeight()) + 6 + b.why.getHeight() + 8;
        int n = 1;
        for (auto& s : is.steps)
        {
            juce::AttributedString a;
            a.append (juce::String (n++) + "   ", juce::Font (juce::FontOptions (13.0f, juce::Font::bold)), col::accent);
            a.append (s, juce::Font (juce::FontOptions (13.0f)), col::text);
            juce::TextLayout l;
            l.createLayout (a, textW - 10.0f);
            h += l.getHeight() + 5;
            b.steps.push_back (std::move (l));
        }
        b.height = h + 10;
        y += b.height + 10;
        blocks.push_back (std::move (b));
    }
    for (size_t i = 0; i < blocks.size(); ++i) blocks[i].issue = &items[i];
    setSize (width, juce::jmax (10, (int) y));
    repaint();
}

void IssueList::paint (juce::Graphics& g)
{
    float y = 0;
    const float w = (float) getWidth();
    for (auto& b : blocks)
    {
        const auto& is = *b.issue;
        auto card = juce::Rectangle<float> (0, y, w - 4, b.height);
        g.setColour (col::panel);
        g.fillRoundedRectangle (card, 8.0f);
        g.setColour (severityColour (is.severity));
        g.fillRoundedRectangle (card.withWidth (5.0f), 2.5f);

        float cy = y + 14;
        // chip
        const auto cat = mx::categoryName (is.category);
        auto chip = juce::Rectangle<float> (w - 150, cy, 130, 22);
        g.setColour (categoryColour (is.category).withAlpha (0.18f));
        g.fillRoundedRectangle (chip, 11.0f);
        g.setColour (categoryColour (is.category));
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (cat, chip, juce::Justification::centred);

        b.title.draw (g, { 20.0f, cy, w - 190.0f, b.title.getHeight() });
        cy += juce::jmax (22.0f, b.title.getHeight()) + 6;
        b.why.draw (g, { 20.0f, cy, w - 40.0f, b.why.getHeight() });
        cy += b.why.getHeight() + 8;
        for (auto& s : b.steps)
        {
            s.draw (g, { 28.0f, cy, w - 50.0f, s.getHeight() });
            cy += s.getHeight() + 5;
        }
        y += b.height + 10;
    }

    if (blocks.empty())
    {
        g.setColour (col::dim);
        g.setFont (juce::FontOptions (14.0f));
        g.drawText ("No issues found.", getLocalBounds(), juce::Justification::centred);
    }
}

//==============================================================================
void HealthPanel::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour (col::panel);
    g.fillRoundedRectangle (b, 10.0f);

    const auto& d = proc.getDiagnosis();
    const auto& r = proc.getReport();

    // score ring
    auto ring = juce::Rectangle<float> (b.getCentreX() - 70, 22, 140, 140);
    g.setColour (col::panel2);
    g.drawEllipse (ring.reduced (6), 10.0f);
    if (d.valid)
    {
        const float frac = (float) d.score / 100.0f;
        const auto c = d.score >= 80 ? col::accent : d.score >= 55 ? severityColour (1) : d.score >= 35 ? severityColour (2) : severityColour (3);
        juce::Path arc;
        arc.addCentredArc (ring.getCentreX(), ring.getCentreY(), 64, 64, 0, 0, juce::MathConstants<float>::twoPi * frac, true);
        g.setColour (c);
        g.strokePath (arc, juce::PathStrokeType (10.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (col::text);
        g.setFont (juce::FontOptions (44.0f, juce::Font::bold));
        g.drawText (juce::String (d.score), ring.translated (0, -8), juce::Justification::centred);
        g.setColour (col::dim);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText ("MIX HEALTH", ring.translated (0, 30), juce::Justification::centred);
    }
    else
    {
        g.setColour (col::dim);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText ("ANALYZE", ring, juce::Justification::centred);
    }

    // metrics
    float y = 186;
    auto row = [&] (const juce::String& k, const juce::String& v, juce::Colour c = col::text)
    {
        g.setFont (juce::FontOptions (12.0f));
        g.setColour (col::dim);
        g.drawText (k, 18, (int) y, 160, 18, juce::Justification::centredLeft);
        g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
        g.setColour (c);
        g.drawText (v, (int) b.getWidth() - 140, (int) y, 122, 18, juce::Justification::centredRight);
        y += 22;
    };

    row ("Momentary (live)", juce::String (proc.analyzer.liveMomentary.load(), 1) + " LUFS");
    row ("Short-term (live)", juce::String (proc.analyzer.liveShortTerm.load(), 1) + " LUFS");
    row ("Correlation (live)", juce::String (proc.analyzer.liveCorrelation.load(), 2));
    y += 8;
    if (r.valid)
    {
        row ("Integrated", juce::String (r.integratedLufs, 1) + " LUFS", col::orange);
        row ("Peak", juce::String (r.peakDb, 1) + " dBFS", r.peakDb > -1.0f ? severityColour (2) : col::text);
        row ("Crest factor", juce::String (r.crestDb, 1) + " dB");
        row ("Dynamics (P90-P10)", juce::String (r.dynRangeDb, 1) + " dB");
        auto stereoText = [] (float v) { return v < -60.0f ? juce::String ("mono") : juce::String (v, 1) + " dB"; };
        row ("Stereo (Side/Mid)", stereoText (r.sideToMidDb));
        row ("Low end stereo", stereoText (r.lowSideToMidDb), r.lowSideToMidDb > -15 ? severityColour (2) : col::text);
        if (r.sidechainActive)
            row ("Ducking on kick", juce::String (r.existingDuckDb, 1) + " dB");
        row ("Analyzed", juce::String (r.seconds, 1) + " s" + (r.bpm > 0 ? "  @ " + juce::String (r.bpm, 0) + " BPM" : juce::String()));
    }
}
} // namespace mxui

//==============================================================================
MixXEditor::MixXEditor (MixXProcessor& p)
    : AudioProcessorEditor (&p), proc (p), spectrum (p), health (p)
{
    setLookAndFeel (&lnf);

    profileBox.addItemList (mx::profileNames(), 1);
    genreBox.addItemList (mx::genreNames(), 1);
    dawBox.addItemList (mx::dawNames(), 1);
    loudBox.addItemList (mx::loudnessNames(), 1);
    profileAtt = std::make_unique<CA> (proc.apvts, "profile", profileBox);
    genreAtt = std::make_unique<CA> (proc.apvts, "genre", genreBox);
    dawAtt = std::make_unique<CA> (proc.apvts, "daw", dawBox);
    loudAtt = std::make_unique<CA> (proc.apvts, "loudness", loudBox);

    analyzeButton.setColour (juce::TextButton::buttonColourId, mxui::col::accent);
    analyzeButton.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
    analyzeButton.onClick = [this]
    {
        if (proc.analyzer.isLearning()) proc.stopAnalysis();
        else proc.startAnalysis (30.0);
    };
    copyButton.onClick = [this] { juce::SystemClipboard::copyTextToClipboard (proc.getReportText()); statusLabel.setText ("List copied to clipboard", juce::dontSendNotification); };
    saveButton.onClick = [this] { saveReport(); };

    referenceButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffb32626));
    referenceButton.setTooltip ("Load a finished track you like: its spectrum appears in red and Mix-X compares your mix to it");
    referenceButton.onClick = [this] { chooseReference(); };
    clearRefButton.setTooltip ("Remove the reference track");
    clearRefButton.onClick = [this] { proc.clearReference(); referenceButton.setButtonText ("LOAD REFERENCE"); };

    for (auto* c : std::initializer_list<juce::Component*> { &profileBox, &genreBox, &dawBox, &loudBox, &analyzeButton, &referenceButton, &clearRefButton, &copyButton, &saveButton, &statusLabel, &spectrum, &health, &viewport })
        addAndMakeVisible (*c);

    statusLabel.setFont (juce::FontOptions (12.0f));
    viewport.setViewedComponent (&list, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (8);

    setSize (1240, 830);
    sendLookAndFeelChange();
    startTimerHz (10);
}

MixXEditor::~MixXEditor()
{
    setLookAndFeel (nullptr);
}

void MixXEditor::saveReport()
{
    auto start = juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile ("Mix-X report.txt");
    chooser = std::make_unique<juce::FileChooser> ("Save report", start, "*.txt");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (f == juce::File()) return;
                              f.withFileExtension ("txt").replaceWithText (proc.getReportText());
                              statusLabel.setText ("Report saved: " + f.getFileName(), juce::dontSendNotification);
                          });
}

void MixXEditor::chooseReference()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose a reference track",
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.caf");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f.existsAsFile()) proc.loadReference (f);
                          });
}

void MixXEditor::timerCallback()
{
    const bool refLoading = proc.isReferenceLoading();
    if (refLoading)
    {
        referenceButton.setButtonText ("ANALYZING REF...");
        statusLabel.setText ("Analyzing reference: " + proc.getReferenceFile().getFileName(), juce::dontSendNotification);
    }
    else if (wasRefLoading || (proc.getReferenceReport().valid && referenceButton.getButtonText() == "LOAD REFERENCE"))
    {
        const auto& ref = proc.getReferenceReport();
        referenceButton.setButtonText (ref.valid ? "REF: " + proc.getReferenceFile().getFileNameWithoutExtension() : juce::String ("LOAD REFERENCE"));
        if (wasRefLoading)
            statusLabel.setText (ref.valid ? "Reference loaded: your mix is now compared to it"
                                           : "Could not load the reference: " + proc.getReferenceError(), juce::dontSendNotification);
    }
    wasRefLoading = refLoading;
    clearRefButton.setVisible (proc.getReferenceReport().valid);

    const bool learning = proc.analyzer.isLearning();
    if (learning)
    {
        analyzeButton.setButtonText ("STOP  " + juce::String (proc.analyzer.getLearnedSeconds(), 0) + "s");
        analyzeButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffe8484a));
        statusLabel.setText ("Analyzing: play your track (stops automatically at 30 s, 10 s minimum recommended)", juce::dontSendNotification);
    }
    else if (analyzeButton.getButtonText() != "ANALYZE")
    {
        analyzeButton.setButtonText ("ANALYZE");
        analyzeButton.setColour (juce::TextButton::buttonColourId, mxui::col::accent);
        statusLabel.setText ("Analysis complete", juce::dontSendNotification);
    }

    if (shownGeneration != proc.getDiagnosisGeneration())
    {
        shownGeneration = proc.getDiagnosisGeneration();
        list.setIssues (proc.getDiagnosis().issues, viewport.getWidth() - 10);
    }
    health.repaint();
}

void MixXEditor::paint (juce::Graphics& g)
{
    g.fillAll (mxui::col::bg);
    g.setColour (mxui::col::text);
    g.setFont (juce::FontOptions (32.0f, juce::Font::bold | juce::Font::italic));
    g.drawText ("MIX", 18, 8, 70, 40, juce::Justification::centredLeft);
    g.setColour (mxui::col::accent);
    g.drawText ("-X", 74, 8, 60, 40, juce::Justification::centredLeft);
    g.setColour (mxui::col::dim);
    g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
    g.drawText ("TUNE TALKS  |  MIX DOCTOR", 20, 46, 200, 14, juce::Justification::centredLeft);

    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    const char* labels[] = { "TRACK TYPE", "GENRE", "DAW", "LOUDNESS TARGET" };
    juce::Component* boxes[] = { &profileBox, &genreBox, &dawBox, &loudBox };
    for (int i = 0; i < 4; ++i)
        g.drawText (labels[i], boxes[i]->getX(), boxes[i]->getY() - 14, 150, 12, juce::Justification::centredLeft);

    g.setColour (mxui::col::text);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText ("WHAT TO IMPROVE AND HOW", viewport.getX(), viewport.getY() - 22, 400, 18, juce::Justification::centredLeft);
}

void MixXEditor::resized()
{
    int x = 230;
    for (auto* b : { &profileBox, &genreBox, &dawBox, &loudBox })
    {
        b->setBounds (x, 26, b == &loudBox ? 170 : 150, 32);
        x += b == &loudBox ? 180 : 160;
    }
    referenceButton.setBounds (902, 16, 150, 44);
    clearRefButton.setBounds (1056, 24, 28, 28);
    analyzeButton.setBounds (1092, 16, getWidth() - 1092 - 12, 44);

    spectrum.setBounds (12, 74, getWidth() - 24, 250);
    health.setBounds (12, 336, 300, getHeight() - 336 - 40);
    viewport.setBounds (326, 358, getWidth() - 338, getHeight() - 358 - 40);

    auto bottom = juce::Rectangle<int> (12, getHeight() - 34, getWidth() - 24, 28);
    copyButton.setBounds (bottom.removeFromRight (120));
    bottom.removeFromRight (8);
    saveButton.setBounds (bottom.removeFromRight (120));
    bottom.removeFromRight (12);
    statusLabel.setBounds (bottom);

    list.setIssues (proc.getDiagnosis().issues, viewport.getWidth() - 10);
}
