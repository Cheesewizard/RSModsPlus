// Offline test for Audio::HumRemover (DLL/Audio/HumRemover.hpp). No game, no hardware.
//
//   HumRemoverTest.exe                 synthetic buzz + guitar notes; asserts lock, buzz removal, tone kept
//   HumRemoverTest.exe take.wav [...]  also runs real 16-bit PCM takes and reports what it finds
//
// The synthetic buzz copies Philip's measured one (2026-09-24): odd harmonics of a 49.93 Hz grid up to
// ~5 kHz plus a little even-harmonic hum, drifting slowly, under guitar notes with gaps between them.
// Exit code 0 = pass. Build + run via run-hum-test.sh.
#include "../HumRemover.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <vector>

static constexpr double PI = 3.14159265358979323846;
static constexpr uint32_t SR = 48000;
static constexpr uint32_t PACKET = 128;

static void Run(Audio::HumRemover& hum, const std::vector<float>& in, std::vector<float>& out, float nominal)
{
	out.resize(in.size());
	for (size_t start = 0, packets = 0; start < in.size(); start += PACKET, ++packets)
	{
		const size_t n = std::min<size_t>(PACKET, in.size() - start);
		hum.BeginBlock(SR, nominal, 1);
		for (size_t i = 0; i < n; ++i) out[start + i] = hum.Process(0, in[start + i]);
		hum.EndBlock();
		if (packets % 16 == 15) hum.Analyze();   // the game's worker runs about this often
	}
}

// Power of the lines at k * f0 (odd and even) in [loHz, hiHz) over a segment, via one long Hann FFT.
static double LinePower(const std::vector<float>& x, size_t from, size_t count, double f0, double loHz, double hiHz)
{
	if (f0 < 1.0) return 1e-30;
	size_t n = 1; while (n * 2 <= count) n *= 2;
	std::vector<std::complex<double>> buf(n);
	for (size_t i = 0; i < n; ++i) buf[i] = x[from + i] * (0.5 - 0.5 * std::cos(2 * PI * i / n));
	// small DFT-free FFT (same algorithm as the remover's)
	for (size_t i = 1, j = 0; i < n; ++i) { size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(buf[i], buf[j]); }
	for (size_t len = 2; len <= n; len <<= 1)
	{
		const std::complex<double> wl = std::polar(1.0, -2 * PI / len);
		for (size_t i = 0; i < n; i += len) { std::complex<double> w = 1; for (size_t j = 0; j < len / 2; ++j) { auto u = buf[i + j], v = buf[i + j + len / 2] * w; buf[i + j] = u + v; buf[i + j + len / 2] = u - v; w *= wl; } }
	}
	const double bin = static_cast<double>(SR) / n;
	double total = 0;
	for (int k = 1; k * f0 < hiHz; ++k)
	{
		if (k * f0 < loHz) continue;
		const int c = static_cast<int>(std::lround(k * f0 / bin));
		for (int b = c - 3; b <= c + 3; ++b) if (b > 0 && b < static_cast<int>(n / 2)) total += std::norm(buf[b]);
	}
	return total + 1e-30;
}

