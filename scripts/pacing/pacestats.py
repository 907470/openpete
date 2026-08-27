#!/usr/bin/env python3
"""pacestats.py — the statistics behind the pacing analysis suite.

Kept separate from the parser and the CLI so the maths can be tested against
synthetic inputs with known answers (test_pacestats.py). A wrong Allan
deviation is worse than none: it looks authoritative and nobody re-derives it.

Everything here takes plain sequences and returns plain numbers — no Log
objects, no I/O — which is what makes it testable in isolation.
"""
import math

try:
    import numpy as np
except ImportError:                                   # pragma: no cover
    np = None


# ---------------------------------------------------------------- summary

def percentile(vals, p):
    """Nearest-rank percentile on a sorted copy.

    Deliberately not interpolating: these are latency samples, and reporting
    a p99 that no tick actually experienced invites arguments about whether a
    threshold was crossed."""
    if not vals:
        return 0.0
    s = sorted(vals)
    i = min(len(s) - 1, max(0, int(math.ceil(p / 100.0 * len(s))) - 1))
    return float(s[i])


def median(vals):
    if not vals:
        return 0.0
    s = sorted(vals)
    n = len(s)
    return float(s[n // 2]) if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2.0


def robust_sigma(vals):
    """1.4826 x median-absolute-deviation: the width of the CORE.

    Immune to outliers by construction — over half the samples must move
    before it does — which is the point. Measured pacing distributions are a
    needle with heavy tails, where a plain stdev is carried almost entirely by
    a handful of spikes and reads as a broad distribution that does not exist.

    Read it as a scale estimate of the bulk, not as a corrected standard
    deviation: the 1.4826 factor equals sigma only for a normal distribution,
    and this one is not. It discards exactly the tail that causes a visible
    hitch, so it must never replace p99/max — hence `triple` below."""
    if not vals:
        return 0.0
    med = median(vals)
    return 1.4826 * median([abs(v - med) for v in vals])


def triple(vals):
    """The mandated S1/S2 reporting form: (robust sigma, p99, max |x|).

    Three different questions — how tight is normal operation, how bad is a
    typical bad tick, what is the worst thing that happened. No one of them
    substitutes for another, and a single stdev answers none of them."""
    return (robust_sigma(vals), percentile(vals, 99),
            max((abs(v) for v in vals), default=0.0))


def worst_window(times_ns, vals, window_ns=1_000_000_000):
    """Worst value within any sliding window of `window_ns`.

    A run's max says when the worst tick happened; this says whether the bad
    ticks cluster. Ten spikes inside one second is a stall; ten spread over a
    minute is scheduler noise, and they need different fixes."""
    if not vals:
        return 0.0, 0
    best, best_at, lo = 0.0, 0, 0
    cur = []
    for hi in range(len(vals)):
        while times_ns[hi] - times_ns[lo] > window_ns:
            lo += 1
        cur = vals[lo:hi + 1]
        m = max(abs(v) for v in cur)
        if m > best:
            best, best_at = m, times_ns[lo]
    return best, best_at


# ------------------------------------------------------------------ drift

def ols_slope(xs, ys):
    """Ordinary least squares slope + intercept. Returns (slope, intercept)."""
    n = len(xs)
    if n < 2:
        return 0.0, 0.0
    mx = sum(xs) / n
    my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return 0.0, my
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    slope = sxy / sxx
    return slope, my - slope * mx


def ols_slope_se(xs, ys):
    """Standard error of the OLS slope.

    Without it a drift figure is an unqualified point estimate, and over a
    short span with a wide residual core the estimate is dominated by noise —
    a 5-second window with a 1 ms spread resolves nothing near 1 ppm, but
    still prints a confident number with a verdict attached."""
    n = len(xs)
    if n < 3:
        return float("inf")
    slope, intercept = ols_slope(xs, ys)
    mx = sum(xs) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return float("inf")
    resid = sum((y - (slope * x + intercept)) ** 2 for x, y in zip(xs, ys))
    return math.sqrt(resid / (n - 2) / sxx)


def drift_ppm(indices, times_ns, nominal_period_ns):
    """Frequency error of the executed grid, in parts per million.

    Fitted over VBL INDEX rather than sample number so a gap (a level load, a
    paused span) does not silently compress the time axis and manufacture a
    drift that never happened."""
    slope, _ = ols_slope(indices, times_ns)
    if nominal_period_ns == 0:
        return 0.0, slope
    return (slope - nominal_period_ns) / nominal_period_ns * 1e6, slope


def drift_ppm_se(indices, times_ns, nominal_period_ns, blocks=True,
                 n_resample=200, seed=99):
    """One-sigma uncertainty on drift_ppm, same units.

    By default via a MOVING-BLOCK BOOTSTRAP of the residuals rather than the
    textbook OLS standard error. The textbook formula assumes independent
    residuals; pacing phase errors are strongly serially correlated — a busy
    stretch is late for many consecutive ticks — and under correlation it
    understates the uncertainty badly. Observed: two 55 s runs of the same
    configuration reported -0.20 +/- 0.14 and +2.24 +/- 0.52 ppm, disagreeing
    by far more than either interval allowed. An interval that excludes the
    truth is worse than a wide one, because a gate believes it.

    Blocks of ~sqrt(n) consecutive residuals preserve that correlation
    structure while resampling."""
    n = len(indices)
    if nominal_period_ns == 0 or n < 8:
        return float("inf")
    if not blocks:
        return ols_slope_se(indices, times_ns) / nominal_period_ns * 1e6

    slope, intercept = ols_slope(indices, times_ns)
    resid = [y - (slope * x + intercept) for x, y in zip(indices, times_ns)]
    blk = max(2, int(math.sqrt(n)))
    n_blocks = n // blk
    if n_blocks < 4:
        return ols_slope_se(indices, times_ns) / nominal_period_ns * 1e6

    import random
    rng = random.Random(seed)
    slopes = []
    for _ in range(n_resample):
        r = []
        while len(r) < n:
            st = rng.randrange(0, n - blk)
            r.extend(resid[st:st + blk])
        y = [slope * x + intercept + e for x, e in zip(indices, r[:n])]
        slopes.append(ols_slope(indices, y)[0])
    m = sum(slopes) / len(slopes)
    var = sum((v - m) ** 2 for v in slopes) / max(1, len(slopes) - 1)
    return math.sqrt(var) / nominal_period_ns * 1e6


def cusum(vals, threshold=4.0, min_seg=25, _base=0):
    """Change-point detection on the MEDIAN, binary-segmented.

    Finds where the typical value steps, which is what an undeclared grid
    re-anchor looks like: tight before, tight after, only the reference moved.
    No percentile and no histogram notices it.

    The statistic is a SIGN cumulative sum — sum of sign(v_i - median) — not
    a cumulative sum of deviations. That distinction is the whole thing on
    this data. Pacing distributions are a needle with heavy tails: the robust
    scale is ~1.6 us while a single scheduler outlier is ~17 ms, ten thousand
    times larger. A deviation-based statistic scaled by a robust sigma is
    then guaranteed to trip on any one outlier, and it did — 60 spurious
    "undeclared re-anchors" in a run that had none, while every segment's
    median was identical to within 0.2 us.

    Sign steps are +/-1 regardless of how extreme an outlier is, so the walk
    is bounded and a threshold on max|S| / sqrt(n) means what it says.

    Returns [(index, direction)] sorted by index, direction +1 / -1."""
    n = len(vals)
    if n < 2 * min_seg:
        return []
    med = median(vals)

    best_k, best_s, run = 0, 0.0, 0.0
    for i in range(n - 1):
        d = vals[i] - med
        run += (1.0 if d > 0 else (-1.0 if d < 0 else 0.0))
        if abs(run) > abs(best_s):
            best_s, best_k = run, i + 1

    if abs(best_s) / math.sqrt(n) < threshold:
        return []
    if best_k < min_seg or n - best_k < min_seg:
        return []

    left, right = median(vals[:best_k]), median(vals[best_k:])
    hits = [(_base + best_k, 1 if right > left else -1)]
    hits += cusum(vals[:best_k], threshold, min_seg, _base)
    hits += cusum(vals[best_k:], threshold, min_seg, _base + best_k)
    return sorted(hits)


# -------------------------------------------------------------- stability

def _phase_from_timestamps(times_ns, nominal_period_ns):
    """Time-error sequence x_i = t_i - i*T0, in seconds.

    Allan deviation is defined on the phase (time-error) series, not on the
    timestamps themselves; feeding raw timestamps yields a meaningless
    dominant ramp."""
    return [(t - i * nominal_period_ns) / 1e9 for i, t in enumerate(times_ns)]


def phase_runs(times_ns, nominal_period_ns, indices=None, breaks_ns=()):
    """Time-error series split into runs with NO discontinuity inside them.

    A second difference is what makes ADEV blind to a constant frequency
    offset, and it is equally what makes it maximally sensitive to a STEP.
    Two kinds of step exist in this log and neither is instability:

      * A GAP in the sequence. The phase is x = t - index*T0, and indexing by
        array POSITION instead of the VBL index renumbers every sample after
        an excluded one — so each VBL dropped as a transient injects a step of
        one whole nominal period. A 7179-VBL gate log excludes 9 of them; at
        16.7 ms each those steps are four orders of magnitude above the 1.4 us
        core this curve is meant to resolve.

      * A SLIP. The clamp discards wall time: the deadline jumps forward and
        the VBL index does not, so the phase steps by however far the loop was
        behind. The slip ledger already reports that as its own quantity.

    Either lands at long tau, which is the one part of the curve ADEV exists
    to report and the one part a histogram cannot show — so a level load would
    read as thermal wander. Segment instead, and let the caller say how much
    reach was lost.

    Returns (runs, cuts): the phase series in seconds, and a per-cut reason
    string ("gap" / "slip"). Reasons are counted rather than derived from the
    run count, because several slips can land inside one already-excluded
    stretch and the arithmetic then goes negative."""
    if indices is None:
        indices = range(len(times_ns))
    brk = sorted(breaks_ns)
    runs, cuts, cur = [], [], []
    prev_idx = None
    bi = 0
    for idx, t in zip(indices, times_ns):
        reason = None
        if prev_idx is not None and idx != prev_idx + 1:
            reason = "gap"
        while bi < len(brk) and brk[bi] <= t:
            if reason is None:
                reason = "slip"
            bi += 1
        if reason and cur:
            runs.append(cur)
            cuts.append(reason)
            cur = []
        cur.append((idx, t))
        prev_idx = idx
    if cur:
        runs.append(cur)
    return ([[(t - i * nominal_period_ns) / 1e9 for i, t in run]
             for run in runs], cuts)


def adev(times_ns, nominal_period_ns, taus=None, indices=None,
         breaks_ns=()):
    """Overlapping Allan deviation, per NIST SP 1065.

        sigma_y^2(tau) = 1/(2 (N-2m) tau^2) * sum (x_{i+2m} - 2x_{i+m} + x_i)^2

    tau is expressed in SAMPLES (m), converted to seconds on return. The
    interesting reading is the SLOPE: white phase noise falls as tau^-1, so a
    departure at large tau — a flicker floor, thermal wander, a drift ramp —
    is the actionable signal, and it is invisible in any histogram.

    `indices` and `breaks_ns` are passed to phase_runs: every triplet is taken
    from within one discontinuity-free run, and runs contribute to a common
    sum rather than to separate curves. Longest tau is therefore bounded by
    the LONGEST run, not by the span of the log.

    Returns [(tau_s, adev), ...]."""
    runs, _ = phase_runs(times_ns, nominal_period_ns, indices, breaks_ns)
    longest = max((len(r) for r in runs), default=0)
    if sum(len(r) for r in runs) < 5 or longest < 5:
        return []
    t0 = nominal_period_ns / 1e9
    if taus is None:
        taus, m = [], 1
        while m <= (longest - 1) // 2:
            taus.append(m)
            m = max(m + 1, int(m * 1.3))
    out = []
    for m in taus:
        acc, cnt = 0.0, 0
        for x in runs:
            for i in range(len(x) - 2 * m):
                d = x[i + 2 * m] - 2 * x[i + m] + x[i]
                acc += d * d
                cnt += 1
        if cnt < 1:
            continue
        tau = m * t0
        out.append((tau, math.sqrt(acc / (2.0 * cnt * tau * tau))))
    return out


def mdev(times_ns, nominal_period_ns, taus=None, indices=None,
         breaks_ns=()):
    """Overlapping modified Allan deviation.

    ADEV cannot tell white phase noise from flicker phase noise — both look
    close to tau^-1. MDEV separates them (tau^-1.5 vs tau^-1), and they call
    for different responses: white is the measurement floor and nothing to
    chase; flicker is a real instability.

    Run-split on the same terms as adev()."""
    runs, _ = phase_runs(times_ns, nominal_period_ns, indices, breaks_ns)
    longest = max((len(r) for r in runs), default=0)
    if sum(len(r) for r in runs) < 5 or longest < 5:
        return []
    t0 = nominal_period_ns / 1e9
    if taus is None:
        taus, m = [], 1
        while m <= (longest - 1) // 3:
            taus.append(m)
            m = max(m + 1, int(m * 1.3))
    out = []
    for m in taus:
        acc, cnt = 0.0, 0
        for x in runs:
            for j in range(len(x) - 3 * m + 1):
                inner = sum(x[i + 2 * m] - 2 * x[i + m] + x[i]
                            for i in range(j, j + m))
                acc += (inner / m) ** 2
                cnt += 1
        if cnt < 1:
            continue
        tau = m * t0
        out.append((tau, math.sqrt(acc / (2.0 * cnt * tau * tau))))
    return out


def adev_slope(points, lo_frac=0.0, hi_frac=1.0):
    """Log-log slope of an ADEV/MDEV curve over a fractional tau range.

    -1 is white phase noise (ADEV tracks a plain stddev there and says
    nothing new); flatter than about -0.5 at large tau means something is
    wandering."""
    pts = [(t, d) for t, d in points if t > 0 and d > 0]
    if len(pts) < 3:
        return 0.0
    lo = int(len(pts) * lo_frac)
    hi = max(lo + 2, int(len(pts) * hi_frac))
    seg = pts[lo:hi]
    slope, _ = ols_slope([math.log10(t) for t, _ in seg],
                         [math.log10(d) for _, d in seg])
    return slope


# --------------------------------------------------------------- spectrum

def spectrum(times_ns, vals, oversample=5, f_max_hz=None,
             max_freqs=20000):
    """Lomb-Scargle periodogram of a value series against real timestamps.

    Lomb-Scargle rather than an FFT because presents and ticks are not
    uniformly sampled once anything is shed or slipped, and resampling onto a
    uniform grid would invent structure at the resampling rate.

    A LINE here is a diagnosis, not a number: a FIFO beat against a monitor
    whose refresh is not the tick rate lands at the difference frequency,
    timer-granularity artefacts land at their harmonics, and a compositor
    shows up at its own cadence.

    Returns [(freq_hz, power), ...]."""
    if np is None or len(vals) < 8:
        return []
    t = np.asarray(times_ns, dtype=float) / 1e9
    y = np.asarray(vals, dtype=float)
    t = t - t[0]
    y = y - y.mean()
    span = t[-1] - t[0]
    if span <= 0 or not np.any(y):
        return []
    if f_max_hz is None:
        # Half the mean sample rate: the Nyquist limit of the average cadence.
        f_max_hz = 0.5 * len(t) / span
    f_min = 1.0 / span
    if f_max_hz <= f_min:
        return []
    # Grid spacing must OVERSAMPLE the natural resolution (1/span), not merely
    # meet it. The lines this exists to find sit at the very bottom of the
    # band — a 0.06 Hz beat over a 100 s run is the sixth resolvable
    # frequency — and a coarse linear grid puts them between bins, where they
    # vanish entirely rather than merely blurring.
    df = 1.0 / (oversample * span)
    freqs = np.arange(f_min, f_max_hz, df)
    if freqs.size < 4:
        return []
    if freqs.size > max_freqs:          # decimate the TOP, keep resolution low
        freqs = freqs[:max_freqs]
    try:
        from scipy.signal import lombscargle
        power = lombscargle(t, y, 2 * np.pi * freqs, normalize=True)
    except ImportError:                               # pragma: no cover
        return []
    return list(zip(freqs.tolist(), power.tolist()))


def spectrum_lines(points, n=5, min_power=0.05):
    """The strongest local maxima of a periodogram, strongest first."""
    peaks = []
    for i in range(1, len(points) - 1):
        f, p = points[i]
        if p >= points[i - 1][1] and p >= points[i + 1][1] and p >= min_power:
            peaks.append((f, p))
    peaks.sort(key=lambda fp: -fp[1])
    return peaks[:n]


# ------------------------------------------------------------------- diff

def bootstrap_ci(a, b, stat=median, n_resample=2000, conf=0.95, seed=12345):
    """Bootstrap confidence interval on stat(b) - stat(a).

    A prose claim that a change helped is not evidence; an interval that
    excludes zero is. Deterministic seed so a reported interval is
    reproducible from the same two logs."""
    if not a or not b:
        return 0.0, 0.0, 0.0
    import random
    rng = random.Random(seed)
    point = stat(b) - stat(a)
    deltas = []
    for _ in range(n_resample):
        ra = [a[rng.randrange(len(a))] for _ in range(len(a))]
        rb = [b[rng.randrange(len(b))] for _ in range(len(b))]
        deltas.append(stat(rb) - stat(ra))
    deltas.sort()
    lo = deltas[int((1 - conf) / 2 * n_resample)]
    hi = deltas[min(n_resample - 1, int((1 + conf) / 2 * n_resample))]
    return point, lo, hi
