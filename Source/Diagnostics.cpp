#include "Diagnostics.h"

namespace mx
{
using juce::String;
using juce::StringArray;

StringArray profileNames() { return { "Master / Mix", "Drum Bus", "Kick", "Bass", "Vocal", "Lead / Synth", "Pad", "Hats / Perc" }; }
StringArray genreNames()   { return { "Generic", "Tech House", "House", "Deep House", "Afro Tech", "Indie Tech" }; }
StringArray dawNames()     { return { "Logic Pro", "Ableton Live", "Other DAW" }; }
StringArray loudnessNames(){ return { "Club (genre median)", "Streaming (-14 LUFS)", "No target" }; }

// median integrated loudness of released club tracks per genre (Generic, Tech House, House, Deep House, Afro Tech, Indie Tech)
float clubTargetLufs (int genre)
{
    // drop / main-section loudness: published full-track genre medians + 1.2 LU (median drop-vs-track offset measured on 13 club masters)
    static const float t[] = { -8.0f, -7.7f, -8.0f, -9.1f, -8.6f, -8.7f };
    return t[juce::jlimit (0, 5, genre)];
}

Tuning& tuning()
{
    static Tuning t;
    return t;
}

String categoryName (Category c)
{
    switch (c)
    {
        case Category::EQ:          return "EQ";
        case Category::Compression: return "COMPRESSION";
        case Category::Sidechain:   return "SIDECHAIN";
        case Category::Saturation:  return "SATURATION";
        case Category::Stereo:      return "STEREO";
        case Category::Loudness:    return "LOUDNESS";
        case Category::Info:        return "INFO";
    }
    return {};
}

namespace
{
// ---------------------------------------------------------------- formatting
String hz (float f)
{
    if (f >= 1000.0f) return String (std::round (f / 100.0f) / 10.0f, 1) + " kHz";
    const float step = f < 100.0f ? 1.0f : 5.0f;
    return String ((int) (std::round (f / step) * step)) + " Hz";
}
String db (float g)
{
    const float r = std::round (g * 2.0f) / 2.0f;
    return (r > 0 ? "+" : "") + String (r, 1) + " dB";
}
String num (float v, int dec = 1) { return String (v, dec); }
String ms (float v) { return String ((int) std::round (v)) + " ms"; }

String profileWord (Profile p)
{
    static const char* w[] = { "master", "drum bus", "kick", "bass", "vocal", "synth/lead", "pad", "hats/percussion" };
    return w[(int) p];
}
String profileThe (Profile p) { return "the " + profileWord (p); }
String capitalised (const String& s) { return s.substring (0, 1).toUpperCase() + s.substring (1); }

// ---------------------------------------------------------------- targets
std::vector<std::pair<float, float>> targetPoints (Profile p)
{
    switch (p)
    {
        case Profile::Master:   return { { 20, -2 }, { 30, 3 }, { 60, 5 }, { 100, 3.5f }, { 200, 0 }, { 500, -3 }, { 1000, -5 }, { 2000, -7 }, { 4000, -9 }, { 8000, -12 }, { 12000, -15 }, { 16000, -20 }, { 20000, -30 } };
        case Profile::DrumBus:  return { { 20, -8 }, { 40, 2 }, { 60, 3 }, { 100, 1 }, { 250, -3 }, { 500, -6 }, { 1000, -7 }, { 3000, -7 }, { 6000, -8 }, { 10000, -10 }, { 16000, -15 }, { 20000, -24 } };
        case Profile::Kick:     return { { 20, -10 }, { 30, -2 }, { 50, 4 }, { 60, 5 }, { 100, 2 }, { 200, -6 }, { 400, -12 }, { 1000, -16 }, { 3000, -14 }, { 5000, -16 }, { 10000, -24 }, { 20000, -40 } };
        case Profile::Bass:     return { { 20, -12 }, { 30, -3 }, { 50, 3 }, { 80, 4 }, { 150, 0 }, { 300, -6 }, { 600, -12 }, { 1200, -16 }, { 3000, -24 }, { 8000, -36 }, { 20000, -50 } };
        case Profile::Vocal:    return { { 20, -40 }, { 50, -24 }, { 100, -6 }, { 200, 0 }, { 500, -1 }, { 1000, -3 }, { 3000, -4 }, { 5000, -6 }, { 8000, -10 }, { 12000, -14 }, { 16000, -20 }, { 20000, -30 } };
        case Profile::Lead:     return { { 20, -40 }, { 60, -24 }, { 120, -10 }, { 250, -2 }, { 500, 0 }, { 1000, 0 }, { 2000, -2 }, { 4000, -5 }, { 8000, -9 }, { 12000, -13 }, { 16000, -18 }, { 20000, -28 } };
        case Profile::Pad:      return { { 20, -36 }, { 60, -18 }, { 150, -4 }, { 300, 0 }, { 600, -1 }, { 1200, -3 }, { 3000, -6 }, { 6000, -10 }, { 10000, -14 }, { 16000, -20 }, { 20000, -30 } };
        case Profile::HatsPerc: return { { 20, -50 }, { 100, -30 }, { 300, -18 }, { 1000, -8 }, { 3000, -2 }, { 6000, 0 }, { 10000, 0 }, { 14000, -3 }, { 18000, -8 }, { 20000, -14 } };
    }
    return { { 20, 0 }, { 20000, 0 } };
}

float interpTarget (const std::vector<std::pair<float, float>>& pts, float f)
{
    if (f <= pts.front().first) return pts.front().second;
    if (f >= pts.back().first) return pts.back().second;
    for (size_t i = 1; i < pts.size(); ++i)
        if (f <= pts[i].first)
        {
            const float t = std::log2 (f / pts[i - 1].first) / std::log2 (pts[i].first / pts[i - 1].first);
            return pts[i - 1].second + t * (pts[i].second - pts[i - 1].second);
        }
    return pts.back().second;
}

void genreTweak (int genre, Profile p, float f, float& t)
{
    if (p != Profile::Master && p != Profile::DrumBus)
        return;
    if (genre == 0 && p == Profile::Master)
    {
        // "Generic": half of the measured average deviation of real (non-club) recordings from the club curve,
        // clipped to +-6 dB (see Tests/Trainer.cpp, CALIB line)
        static const float calib[kThirds] = { -6.0f, -6.0f, -6.0f, -6.0f, -6.0f, -6.0f, -6.0f, -5.7f, -6.0f, -4.2f, -2.5f, -1.1f, 2.8f, 2.9f, 3.6f, 3.5f, 3.2f, 3.4f, 4.0f, 2.4f, 0.9f, 1.1f, 0.7f, -0.0f, -1.1f, -2.1f, -2.5f, -3.2f, -4.0f, -5.3f, -6.0f };
        const auto& fcs = thirdOctaveCentres();
        int i = 0;
        while (i < kThirds - 2 && fcs[(size_t) i + 1] < f) ++i;
        const float x = juce::jlimit (0.0f, 1.0f, std::log2 (f / fcs[(size_t) i]) / std::log2 (fcs[(size_t) i + 1] / fcs[(size_t) i]));
        t += calib[i] + x * (calib[i + 1] - calib[i]);
        return;
    }
    if (genre == 0 && f < 45.0f)
        t -= 4.0f * std::log2 (45.0f / f);   // generic drum bus: sub rolls off

    if (genre != 0 && p == Profile::Master)
    {
        // club calibration: average deviation of 13 released club masters (ATCG catalogue + peers, drop sections)
        // from the original club curve (Tests/Calibrate.cpp); the 20 kHz edge band is halved
        static const float club[kThirds] = { -20.7f, -18.9f, -12.4f, -0.2f, 3.6f, 1.4f, -0.6f, -0.6f, -1.3f, -1.4f, -0.9f, -3.0f, -3.6f, -2.9f, -2.4f, -1.9f, -1.1f, -0.8f, -0.5f, -0.1f, 0.0f, 0.3f, 0.7f, 1.5f, 2.2f, 4.4f, 5.3f, 6.1f, 5.8f, 4.5f, 3.1f };
        const auto& fcs = thirdOctaveCentres();
        int i = 0;
        while (i < kThirds - 2 && fcs[(size_t) i + 1] < f) ++i;
        const float x = juce::jlimit (0.0f, 1.0f, std::log2 (f / fcs[(size_t) i]) / std::log2 (fcs[(size_t) i + 1] / fcs[(size_t) i]));
        t += club[i] + x * (club[i + 1] - club[i]);
    }
    const float oct = std::log2 (juce::jmax (1.0f, f / 1000.0f));
    switch (genre)
    {
        case 1: if (f < 120) t += 1.0f; break;                  // tech house: solid low end
        case 3: t -= 0.6f * oct; if (f < 120) t += 1.0f; break; // deep house: darker, warm
        case 4: if (f > 2000 && f < 6000) t += 1.0f; break;      // afro tech: percussive presence
        case 5: t += 0.5f * oct; if (f < 80) t -= 1.0f; break;   // indie: brighter, less sub
        default: break;
    }
}

// ---------------------------------------------------------------- DAW instructions
struct Steps
{
    Daw daw;
    Profile profile;

