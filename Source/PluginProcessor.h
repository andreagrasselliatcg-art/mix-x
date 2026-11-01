#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Analyzer.h"
#include "Diagnostics.h"

// Mix-X: analysis only. Audio passes through untouched.
class MixXProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    MixXProcessor();
    ~MixXProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override { analyzer.release(); }
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    mx::Analyzer analyzer;

    // message thread
    void startAnalysis (double seconds = 30.0) { analyzer.startLearning (seconds); }
    void stopAnalysis() { analyzer.stopLearning(); }
    const mx::Diagnosis& getDiagnosis() const { return diagnosis; }
    const mx::Report& getReport() const { return report; }
    int getDiagnosisGeneration() const { return diagnosisGeneration; }
    juce::String getReportText() const;
    void refreshDiagnosis();

    // reference track
    void loadReference (const juce::File& file);
    void clearReference();
    bool isReferenceLoading() const { return refLoader != nullptr && refLoader->isThreadRunning(); }
    const mx::Report& getReferenceReport() const { return refReport; }
    juce::File getReferenceFile() const { return refFile; }
    juce::String getReferenceError() const { return refError; }

    mx::Profile getProfile() const { return (mx::Profile) (int) apvts.getRawParameterValue ("profile")->load(); }
    mx::Daw getDaw() const { return (mx::Daw) (int) apvts.getRawParameterValue ("daw")->load(); }

private:
    void timerCallback() override;

    struct RefLoader : juce::Thread
    {
        RefLoader (juce::File f) : juce::Thread ("Mix-X reference"), file (std::move (f)) {}
        void run() override { result = analyser.analyseFile (file, [this] { return threadShouldExit(); }, error); done = true; }
        juce::File file;
        mx::Analyzer analyser;
        mx::Report result;
        juce::String error;
        std::atomic<bool> done { false };
    };
    std::unique_ptr<RefLoader> refLoader;
    mx::Report refReport;
    juce::File refFile;
    juce::String refError;

    mx::Report report;
    mx::Diagnosis diagnosis;
    int diagnosisGeneration = 0, lastReportGeneration = 0;
    int lastSettingsHash = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixXProcessor)
};
