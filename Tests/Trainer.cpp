// Mix-X trainer: builds a labelled corpus (clean mixes + mixes with injected defects of known size),
// analyses it once, then tunes the diagnosis thresholds (coordinate descent) to maximise detection
// while minimising false alarms. Also checks real CC-licensed recordings for sanity.
#include "Analyzer.h"
#include "Diagnostics.h"
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_formats/juce_audio_formats.h>

using namespace mx;
static constexpr double PI = 3.14159265358979;

//==============================================================================
// target curve: exactly what the diagnosis uses (generic genre), extrapolated outside 20 Hz - 20 kHz
static float targetDb (Profile p, float f)
{
    if (f < 20.0f) return targetCurveDb (p, 0, 20.0f) - 24.0f * std::log2 (20.0f / juce::jmax (1.0f, f));
    if (f > 20000.0f) return targetCurveDb (p, 0, 20000.0f) - 24.0f * std::log2 (f / 20000.0f);
    return targetCurveDb (p, 0, f);
}

//==============================================================================
// noise "music" whose 1/3-octave energy follows a target curve (+ smooth random variation)
static juce::AudioBuffer<float> shapedNoise (Profile p, double sr, double seconds, float width, juce::Random& rng, float variationDb)
{
    const int order = sr > 60000.0 ? 14 : 13, N = 1 << order, hop = N / 4;
    juce::dsp::FFT fft (order);
    const int total = (int) (sr * seconds);
    juce::AudioBuffer<float> out (2, total + N);
    out.clear();

    // smooth random deviation: a few random bumps in log frequency
    float bumpF[4], bumpG[4], bumpW[4];
    for (int i = 0; i < 4; ++i) { bumpF[i] = 30.0f * std::pow (600.0f, rng.nextFloat()); bumpG[i] = (rng.nextFloat() * 2 - 1) * variationDb; bumpW[i] = 0.8f + rng.nextFloat(); }
    const float tilt = (rng.nextFloat() * 2 - 1) * 0.4f;   // +-0.4 dB/oct

    std::vector<float> mag ((size_t) N / 2 + 1);
    for (int k = 1; k <= N / 2; ++k)
    {
        const float f = (float) (k * sr / N);
        float d = targetDb (p, f) + tilt * std::log2 (f / 1000.0f);
        for (int i = 0; i < 4; ++i) { const float x = std::log2 (f / bumpF[i]) / bumpW[i]; d += bumpG[i] * std::exp (-x * x); }
        mag[(size_t) k] = std::sqrt (std::pow (10.0f, d / 10.0f) / f);   // per-bin density so that band energy follows d
    }
    mag[0] = 0;

    std::vector<float> win ((size_t) N), data ((size_t) N * 2);
    for (int i = 0; i < N; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * (float) PI * i / N);

    for (int ch = 0; ch < 3; ++ch)  // 0 = mid, 1 = side source
    {
        if (ch == 2) break;
        juce::AudioBuffer<float> tmp (1, total + N);
        tmp.clear();
        for (int pos = 0; pos < total; pos += hop)
        {
            std::fill (data.begin(), data.end(), 0.0f);
            for (int k = 1; k < N / 2; ++k)
            {
                const float ph = rng.nextFloat() * 2.0f * (float) PI;
                data[(size_t) (2 * k)] = mag[(size_t) k] * std::cos (ph);
                data[(size_t) (2 * k + 1)] = mag[(size_t) k] * std::sin (ph);
                data[(size_t) (2 * (N - k))] = data[(size_t) (2 * k)];
                data[(size_t) (2 * (N - k) + 1)] = -data[(size_t) (2 * k + 1)];
            }
            fft.performRealOnlyInverseTransform (data.data());
            for (int i = 0; i < N && pos + i < total + N; ++i)
                tmp.addSample (0, pos + i, data[(size_t) i] * win[(size_t) i]);
        }
        if (ch == 0) { out.copyFrom (0, 0, tmp, 0, 0, total); out.copyFrom (1, 0, tmp, 0, 0, total); }
        else
        {
            // like real mixes: the stereo (side) content lives above ~150 Hz
            for (int pass = 0; pass < 2; ++pass)
            {
                juce::IIRFilter hpf;
                hpf.setCoefficients (juce::IIRCoefficients::makeHighPass (sr, 150.0));
                hpf.processSamples (tmp.getWritePointer (0), total);
            }
            out.addFrom (0, 0, tmp, 0, 0, total, width);
            out.addFrom (1, 0, tmp, 0, 0, total, -width);
        }
    }
    out.setSize (2, total, true);
    return out;
}

