#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "audio.hpp"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>

#pragma comment(lib, "winmm.lib")

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSemi = 1.0594630943592953;
constexpr double kFine16 = 1.007246412223704;
constexpr double kFine = 1.0004513695322617;

const float kAtkMs[64] = {
	100000000.0f, 100000000.0f, 8100.0f, 6900.0f, 6000.0f, 4800.0f, 4000.0f, 3400.0f,
	3000.0f, 2400.0f, 2000.0f, 1700.0f, 1500.0f, 1200.0f, 1000.0f, 860.0f,
	760.0f, 600.0f, 500.0f, 430.0f, 380.0f, 300.0f, 250.0f, 220.0f,
	190.0f, 150.0f, 130.0f, 110.0f, 95.0f, 76.0f, 63.0f, 55.0f,
	47.0f, 38.0f, 31.0f, 27.0f, 24.0f, 19.0f, 15.0f, 13.0f,
	12.0f, 9.39999962f, 7.9000001f, 6.80000019f, 6.0f, 4.69999981f, 3.79999995f, 3.4000001f,
	3.0f, 2.4000001f, 2.0f, 1.79999995f, 1.60000002f, 1.29999995f, 1.10000002f, 0.930000007f,
	0.850000024f, 0.649999976f, 0.529999971f, 0.439999998f, 0.400000006f, 0.349999994f, 0.0f, 0.0f,
};

const float kDecMs[64] = {
	100000000.0f, 100000000.0f, 118200.0f, 101300.0f, 88600.0f, 70900.0f, 59100.0f, 50700.0f,
	44300.0f, 35500.0f, 29600.0f, 25300.0f, 22200.0f, 17700.0f, 14800.0f, 12700.0f,
	11100.0f, 8900.0f, 7400.0f, 6300.0f, 5500.0f, 4400.0f, 3700.0f, 2800.0f,
	2200.0f, 1800.0f, 1600.0f, 1400.0f, 1100.0f, 920.0f, 790.0f, 690.0f,
	550.0f, 460.0f, 390.0f, 340.0f, 270.0f, 230.0f, 200.0f, 170.0f,
	140.0f, 110.0f, 98.0f, 85.0f, 68.0f, 57.0f, 49.0f, 43.0f,
	34.0f, 28.0f, 25.0f, 22.0f, 18.0f, 14.0f, 12.0f, 11.0f,
	8.5f, 7.0999999f, 6.0999999f, 5.4000001f, 4.30000019f, 3.5999999f, 3.0999999f, 0.0f,
};

uint32_t be32(const uint8_t* p)
{
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint16_t be16(const uint8_t* p)
{
	return uint16_t((uint32_t(p[0]) << 8) | uint32_t(p[1]));
}

int16_t be16s(const uint8_t* p)
{
	return (int16_t)be16(p);
}

uint32_t le32(const uint8_t* p)
{
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint16_t le16(const uint8_t* p)
{
	return uint16_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8));
}

int clamp_i(int v, int lo, int hi)
{
	if (v < lo) return lo;

	if (v > hi) return hi;

	return v;
}

double key_ratio(int root, int fine, int key)
{
	double r = 90316.8;
	if (fine < 0) {
		root++;
		fine += 0x80;
	}

	root -= key;
	if (root >= 0) {
		while (root >= 12) {
			root -= 12;
			r *= 0.5;
		}

		while (root > 0) {
			root--;
			r /= kSemi;
		}
	} else {
		root = -root;
		while (root >= 12) {
			root -= 12;
			r *= 2.0;
		}

		while (root > 0) {
			root--;
			r *= kSemi;
		}
	}

	while (fine > 16) {
		fine -= 16;
		r *= kFine16;
	}

	while (fine > 0) {
		fine--;
		r *= kFine;
	}

	return r;
}

