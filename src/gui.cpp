/* EMALT window. Win32 + DirectX 11 + Dear ImGui. The bank bytes are still
 * tools/emalt/src/emalt.cpp; this file is only the editor in front of them. */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <d3d11.h>
#include <tchar.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "emalt.hpp"
#include "audio.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "imm32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static bool g_occluded = false;
static UINT g_resize_w = 0, g_resize_h = 0;
static ID3D11RenderTargetView* g_target = nullptr;

static bool create_device(HWND hwnd);
static void cleanup_device();
static void create_target();
static void cleanup_target();
static LRESULT WINAPI wnd_proc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

enum class SelKind { Root, Entry, Sample, Curve, Prog, Layer, Region };

struct Sel {
	SelKind kind = SelKind::Root;
	int entry = -1;
	int sample = 0;
	int curve = 0;
	int prog = 0;
	int layer = 0;
	int region = 0;
};

struct App {
	HWND hwnd = nullptr;
	Document doc;

	bool loaded = false;
	bool dirty = false;
	bool status_error = false;

	std::string path;
	std::string status;
	std::string pending_open;

	Sel sel;
	char filter[64] = {};
	ImFont* mono = nullptr;

	int pending_edit = 0;
	int pending_entry = -1;
	int pending_prog = -1;
	int pending_layer = -1;
	int pending_region = -1;
};

enum PendingEdit { EditNone = 0, EditAddRegion, EditDeleteRegion, EditAddLayer, EditRemoveLayer, EditAddProgram, EditDumpWavs };

static void queue_edit(App& app, int op, int entry, int prog, int layer, int region);

static App* g_app = nullptr;

static bool same_sel(const Sel& a, const Sel& b)
{
	return a.kind == b.kind && a.entry == b.entry && a.sample == b.sample && a.curve == b.curve && a.prog == b.prog &&
	       a.layer == b.layer && a.region == b.region;
}

static Sel sel_root()
{
	Sel s;
	s.kind = SelKind::Root;
	return s;
}

static Sel sel_entry(int entry)
{
	Sel s;
	s.kind = SelKind::Entry;
	s.entry = entry;
	return s;
}

static const char* kind_name(FileKind kind)
{
	switch (kind) {
	case FileKind::Mlt: return "MLT";
	case FileKind::Mpb: return "MPB";
	case FileKind::Mpbp: return "MPBP";
	case FileKind::Mpbw: return "MPBW";
	case FileKind::Msb: return "MSB";
	default: return "bank";
	}
}

static std::string file_leaf(const std::string& path)
{
	size_t slash = path.find_last_of("\\/");
	if (slash == std::string::npos) return path;

	return path.substr(slash + 1);
}

static void set_title(App& app)
{
	std::string title = "EMALT";
	if (app.loaded) {
		title += " - ";
		title += file_leaf(app.path);
		if (app.dirty) {
			title += " *";
		}
	}

	SetWindowTextA(app.hwnd, title.c_str());
}

static void set_status(App& app, const std::string& text, bool error)
{
	app.status = text;
	app.status_error = error;
}

static std::string base_path(const App& app)
{
	if (app.doc.kind == FileKind::Mlt && app.sel.entry >= 0) return "entry[" + std::to_string(app.sel.entry) + "]";

	return {};
}

static std::string join_path(const std::string& base, const std::string& rest)
{
	if (base.empty()) return rest;

	if (rest.empty()) return base;

	return base + "." + rest;
}

static const Mpb* mpb_of(const Document& doc, int entry)
{
	if (doc.kind == FileKind::Mlt) {
		if (entry < 0 || entry >= (int)doc.entries.size() || !doc.entries[entry].mpb) return nullptr;

		return &*doc.entries[entry].mpb;
	}

	if (entry < 0 && doc.mpb) return &*doc.mpb;

	return nullptr;
}

static const Params* params_of(const Document& doc, int entry)
{
	if (doc.kind == FileKind::Mpbp && entry < 0 && doc.params) return &*doc.params;

	const Mpb* mpb = mpb_of(doc, entry);
	if (!mpb || !mpb->has_param) return nullptr;

	return &mpb->params;
}

static int bank_entry(const App& app)
{
	if (!app.loaded) return -2;

	if (app.doc.kind == FileKind::Mlt) {
		if (app.sel.entry < 0 || app.sel.entry >= (int)app.doc.entries.size()) return -2;

		if (!app.doc.entries[app.sel.entry].mpb) return -2;

		return app.sel.entry;
	}

	if (app.doc.mpb || app.doc.kind == FileKind::Mpbp) return -1;

	return -2;
}

static void note_text(int n, char* out, size_t nout)
{
	static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
	if (n < 0) n = 0;
	if (n > 127) n = 127;
	std::snprintf(out, nout, "%s%d", names[n % 12], n / 12 - 1);
}

static bool match_filter(const App& app, const std::string& label)
{
	if (app.filter[0] == 0) return true;

	std::string a = label;
	std::string b = app.filter;
	for (char& c : a) c = (char)std::tolower((unsigned char)c);
	for (char& c : b) c = (char)std::tolower((unsigned char)c);
	return a.find(b) != std::string::npos;
}

static bool read_int(const Document& doc, const std::string& path, int& out)
{
	out = std::stoi(get_field(doc, path), nullptr, 0);
	return true;
}

static void try_set(App& app, const std::string& path, const std::string& value, bool reparse)
{
	try {
		set_field(app.doc, path, value, reparse);
		app.dirty = true;
		app.status_error = false;
		app.status.clear();
	} catch (const std::exception& ex) {
		set_status(app, ex.what(), true);
	}
}

static void slider_int(App& app, const std::string& path, const char* caption, int lo, int hi)
{
	int value = 0;
	try {
		read_int(app.doc, path, value);
	} catch (const std::exception& ex) {
		ImGui::TextDisabled("%s", ex.what());
		return;
	}

	int shown = value;
	if (shown < lo) lo = shown;
	if (shown > hi) hi = shown;
	const char* mark = std::strstr(caption, "###");
	const char* slider_id = mark ? mark : caption;
	char hidden[96];
	if (!mark) {
		std::snprintf(hidden, sizeof hidden, "##%s", caption);
		slider_id = hidden;
	}

	float column = ImGui::CalcTextSize("Velocity curve").x + ImGui::GetStyle().ItemInnerSpacing.x;
	ImVec2 label_size = ImGui::CalcTextSize(caption, mark);
	if (label_size.x + ImGui::GetStyle().ItemInnerSpacing.x > column) {
		column = label_size.x + ImGui::GetStyle().ItemInnerSpacing.x;
	}

	float avail = ImGui::GetContentRegionAvail().x;
	if (column + 80.0f > avail) {
		ImGui::TextUnformatted(caption, mark);
		ImGui::SetNextItemWidth(-1.0f);
	} else {
		float row = ImGui::GetCursorPosX();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(caption, mark);
		ImGui::SameLine();
		float at = ImGui::GetCursorPosX();
		if (at < row + column) ImGui::SetCursorPosX(row + column);
		ImGui::SetNextItemWidth(-1.0f);
	}

	if (ImGui::SliderInt(slider_id, &shown, lo, hi)) {
		try_set(app, path, std::to_string(shown), false);
	}
}

static void int_field(App& app, const std::string& path, const char* label, int lo, int hi)
{
	int value = 0;
	try {
		read_int(app.doc, path, value);
	} catch (const std::exception& ex) {
		ImGui::TextDisabled("%s", ex.what());
		return;
	}

	ImGui::SetNextItemWidth(160.0f);
	if (ImGui::InputInt(label, &value)) {
		if (value < lo) value = lo;
		if (value > hi) value = hi;
		try_set(app, path, std::to_string(value), false);
	}
}

static void hex_field(App& app, const std::string& path, const char* label, bool reparse)
{
	unsigned value = 0;
	try {
		value = (unsigned)std::stoul(get_field(app.doc, path), nullptr, 0);
	} catch (const std::exception& ex) {
		ImGui::TextDisabled("%s", ex.what());
		return;
	}

	ImGui::SetNextItemWidth(160.0f);
	if (ImGui::InputScalar(label, ImGuiDataType_U32, &value, nullptr, nullptr, "%08X", ImGuiInputTextFlags_CharsHexadecimal)) {
		char buf[16];
		std::snprintf(buf, sizeof buf, "0x%08X", value);
		try_set(app, path, buf, reparse);
	}
}

static void section(const char* text)
{
	ImGui::Spacing();
	ImGui::TextColored(ImVec4(0.55f, 0.86f, 0.74f, 1.0f), "%s", text);
	ImGui::Separator();
}

