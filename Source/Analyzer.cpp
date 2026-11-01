#include "Analyzer.h"

namespace mx
{
const std::array<float, kThirds>& thirdOctaveCentres()
{
    static const std::array<float, kThirds> c { 20, 25, 31.5f, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630,
                                                800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000,
                                                12500, 16000, 20000 };
    return c;
}

static inline float toDb (double power) { return (float) (10.0 * std::log10 (power + 1.0e-20)); }
static inline float ampDb (double amp) { return (float) (20.0 * std::log10 (amp + 1.0e-10)); }

static float percentile (std::vector<float> v, float p)
{
    if (v.empty()) return 0.0f;
    std::sort (v.begin(), v.end());
    const float idx = juce::jlimit (0.0f, (float) (v.size() - 1), p * (float) (v.size() - 1));
    const int i0 = (int) idx;
    const int i1 = juce::jmin ((int) v.size() - 1, i0 + 1);
    return v[(size_t) i0] + (idx - (float) i0) * (v[(size_t) i1] - v[(size_t) i0]);
}

float Report::bandEnergyDb (float loHz, float hiHz) const
{
    if (spectrumDb.empty()) return -120.0f;
    double sum = 0.0;
    const double binHz = sampleRate / fftSize;
    for (int b = juce::jmax (1, (int) (loHz / binHz)); b <= juce::jmin ((int) spectrumDb.size() - 1, (int) (hiHz / binHz)); ++b)
        sum += std::pow (10.0, spectrumDb[(size_t) b] / 10.0);
    return toDb (sum);
}

//==============================================================================
Analyzer::Analyzer() : juce::Thread ("Mix-X analyser")
{
    fifoL.assign (kFifoSize, 0.0f);
    fifoR.assign (kFifoSize, 0.0f);
    fifoSc.assign (kFifoSize, 0.0f);
    recentBlocks.assign (30, 0.0);
    setup (48000.0);
}

Analyzer::~Analyzer()
{
    stopThread (3000);
}

void Analyzer::prepare (double sampleRate)
{
    stopThread (3000);
    fifo.reset();
    setup (sampleRate);
    learning = false;
    startThread (juce::Thread::Priority::low);
}

void Analyzer::setup (double sampleRate)
{
    sr = sampleRate;

    // keep ~5.9 Hz resolution at any sample rate (8192 @ 44.1/48k, 16384 @ 88.2/96k, 32768 @ 176/192k)
    const int order = sr > 120000.0 ? kFftOrder + 2 : sr > 60000.0 ? kFftOrder + 1 : kFftOrder;
    if (fftEngine == nullptr || (1 << order) != fftSize)
    {
        fftSize = 1 << order;
        bins = fftSize / 2 + 1;
        hop = fftSize / 4;
        fftEngine = std::make_unique<juce::dsp::FFT> (order);
        window.resize ((size_t) fftSize);
        for (int i = 0; i < fftSize; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) (fftSize - 1));
        double sumW = 0;
        for (auto w : window) sumW += w;
        windowGainDb = ampDb (sumW / 2.0);
        ringMid.assign ((size_t) fftSize, 0.0f);
        fftData.assign ((size_t) fftSize * 2, 0.0f);
        const juce::SpinLock::ScopedLockType sl (liveLock);
        liveDb.assign ((size_t) bins, -120.0f);
    }
    avgPower.assign ((size_t) bins, 0.0);