void pitch_oct(int root, int fine, int key, int& oct, int& frac)
{
	double ratio = key_ratio(root, fine, key);
	uint32_t src = (uint32_t)ratio;
	if (src < 0x200) src = 0x200;
	if (src > 0x40000) src = 0x3FFFF;
	int freq = (int)(0.5 + 32000.0 * (src / 65536.0));
	if (freq < 1) freq = 1;
	double n = std::log10(freq / 172.265625) / std::log10(2.0);
	int i = (int)n;
	oct = (i - 8) & 0xF;
	double base = 172.265625 / std::pow(2.0, -i);
	int code = (int)(1024.0 * (freq - base) / base);
	if (oct > 8) oct = -(16 - oct);
	frac = (code & 0x200) ? 1 : 0;
}

int table_index(int bias, int oct, int frac, int byte)
{
	int i = bias + oct + frac + byte * 2;
	return clamp_i(i, 0, 63);
}

float table_ms(const float* table, int bias, int oct, int frac, int byte)
{
	return table[table_index(bias, oct, frac, byte)];
}

int nearest_byte(const float* table, int bias, int oct, int frac, float ms)
{
	int best = 0;
	double best_d = 1e300;
	double target = std::log(std::max(ms, 0.05f) + 1.0f);
	for (int b = 0; b < 128; b++) {
		float t = table_ms(table, bias, oct, frac, b);
		double d = std::fabs(std::log(std::max(t, 0.05f) + 1.0f) - target);
		if (d < best_d) {
			best_d = d;
			best = b;
		}
	}

	return best;
}

float rate_of(float ms, bool attack)
{
	if (attack && ms < 5.0f) return 1.0f;

	if (ms <= 0.0f) return 1.0f;

	if (ms >= 1.0e7f) return 0.0f;

	return 5.0f / ms;
}

int16_t clamp16(int v)
{
	if (v > 32767) return 32767;

	if (v < -32768) return -32768;

	return (int16_t)v;
}

int adpcm_sample(int nibble, int16_t c1, int16_t c2, int scale, int16_t& y1, int16_t& y2)
{
	int sample = (nibble * scale) << 11;
	sample += (int)c1 * y1 + (int)c2 * y2;
	sample = (sample + 1024) >> 11;
	sample = clamp16(sample);
	y2 = y1;
	y1 = (int16_t)sample;
	return sample;
}

int nibble_at(const uint8_t* frame, int sample_in_frame)
{
	int byte = frame[1 + sample_in_frame / 2];
	int raw = (sample_in_frame & 1) ? (byte & 0xF) : (byte >> 4);
	return raw >= 8 ? raw - 16 : raw;
}

bool decode_adpcm(const uint8_t* data, size_t size, int cur, int end, const int16_t coef[16], int16_t y1, int16_t y2,
                  std::vector<int16_t>& pcm, int loop_nibble, int& loop_from)
{
	if (end < cur) return false;

	loop_from = 0;
	bool saw_loop = false;
	int pos = cur;
	while (pos < end) {
		int frame = pos / 16;
		int within = pos % 16;
		size_t off = (size_t)frame * 8;
		if (off + 8 > size) break;

		if (within < 2) {
			pos = frame * 16 + 2;
			continue;
		}

		const uint8_t* fr = data + off;
		int header = fr[0];
		int scale = 1 << (header & 0xF);
		int pred = (header >> 4) & 7;
		int index = within - 2;
		if (!saw_loop && loop_nibble >= 0 && pos >= loop_nibble) {
			loop_from = (int)pcm.size();
			saw_loop = true;
		}

		int s = adpcm_sample(nibble_at(fr, index), coef[pred * 2], coef[pred * 2 + 1], scale, y1, y2);
		pcm.push_back((int16_t)s);
		pos++;
	}

	if (!saw_loop) loop_from = 0;
	return !pcm.empty();
}

bool decode_pcm16(const uint8_t* data, size_t size, uint32_t cur, uint32_t end, std::vector<int16_t>& pcm)
{
	if (end <= cur) return false;

	uint64_t bytes = uint64_t(end - cur) * 2;
	uint64_t off = uint64_t(cur) * 2;
	if (off + bytes > size) return false;

	pcm.reserve((size_t)(end - cur));
	for (uint32_t i = cur; i < end; i++) pcm.push_back(be16s(data + i * 2));
	return !pcm.empty();
}