static bool is_black_key(int key)
{
	int n = key % 12;
	return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

static int whites_before(int key)
{
	int n = 0;
	for (int i = 0; i < key; i++) {
		if (!is_black_key(i)) n++;
	}

	return n;
}

/* 0 ms sits on the left end and a held stage sits on the right end, with the
 * measured times strictly between them so a click can land back on either. */
static float env_span(float ms)
{
	const float instant = 16.0f;
	const float held = 210.0f;
	if (ms <= 0.0f) return instant;

	if (ms >= 1.0e7f) return held;

	float t = std::log(std::max(ms, 0.25f) / 0.25f) / std::log(20000.0f / 0.25f);
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	return instant + 14.0f + t * (held - instant - 28.0f);
}

static const char* ms_text(float ms, char* buf, size_t n)
{
	if (ms >= 1.0e7f) std::snprintf(buf, n, "hold");
	else if (ms >= 1000.0f) std::snprintf(buf, n, "%.1f s", ms / 1000.0f);
	else if (ms < 5.0f) std::snprintf(buf, n, "instant");
	else std::snprintf(buf, n, "%.0f ms", ms);

	return buf;
}

static void envelope_editor(App& app, const std::string& path, int root, int fine)
{
	int atk = 0, dec = 0, sus = 0, rel = 0, sus_level = 0, bias = 0;
	try {
		read_int(app.doc, path + ".atk", atk);
		read_int(app.doc, path + ".dec", dec);
		read_int(app.doc, path + ".sus", sus);
		read_int(app.doc, path + ".rel", rel);
		read_int(app.doc, path + ".sus_level", sus_level);
		read_int(app.doc, path + ".rate_bias", bias);
	} catch (const std::exception& ex) {
		ImGui::TextDisabled("%s", ex.what());
		return;
	}

	float width = ImGui::GetContentRegionAvail().x;
	float height = 168.0f;
	ImVec2 origin = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("envelope", ImVec2(std::max(width, 32.0f), height));
	bool hot = ImGui::IsItemActive();
	const float pad = 16.0f;
	float inner = std::max(width - pad * 2.0f, 32.0f);
	auto y_of = [&](float v) { return origin.y + height - 18.0f - (height - 36.0f) * v; };
	auto place = [&](int a, int d, int s, int lv, int r, float* xs, float* ys) {
		EnvView v = env_view(bias, root, fine, root, a, d, s, lv, r);
		float w[4] = {env_span(v.atk_ms), env_span(v.dec_ms), env_span(v.sus_ms), env_span(v.rel_ms)};
		/* Scale is fixed from the longest possible envelope, so dragging one
		 * stage does not rescale the others and the release end can move. */
		float scale = inner / (env_span(1.0e8f) * 4.0f);
		if (scale > 1.0f) scale = 1.0f;
		xs[0] = origin.x + pad;
		for (int i = 0; i < 4; i++) xs[i + 1] = xs[i] + w[i] * scale;
		ys[0] = v.atk_ms < 5.0f ? 1.0f : 0.0f;
		ys[1] = 1.0f;
		ys[2] = v.sus_env;
		ys[3] = v.sus_ms >= 1.0e7f ? v.sus_env : 0.0f;
		ys[4] = 0.0f;
	};

	float xs[5], ys[5];
	place(atk, dec, sus, sus_level, rel, xs, ys);
	ImVec2 pt[5];
	for (int i = 0; i < 5; i++) pt[i] = ImVec2(xs[i], y_of(ys[i]));
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(20, 22, 26, 255), 4.0f);
	float floor_y = y_of(0.0f);
	draw->AddLine(ImVec2(origin.x + pad, floor_y), ImVec2(origin.x + width - pad, floor_y), IM_COL32(50, 54, 60, 255), 1.0f);
	for (int i = 0; i < 4; i++) {
		ImVec2 quad[4] = {ImVec2(pt[i].x, floor_y), pt[i], pt[i + 1], ImVec2(pt[i + 1].x, floor_y)};
		ImU32 fill = i == 3 ? IM_COL32(232, 186, 84, 40) : IM_COL32(78, 186, 156, 48);
		ImU32 line = i == 3 ? IM_COL32(232, 186, 84, 255) : IM_COL32(140, 220, 190, 255);
		draw->AddConvexPolyFilled(quad, 4, fill);
		draw->AddLine(pt[i], pt[i + 1], line, 2.0f);
	}

	ImVec2 handles[4] = {pt[1], pt[2], pt[3], pt[4]};
	static int drag = 0;
	static ImVec2 grab;
	ImVec2 mouse = ImGui::GetIO().MousePos;
	auto nearest_handle = [&]() {
		int which = 0;
		float best = 12.0f;
		for (int i = 0; i < 4; i++) {
			float dx = mouse.x - handles[i].x;
			float dy = mouse.y - handles[i].y;
			float dist = std::sqrt(dx * dx + dy * dy);
			if (dist < best) {
				best = dist;
				which = i + 1;
			}
		}

		return which;
	};

	if (ImGui::IsItemActivated()) {
		drag = nearest_handle();
		grab = mouse;
	}

	if (!hot) drag = 0;
	auto commit = [&](const char* field, int value) {
		try {
			int cur = 0;
			read_int(app.doc, path + field, cur);
			if (cur != value) try_set(app, path + field, std::to_string(value), false);
		} catch (const std::exception&) {
		}
	};

	auto pick_byte = [&](int slot, int current) {
		auto dist_of = [&](int b) {
			int a = atk, d = dec, s = sus, r = rel;
			if (slot == 1) a = b;
			else if (slot == 2) d = b;
			else if (slot == 3) s = b;
			else r = b;

			float tx[5], ty[5];
			place(a, d, s, sus_level, r, tx, ty);
			return std::fabs(tx[slot] - mouse.x);
		};

		float best = dist_of(current);
		int chosen = current;
		for (int b = 0; b < 128; b++) {
			if (b == current) continue;

			float dist = dist_of(b);
			if (dist + 0.5f < best) {
				best = dist;
				chosen = b;
			}
		}

		return chosen;
	};

	auto pick_level = [&]() {
		auto dist_of = [&](int lv) {
			float tx[5], ty[5];
			place(atk, dec, sus, lv, rel, tx, ty);
			return std::fabs(y_of(ty[2]) - mouse.y);
		};

		float best = dist_of(sus_level);
		int chosen = sus_level;
		for (int lv = 0; lv <= 31; lv++) {
			if (lv == sus_level) continue;

			float dist = dist_of(lv);
			if (dist + 0.5f < best) {
				best = dist;
				chosen = lv;
			}
		}

		return chosen;
	};

	float moved = std::sqrt((mouse.x - grab.x) * (mouse.x - grab.x) + (mouse.y - grab.y) * (mouse.y - grab.y));
	if (hot && drag && moved > 2.0f) {
		if (drag == 1) commit(".atk", pick_byte(1, atk));
		else if (drag == 2) {
			commit(".dec", pick_byte(2, dec));
			commit(".sus_level", pick_level());
		} else if (drag == 3) commit(".sus", pick_byte(3, sus));
		else if (drag == 4) commit(".rel", pick_byte(4, rel));

		ImGui::SetMouseCursor(drag == 2 ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_ResizeEW);
	}

	for (int i = 0; i < 4; i++) {
		bool on = drag == i + 1;
		draw->AddCircleFilled(handles[i], on ? 7.0f : 5.5f, on ? IM_COL32(230, 246, 238, 255) : IM_COL32(120, 210, 180, 255));
	}

	if (ImGui::IsItemHovered()) {
		int which = nearest_handle();
		if (which) {
			EnvView view = env_view(bias, root, fine, root, atk, dec, sus, sus_level, rel);
			char buf[32];
			const char* tip[] = {"Attack", "Decay", "Sustain", "Release"};
			float times[] = {view.atk_ms, view.dec_ms, view.sus_ms, view.rel_ms};
			ImGui::SetTooltip("%s  %s", tip[which - 1], ms_text(times[which - 1], buf, sizeof buf));
		}
	}

	EnvView view = env_view(bias, root, fine, root, atk, dec, sus, sus_level, rel);
	char a[32], dbuf[32], s[32], rbuf[32];
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("Attack %s    decay %s    sustain %s    release %s", ms_text(view.atk_ms, a, sizeof a), ms_text(view.dec_ms, dbuf, sizeof dbuf), ms_text(view.sus_ms, s, sizeof s), ms_text(view.rel_ms, rbuf, sizeof rbuf));
	ImGui::PopStyleColor();
}

static bool sample_bytes(const App& app, int sample, const uint8_t*& rec, const uint8_t*& wave, size_t& wave_n)
{
	int entry = app.doc.kind == FileKind::Mlt ? app.sel.entry : -1;
	const Mpb* mpb = mpb_of(app.doc, entry);
	const Params* params = params_of(app.doc, entry);
	if (!mpb || !mpb->has_wave || !params || sample < 0 || sample >= (int)params->samples.size()) return false;

	if (mpb->wave + 16 > app.doc.bytes.size()) return false;

	wave_n = (app.doc.bytes[mpb->wave + 12] << 24) | (app.doc.bytes[mpb->wave + 13] << 16) | (app.doc.bytes[mpb->wave + 14] << 8) | app.doc.bytes[mpb->wave + 15];
	if ((uint64_t)mpb->wave + 16 + wave_n > app.doc.bytes.size()) return false;

	if (params->samples[sample].off + 0x50 > app.doc.bytes.size()) return false;

	rec = app.doc.bytes.data() + params->samples[sample].off;
	wave = app.doc.bytes.data() + mpb->wave + 16;
	return true;
}

static void append_u16(std::vector<uint8_t>& b, uint32_t v)
{
	b.push_back((uint8_t)(v & 0xFF));
	b.push_back((uint8_t)((v >> 8) & 0xFF));
}

static void append_u32(std::vector<uint8_t>& b, uint32_t v)
{
	append_u16(b, v);
	append_u16(b, v >> 16);
}

static std::string dialog_folder(HWND hwnd);

/* Root-key playback is 44100 Hz: AX's 32000 Hz clock times gcaxKeyRatio's unity. */
static void write_wav(const std::string& path, const std::vector<int16_t>& pcm)
{
	uint32_t data_bytes = (uint32_t)pcm.size() * 2;
	std::vector<uint8_t> hdr;
	hdr.reserve(44);
	hdr.insert(hdr.end(), {'R', 'I', 'F', 'F'});
	append_u32(hdr, 36 + data_bytes);
	hdr.insert(hdr.end(), {'W', 'A', 'V', 'E'});
	hdr.insert(hdr.end(), {'f', 'm', 't', ' '});
	append_u32(hdr, 16);
	append_u16(hdr, 1);
	append_u16(hdr, 1);
	append_u32(hdr, 44100);
	append_u32(hdr, 44100 * 2);
	append_u16(hdr, 2);
	append_u16(hdr, 16);
	hdr.insert(hdr.end(), {'d', 'a', 't', 'a'});
	append_u32(hdr, data_bytes);
	std::ofstream out(path, std::ios::binary);
	if (!out) throw std::runtime_error("cannot write " + path);

	out.write((const char*)hdr.data(), (std::streamsize)hdr.size());
	if (!pcm.empty()) out.write((const char*)pcm.data(), (std::streamsize)data_bytes);
	if (!out) throw std::runtime_error("cannot write " + path);
}