    // ITU-R BS.1770 K-weighting for any sample rate
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan (juce::MathConstants<double>::pi * f0 / sr);
        const double Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        BQ s;
        s.b0 = (Vh + Vb * K / Q + K * K) / a0; s.b1 = 2.0 * (K * K - Vh) / a0; s.b2 = (Vh - Vb * K / Q + K * K) / a0;
        s.a1 = 2.0 * (K * K - 1.0) / a0; s.a2 = (1.0 - K / Q + K * K) / a0;
        kShelfL = kShelfR = s;
    }
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan (juce::MathConstants<double>::pi * f0 / sr);
        const double den = 1.0 + K / Q + K * K;
        BQ h;
        h.b0 = 1.0; h.b1 = -2.0; h.b2 = 1.0;
        h.a1 = 2.0 * (K * K - 1.0) / den; h.a2 = (1.0 - K / Q + K * K) / den;
        kHpL = kHpR = h;
    }
    auto lp = [this] (double fc)
    {
        const double w = 2.0 * juce::MathConstants<double>::pi * fc / sr;
        const double alpha = std::sin (w) / (2.0 * 0.7071), c = std::cos (w), a0 = 1.0 + alpha;
        BQ b;
        b.b0 = (1.0 - c) * 0.5 / a0; b.b1 = (1.0 - c) / a0; b.b2 = b.b0;
        b.a1 = -2.0 * c / a0; b.a2 = (1.0 - alpha) / a0;
        return b;
    };
    lowMid1 = lowMid2 = lowSide1 = lowSide2 = lp (120.0);
    lowHist.assign ((size_t) (sr * 0.2), 0.0f);
    lowHistPos = 0;
    scLp1 = scLp2 = mainLp1 = mainLp2 = lp (150.0);

    blockLen = juce::jmax (1, (int) (sr * 0.1));
    blockCount = 0;
    blockK = blockSq = 0.0;
    blockPeak = 0.0f;
    std::fill (recentBlocks.begin(), recentBlocks.end(), 0.0);
    std::fill (ringMid.begin(), ringMid.end(), 0.0f);
    ringPos = hopCounter = 0;
    resetLearning();
}

Report Analyzer::analyseBuffer (const juce::AudioBuffer<float>& audio, double sampleRate, const juce::AudioBuffer<float>* sidechain, double bpm)
{
    setup (sampleRate);
    hostBpm = bpm;
    learning = true;
    const float* l = audio.getReadPointer (0);
    const float* r = audio.getNumChannels() > 1 ? audio.getReadPointer (1) : l;
    const float* sc = sidechain != nullptr ? sidechain->getReadPointer (0) : nullptr;
    for (int i = 0; i < audio.getNumSamples(); ++i)
        processSample (l[i], r[i], sc != nullptr ? sc[i] : 0.0f, sc != nullptr);
    finalise();
    learning = false;
    return getReport();
}

// Analyse a whole audio file (reference track) on the calling thread.
Report Analyzer::analyseFile (const juce::File& file, const std::function<bool()>& cancelled, juce::String& error)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr)
    {
        error = "Unsupported or unreadable file";
        return {};
    }

    setup (reader->sampleRate);
    learning = true;

    const int chunk = 65536;
    juce::AudioBuffer<float> buf ((int) juce::jmax (2u, reader->numChannels), chunk);
    const auto total = juce::jmin (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 600.0)); // max 10 min
    for (juce::int64 pos = 0; pos < total; pos += chunk)
    {
        if (cancelled && cancelled())
        {
            error = "cancelled";
            learning = false;
            return {};
        }
        const int n = (int) juce::jmin ((juce::int64) chunk, total - pos);
        buf.clear();
        reader->read (&buf, 0, n, pos, true, true);
        const float* l = buf.getReadPointer (0);
        const float* r = reader->numChannels > 1 ? buf.getReadPointer (1) : l;
        for (int i = 0; i < n; ++i)
            processSample (l[i], r[i], 0.0f, false);
    }

    finalise();
    learning = false;
    auto rep = getReport();
    if (! rep.valid)
        error = "The file is too short or silent";
    return rep;
}

void Analyzer::release()
{
    stopThread (3000);
}

void Analyzer::push (const float* l, const float* r, const float* sc, int n) noexcept
{
    scFlag.store (sc != nullptr, std::memory_order_relaxed);
    int s1, z1, s2, z2;
    fifo.prepareToWrite (n, s1, z1, s2, z2);
    auto copy = [&] (int dst, int src, int count)
    {
        std::memcpy (fifoL.data() + dst, l + src, sizeof (float) * (size_t) count);
        std::memcpy (fifoR.data() + dst, r + src, sizeof (float) * (size_t) count);
        if (sc != nullptr) std::memcpy (fifoSc.data() + dst, sc + src, sizeof (float) * (size_t) count);
        else std::fill (fifoSc.begin() + dst, fifoSc.begin() + dst + count, 0.0f);
    };
    if (z1 > 0) copy (s1, 0, z1);
    if (z2 > 0) copy (s2, z1, z2);
    fifo.finishedWrite (z1 + z2);
}

