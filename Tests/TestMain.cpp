// Mix-X tests: feed mixes with known problems and check the diagnosis finds them.
#include "PluginProcessor.h"
#include "PluginEditor.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (! (cond)) { ++failures; std::cout << "FAIL: " << msg << "\n"; } else std::cout << "ok:   " << msg << "\n"; } while (0)

static constexpr double SR = 48000.0;
static constexpr int BLOCK = 512;
static constexpr double PI = 3.14159265358979;

struct Gen
{
    // returns stereo sample (l, r) for sample index i; sc = sidechain sample
    std::function<void (int, float&, float&, float&)> fn;
};

static void runAnalysis (MixXProcessor& p, const Gen& g, double seconds, bool withSidechain)
{
    if (withSidechain)
    {
        auto layout = p.getBusesLayout();
        layout.inputBuses.getReference (1) = juce::AudioChannelSet::stereo();
        p.setBusesLayout (layout);
    }
    p.setRateAndBufferSizeDetails (SR, BLOCK);   // (setPlayConfigDetails would disable the side-chain bus)
    p.prepareToPlay (SR, BLOCK);
    p.startAnalysis (seconds - 0.5);

    juce::AudioBuffer<float> buf (withSidechain ? 4 : 2, BLOCK);
    juce::MidiBuffer midi;
    const int total = (int) (seconds * SR);
    for (int pos = 0; pos < total; pos += BLOCK)
    {
        for (int i = 0; i < BLOCK; ++i)
        {
            float l, r, sc = 0;
            g.fn (pos + i, l, r, sc);
            buf.setSample (0, i, l);
            buf.setSample (1, i, r);
            if (withSidechain) { buf.setSample (2, i, sc); buf.setSample (3, i, sc); }
        }
        p.processBlock (buf, midi);
        if ((pos / BLOCK) % 4 == 0) juce::Thread::sleep (1);   // don't outrun the analysis thread
    }
    // let the analysis thread finish
    for (int i = 0; i < 400 && p.analyzer.isLearning(); ++i)
        juce::Thread::sleep (10);
    if (p.analyzer.isLearning()) p.stopAnalysis();
    for (int i = 0; i < 200 && p.analyzer.isLearning(); ++i) juce::Thread::sleep (10);
    juce::Thread::sleep (50);
    // processor timer is message-thread based: pump it
    juce::MessageManager::getInstance()->runDispatchLoopUntil (400);
    std::cout << "[debug] learning=" << p.analyzer.isLearning() << " learned=" << p.analyzer.getLearnedSeconds()
              << " gen=" << p.analyzer.getReportGeneration() << " valid=" << p.analyzer.getReport().valid
              << " procValid=" << p.getReport().valid << "\n";
}

static bool hasIssue (const MixXProcessor& p, const juce::String& titlePart)
{
    for (auto& i : p.getDiagnosis().issues)
        if (i.title.containsIgnoreCase (titlePart)) return true;
    return false;
}

static void setChoice (MixXProcessor& p, const char* id, int v)
{
    auto* q = p.apvts.getParameter (id);
    q->setValueNotifyingHost (q->convertTo0to1 ((float) v));
}