static float rms (const juce::AudioBuffer<float>& b)
{
    double s = 0;
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i) s += (double) b.getSample (c, i) * b.getSample (c, i);
    return (float) std::sqrt (s / (b.getNumSamples() * b.getNumChannels()));
}

static void setLevel (juce::AudioBuffer<float>& b, float dbRms)
{
    const float r = rms (b);
    if (r > 0) b.applyGain (juce::Decibels::decibelsToGain (dbRms) / r);
}

// rhythmic / phrase envelope typical of each track type
static void applyEnvelope (juce::AudioBuffer<float>& b, Profile p, double sr, double bpm, juce::Random& rng)
{
    const double beat = 60.0 / bpm;
    const float phase = rng.nextFloat();
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const double t = i / sr + phase;
        float e = 1.0f;
        switch (p)
        {
            case Profile::Master:   e = 0.75f + 0.25f * (float) std::exp (-std::fmod (t, beat) * 12.0); break;
            case Profile::DrumBus:  e = 0.15f + 0.85f * (float) std::exp (-std::fmod (t, beat / 2) * 18.0); break;
            case Profile::Kick:     e = (float) std::exp (-std::fmod (t, beat) * 22.0); break;
            case Profile::Bass:     { const double tb = std::fmod (t, beat / 2); e = tb < beat * 0.4 ? 1.0f : 0.25f; break; }
            case Profile::Vocal:    { const double s = std::fmod (t, 0.3); e = 0.15f + 0.85f * (float) std::max (0.0, std::sin (PI * s / 0.3)); e *= (std::fmod (t, 4.0) < 3.4 ? 1.0f : 0.1f); break; }
            case Profile::Lead:     { const double s = std::fmod (t, beat / 2); e = s < beat * 0.4 ? 1.0f : 0.3f; break; }
            case Profile::Pad:      e = 0.8f + 0.2f * (float) std::sin (2 * PI * 0.25 * t); break;
            case Profile::HatsPerc: e = 0.05f + (float) std::exp (-std::fmod (t, beat / 4) * 45.0); break;
        }
        b.setSample (0, i, b.getSample (0, i) * e);
        b.setSample (1, i, b.getSample (1, i) * e);
    }
}

static void peakFilter (juce::AudioBuffer<float>& b, double sr, float f, float q, float gainDb)
{
    for (int c = 0; c < b.getNumChannels(); ++c)
    {
        juce::IIRFilter flt;
        flt.setCoefficients (juce::IIRCoefficients::makePeakFilter (sr, f, q, juce::Decibels::decibelsToGain (gainDb)));
        flt.processSamples (b.getWritePointer (c), b.getNumSamples());
    }
}

static float kickAt (double t, double bpm)
{
    const double tb = std::fmod (t, 60.0 / bpm);
    return (float) (std::sin (2 * PI * (50.0 * tb + 120.0 / 40.0 * (1.0 - std::exp (-tb * 40.0)))) * std::exp (-tb * 9.0) * 0.9);
}

//==============================================================================
enum class Defect { None, EqBoost, EqCut, Rumble, Resonance, Dynamics, Clipping, OverLimit, StereoBass, Phase, Masking, Ducked };
static const char* defectName (Defect d)
{
    static const char* n[] = { "clean", "eq boost", "eq cut", "rumble", "resonance", "too dynamic", "clipping", "over-limited", "stereo bass", "phase", "kick masking", "ducked (no warn)" };
    return n[(int) d];
}