bool decode_pcm8(const uint8_t* data, size_t size, uint32_t cur, uint32_t end, std::vector<int16_t>& pcm)
{
	if (end <= cur || (uint64_t)end > size) return false;

	for (uint32_t i = cur; i < end; i++) {
		int s = ((int)data[i] - 128) << 8;
		pcm.push_back((int16_t)s);
	}

	return !pcm.empty();
}

struct Predictor {
	double a1 = 0;
	double a2 = 0;
};

Predictor fit_frame(const std::vector<int16_t>& pcm, int start)
{
	double aa = 0, ab = 0, bb = 0, ay = 0, by = 0;
	for (int n = 0; n < 14; n++) {
		int i = start + n;
		double y = (i >= 0 && i < (int)pcm.size()) ? pcm[i] : 0;
		double x1 = (i - 1 >= 0 && i - 1 < (int)pcm.size()) ? pcm[i - 1] : 0;
		double x2 = (i - 2 >= 0 && i - 2 < (int)pcm.size()) ? pcm[i - 2] : 0;
		aa += x1 * x1;
		ab += x1 * x2;
		bb += x2 * x2;
		ay += x1 * y;
		by += x2 * y;
	}

	double det = aa * bb - ab * ab;
	Predictor p;
	if (std::fabs(det) < 1.0) return p;

	p.a1 = (ay * bb - by * ab) / det;
	p.a2 = (aa * by - ab * ay) / det;
	p.a1 = std::clamp(p.a1, -15.0, 15.0);
	p.a2 = std::clamp(p.a2, -15.0, 15.0);
	return p;
}

int16_t qcoef(double a)
{
	int v = (int)std::lround(a * 2048.0);
	return (int16_t)clamp_i(v, -32768, 32767);
}