// --- signals
static float kickAt (double t, double bpm)
{
    const double beat = 60.0 / bpm;
    const double tb = std::fmod (t, beat);
    return (float) (std::sin (2 * PI * (50.0 * tb + 120.0 / 40.0 * (1.0 - std::exp (-tb * 40.0)))) * std::exp (-tb * 9.0) * 0.9);
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::Random rng (3);

    // ---------------------------------------------------------- 1) bass: pure sine + stereo low end
    {
        MixXProcessor p;
        setChoice (p, "profile", 3); // Bass
        runAnalysis (p, { [] (int i, float& l, float& r, float&)
        {
            const double t = i / SR;
            const double notes[] = { 55.0, 55.0, 65.4, 49.0 };
            const double f = notes[(int) (t / 0.5) % 4];
            const float env = (float) (0.4 + 0.6 * std::exp (-std::fmod (t, 0.5) * 3.0));
            l = (float) std::sin (2 * PI * f * t) * 0.5f * env;
            r = (float) std::sin (2 * PI * f * 1.012 * t + 1.3) * 0.5f * env;   // detuned R = stereo bass
        } }, 12.0, false);
        std::cout << "\n" << p.getReportText();
        CHECK (hasIssue (p, "without harmonics"), "bass: no harmonics detected");
        CHECK (hasIssue (p, "Stereo low end") || hasIssue (p, "is not mono"), "bass: stereo low end detected");
    }

    // ---------------------------------------------------------- 2) vocal: mud + rumble + big dynamics
    {
        MixXProcessor p;
        setChoice (p, "profile", 4); // Vocal
        setChoice (p, "daw", 1);     // Ableton
        auto tilt = std::make_shared<juce::IIRFilter>(), f1 = std::make_shared<juce::IIRFilter>(), f2 = std::make_shared<juce::IIRFilter>(),
             f3 = std::make_shared<juce::IIRFilter>(), mud = std::make_shared<juce::IIRFilter>(), hp = std::make_shared<juce::IIRFilter>();
        tilt->setCoefficients (juce::IIRCoefficients::makeLowPass (SR, 400, 0.5));
        f1->setCoefficients (juce::IIRCoefficients::makePeakFilter (SR, 700, 2, juce::Decibels::decibelsToGain (6.0f)));
        f2->setCoefficients (juce::IIRCoefficients::makePeakFilter (SR, 1200, 2, juce::Decibels::decibelsToGain (4.0f)));
        f3->setCoefficients (juce::IIRCoefficients::makePeakFilter (SR, 2800, 2, juce::Decibels::decibelsToGain (8.0f)));
        mud->setCoefficients (juce::IIRCoefficients::makePeakFilter (SR, 300, 1.2, juce::Decibels::decibelsToGain (12.0f)));
        hp->setCoefficients (juce::IIRCoefficients::makeHighPass (SR, 120, 0.7));
        auto ph = std::make_shared<double> (0.0);
        runAnalysis (p, { [=] (int i, float& l, float& r, float&) mutable
        {
            const double t = i / SR;
            const double f0 = 180 + 40 * std::sin (2 * PI * 0.3 * t);
            *ph += f0 / SR;
            float pulse = 0;
            if (*ph >= 1) { *ph -= 1; pulse = 1; }
            float v = tilt->processSingleSampleRaw (pulse * 4.0f) + pulse * 0.25f;        // ~ -6 dB/oct voice tilt
            v = hp->processSingleSampleRaw (f3->processSingleSampleRaw (f2->processSingleSampleRaw (f1->processSingleSampleRaw (v))));
            v = mud->processSingleSampleRaw (v);
            const double phrase = std::fmod (t, 2.0);
            const float loud = phrase < 1.0 ? 1.0f : 0.15f;              // loud / quiet phrases (16 dB)
            const float syl = (float) std::max (0.0, std::sin (PI * std::fmod (t, 0.25) / 0.25));
            v *= loud * syl * 0.5f;
            v += (float) std::sin (2 * PI * 32 * t) * 0.01f;             // rumble
            l = r = v;
        } }, 14.0, false);
        std::cout << "\n" << p.getReportText();
        CHECK (hasIssue (p, "Muddy"), "vocal: mud found");
        CHECK (hasIssue (p, "below 80 Hz"), "vocal: rumble -> high pass");
        CHECK (hasIssue (p, "too dynamic"), "vocal: compression suggested");
        CHECK (p.getReportText().contains ("EQ Eight"), "Ableton instructions used");

        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
        juce::File png ("/tmp/mx_gui.png");
        png.deleteFile();
        juce::FileOutputStream os (png);
        juce::PNGImageFormat().writeImageToStream (img, os);
    }

    // ---------------------------------------------------------- 3) master: clipped & squashed
    {
        MixXProcessor p;
        setChoice (p, "profile", 0);
        setChoice (p, "loudness", 1);   // streaming target: the -1 dBTP rule applies
        runAnalysis (p, { [&] (int i, float& l, float& r, float&)
        {
            const double t = i / SR;
            float k = kickAt (t, 126);
            float bass = (float) std::sin (2 * PI * 55 * t) * 0.5f;
            float hats = (rng.nextFloat() * 2 - 1) * 0.15f * (float) std::exp (-std::fmod (t + 0.238, 0.476) * 30);
            float m = (k + bass + hats) * 4.0f;                     // way too hot
            m = juce::jlimit (-1.0f, 1.0f, m);                       // hard clip
            l = r = m;
        } }, 12.0, false);
        std::cout << "\n" << p.getReportText();
        CHECK (hasIssue (p, "clipping"), "master: clipping found");
        CHECK (hasIssue (p, "Over-limited"), "master: over-limited (PLR) found");
        CHECK (hasIssue (p, "-1 dBTP"), "master: true peak warning");
    }

    // ---------------------------------------------------------- 4) bass vs kick on sidechain, no ducking
    {
        MixXProcessor p;
        setChoice (p, "profile", 3);
        runAnalysis (p, { [] (int i, float& l, float& r, float& sc)
        {
            const double t = i / SR;
            sc = kickAt (t, 124);
            const float bass = (float) (std::sin (2 * PI * 55 * t) + 0.4 * std::sin (2 * PI * 110 * t) + 0.2 * std::sin (2 * PI * 220 * t)) * 0.35f;
            l = r = bass;
        } }, 12.0, true);
        std::cout << "\n" << p.getReportText();
        CHECK (p.getReport().sidechainActive, "sidechain kick detected (" + juce::String (p.getReport().kickHits) + " hits)");
        CHECK (hasIssue (p, "masking each other"), "masking -> sidechain suggestion");
        CHECK (p.getReportText().contains ("Side Chain"), "Logic sidechain instructions");
    }

    // ---------------------------------------------------------- 5) same but already ducked -> OK
    {
        MixXProcessor p;
        setChoice (p, "profile", 3);
        runAnalysis (p, { [] (int i, float& l, float& r, float& sc)
        {
            const double t = i / SR;
            sc = kickAt (t, 124);
            const double beat = 60.0 / 124.0, tb = std::fmod (t, beat);
            const float duck = (float) (1.0 - 0.8 * std::exp (-tb * 8.0));   // ~ -14 dB at the hit, back by next beat
            const float bass = (float) (std::sin (2 * PI * 55 * t) + 0.4 * std::sin (2 * PI * 110 * t) + 0.2 * std::sin (2 * PI * 220 * t)) * 0.35f * duck;
            l = r = bass;
        } }, 12.0, true);
        std::cout << "duck measured: " << p.getReport().existingDuckDb << " dB\n";
        CHECK (! hasIssue (p, "masking each other"), "ducked bass: no masking warning");
    }

    // ---------------------------------------------------------- 6) loudness accuracy: -20 dBFS 1 kHz sine stereo = -20+3.01-0.691+... ~ -17.0 LUFS (K-weight ~+0.0 at 1k)
    {
        MixXProcessor p;
        setChoice (p, "profile", 5);
        runAnalysis (p, { [] (int i, float& l, float& r, float&)
        {
            const float s = (float) (std::sin (2 * PI * 997 * i / SR) * juce::Decibels::decibelsToGain (-20.0));
            l = r = s;
        } }, 8.0, false);
        const float L = p.getReport().integratedLufs;
        std::cout << "1 kHz -20 dBFS stereo sine -> " << L << " LUFS (expected about -20.0)\n";
        CHECK (std::abs (L - (-20.0f)) < 0.5f, "LUFS meter calibrated");
    }

    // ---------------------------------------------------------- 7) reference track
    {
        // write a reference file: bright, punchy "master" (kick + bass + hats) at about -8 LUFS
        juce::File refFile ("/tmp/mx_reference.wav");
        refFile.deleteFile();
        {
            const int n = (int) (SR * 20);
            juce::AudioBuffer<float> b (2, n);
            juce::Random rr (11);
            for (int i = 0; i < n; ++i)
            {
                const double t = i / SR;
                const float k = kickAt (t, 126);
                const float bass = (float) std::sin (2 * PI * 55 * t) * 0.3f;
                const float hat = (rr.nextFloat() * 2 - 1) * 0.25f * (float) std::exp (-std::fmod (t + 0.238, 0.476) * 25);
                const float pad = (float) (std::sin (2 * PI * 440 * t) + std::sin (2 * PI * 554 * t + 1)) * 0.06f;
                const float m = std::tanh ((k + bass + hat + pad) * 1.6f) * 0.85f;
                b.setSample (0, i, m + hat * 0.3f);
                b.setSample (1, i, m - hat * 0.3f);
            }
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os (refFile.createOutputStream().release());
            std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), SR, 2, 24, {}, 0));
            os.release();
            w->writeFromAudioSampleBuffer (b, 0, n);
        }

        MixXProcessor p;
        setChoice (p, "profile", 0);
        // the song: dull (no hats), quieter
        runAnalysis (p, { [] (int i, float& l, float& r, float&)
        {
            const double t = i / SR;
            const float m = (kickAt (t, 126) + (float) std::sin (2 * PI * 55 * t) * 0.3f) * 0.35f;
            l = r = m;
        } }, 12.0, false);

        p.loadReference (refFile);
        for (int i = 0; i < 100 && (p.isReferenceLoading() || ! p.getReferenceReport().valid); ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);

        std::cout << "\n" << p.getReportText();
        CHECK (p.getReferenceReport().valid, "reference analysed (" + juce::String (p.getReferenceReport().integratedLufs, 1) + " LUFS)");
        CHECK (p.getDiagnosis().usedReference, "diagnosis uses the reference");
        CHECK (hasIssue (p, "vs reference"), "EQ issues relative to the reference");
        CHECK (hasIssue (p, "Quieter than the reference"), "loudness compared to the reference");

        juce::MemoryBlock mb;
        p.getStateInformation (mb);
        MixXProcessor p2;
        p2.setStateInformation (mb.getData(), (int) mb.getSize());
        for (int i = 0; i < 100 && (p2.isReferenceLoading() || ! p2.getReferenceReport().valid); ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        CHECK (p2.getReferenceReport().valid, "reference reloaded from project state");

        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (400);
        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
        juce::File png ("/tmp/mx_gui_ref.png");
        png.deleteFile();
        juce::FileOutputStream os (png);
        juce::PNGImageFormat().writeImageToStream (img, os);
    }

    std::cout << "\n" << (failures == 0 ? juce::String ("ALL TESTS PASSED") : juce::String (failures) + " FAILURES") << "\n";
    return failures == 0 ? 0 : 1;
}