    String bell (float f, float g, float q) const
    {
        switch (daw)
        {
            case Daw::Logic:   return "Channel EQ: use a Peak (bell) band at " + hz (f) + ", Gain " + db (g) + ", Q " + num (q);
            case Daw::Ableton: return "EQ Eight: set a band to Bell at " + hz (f) + ", Gain " + db (g) + ", Q " + num (q);
            default:           return "Parametric EQ: bell at " + hz (f) + ", " + db (g) + ", Q " + num (q);
        }
    }

    String shelf (bool high, float f, float g) const
    {
        const String type = high ? "High Shelf" : "Low Shelf";
        switch (daw)
        {
            case Daw::Logic:   return "Channel EQ: " + type + " at " + hz (f) + ", Gain " + db (g);
            case Daw::Ableton: return "EQ Eight: set a band to " + type + " at " + hz (f) + ", Gain " + db (g);
            default:           return type + " at " + hz (f) + ", " + db (g);
        }
    }

    String highPass (float f) const
    {
        switch (daw)
        {
            case Daw::Logic:   return "Channel EQ: enable the Low Cut (first band) at " + hz (f) + ", slope 24 dB/Oct";
            case Daw::Ableton: return "EQ Eight: set band 1 to Low Cut (48 dB/oct) at " + hz (f);
            default:           return "High-pass (low cut) filter at " + hz (f) + ", 24 dB/oct";
        }
    }

    String gain (float g) const
    {
        switch (daw)
        {
            case Daw::Logic:   return "Gain plug-in (Utility > Gain) in the first slot: Gain " + db (g) + " (or turn down whatever is clipping upstream)";
            case Daw::Ableton: return "Utility as the first device: Gain " + db (g);
            default:           return "Input gain/trim: " + db (g);
        }
    }

    StringArray compressor (float thr, float ratio, float atkMs, float relMs, float makeup, float targetGr) const
    {
        StringArray s;
        const bool glue = profile == Profile::Master || profile == Profile::DrumBus;
        if (daw == Daw::Logic)
        {
            String model = "Platinum Digital";
            if (profile == Profile::Vocal) model = "Vintage Opto";
            else if (profile == Profile::Bass) model = "Vintage FET";
            else if (profile == Profile::Kick || glue) model = "Vintage VCA";
            s.add ("Insert Compressor (" + model + " model)");
            s.add ("Threshold " + db (thr) + ", Ratio " + num (ratio) + ":1, Attack " + ms (atkMs) + ", Release " + ms (relMs)
                   + ", Make Up " + db (makeup));
        }
        else if (daw == Daw::Ableton)
        {
            if (glue)
            {
                static const float atks[] = { 0.01f, 0.1f, 0.3f, 1, 3, 10, 30 };
                float a = atks[0];
                for (auto v : atks) if (std::abs (v - atkMs) < std::abs (a - atkMs)) a = v;
                const String rel = relMs < 150 ? "0.1 s" : relMs < 300 ? "0.2 s" : relMs < 500 ? "0.4 s" : "Auto";
                s.add ("Insert Glue Compressor");
                s.add ("Threshold " + db (thr) + ", Ratio " + String (ratio >= 3 ? 4 : 2) + ", Attack " + String (a, a < 1 ? 2 : 0)
                       + " ms, Release " + rel + ", Makeup " + db (makeup));
            }
            else
            {
                s.add ("Insert Compressor (Peak mode for percussive sounds, RMS for vocals/pads)");
                s.add ("Threshold " + db (thr) + ", Ratio " + num (ratio) + ":1, Attack " + ms (atkMs) + ", Release " + ms (relMs)
                       + ", Makeup " + db (makeup));
            }
        }
        else
        {
            s.add ("Compressor: Threshold " + db (thr) + ", Ratio " + num (ratio) + ":1, Attack " + ms (atkMs) + ", Release " + ms (relMs)
                   + ", Make-up " + db (makeup));
        }
        s.add ("Fine-tune the Threshold until you see about " + num (targetGr, 0) + " dB of gain reduction on the loudest parts");
        return s;
    }