static void dump_wavs(App& app, int entry)
{
	std::string dir = dialog_folder(app.hwnd);
	if (dir.empty()) return;

	const Mpb* mpb = mpb_of(app.doc, entry);
	const Params* params = params_of(app.doc, entry);
	if (!mpb || !mpb->has_wave || !params || params->samples.empty()) {
		set_status(app, "This bank has no samples to dump.", true);
		return;
	}

	if (mpb->wave + 16 > app.doc.bytes.size()) {
		set_status(app, "This sample has no wave data.", true);
		return;
	}

	size_t wave_n = (app.doc.bytes[mpb->wave + 12] << 24) | (app.doc.bytes[mpb->wave + 13] << 16) | (app.doc.bytes[mpb->wave + 14] << 8) |
	                app.doc.bytes[mpb->wave + 15];
	if ((uint64_t)mpb->wave + 16 + wave_n > app.doc.bytes.size()) {
		set_status(app, "This sample has no wave data.", true);
		return;
	}

	const uint8_t* wave = app.doc.bytes.data() + mpb->wave + 16;
	int width = 2;
	int span = 10;
	int last = (int)params->samples.size() - 1;
	while (last >= span && width < 6) {
		width++;
		span *= 10;
	}

	char slash = dir.empty() ? '\\' : dir.back();
	std::string folder = (slash == '\\' || slash == '/') ? dir : dir + "\\";
	int wrote = 0;
	int failed = 0;
	std::string first;
	for (int i = 0; i < (int)params->samples.size(); i++) {
		if (params->samples[i].off + 0x50 > app.doc.bytes.size()) {
			failed++;
			if (first.empty()) first = "sample " + std::to_string(i) + " is outside the bank";
			continue;
		}

		DecodedSample decoded;
		std::string error;
		const uint8_t* rec = app.doc.bytes.data() + params->samples[i].off;
		if (!decode_gcax_sample(rec, wave, wave_n, decoded, error)) {
			failed++;
			if (first.empty()) first = "sample " + std::to_string(i) + ": " + error;
			continue;
		}

		char name[32];
		std::snprintf(name, sizeof name, "sample_%0*d.wav", width, i);
		try {
			write_wav(folder + name, decoded.pcm);
			wrote++;
		} catch (const std::exception& ex) {
			failed++;
			if (first.empty()) first = ex.what();
		}
	}

	if (wrote == 0) {
		set_status(app, first.empty() ? "No samples were written." : first, true);
		return;
	}

	std::string status = "Wrote " + std::to_string(wrote) + (wrote == 1 ? " WAV to " : " WAVs to ") + dir;
	if (failed) status += " (" + std::to_string(failed) + " failed)";
	set_status(app, status, failed != 0);
}

static void play_sample(App& app, int sample, int key, int root, int fine, bool shaped, int atk, int dec, int sus, int sus_level, int rel, int bias, float level)
{
	const uint8_t* rec = nullptr;
	const uint8_t* wave = nullptr;
	size_t wave_n = 0;
	if (!sample_bytes(app, sample, rec, wave, wave_n)) {
		set_status(app, "This sample has no wave data.", true);
		return;
	}

	DecodedSample decoded;
	std::string error;
	if (!decode_gcax_sample(rec, wave, wave_n, decoded, error)) {
		set_status(app, error, true);
		return;
	}

	Note note;
	note.pcm = std::move(decoded.pcm);
	note.loop = decoded.loop;
	note.loop_from = decoded.loop_from;
	note.loop_to = decoded.loop_to;
	note.key = key;
	note.shaped = shaped;
	note.level = level;
	double base = 1.0;
	EnvView view = env_view(bias, root, fine, key, atk, dec, sus, sus_level, rel);
	env_rates(view, note.atk_rate, note.dec_rate, note.sus_rate, note.rel_rate, note.sus_db);
	double ratio_key = 1.0;
	double ratio_root = 1.0;
	auto ratio = [](int root_key, int fine_tune, int played) {
		double r = 90316.8;
		int delta = root_key - played;
		const double semi = 1.0594630943592953;
		if (delta >= 0) {
			while (delta >= 12) {
				delta -= 12;
				r *= 0.5;
			}

			while (delta > 0) {
				delta--;
				r /= semi;
			}
		} else {
			delta = -delta;
			while (delta >= 12) {
				delta -= 12;
				r *= 2.0;
			}

			while (delta > 0) {
				delta--;
				r *= semi;
			}
		}

		int f = fine_tune;
		const double f16 = 1.007246412223704;
		const double f1 = 1.0004513695322617;
		while (f > 16) {
			f -= 16;
			r *= f16;
		}

		while (f > 0) {
			f--;
			r *= f1;
		}

		return r;
	};

	ratio_key = ratio(root, fine, key);
	ratio_root = ratio(root, fine, root);
	if (ratio_root > 1.0) base = ratio_key / ratio_root;
	note.step = base;
	if (!audio_play(std::move(note), error)) set_status(app, error, true);
}

static void piano_keyboard(App& app, const std::string& path, int sample, float level)
{
	int lo = 60, hi = 72, root = 60, fine = 0;
	int atk = 0, dec = 0, sus = 0, rel = 0, sus_level = 0, bias = 0;
	try {
		read_int(app.doc, path + ".key_lo", lo);
		read_int(app.doc, path + ".key_hi", hi);
		read_int(app.doc, path + ".root", root);
		read_int(app.doc, path + ".fine", fine);
		read_int(app.doc, path + ".atk", atk);
		read_int(app.doc, path + ".dec", dec);
		read_int(app.doc, path + ".sus", sus);
		read_int(app.doc, path + ".rel", rel);
		read_int(app.doc, path + ".sus_level", sus_level);
		read_int(app.doc, path + ".rate_bias", bias);
	} catch (const std::exception& ex) {
		ImGui::TextDisabled("%s", ex.what());
		return;
	}

	if (hi < lo) std::swap(lo, hi);
	char a[16], b[16], c[16];
	note_text(lo, a, sizeof a);
	note_text(hi, b, sizeof b);
	note_text(root, c, sizeof c);
	ImGui::Text("%s - %s", a, b);
	ImGui::SameLine();
	ImGui::TextDisabled("root %s", c);
	const float white_w = 18.0f;
	const float white_h = 92.0f;
	const float black_w = 11.0f;
	const float black_h = 56.0f;
	const float marker = 16.0f;
	int whites = whites_before(128);
	float total = whites * white_w;
	ImGui::BeginChild("piano", ImVec2(0, marker + white_h + 8.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
	ImVec2 origin = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("keys", ImVec2(total, marker + white_h));
	ImDrawList* draw = ImGui::GetWindowDrawList();
	int playing = audio_key();
	auto key_rect = [&](int key, ImVec2& r0, ImVec2& r1, bool& black) {
		black = is_black_key(key);
		float top = origin.y + marker;
		if (black) {
			float x = origin.x + whites_before(key) * white_w - black_w * 0.5f;
			r0 = ImVec2(x, top);
			r1 = ImVec2(x + black_w, top + black_h);
		} else {
			float x = origin.x + whites_before(key) * white_w;
			r0 = ImVec2(x + 1.0f, top);
			r1 = ImVec2(x + white_w - 1.0f, top + white_h);
		}
	};

	auto paint = [&](int key) {
		ImVec2 r0, r1;
		bool black = false;
		key_rect(key, r0, r1, black);
		bool in = key >= lo && key <= hi;
		bool on = key == playing;
		ImU32 col;
		if (black) col = on ? IM_COL32(90, 190, 160, 255) : in ? IM_COL32(36, 110, 96, 255) : IM_COL32(28, 30, 34, 255);
		else col = on ? IM_COL32(170, 230, 210, 255) : in ? IM_COL32(186, 228, 214, 255) : IM_COL32(226, 224, 216, 255);

		draw->AddRectFilled(r0, r1, col, black ? 1.0f : 2.0f);
	};

	for (int key = 0; key < 128; key++) {
		if (!is_black_key(key)) paint(key);
	}

	for (int key = 0; key < 128; key++) {
		if (is_black_key(key)) paint(key);
	}

	ImVec2 root0, root1;
	bool root_black = false;
	key_rect(root, root0, root1, root_black);
	ImVec2 tip((root0.x + root1.x) * 0.5f, origin.y + 3.0f);
	draw->AddTriangleFilled(ImVec2(tip.x - 5.0f, origin.y + marker - 2.0f), ImVec2(tip.x + 5.0f, origin.y + marker - 2.0f), ImVec2(tip.x, origin.y + 2.0f), IM_COL32(232, 186, 84, 255));
	auto key_at = [&](ImVec2 mouse) -> int {
		if (mouse.y < origin.y + marker) return -1;

		if (mouse.y < origin.y + marker + black_h) {
			for (int key = 0; key < 128; key++) {
				if (!is_black_key(key)) continue;

				ImVec2 r0, r1;
				bool black = false;
				key_rect(key, r0, r1, black);
				if (mouse.x >= r0.x && mouse.x < r1.x && mouse.y >= r0.y && mouse.y < r1.y) return key;
			}
		}

		for (int key = 0; key < 128; key++) {
			if (is_black_key(key)) continue;

			ImVec2 r0, r1;
			bool black = false;
			key_rect(key, r0, r1, black);
			if (mouse.x >= r0.x && mouse.x < r1.x && mouse.y >= r0.y && mouse.y < r1.y) return key;
		}

		return -1;
	};

	static int mode = 0;
	static int sounded = -1;
	ImVec2 mouse = ImGui::GetIO().MousePos;
	auto commit_root = [&](int value) {
		if (value < 0) value = 0;
		if (value > 127) value = 127;
		try {
			int cur = 0;
			read_int(app.doc, path + ".root", cur);
			if (cur != value) try_set(app, path + ".root", std::to_string(value), false);
		} catch (const std::exception&) {
		}
	};

	if (ImGui::IsItemActivated()) {
		int key = key_at(mouse);
		float root_x = (root0.x + root1.x) * 0.5f;
		if (mouse.y < origin.y + marker && std::fabs(mouse.x - root_x) < 12.0f) mode = 4;
		else if (key >= 0) {
			mode = 1;
			sounded = key;
			play_sample(app, sample, key, root, fine, true, atk, dec, sus, sus_level, rel, bias, level);
		} else mode = 0;
	}

	if (ImGui::IsItemActive() && mode == 1) {
		int key = key_at(mouse);
		if (key >= 0 && key != sounded) {
			sounded = key;
			play_sample(app, sample, key, root, fine, true, atk, dec, sus, sus_level, rel, bias, level);
		}
	} else if (ImGui::IsItemActive() && mode == 4) {
		int key = key_at(mouse);
		if (key >= 0) commit_root(key);
	}

	if (ImGui::IsItemDeactivated() && mode == 1) {
		audio_release();
		sounded = -1;
	}

	if (!ImGui::IsItemActive()) mode = 0;
	if (ImGui::IsItemHovered()) {
		int key = key_at(mouse);
		if (key >= 0) {
			char name[8];
			note_text(key, name, sizeof name);
			ImGui::SetTooltip("%s", name);
		}
	}

	ImGui::EndChild();
	ImGui::TextDisabled("Click a key to play it. Drag the marker to move the root.");
}

static void curve_editor(App& app, const Curve& curve)
{
	const float height = 180.0f;
	ImVec2 origin = ImGui::GetCursorScreenPos();
	float width = ImGui::GetContentRegionAvail().x;
	if (width < 128.0f) width = 128.0f;
	ImVec2 size(width, height);
	ImGui::InvisibleButton("curve", size);
	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(20, 22, 26, 255), 4.0f);
	uint32_t off = curve.off;
	if ((uint64_t)off + 128 > app.doc.bytes.size()) return;

	if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
		ImVec2 mouse = ImGui::GetIO().MousePos;
		int index = (int)((mouse.x - origin.x) / size.x * 128.0f);
		if (index < 0) index = 0;
		if (index > 127) index = 127;
		float t = 1.0f - (mouse.y - origin.y) / size.y;
		if (t < 0.0f) t = 0.0f;
		if (t > 1.0f) t = 1.0f;
		int value = (int)(t * 127.0f + 0.5f);
		if (app.doc.bytes[off + index] != (uint8_t)value) {
			app.doc.bytes[off + index] = (uint8_t)value;
			app.dirty = true;
		}
	}

	float bar = size.x / 128.0f;
	for (int i = 0; i < 128; i++) {
		float v = app.doc.bytes[off + i] / 127.0f;
		ImVec2 a(origin.x + i * bar, origin.y + size.y - size.y * v);
		ImVec2 b(origin.x + (i + 1) * bar - 1.0f, origin.y + size.y);
		draw->AddRectFilled(a, b, IM_COL32(86, 196, 166, 255));
	}

	if (ImGui::IsItemHovered()) {
		ImVec2 mouse = ImGui::GetIO().MousePos;
		int index = (int)((mouse.x - origin.x) / size.x * 128.0f);
		if (index < 0) index = 0;
		if (index > 127) index = 127;
		ImGui::SetTooltip("velocity %d = %u", index, (unsigned)app.doc.bytes[off + index]);
	}

	ImGui::TextDisabled("Drag across the bars. Each one is a velocity 0-127.");
}

