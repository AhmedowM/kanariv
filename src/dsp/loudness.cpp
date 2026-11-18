#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <expected>
#include <kanariv/dsp/loudness.hpp>
#include <kanariv/fwd.hpp>
#include <kanariv/utils/error.hpp>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

namespace kanariv::dsp::loudness {

namespace {

// BS.1770-4 constants: the -0.691 dB offset makes a 997 Hz full-scale sine in
// one channel read 0 LKFS after K-weighting.
constexpr double kLoudnessOffset = -0.691;
constexpr double kAbsoluteGateLufs = -70.0;
constexpr double kRelativeGateLu = -10.0;

constexpr std::size_t kBlockHops = 4; // 400 ms block, 100 ms hop
constexpr double kHopSeconds = 0.1;

constexpr double kMinSampleRate = 8000.0;
constexpr double kMaxSampleRate = 384000.0;
constexpr unsigned kMaxChannels = 8;

constexpr unsigned kTruePeakPhases = 4;
constexpr unsigned kTruePeakTapsPerPhase = 16;
constexpr unsigned kTruePeakTaps = kTruePeakPhases * kTruePeakTapsPerPhase;

using TruePeakTaps = std::array<std::array<double, kTruePeakTapsPerPhase>, kTruePeakPhases>;

double powerToLufs(double power) noexcept {
    if (!(power > 0.0))
        return -std::numeric_limits<double>::infinity();
    return kLoudnessOffset + 10.0 * std::log10(power);
}

// One biquad section, transposed direct form II. Coefficients are normalized
// so a0 == 1; the state is double precision even though the input is float.
struct Biquad {
    double b0{1.0};
    double b1{0.0};
    double b2{0.0};
    double a1{0.0};
    double a2{0.0};
    double z1{0.0};
    double z2{0.0};

    double process(double x) noexcept {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// K-weighting stage 1: high shelf (+4 dB above ~1.7 kHz). Coefficients are the
// libebur128 / BS.1770 reference values, derived from the analog prototype at
// the given sample rate by the bilinear transform.
Biquad makeHighShelf(double sampleRate) {
    constexpr double f0 = 1681.974450955533;
    constexpr double gainDb = 3.999843853973347;
    constexpr double q = 0.7071752369554196;

    const double k = std::tan(std::numbers::pi * f0 / sampleRate);
    const double vh = std::pow(10.0, gainDb / 20.0);
    const double vb = std::pow(vh, 0.4996667741545416);
    const double a0 = 1.0 + k / q + k * k;

    Biquad b;
    b.b0 = (vh + vb * k / q + k * k) / a0;
    b.b1 = 2.0 * (k * k - vh) / a0;
    b.b2 = (vh - vb * k / q + k * k) / a0;
    b.a1 = 2.0 * (k * k - 1.0) / a0;
    b.a2 = (1.0 - k / q + k * k) / a0;
    return b;
}

// K-weighting stage 2: high pass at 38 Hz. Its numerator is [1, -2, 1] by
// construction, so only the denominator is normalized (as in libebur128).
Biquad makeHighPass(double sampleRate) {
    constexpr double f0 = 38.13547087602444;
    constexpr double q = 0.5003270373238773;

    const double k = std::tan(std::numbers::pi * f0 / sampleRate);
    const double a0 = 1.0 + k / q + k * k;

    Biquad b;
    b.b0 = 1.0;
    b.b1 = -2.0;
    b.b2 = 1.0;
    b.a1 = 2.0 * (k * k - 1.0) / a0;
    b.a2 = (1.0 - k / q + k * k) / a0;
    return b;
}

// 4x polyphase interpolator: a Blackman-windowed sinc with 16 taps per phase.
// Phase p produces the signal at 1/4 sample offsets, so the four phases cover
// the continuous waveform between input samples; each phase is normalized to
// unit DC gain. The filter delay is a whole-number-free 7.5 input samples,
// which shifts the oversampled stream in time but not its peak.
TruePeakTaps makeTruePeakTaps() {
    std::array<double, kTruePeakTaps> filter{};
    constexpr double center = static_cast<double>((kTruePeakTaps - kTruePeakPhases) / 2);
    constexpr double denom = static_cast<double>(kTruePeakTaps - 1);

    for (unsigned m = 0; m < kTruePeakTaps; ++m) {
        const double u = (static_cast<double>(m) - center) / kTruePeakPhases;
        const double sinc =
            (u == 0.0) ? 1.0 : std::sin(std::numbers::pi * u) / (std::numbers::pi * u);
        const double phase = 2.0 * std::numbers::pi * static_cast<double>(m) / denom;
        const double window = 0.42 - 0.5 * std::cos(phase) + 0.08 * std::cos(2.0 * phase);
        filter[m] = sinc * window;
    }

    TruePeakTaps taps{};
    for (unsigned p = 0; p < kTruePeakPhases; ++p) {
        double sum = 0.0;
        for (unsigned k = 0; k < kTruePeakTapsPerPhase; ++k) {
            taps[p][k] = filter[p + kTruePeakPhases * k];
            sum += taps[p][k];
        }
        for (unsigned k = 0; k < kTruePeakTapsPerPhase; ++k)
            taps[p][k] /= sum;
    }
    return taps;
}

} // namespace

struct LoudnessMeter::Impl {
    double sampleRate{0.0};
    unsigned channels{0};
    std::size_t hopSamples{0};