bool encode_pcm(const std::vector<int16_t>& pcm, uint32_t rate, int loop_sample, EncodedAdpcm& out, std::string& error)
{
	if (pcm.empty()) {
		error = "The WAV has no samples.";
		return false;
	}

	if (pcm.size() > 2000000) {
		error = "That WAV is longer than the editor will encode.";
		return false;
	}

	int n = (int)pcm.size();
	int frames = (n + 13) / 14;
	std::vector<Predictor> fits;
	fits.reserve(frames);
	for (int f = 0; f < frames; f++) fits.push_back(fit_frame(pcm, f * 14));

	int16_t coef[16] = {};
	coef[0] = 0;
	coef[1] = 0;
	coef[2] = 2048;
	coef[3] = 0;
	coef[4] = 4096;
	coef[5] = -2048;
	std::vector<int> used(frames, 0);
	for (int slot = 3; slot < 8; slot++) {
		int pick = 0;
		double best = -1;
		for (int f = 0; f < frames; f++) {
			if (used[f]) continue;

			double dmin = 1e300;
			for (int s = 0; s < slot; s++) {
				double d1 = fits[f].a1 - (coef[s * 2] / 2048.0);
				double d2 = fits[f].a2 - (coef[s * 2 + 1] / 2048.0);
				dmin = std::min(dmin, d1 * d1 + d2 * d2);
			}

			if (dmin > best) {
				best = dmin;
				pick = f;
			}
		}

		used[pick] = 1;
		coef[slot * 2] = qcoef(fits[pick].a1);
		coef[slot * 2 + 1] = qcoef(fits[pick].a2);
	}

	out = EncodedAdpcm{};
	out.rate = rate ? rate : 44100;
	out.length = (uint32_t)n;
	out.cur_adr = 2;
	out.data.assign((size_t)frames * 8, 0);
	std::memcpy(out.coef, coef, sizeof coef);

	int16_t y1 = 0, y2 = 0;
	int loop_frame = -1;
	int loop_index = -1;
	if (loop_sample >= 0) {
		loop_sample = clamp_i(loop_sample, 0, n - 1);
		loop_frame = loop_sample / 14;
		loop_index = loop_sample % 14;
		out.loop = 1;
	}

	for (int f = 0; f < frames; f++) {
		int16_t enter1 = y1, enter2 = y2;
		int best_pred = 0, best_scale = 0;
		int64_t best_err = INT64_MAX;
		int best_nib[14] = {};
		int16_t best_y1 = y1, best_y2 = y2;
		for (int pred = 0; pred < 8; pred++) {
			for (int scale_n = 0; scale_n <= 12; scale_n++) {
				int scale = 1 << scale_n;
				int16_t hy1 = enter1, hy2 = enter2;
				int nib[14] = {};
				int64_t err = 0;
				for (int s = 0; s < 14; s++) {
					int i = f * 14 + s;
					int want = (i < n) ? pcm[i] : 0;
					int predict = (((int)coef[pred * 2] * hy1 + (int)coef[pred * 2 + 1] * hy2) + 1024) >> 11;
					int raw = (int)std::lround((want - predict) / (double)scale);
					raw = clamp_i(raw, -8, 7);
					nib[s] = raw;
					int got = adpcm_sample(raw, coef[pred * 2], coef[pred * 2 + 1], scale, hy1, hy2);
					int diff = got - want;
					err += (int64_t)diff * diff;
				}

				if (err < best_err) {
					best_err = err;
					best_pred = pred;
					best_scale = scale_n;
					best_y1 = hy1;
					best_y2 = hy2;
					std::memcpy(best_nib, nib, sizeof nib);
				}
			}
		}

		if (f == loop_frame) {
			int16_t hy1 = enter1, hy2 = enter2;
			int scale = 1 << best_scale;
			for (int s = 0; s < loop_index; s++) {
				adpcm_sample(best_nib[s], coef[best_pred * 2], coef[best_pred * 2 + 1], scale, hy1, hy2);
			}

			out.loop_pred_scale = (uint16_t)((best_pred << 4) | best_scale);
			out.loop_yn1 = hy1;
			out.loop_yn2 = hy2;
			out.loop_adr = (uint32_t)(f * 16 + 2 + loop_index);
		}

		uint8_t* fr = out.data.data() + (size_t)f * 8;
		fr[0] = (uint8_t)((best_pred << 4) | best_scale);
		for (int s = 0; s < 14; s++) {
			int raw = best_nib[s] & 0xF;
			if (s & 1) fr[1 + s / 2] |= (uint8_t)raw;
			else fr[1 + s / 2] = (uint8_t)(raw << 4);
		}

		y1 = best_y1;
		y2 = best_y2;
		if (f == 0) {
			out.pred_scale = fr[0];
			out.yn1 = 0;
			out.yn2 = 0;
		}
	}

	int last = n - (frames - 1) * 14;
	if (last < 1) last = 1;
	if (last > 14) last = 14;
	out.end_adr = (uint32_t)((frames - 1) * 16 + 2 + last);
	out.nibbles = out.end_adr;
	if (!out.loop) {
		out.loop_adr = out.cur_adr;
		out.loop_pred_scale = out.pred_scale;
		out.loop_yn1 = out.yn1;
		out.loop_yn2 = out.yn2;
	}

	error.clear();
	return true;
}

bool resample_mono(const std::vector<float>& src, uint32_t rate, std::vector<int16_t>& dst, std::string& error)
{
	if (src.empty() || rate == 0) {
		error = "The WAV has no samples.";
		return false;
	}

	double step = (double)rate / 44100.0;
	size_t out_n = (size_t)std::max(1.0, std::floor(src.size() / step));
	if (out_n > 2000000) {
		error = "That WAV is longer than the editor will encode.";
		return false;
	}

	dst.resize(out_n);
	for (size_t i = 0; i < out_n; i++) {
		double pos = i * step;
		size_t i0 = (size_t)pos;
		double frac = pos - i0;
		float a = src[i0];
		float b = src[std::min(i0 + 1, src.size() - 1)];
		int s = (int)std::lround(a + (b - a) * frac);
		dst[i] = clamp16(s);
	}

	return true;
}

struct Voice {
	Note note;
	double cursor = 0;
	bool active = false;
	bool released = false;
	int phase = 0;
	float env = 0;
	float env_db = -96;
	int until = 0;
	int key = -1;
};

struct Player {
	HWAVEOUT wave = nullptr;
	WAVEHDR hdr[4] = {};
	std::vector<int16_t> buf[4];

	std::thread thread;
	std::atomic<bool> run{false};
	std::mutex mu;

	Voice voice;
	bool open = false;
	std::string fail;
};