struct Item
{
    Profile profile;
    Defect defect;
    float freq = 0;   // for EQ / resonance defects
    Report report;
    juce::String label;
};

static bool detected (const Item& it, const Diagnosis& d)
{
    auto any = [&] (std::function<bool (const Issue&)> f) { for (auto& i : d.issues) if (f (i)) return true; return false; };
    auto nearF = [&] (const Issue& i, float oct) { return i.freqHz > 0 && std::abs (std::log2 (i.freqHz / it.freq)) <= oct; };
    switch (it.defect)
    {
        case Defect::EqBoost:   return any ([&] (const Issue& i) { return i.category == Category::EQ && nearF (i, 0.7f) && i.why.contains ("louder"); });
        case Defect::EqCut:     return any ([&] (const Issue& i) { return i.category == Category::EQ && nearF (i, 0.7f) && i.why.contains ("quieter"); });
        case Defect::Rumble:    return any ([&] (const Issue& i) { return i.title.startsWith ("Useless energy") || (i.category == Category::EQ && i.freqHz > 0 && i.freqHz < 100 && i.why.contains ("louder")); });
        case Defect::Resonance: return any ([&] (const Issue& i) { return i.category == Category::EQ && nearF (i, 0.34f) && (i.title.startsWith ("Resonance") || i.why.contains ("louder")); });
        case Defect::Dynamics:  return any ([&] (const Issue& i) { return i.category == Category::Compression; });
        case Defect::Clipping:  return any ([&] (const Issue& i) { return i.title.startsWith ("Digital clipping") || i.title.startsWith ("Hard clipping"); });
        case Defect::OverLimit: return any ([&] (const Issue& i) { return i.title.startsWith ("Over-limited"); });
        case Defect::StereoBass:return any ([&] (const Issue& i) { return i.title.startsWith ("Stereo low end") || i.title.endsWith ("is not mono"); });
        case Defect::Phase:     return any ([&] (const Issue& i) { return i.title.startsWith ("Phase problems"); });
        case Defect::Masking:   return any ([&] (const Issue& i) { return i.category == Category::Sidechain && i.title.contains ("masking"); });
        case Defect::Ducked:    return ! any ([&] (const Issue& i) { return i.category == Category::Sidechain && i.severity > 0; });
        case Defect::None:      return true;
    }
    return false;
}

// false alarms: issues that are not explained by the injected defect
static int falseAlarms (const Item& it, const Diagnosis& d)
{
    int n = 0;
    for (auto& i : d.issues)
    {
        if (i.severity == 0) continue;
        const bool explained =
            (it.defect == Defect::EqBoost || it.defect == Defect::EqCut || it.defect == Defect::Resonance) ? i.category == Category::EQ
          : it.defect == Defect::Rumble ? i.category == Category::EQ
          : it.defect == Defect::Dynamics ? i.category == Category::Compression
          : it.defect == Defect::Clipping ? (i.category == Category::Saturation || i.category == Category::Loudness || i.category == Category::Compression)
          : it.defect == Defect::OverLimit ? (i.category == Category::Loudness || i.category == Category::Saturation || i.category == Category::Compression)
          : it.defect == Defect::StereoBass ? i.category == Category::Stereo
          : it.defect == Defect::Phase ? i.category == Category::Stereo
          : it.defect == Defect::Masking ? i.category == Category::Sidechain
          : false;
        // loudness-target notes on the master are informational, not false alarms
        if (i.category == Category::Loudness && i.title.startsWith ("Below the loudness")) continue;
        if (! explained) ++n;
    }
    return n;
}

