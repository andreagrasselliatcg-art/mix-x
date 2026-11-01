# How Mix-X was trained

Mix-X's diagnosis is rule-based. "Training" means tuning every decision threshold (in `Source/Diagnostics.h`,
struct `Tuning`) against a labelled corpus. `Tests/Trainer.cpp` does this automatically.

## Corpus
- **Synthetic labelled mixes, about 420 per run.** For each of the 8 track types (Master, Drum Bus, Kick, Bass,
  Vocal, Lead, Pad, Hats/Perc) the trainer synthesises noise-based "music". Its 1/3-octave balance follows the
  reference curve, with random natural variation (±1.5 dB bumps, ±0.4 dB/oct tilt), a type-specific rhythmic or phrase
  envelope, random tempo (118-129 BPM), level (-12 to -22 dB RMS) and sample rate (44.1 / 48 / 96 kHz).
  Stereo content sits above 150 Hz, as in real mixes.
- **Defects of known size, injected one at a time:** EQ boost (+5 to +9 dB) and cut (-6 to -10 dB) at a random frequency, rumble,
  narrow resonance (+12 dB, Q 10), phrase-level dynamics (15 dB swings), hard clipping, over-limiting,
  low end only in the side channel, near-inverted phase, kick/bass masking via side-chain (and the same with ducking,
  which must *not* be flagged).
- **10 real Creative-Commons recordings** (electronic, drum & bass, pop, ragtime, waltz, orchestral, trumpet, 3 speech
  recordings) from the librosa example set. They are used as a sanity check: a good analyser must not bury real
  music in warnings. They also calibrated the *Generic* genre curve (half of their average deviation, clipped to ±6 dB).
- **Loudness targets per genre** (median of released club tracks): Tech House -8.9, House -9.2, Deep House -10.3,
  Indie Dance -9.9, Organic/Afro -9.8 to -10.1 LUFS.

## Objective
`recall (mean over defect types) − 0.20 × false alarms per clean mix − 0.10 × false alarms per mix − penalty for
too many issues on the real recordings`, optimised by coordinate descent over 21 thresholds.

## Results
| | before training | after |
|---|---|---|
| Defects detected (recall) | 90.9 % | **99.0 %** |
| False alarms per clean mix | 1.10 | **0.06** |
| False alarms per mix (all) | 0.75 | **0.02** |

## What training changed in the engine (not only thresholds)
- Clipping is now detected as **flat-topped waveforms**. Floating-point overs above 0 dBFS inside a DAW
  are not clipping. Intentional clippers under 0 dBFS are reported gently.
- Spectral level alignment uses the **median** of the core bands, so one problem no longer makes the rest
  of the spectrum look wrong.
- The FFT size scales with the sample rate (constant ~5.9 Hz resolution at 44.1-192 kHz).
- Removed two unreliable heuristics: "harsh distortion" from spectral flatness (fired on natural cymbal noise)
  and "kick transient vs body" from crest factor (inflated by the silence between hits).
- Kick and bass mids (250-1500 Hz, the "boxy" zone) are now judged.
- Dance-specific master tips only appear with the Club loudness target.
- The ducking measurement uses energy windows (0-60 ms after each kick vs 150-30 ms before), not a slow envelope.

Re-run: `cmake -B build -DMX_BUILD_TESTS=ON && cmake --build build --target MixXTrainer`, then
`MixXTrainer 8 /path/to/real/audio`.

## Club calibration on real releases (ATCG catalogue + peers)
13 finished club masters (about 67 min of audio: 9 ATCG productions/remixes/edits plus Kolter & Riton, Bob Sinclar, ICARO,
ZINGA and a SENATVS x Teddy-O remix) were analysed with `Tests/Calibrate.cpp`, both whole and on their loudest 32 s (the drop).

Measured (median, min to max):
| | full track | drop |
|---|---|---|
| Integrated loudness | -8.0 LUFS (-10.3 to -6.0) | -6.8 LUFS (-9.3 to -4.6) |
| Peak-to-loudness ratio | 8.4 dB (6.0 to 10.3) | 6.8 dB (5.4 to 9.2) |
| Low-end Side/Mid (<120 Hz) | | -37.6 dB (-41.7 to -17.6) |
| Stereo correlation | | 0.88 (0.68 to 0.97) |
| Sample peak | 0.0 dBFS (-0.5 to +1.5) | |

What changed:
- **Club target curve:** the master curve for all club genres is corrected by the measured average deviation.
  Released masters have far less infra-sub below 30 Hz (-12 to -20 dB), slightly leaner low mids at 250-630 Hz (-2 to -3.6 dB)
  and a brighter top above 6 kHz (+4 to +6 dB). The residual deviation of the 13 tracks from the new curve is 0.1-0.2 dB per band.
- **Per-band tolerance:** taken from the spread of the real tracks. Sub energy varies by ±4.6 dB between releases (a style
  choice); mids and highs by only ±1.5-2 dB.
- **Loudness targets** refer to the drop (published full-track genre medians + 1.2 LU, the measured drop offset).
- **Over-limiting** threshold for the club target: PLR < 5 dB (the most limited release measured was 5.4).
- **Peaks:** -1 dBTP is a streaming rule. For club files only overs above 0 dBFS are flagged.
- **Clipping** severity scales with how often it happens (a few flat-tops from a mastering clipper are a tip, not critical).

Before calibration these masters scored 51-74 with 3-6 issues each (mostly false alarms). After calibration they score
82-97 with 1-2 track-specific notes each. These scores are in-sample: the same tracks were used to calibrate,
so new tracks are the real test. The synthetic benchmark is unchanged (generic genre).