Player& player()
{
	static Player p;
	return p;
}

void step_env(Voice& v)
{
	Note& n = v.note;
	if (!n.shaped) {
		if (v.released) {
			v.env -= 5.0f / 80.0f;
			if (v.env < 0) v.env = 0;
		} else v.env = 1;

		v.env_db = -96.0f + 96.0f * v.env;
		return;
	}

	if (v.released && v.phase < 4) v.phase = 5;
	/* sus_db is the dB the sustain byte encodes. The DOL stores gcaxLookup of that
	 * dB and compares it with env_db, so decay never runs in the game. The preview
	 * compares the dB itself, which is what the byte was written to mean. */
	switch (v.phase) {
	case 0:
		v.env += n.atk_rate;
		if (v.env >= 1.0f) {
			v.env = 1.0f;
			v.phase = 1;
		}

		break;
	case 1:
		v.env_db = -96.0f + 96.0f * v.env;
		if (v.env_db <= n.sus_db) {
			v.phase = 3;
			break;
		}

		v.env -= n.dec_rate;
		if (v.env < 0) v.env = 0;
		v.env_db = -96.0f + 96.0f * v.env;
		if (v.env_db <= n.sus_db) v.phase = 3;
		break;
	case 3:
		v.env -= n.sus_rate;
		if (v.env < 0) v.env = 0;
		break;
	default:
		v.env -= n.rel_rate;
		if (v.env < 0) v.env = 0;
		break;
	}

	v.env_db = -96.0f + 96.0f * v.env;
}

void fill_buffer(int16_t* dst, int count)
{
	Player& p = player();
	std::lock_guard<std::mutex> lock(p.mu);
	Voice& v = p.voice;
	for (int i = 0; i < count; i++) {
		if (!v.active || v.note.pcm.empty()) {
			dst[i] = 0;
			continue;
		}

		if (v.until <= 0) {
			step_env(v);
			v.until = 220;
			if (v.env <= 0.0f && (v.released || v.phase >= 3)) {
				bool dying = v.released || !v.note.loop;
				if (dying && v.env_db <= -90.0f) {
					v.active = false;
					v.key = -1;
					dst[i] = 0;
					continue;
				}
			}
		}

		v.until--;
		const std::vector<int16_t>& pcm = v.note.pcm;
		if (v.cursor >= (double)pcm.size()) {
			if (v.note.loop && v.note.loop_to > v.note.loop_from) {
				double span = (double)(v.note.loop_to - v.note.loop_from);
				if (span < 1) span = 1;
				while (v.cursor >= v.note.loop_to) v.cursor -= span;
				if (v.cursor < v.note.loop_from) v.cursor = v.note.loop_from;
			} else {
				dst[i] = 0;
				if (v.released || v.env <= 0.0f) {
					v.active = false;
					v.key = -1;
				}

				continue;
			}
		}

		int i0 = (int)v.cursor;
		if (i0 < 0) i0 = 0;
		if (i0 >= (int)pcm.size()) i0 = (int)pcm.size() - 1;
		int i1 = std::min(i0 + 1, (int)pcm.size() - 1);
		double frac = v.cursor - i0;
		double s = pcm[i0] + (pcm[i1] - pcm[i0]) * frac;
		float amp = std::pow(10.0f, v.env_db / 20.0f) * v.note.level;
		int out = (int)std::lround(s * amp);
		dst[i] = clamp16(out);
		v.cursor += v.note.step;
	}
}

void audio_thread()
{
	Player& p = player();
	while (p.run.load()) {
		bool wrote = false;
		for (int i = 0; i < 4; i++) {
			WAVEHDR& h = p.hdr[i];
			if ((h.dwFlags & WHDR_DONE) || !(h.dwFlags & WHDR_PREPARED)) {
				if (h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(p.wave, &h, sizeof h);
				fill_buffer(p.buf[i].data(), (int)p.buf[i].size());
				h.lpData = (LPSTR)p.buf[i].data();
				h.dwBufferLength = (DWORD)(p.buf[i].size() * sizeof(int16_t));
				h.dwFlags = 0;
				waveOutPrepareHeader(p.wave, &h, sizeof h);
				waveOutWrite(p.wave, &h, sizeof h);
				wrote = true;
			}
		}

		if (!wrote) Sleep(2);
	}
}

} // namespace