static void hex_preview(const App& app, uint32_t off, uint32_t count)
{
	if (off >= app.doc.bytes.size()) return;

	if ((uint64_t)off + count > app.doc.bytes.size()) count = (uint32_t)app.doc.bytes.size() - off;
	std::string text;
	text.reserve(count * 4);
	char line[80];
	for (uint32_t i = 0; i < count; i += 16) {
		std::snprintf(line, sizeof line, "%04X  ", i);
		text += line;
		for (uint32_t j = 0; j < 16 && i + j < count; j++) {
			std::snprintf(line, sizeof line, "%02X ", app.doc.bytes[off + i + j]);
			text += line;
		}

		text += "\n";
	}

	if (app.mono) ImGui::PushFont(app.mono);
	ImGui::TextUnformatted(text.c_str());
	if (app.mono) ImGui::PopFont();
}

static std::wstring widen(const char* text)
{
	if (!text || !text[0]) {
		return {};
	}

	int n = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
	std::wstring wide(n > 0 ? n - 1 : 0, 0);
	if (n > 1) MultiByteToWideChar(CP_ACP, 0, text, -1, wide.data(), n);
	return wide;
}

static std::string narrow(const wchar_t* text)
{
	if (!text || !text[0]) {
		return {};
	}

	int n = WideCharToMultiByte(CP_ACP, 0, text, -1, nullptr, 0, nullptr, nullptr);
	std::string out(n > 0 ? n - 1 : 0, 0);
	if (n > 1) WideCharToMultiByte(CP_ACP, 0, text, -1, out.data(), n, nullptr, nullptr);
	return out;
}

static std::string dialog_pick(HWND hwnd, bool save, bool folder, const char* suggest, const COMDLG_FILTERSPEC* specs = nullptr, UINT nspec = 0, const wchar_t* ext = L"mpb")
{
	IFileDialog* dialog = nullptr;
	REFCLSID clsid = save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
	if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) || !dialog) {
		return {};
	}

	COMDLG_FILTERSPEC banks[] = {
		{L"GCAX banks", L"*.mlt;*.mpb;*.mbp;*.msb;*.mpbp;*.mpbw"},
		{L"All files", L"*.*"},
	};

	if (!specs) {
		specs = banks;
		nspec = 2;
	}

	if (!folder) {
		dialog->SetFileTypes(nspec, specs);
		dialog->SetDefaultExtension(ext);
	}

	std::wstring name = widen(suggest);
	if (!name.empty()) dialog->SetFileName(name.c_str());
	DWORD options = 0;
	dialog->GetOptions(&options);
	options |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST;
	if (save) {
		options |= FOS_OVERWRITEPROMPT;
	} else {
		options |= FOS_FILEMUSTEXIST;
	}

	if (folder) {
		options |= FOS_PICKFOLDERS;
	}

	dialog->SetOptions(options);
	std::string result;
	if (SUCCEEDED(dialog->Show(hwnd))) {
		IShellItem* item = nullptr;
		if (SUCCEEDED(dialog->GetResult(&item)) && item) {
			PWSTR path = nullptr;
			if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
				result = narrow(path);
				CoTaskMemFree(path);
			}

			item->Release();
		}
	}

	dialog->Release();
	return result;
}

static std::string dialog_file(HWND hwnd, bool save, const char* suggest)
{
	return dialog_pick(hwnd, save, false, suggest);
}

static std::string dialog_folder(HWND hwnd)
{
	return dialog_pick(hwnd, false, true, nullptr);
}

static std::string dialog_wav(HWND hwnd)
{
	COMDLG_FILTERSPEC specs[] = {
		{L"WAV audio", L"*.wav"},
		{L"All files", L"*.*"},
	};

	return dialog_pick(hwnd, false, false, nullptr, specs, 2, L"wav");
}

static bool save_copy(App& app, const std::string& path)
{
	try {
		save_file(app.doc, path);
		app.path = path;
		app.doc.name = path;
		app.dirty = false;
		set_title(app);
		set_status(app, "Saved " + file_leaf(path), false);
		return true;
	} catch (const std::exception& ex) {
		set_status(app, ex.what(), true);
		MessageBoxA(app.hwnd, ex.what(), "EMALT", MB_ICONERROR);
		return false;
	}
}

static bool save_app(App& app)
{
	if (!app.loaded) return false;

	if (app.path.empty()) {
		std::string path = dialog_file(app.hwnd, true, "bank.mpb");
		if (path.empty()) return false;

		return save_copy(app, path);
	}

	return save_copy(app, app.path);
}

static bool save_app_as(App& app)
{
	if (!app.loaded) return false;

	std::string path = dialog_file(app.hwnd, true, file_leaf(app.path).c_str());
	if (path.empty()) return false;

	return save_copy(app, path);
}

static bool confirm_discard(HWND hwnd)
{
	int answer = MessageBoxA(hwnd, "Save changes to this bank?", "EMALT", MB_YESNOCANCEL | MB_ICONWARNING);
	if (answer == IDCANCEL) return false;

	if (answer == IDNO) return true;

	return g_app && save_app(*g_app);
}

static void open_path(App& app, const std::string& path)
{
	audio_stop();
	if (app.dirty && !confirm_discard(app.hwnd)) return;

	try {
		app.doc = load_file(path);
		app.path = path;
		app.loaded = app.doc.ok;
		app.dirty = false;
		app.sel = sel_root();
		if (app.doc.kind == FileKind::Mlt && !app.doc.entries.empty()) app.sel = sel_entry(0);
		set_title(app);
		set_status(app, "Opened " + file_leaf(path), false);
	} catch (const std::exception& ex) {
		set_status(app, ex.what(), true);
		MessageBoxA(app.hwnd, ex.what(), "EMALT", MB_ICONERROR);
	}
}

static void do_open(App& app)
{
	std::string path = dialog_file(app.hwnd, false, nullptr);
	if (!path.empty()) open_path(app, path);
}

static void replace_from_file(App& app, int which)
{
	int entry = bank_entry(app);
	if (which != 2 && entry < -1) return;

	const char* title = which == 0 ? "wave.bin" : which == 1 ? "params.bin" : "bank.mpb";
	std::string path = dialog_file(app.hwnd, false, title);
	if (path.empty()) return;

	try {
		std::vector<uint8_t> bytes = read_file(path);
		if (which == 0) replace_wave(app.doc, std::move(bytes), entry);
		else if (which == 1) replace_params(app.doc, std::move(bytes), entry);
		else replace_bank(app.doc, app.sel.entry, std::move(bytes));

		app.dirty = true;
		set_status(app, std::string("Replaced from ") + file_leaf(path), false);
	} catch (const std::exception& ex) {
		set_status(app, ex.what(), true);
		MessageBoxA(app.hwnd, ex.what(), "EMALT", MB_ICONERROR);
	}
}

static void do_extract(App& app)
{
	if (!app.loaded) return;

	std::string dir = dialog_folder(app.hwnd);
	if (dir.empty()) return;

	try {
		std::string listed = extract_to(app.doc, dir);
		set_status(app, "Extracted into " + dir, false);
		if (!listed.empty()) MessageBoxA(app.hwnd, listed.c_str(), "EMALT", MB_OK);
	} catch (const std::exception& ex) {
		set_status(app, ex.what(), true);
		MessageBoxA(app.hwnd, ex.what(), "EMALT", MB_ICONERROR);
	}
}

static bool branch(App& app, const char* id, const char* label, const Sel& sel)
{
	if (app.filter[0]) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (same_sel(app.sel, sel)) {
		flags |= ImGuiTreeNodeFlags_Selected;
	}

	bool open = ImGui::TreeNodeEx(id, flags, "%s", label);
	if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) app.sel = sel;
	return open;
}

static void leaf(App& app, const char* id, const char* label, const Sel& sel)
{
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (same_sel(app.sel, sel)) {
		flags |= ImGuiTreeNodeFlags_Selected;
	}

	ImGui::TreeNodeEx(id, flags, "%s", label);
	if (ImGui::IsItemClicked()) app.sel = sel;
}

