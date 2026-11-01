#pragma once

#include "Analyzer.h"
#include <juce_core/juce_core.h>

namespace mx
{
enum class Profile { Master = 0, DrumBus, Kick, Bass, Vocal, Lead, Pad, HatsPerc };
enum class Daw { Logic = 0, Ableton, Generic };
enum class Category { EQ = 0, Compression, Sidechain, Saturation, Stereo, Loudness, Info };

struct Issue
{
    Category category = Category::Info;
    int severity = 1;               // 0 = OK/info, 1 = minor, 2 = important, 3 = critical
    juce::String title, why;
    juce::StringArray steps;
    float freqHz = 0.0f;            // marker for the spectrum view (0 = none)
};

struct Diagnosis
{
    std::vector<Issue> issues;
    int score = 0;
    std::array<float, kThirds> targetThirdsDb {};   // target curve aligned to the measured level
    bool valid = false;
    bool usedReference = false;
};

// Every decision threshold in one place (tuned by the trainer, see Tests/Trainer.cpp)
struct Tuning
{
    // values found by Tests/Trainer.cpp (coordinate descent on ~420 labelled mixes + 10 real recordings)
    float eqTol = 2.5f, eqTolBus = 3.0f;          // dB deviation before an EQ issue is reported
    float eqSev2 = 4.5f, eqSev3 = 6.0f;           // severity steps
    float eqRange = 12.0f, eqRangeLowExtra = 12.0f; // judge bands within this many dB of the target's loudest band (kick/bass: wider)
    float rumbleRel = -18.0f, subsonicRel = -8.0f;
    float resonanceDb = 8.0f, resonanceSev2 = 10.0f;
    float vocalDR = 9.0f, bassDR = 6.0f, leadDR = 8.0f, padDR = 9.0f, hatsDR = 10.0f;
    float kickCrestHigh = 20.0f, kickCrestLow = 9.0f, busCrestHigh = 21.0f, busCrestLow = 10.0f;
    float masterPlr = 7.0f, masterPlrClub = 5.0f;   // club: below every released master measured (min 5.4)
    float bassHarmonics = -20.0f, kickClickLow = -30.0f, kickClickHigh = -8.0f;
    float lowSide = -12.0f, correlation = 0.2f, narrowSide = -25.0f, monoSide = -20.0f;
    float overlapBass = -18.0f, overlapOther = -24.0f, duckMin = 3.0f, duckMax = 12.0f;
};
Tuning& tuning();
float clubTargetLufs (int genre);
float targetCurveDb (Profile p, int genre, float f);   // reference balance (1/3-octave energy, relative dB)

juce::StringArray profileNames();
juce::StringArray genreNames();
juce::StringArray dawNames();
juce::StringArray loudnessNames();
juce::String categoryName (Category c);

// reference: optional analysed reference track; when valid it replaces the generic target curve
Diagnosis diagnose (const Report& r, Profile profile, int genre, Daw daw, int loudnessTarget, const Report* reference = nullptr);
juce::String reportToText (const Diagnosis& d, const Report& r, Profile profile, Daw daw);
} // namespace mx