EnvView env_view(int rate_bias, int root, int fine, int key, int atk, int dec, int sus, int sus_level, int rel)
{
	int oct = 0, frac = 0;
	pitch_oct(root, fine, key, oct, frac);
	EnvView v;
	v.atk_ms = table_ms(kAtkMs, rate_bias, oct, frac, clamp_i(atk, 0, 127));
	v.dec_ms = table_ms(kDecMs, rate_bias, oct, frac, clamp_i(dec, 0, 127));
	v.sus_ms = table_ms(kDecMs, rate_bias, oct, frac, clamp_i(sus, 0, 127));
	v.rel_ms = table_ms(kDecMs, rate_bias, oct, frac, clamp_i(rel, 0, 127));
	if (sus_level <= 0 || sus_level >= 31) v.sus_env = 1.0f;
	else {
		float db = 1.5f * (float)(sus_level - 31);
		v.sus_env = std::clamp((db + 96.0f) / 96.0f, 0.0f, 1.0f);
	}

	return v;
}

int env_time_to_byte(bool attack, int rate_bias, int root, int fine, int key, float ms)
{
	int oct = 0, frac = 0;
	pitch_oct(root, fine, key, oct, frac);
	return nearest_byte(attack ? kAtkMs : kDecMs, rate_bias, oct, frac, ms);
}

int env_level_to_byte(float sus_env)
{
	sus_env = std::clamp(sus_env, 0.0f, 1.0f);
	if (sus_env > 0.985f) return 0;

	float db = sus_env * 96.0f - 96.0f;
	int n = (int)std::lround(31.0f + db / 1.5f);
	return clamp_i(n, 1, 31);
}

void env_rates(const EnvView& view, float& atk_rate, float& dec_rate, float& sus_rate, float& rel_rate, float& sus_db)
{
	atk_rate = rate_of(view.atk_ms, true);
	dec_rate = rate_of(view.dec_ms, false);
	sus_rate = rate_of(view.sus_ms, false);
	rel_rate = rate_of(view.rel_ms, false);
	sus_db = view.sus_env * 96.0f - 96.0f;
}

bool decode_gcax_sample(const uint8_t* sample, const uint8_t* wave, size_t wave_n, DecodedSample& out, std::string& error)
{
	out = DecodedSample{};
	if (!sample || !wave) {
		error = "This sample has no wave data.";
		return false;
	}

	uint32_t offset = be32(sample + 0x4C);
	uint8_t format = sample[0x4A];
	uint16_t loop = be16(sample + 0x0C);
	uint32_t loop_adr = be32(sample + 0x10);
	uint32_t end_adr = be32(sample + 0x14);
	uint32_t cur_adr = be32(sample + 0x18);
	if ((uint64_t)offset > wave_n) {
		error = "The sample offset is past the wave.";
		return false;
	}

	const uint8_t* data = wave + offset;
	size_t size = wave_n - offset;
	out.loop = loop != 0;
	if (format == 0) {
		int16_t coef[16];
		for (int i = 0; i < 16; i++) coef[i] = be16s(sample + 0x1C + i * 2);
		int loop_from = 0;
		int loop_nibble = out.loop ? (int)loop_adr : -1;
		if (!decode_adpcm(data, size, (int)cur_adr, (int)end_adr, coef, be16s(sample + 0x40), be16s(sample + 0x42), out.pcm,
		                  loop_nibble, loop_from)) {
			error = "The ADPCM sample did not decode.";
			return false;
		}

		out.loop_from = loop_from;
		out.loop_to = (int)out.pcm.size();
	} else if (format == 10) {
		if (!decode_pcm16(data, size, cur_adr, end_adr, out.pcm)) {
			error = "The PCM16 sample did not decode.";
			return false;
		}

		out.loop_from = out.loop ? (int)std::min(loop_adr, end_adr) : 0;
		out.loop_to = (int)out.pcm.size();
	} else if (format == 25) {
		if (!decode_pcm8(data, size, cur_adr, end_adr, out.pcm)) {
			error = "The PCM8 sample did not decode.";
			return false;
		}

		out.loop_from = out.loop ? (int)std::min(loop_adr, end_adr) : 0;
		out.loop_to = (int)out.pcm.size();
	} else {
		error = "This sample format is not one the voice player uses.";
		return false;
	}

	if (out.loop_from < 0) out.loop_from = 0;
	if (out.loop_from >= out.loop_to) out.loop_from = 0;
	error.clear();
	return true;
}