static void draw_params_tree(App& app, int entry, const Params& params)
{
	std::string base = (app.doc.kind == FileKind::Mlt) ? "e" + std::to_string(entry) : "bank";
	char id[64];
	char label[96];
	std::snprintf(id, sizeof id, "%s-samples", base.c_str());
	Sel samples = sel_entry(entry);
	samples.kind = SelKind::Sample;
	samples.sample = -1;
	bool samples_open = branch(app, id, ("samples  " + std::to_string(params.samples.size())).c_str(), samples);
	if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) app.sel = samples;
	if (ImGui::BeginPopupContextItem()) {
		if (ImGui::MenuItem("Dump WAVs")) queue_edit(app, EditDumpWavs, entry, 0, 0, 0);
		ImGui::EndPopup();
	}

	if (samples_open) {
		for (int i = 0; i < (int)params.samples.size(); i++) {
			uint32_t off = params.samples[i].off;
			unsigned fmt = off + 0x4A < app.doc.bytes.size() ? app.doc.bytes[off + 0x4A] : 0;
			const char* fmt_name = fmt == 0 ? "adpcm" : fmt == 10 ? "pcm16" : fmt == 25 ? "pcm8" : "fmt";
			std::snprintf(label, sizeof label, "%d   %s", i, fmt_name);
			if (!match_filter(app, label)) continue;

			Sel sel = sel_entry(entry);
			sel.kind = SelKind::Sample;
			sel.sample = i;
			std::snprintf(id, sizeof id, "%s-s%d", base.c_str(), i);
			leaf(app, id, label, sel);
		}

		ImGui::TreePop();
	}

	std::snprintf(id, sizeof id, "%s-vel", base.c_str());
	Sel curves = sel_entry(entry);
	curves.kind = SelKind::Curve;
	curves.curve = -1;
	if (branch(app, id, ("velocity  " + std::to_string(params.curves.size())).c_str(), curves)) {
		for (int i = 0; i < (int)params.curves.size(); i++) {
			std::snprintf(label, sizeof label, "curve %d", i);
			if (!match_filter(app, label)) continue;

			Sel sel = sel_entry(entry);
			sel.kind = SelKind::Curve;
			sel.curve = i;
			std::snprintf(id, sizeof id, "%s-c%d", base.c_str(), i);
			leaf(app, id, label, sel);
		}

		ImGui::TreePop();
	}

	for (int p = 0; p < (int)params.progs.size(); p++) {
		const Program& prog = params.progs[p];
		std::snprintf(label, sizeof label, "program %d", p);
		std::snprintf(id, sizeof id, "%s-p%d", base.c_str(), p);
		Sel psel = sel_entry(entry);
		psel.kind = SelKind::Prog;
		psel.prog = p;
		bool prog_open = branch(app, id, label, psel);
		if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) app.sel = psel;
		if (ImGui::BeginPopupContextItem()) {
			if (ImGui::MenuItem("Add layer")) queue_edit(app, EditAddLayer, entry, p, 0, 0);
			if (ImGui::MenuItem("Add program")) queue_edit(app, EditAddProgram, entry, p, 0, 0);
			ImGui::EndPopup();
		}

		if (!prog_open) continue;

		for (int L = 0; L < (int)prog.layers.size(); L++) {
			const Layer& layer = prog.layers[L];
			unsigned up = layer.off + 4 < app.doc.bytes.size() ? app.doc.bytes[layer.off + 4] : 0;
			unsigned down = layer.off + 5 < app.doc.bytes.size() ? app.doc.bytes[layer.off + 5] : 0;
			std::snprintf(label, sizeof label, "layer %d   bend %u/%u", L, up, down);
			std::snprintf(id, sizeof id, "%s-p%d-l%d", base.c_str(), p, L);
			Sel lsel = psel;
			lsel.kind = SelKind::Layer;
			lsel.layer = L;
			bool layer_open = branch(app, id, label, lsel);
			if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) app.sel = lsel;
			if (ImGui::BeginPopupContextItem()) {
				if (ImGui::MenuItem("Add region")) queue_edit(app, EditAddRegion, entry, p, L, 0);
				if (ImGui::MenuItem("Remove layer")) queue_edit(app, EditRemoveLayer, entry, p, L, 0);
				ImGui::EndPopup();
			}

			if (!layer_open) continue;

			for (int r = 0; r < (int)layer.regions.size(); r++) {
				uint32_t roff = layer.regions[r].off;
				int lo = roff + 8 < app.doc.bytes.size() ? app.doc.bytes[roff + 8] : 0;
				int hi = roff + 9 < app.doc.bytes.size() ? app.doc.bytes[roff + 9] : 0;
				char nlo[8], nhi[8];
				note_text(lo, nlo, sizeof nlo);
				note_text(hi, nhi, sizeof nhi);
				unsigned sample = 0;
				if (roff + 4 <= app.doc.bytes.size()) {
					sample = (app.doc.bytes[roff] << 24) | (app.doc.bytes[roff + 1] << 16) | (app.doc.bytes[roff + 2] << 8) |
					         app.doc.bytes[roff + 3];
				}

				std::snprintf(label, sizeof label, "region %d   %s-%s   sample %u", r, nlo, nhi, sample);
				if (!match_filter(app, label)) continue;

				Sel rsel = lsel;
				rsel.kind = SelKind::Region;
				rsel.region = r;
				std::snprintf(id, sizeof id, "%s-p%d-l%d-r%d", base.c_str(), p, L, r);
				leaf(app, id, label, rsel);
				if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) app.sel = rsel;
				if (ImGui::BeginPopupContextItem()) {
					if (ImGui::MenuItem("Delete region")) queue_edit(app, EditDeleteRegion, entry, p, L, r);
					ImGui::EndPopup();
				}
			}

			ImGui::TreePop();
		}

		ImGui::TreePop();
	}
}

static void draw_tree(App& app)
{
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputTextWithHint("##filter", "Filter", app.filter, sizeof app.filter);
	if (!app.loaded) {
		ImGui::TextDisabled("No bank open.");
		return;
	}

	leaf(app, "root", file_leaf(app.path).c_str(), sel_root());
	if (app.doc.kind != FileKind::Mlt && params_of(app.doc, -1)) {
		if (ImGui::BeginPopupContextItem()) {
			if (ImGui::MenuItem("Add program")) queue_edit(app, EditAddProgram, -1, 0, 0, 0);
			ImGui::EndPopup();
		}
	}

	if (app.doc.kind == FileKind::Mlt) {
		for (const Entry& entry : app.doc.entries) {
			char label[96];
			char id[32];
			std::snprintf(id, sizeof id, "entry%d", entry.index);
			if (entry.kind == EntryKind::Hang) std::snprintf(label, sizeof label, "entry %u   loader stops here", entry.index);
			else {
				std::string slot = "0";
				std::string type = "?";
				try {
					slot = get_field(app.doc, "entry[" + std::to_string(entry.index) + "].bank");
					type = get_field(app.doc, "entry[" + std::to_string(entry.index) + "].type");
				} catch (const std::exception&) {
				}

				for (char& c : type) c = (char)std::toupper((unsigned char)c);
				std::snprintf(label, sizeof label, "entry %u   %s   slot %s", entry.index, type.c_str(), slot.c_str());
			}

			Sel sel = sel_entry((int)entry.index);
			const Params* params = params_of(app.doc, (int)entry.index);
			if (!params) {
				leaf(app, id, label, sel);
				continue;
			}

			bool entry_open = branch(app, id, label, sel);
			if (ImGui::BeginPopupContextItem()) {
				if (ImGui::MenuItem("Add program")) queue_edit(app, EditAddProgram, (int)entry.index, 0, 0, 0);
				ImGui::EndPopup();
			}

			if (!entry_open) continue;

			draw_params_tree(app, (int)entry.index, *params);
			ImGui::TreePop();
		}

		return;
	}

	const Params* params = params_of(app.doc, -1);
	if (params) draw_params_tree(app, -1, *params);
}

static void draw_warnings(const App& app)
{
	if (app.doc.warnings.empty()) return;

	if (!ImGui::CollapsingHeader("Warnings")) return;

	for (const std::string& warning : app.doc.warnings) ImGui::TextWrapped("%s", warning.c_str());
}

static void draw_root(App& app)
{
	section(kind_name(app.doc.kind));
	ImGui::Text("%s", file_leaf(app.path).c_str());
	ImGui::TextDisabled("%u bytes", (unsigned)app.doc.bytes.size());
	if (app.doc.kind == FileKind::Mlt) ImGui::Text("%u entries", (unsigned)app.doc.entries.size());
	const Params* params = params_of(app.doc, app.doc.kind == FileKind::Mlt ? app.sel.entry : -1);
	if (app.doc.kind != FileKind::Mlt && params) {
		ImGui::Text("%u programs, %u samples, %u velocity curves", (unsigned)params->progs.size(), (unsigned)params->samples.size(),
		            (unsigned)params->curves.size());
	}

	const Mpb* mpb = (app.doc.kind == FileKind::Mlt) ? nullptr : mpb_of(app.doc, -1);
	if (mpb && mpb->has_wave && mpb->wave + 16 <= app.doc.bytes.size()) {
		unsigned size = (app.doc.bytes[mpb->wave + 12] << 24) | (app.doc.bytes[mpb->wave + 13] << 16) |
		                (app.doc.bytes[mpb->wave + 14] << 8) | app.doc.bytes[mpb->wave + 15];
		ImGui::Text("Wave %u bytes", size);
	}

	draw_warnings(app);
}

static void draw_entry(App& app)
{
	if (app.sel.entry < 0 || app.sel.entry >= (int)app.doc.entries.size()) {
		ImGui::TextDisabled("This entry is gone.");
		return;
	}

	const Entry& entry = app.doc.entries[app.sel.entry];
	std::string base = "entry[" + std::to_string(app.sel.entry) + "]";
	section("Entry");
	if (entry.kind == EntryKind::Hang) {
		ImGui::TextWrapped("The bank word is 0xFFFFFFFF. gcaxLoaderLoad stays on this entry and does not move on.");
		hex_field(app, base + ".bank_word", "Bank word", true);
		return;
	}

	std::string type;
	try {
		type = get_field(app.doc, base + ".type");
	} catch (const std::exception&) {
		type = "";
	}

	int current = type == "mpb" ? 0 : type == "msb" ? 1 : 2;
	const char* types[] = {"MPB", "MSB"};
	if (current < 2) {
		if (ImGui::Combo("Type", &current, types, 2)) {
			try_set(app, base + ".type", current == 0 ? "mpb" : "msb", true);
		}
	} else int_field(app, base + ".type", "Type nibble", 0, 15);

	ImGui::TextDisabled("Changing the type keeps the other bits of the word. The loader calls MPB for 1 and MSB for 4.");
	int_field(app, base + ".bank", "Slot", 0, 15);
	hex_field(app, base + ".offset", "Offset", true);
	hex_field(app, base + ".extra", "Extra", false);
	if (const Mpb* mpb = mpb_of(app.doc, app.sel.entry)) {
		if (mpb->has_wave && mpb->wave + 16 <= app.doc.bytes.size()) {
			unsigned size = (app.doc.bytes[mpb->wave + 12] << 24) | (app.doc.bytes[mpb->wave + 13] << 16) |
			                (app.doc.bytes[mpb->wave + 14] << 8) | app.doc.bytes[mpb->wave + 15];
			ImGui::Text("Wave %u bytes", size);
		}

		if (const Params* params = params_of(app.doc, app.sel.entry)) {
			ImGui::Text("%u programs, %u samples, %u velocity curves", (unsigned)params->progs.size(),
			            (unsigned)params->samples.size(), (unsigned)params->curves.size());
		}
	}

	if (entry.msb && entry.msb->span > 16) {
		section("Sequence");
		ImGui::TextDisabled("gcaxSndLoadMsb copies this blob. The player reads it as MIDI-style events.");
		hex_preview(app, entry.msb->head + 16, entry.msb->span - 16 > 256 ? 256 : entry.msb->span - 16);
	}

	draw_warnings(app);
}