void Analyzer::startLearning (double maxSeconds)
{
    maxLearnSeconds = maxSeconds;
    stopRequested = false;
    startRequested = true;
}

Report Analyzer::getReport() const
{
    const juce::ScopedLock sl (reportLock);
    return report;
}

void Analyzer::getLiveSpectrum (std::vector<float>& out) const
{
    const juce::SpinLock::ScopedLockType sl (liveLock);
    out = liveDb;
}

//==============================================================================
void Analyzer::run()
{
    while (! threadShouldExit())
    {
        if (startRequested.exchange (false))
        {
            resetLearning();
            learning = true;
        }

        const int ready = fifo.getNumReady();
        if (ready == 0)
        {
            wait (5);
            continue;
        }

        const bool scPresent = scFlag.load();
        int s1, z1, s2, z2;
        fifo.prepareToRead (ready, s1, z1, s2, z2);
        for (int i = 0; i < z1; ++i) processSample (fifoL[(size_t) (s1 + i)], fifoR[(size_t) (s1 + i)], fifoSc[(size_t) (s1 + i)], scPresent);
        for (int i = 0; i < z2; ++i) processSample (fifoL[(size_t) (s2 + i)], fifoR[(size_t) (s2 + i)], fifoSc[(size_t) (s2 + i)], scPresent);
        fifo.finishedRead (z1 + z2);

        if (learning && (stopRequested.load() || learnedSeconds.load() >= maxLearnSeconds))
        {
            finalise();
            learning = false;
            stopRequested = false;
        }
        else if (! learning)
        {
            stopRequested = false;
        }
    }
}

void Analyzer::resetLearning()
{
    std::fill (avgPower.begin(), avgPower.end(), 0.0);
    hopsLearned = 0;
    learnBlocksK.clear(); learnBlocksK.reserve (6000);
    learnBlockRmsDb.clear(); learnBlockRmsDb.reserve (6000);
    learnBlockCrestDb.clear(); learnBlockCrestDb.reserve (6000);
    sumL2 = sumR2 = sumLR = sumMid2 = sumSide2 = sumMid = sumLowMid2 = sumLowSide2 = 0.0;
    activeSamples = 0;
    learnPeak = 0.0f;
    clipRun = clipEvents = 0;
    flatPresSum = flatHighSum = 0.0; flatCount = 0;
    pending.clear();
    sumAtDb = sumBetweenDb = sumScDb = 0.0;
    hitsAt = hitsBetween = kickHits = 0;
    scSeen = false;
    refractory = 0;
    learnedSeconds = 0.0;
}