bool encode_wav(const uint8_t* wav, size_t size, int loop_sample, EncodedAdpcm& out, std::string& error)
{
	if (!wav || size < 12 || std::memcmp(wav, "RIFF", 4) != 0 || std::memcmp(wav + 8, "WAVE", 4) != 0) {
		error = "That file is not a WAV.";
		return false;
	}

	uint16_t format = 0, channels = 0, bits = 0;
	uint32_t rate = 0;
	const uint8_t* data = nullptr;
	uint32_t data_n = 0;
	size_t pos = 12;
	while (pos + 8 <= size) {
		uint32_t id = le32(wav + pos);
		uint32_t n = le32(wav + pos + 4);
		pos += 8;
		if ((uint64_t)pos + n > size) break;

		if (id == 0x20746D66u && n >= 16) {
			format = le16(wav + pos);
			channels = le16(wav + pos + 2);
			rate = le32(wav + pos + 4);
			bits = le16(wav + pos + 14);
		} else if (id == 0x61746164u) {
			data = wav + pos;
			data_n = n;
		}

		pos += n + (n & 1);
	}

	if (!data || channels == 0 || rate == 0 || (format != 1 && format != 3)) {
		error = "The WAV needs to be PCM or float.";
		return false;
	}

	int bytes_per = format == 3 ? 4 : bits / 8;
	if (bytes_per <= 0) {
		error = "The WAV sample size is not one this editor reads.";
		return false;
	}

	size_t frames = data_n / (bytes_per * channels);
	std::vector<float> mono(frames);
	for (size_t i = 0; i < frames; i++) {
		double sum = 0;
		for (uint16_t c = 0; c < channels; c++) {
			const uint8_t* s = data + (i * channels + c) * bytes_per;
			if (format == 3 && bytes_per >= 4) {
				float f;
				std::memcpy(&f, s, 4);
				sum += f * 32767.0;
			} else if (bits == 8) sum += ((int)s[0] - 128) * 256.0;
			else if (bits == 16) {
				int16_t v = (int16_t)le16(s);
				sum += v;
			} else if (bits == 24) {
				int v = (int)s[0] | ((int)s[1] << 8) | ((int)s[2] << 16);
				if (v & 0x800000) v |= ~0xFFFFFF;
				sum += v / 256.0;
			} else {
				error = "The WAV sample size is not one this editor reads.";
				return false;
			}
		}

		mono[i] = (float)(sum / channels);
	}

	std::vector<int16_t> pcm;
	if (!resample_mono(mono, rate, pcm, error)) return false;

	if (loop_sample >= 0 && rate != 44100) loop_sample = (int)std::lround(loop_sample * (44100.0 / (double)rate));
	return encode_pcm(pcm, 44100, loop_sample, out, error);
}