//==============================================================================
struct Corpus
{
    std::vector<Item> items;
    void build (int variants, juce::Random& rng)
    {
        Analyzer an;
        const double rates[] = { 44100.0, 48000.0, 96000.0 };
        const Profile all[] = { Profile::Master, Profile::DrumBus, Profile::Kick, Profile::Bass, Profile::Vocal, Profile::Lead, Profile::Pad, Profile::HatsPerc };

        auto eqRange = [] (Profile p, float& lo, float& hi)
        {
            switch (p)
            {
                case Profile::Master: case Profile::DrumBus: lo = 60; hi = 8000; break;
                case Profile::Kick: lo = 50; hi = 4000; break;
                case Profile::Bass: lo = 45; hi = 700; break;
                case Profile::Vocal: lo = 200; hi = 8000; break;
                case Profile::Lead: lo = 250; hi = 8000; break;
                case Profile::Pad: lo = 200; hi = 5000; break;
                case Profile::HatsPerc: lo = 2500; hi = 12000; break;
            }
        };

        int count = 0;
        for (auto p : all)
            for (int v = 0; v < variants; ++v)
            {
                const double sr = rates[rng.nextInt (3)];
                const double bpm = 118 + rng.nextInt (12);
                const float width = (p == Profile::Kick || p == Profile::Bass) ? 0.0f : 0.1f + 0.25f * rng.nextFloat();
                const float level = -12.0f - 10.0f * rng.nextFloat();

                auto base = [&] (float widthOverride = -1.0f)
                {
                    juce::Random r2 (rng.nextInt64());
                    auto b = shapedNoise (p, sr, 12.0, widthOverride >= 0 ? widthOverride : width, r2, 1.5f);
                    applyEnvelope (b, p, sr, bpm, r2);
                    setLevel (b, level);
                    return b;
                };
                auto add = [&] (Defect d, juce::AudioBuffer<float> b, float f = 0, const juce::AudioBuffer<float>* sc = nullptr)
                {
                    Item it { p, d, f, an.analyseBuffer (b, sr, sc, bpm), {} };
                    it.label = profileNames()[(int) p] + " / " + defectName (d) + (f > 0 ? " @" + juce::String ((int) f) + "Hz" : juce::String()) + " / " + juce::String ((int) sr);
                    items.push_back (std::move (it));
                    ++count;
                };

                add (Defect::None, base());

                float lo, hi;
                eqRange (p, lo, hi);
                {
                    const float f = lo * std::pow (hi / lo, rng.nextFloat());
                    auto b = base(); peakFilter (b, sr, f, 1.2f, 5.0f + 4.0f * rng.nextFloat()); setLevel (b, level);
                    add (Defect::EqBoost, std::move (b), f);
                }
                {
                    const float f = lo * std::pow (hi / lo, rng.nextFloat());
                    auto b = base(); peakFilter (b, sr, f, 1.0f, -(6.0f + 4.0f * rng.nextFloat())); setLevel (b, level);
                    add (Defect::EqCut, std::move (b), f);
                }
                if (p == Profile::Vocal || p == Profile::Lead || p == Profile::Pad || p == Profile::HatsPerc)
                {
                    auto b = base();
                    const float hpf = p == Profile::Vocal ? 80.0f : p == Profile::Lead ? 120.0f : p == Profile::Pad ? 100.0f : 300.0f;
                    const float amp = juce::Decibels::decibelsToGain (level - 6.0f - 8.0f * rng.nextFloat());
                    for (int i = 0; i < b.getNumSamples(); ++i)
                    {
                        const float s = amp * (float) std::sin (2 * PI * hpf * 0.4 * i / sr) * 1.41f;
                        b.addSample (0, i, s); b.addSample (1, i, s);
                    }
                    add (Defect::Rumble, std::move (b));
                }
                if (p == Profile::Master || p == Profile::DrumBus || p == Profile::Vocal)
                {
                    const float f = 300.0f * std::pow (20.0f, rng.nextFloat());
                    auto b = base(); peakFilter (b, sr, f, 10.0f, 12.0f); setLevel (b, level);
                    add (Defect::Resonance, std::move (b), f);
                }
                if (p == Profile::Vocal || p == Profile::Bass || p == Profile::Lead || p == Profile::Pad)
                {
                    auto b = base();
                    for (int i = 0; i < b.getNumSamples(); ++i)
                    {
                        const float g = std::fmod (i / sr, 3.0) < 1.5 ? 1.0f : 0.18f;   // 15 dB phrase swings
                        b.setSample (0, i, b.getSample (0, i) * g); b.setSample (1, i, b.getSample (1, i) * g);
                    }
                    setLevel (b, level);
                    add (Defect::Dynamics, std::move (b));
                }
                {
                    auto b = base();
                    b.applyGain (1.8f / juce::jmax (1e-6f, b.getMagnitude (0, b.getNumSamples())));
                    for (int c = 0; c < 2; ++c)
                        for (int i = 0; i < b.getNumSamples(); ++i)
                            b.setSample (c, i, juce::jlimit (-1.0f, 1.0f, b.getSample (c, i)));
                    add (Defect::Clipping, std::move (b));
                }
                if (p == Profile::Master)
                {
                    auto b = base();
                    b.applyGain (8.0f / juce::jmax (1e-6f, b.getMagnitude (0, b.getNumSamples())));
                    for (int c = 0; c < 2; ++c)
                        for (int i = 0; i < b.getNumSamples(); ++i)
                            b.setSample (c, i, 0.97f * std::tanh (b.getSample (c, i)));
                    add (Defect::OverLimit, std::move (b));
                }
                if (p == Profile::Master || p == Profile::Bass || p == Profile::DrumBus)
                {
                    auto b = base();
                    juce::Random r3 (rng.nextInt64());
                    auto low = shapedNoise (Profile::Bass, sr, 12.0, 0.0f, r3, 0.0f);
                    setLevel (low, level - 2.0f);
                    for (int i = 0; i < b.getNumSamples(); ++i)
                    {
                        const float s = low.getSample (0, i);
                        b.addSample (0, i, s); b.addSample (1, i, -s);   // low end entirely in the side channel
                    }
                    add (Defect::StereoBass, std::move (b));
                }
                if (p == Profile::Master || p == Profile::Pad)
                {
                    auto b = base();
                    for (int i = 0; i < b.getNumSamples(); ++i)
                        b.setSample (1, i, -0.9f * b.getSample (0, i) + 0.1f * b.getSample (1, i));
                    add (Defect::Phase, std::move (b));
                }
                if (p == Profile::Bass || p == Profile::Pad)
                {
                    juce::AudioBuffer<float> sc (1, (int) (12.0 * sr));
                    for (int i = 0; i < sc.getNumSamples(); ++i) sc.setSample (0, i, kickAt (i / sr, bpm));
                    auto b = base();
                    if (p == Profile::Pad) b.applyGain (2.0f);
                    add (Defect::Masking, b, 0, &sc);
                    for (int i = 0; i < b.getNumSamples(); ++i)
                    {
                        const double tb = std::fmod (i / sr, 60.0 / bpm);
                        const float g = (float) (1.0 - 0.8 * std::exp (-tb * 8.0));
                        b.setSample (0, i, b.getSample (0, i) * g); b.setSample (1, i, b.getSample (1, i) * g);
                    }
                    add (Defect::Ducked, std::move (b), 0, &sc);
                }
                std::cout << "\r  corpus: " << count << " items" << std::flush;
            }
        std::cout << "\n";
    }
};

