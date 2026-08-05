#!/usr/bin/env python3
"""Compare a Sound-cdev recording against the signal that was fed in.

Sample-exactness is not available and not wanted here: between the input
WAV and the captured playback the guest resamples to its own rate, drives
an aggressive record AGC, quantises to 8 bits and plays back through the
DSP.  What must survive all of that is the SHAPE of the utterance, so that
is what is measured:

  1. Envelope correlation AGAINST A DECOY.  Both signals are reduced to a
     20 ms RMS envelope, normalised, and cross-correlated over the
     plausible lag range (the recording starts at an unknown offset).  The
     recording is then scored against the utterance that was actually fed
     AND against a different one, and must match the former by a clear
     margin.
     A bare threshold cannot do this job: measured on the two assets in
     tests/data/speech, two DIFFERENT utterances already score 0.592,
     because both are speech and speech envelopes resemble each other.  Any
     absolute bar low enough to accept a real recording (degraded by 8-bit
     quantisation, an AGC and resampling) would also accept a recording of
     the wrong sound.  Scoring against a decoy removes the need to guess
     the bar, and calibrates itself to however much the guest mangles the
     signal.

  2. Dynamic range.  The gap between the quietest and loudest tenth of the
     recording.  This is the assertion that catches the failure the test
     was written for: fed silence, the guest's AGC returns a full-scale
     flat roar measuring 0.5 dB here, while any real recording of speech
     with pauses in it measures tens of dB.

Both are reported before either is asserted, so a failure says what the
recording actually looked like rather than only that it was wrong.
"""

import math
import struct
import sys
import wave

WIN_MS = 20.0
# How much better the recording must match what was fed than a different
# utterance.  Self-vs-self scores 1.000 and self-vs-decoy 0.592 on these
# assets, so a real recording has ~0.4 of headroom to lose to the guest's
# 8-bit path before this trips.
MIN_DECOY_MARGIN = 0.15
# Speech with pauses, even through 8-bit and an AGC, keeps well more than
# this. A flat roar has ~0.5 dB.
MIN_DYNAMIC_DB = 12.0


def read_mono(path):
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            raise SystemExit(f"{path}: expected PCM16, got {w.getsampwidth()*8}-bit")
        ch, rate, n = w.getnchannels(), w.getframerate(), w.getnframes()
        raw = struct.unpack(f"<{n * ch}h", w.readframes(n))
    return list(raw[0::ch]), rate


def envelope(samples, rate):
    win = max(1, int(rate * WIN_MS / 1000.0))
    out = []
    for i in range(0, len(samples) - win + 1, win):
        acc = 0
        for v in samples[i : i + win]:
            acc += v * v
        out.append(math.sqrt(acc / win))
    return out


def normalise(env):
    peak = max(env) if env else 0.0
    return [v / peak for v in env] if peak > 0 else env


def dynamic_range_db(env):
    """Loudest tenth over quietest tenth, in dB."""
    if not env:
        return 0.0
    s = sorted(env)
    k = max(1, len(s) // 10)
    quiet = sum(s[:k]) / k
    loud = sum(s[-k:]) / k
    if quiet <= 0:
        return 999.0
    return 20.0 * math.log10(loud / quiet)


def best_correlation(a, b):
    """Peak normalised cross-correlation of two envelopes over all lags."""
    if len(a) < 4 or len(b) < 4:
        return 0.0, 0
    best, best_lag = -1.0, 0
    span = min(len(a), len(b))
    for lag in range(0, max(1, len(b) - span // 2)):
        n = min(len(a), len(b) - lag)
        if n < span // 2:
            break
        x = a[:n]
        y = b[lag : lag + n]
        mx, my = sum(x) / n, sum(y) / n
        num = sum((x[i] - mx) * (y[i] - my) for i in range(n))
        dx = math.sqrt(sum((v - mx) ** 2 for v in x))
        dy = math.sqrt(sum((v - my) ** 2 for v in y))
        if dx <= 0 or dy <= 0:
            continue
        c = num / (dx * dy)
        if c > best:
            best, best_lag = c, lag
    return best, best_lag


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: compare.py <input.wav> <decoy.wav> <playback.wav>")
    src, decoy, rec = sys.argv[1], sys.argv[2], sys.argv[3]

    s_samples, s_rate = read_mono(src)
    d_samples, d_rate = read_mono(decoy)
    r_samples, r_rate = read_mono(rec)
    s_env = normalise(envelope(s_samples, s_rate))
    d_env = normalise(envelope(d_samples, d_rate))
    r_env = normalise(envelope(r_samples, r_rate))

    src_dyn = dynamic_range_db(envelope(s_samples, s_rate))
    rec_dyn = dynamic_range_db(envelope(r_samples, r_rate))
    corr, lag = best_correlation(s_env, r_env)
    decoy_corr, _ = best_correlation(d_env, r_env)

    print(f"  input    {len(s_samples)/s_rate:5.2f} s @ {s_rate} Hz, dynamic range {src_dyn:5.1f} dB")
    print(f"  playback {len(r_samples)/r_rate:5.2f} s @ {r_rate} Hz, dynamic range {rec_dyn:5.1f} dB")
    print(f"  correlation vs the utterance fed  {corr:.3f} at lag {lag * WIN_MS / 1000.0:.2f} s")
    print(f"  correlation vs a decoy utterance  {decoy_corr:.3f}")

    failures = []
    if rec_dyn < MIN_DYNAMIC_DB:
        failures.append(
            f"the recording has {rec_dyn:.1f} dB of dynamic range (want >= {MIN_DYNAMIC_DB:.0f}); "
            "the input had {:.1f} dB, so the structure of the utterance did not survive".format(src_dyn)
        )
    if corr < decoy_corr + MIN_DECOY_MARGIN:
        failures.append(
            f"the recording matches a DIFFERENT utterance about as well as the one that was fed "
            f"({corr:.3f} vs {decoy_corr:.3f}, want a margin of {MIN_DECOY_MARGIN:.2f}); "
            "what came back is not what went in"
        )
    for f in failures:
        print(f"FAIL: {f}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