static void draw_sample(App& app, const Params& params)
{
	if (app.sel.sample < 0 || app.sel.sample >= (int)params.samples.size()) {
		ImGui::TextDisabled("Pick a sample.");
		return;
	}

	std::string path = join_path(base_path(app), "sample[" + std::to_string(app.sel.sample) + "]");
	section("Sample");
	int length = 0, format = -1, loop = 0;
	try {
		read_int(app.doc, path + ".length", length);
		read_int(app.doc, path + ".format", format);
		read_int(app.doc, path + ".loop", loop);
	} catch (const std::exception&) {
	}

	const char* format_name = format == 0 ? "ADPCM" : format == 10 ? "PCM16" : format == 25 ? "PCM8" : "unknown format";
	ImGui::Text("%d samples, %s", length, format_name);
	bool enabled = loop != 0;
	if (ImGui::Checkbox("Loop", &enabled)) {
		try_set(app, path + ".loop", enabled ? "1" : "0", false);
	}

	if (ImGui::Button("Play")) play_sample(app, app.sel.sample, 60, 60, 0, false, 0, 0, 0, 0, 0, 0, 1.0f);
	ImGui::SameLine();
	if (ImGui::Button("Stop")) audio_release();
	ImGui::SameLine();
	if (ImGui::Button("Import WAV...")) {
		std::string file = dialog_wav(app.hwnd);
		if (!file.empty()) {
			try {
				std::vector<uint8_t> wav = read_file(file);
				EncodedAdpcm enc;
				std::string error;
				if (!encode_wav(wav.data(), wav.size(), enabled ? 0 : -1, enc, error)) set_status(app, error, true);
				else {
					SampleWave wave;
					wave.data = std::move(enc.data);
					wave.length = enc.length;
					wave.nibbles = enc.nibbles;
					wave.rate = enc.rate;
					wave.loop = enc.loop;
					wave.loop_adr = enc.loop_adr;
					wave.end_adr = enc.end_adr;
					wave.cur_adr = enc.cur_adr;
					for (int i = 0; i < 16; i++) wave.coef[i] = enc.coef[i];
					wave.pred_scale = enc.pred_scale;
					wave.yn1 = enc.yn1;
					wave.yn2 = enc.yn2;
					wave.loop_pred_scale = enc.loop_pred_scale;
					wave.loop_yn1 = enc.loop_yn1;
					wave.loop_yn2 = enc.loop_yn2;
					wave.format = 0;
					int entry = app.doc.kind == FileKind::Mlt ? app.sel.entry : -1;
					install_sample_wave(app.doc, entry, app.sel.sample, std::move(wave));
					app.dirty = true;
					set_status(app, "Imported " + file_leaf(file), false);
					set_title(app);
				}
			} catch (const std::exception& ex) {
				set_status(app, ex.what(), true);
			}
		}
	}

	ImGui::TextDisabled("Importing a WAV encodes it and writes the ADPCM header.");
}

static void draw_curve(App& app, const Params& params)
{
	if (app.sel.curve < 0 || app.sel.curve >= (int)params.curves.size()) {
		ImGui::TextDisabled("Pick a velocity curve.");
		return;
	}

	section("Velocity curve");
	ImGui::Text("Curve %d", app.sel.curve);
	curve_editor(app, params.curves[app.sel.curve]);
}

static void queue_edit(App& app, int op, int entry, int prog, int layer, int region)
{
	app.pending_edit = op;
	app.pending_entry = entry;
	app.pending_prog = prog;
	app.pending_layer = layer;
	app.pending_region = region;
}

static void region_rgb(int index, int& r, int& g, int& b)
{
	static const int pal[][3] = {
		{78, 186, 156}, {232, 186, 84}, {120, 168, 230}, {220, 120, 140}, {168, 140, 220}, {150, 200, 120},
	};

	r = pal[index % 6][0];
	g = pal[index % 6][1];
	b = pal[index % 6][2];
}

static ImU32 region_key_color(int index, bool black)
{
	int r = 0, g = 0, b = 0;
	region_rgb(index, r, g, b);
	if (black) return IM_COL32(r * 45 / 100, g * 50 / 100, b * 50 / 100, 255);

	auto mix = [](int c, int ivory) { return ivory + (c - ivory) * 70 / 100; };
	return IM_COL32(mix(r, 226), mix(g, 224), mix(b, 216), 255);
}