void audio_init()
{
	Player& p = player();
	if (p.open) return;

	WAVEFORMATEX fmt = {};
	fmt.wFormatTag = WAVE_FORMAT_PCM;
	fmt.nChannels = 1;
	fmt.nSamplesPerSec = 44100;
	fmt.wBitsPerSample = 16;
	fmt.nBlockAlign = 2;
	fmt.nAvgBytesPerSec = 44100 * 2;
	if (waveOutOpen(&p.wave, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
		p.fail = "Audio output did not start.";
		return;
	}

	for (int i = 0; i < 4; i++) {
		p.buf[i].assign(512, 0);
		p.hdr[i] = {};
	}

	p.open = true;
	p.run = true;
	p.thread = std::thread(audio_thread);
}

void audio_shutdown()
{
	Player& p = player();
	if (p.run.exchange(false) && p.thread.joinable()) p.thread.join();
	if (p.wave) {
		waveOutReset(p.wave);
		for (int i = 0; i < 4; i++) {
			if (p.hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(p.wave, &p.hdr[i], sizeof p.hdr[i]);
		}

		waveOutClose(p.wave);
		p.wave = nullptr;
	}

	p.open = false;
}

bool audio_play(Note note, std::string& error)
{
	Player& p = player();
	if (!p.open) {
		error = p.fail.empty() ? "Audio output did not start." : p.fail;
		return false;
	}

	if (note.pcm.empty()) {
		error = "This sample has no wave data.";
		return false;
	}

	if (note.step < 0.001) note.step = 0.001;
	std::lock_guard<std::mutex> lock(p.mu);
	Voice& v = p.voice;
	v.note = std::move(note);
	v.cursor = 0;
	v.active = true;
	v.released = false;
	v.until = 0;
	v.key = v.note.key;
	if (!v.note.shaped || v.note.atk_rate >= 1.0f) {
		v.env = 1;
		v.env_db = 0;
		v.phase = v.note.shaped ? 1 : 3;
	} else {
		v.env = 0;
		v.env_db = -96;
		v.phase = 0;
	}

	error.clear();
	return true;
}

void audio_release()
{
	Player& p = player();
	std::lock_guard<std::mutex> lock(p.mu);
	if (p.voice.active) p.voice.released = true;
}

void audio_stop()
{
	Player& p = player();
	std::lock_guard<std::mutex> lock(p.mu);
	p.voice.active = false;
	p.voice.key = -1;
	p.voice.note.pcm.clear();
}

int audio_key()
{
	Player& p = player();
	std::lock_guard<std::mutex> lock(p.mu);
	return p.voice.active ? p.voice.key : -1;
}

void audio_selftest()
{
	std::vector<int16_t> src(2048);
	for (int i = 0; i < (int)src.size(); i++) src[i] = (int16_t)(12000.0 * std::sin(2.0 * kPi * 440.0 * i / 44100.0));
	EncodedAdpcm enc;
	std::string error;
	if (!encode_pcm(src, 44100, 14 * 4, enc, error)) throw std::runtime_error(error);

	if (enc.data.size() < 8 || enc.coef[2] == 0) throw std::runtime_error("selftest: encoder header");

	std::vector<uint8_t> hdr(0x50, 0);
	auto put32 = [&](int o, uint32_t v) {
		hdr[o] = (uint8_t)(v >> 24);
		hdr[o + 1] = (uint8_t)(v >> 16);
		hdr[o + 2] = (uint8_t)(v >> 8);
		hdr[o + 3] = (uint8_t)v;
	};

	auto put16 = [&](int o, uint16_t v) {
		hdr[o] = (uint8_t)(v >> 8);
		hdr[o + 1] = (uint8_t)v;
	};

	put32(0, enc.length);
	put32(4, enc.nibbles);
	put32(8, enc.rate);
	put16(0x0C, enc.loop);
	put32(0x10, enc.loop_adr);
	put32(0x14, enc.end_adr);
	put32(0x18, enc.cur_adr);
	for (int i = 0; i < 16; i++) put16(0x1C + i * 2, (uint16_t)enc.coef[i]);
	put16(0x3E, enc.pred_scale);
	put16(0x40, (uint16_t)enc.yn1);
	put16(0x42, (uint16_t)enc.yn2);
	put16(0x44, enc.loop_pred_scale);
	put16(0x46, (uint16_t)enc.loop_yn1);
	put16(0x48, (uint16_t)enc.loop_yn2);
	hdr[0x4A] = 0;
	DecodedSample dec;
	if (!decode_gcax_sample(hdr.data(), enc.data.data(), enc.data.size(), dec, error)) throw std::runtime_error(error);

	int n = std::min((int)src.size(), (int)dec.pcm.size());
	if (n < 1000) throw std::runtime_error("selftest: short decode");

	double err = 0;
	for (int i = 0; i < n; i++) err += std::fabs((double)src[i] - (double)dec.pcm[i]);
	err /= n;
	if (err > 1800.0) throw std::runtime_error("selftest: adpcm round trip");

	if (!dec.loop || dec.loop_from < 14) throw std::runtime_error("selftest: loop point");
}