struct Score { double objective, recall, fpClean, fpAll; std::map<int, std::pair<int, int>> perDefect; };

static Score evaluate (const Corpus& c, const std::vector<Report>& real, const std::vector<Profile>& realProfiles, bool verbose = false)
{
    Score s {};
    int clean = 0, fpClean = 0, fpAll = 0;
    for (auto& it : c.items)
    {
        const auto d = diagnose (it.report, it.profile, 0, Daw::Logic, 2);
        const bool ok = detected (it, d);
        auto& pd = s.perDefect[(int) it.defect];
        pd.second++;
        if (ok) pd.first++;
        const int fa = falseAlarms (it, d);
        fpAll += fa;
        if (it.defect == Defect::None) { ++clean; fpClean += fa; }
        if (verbose && (! ok || fa > 0))
        {
            std::cout << "   " << (ok ? "   " : "MISS ") << it.label << (fa > 0 ? "  false alarms: " : "");
            if (fa > 0) for (auto& i : d.issues) if (i.severity > 0) std::cout << "[" << i.title << "] ";
            std::cout << "\n";
        }
    }
    double recallSum = 0; int types = 0;
    for (auto& [k, v] : s.perDefect)
        if (k != (int) Defect::None) { recallSum += (double) v.first / v.second; ++types; }
    s.recall = recallSum / juce::jmax (1, types);
    s.fpClean = (double) fpClean / juce::jmax (1, clean);
    s.fpAll = (double) fpAll / juce::jmax (1, (int) c.items.size());

    // real recordings: we don't know their "truth", but a sane analyser should not bury them in issues
    double realExcess = 0;
    for (size_t i = 0; i < real.size(); ++i)
    {
        const auto d = diagnose (real[i], realProfiles[i], 0, Daw::Logic, 2);
        int n = 0, all = 0;
        for (auto& is : d.issues) { if (is.severity >= 2) ++n; if (is.severity >= 1) ++all; }
        realExcess += juce::jmax (0, n - 2) + 0.5 * juce::jmax (0, all - 4);
    }
    s.objective = s.recall - 0.20 * s.fpClean - 0.10 * s.fpAll - 0.02 * realExcess;
    return s;
}