static void layer_keymap(App& app, const std::string& path, const Layer& layer)
{
	const float white_w = 18.0f;
	const float white_h = 56.0f;
	const float black_w = 11.0f;
	const float black_h = 34.0f;
	const float row_h = 22.0f;
	int count = (int)layer.regions.size();
	struct Zone {
		int index = 0;
		int lo = 0;
		int hi = 0;
		int root = 60;
	};

	std::vector<Zone> zones;
	zones.reserve(layer.regions.size());
	for (int i = 0; i < count; i++) {
		Zone z;
		z.index = i;
		std::string rpath = path + ".region[" + std::to_string(i) + "]";
		try {
			read_int(app.doc, rpath + ".key_lo", z.lo);
			read_int(app.doc, rpath + ".key_hi", z.hi);
			read_int(app.doc, rpath + ".root", z.root);
		} catch (const std::exception&) {
			continue;
		}

		if (z.lo < 0) z.lo = 0;
		if (z.hi > 127) z.hi = 127;
		if (z.hi < z.lo) std::swap(z.lo, z.hi);
		zones.push_back(z);
	}

	int whites = whites_before(128);
	float total = whites * white_w;
	float map_h = white_h + 8.0f + (zones.empty() ? row_h : (float)zones.size() * row_h);
	ImGui::BeginChild("layermap", ImVec2(0, map_h + ImGui::GetStyle().ScrollbarSize), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
	ImVec2 origin = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("map", ImVec2(total, map_h));
	ImDrawList* draw = ImGui::GetWindowDrawList();
	auto key_rect = [&](int key, ImVec2& r0, ImVec2& r1, bool& black) {
		black = is_black_key(key);
		if (black) {
			float x = origin.x + whites_before(key) * white_w - black_w * 0.5f;
			r0 = ImVec2(x, origin.y);
			r1 = ImVec2(x + black_w, origin.y + black_h);
		} else {
			float x = origin.x + whites_before(key) * white_w;
			r0 = ImVec2(x + 1.0f, origin.y);
			r1 = ImVec2(x + white_w - 1.0f, origin.y + white_h);
		}
	};

	auto paint = [&](int key) {
		ImVec2 r0, r1;
		bool black = false;
		key_rect(key, r0, r1, black);
		int hits[8];
		int cover = 0;
		for (const Zone& z : zones) {
			if (key < z.lo || key > z.hi) continue;

			if (cover < 8) hits[cover] = z.index;
			cover++;
		}

		if (cover <= 0) {
			ImU32 col = black ? IM_COL32(28, 30, 34, 255) : IM_COL32(226, 224, 216, 255);
			draw->AddRectFilled(r0, r1, col, black ? 1.0f : 2.0f);
			return;
		}

		int shown = cover < 8 ? cover : 8;
		if (shown == 1) {
			draw->AddRectFilled(r0, r1, region_key_color(hits[0], black), black ? 1.0f : 2.0f);
			return;
		}

		float band = (r1.y - r0.y) / (float)shown;
		for (int i = 0; i < shown; i++) {
			float y0 = r0.y + band * (float)i;
			float y1 = i + 1 == shown ? r1.y : y0 + band;
			draw->AddRectFilled(ImVec2(r0.x, y0), ImVec2(r1.x, y1), region_key_color(hits[i], black));
		}
	};

	for (int key = 0; key < 128; key++) {
		if (!is_black_key(key)) paint(key);
	}

	for (int key = 0; key < 128; key++) {
		if (is_black_key(key)) paint(key);
	}

	auto key_x = [&](float x) {
		if (x < origin.x) return 0;

		if (x >= origin.x + total) return 127;

		int best = 0;
		float best_d = 1.0e9f;
		for (int key = 0; key < 128; key++) {
			ImVec2 r0, r1;
			bool black = false;
			key_rect(key, r0, r1, black);
			float d = std::fabs(x - (r0.x + r1.x) * 0.5f);
			if (d < best_d) {
				best_d = d;
				best = key;
			}
		}

		return best;
	};

	struct Bar {
		int index = 0;
		float x0 = 0;
		float x1 = 0;
		float y0 = 0;
		float y1 = 0;
	};

	std::vector<Bar> bars;
	for (int i = 0; i < (int)zones.size(); i++) {
		const Zone& z = zones[i];
		ImVec2 a0, a1, b0, b1;
		bool dummy = false;
		key_rect(z.lo, a0, a1, dummy);
		key_rect(z.hi, b0, b1, dummy);
		Bar bar;
		bar.index = z.index;
		bar.x0 = a0.x < b0.x ? a0.x : b0.x;
		bar.x1 = a1.x > b1.x ? a1.x : b1.x;
		bar.y0 = origin.y + white_h + 6.0f + i * row_h;
		bar.y1 = bar.y0 + row_h - 4.0f;
		bars.push_back(bar);
		int cr = 0, cg = 0, cb = 0;
		region_rgb(z.index, cr, cg, cb);
		draw->AddRectFilled(ImVec2(bar.x0, bar.y0), ImVec2(bar.x1, bar.y1), IM_COL32(cr, cg, cb, 230), 3.0f);
		char lo_n[8], hi_n[8], caption[32];
		note_text(z.lo, lo_n, sizeof lo_n);
		note_text(z.hi, hi_n, sizeof hi_n);
		std::snprintf(caption, sizeof caption, "%d  %s-%s", z.index, lo_n, hi_n);
		draw->PushClipRect(ImVec2(bar.x0, bar.y0), ImVec2(bar.x1, bar.y1), true);
		draw->AddText(ImVec2(bar.x0 + 4.0f, bar.y0 + 1.0f), IM_COL32(16, 20, 18, 255), caption);
		draw->PopClipRect();
	}

	static int drag = 0;
	static int drag_index = -1;
	static int lo0 = 0;
	static int hi0 = 0;
	static int anchor = 0;
	ImVec2 mouse = ImGui::GetIO().MousePos;
	auto zone_of = [&](int index) -> const Zone* {
		for (const Zone& z : zones) {
			if (z.index == index) return &z;
		}

		return nullptr;
	};

	auto commit = [&](int index, const char* field, int value) {
		if (value < 0) value = 0;
		if (value > 127) value = 127;
		std::string field_path = path + ".region[" + std::to_string(index) + "]." + field;
		try {
			int cur = 0;
			read_int(app.doc, field_path, cur);
			if (cur != value) try_set(app, field_path, std::to_string(value), false);
		} catch (const std::exception&) {
		}
	};

	if (ImGui::IsItemActivated()) {
		drag = 0;
		drag_index = -1;
		for (int i = (int)bars.size() - 1; i >= 0; i--) {
			const Bar& bar = bars[i];
			if (mouse.y < bar.y0 || mouse.y >= bar.y1 || mouse.x < bar.x0 - 3.0f || mouse.x >= bar.x1 + 3.0f) continue;

			const Zone* z = zone_of(bar.index);
			if (!z) continue;

			float edge = 6.0f;
			if (bar.x1 - bar.x0 < 20.0f) edge = (bar.x1 - bar.x0) * 0.28f;
			drag_index = bar.index;
			lo0 = z->lo;
			hi0 = z->hi;
			anchor = key_x(mouse.x);
			if (mouse.x <= bar.x0 + edge) drag = 1;
			else if (mouse.x >= bar.x1 - edge) drag = 3;
			else drag = 2;

			break;
		}
	}

	if (ImGui::IsItemActive() && drag && drag_index >= 0) {
		int key = key_x(mouse.x);
		if (drag == 1) commit(drag_index, "key_lo", key <= hi0 ? key : hi0);
		else if (drag == 3) commit(drag_index, "key_hi", key >= lo0 ? key : lo0);
		else if (drag == 2) {
			int delta = key - anchor;
			int lo = lo0 + delta;
			int hi = hi0 + delta;
			int width = hi0 - lo0;
			if (lo < 0) {
				lo = 0;
				hi = width;
			}

			if (hi > 127) {
				hi = 127;
				lo = 127 - width;
			}

			commit(drag_index, "key_lo", lo);
			commit(drag_index, "key_hi", hi);
		}

		ImGui::SetMouseCursor(drag == 2 ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_ResizeEW);
	}

	if (!ImGui::IsItemActive()) drag = 0;
	if (ImGui::IsItemHovered()) {
		for (const Bar& bar : bars) {
			if (mouse.y < bar.y0 || mouse.y >= bar.y1 || mouse.x < bar.x0 || mouse.x >= bar.x1) continue;

			float edge = 6.0f;
			if (bar.x1 - bar.x0 < 20.0f) edge = (bar.x1 - bar.x0) * 0.28f;
			if (mouse.x <= bar.x0 + edge || mouse.x >= bar.x1 - edge) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
		}
	}

	ImGui::EndChild();
}

static void draw_layer(App& app)
{
	std::string path = join_path(base_path(app), "prog[" + std::to_string(app.sel.prog) + "].layer[" + std::to_string(app.sel.layer) + "]");
	section("Layer");
	slider_int(app, path + ".bend_up", "Bend up###bendup", 0, 255);
	slider_int(app, path + ".bend_down", "Bend down###benddown", 0, 255);
	ImGui::TextDisabled("Pitch bend scales by these. The unknown 10 bytes stay as they are.");
	const Params* params = params_of(app.doc, app.doc.kind == FileKind::Mlt ? app.sel.entry : -1);
	section("Regions");
	if (!params || app.sel.prog < 0 || app.sel.prog >= (int)params->progs.size() || app.sel.layer < 0 ||
	    app.sel.layer >= (int)params->progs[app.sel.prog].layers.size()) {
		ImGui::TextDisabled("This layer is gone.");
		return;
	}

	const Layer& layer = params->progs[app.sel.prog].layers[app.sel.layer];
	if (layer.regions.empty()) ImGui::TextDisabled("Right-click the layer and choose Add region.");
	else {
		layer_keymap(app, path, layer);
		ImGui::TextDisabled("Drag a bar to move that region's keys. Drag either end to change the range. Right-click a region to delete it.");
	}
}

static void draw_region(App& app, const Params& params)
{
	if (app.sel.prog < 0 || app.sel.prog >= (int)params.progs.size()) return;

	const Program& prog = params.progs[app.sel.prog];
	if (app.sel.layer < 0 || app.sel.layer >= (int)prog.layers.size()) return;

	const Layer& layer = prog.layers[app.sel.layer];
	if (app.sel.region < 0 || app.sel.region >= (int)layer.regions.size()) {
		ImGui::TextDisabled("Pick a region.");
		return;
	}

	std::string path = join_path(base_path(app), "prog[" + std::to_string(app.sel.prog) + "].layer[" + std::to_string(app.sel.layer) +
	                                                 "].region[" + std::to_string(app.sel.region) + "]");
	section("Region");
	int sample = 0, level = 100;
	try {
		read_int(app.doc, path + ".sample", sample);
		read_int(app.doc, path + ".level", level);
	} catch (const std::exception&) {
	}

	float gain = level <= 0 ? 0.0f : level / 127.0f;
	piano_keyboard(app, path, sample, gain);
	slider_int(app, path + ".vel_lo", "Velocity low###vlo", 0, 127);
	slider_int(app, path + ".vel_hi", "Velocity high###vhi", 0, 127);
	int curves = (int)params.curves.size();
	if (curves < 1) curves = 1;
	slider_int(app, path + ".velcurve", "Velocity curve###vcur", 0, curves - 1);
	int_field(app, path + ".sample", "Sample", 0, 65535);
	section("Pitch and level");
	slider_int(app, path + ".fine", "Fine###fine", 0, 255);
	slider_int(app, path + ".level", "Level###level", 0, 127);
	slider_int(app, path + ".pan", "Pan###pan", 0, 31);
	ImGui::TextDisabled("Pan uses the low 5 bits.");
	section("Envelope");
	int root = 60, fine = 0;
	try {
		read_int(app.doc, path + ".root", root);
		read_int(app.doc, path + ".fine", fine);
	} catch (const std::exception&) {
	}

	envelope_editor(app, path, root, fine);
	slider_int(app, path + ".rate_bias", "Rate bias###bias", 0, 127);
	slider_int(app, path + ".atten", "Attenuation###atten", 0, 127);
	slider_int(app, path + ".fx_send", "Effect send###fx", 0, 127);
	slider_int(app, path + ".fx_invert", "Effect invert###fxi", 0, 127);
}

static void draw_inspector(App& app)
{
	if (!app.loaded) {
		ImVec2 avail = ImGui::GetContentRegionAvail();
		const char* line = "Open a .mlt or .mpb";
		ImVec2 size = ImGui::CalcTextSize(line);
		ImGui::SetCursorPosX((avail.x - size.x) * 0.5f);
		ImGui::SetCursorPosY(avail.y * 0.38f);
		if (ImGui::Button(line)) {
			do_open(app);
		}

		ImGui::SetCursorPosX((avail.x - ImGui::CalcTextSize("or drop a bank on the window").x) * 0.5f);
		ImGui::TextDisabled("or drop a bank on the window");
		return;
	}

	const Params* params = params_of(app.doc, app.doc.kind == FileKind::Mlt ? app.sel.entry : -1);
	switch (app.sel.kind) {
	case SelKind::Root: draw_root(app); break;
	case SelKind::Entry: draw_entry(app); break;
	case SelKind::Sample:
		if (params) draw_sample(app, *params);
		break;
	case SelKind::Curve:
		if (params) draw_curve(app, *params);
		break;
	case SelKind::Prog:
		section("Program");
		if (params && app.sel.prog >= 0 && app.sel.prog < (int)params->progs.size()) {
			ImGui::Text("%u layers", (unsigned)params->progs[app.sel.prog].layers.size());
		}

		break;
	case SelKind::Layer: draw_layer(app); break;
	case SelKind::Region:
		if (params) draw_region(app, *params);
		break;
	}
}

static void draw_menu(App& app)
{
	if (!ImGui::BeginMenuBar()) return;

	if (ImGui::BeginMenu("File")) {
		if (ImGui::MenuItem("Open...", "Ctrl+O")) {
			do_open(app);
		}

		if (ImGui::MenuItem("Save", "Ctrl+S", false, app.loaded && app.dirty)) save_app(app);
		if (ImGui::MenuItem("Save as...", nullptr, false, app.loaded)) save_app_as(app);
		ImGui::Separator();
		if (ImGui::MenuItem("Extract...", nullptr, false, app.loaded)) {
			do_extract(app);
		}

		int entry = bank_entry(app);
		if (ImGui::MenuItem("Replace wave...", nullptr, false, entry >= -1 && app.doc.kind != FileKind::Mpbp)) replace_from_file(app, 0);
		if (ImGui::MenuItem("Replace parameters...", nullptr, false, entry >= -1)) replace_from_file(app, 1);
		if (ImGui::MenuItem("Replace bank...", nullptr, false, app.doc.kind == FileKind::Mlt && app.sel.entry >= 0)) {
			replace_from_file(app, 2);
		}

		ImGui::Separator();
		if (ImGui::MenuItem("Exit")) PostMessage(app.hwnd, WM_CLOSE, 0, 0);
		ImGui::EndMenu();
	}

	ImGui::EndMenuBar();
}

static void draw_frame(App& app)
{
	if (app.pending_edit != EditNone) {
		int op = app.pending_edit;
		int entry = app.pending_entry;
		int prog = app.pending_prog;
		int layer = app.pending_layer;
		int region = app.pending_region;
		app.pending_edit = EditNone;
		if (op == EditDumpWavs) {
			try {
				dump_wavs(app, entry);
			} catch (const std::exception& ex) {
				set_status(app, ex.what(), true);
			}
		} else try {
			if (op == EditAddRegion) {
				add_region(app.doc, entry, prog, layer);
				app.sel.kind = SelKind::Layer;
				app.sel.layer = layer;
				set_status(app, "Added a region", false);
			} else if (op == EditDeleteRegion) {
				delete_region(app.doc, entry, prog, layer, region);
				if (app.sel.entry == entry && app.sel.prog == prog && app.sel.layer == layer && app.sel.kind == SelKind::Region) {
					if (app.sel.region == region) app.sel.kind = SelKind::Layer;
					else if (app.sel.region > region) app.sel.region--;
				}

				set_status(app, "Deleted a region", false);
			} else if (op == EditAddLayer) {
				add_layer(app.doc, entry, prog);
				const Params* params = params_of(app.doc, entry);
				app.sel.kind = SelKind::Layer;
				app.sel.layer = params ? (int)params->progs[prog].layers.size() - 1 : 0;
				set_status(app, "Added a layer", false);
			} else if (op == EditRemoveLayer) {
				remove_layer(app.doc, entry, prog, layer);
				if (app.sel.entry == entry && app.sel.prog == prog) {
					if (app.sel.layer == layer && (app.sel.kind == SelKind::Layer || app.sel.kind == SelKind::Region)) {
						app.sel.kind = SelKind::Prog;
					} else if (app.sel.layer > layer) app.sel.layer--;
				}

				set_status(app, "Removed a layer", false);
			} else if (op == EditAddProgram) {
				add_program(app.doc, entry);
				const Params* params = params_of(app.doc, entry);
				app.sel.kind = SelKind::Prog;
				app.sel.prog = params ? (int)params->progs.size() - 1 : 0;
				app.sel.layer = 0;
				app.sel.region = 0;
				set_status(app, "Added a program", false);
			}

			app.sel.entry = entry;
			app.sel.prog = op == EditAddProgram ? app.sel.prog : prog;
			app.dirty = true;
			set_title(app);
		} catch (const std::exception& ex) {
			set_status(app, ex.what(), true);
		}
	}

	if (!app.pending_open.empty()) {
		std::string path = app.pending_open;
		app.pending_open.clear();
		open_path(app, path);
	}

	ImGuiIO& io = ImGui::GetIO();
	if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) {
		do_open(app);
	}

	if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S) && !io.KeyShift) save_app(app);
	if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S)) save_app_as(app);

	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(io.DisplaySize);
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
	                         ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
	ImGui::Begin("EMALT", nullptr, flags);
	draw_menu(app);
	float status_h = ImGui::GetFrameHeight();
	float body_h = ImGui::GetContentRegionAvail().y - status_h - ImGui::GetStyle().ItemSpacing.y;
	if (body_h < 1.0f) body_h = 1.0f;
	ImGui::BeginChild("body", ImVec2(0, body_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
	static float tree_w = 360.0f;
	ImGui::BeginChild("tree", ImVec2(tree_w, 0), ImGuiChildFlags_Border);
	draw_tree(app);
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::InvisibleButton("split", ImVec2(6.0f, -1.0f));
	if (ImGui::IsItemActive()) {
		tree_w += io.MouseDelta.x;
	}

	if (tree_w < 220.0f) tree_w = 220.0f;
	if (tree_w > 640.0f) tree_w = 640.0f;
	if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
	ImGui::SameLine();
	ImGui::BeginChild("inspector", ImVec2(0, 0), ImGuiChildFlags_Border);
	draw_inspector(app);
	ImGui::EndChild();
	ImGui::EndChild();
	if (app.status_error) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f), "%s", app.status.c_str());
	else if (!app.status.empty()) ImGui::TextDisabled("%s", app.status.c_str());
	else ImGui::TextDisabled("%s", app.loaded ? kind_name(app.doc.kind) : "No bank");

	if (app.loaded && !app.doc.warnings.empty()) {
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.35f, 1.0f), "%u warnings", (unsigned)app.doc.warnings.size());
	}

	ImGui::End();
}

