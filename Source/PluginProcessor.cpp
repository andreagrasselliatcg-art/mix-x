#include "PluginProcessor.h"
#include "PluginEditor.h"

static juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "profile", 1 }, "Track Type", mx::profileNames(), 0));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "genre", 1 }, "Genre", mx::genreNames(), 1));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "daw", 1 }, "DAW", mx::dawNames(), 0));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "loudness", 1 }, "Loudness Target", mx::loudnessNames(), 0));
    return l;
}

MixXProcessor::MixXProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput ("Sidechain (Kick)", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "MixX", createLayout())
{
    refreshDiagnosis();
    startTimerHz (5);
}

MixXProcessor::~MixXProcessor()
{
    stopTimer();
    if (refLoader) refLoader->stopThread (5000);
}

void MixXProcessor::loadReference (const juce::File& file)
{
    if (refLoader) refLoader->stopThread (5000);
    refFile = file;
    refError.clear();
    refLoader = std::make_unique<RefLoader> (file);
    refLoader->startThread (juce::Thread::Priority::low);
}

void MixXProcessor::clearReference()
{
    if (refLoader) refLoader->stopThread (5000);
    refLoader.reset();
    refReport = {};
    refFile = juce::File();
    refError.clear();
    refreshDiagnosis();
}

bool MixXProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    if (in != out) return false;
    if (in != juce::AudioChannelSet::stereo() && in != juce::AudioChannelSet::mono()) return false;
    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.getChannelSet (true, 1);
        if (! sc.isDisabled() && sc != juce::AudioChannelSet::stereo() && sc != juce::AudioChannelSet::mono())
            return false;
    }
    return true;
}

void MixXProcessor::prepareToPlay (double sampleRate, int)
{
    analyzer.prepare (sampleRate);
}

void MixXProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n == 0) return;

    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto bpm = pos->getBpm())
                analyzer.setHostBpm (*bpm);

    auto main = getBusBuffer (buffer, true, 0);
    const float* l = main.getReadPointer (0);
    const float* r = main.getNumChannels() > 1 ? main.getReadPointer (1) : l;

    const float* sc = nullptr;
    if (getBusCount (true) > 1 && getBus (true, 1)->isEnabled())
    {
        auto side = getBusBuffer (buffer, true, 1);
        if (side.getNumChannels() > 0)
            sc = side.getReadPointer (0);
    }

    analyzer.push (l, r, sc, n);
    // audio passes through unchanged (main input and output share the same channels)
}

void MixXProcessor::refreshDiagnosis()
{
    const int genre = (int) apvts.getRawParameterValue ("genre")->load();
    const int loud = (int) apvts.getRawParameterValue ("loudness")->load();
    diagnosis = mx::diagnose (report, getProfile(), genre, getDaw(), loud, refReport.valid ? &refReport : nullptr);
    ++diagnosisGeneration;
}

void MixXProcessor::timerCallback()
{
    bool changed = false;

    if (refLoader != nullptr && refLoader->done.load())
    {
        refLoader->stopThread (1000);
        refReport = refLoader->result;
        refError = refLoader->error;
        if (! refReport.valid) refFile = juce::File();
        refLoader.reset();
        changed = true;
    }

    const int gen = analyzer.getReportGeneration();
    if (gen != lastReportGeneration)
    {
        lastReportGeneration = gen;
        report = analyzer.getReport();
        changed = true;
    }

    const int hash = (int) apvts.getRawParameterValue ("profile")->load() * 1000
                   + (int) apvts.getRawParameterValue ("genre")->load() * 100
                   + (int) apvts.getRawParameterValue ("daw")->load() * 10
                   + (int) apvts.getRawParameterValue ("loudness")->load();
    if (hash != lastSettingsHash)
    {
        lastSettingsHash = hash;
        changed = true;
    }

    if (changed)
        refreshDiagnosis();
}

juce::String MixXProcessor::getReportText() const
{
    return mx::reportToText (diagnosis, report, getProfile(), getDaw());
}

void MixXProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("reference", refFile.getFullPathName(), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void MixXProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            const juce::String ref = tree.getProperty ("reference").toString();
            apvts.replaceState (tree);
            if (ref.isNotEmpty() && juce::File::isAbsolutePath (ref) && juce::File (ref).existsAsFile())
                loadReference (juce::File (ref));
        }
}

juce::AudioProcessorEditor* MixXProcessor::createEditor()
{
    return new MixXEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MixXProcessor();
}