static bool Synthetic()
{
	std::mt19937 rng(7);
	std::uniform_real_distribution<double> phaseDist(0, 2 * PI);
	std::normal_distribution<double> noise(0, 1);
	const double seconds = 40.0;
	const size_t total = static_cast<size_t>(seconds * SR);
	std::vector<float> buzz(total), guitar(total, 0.0f), in(total);

	// Buzz: odd harmonics to 5 kHz (1/sqrt(k)) plus weak even ones, grid drifting 49.93 +/- 0.01 Hz over a minute.
	std::vector<double> amp(101), phase(101);
	for (int k = 1; k <= 100; ++k) { amp[k] = (k % 2 ? 0.003 : 0.0006) / std::sqrt(static_cast<double>(k)); phase[k] = phaseDist(rng); }
	double theta = 0;
	for (size_t i = 0; i < total; ++i)
	{
		const double t = static_cast<double>(i) / SR;
		theta += 2 * PI * (49.93 + 0.01 * std::sin(2 * PI * t / 60.0)) / SR;
		double s = 0;
		for (int k = 1; k <= 100; ++k) s += amp[k] * std::sin(k * theta + phase[k]);
		buzz[i] = static_cast<float>(s + 0.0002 * noise(rng));
	}
	// Guitar: plucked notes (decaying harmonic tones, slight inharmonicity) 3 s each with 2 s gaps, from 10 s on.
	const double notes[] = { 82.41, 110.0, 146.83, 196.0, 246.94, 329.63 };
	for (int n = 0; n < 6; ++n)
	{
		const size_t start = static_cast<size_t>((10.0 + n * 5.0) * SR);
		for (size_t i = 0; i < static_cast<size_t>(3.0 * SR) && start + i < total; ++i)
		{
			const double t = static_cast<double>(i) / SR;
			double s = 0;
			for (int h = 1; h <= 30 && notes[n] * h < 10000; ++h)
				s += (0.3 / h) * std::exp(-t * (1.5 + 0.3 * h)) * std::sin(2 * PI * notes[n] * h * (1 + 0.0002 * h * h) * t);
			guitar[start + i] = static_cast<float>(s);
		}
	}
	for (size_t i = 0; i < total; ++i) in[i] = buzz[i] + guitar[i];

	printf("[synthetic] signal built\n");
	auto hum = std::make_unique<Audio::HumRemover>();
	std::vector<float> out;
	Run(*hum, in, out, 50.0f);
	printf("[synthetic] processed\n");
	const auto status = hum->GetStatus();
	printf("[synthetic] locked %.4f Hz (true 49.93 +/- 0.01), %d lines up to %.0f Hz, strongest %.1f dB, %u analyses\n",
		status.fundamentalHz, status.lines, status.highestHz, status.strongestDb, status.analyses);
	bool ok = true;
	if (std::fabs(status.fundamentalHz - 49.93) > 0.03) { printf("FAIL [synthetic]: fundamental off\n"); ok = false; }
	if (status.lines < 30 || status.highestHz < 4500) { printf("FAIL [synthetic]: did not find the upper buzz lines\n"); ok = false; }

	// Buzz removal on the idle stretch 5..9 s (locked by then), below and above 2 kHz.
	const size_t from = 5 * SR, count = 4 * SR;
	const double f0 = status.fundamentalHz;
	const double low = 10 * std::log10(LinePower(out, from, count, f0, 40, 2000) / LinePower(in, from, count, f0, 40, 2000));
	const double high = 10 * std::log10(LinePower(out, from, count, f0, 2000, 5100) / LinePower(in, from, count, f0, 2000, 5100));
	printf("[synthetic] buzz line power change: below 2 kHz %.1f dB, 2-5 kHz %.1f dB\n", low, high);
	if (low > -20 || high > -15) { printf("FAIL [synthetic]: buzz not removed enough\n"); ok = false; }

	// Tone kept: the published notch set's response at every partial of every guitar note E2..E6, and band averages.
	double worst = 0, sum = 0; int partials = 0;
	for (int midi = 40; midi <= 88; ++midi)
		for (int h = 1; h <= 20; ++h)
		{
			const double f = 440.0 * std::pow(2.0, (midi - 69) / 12.0) * h;
			if (f > 8000) break;
			const double db = hum->ResponseDb(f);
			worst = std::min(worst, db); sum += db; ++partials;
		}
	// The previous fixed comb (Q = 30 at exact 50 Hz multiples to 2 kHz) on the same partials, for comparison.
	double oldSum = 0;
	for (int midi = 40; midi <= 88; ++midi)
		for (int h = 1; h <= 20; ++h)
		{
			const double f = 440.0 * std::pow(2.0, (midi - 69) / 12.0) * h;
			if (f > 8000) break;
			std::complex<double> hz = 1.0;
			const double w = 2 * PI * f / SR;
			const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2 * w);
			for (int k = 1; k * 50.0 <= 2000.0; ++k)
			{
				const double w0 = 2 * PI * 50.0 * k / SR, alpha = std::sin(w0) / 60.0;
				hz *= (1.0 - 2 * std::cos(w0) * z1 + z2) / (1 + alpha - 2 * std::cos(w0) * z1 + (1 - alpha) * z2);
			}
			oldSum += (std::max)(-60.0, 20 * std::log10(std::abs(hz)));   // a partial exactly on a notch is -inf; cap it
		}
	printf("[synthetic] guitar partials E2..E6 (%d): mean %.2f dB, worst %.1f dB (previous fixed comb: mean %.2f dB)\n", partials, sum / partials, worst, oldSum / partials);
	for (const auto& band : { std::pair<double, double>{ 100, 500 }, { 500, 1000 }, { 1000, 2000 }, { 2000, 5000 } })
	{
		double s = 0; int n = 0;
		for (double f = band.first; f < band.second; f += 0.25) { s += std::pow(10.0, hum->ResponseDb(f) / 10.0); ++n; }
		printf("[synthetic] band %4.0f-%4.0f Hz average power %.2f dB\n", band.first, band.second, 10 * std::log10(s / n));
		if (10 * std::log10(s / n) < -1.0) { printf("FAIL [synthetic]: band loses more than 1 dB\n"); ok = false; }
	}
	if (sum / partials < -1.0) { printf("FAIL [synthetic]: guitar partials lose too much on average\n"); ok = false; }
	return ok;
}

