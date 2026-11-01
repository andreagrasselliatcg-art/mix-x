#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <functional>
#include <array>
#include <atomic>
#include <vector>

namespace mx
{
constexpr int kFftOrder = 13;
constexpr int kFftSize = 1 << kFftOrder;   // 8192
constexpr int kHop = 2048;
constexpr int kBins = kFftSize / 2 + 1;
constexpr int kThirds = 31;

const std::array<float, kThirds>& thirdOctaveCentres();

// Everything learned during an ANALYZE pass
struct Report
{
    bool valid = false;
    double seconds = 0.0, sampleRate = 48000.0, bpm = 0.0;

    std::vector<float> spectrumDb;          // long-term average power per FFT bin (dB re full-scale sine)
    int fftSize = kFftSize;                 // FFT size used (grows with the sample rate: ~5.9 Hz resolution)
    std::array<float, kThirds> thirdsDb {}; // 1/3-octave band energy (dB)

    float integratedLufs = -70.0f, maxMomentaryLufs = -70.0f, maxShortTermLufs = -70.0f, loudnessRange = 0.0f;
    float peakDb = -100.0f, rmsDb = -100.0f, crestDb = 0.0f, medianBlockCrestDb = 0.0f;
    float dynRangeDb = 0.0f;               // P90 - P10 of momentary loudness (active part)
    float blockRmsP60Db = -30.0f, blockRmsP90Db = -20.0f;
    int clipEvents = 0;

    float correlation = 1.0f, sideToMidDb = -100.0f, lowSideToMidDb = -100.0f, dcOffset = 0.0f;
    float flatnessPresence = 0.0f, flatnessHigh = 0.0f;
    float bassHarmonicRatioDb = 0.0f, kickClickRatioDb = 0.0f;

    bool sidechainActive = false;
    int kickHits = 0;
    float kicksPerSecond = 0.0f, lowOverlapDb = -100.0f, existingDuckDb = 0.0f;

    float bandEnergyDb (float loHz, float hiHz) const;
};

class Analyzer : private juce::Thread
{
public:
    Analyzer();
    ~Analyzer() override;

    void prepare (double sampleRate);
    void release();

    // offline analysis of an audio file (used for the reference track; do not call on a live analyser)
    Report analyseFile (const juce::File& file, const std::function<bool()>& cancelled, juce::String& error);
    // offline analysis of an in-memory buffer (optional side-chain); used by the trainer and tests
    Report analyseBuffer (const juce::AudioBuffer<float>& audio, double sampleRate, const juce::AudioBuffer<float>* sidechain = nullptr, double bpm = 0.0);

    // audio thread
    void push (const float* l, const float* r, const float* sc, int numSamples) noexcept;
    void setHostBpm (double bpm) { hostBpm.store (bpm); }

    // message thread
    void startLearning (double maxSeconds);
    void stopLearning() { stopRequested.store (true); }
    bool isLearning() const { return learning.load() || startRequested.load(); }
    double getLearnedSeconds() const { return learnedSeconds.load(); }
    int getReportGeneration() const { return reportGeneration.load(); }
    Report getReport() const;
    void getLiveSpectrum (std::vector<float>& out) const;
    int getFftSize() const { return fftSize; }

    std::atomic<float> liveMomentary { -70.0f }, liveShortTerm { -70.0f }, livePeakDb { -100.0f }, liveCorrelation { 1.0f };

private:
    void run() override;
    void setup (double sampleRate);
    void processSample (float l, float r, float sc, bool scPresent);
    void analyseHop();
    void endBlock();
    void finalise();
    void resetLearning();

    // fifo (audio -> analysis thread)
    static constexpr int kFifoSize = 1 << 17;
    juce::AbstractFifo fifo { kFifoSize };
    std::vector<float> fifoL, fifoR, fifoSc;
    std::atomic<bool> scFlag { false };

    double sr = 48000.0;
    std::atomic<double> hostBpm { 0.0 };

    // fft
    std::unique_ptr<juce::dsp::FFT> fftEngine;
    int fftSize = kFftSize, bins = kBins, hop = kHop;
    std::vector<float> window, ringMid, fftData;
    int ringPos = 0, hopCounter = 0;
    float windowGainDb = 0.0f;

    // live display
    mutable juce::SpinLock liveLock;
    std::vector<float> liveDb;

    // K-weighting (per channel, 2 stages)
    struct BQ { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
                inline double process (double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } };
    BQ kShelfL, kShelfR, kHpL, kHpR;
    BQ lowMid1, lowMid2, lowSide1, lowSide2, scLp1, scLp2, mainLp1, mainLp2;

    // 100 ms blocks
    int blockLen = 4800, blockCount = 0;
    double blockK = 0.0, blockSq = 0.0;
    float blockPeak = 0.0f;
    bool lastBlockActive = false;
    std::vector<double> recentBlocks;      // K-weighted mean squares, ring of 30 (3 s)
    int recentPos = 0;

    // learning accumulators
    std::atomic<bool> learning { false }, stopRequested { false }, startRequested { false };
    std::atomic<double> learnedSeconds { 0.0 };
    double maxLearnSeconds = 30.0;
    std::vector<double> avgPower;
    long hopsLearned = 0;
    std::vector<double> learnBlocksK;      // every 100ms block (active or not) while learning
    std::vector<float> learnBlockRmsDb, learnBlockCrestDb;
    double sumL2 = 0, sumR2 = 0, sumLR = 0, sumMid2 = 0, sumSide2 = 0, sumMid = 0, sumLowMid2 = 0, sumLowSide2 = 0;
    long activeSamples = 0;
    float learnPeak = 0.0f;
    int clipRun = 0, clipEvents = 0;
    float prevL = 0.0f, prevR = 0.0f;
    double flatPresSum = 0, flatHighSum = 0;
    long flatCount = 0;

    // block-local sums (committed if block active)
    double bL2 = 0, bR2 = 0, bLR = 0, bMid2 = 0, bSide2 = 0, bMid = 0, bLowMid2 = 0, bLowSide2 = 0;

    // sidechain / kick
    float scEnv = 0.0f, scSlow = 0.0f, mainLowEnv = 0.0f;
    int refractory = 0;
    struct PendingHit { int remaining; double mainSum, scSum; int n; };
    std::vector<PendingHit> pending;
    std::vector<float> lowHist;   // last 200 ms of main low-band energy
    int lowHistPos = 0;
    double sumAtDb = 0, sumBetweenDb = 0, sumScDb = 0;
    int hitsAt = 0, hitsBetween = 0, kickHits = 0;
    bool scSeen = false;

    mutable juce::CriticalSection reportLock;
    Report report;
    std::atomic<int> reportGeneration { 0 };
};
} // namespace mx
