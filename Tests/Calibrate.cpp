// Mix-X calibration: analyse a folder of finished club masters and measure how they deviate from the club target.
// Usage: MixXCalibrate <folder> [genre]
#include "Analyzer.h"
#include "Diagnostics.h"
#include <juce_audio_formats/juce_audio_formats.h>

using namespace mx;

static float median (std::vector<float> v)
{
    if (v.empty()) return 0;
    std::sort (v.begin(), v.end());
    const size_t m = v.size() / 2;
    return v.size() % 2 ? v[m] : 0.5f * (v[m - 1] + v[m]);
}

int main (int argc, char** argv)
{
    const juce::File dir (argc > 1 ? argv[1] : "/home/claude/corpus/club");
    const int genre = argc > 2 ? juce::String (argv[2]).getIntValue() : 2;
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    std::vector<std::array<float, kThirds>> devs;
    std::vector<float> lufs, plr, peak, dropLufs, dropPlr, lowSide, side, corr, dr, crest;
    juce::String reports;

    for (const auto& e : juce::RangedDirectoryIterator (dir, false, "*.wav"))
    {
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (e.getFile()));
        if (! rd) continue;
        const int n = (int) rd->lengthInSamples;
        const double sr = rd->sampleRate;
        juce::AudioBuffer<float> buf (2, n);
        rd->read (&buf, 0, n, 0, true, true);
        if (rd->numChannels == 1) buf.copyFrom (1, 0, buf, 0, 0, n);

        // loudest 32 s (the drop): sliding 1-s RMS
        const int sec = (int) sr, win = 32;
        std::vector<double> ms ((size_t) (n / sec));
        for (size_t k = 0; k < ms.size(); ++k)
        {
            double s = 0;
            for (int i = (int) k * sec; i < (int) (k + 1) * sec; ++i) s += buf.getSample (0, i) * buf.getSample (0, i) + buf.getSample (1, i) * buf.getSample (1, i);
            ms[k] = s;
        }
        int bestStart = 0; double best = -1;
        for (int k = 0; k + win <= (int) ms.size(); ++k)
        {
            double s = 0;
            for (int j = 0; j < win; ++j) s += ms[(size_t) (k + j)];
            if (s > best) { best = s; bestStart = k; }
        }
        const int dropLen = juce::jmin (n - bestStart * sec, win * sec);
        juce::AudioBuffer<float> drop (2, dropLen);
        for (int c = 0; c < 2; ++c) drop.copyFrom (c, 0, buf, c, bestStart * sec, dropLen);

        Analyzer an;
        const auto full = an.analyseBuffer (buf, sr);
        const auto d = an.analyseBuffer (drop, sr);

        std::array<float, kThirds> dev {};
        std::vector<float> diffs;
        const auto& fc = thirdOctaveCentres();
        for (int t = 0; t < kThirds; ++t)
            if (fc[(size_t) t] >= 40 && fc[(size_t) t] <= 12000) diffs.push_back (d.thirdsDb[(size_t) t] - targetCurveDb (Profile::Master, genre, fc[(size_t) t]));
        const float off = median (diffs);
        for (int t = 0; t < kThirds; ++t) dev[(size_t) t] = d.thirdsDb[(size_t) t] - targetCurveDb (Profile::Master, genre, fc[(size_t) t]) - off;
        devs.push_back (dev);

        lufs.push_back (full.integratedLufs); plr.push_back (full.peakDb - full.integratedLufs); peak.push_back (full.peakDb);
        dropLufs.push_back (d.integratedLufs); dropPlr.push_back (d.peakDb - d.integratedLufs);
        lowSide.push_back (d.lowSideToMidDb); side.push_back (d.sideToMidDb); corr.push_back (d.correlation);
        dr.push_back (d.dynRangeDb); crest.push_back (d.crestDb);

        const auto name = e.getFile().getFileNameWithoutExtension();
        std::cout << name.substring (0, 44).paddedRight (' ', 46)
                  << " full " << juce::String (full.integratedLufs, 1) << " LUFS  PLR " << juce::String (full.peakDb - full.integratedLufs, 1)
                  << " | drop @" << bestStart << "s " << juce::String (d.integratedLufs, 1) << " LUFS  PLR " << juce::String (d.peakDb - d.integratedLufs, 1)
                  << "  peak " << juce::String (d.peakDb, 2) << "  lowS/M " << juce::String (d.lowSideToMidDb, 1)
                  << "  S/M " << juce::String (d.sideToMidDb, 1) << "  corr " << juce::String (d.correlation, 2)
                  << "  DR " << juce::String (d.dynRangeDb, 1) << "  clips " << d.clipEvents << "\n";

        // what Mix-X (current thresholds) says about the drop
        const auto diag = diagnose (d, Profile::Master, genre, Daw::Logic, 0);
        reports << "\n##### " << name << "\n" << reportToText (diag, d, Profile::Master, Daw::Logic);
    }

    auto stats = [] (const char* label, const std::vector<float>& v)
    {
        auto s = v; std::sort (s.begin(), s.end());
        std::cout << "  " << juce::String (label).paddedRight (' ', 18) << " median " << juce::String (median (v), 2)
                  << "   min " << juce::String (s.front(), 2) << "   max " << juce::String (s.back(), 2) << "\n";
    };
    std::cout << "\nSTATS over " << devs.size() << " tracks\n";
    stats ("full LUFS", lufs); stats ("full PLR", plr); stats ("peak dBFS", peak);
    stats ("drop LUFS", dropLufs); stats ("drop PLR", dropPlr); stats ("drop low S/M", lowSide);
    stats ("drop S/M", side); stats ("drop corr", corr); stats ("drop DR", dr); stats ("drop crest", crest);

    std::cout << "\nBAND DEVIATION vs club curve (median-aligned, dB): mean / std\n";
    const auto& fc = thirdOctaveCentres();
    std::cout << "CLUBMEAN";
    for (int t = 0; t < kThirds; ++t)
    {
        double m = 0; for (auto& d : devs) m += d[(size_t) t]; m /= juce::jmax ((size_t) 1, devs.size());
        std::cout << " " << juce::String (m, 1);
    }
    std::cout << "\nCLUBSTD ";
    for (int t = 0; t < kThirds; ++t)
    {
        double m = 0, v = 0; for (auto& d : devs) m += d[(size_t) t]; m /= juce::jmax ((size_t) 1, devs.size());
        for (auto& d : devs) v += (d[(size_t) t] - m) * (d[(size_t) t] - m);
        std::cout << " " << juce::String (std::sqrt (v / juce::jmax ((size_t) 1, devs.size())), 1);
    }
    std::cout << "\nFREQS   ";
    for (int t = 0; t < kThirds; ++t) std::cout << " " << fc[(size_t) t];
    std::cout << "\n";
    juce::File ("/tmp/mx_club_reports.txt").replaceWithText (reports);
    return 0;
}