    StringArray saturation (const String& character, float driveDb, int mixPct) const
    {
        StringArray s;
        if (daw == Daw::Logic)
        {
            s.add ("Insert ChromaGlow (Logic Pro 11): a " + character + "-style model, Drive about " + String ((int) (driveDb * 6)) + "%, Mix " + String (mixPct) + "%");
            s.add ("On older Logic versions: Overdrive or Phat FX (Distortion module) with low Drive, blended in parallel");
        }
        else if (daw == Daw::Ableton)
        {
            const String curve = character == "tape" ? "Soft Sine" : character == "tube" ? "Analog Clip" : "Bass Shaper";
            s.add ("Insert Saturator: " + curve + " curve, Drive " + db (driveDb) + ", Dry/Wet " + String (mixPct) + "%, Soft Clip ON");
            s.add ("Match the level with Saturator's Output (same loudness with bypass on/off)");
        }
        else
        {
            s.add (capitalised (character) + " saturation: Drive " + db (driveDb) + ", Mix " + String (mixPct) + "%, level-match the output");
        }
        return s;
    }

    StringArray sidechain (float depthDb, float relMs) const
    {
        StringArray s;
        if (daw == Daw::Logic)
        {
            s.add ("On the " + profileWord (profile) + " track insert Compressor (Platinum Digital model)");
            s.add ("Open the Side Chain menu at the top right of the plug-in window and choose the Kick track");
            s.add ("Ratio 4:1, Attack 0 - 1 ms, Release " + ms (relMs) + ", Knee 0.8");
            s.add ("Lower the Threshold until you see " + num (depthDb, 0) + " dB of gain reduction on every kick hit");
        }
        else if (daw == Daw::Ableton)
        {
            s.add ("On the " + profileWord (profile) + " track insert Compressor");
            s.add ("Open the Sidechain section (arrow at the top left), enable Sidechain and set Audio From: Kick track (Post FX)");
            s.add ("Ratio 4:1, Attack 0.1 ms, Release " + ms (relMs) + " (Auto Release OFF)");
            s.add ("Lower the Threshold until the GR meter reaches " + num (depthDb, 0) + " dB on every kick hit");
        }
        else
        {
            s.add ("Compressor side-chained from the Kick: Ratio 4:1, Attack ~1 ms, Release " + ms (relMs) + ", " + num (depthDb, 0) + " dB of GR on each kick");
        }
        return s;
    }

    String monoBass (float f) const
    {
        switch (daw)
        {
            case Daw::Ableton: return "Utility: enable Bass Mono and set the frequency to " + hz (f);
            case Daw::Logic:
                if (profile == Profile::Kick || profile == Profile::Bass)
                    return "Insert Direction Mixer (Imaging) with Spread 0: the track becomes mono";
                return "Logic has no stock 'bass mono': use an M/S plug-in (e.g. the free Voxengo MSED) with a low cut at " + hz (f) + " on the Side channel only, or make the bass tracks mono (Direction Mixer, Spread 0)";
            default: return "Make everything below " + hz (f) + " mono (bass mono / M/S EQ)";
        }
    }

    String width (float pct) const
    {
        switch (daw)
        {
            case Daw::Ableton: return "Utility: Width " + String ((int) pct) + "%";
            case Daw::Logic:   return "Direction Mixer (Imaging): Spread " + num (pct / 100.0f, 1);
            default:           return "Stereo width " + String ((int) pct) + "%";
        }
    }

    String limiter (float ceiling, float gainDb) const
    {
        switch (daw)
        {
            case Daw::Logic:   return "Last slot: Adaptive Limiter (or Limiter) with Out Ceiling " + db (ceiling) + ", Gain " + db (gainDb) + "; check with Loudness Meter";
            case Daw::Ableton: return "Last device: Limiter with Ceiling " + db (ceiling) + ", Gain " + db (gainDb) + " (True Peak ON in Live 12)";
            default:           return "Final limiter: Ceiling " + db (ceiling) + ", Gain " + db (gainDb);
        }
    }