void Analyzer::processSample (float l, float r, float sc, bool scPresent)
{
    const float mid = 0.5f * (l + r), side = 0.5f * (l - r);

    ringMid[(size_t) ringPos] = mid;
    ringPos = (ringPos + 1) % fftSize;
    if (++hopCounter >= hop)
    {
        hopCounter = 0;
        analyseHop();
    }

    const double kl = kHpL.process (kShelfL.process (l));
    const double kr = kHpR.process (kShelfR.process (r));
    blockK += kl * kl + kr * kr;
    blockSq += 0.5 * ((double) l * l + (double) r * r);
    const float pk = juce::jmax (std::abs (l), std::abs (r));
    blockPeak = juce::jmax (blockPeak, pk);

    const bool isLearning = learning.load (std::memory_order_relaxed);
    struct PrevGuard { float& pl; float& pr; float l, r; ~PrevGuard() { pl = l; pr = r; } } guard { prevL, prevR, l, r };
    if (isLearning)
    {
        // real clipping = flat tops: consecutive (near) identical samples at a high level.
        // Floating-point overs (> 0 dBFS but still a curved waveform) are not clipping inside a DAW.
        const bool flatL = std::abs (l) >= 0.5f && std::abs (l - prevL) < 1.0e-6f;
        const bool flatR = std::abs (r) >= 0.5f && std::abs (r - prevR) < 1.0e-6f;
        if (flatL || flatR) { if (++clipRun == 2) ++clipEvents; }
        else clipRun = 0;
    }

    const double lm = lowMid2.process (lowMid1.process (mid));
    const double ls = lowSide2.process (lowSide1.process (side));
    bL2 += (double) l * l; bR2 += (double) r * r; bLR += (double) l * r;
    bMid2 += (double) mid * mid; bSide2 += (double) side * side; bMid += mid;
    bLowMid2 += lm * lm; bLowSide2 += ls * ls;

    if (isLearning && scPresent)
    {
        const float scl = (float) scLp2.process (scLp1.process (sc));
        const float a = std::abs (scl);
        const float atk = 1.0f - std::exp (-1.0f / (float) (0.001 * sr));
        const float rel = 1.0f - std::exp (-1.0f / (float) (0.06 * sr));
        const float slow = 1.0f - std::exp (-1.0f / (float) (0.3 * sr));
        scEnv += (a > scEnv ? atk : rel) * (a - scEnv);
        scSlow += slow * (scEnv - scSlow);

        const float ml = (float) mainLp2.process (mainLp1.process (mid));
        const float ml2 = ml * ml, sc2 = scl * scl;
        lowHist[(size_t) lowHistPos] = ml2;
        lowHistPos = (lowHistPos + 1) % (int) lowHist.size();

        if (scEnv > 1.0e-4f) scSeen = true;

        if (refractory > 0) --refractory;
        else if (scEnv > 0.01f && scEnv > scSlow * 2.0f)
        {
            ++kickHits;
            refractory = (int) (0.15 * sr);
            pending.push_back ({ (int) (0.06 * sr), 0.0, 0.0, 0 });

            // "between kicks" reference: main low energy from 150 ms to 30 ms before this hit
            const int hist = (int) lowHist.size();
            const int from = (int) (0.15 * sr), to = (int) (0.03 * sr);
            double e = 0; int n = 0;
            for (int k = to; k < from && k < hist; ++k) { e += lowHist[(size_t) ((lowHistPos - 1 - k + hist) % hist)]; ++n; }
            if (n > 0 && kickHits > 1) { sumBetweenDb += toDb (e / n); ++hitsBetween; }
        }

        for (auto& p : pending)
            if (p.remaining > 0)
            {
                p.mainSum += ml2; p.scSum += sc2; ++p.n;
                if (--p.remaining == 0)
                {
                    sumAtDb += toDb (p.mainSum / p.n);
                    sumScDb += toDb (p.scSum / p.n);
                    ++hitsAt;
                }
            }
        pending.erase (std::remove_if (pending.begin(), pending.end(), [] (const PendingHit& p) { return p.remaining <= 0; }),
                       pending.end());
    }

    if (++blockCount >= blockLen)
        endBlock();
}

void Analyzer::endBlock()
{
    const double ms = blockK / blockLen;
    const float lufs = (float) (-0.691 + 10.0 * std::log10 (ms + 1.0e-20));
    const bool active = lufs > -70.0f;

    recentBlocks[(size_t) recentPos] = ms;
    recentPos = (recentPos + 1) % 30;
    double m = 0, s = 0;
    for (int i = 0; i < 4; ++i) m += recentBlocks[(size_t) ((recentPos - 1 - i + 30) % 30)];
    for (auto v : recentBlocks) s += v;
    liveMomentary = (float) (-0.691 + 10.0 * std::log10 (m / 4.0 + 1.0e-20));
    liveShortTerm = (float) (-0.691 + 10.0 * std::log10 (s / 30.0 + 1.0e-20));
    livePeakDb = juce::jmax (ampDb (blockPeak), livePeakDb.load() - 1.5f);
    if (bL2 > 1.0e-9 && bR2 > 1.0e-9)
        liveCorrelation = 0.8f * liveCorrelation.load() + 0.2f * (float) (bLR / std::sqrt (bL2 * bR2));

    if (learning.load())
    {
        learnBlocksK.push_back (ms);
        if (active)
        {
            sumL2 += bL2; sumR2 += bR2; sumLR += bLR; sumMid2 += bMid2; sumSide2 += bSide2; sumMid += bMid;
            sumLowMid2 += bLowMid2; sumLowSide2 += bLowSide2;
            activeSamples += blockLen;
            learnPeak = juce::jmax (learnPeak, blockPeak);
            const float rmsDb = toDb (blockSq / blockLen);
            learnBlockRmsDb.push_back (rmsDb);
            learnBlockCrestDb.push_back (ampDb (blockPeak) - rmsDb);
            learnedSeconds = learnedSeconds.load() + 0.1;
        }
    }

    lastBlockActive = active;
    blockK = blockSq = 0.0;
    blockPeak = 0.0f;
    bL2 = bR2 = bLR = bMid2 = bSide2 = bMid = bLowMid2 = bLowSide2 = 0.0;
    blockCount = 0;
}

