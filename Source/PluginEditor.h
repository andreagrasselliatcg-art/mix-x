#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

namespace mxui
{
namespace col
{
    const juce::Colour bg     { 0xff101216 };
    const juce::Colour panel  { 0xff1a1d23 };
    const juce::Colour panel2 { 0xff22262e };
    const juce::Colour text   { 0xffe9ebf0 };
    const juce::Colour dim    { 0xff8d93a3 };
    const juce::Colour accent { 0xff29d391 };
    const juce::Colour orange { 0xffff8a3d };
}

juce::Colour categoryColour (mx::Category c);
juce::Colour severityColour (int s);

class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel();
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawComboBox (juce::Graphics&, int w, int h, bool, int, int, int, int, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override { return juce::Font (juce::FontOptions (13.0f, juce::Font::bold)); }
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return juce::Font (juce::FontOptions (13.0f, juce::Font::bold)); }
};

class SpectrumView : public juce::Component, private juce::Timer
{
public:
    explicit SpectrumView (MixXProcessor& p);
    void paint (juce::Graphics&) override;
private:
    void timerCallback() override;
    float xForFreq (float f) const;
    float yForDb (float d) const;
    static void bandCurve (const std::vector<float>& binDb, double sr, int points, std::vector<float>& out);

    MixXProcessor& proc;
    std::vector<float> live, liveCurve, learnedCurve, refCurve;
    int learnedGen = -1;
    juce::String refName;
};

class IssueList : public juce::Component
{
public:
    void setIssues (const std::vector<mx::Issue>& issues, int width);
private:
    void paint (juce::Graphics&) override;
    struct Block { juce::TextLayout title, why; std::vector<juce::TextLayout> steps; float height = 0; const mx::Issue* issue = nullptr; };
    std::vector<mx::Issue> items;
    std::vector<Block> blocks;
};

class HealthPanel : public juce::Component
{
public:
    explicit HealthPanel (MixXProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;
private:
    MixXProcessor& proc;
};
} // namespace mxui

class MixXEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit MixXEditor (MixXProcessor&);
    ~MixXEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void saveReport();
    void chooseReference();

    MixXProcessor& proc;
    mxui::LookAndFeel lnf;

    juce::ComboBox profileBox, genreBox, dawBox, loudBox;
    using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    std::unique_ptr<CA> profileAtt, genreAtt, dawAtt, loudAtt;
    juce::TextButton analyzeButton { "ANALYZE" }, copyButton { "COPY LIST" }, saveButton { "SAVE .TXT" },
                     referenceButton { "LOAD REFERENCE" }, clearRefButton { "X" };
    bool wasRefLoading = false;
    juce::Label statusLabel;

    mxui::SpectrumView spectrum;
    mxui::HealthPanel health;
    juce::Viewport viewport;
    mxui::IssueList list;
    std::unique_ptr<juce::FileChooser> chooser;
    int shownGeneration = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixXEditor)
};