    String transient (bool more) const
    {
        switch (daw)
        {
            case Daw::Logic:   return String ("Enveloper: Attack ") + (more ? "+30%" : "-20%") + " to " + (more ? "bring out" : "soften") + " the transient";
            case Daw::Ableton: return String ("Drum Buss: Transients ") + (more ? "+30%" : "-20%");
            default:           return String ("Transient shaper: attack ") + (more ? "+" : "-");
        }
    }
};

String rangeName (float f, bool excess)
{
    if (excess)
    {
        if (f < 60) return "Too much sub";
        if (f < 150) return "Boomy low end";
        if (f < 450) return "Muddy low mids";
        if (f < 1000) return "Boxy sound";
        if (f < 2500) return "Nasal / honky mids";
        if (f < 6000) return "Harsh upper mids";
        if (f < 10000) return "Sibilant / too bright";
        return "Too much air / hiss";
    }
    if (f < 60) return "Missing sub";
    if (f < 150) return "Low end lacks weight";
    if (f < 450) return "Thin, lacks warmth";
    if (f < 1000) return "Hollow mids";
    if (f < 2500) return "Mids lack definition";
    if (f < 6000) return "Lacks presence";
    if (f < 10000) return "Dull, lacks brightness";
    return "Lacks air";
}

float roundQ (float q) { return std::round (juce::jlimit (0.5f, 10.0f, q) * 10.0f) / 10.0f; }

double beatMs (const Report& r) { return 60000.0 / (r.bpm > 40 ? r.bpm : 124.0); }
} // namespace

float targetCurveDb (Profile p, int genre, float f)
{
    float t = interpTarget (targetPoints (p), f);
    genreTweak (genre, p, f, t);
    return t;
}

//==============================================================================
Diagnosis diagnose (const Report& r, Profile profile, int genre, Daw daw, int loudnessTarget, const Report* reference)
{
    const bool useRef = reference != nullptr && reference->valid;
    const Tuning& T = tuning();
    Diagnosis d;
    const Steps st { daw, profile };
    auto add = [&] (Category c, int sev, String title, String why, StringArray steps, float f = 0.0f)
    {
        d.issues.push_back ({ c, sev, std::move (title), std::move (why), std::move (steps), f });
    };

    if (! r.valid)
    {
        add (Category::Info, 0, "No analysis yet",
             "Press ANALYZE and play the most representative part of the song for at least 10 seconds.", {});
        d.score = 0;
        return d;
    }
    d.valid = true;
    d.usedReference = useRef;

    const auto& fc = thirdOctaveCentres();
    const auto pts = targetPoints (profile);

    // ------------------------------------------------------------ spectral balance
    std::array<float, kThirds> target {};
    float tMax = -100.0f;
    for (int i = 0; i < kThirds; ++i)
    {
        if (useRef)
        {
            // reference track spectrum, lightly smoothed
            const auto& t = reference->thirdsDb;
            const float a = t[(size_t) juce::jmax (0, i - 1)], b = t[(size_t) i], c = t[(size_t) juce::jmin (kThirds - 1, i + 1)];
            target[(size_t) i] = 0.25f * a + 0.5f * b + 0.25f * c;
        }
        else
        {
            target[(size_t) i] = interpTarget (pts, fc[(size_t) i]);
            genreTweak (genre, profile, fc[(size_t) i], target[(size_t) i]);
        }
        tMax = juce::jmax (tMax, target[(size_t) i]);
    }

    const float nyq = (float) r.sampleRate * 0.45f;
    // level alignment: median difference over the core bands, so one problem area doesn't shift everything else
    std::vector<float> diffs;
    for (int i = 0; i < kThirds; ++i)
        if (target[(size_t) i] >= tMax - 12.0f && fc[(size_t) i] >= 40 && fc[(size_t) i] <= 12000 && r.thirdsDb[(size_t) i] > -110)
            diffs.push_back (r.thirdsDb[(size_t) i] - target[(size_t) i]);
    float offset = 0.0f;
    if (! diffs.empty())
    {
        std::sort (diffs.begin(), diffs.end());
        const size_t m = diffs.size() / 2;
        offset = diffs.size() % 2 ? diffs[m] : 0.5f * (diffs[m - 1] + diffs[m]);
    }
    for (int i = 0; i < kThirds; ++i)
        d.targetThirdsDb[(size_t) i] = target[(size_t) i] + offset;

    std::array<float, kThirds> dev {}, sm {};
    std::array<bool, kThirds> use {};
    for (int i = 0; i < kThirds; ++i)
    {
        dev[(size_t) i] = r.thirdsDb[(size_t) i] - target[(size_t) i] - offset;
        use[(size_t) i] = target[(size_t) i] >= tMax - (useRef ? 30.0f : T.eqRange + ((profile == Profile::Kick || profile == Profile::Bass) ? T.eqRangeLowExtra : 0.0f)) && fc[(size_t) i] >= 30 && fc[(size_t) i] <= 16000
                          && fc[(size_t) i] < nyq && r.thirdsDb[(size_t) i] > -110;
    }
    for (int i = 0; i < kThirds; ++i)
    {
        const float a = dev[(size_t) juce::jmax (0, i - 1)], b = dev[(size_t) i], c = dev[(size_t) juce::jmin (kThirds - 1, i + 1)];
        sm[(size_t) i] = 0.25f * a + 0.5f * b + 0.25f * c;
    }

    const float tol = (profile == Profile::Master || profile == Profile::DrumBus) ? T.eqTolBus : T.eqTol;
    // per-band tolerance: on club masters, bands where released tracks differ a lot (e.g. the sub) get more room
    static const float clubStd[kThirds] = { 6.3f, 6.5f, 9.4f, 4.6f, 2.1f, 2.2f, 3.2f, 3.1f, 2.8f, 2.6f, 2.9f, 2.2f, 2.0f, 1.9f, 1.7f, 1.5f, 1.6f, 1.4f, 1.5f, 1.7f, 1.1f, 1.4f, 1.0f, 1.6f, 1.8f, 2.1f, 2.0f, 2.0f, 1.9f, 2.2f, 4.1f };
    std::array<float, kThirds> tolBand {};
    for (int i = 0; i < kThirds; ++i)
        tolBand[(size_t) i] = (! useRef && genre != 0 && profile == Profile::Master) ? juce::jmax (tol, 0.9f * clubStd[i]) : tol;
    struct Group { int start, end, peak; float peakDev; };
    std::vector<Group> groups;
    for (int i = 0; i < kThirds; ++i)
    {
        if (! use[(size_t) i] || std::abs (sm[(size_t) i]) <= tolBand[(size_t) i]) continue;
        const bool pos = sm[(size_t) i] > 0;
        Group g { i, i, i, sm[(size_t) i] };
        while (g.end + 1 < kThirds && use[(size_t) g.end + 1] && std::abs (sm[(size_t) g.end + 1]) > tolBand[(size_t) g.end + 1] && (sm[(size_t) g.end + 1] > 0) == pos)
        {
            ++g.end;
            if (std::abs (sm[(size_t) g.end]) > std::abs (g.peakDev)) { g.peak = g.end; g.peakDev = sm[(size_t) g.end]; }
        }
        groups.push_back (g);
        i = g.end;
    }
    std::sort (groups.begin(), groups.end(), [] (const Group& a, const Group& b) { return std::abs (a.peakDev) > std::abs (b.peakDev); });
    if (groups.size() > 5) groups.resize (5);

    for (const auto& g : groups)
    {
        const bool excess = g.peakDev > 0;
        const float f = fc[(size_t) g.peak];
        const float widthOct = (float) (g.end - g.start + 1) / 3.0f + 0.33f;
        const float q = roundQ (1.0f / (2.0f * std::sinh (std::log (2.0f) / 2.0f * widthOct)));
        const float gain = excess ? -juce::jmin (std::abs (g.peakDev) * 0.7f, 8.0f) : juce::jmin (std::abs (g.peakDev) * 0.6f, 5.0f);
        const float extra = tolBand[(size_t) g.peak] - tol;   // severity steps move with the band tolerance
        const int sev = std::abs (g.peakDev) > T.eqSev3 + extra ? 3 : std::abs (g.peakDev) > T.eqSev2 + extra ? 2 : 1;

        // a "lack" below an instrument's natural range is not a problem
        const float naturalLow = profile == Profile::Vocal ? 150.0f : profile == Profile::Lead ? 200.0f
                               : profile == Profile::Pad ? 150.0f : profile == Profile::HatsPerc ? 500.0f : 0.0f;
        if (! excess && f < naturalLow)
            continue;

        StringArray steps;
        if (! excess && f >= 9000)
            steps.add (st.shelf (true, fc[(size_t) g.start], gain));
        else if (excess && f <= 45 && (profile == Profile::Master || profile == Profile::DrumBus))
            steps.add (st.shelf (false, fc[(size_t) g.end] * 1.2f, gain));
        else
            steps.add (st.bell (f, gain, q));
        if (! excess && std::abs (g.peakDev) > 12.0f)
            steps.add ("EQ alone won't fix this: there is almost no energy here. Add harmonics with light saturation, or pick a richer sound/sample");
        else
            steps.add (excess ? "Before cutting, boost the band +6 dB and sweep slowly around " + hz (f) + ": where it sounds worst is where the cut goes"
                              : "Less is more: if a small boost isn't enough, fix it at the source (sample/preset) instead of pushing the EQ");

        add (Category::EQ, sev, rangeName (f, excess) + " around " + hz (f) + (useRef ? " (vs reference)" : ""),
             capitalised (profileThe (profile)) + " is about " + num (std::abs (g.peakDev), 1) + " dB "
                 + (excess ? "louder" : "quieter") + (useRef ? " than your reference track" : " than a reference balance") + " in this range ("
                 + hz (fc[(size_t) g.start]) + " - " + hz (fc[(size_t) g.end]) + ").",
             steps, f);
    }

    // rumble / sub-sonic
    {
        float hpf = 0;
        switch (profile)
        {
            case Profile::Vocal: hpf = 80; break;
            case Profile::Lead: hpf = 120; break;
            case Profile::Pad: hpf = 100; break;
            case Profile::HatsPerc: hpf = 300; break;
            default: hpf = 0; break;
        }
        if (hpf > 0)
        {
            const float low = r.bandEnergyDb (20, hpf * 0.8f);
            const float ref = r.bandEnergyDb (hpf * 1.5f, 5000);
            if (low - ref > T.rumbleRel)
                add (Category::EQ, 2, "Useless energy below " + hz (hpf),
                     "Low frequencies (rumble, reverb tails, bleed) are taking up room that belongs to the kick and bass, without adding anything.",
                     { st.highPass (hpf), "Listen in solo: if the sound loses body, move the cut 10-20 Hz lower" }, hpf);
        }
        else
        {
            const float sub = r.bandEnergyDb (5, 24);
            const float ref = r.bandEnergyDb (40, 100);
            if (sub - ref > T.subsonicRel)
                add (Category::EQ, 1, "Sub-sonic energy below 25 Hz",
                     "Speakers can't reproduce it, but it eats headroom and makes compressors and limiters work harder.",
                     { st.highPass (25) }, 25);
        }
    }

    // resonances (only where long-term averages are meaningful)
    if (profile == Profile::Master || profile == Profile::DrumBus || profile == Profile::Vocal || profile == Profile::HatsPerc)
    {
        const double binHz = r.sampleRate / r.fftSize;
        const int kB = (int) r.spectrumDb.size();
        // prefix sums of linear power: any smoothing window is O(1)
        std::vector<double> prefix ((size_t) kB + 1, 0.0);
        for (int k = 0; k < kB; ++k)
            prefix[(size_t) k + 1] = prefix[(size_t) k] + std::pow (10.0, r.spectrumDb[(size_t) k] / 10.0);
        auto smooth = [&] (int b, float octWidth)
        {
            const double f = b * binHz;
            const int lo = juce::jmax (1, (int) (f / std::pow (2.0, octWidth / 2) / binHz));
            const int hi = juce::jmin (kB - 1, (int) (f * std::pow (2.0, octWidth / 2) / binHz));
            const double sum = prefix[(size_t) hi + 1] - prefix[(size_t) lo];
            return (float) (10.0 * std::log10 (sum / juce::jmax (1, hi - lo + 1) + 1e-20));
        };
        struct Res { float f, excess; };
        std::vector<Res> res;
        const float fLo = profile == Profile::HatsPerc ? 2000.0f : 150.0f;
        float lastF = 0, lastEx = -1;
        for (int b = (int) (fLo / binHz); b < (int) (8000.0 / binHz); ++b)
        {
            const float ex = smooth (b, 1.0f / 12.0f) - smooth (b, 1.0f);
            const float f = (float) (b * binHz);
            if (ex > T.resonanceDb)
            {
                if (lastF > 0 && f / lastF < 1.26f) { if (ex > lastEx) { res.back() = { f, ex }; lastEx = ex; lastF = f; } continue; }
                res.push_back ({ f, ex }); lastF = f; lastEx = ex;
            }
        }
        std::sort (res.begin(), res.end(), [] (const Res& a, const Res& b) { return a.excess > b.excess; });
        for (size_t i = 0; i < juce::jmin ((size_t) 2, res.size()); ++i)
            add (Category::EQ, res[i].excess > T.resonanceSev2 ? 2 : 1, "Resonance at " + hz (res[i].f),
                 "A narrow peak " + num (res[i].excess, 1) + " dB above the surrounding spectrum: it sounds like a ringing tone or boom that tires the ear.",
                 { st.bell (res[i].f, -juce::jlimit (3.0f, 8.0f, res[i].excess * 0.7f), 6.0f),
                   daw == Daw::Ableton ? String ("If it only rings on certain notes, use Multiband Dynamics on that band instead of a static cut")
                                       : String ("If it only rings on certain notes, use Multipressor on that band instead of a static cut") },
                 res[i].f);
    }

    // ------------------------------------------------------------ dynamics / compression
    {
        const float DR = r.dynRangeDb;
        const float crest = r.crestDb;
        const float blockCrest = r.medianBlockCrestDb;
        const float sixteenth = (float) (beatMs (r) / 4.0);
        auto makeup = [&] (float thr, float ratio) { return juce::jlimit (0.0f, 8.0f, (1.0f - 1.0f / ratio) * juce::jmax (0.0f, r.blockRmsP90Db + 3.0f - thr) * 0.6f); };

        switch (profile)
        {
            case Profile::Vocal:
                if (DR > T.vocalDR)
                {
                    const float ratio = DR > 11 ? 4.0f : 3.0f, thr = r.blockRmsP60Db + 2.0f;
                    add (Category::Compression, DR > 11 ? 3 : 2, "The vocal is too dynamic",
                         "There are " + num (DR, 1) + " dB between the quiet and loud parts: some words disappear in the mix while others jump out.",
                         st.compressor (thr, ratio, 8, 80, makeup (thr, ratio), DR > 11 ? 6 : 4));
                }
                else if (DR < 2.5f && blockCrest < 8.0f)
                    add (Category::Compression, 2, "Over-compressed vocal",
                         "Only " + num (DR, 1) + " dB of dynamics and flattened transients: the vocal sounds flat and tiring.",
                         { "Lower the Ratio of the existing compression (e.g. from 6:1 to 3:1) or raise the Threshold by 3-4 dB",
                           "Or use parallel compression (Mix / Dry-Wet at 50-60%)" });
                break;

            case Profile::Bass:
                if (DR > T.bassDR)
                {
                    const float thr = r.blockRmsP60Db + 1.0f;
                    add (Category::Compression, DR > 10 ? 3 : 2, "Unstable bass level",
                         "The bass notes vary by " + num (DR, 1) + " dB: the low end wobbles and the limiter works in bursts.",
                         st.compressor (thr, 4, 15, juce::jlimit (40.0f, 200.0f, sixteenth), makeup (thr, 4), 4));
                }
                else if (blockCrest < 5.0f)
                    add (Category::Compression, 1, "Very squashed bass",
                         "The bass has almost no dynamics: fine for sub-heavy styles, but it costs groove and attack.",
                         { "Raise the compressor Attack to 20-30 ms to let the note attack through" });
                break;

            case Profile::Kick:
                if (crest < T.kickCrestLow)
                    add (Category::Compression, 2, "Over-compressed kick: dull transient",
                         "Crest factor of only " + num (crest, 1) + " dB: the kick is losing punch.",
                         { "Raise the compressor Attack to 20-30 ms or lower the Ratio", st.transient (true) });
                break;

            case Profile::DrumBus:
                if (crest > T.busCrestHigh)
                {
                    const float thr = r.blockRmsP90Db;
                    add (Category::Compression, 1, "Drums don't sound glued",
                         "Crest factor of " + num (crest, 1) + " dB: the hits feel disconnected. A light bus compressor makes them sound like one kit.",
                         st.compressor (thr, 2, 30, (float) (beatMs (r) / 2.0), 1.5f, 2.5f));
                }
                else if (crest < T.busCrestLow)
                    add (Category::Compression, 2, "Over-compressed drum bus",
                         "Crest factor of " + num (crest, 1) + " dB: the transients are squashed and the groove loses energy.",
                         { "Bring the bus gain reduction down to 2-3 dB, with an Attack of at least 10-30 ms", st.transient (true) });
                break;

            case Profile::Lead:
                if (DR > T.leadDR)
                {
                    const float thr = r.blockRmsP60Db + 2.0f;
                    add (Category::Compression, 2, "Unstable lead level",
                         "The synth varies by " + num (DR, 1) + " dB: some notes poke out of the mix.",
                         st.compressor (thr, 3, 5, 60, makeup (thr, 3), 3));
                }
                break;

            case Profile::Pad:
                if (DR > T.padDR)
                {
                    const float thr = r.blockRmsP60Db + 2.0f;
                    add (Category::Compression, 1, "Unstable pad level",
                         "The pad varies by " + num (DR, 1) + " dB: gentle compression keeps it steady behind the mix.",
                         st.compressor (thr, 2, 30, 200, makeup (thr, 2), 2));
                }
                break;

            case Profile::HatsPerc:
                if (DR > T.hatsDR)
                    add (Category::Compression, 1, "Uneven percussion",
                         "Some hits are much louder than others (" + num (DR, 1) + " dB of variation).",
                         st.compressor (r.blockRmsP90Db, 3, 2, 40, 1, 3));
                break;

            case Profile::Master:
            {
                const float plr = r.peakDb - r.integratedLufs;
                if (plr < (loudnessTarget == 0 ? T.masterPlrClub : T.masterPlr))
                    add (Category::Loudness, 3, "Over-limited master",
                         "Peak-to-loudness ratio (PLR) of " + num (plr, 1) + " dB: the mix is limited too hard, losing punch and causing ear fatigue.",
                         { "Reduce the limiter Gain by " + num (8.0f - plr, 1) + " dB",
                           "Ease off the master bus compression (2 dB of GR at most)" });
                if (loudnessTarget == 0 && crest > 15.0f && r.integratedLufs < -12.0f)
                    add (Category::Compression, 1, "Mix not cohesive",
                         "Very wide dynamics for a dance master: a light bus compressor will glue the mix together.",
                         st.compressor (r.blockRmsP90Db, 2, 30, 200, 1.5f, 2));
                break;
            }
        }
    }

    // ------------------------------------------------------------ loudness (master)
    if (profile == Profile::Master && (loudnessTarget != 2 || useRef))
    {
        const float targetL = useRef ? reference->integratedLufs : loudnessTarget == 0 ? clubTargetLufs (genre) : -14.0f;
        const float diff = targetL - r.integratedLufs;
        if (diff > 2.0f)
            add (Category::Loudness, loudnessTarget == 0 && diff > 5 ? 2 : 1,
                 String (useRef ? "Quieter than the reference (" : "Below the loudness target (") + num (r.integratedLufs, 1) + " LUFS)",
                 String (useRef ? "Your reference plays at " : "Your target is ") + num (targetL, 1) + " LUFS integrated: you are about " + num (diff, 1) + " dB short.",
                 { st.limiter (-1.0f, juce::jmin (diff, 6.0f)),
                   "If you need more than 4-5 dB, get part of it from a light bus compressor first (2:1, 2 dB GR), then the limiter" });
        else if (diff < -1.5f && useRef)
            add (Category::Loudness, 1, "Louder than the reference",
                 "Your track is " + num (-diff, 1) + " dB louder than the reference (" + num (targetL, 1) + " LUFS): you may be over-limiting.",
                 { st.limiter (-1.0f, diff) });
        else if (diff < -1.5f && loudnessTarget == 1)
            add (Category::Loudness, 1, "Louder than needed for streaming",
                 "Spotify, Apple Music & co. will turn the track down by about " + num (-diff, 1) + " dB: you are giving up dynamics for nothing.",
                 { st.limiter (-1.0f, diff) });
        if (loudnessTarget == 0)
        {
            if (r.peakDb > 0.05f)
                add (Category::Loudness, 1, "Overs above 0 dBFS",
                     "Peaks reach " + num (r.peakDb, 1) + " dBFS. Club files usually sit at -0.3 to 0 dBFS; anything above can clip in the DJ's player or after conversion.",
                     { st.limiter (-0.3f, 0.0f), "For the streaming version use a -1 dBTP ceiling instead" });
        }
        else if (r.peakDb > -1.0f)
            add (Category::Loudness, 2, "Peaks above -1 dBTP",
                 "Max peak at " + num (r.peakDb, 1) + " dBFS: after MP3/AAC encoding this can distort.",
                 { st.limiter (-1.0f, 0.0f) });
    }

    // ------------------------------------------------------------ saturation / distortion
    if (r.clipEvents > 0 && r.peakDb < -0.05f)
        add (Category::Saturation, r.clipEvents > 200 ? 2 : 1, "Hard clipping below 0 dBFS (" + String (r.clipEvents) + " events)",
             "Flat-topped peaks at " + num (r.peakDb, 1) + " dBFS: a clipper or saturator is clipping. Fine if intentional, but keep it to 1-2 dB of clipping on transients.",
             { "Lower the input of the clipper/saturator until it only shaves the loudest transients",
               "Compare bypassed vs active at the same loudness: if the transients sound duller, back off" });
    else if (r.clipEvents > 0)
        add (Category::Saturation, r.clipEvents / juce::jmax (1.0, r.seconds) > 2.0 ? 3 : r.clipEvents / juce::jmax (1.0, r.seconds) > 0.3 ? 2 : 1,
             "Digital clipping (" + String (r.clipEvents) + " events)",
             "The waveform is flattened at 0 dBFS for several consecutive samples: hard, unwanted distortion.",
             { st.gain (r.clipEvents / juce::jmax (1.0, r.seconds) > 0.3 ? -3.0f : -1.0f),
               profile == Profile::Master ? st.limiter (loudnessTarget == 0 ? -0.3f : -1.0f, 0.0f) : String ("Check plug-ins with high Drive/Output before Mix-X") });

    if (profile == Profile::Bass && r.bassHarmonicRatioDb < T.bassHarmonics)
        add (Category::Saturation, 2, "Bass without harmonics",
             "Energy above 150 Hz is " + num (-r.bassHarmonicRatioDb, 0) + " dB below the fundamental: on phones, laptops and small speakers the bass disappears.",
             st.saturation ("tube", 6, 50), 200);
    if (profile == Profile::Kick)
    {
        if (r.kickClickRatioDb < T.kickClickLow)
        {
            auto s = st.saturation ("tape", 4, 40);
            s.insert (0, st.bell (3500, 3, 1.2f));
            add (Category::Saturation, 2, "Kick has no click",
                 "Little energy between 1 and 5 kHz: the kick works on big speakers but gets lost in the mix and on small ones.", s, 3500);
        }
        else if (r.kickClickRatioDb > T.kickClickHigh)
            add (Category::EQ, 1, "Kick too clicky",
                 "The top-end attack is almost as loud as the body: it can sound harsh.",
                 { st.bell (4000, -3, 1.0f) }, 4000);
    }
    if ((profile == Profile::Vocal || profile == Profile::Lead || profile == Profile::Pad) && r.medianBlockCrestDb > 12.0f && r.flatnessPresence < 0.08f)
        add (Category::Saturation, 1, "Clean but lacking density",
             "A touch of saturation adds harmonics and helps " + profileThe (profile) + " sit forward without turning it up.",
             st.saturation ("tape", 3, 35));
    if (profile == Profile::Master && loudnessTarget == 0 && r.crestDb > 16.0f && r.integratedLufs < -12.0f)
        add (Category::Saturation, 1, "Master: a bit of analog colour",
             "Very light tape saturation on the master adds density and cohesion.",
             st.saturation ("tape", 2, 30));

    // ------------------------------------------------------------ sidechain
    {
        const bool needsSc = profile == Profile::Bass || profile == Profile::Pad || profile == Profile::Lead || profile == Profile::Vocal;
        const float rel = (float) (std::round (beatMs (r) * 0.4 / 5.0) * 5.0);
        if (r.sidechainActive)
        {
            const float overlap = r.lowOverlapDb, duck = r.existingDuckDb;
            if (needsSc && overlap > (profile == Profile::Bass ? T.overlapBass : T.overlapOther) && duck < T.duckMin)
            {
                const float depth = juce::jlimit (3.0f, 10.0f, std::round (4.0f + (overlap + 12.0f) * 0.4f));
                add (Category::Sidechain, profile == Profile::Bass ? 3 : 2, "Kick and " + profileWord (profile) + " are masking each other",
                     "On every kick hit " + profileThe (profile) + " has low-end energy (below 150 Hz) only " + num (-overlap, 1)
                         + " dB under the kick, and right now it only ducks by " + num (juce::jmax (0.0f, duck), 1) + " dB.",
                     st.sidechain (depth, rel), 60);
            }
            else if (duck > T.duckMax)
                add (Category::Sidechain, 2, "Sidechain ducking too deep",
                     capitalised (profileThe (profile)) + " drops by " + num (duck, 1) + " dB on every kick: the pumping is overdone and the low end has holes.",
                     { "Raise the Threshold of the side-chained compressor until GR comes down to 6-8 dB",
                       "Use a shorter Release (" + ms (rel) + ") so the sound comes back before the next hit" });
            else if (duck >= T.duckMin)
                add (Category::Info, 0, "Sidechain OK",
                     capitalised (profileThe (profile)) + " ducks by " + num (duck, 1) + " dB on the kick hits (" + num (r.kicksPerSecond * 60.0f, 0) + " hits/min).", {});
        }
        else if (needsSc)
        {
            StringArray s;
            if (daw == Daw::Logic) s.add ("In the Mix-X window, top right: Side Chain menu > Kick track, then run ANALYZE again");
            else if (daw == Daw::Ableton) s.add ("In the Mix-X device open the Sidechain settings (if your Live version supports it), pick the Kick track, then run ANALYZE again");
            else s.add ("Route the kick track to Mix-X's side-chain input and analyze again");
            add (Category::Info, 0, "Check the clash with the kick",
                 "Connect the Kick to Mix-X's side-chain: it will measure how much the kick and " + profileThe (profile) + " overlap and tell you how much ducking you need.", s);
        }
    }

    // ------------------------------------------------------------ stereo
    {
        const bool lowCritical = profile == Profile::Master || profile == Profile::Bass || profile == Profile::Kick || profile == Profile::DrumBus;
        if (r.lowSideToMidDb > T.lowSide && lowCritical)
            add (Category::Stereo, profile == Profile::Master ? 3 : 2, "Stereo low end",
                 "Below 120 Hz there is a lot of stereo energy (Side at " + num (r.lowSideToMidDb, 1) + " dB vs Mid): in clubs and in mono the low end loses power.",
                 { st.monoBass (120) }, 120);
        if (r.correlation < T.correlation)
            add (Category::Stereo, r.correlation < 0.0f ? 3 : 2, "Phase problems / poor mono compatibility",
                 "Stereo correlation " + num (r.correlation, 2) + ": in mono (phones, clubs, radio) parts of the sound cancel out.",
                 { "Check stereo wideners, choruses and delays with inverted channels", st.width (75),
                   daw == Daw::Ableton ? String ("Check in mono with Utility (Mono ON)") : String ("Check in mono with the Gain plug-in (Mono) or the Correlation Meter") });
        if ((profile == Profile::Kick || profile == Profile::Bass) && r.sideToMidDb > T.monoSide)
            add (Category::Stereo, 1, capitalised (profileThe (profile)) + " is not mono",
                 "For kick and bass, the centre of the stereo image is the most solid choice.", { st.monoBass (120) });
        if ((profile == Profile::Pad || profile == Profile::Master) && r.sideToMidDb < T.narrowSide)
            add (Category::Stereo, 1, "Very narrow stereo image",
                 "Almost everything sits in the centre: there is room to widen.",
                 { st.width (125), daw == Daw::Logic ? String ("Alternatively: Chorus or Ensemble at 20-30% mix") : String ("Alternatively: Chorus-Ensemble with Dry/Wet at 20-30%") });
        if (std::abs (r.dcOffset) > 0.005f)
            add (Category::Stereo, 1, "DC offset",
                 "The signal isn't centred on zero: it wastes headroom.",
                 { daw == Daw::Ableton ? String ("Utility: enable DC") : st.highPass (20) });
    }

    // ------------------------------------------------------------ reference comparisons
    if (useRef)
    {
        if (profile == Profile::Master || profile == Profile::DrumBus)
        {
            const float plr = r.peakDb - r.integratedLufs, refPlr = reference->peakDb - reference->integratedLufs;
            if (plr < refPlr - 2.0f)
                add (Category::Compression, 2, "More squashed than the reference",
                     "Your peak-to-loudness ratio is " + num (plr, 1) + " dB vs " + num (refPlr, 1) + " dB in the reference: less punch at the same loudness.",
                     { "Reduce limiter/bus compression gain reduction by about " + num (refPlr - plr, 1) + " dB", st.transient (true) });
            else if (plr > refPlr + 3.0f)
                add (Category::Compression, 1, "More dynamic than the reference",
                     "Your peak-to-loudness ratio is " + num (plr, 1) + " dB vs " + num (refPlr, 1) + " dB: the reference is denser and will sound fuller.",
                     st.compressor (r.blockRmsP90Db, 2, 30, 200, 1.5f, 2));
        }
        const float wDiff = r.sideToMidDb - reference->sideToMidDb;
        if (r.sideToMidDb > -60.0f && reference->sideToMidDb > -60.0f && std::abs (wDiff) > 4.0f
            && profile != Profile::Kick && profile != Profile::Bass)
            add (Category::Stereo, 1, wDiff < 0 ? "Narrower than the reference" : "Wider than the reference",
                 "Side/Mid is " + num (r.sideToMidDb, 1) + " dB vs " + num (reference->sideToMidDb, 1) + " dB in the reference.",
                 { st.width (wDiff < 0 ? 120.0f : 85.0f) });
    }

    // ------------------------------------------------------------ score
    std::stable_sort (d.issues.begin(), d.issues.end(), [] (const Issue& a, const Issue& b) { return a.severity > b.severity; });
    int score = 100;
    for (auto& i : d.issues) score -= i.severity == 3 ? 15 : i.severity == 2 ? 8 : i.severity == 1 ? 3 : 0;
    d.score = juce::jlimit (5, 100, score);
    return d;
}

String reportToText (const Diagnosis& d, const Report& r, Profile profile, Daw daw)
{
    String t;
    t << "MIX-X by Tune Talks - report\n";
    t << "Track: " << profileNames()[(int) profile] << "   DAW: " << dawNames()[(int) daw]
      << "   Score: " << d.score << "/100\n";
    if (r.valid)
    {
        t << "Analyzed " << String (r.seconds, 1) << " s"
          << (r.bpm > 0 ? "  |  " + String (r.bpm, 1) + " BPM" : String()) << "\n";
        t << "Integrated loudness " << String (r.integratedLufs, 1) << " LUFS  |  Peak " << String (r.peakDb, 1)
          << " dBFS  |  Crest " << String (r.crestDb, 1) << " dB  |  Dynamics " << String (r.dynRangeDb, 1)
          << " dB  |  Correlation " << String (r.correlation, 2) << "\n";
    }
    t << "\n";

    int n = 1;
    for (const auto& i : d.issues)
    {
        const String sev = i.severity == 3 ? "CRITICAL" : i.severity == 2 ? "IMPORTANT" : i.severity == 1 ? "TIP" : "OK";
        t << n++ << ". [" << categoryName (i.category) << " - " << sev << "] " << i.title << "\n";
        t << "   " << i.why << "\n";
        int k = 1;
        for (const auto& s : i.steps)
            t << "   " << String (k++) << ") " << s << "\n";
        t << "\n";
    }
    return t;
}
} // namespace mx