    std::vector<Biquad> shelf;
    std::vector<Biquad> highpass;

    std::size_t hopFilled{0};
    std::vector<double> hopEnergy;
    // Rolling window of the last kBlockHops completed hops, one row per slot.
    std::vector<double> hopRing;
    unsigned hopsSeen{0};
    unsigned ringSlot{0};

    std::vector<double> blocks;

    TruePeakTaps truePeakTaps{};
    std::vector<std::array<float, kTruePeakTaps>> history;
    std::size_t historyPos{0};
    double peakLinear{0.0};

    void emitHop() noexcept {
        double* slot = hopRing.data() + static_cast<std::size_t>(ringSlot) * channels;
        for (unsigned c = 0; c < channels; ++c)
            slot[c] = hopEnergy[c];
        std::fill(hopEnergy.begin(), hopEnergy.end(), 0.0);

        ringSlot = (ringSlot + 1) % kBlockHops;
        if (hopsSeen < kBlockHops)
            ++hopsSeen;
        if (hopsSeen < kBlockHops)
            return;

        double total = 0.0;
        for (double energy : hopRing)
            total += energy;
        blocks.push_back(total /
                         (static_cast<double>(kBlockHops) * static_cast<double>(hopSamples)));
    }

    void processFrame(const float* frame) noexcept {
        for (unsigned c = 0; c < channels; ++c) {
            const float sample = frame[c];
            history[c][historyPos] = sample;

            // All four output phases share the tap history; one backward walk
            // feeds four accumulators.
            double p0 = 0.0;
            double p1 = 0.0;
            double p2 = 0.0;
            double p3 = 0.0;
            std::size_t idx = historyPos;
            for (unsigned k = 0; k < kTruePeakTapsPerPhase; ++k) {
                const double x = static_cast<double>(history[c][idx]);
                p0 += truePeakTaps[0][k] * x;
                p1 += truePeakTaps[1][k] * x;
                p2 += truePeakTaps[2][k] * x;
                p3 += truePeakTaps[3][k] * x;
                idx = (idx == 0) ? kTruePeakTaps - 1 : idx - 1;
            }
            const double peak = std::max({std::abs(p0), std::abs(p1), std::abs(p2), std::abs(p3),
                                          std::abs(static_cast<double>(sample))});
            if (peak > peakLinear)
                peakLinear = peak;

            const double weighted =
                highpass[c].process(shelf[c].process(static_cast<double>(sample)));
            hopEnergy[c] += weighted * weighted;
        }
        historyPos = (historyPos + 1) % kTruePeakTaps;
    }
};

LoudnessMeter::LoudnessMeter(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

LoudnessMeter::LoudnessMeter(LoudnessMeter&&) noexcept = default;

LoudnessMeter& LoudnessMeter::operator=(LoudnessMeter&&) noexcept = default;

LoudnessMeter::~LoudnessMeter() = default;

kanariv::utils::Expected<std::unique_ptr<LoudnessMeter>> LoudnessMeter::create(double sampleRate,
                                                                               unsigned channels) {
    if (!std::isfinite(sampleRate) || sampleRate < kMinSampleRate || sampleRate > kMaxSampleRate)
        return std::unexpected{kanariv::utils::makeError(
            kanariv::utils::StatusCode::InvalidArg,
            "loudness sample rate must be finite and within [8000, 384000] Hz")};
    if (channels == 0 || channels > kMaxChannels)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                                         "loudness channel count must be 1..8")};