static void style_ui(float dpi)
{
	ImGui::StyleColorsDark();
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowRounding = 0.0f;
	style.ChildRounding = 4.0f;
	style.FrameRounding = 3.0f;
	style.GrabRounding = 3.0f;
	style.PopupRounding = 3.0f;
	style.ScrollbarRounding = 4.0f;
	style.WindowPadding = ImVec2(12.0f, 10.0f);
	style.FramePadding = ImVec2(8.0f, 5.0f);
	style.ItemSpacing = ImVec2(8.0f, 6.0f);
	ImVec4* c = style.Colors;
	c[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.11f, 0.13f, 1.0f);
	c[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.13f, 0.15f, 1.0f);
	c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.13f, 0.15f, 1.0f);
	c[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.20f, 0.23f, 1.0f);
	c[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.28f, 0.32f, 1.0f);
	c[ImGuiCol_FrameBgActive] = ImVec4(0.28f, 0.34f, 0.36f, 1.0f);
	c[ImGuiCol_Header] = ImVec4(0.16f, 0.36f, 0.33f, 1.0f);
	c[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.46f, 0.41f, 1.0f);
	c[ImGuiCol_HeaderActive] = ImVec4(0.18f, 0.55f, 0.47f, 1.0f);
	c[ImGuiCol_Button] = ImVec4(0.18f, 0.40f, 0.37f, 1.0f);
	c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.52f, 0.46f, 1.0f);
	c[ImGuiCol_ButtonActive] = ImVec4(0.16f, 0.62f, 0.52f, 1.0f);
	c[ImGuiCol_SliderGrab] = ImVec4(0.36f, 0.76f, 0.64f, 1.0f);
	c[ImGuiCol_SliderGrabActive] = ImVec4(0.52f, 0.90f, 0.76f, 1.0f);
	c[ImGuiCol_CheckMark] = ImVec4(0.46f, 0.90f, 0.74f, 1.0f);
	c[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.09f, 0.10f, 1.0f);
	c[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.13f, 0.13f, 1.0f);
	c[ImGuiCol_MenuBarBg] = ImVec4(0.11f, 0.12f, 0.14f, 1.0f);
	c[ImGuiCol_Separator] = ImVec4(0.24f, 0.27f, 0.30f, 1.0f);
	c[ImGuiCol_ScrollbarBg] = ImVec4(0.10f, 0.11f, 0.12f, 1.0f);
	c[ImGuiCol_ScrollbarGrab] = ImVec4(0.28f, 0.32f, 0.34f, 1.0f);
	style.ScaleAllSizes(dpi);
}

int gui_main(const char* open_at)
{
	ImGui_ImplWin32_EnableDpiAwareness();
	WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, wnd_proc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"EMALT", nullptr};
	RegisterClassExW(&wc);
	HWND hwnd = CreateWindowW(wc.lpszClassName, L"EMALT", WS_OVERLAPPEDWINDOW, 80, 60, 1440, 900, nullptr, nullptr, wc.hInstance, nullptr);
	if (!create_device(hwnd)) {
		cleanup_device();
		UnregisterClassW(wc.lpszClassName, wc.hInstance);
		MessageBoxA(nullptr, "DirectX 11 did not start.", "EMALT", MB_ICONERROR);
		return 1;
	}

	ShowWindow(hwnd, SW_SHOWDEFAULT);
	UpdateWindow(hwnd);
	DragAcceptFiles(hwnd, TRUE);
	audio_init();
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.IniFilename = nullptr;
	float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
	if (dpi < 1.0f) dpi = 1.0f;
	style_ui(dpi);
	if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f * dpi)) io.Fonts->AddFontDefault();
	App app;
	app.hwnd = hwnd;
	app.mono = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 15.0f * dpi);
	g_app = &app;

	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(g_device, g_context);
	if (open_at && open_at[0]) open_path(app, open_at);

	bool done = false;
	while (!done) {
		MSG msg;
		while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			if (msg.message == WM_QUIT) {
				done = true;
			}
		}

		if (done) break;

		if (g_occluded && g_swap->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
			Sleep(10);
			continue;
		}

		g_occluded = false;
		if (g_resize_w != 0 && g_resize_h != 0) {
			cleanup_target();
			g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
			g_resize_w = g_resize_h = 0;
			create_target();
		}

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		draw_frame(app);
		ImGui::Render();
		const float clear[4] = {0.10f, 0.11f, 0.13f, 1.0f};
		g_context->OMSetRenderTargets(1, &g_target, nullptr);
		g_context->ClearRenderTargetView(g_target, clear);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		HRESULT present = g_swap->Present(1, 0);
		g_occluded = (present == DXGI_STATUS_OCCLUDED);
	}

	audio_shutdown();
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	g_app = nullptr;
	cleanup_device();
	if (IsWindow(hwnd)) DestroyWindow(hwnd);
	UnregisterClassW(wc.lpszClassName, wc.hInstance);
	CoUninitialize();
	return 0;
}

static bool create_device(HWND hwnd)
{
	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferCount = 2;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hwnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	D3D_FEATURE_LEVEL level;
	const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
	HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &level, &g_context);
	if (result == DXGI_ERROR_UNSUPPORTED) {
		result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &level, &g_context);
	}

	if (result != S_OK) return false;

	create_target();
	return true;
}

static void cleanup_device()
{
	cleanup_target();
	if (g_swap) {
		g_swap->Release();
		g_swap = nullptr;
	}

	if (g_context) {
		g_context->Release();
		g_context = nullptr;
	}

	if (g_device) {
		g_device->Release();
		g_device = nullptr;
	}
}

static void create_target()
{
	ID3D11Texture2D* back = nullptr;
	g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
	g_device->CreateRenderTargetView(back, nullptr, &g_target);
	back->Release();
}

static void cleanup_target()
{
	if (g_target) {
		g_target->Release();
		g_target = nullptr;
	}
}

static LRESULT WINAPI wnd_proc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;

	switch (msg) {
	case WM_SIZE:
		if (wParam == SIZE_MINIMIZED) return 0;

		g_resize_w = LOWORD(lParam);
		g_resize_h = HIWORD(lParam);
		return 0;
	case WM_SYSCOMMAND:
		if ((wParam & 0xfff0) == SC_KEYMENU) return 0;

		break;
	case WM_DROPFILES: {
		HDROP drop = (HDROP)wParam;
		char path[MAX_PATH];
		if (g_app && DragQueryFileA(drop, 0, path, MAX_PATH)) g_app->pending_open = path;
		DragFinish(drop);
		return 0;
	}
	case WM_CLOSE:
		if (g_app && g_app->dirty) {
			int answer = MessageBoxA(hWnd, "Save changes to this bank?", "EMALT", MB_YESNOCANCEL | MB_ICONWARNING);
			if (answer == IDCANCEL) return 0;

			if (answer == IDYES && !save_app(*g_app)) return 0;
		}

		DestroyWindow(hWnd);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}

	return DefWindowProcW(hWnd, msg, wParam, lParam);
}