void Analyzer::analyseHop()
{
    for (int i = 0; i < fftSize; ++i)
        fftData[(size_t) i] = ringMid[(size_t) ((ringPos + i) % fftSize)] * window[(size_t) i];
    std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
    fftEngine->performFrequencyOnlyForwardTransform (fftData.data());

    {
        const juce::SpinLock::ScopedLockType sl (liveLock);
        for (int b = 0; b < bins; ++b)
        {
            const float db = ampDb (fftData[(size_t) b]) - windowGainDb;
            liveDb[(size_t) b] = juce::jmax (db, liveDb[(size_t) b] - 1.5f);
        }
    }

    if (learning.load() && lastBlockActive)
    {
        for (int b = 0; b < bins; ++b)
            avgPower[(size_t) b] += (double) fftData[(size_t) b] * fftData[(size_t) b];
        ++hopsLearned;

        auto flatness = [&] (double lo, double hi)
        {
            const double binHz = sr / fftSize;
            double logSum = 0, sum = 0;
            int n = 0;
            for (int b = (int) (lo / binHz); b <= (int) (hi / binHz) && b < bins; ++b)
            {
                const double p = (double) fftData[(size_t) b] * fftData[(size_t) b] + 1.0e-20;
                logSum += std::log (p); sum += p; ++n;
            }
            return n > 0 ? std::exp (logSum / n) / (sum / n) : 0.0;
        };
        flatPresSum += flatness (1000, 5000);
        flatHighSum += flatness (3000, 8000);
        ++flatCount;
    }
}