static void SaveWav16(const std::string& path, const float* x, size_t n)
{
	FILE* f = nullptr;
	if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return;
	const uint32_t dataBytes = static_cast<uint32_t>(n * 2), riff = 36 + dataBytes, fmtSize = 16, rate = SR, byteRate = SR * 2;
	const uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
	fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmtSize, 4, 1, f);
	fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
	for (size_t i = 0; i < n; ++i) { const int16_t v = static_cast<int16_t>(std::lround(std::clamp(x[i], -1.0f, 1.0f) * 32767.0f)); fwrite(&v, 2, 1, f); }
	fclose(f);
}

static bool LoadWav16(const char* path, std::vector<float>& mono)
{
	FILE* f = nullptr;
	if (fopen_s(&f, path, "rb") != 0 || !f) return false;
	std::vector<unsigned char> data;
	unsigned char chunk[65536]; size_t got;
	while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) data.insert(data.end(), chunk, chunk + got);
	fclose(f);
	if (data.size() < 44 || std::memcmp(data.data(), "RIFF", 4) != 0) return false;
	size_t pos = 12; int channels = 1, bits = 16;
	while (pos + 8 <= data.size())
	{
		const uint32_t size = data[pos + 4] | (data[pos + 5] << 8) | (data[pos + 6] << 16) | (data[pos + 7] << 24);
		if (std::memcmp(&data[pos], "fmt ", 4) == 0) { channels = data[pos + 10] | (data[pos + 11] << 8); bits = data[pos + 22] | (data[pos + 23] << 8); }
		if (std::memcmp(&data[pos], "data", 4) == 0)
		{
			if (bits != 16) return false;
			const size_t frames = std::min<size_t>(size, data.size() - pos - 8) / (2 * channels);
			mono.resize(frames);
			for (size_t i = 0; i < frames; ++i)
			{
				const unsigned char* p = &data[pos + 8 + i * 2 * channels];
				mono[i] = static_cast<int16_t>(p[0] | (p[1] << 8)) / 32768.0f;
			}
			return true;
		}
		pos += 8 + size + (size & 1);
	}
	return false;
}

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	bool ok = Synthetic();
	for (int a = 1; a < argc; ++a)
	{
		std::vector<float> in, out;
		if (!LoadWav16(argv[a], in)) { printf("[take] could not read %s\n", argv[a]); continue; }
		auto hum = std::make_unique<Audio::HumRemover>();
		std::vector<float> twice(in); twice.insert(twice.end(), in.begin(), in.end());   // pass 1 locks, pass 2 is measured
		Run(*hum, twice, out, 50.0f);
		const auto status = hum->GetStatus();
		std::string name = argv[a];
		const size_t slash = name.find_last_of("/\\");
		if (slash != std::string::npos) name = name.substr(slash + 1);
		if (name.size() > 4) name = name.substr(0, name.size() - 4);
		SaveWav16(name + "-dehummed.wav", out.data() + in.size(), in.size());   // pass 2: locked from the first sample
		printf("[take] %s\n       locked %.4f Hz, %d lines up to %.0f Hz, strongest %.1f dB\n", argv[a], status.fundamentalHz, status.lines, status.highestHz, status.strongestDb);
		if (status.fundamentalHz > 0)
		{
			const size_t from = in.size() + static_cast<size_t>(0.1 * SR), count = static_cast<size_t>(2.2 * SR);   // idle start of pass 2
			const double f0 = status.fundamentalHz;
			printf("       idle buzz line power change: below 2 kHz %.1f dB, 2-8 kHz %.1f dB\n",
				10 * std::log10(LinePower(out, from, count, f0, 40, 2000) / LinePower(twice, from, count, f0, 40, 2000)),
				10 * std::log10(LinePower(out, from, count, f0, 2000, 8000) / LinePower(twice, from, count, f0, 2000, 8000)));
		}
	}
	printf(ok ? "PASS: hum remover locks to the grid, removes the buzz, keeps the guitar.\n" : "FAIL\n");
	return ok ? 0 : 1;
}