int main (int argc, char** argv)
{
    juce::ignoreUnused (argc);
    const int variants = argc > 1 ? juce::String (argv[1]).getIntValue() : 6;
    const juce::File realDir (argc > 2 ? argv[2] : "/home/claude/corpus/real");

    // --- real recordings
    std::vector<Report> real;
    std::vector<Profile> realProfiles;
    std::vector<juce::String> realNames;
    for (const auto& e : juce::RangedDirectoryIterator (realDir, false, "*.ogg;*.wav;*.flac;*.mp3"))
    {
        Analyzer an;
        juce::String err;
        auto r = an.analyseFile (e.getFile(), {}, err);
        if (! r.valid) continue;
        const auto name = e.getFile().getFileNameWithoutExtension();
        const bool speech = name.containsOnly ("0123456789-.hq");
        real.push_back (r);
        realProfiles.push_back (speech ? Profile::Vocal : Profile::Master);
        realNames.push_back (name);
    }
    std::cout << "real recordings: " << real.size() << "\n";

    // --- synthetic labelled corpus
    juce::Random rng (2026);
    Corpus corpus;
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    corpus.build (variants, rng);
    std::cout << "built in " << juce::String ((juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0, 1) << " s\n";

    auto report = [&] (const char* title)
    {
        const auto s = evaluate (corpus, real, realProfiles);
        std::cout << "\n== " << title << ": objective " << juce::String (s.objective, 3) << "  recall " << juce::String (s.recall * 100, 1)
                  << "%  false alarms/clean " << juce::String (s.fpClean, 2) << "  false alarms/item " << juce::String (s.fpAll, 2) << "\n";
        for (auto& [k, v] : s.perDefect)
            std::cout << "   " << juce::String (defectName ((Defect) k)).paddedRight (' ', 18) << v.first << "/" << v.second << "\n";
        for (size_t i = 0; i < real.size(); ++i)
        {
            const auto d = diagnose (real[i], realProfiles[i], 0, Daw::Logic, 2);
            std::cout << "   real: " << realNames[i].substring (0, 40).paddedRight (' ', 42) << " score " << d.score << "  issues:";
            for (auto& is : d.issues) if (is.severity > 0) std::cout << " [" << is.title << "]";
            std::cout << "\n";
        }
    };

    // --- calibration data: how real music deviates from the generic master curve (median-aligned)
    {
        std::array<double, kThirds> sum {};
        int n = 0;
        for (size_t i = 0; i < real.size(); ++i)
        {
            if (realProfiles[i] != Profile::Master) continue;
            std::vector<float> diffs;
            for (int t = 0; t < kThirds; ++t)
            {
                const float f = thirdOctaveCentres()[(size_t) t];
                if (f >= 40 && f <= 12000) diffs.push_back (real[i].thirdsDb[(size_t) t] - targetCurveDb (Profile::Master, 0, f));
            }
            std::sort (diffs.begin(), diffs.end());
            const float off = diffs[diffs.size() / 2];
            for (int t = 0; t < kThirds; ++t)
                sum[(size_t) t] += real[i].thirdsDb[(size_t) t] - targetCurveDb (Profile::Master, 0, thirdOctaveCentres()[(size_t) t]) - off;
            ++n;
        }
        std::cout << "\nreal music vs generic master curve (avg deviation, dB):\nCALIB";
        for (int t = 0; t < kThirds; ++t) std::cout << " " << juce::String (sum[(size_t) t] / juce::jmax (1, n), 1);
        std::cout << "\n";
    }

    report ("BEFORE training");

    // --- coordinate descent over the thresholds
    Tuning& T = tuning();
    struct Knob { const char* name; float* v; std::vector<float> candidates; };
    std::vector<Knob> knobs {
        { "eqTol", &T.eqTol, { 2.0f, 2.5f, 3.0f, 3.5f, 4.0f, 4.5f } },
        { "eqTolBus", &T.eqTolBus, { 2.0f, 2.5f, 3.0f, 3.5f, 4.0f } },
        { "eqRange", &T.eqRange, { 12, 15, 18, 21, 24 } },
        { "eqRangeLowExtra", &T.eqRangeLowExtra, { 0, 6, 12, 18 } },
        { "rumbleRel", &T.rumbleRel, { -30, -26, -22, -18, -14 } },
        { "subsonicRel", &T.subsonicRel, { -20, -17, -14, -11, -8 } },
        { "resonanceDb", &T.resonanceDb, { 7.0f, 7.5f, 8.0f, 9.0f } },
        { "vocalDR", &T.vocalDR, { 5, 6, 7, 8, 9, 10 } },
        { "bassDR", &T.bassDR, { 4, 5, 6, 7, 8 } },
        { "leadDR", &T.leadDR, { 6, 7, 8, 9, 10 } },
        { "padDR", &T.padDR, { 6, 7, 8, 9, 10, 11 } },
        
        { "kickCrestLow", &T.kickCrestLow, { 6, 7, 8, 9, 10 } },
        
        { "busCrestLow", &T.busCrestLow, { 6, 7, 8, 9, 10 } },
        { "masterPlr", &T.masterPlr, { 5, 6, 7, 8 } },
        { "lowSide", &T.lowSide, { -18, -15, -12 } },
        { "correlation", &T.correlation, { 0.0f, 0.1f, 0.2f, 0.3f } },
        { "narrowSide", &T.narrowSide, { -35, -30, -25, -20 } },
        { "monoSide", &T.monoSide, { -25, -20, -15, -10 } },
        { "overlapBass", &T.overlapBass, { -18, -15, -12, -9 } },
        { "overlapOther", &T.overlapOther, { -24, -21, -18, -15 } },
        { "duckMin", &T.duckMin, { 2, 3, 4, 5 } },
    };

    double best = evaluate (corpus, real, realProfiles).objective;
    for (int round = 0; round < 3; ++round)
    {
        bool improved = false;
        for (auto& k : knobs)
        {
            const float original = *k.v;
            float bestV = original;
            for (float c : k.candidates)
            {
                *k.v = c;
                const double o = evaluate (corpus, real, realProfiles).objective;
                if (o > best + 1e-6) { best = o; bestV = c; improved = true; }
            }
            *k.v = bestV;
        }
        std::cout << "round " << round + 1 << ": objective " << juce::String (best, 3) << "\n";
        if (! improved) break;
    }

    report ("AFTER training");
    std::cout << "\nremaining misses / false alarms:\n";
    evaluate (corpus, real, realProfiles, true);

    std::cout << "\n// tuned thresholds\n";
    for (auto& k : knobs)
        std::cout << "TUNED " << k.name << " = " << *k.v << "\n";
    return 0;
}