void Analyzer::finalise()
{
    Report r;
    r.sampleRate = sr;
    r.fftSize = fftSize;
    r.bpm = hostBpm.load();
    r.seconds = learnedSeconds.load();
    r.valid = hopsLearned > 4 && activeSamples > (long) (sr * 2.0);

    if (r.valid)
    {
        r.spectrumDb.resize (bins);
        for (int b = 0; b < bins; ++b)
            r.spectrumDb[(size_t) b] = toDb (avgPower[(size_t) b] / (double) hopsLearned) - windowGainDb;

        const double binHz = sr / fftSize;
        for (int t = 0; t < kThirds; ++t)
        {
            const double fc = thirdOctaveCentres()[(size_t) t];
            const double lo = fc / std::pow (2.0, 1.0 / 6.0), hi = fc * std::pow (2.0, 1.0 / 6.0);
            double sum = 0;
            int n = 0;
            for (int b = 1; b < bins; ++b)
            {
                const double f = b * binHz;
                if (f >= lo && f < hi) { sum += std::pow (10.0, r.spectrumDb[(size_t) b] / 10.0); ++n; }
            }
            if (n == 0) // band narrower than an FFT bin: use the nearest bin
                sum = std::pow (10.0, r.spectrumDb[(size_t) juce::jlimit (1, bins - 1, (int) std::round (fc / binHz))] / 10.0);
            r.thirdsDb[(size_t) t] = fc < sr * 0.5 ? toDb (sum) : -120.0f;
        }

        // loudness (BS.1770 gating)
        std::vector<float> momentary, shortTerm;
        const auto& B = learnBlocksK;
        for (size_t k = 3; k < B.size(); ++k)
            momentary.push_back ((float) (-0.691 + 10.0 * std::log10 ((B[k] + B[k - 1] + B[k - 2] + B[k - 3]) / 4.0 + 1.0e-20)));
        for (size_t k = 29; k < B.size(); ++k)
        {
            double s = 0;
            for (size_t j = k - 29; j <= k; ++j) s += B[j];
            shortTerm.push_back ((float) (-0.691 + 10.0 * std::log10 (s / 30.0 + 1.0e-20)));
        }

        auto gatedMean = [] (const std::vector<float>& v, float relGate, std::vector<float>* kept)
        {
            double e = 0; int n = 0;
            for (auto x : v) if (x > -70.0f) { e += std::pow (10.0, (x + 0.691) / 10.0); ++n; }
            if (n == 0) return -70.0f;
            const float rel = (float) (-0.691 + 10.0 * std::log10 (e / n)) + relGate;
            e = 0; n = 0;
            for (auto x : v)
                if (x > -70.0f && x > rel)
                {
                    e += std::pow (10.0, (x + 0.691) / 10.0); ++n;
                    if (kept) kept->push_back (x);
                }
            return n > 0 ? (float) (-0.691 + 10.0 * std::log10 (e / n)) : -70.0f;
        };

        r.integratedLufs = gatedMean (momentary, -10.0f, nullptr);
        std::vector<float> keptM, keptS;
        gatedMean (momentary, -20.0f, &keptM);
        gatedMean (shortTerm, -20.0f, &keptS);
        for (auto x : momentary) r.maxMomentaryLufs = juce::jmax (r.maxMomentaryLufs, x);
        for (auto x : shortTerm) r.maxShortTermLufs = juce::jmax (r.maxShortTermLufs, x);
        r.loudnessRange = keptS.size() > 2 ? percentile (keptS, 0.95f) - percentile (keptS, 0.10f) : 0.0f;
        r.dynRangeDb = keptM.size() > 2 ? percentile (keptM, 0.90f) - percentile (keptM, 0.10f) : 0.0f;

        r.peakDb = ampDb (learnPeak);
        r.rmsDb = toDb ((sumL2 + sumR2) / (2.0 * (double) activeSamples));
        r.crestDb = r.peakDb - r.rmsDb;
        r.medianBlockCrestDb = percentile (learnBlockCrestDb, 0.5f);
        r.blockRmsP60Db = percentile (learnBlockRmsDb, 0.6f);
        r.blockRmsP90Db = percentile (learnBlockRmsDb, 0.9f);
        r.clipEvents = clipEvents;

        r.correlation = (sumL2 > 0 && sumR2 > 0) ? (float) (sumLR / std::sqrt (sumL2 * sumR2)) : 1.0f;
        r.sideToMidDb = toDb (sumSide2 / (sumMid2 + 1.0e-20));
        r.lowSideToMidDb = toDb (sumLowSide2 / (sumLowMid2 + 1.0e-20));
        r.dcOffset = (float) (sumMid / (double) activeSamples);
        r.flatnessPresence = flatCount > 0 ? (float) (flatPresSum / flatCount) : 0.0f;
        r.flatnessHigh = flatCount > 0 ? (float) (flatHighSum / flatCount) : 0.0f;
        r.bassHarmonicRatioDb = r.bandEnergyDb (150, 1000) - r.bandEnergyDb (30, 120);
        r.kickClickRatioDb = r.bandEnergyDb (1000, 5000) - r.bandEnergyDb (40, 100);

        r.sidechainActive = scSeen && kickHits >= 4 && hitsAt > 0 && hitsBetween > 0;
        r.kickHits = kickHits;
        if (r.sidechainActive)
        {
            r.kicksPerSecond = (float) (kickHits / juce::jmax (1.0, r.seconds));
            r.lowOverlapDb = (float) (sumAtDb / hitsAt - sumScDb / hitsAt);
            r.existingDuckDb = (float) (sumBetweenDb / hitsBetween - sumAtDb / hitsAt);
        }
    }

    {
        const juce::ScopedLock sl (reportLock);
        report = std::move (r);
    }
    ++reportGeneration;
}
} // namespace mx