    auto impl = std::make_unique<Impl>();
    impl->sampleRate = sampleRate;
    impl->channels = channels;
    impl->hopSamples = static_cast<std::size_t>(std::llround(sampleRate * kHopSeconds));
    impl->shelf.assign(channels, makeHighShelf(sampleRate));
    impl->highpass.assign(channels, makeHighPass(sampleRate));
    impl->hopEnergy.assign(channels, 0.0);
    impl->hopRing.assign(kBlockHops * channels, 0.0);
    impl->history.resize(channels);
    for (auto& channelHistory : impl->history)
        channelHistory.fill(0.0f);
    impl->truePeakTaps = makeTruePeakTaps();
    return std::unique_ptr<LoudnessMeter>(new LoudnessMeter(std::move(impl)));
}

kanariv::utils::Expected<void> LoudnessMeter::addFrames(std::span<const float> interleaved) {
    if (!impl_)
        return std::unexpected{kanariv::utils::makeError(kanariv::utils::StatusCode::Internal,
                                                         "loudness meter is empty")};
    if (interleaved.size() % impl_->channels != 0)
        return std::unexpected{
            kanariv::utils::makeError(kanariv::utils::StatusCode::InvalidArg,
                                      "frame size must be a whole multiple of the channel count")};

    const std::size_t frames = interleaved.size() / impl_->channels;
    const float* frame = interleaved.data();
    for (std::size_t i = 0; i < frames; ++i) {
        impl_->processFrame(frame);
        frame += impl_->channels;
        if (++impl_->hopFilled == impl_->hopSamples) {
            impl_->hopFilled = 0;
            impl_->emitHop();
        }
    }
    return {};
}

std::span<const double> LoudnessMeter::blockEnergies() const noexcept {
    if (!impl_)
        return {};
    return impl_->blocks;
}

double LoudnessMeter::truePeakLinear() const noexcept {
    return impl_ ? impl_->peakLinear : 0.0;
}

LoudnessResult LoudnessMeter::result() const noexcept {
    LoudnessResult result;
    if (!impl_)
        return result;
    result.integratedLufs = integratedLoudness(impl_->blocks);
    result.truePeakDbtp = linearToDbtp(impl_->peakLinear);
    return result;
}

void LoudnessMeter::reset() noexcept {
    if (!impl_)
        return;
    for (auto& biquad : impl_->shelf)
        biquad.z1 = biquad.z2 = 0.0;
    for (auto& biquad : impl_->highpass)
        biquad.z1 = biquad.z2 = 0.0;
    std::fill(impl_->hopEnergy.begin(), impl_->hopEnergy.end(), 0.0);
    std::fill(impl_->hopRing.begin(), impl_->hopRing.end(), 0.0);
    for (auto& channelHistory : impl_->history)
        channelHistory.fill(0.0f);
    impl_->hopFilled = 0;
    impl_->hopsSeen = 0;
    impl_->ringSlot = 0;
    impl_->historyPos = 0;
    impl_->peakLinear = 0.0;
    impl_->blocks.clear();
}

double integratedLoudness(std::span<const double> blockEnergies) noexcept {
    // Pass 1: absolute gate. Blocks without audible content are dropped before
    // they can drag the mean down.
    double sum = 0.0;
    std::size_t count = 0;
    for (double energy : blockEnergies) {
        if (powerToLufs(energy) > kAbsoluteGateLufs) {
            sum += energy;
            ++count;
        }
    }
    if (count == 0)
        return -std::numeric_limits<double>::infinity();

    // Pass 2: relative gate, 10 LU below the loudness of the survivors.
    const double threshold = powerToLufs(sum / static_cast<double>(count)) + kRelativeGateLu;
    sum = 0.0;
    count = 0;
    for (double energy : blockEnergies) {
        if (powerToLufs(energy) > threshold) {
            sum += energy;
            ++count;
        }
    }
    if (count == 0)
        return -std::numeric_limits<double>::infinity();
    return powerToLufs(sum / static_cast<double>(count));
}

double linearToDbtp(double linear) noexcept {
    if (!(linear > 0.0))
        return -std::numeric_limits<double>::infinity();
    return 20.0 * std::log10(linear);
}

double replayGainDb(const LoudnessResult& result) noexcept {
    if (!std::isfinite(result.integratedLufs))
        return 0.0;
    // The peak limit only lowers the gain; an unmeasured peak (-inf dBTP)
    // leaves the loudness match untouched.
    return std::min(kReplayGain2ReferenceLufs - result.integratedLufs, -result.truePeakDbtp);
}

} // namespace kanariv::dsp::loudness
