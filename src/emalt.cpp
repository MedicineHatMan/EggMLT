#include "emalt.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

constexpr uint32_t MAGIC = 0x67636178u;
constexpr uint32_t T_MPB = 0x4D504220u;
constexpr uint32_t T_MPBW = 0x4D504257u;
constexpr uint32_t T_MPBP = 0x4D504250u;
constexpr uint32_t T_MSB = 0x4D534220u;
constexpr uint32_t T_MLT = 0x4D4C5420u;
constexpr uint32_t T_MLTM = 0x4D4C544Du;

constexpr uint32_t SAMPLE_BYTES = 0x50;
constexpr uint32_t REGION_BYTES = 0x40;
constexpr uint32_t LAYER_BYTES = 0x10;
constexpr uint32_t CURVE_BYTES = 0x80;
constexpr uint32_t CHUNK_BYTES = 0x10;

[[noreturn]] void fail(const std::string& m)
{
	throw std::runtime_error(m);
}

void need(bool cond, const std::string& m)
{
	if (!cond) fail(m);
}

uint32_t ru32(const std::vector<uint8_t>& b, uint32_t o)
{
	need(static_cast<uint64_t>(o) + 4 <= b.size(), "read past the end of the file");
	return (uint32_t(b[o]) << 24) | (uint32_t(b[o + 1]) << 16) | (uint32_t(b[o + 2]) << 8) | uint32_t(b[o + 3]);
}

uint16_t ru16(const std::vector<uint8_t>& b, uint32_t o)
{
	need(static_cast<uint64_t>(o) + 2 <= b.size(), "read past the end of the file");
	return uint16_t((uint32_t(b[o]) << 8) | uint32_t(b[o + 1]));
}

uint8_t ru8(const std::vector<uint8_t>& b, uint32_t o)
{
	need(o < b.size(), "read past the end of the file");
	return b[o];
}

void wu32(std::vector<uint8_t>& b, uint32_t o, uint32_t v)
{
	need(static_cast<uint64_t>(o) + 4 <= b.size(), "write past the end of the file");
	b[o] = uint8_t(v >> 24);
	b[o + 1] = uint8_t(v >> 16);
	b[o + 2] = uint8_t(v >> 8);
	b[o + 3] = uint8_t(v);
}

void wu16(std::vector<uint8_t>& b, uint32_t o, uint16_t v)
{
	need(static_cast<uint64_t>(o) + 2 <= b.size(), "write past the end of the file");
	b[o] = uint8_t(v >> 8);
	b[o + 1] = uint8_t(v);
}

void wu8(std::vector<uint8_t>& b, uint32_t o, uint8_t v)
{
	need(o < b.size(), "write past the end of the file");
	b[o] = v;
}

bool inside(uint32_t rel, uint32_t n, uint32_t size)
{
	return n <= size && rel <= size - n;
}

std::string hex8(uint32_t v)
{
	char buf[16];
	std::snprintf(buf, sizeof buf, "0x%08X", v);
	return buf;
}

std::string fourcc(uint32_t t)
{
	char s[5];
	for (int i = 0; i < 4; i++) {
		unsigned char c = uint8_t(t >> (24 - 8 * i));
		s[i] = (c >= 32 && c < 127) ? char(c) : '.';
	}

	s[4] = 0;
	return s;
}

const char* type_label(uint32_t t)
{
	if (t == T_MPB) return "MPB";

	if (t == T_MPBW) return "MPBW";

	if (t == T_MPBP) return "MPBP";

	if (t == T_MSB) return "MSB";

	if (t == T_MLT) return "MLT";

	if (t == T_MLTM) return "MLTM";

	return nullptr;
}

std::string chunk_label(uint32_t t)
{
	if (const char* n = type_label(t)) return n;

	return fourcc(t);
}

uint32_t chunk_type_at(const std::vector<uint8_t>& b, uint32_t pos)
{
	if (static_cast<uint64_t>(pos) + CHUNK_BYTES > b.size()) return 0;

	if (ru32(b, pos) != MAGIC) return 0;

	return ru32(b, pos + 4);
}

/* Bytes occupied by a chunk that starts at pos, clamped to end_cap and the file. */
uint32_t chunk_span(const std::vector<uint8_t>& b, uint32_t pos, uint32_t end_cap, std::vector<std::string>& warn, const char* what)
{
	if (pos >= b.size() || pos >= end_cap || static_cast<uint64_t>(pos) + CHUNK_BYTES > b.size()) return 0;

	uint32_t cap = end_cap < b.size() ? end_cap : uint32_t(b.size());
	if (pos > cap || cap - pos < CHUNK_BYTES) return 0;

	if (ru32(b, pos) != MAGIC) return cap - pos;

	uint32_t sz = ru32(b, pos + 12);
	uint64_t declared = uint64_t(pos) + CHUNK_BYTES + sz;
	if (declared > cap) {
		warn.push_back(std::string(what) + " size runs past the following data");
		return cap - pos;
	}

	return uint32_t(declared - pos);
}

const char* ax_format(uint8_t f)
{
	if (f == 0) return "adpcm";

	if (f == 10) return "pcm16";

	if (f == 25) return "pcm8";

	return nullptr;
}

/* ---- parameter block ----------------------------------------------------------------------- */

void parse_params(const std::vector<uint8_t>& b, Params& p, std::vector<std::string>& warn)
{
	p.samples.clear();
	p.curves.clear();
	p.progs.clear();
	if (p.size < 12) {
		warn.push_back("parameter block is shorter than the 12-byte MPB head");
		return;
	}

	const uint32_t base = p.base;
	const uint32_t size = p.size;
	const uint32_t sw = ru32(b, base);
	const uint32_t vw = ru32(b, base + 4);
	const uint32_t pw = ru32(b, base + 8);
	const uint32_t soff = sw & 0xFFFFFFu;
	const uint32_t voff = vw & 0xFFFFFFu;
	const uint32_t poff = pw & 0xFFFFFFu;

	auto abs_ok = [&](uint32_t rel, uint32_t n) {
		return inside(rel, n, size) && static_cast<uint64_t>(base) + rel + n <= b.size();
	};

	uint32_t pdecl = pw >> 24;
	if (pdecl != 0 && !abs_ok(poff, pdecl * 4)) {
		uint32_t room = (poff < size) ? (size - poff) / 4 : 0;
		warn.push_back("program count does not fit in the parameter block");
		pdecl = room;
	}

	uint32_t max_sample = 0;
	uint32_t max_curve = 0;
	bool any_sample = false;
	bool any_curve = false;
	int bad_region = 0;

	struct LayerOff {
		uint32_t off;
	};

	struct ProgOff {
		std::vector<LayerOff> layers;
		std::vector<uint32_t> regions;
	};

	std::vector<ProgOff> built;

	for (uint32_t i = 0; i < pdecl; i++) {
		ProgOff prog;
		uint32_t word = ru32(b, base + poff + i * 4);
		uint32_t ldecl = word >> 24;
		uint32_t loff = word & 0xFFFFFFu;
		if (ldecl != 0 && !abs_ok(loff, ldecl * LAYER_BYTES)) {
			uint32_t room = (loff < size) ? (size - loff) / LAYER_BYTES : 0;
			warn.push_back("a program's layer list does not fit");
			ldecl = std::min(ldecl, room);
		}

		for (uint32_t L = 0; L < ldecl; L++) {
			uint32_t layer = loff + L * LAYER_BYTES;
			if (!abs_ok(layer, LAYER_BYTES)) break;

			LayerOff layer_off;
			layer_off.off = base + layer;
			prog.layers.push_back(layer_off);
			uint32_t rw = ru32(b, base + layer);
			uint32_t rdecl = rw >> 24;
			uint32_t roff = rw & 0xFFFFFFu;
			if (rdecl != 0 && !abs_ok(roff, rdecl * REGION_BYTES)) {
				uint32_t room = (roff < size) ? (size - roff) / REGION_BYTES : 0;
				warn.push_back("a layer's region list does not fit");
				rdecl = std::min(rdecl, room);
			}

			for (uint32_t R = 0; R < rdecl; R++) {
				uint32_t region = roff + R * REGION_BYTES;
				if (!abs_ok(region, REGION_BYTES)) {
					bad_region++;
					break;
				}

				prog.regions.push_back(base + region);
				uint32_t sample = ru32(b, base + region);
				uint8_t curve = b[base + region + 5];
				if (!any_sample || sample > max_sample) {
					max_sample = sample;
					any_sample = true;
				}

				if (!any_curve || curve > max_curve) {
					max_curve = curve;
					any_curve = true;
				}
			}
		}

		built.push_back(std::move(prog));
	}

	if (bad_region) warn.push_back("a region record runs past the parameter block");

	std::vector<uint32_t> marks;
	auto mark = [&](uint32_t rel) {
		if (rel != 0 && rel < size) marks.push_back(rel);
	};

	mark(soff);
	mark(voff);
	mark(poff);
	for (const auto& prog : built) {
		for (const auto& layer : prog.layers) {
			mark(layer.off - base);
			uint32_t rw = ru32(b, layer.off);
			mark(rw & 0xFFFFFFu);
		}
	}

	auto infer = [&](uint32_t declared, uint32_t off, uint32_t stride, uint32_t need, const char* what) -> uint32_t {
		uint32_t count = 0;
		if (declared != 0) count = declared;
		else if (off >= 12 || need > 0) {
			uint32_t next = size;
			for (uint32_t m : marks) {
				if (m > off && m < next) next = m;
			}

			uint32_t span = (off < next) ? (next - off) / stride : 0;
			count = span < need ? need : span;
		}

		if (off > size) {
			warn.push_back(std::string(what) + " offset is past the parameter block");
			return 0;
		}

		uint32_t room = (size - off) / stride;
		if (count > room) {
			warn.push_back(std::string(what) + " table does not fit");
			count = room;
		}

		return count;
	};

	uint32_t scount = infer(sw >> 24, soff, SAMPLE_BYTES, any_sample ? max_sample + 1 : 0, "sample");
	uint32_t vcount = infer(vw >> 24, voff, CURVE_BYTES, any_curve ? max_curve + 1 : 0, "velocity curve");

	if (scount && abs_ok(soff, scount * SAMPLE_BYTES)) {
		for (uint32_t i = 0; i < scount; i++) {
			Sample s;
			s.off = base + soff + i * SAMPLE_BYTES;
			p.samples.push_back(s);
		}
	}

	if (vcount && abs_ok(voff, vcount * CURVE_BYTES)) {
		for (uint32_t i = 0; i < vcount; i++) {
			Curve c;
			c.off = base + voff + i * CURVE_BYTES;
			p.curves.push_back(c);
		}
	}

	int past_s = 0;
	int past_v = 0;
	for (const auto& prog : built) {
		Program out;
		for (const auto& layer : prog.layers) {
			Layer L;
			L.off = layer.off;
			uint32_t rw = ru32(b, layer.off);
			uint32_t rdecl = rw >> 24;
			uint32_t roff = rw & 0xFFFFFFu;
			uint32_t room = (roff < size) ? (size - roff) / REGION_BYTES : 0;
			if (rdecl > room) rdecl = room;
			for (uint32_t R = 0; R < rdecl; R++) {
				uint32_t region = roff + R * REGION_BYTES;
				if (!abs_ok(region, REGION_BYTES)) break;

				Region reg;
				reg.off = base + region;
				L.regions.push_back(reg);
				uint32_t sample = ru32(b, base + region);
				uint8_t curve = b[base + region + 5];
				if (sample >= p.samples.size()) past_s++;
				if (curve >= p.curves.size()) past_v++;
			}

			out.layers.push_back(std::move(L));
		}

		p.progs.push_back(std::move(out));
	}

	if (past_s) warn.push_back(std::to_string(past_s) + " region(s) name a sample past the sample table");
	if (past_v) warn.push_back(std::to_string(past_v) + " region(s) name a velocity curve past the curve table");
}

/* ---- chunks -------------------------------------------------------------------------------- */

Mpb parse_mpb(const std::vector<uint8_t>& b, uint32_t pos, uint32_t end_cap, std::vector<std::string>& warn)
{
	Mpb m;
	m.head = pos;
	m.span = chunk_span(b, pos, end_cap, warn, "MPB");
	if (m.span < CHUNK_BYTES || chunk_type_at(b, pos) != T_MPB) {
		warn.push_back("MPB chunk is missing");
		m.span = 0;
		return m;
	}

	uint32_t cursor = pos + CHUNK_BYTES;
	uint32_t end = pos + m.span;
	if (static_cast<uint64_t>(cursor) + CHUNK_BYTES <= end && chunk_type_at(b, cursor) == T_MPBW) {
		m.has_wave = true;
		m.wave = cursor;
		uint32_t wsz = ru32(b, cursor + 12);
		uint64_t wend = uint64_t(cursor) + CHUNK_BYTES + wsz;
		if (wend > end) {
			warn.push_back("MPBW size runs past the MPB");
			cursor = end;
		} else cursor = uint32_t(wend);
	} else warn.push_back("MPB has no MPBW wave chunk where gcaxSndLoadMpb expects one");

	if (static_cast<uint64_t>(cursor) + CHUNK_BYTES <= end && chunk_type_at(b, cursor) == T_MPBP) {
		m.has_param = true;
		m.param = cursor;
		uint32_t psz = ru32(b, cursor + 12);
		uint64_t pend = uint64_t(cursor) + CHUNK_BYTES + psz;
		uint32_t payload = cursor + CHUNK_BYTES;
		uint32_t plen = psz;
		if (pend > end) {
			warn.push_back("MPBP size runs past the MPB");
			plen = end > payload ? end - payload : 0;
		}

		if (static_cast<uint64_t>(payload) + plen <= b.size()) {
			m.params.base = payload;
			m.params.size = plen;
			parse_params(b, m.params, warn);
		}
	} else warn.push_back("MPB has no MPBP parameter chunk where gcaxSndLoadMpb expects one");

	return m;
}

Msb parse_msb(const std::vector<uint8_t>& b, uint32_t pos, uint32_t end_cap, std::vector<std::string>& warn)
{
	Msb m;
	m.head = pos;
	m.span = chunk_span(b, pos, end_cap, warn, "MSB");
	if (chunk_type_at(b, pos) != T_MSB) warn.push_back("MSB chunk is missing");
	return m;
}

void parse_entry_body(Document& doc, Entry& e, uint32_t end_cap)
{
	uint32_t kind = chunk_type_at(doc.bytes, e.bank_pos);
	uint32_t nibble = (ru32(doc.bytes, e.off) >> 24) & 0xF;
	if (kind == T_MPB) {
		e.kind = EntryKind::Mpb;
		e.mpb = parse_mpb(doc.bytes, e.bank_pos, end_cap, doc.warnings);
		e.bank_len = e.mpb->span;
		if (nibble != 1) {
			doc.warnings.push_back("entry " + std::to_string(e.index) +
			                       " type nibble is " + std::to_string(nibble) +
			                       " and the chunk is MPB; gcaxLoaderLoad accepts type 1");
		}
	} else if (kind == T_MSB) {
		e.kind = EntryKind::Msb;
		e.msb = parse_msb(doc.bytes, e.bank_pos, end_cap, doc.warnings);
		e.bank_len = e.msb->span;
		if (nibble != 4) {
			doc.warnings.push_back("entry " + std::to_string(e.index) +
			                       " type nibble is " + std::to_string(nibble) +
			                       " and the chunk is MSB; gcaxLoaderLoad accepts type 4");
		}
	} else {
		e.kind = EntryKind::Other;
		e.bank_len = chunk_span(doc.bytes, e.bank_pos, end_cap, doc.warnings, "bank");
		doc.warnings.push_back("entry " + std::to_string(e.index) + " does not point at an MPB or MSB chunk");
	}
}

void parse_mlt(Document& doc)
{
	need(doc.bytes.size() >= 0x20, doc.name + " is too small to be an MLT");
	need(ru32(doc.bytes, 0) == MAGIC && ru32(doc.bytes, 4) == T_MLT, doc.name + " is not an MLT");
	need(ru32(doc.bytes, 0x10) == MAGIC && ru32(doc.bytes, 0x14) == T_MLTM, doc.name + " has no MLTM table");
	doc.kind = FileKind::Mlt;
	uint32_t n = ru32(doc.bytes, 0x1C) >> 4;
	uint64_t need_bytes = 0x20ull + uint64_t(n) * 16ull;
	if (need_bytes > doc.bytes.size()) {
		doc.warnings.push_back("MLTM size names entries past the end of the file");
		n = uint32_t((doc.bytes.size() - 0x20) / 16);
	}

	doc.entries.resize(n);
	for (uint32_t i = 0; i < n; i++) {
		Entry& e = doc.entries[i];
		e.index = i;
		e.off = 0x20 + i * 16;
		uint32_t bank_word = ru32(doc.bytes, e.off + 4);
		if (bank_word == 0xFFFFFFFFu) {
			e.kind = EntryKind::Hang;
			doc.warnings.push_back("entry " + std::to_string(i) +
			                       " bank word is 0xFFFFFFFF; gcaxLoaderLoad stays on this entry");
			continue;
		}

		uint32_t rel = ru32(doc.bytes, e.off + 8);
		uint64_t pos = 0x20ull + rel;
		if (pos >= doc.bytes.size()) {
			e.kind = EntryKind::Other;
			doc.warnings.push_back("entry " + std::to_string(i) + " offset is past the end of the file");
			continue;
		}

		e.has_pos = true;
		e.bank_pos = uint32_t(pos);
	}

	for (Entry& e : doc.entries) {
		if (!e.has_pos) continue;

		uint32_t cap = uint32_t(doc.bytes.size());
		for (const Entry& o : doc.entries) {
			if (o.has_pos && o.bank_pos > e.bank_pos && o.bank_pos < cap) cap = o.bank_pos;
		}

		try {
			parse_entry_body(doc, e, cap);
		} catch (const std::exception& ex) {
			doc.warnings.push_back(std::string("entry ") + std::to_string(e.index) + ": " + ex.what());
			e.kind = EntryKind::Other;
		}
	}

	doc.ok = true;
}

void parse_root_chunk(Document& doc)
{
	need(doc.bytes.size() >= CHUNK_BYTES, doc.name + " is too small");
	need(ru32(doc.bytes, 0) == MAGIC, doc.name + " does not start with the gcax magic");
	uint32_t t = ru32(doc.bytes, 4);
	uint32_t end = uint32_t(doc.bytes.size());
	if (t == T_MLT) {
		parse_mlt(doc);
		return;
	}

	if (t == T_MPB) {
		doc.kind = FileKind::Mpb;
		doc.mpb = parse_mpb(doc.bytes, 0, end, doc.warnings);
		doc.ok = true;
		return;
	}

	if (t == T_MPBP) {
		doc.kind = FileKind::Mpbp;
		uint32_t sz = ru32(doc.bytes, 12);
		uint32_t plen = sz;
		if (uint64_t(CHUNK_BYTES) + sz > doc.bytes.size()) {
			doc.warnings.push_back("MPBP size runs past the end of the file");
			plen = doc.bytes.size() > CHUNK_BYTES ? uint32_t(doc.bytes.size() - CHUNK_BYTES) : 0;
		}

		Params p;
		p.base = CHUNK_BYTES;
		p.size = plen;
		parse_params(doc.bytes, p, doc.warnings);
		doc.params = p;
		doc.ok = true;
		return;
	}

	if (t == T_MPBW) {
		doc.kind = FileKind::Mpbw;
		doc.ok = true;
		return;
	}

	if (t == T_MSB) {
		doc.kind = FileKind::Msb;
		doc.msb = parse_msb(doc.bytes, 0, end, doc.warnings);
		doc.ok = true;
		return;
	}

	fail(doc.name + " is a gcax chunk of type " + chunk_label(t) + ", which EMALT does not edit");
}

/* ---- describe ------------------------------------------------------------------------------- */

uint32_t entry_slot(const Document& doc, const Entry& e)
{
	return (ru32(doc.bytes, e.off + 4) >> 24) & 0xF;
}

uint32_t entry_type_nibble(const Document& doc, const Entry& e)
{
	return (ru32(doc.bytes, e.off) >> 24) & 0xF;
}

const char* nibble_name(uint32_t n)
{
	if (n == 1) return "MPB";

	if (n == 4) return "MSB";

	return nullptr;
}

void dump_params(std::ostringstream& o, const Document& doc, const Params& p, const std::string& pad, bool full)
{
	o << pad << "samples " << p.samples.size();
	if (p.size >= 12) o << "  table " << hex8(ru32(doc.bytes, p.base));
	o << "\n" << pad << "velocity curves " << p.curves.size();
	if (p.size >= 12) o << "  table " << hex8(ru32(doc.bytes, p.base + 4));
	o << "\n" << pad << "programs " << p.progs.size();
	if (p.size >= 12) o << "  table " << hex8(ru32(doc.bytes, p.base + 8));
	o << "\n";
	if (full) {
		for (uint32_t i = 0; i < p.samples.size(); i++) {
			uint32_t s = p.samples[i].off;
			uint8_t fmt = ru8(doc.bytes, s + 0x4A);
			o << pad << "sample " << i << "  length " << ru32(doc.bytes, s) << "  nibbles " << ru32(doc.bytes, s + 4)
			  << "  rate " << ru32(doc.bytes, s + 8) << "  loop " << ru16(doc.bytes, s + 0x0C) << "  dsp_format "
			  << ru16(doc.bytes, s + 0x0E) << "  format " << uint32_t(fmt);
			if (const char* n = ax_format(fmt)) {
				o << " " << n;
			}

			o << "  offset " << hex8(ru32(doc.bytes, s + 0x4C)) << "\n";
			o << pad << "  loop_adr " << hex8(ru32(doc.bytes, s + 0x10)) << "  end_adr " << hex8(ru32(doc.bytes, s + 0x14))
			  << "  cur_adr " << hex8(ru32(doc.bytes, s + 0x18)) << "\n";
			o << pad << "  coef";
			for (int c = 0; c < 16; c++) o << " " << ru16(doc.bytes, s + 0x1C + uint32_t(c) * 2);
			o << "\n";
		}

		for (uint32_t i = 0; i < p.curves.size(); i++) {
			o << pad << "vel " << i;
			for (uint32_t k = 0; k < CURVE_BYTES; k++) {
				if (k % 16 == 0) {
					o << "\n" << pad << "  ";
				}

				char buf[8];
				std::snprintf(buf, sizeof buf, "%02X", doc.bytes[p.curves[i].off + k]);
				o << buf;
			}

			o << "\n";
		}
	} else {
		for (uint32_t i = 0; i < p.samples.size(); i++) {
			uint32_t s = p.samples[i].off;
			uint8_t fmt = ru8(doc.bytes, s + 0x4A);
			o << pad << "sample " << i << "  length " << ru32(doc.bytes, s) << "  format " << uint32_t(fmt);
			if (const char* n = ax_format(fmt)) {
				o << " " << n;
			}

			o << "  loop " << ru16(doc.bytes, s + 0x0C) << "  offset " << hex8(ru32(doc.bytes, s + 0x4C)) << "\n";
		}
	}

	for (uint32_t pi = 0; pi < p.progs.size(); pi++) {
		const Program& prog = p.progs[pi];
		o << pad << "prog " << pi << "  layers " << prog.layers.size() << "\n";
		for (uint32_t li = 0; li < prog.layers.size(); li++) {
			const Layer& layer = prog.layers[li];
			o << pad << "  layer " << li << "  bend " << uint32_t(ru8(doc.bytes, layer.off + 4)) << "/"
			  << uint32_t(ru8(doc.bytes, layer.off + 5)) << "  regions " << layer.regions.size() << "\n";
			if (full) {
				o << pad << "    unk ";
				for (uint32_t k = 0; k < 0x0A; k++) {
					char buf[8];
					std::snprintf(buf, sizeof buf, "%02X", doc.bytes[layer.off + 6 + k]);
					o << buf;
				}

				o << "\n";
			}

			for (uint32_t ri = 0; ri < layer.regions.size(); ri++) {
				uint32_t r = layer.regions[ri].off;
				o << pad << "    region " << ri << "  sample " << ru32(doc.bytes, r) << "  key "
				  << uint32_t(ru8(doc.bytes, r + 8)) << "-" << uint32_t(ru8(doc.bytes, r + 9)) << "  vel "
				  << uint32_t(ru8(doc.bytes, r + 6)) << "-" << uint32_t(ru8(doc.bytes, r + 7)) << "  curve "
				  << uint32_t(ru8(doc.bytes, r + 5)) << "  root " << uint32_t(ru8(doc.bytes, r + 0x0C)) << "  fine "
				  << uint32_t(ru8(doc.bytes, r + 0x0D)) << "  level " << uint32_t(ru8(doc.bytes, r + 0x0A)) << "  pan "
				  << uint32_t(ru8(doc.bytes, r + 0x0B)) << "  env " << uint32_t(ru8(doc.bytes, r + 0x10)) << "/"
				  << uint32_t(ru8(doc.bytes, r + 0x11)) << "/" << uint32_t(ru8(doc.bytes, r + 0x12)) << "/"
				  << uint32_t(ru8(doc.bytes, r + 0x14)) << "  sus " << uint32_t(ru8(doc.bytes, r + 0x13)) << "\n";
				if (full) {
					o << pad << "      unk ";
					for (uint32_t k = 0; k < 0x29; k++) {
						char buf[8];
						std::snprintf(buf, sizeof buf, "%02X", doc.bytes[r + 0x17 + k]);
						o << buf;
					}

					o << "\n";
				}
			}
		}
	}
}

void dump_mpb(std::ostringstream& o, const Document& doc, const Mpb& m, const std::string& pad, bool full)
{
	o << pad << "MPB unk " << hex8(ru32(doc.bytes, m.head + 8)) << "  size " << hex8(ru32(doc.bytes, m.head + 12)) << "\n";
	if (m.has_wave) {
		o << pad << "MPBW unk " << hex8(ru32(doc.bytes, m.wave + 8)) << "  size " << hex8(ru32(doc.bytes, m.wave + 12))
		  << "\n";
	}

	if (m.has_param) {
		o << pad << "MPBP unk " << hex8(ru32(doc.bytes, m.param + 8)) << "  size " << hex8(ru32(doc.bytes, m.param + 12))
		  << "\n";
		dump_params(o, doc, m.params, pad, full);
	}
}

/* ---- paths ---------------------------------------------------------------------------------- */

enum class Show { Dec, Hex };
enum class LocKind { U8, U16, U32, Hex, Type, Bank };

struct Loc {
	uint32_t off = 0;
	LocKind kind = LocKind::U8;
	Show show = Show::Dec;
	uint32_t size = 0;
};

struct Tok {
	std::string name;
	std::vector<uint32_t> idx;
};

uint32_t parse_index(const std::string& s)
{
	need(!s.empty(), "empty index");
	size_t used = 0;
	unsigned long v = std::stoul(s, &used, 0);
	need(used == s.size(), "bad index '" + s + "'");
	return uint32_t(v);
}

std::vector<Tok> tokenize(const std::string& path)
{
	std::vector<Tok> out;
	Tok cur;
	bool in = false;
	for (size_t i = 0; i < path.size();) {
		char c = path[i];
		if (c == '.') {
			need(in, "empty path component in '" + path + "'");
			out.push_back(cur);
			cur = Tok();
			in = false;
			i++;
			continue;
		}

		if (c == '[') {
			need(in, "index without a name in '" + path + "'");
			size_t j = path.find(']', i + 1);
			need(j != std::string::npos, "unclosed index in '" + path + "'");
			cur.idx.push_back(parse_index(path.substr(i + 1, j - i - 1)));
			i = j + 1;
			continue;
		}

		need(std::isalnum(static_cast<unsigned char>(c)) || c == '_', std::string("bad character in path '") + path + "'");
		need(cur.idx.empty(), "name continues after an index in '" + path + "'");
		if (!in) {
			cur = Tok();
			in = true;
		}

		cur.name.push_back(c);
		i++;
	}

	need(in, "empty path");
	out.push_back(cur);
	need(!out.empty(), "empty path");
	return out;
}

enum class At { Root, Entry, Params, Prog, Layer, Region, Sample, Chunk };

struct State {
	At at = At::Root;
	const Entry* entry = nullptr;
	const Mpb* mpb = nullptr;
	const Msb* msb = nullptr;
	const Params* params = nullptr;
	uint32_t chunk = 0;
	size_t prog = 0;
	size_t layer = 0;
	size_t sample = 0;
};

const Params* only_params(const Document& doc)
{
	const Params* hit = nullptr;
	auto take = [&](const Params* p) {
		need(hit == nullptr, "name the bank with entry[N] or slot[N]");
		hit = p;
	};

	if (doc.params) take(&*doc.params);
	if (doc.mpb && doc.mpb->has_param) take(&doc.mpb->params);
	for (const Entry& e : doc.entries) {
		if (e.mpb && e.mpb->has_param) take(&e.mpb->params);
	}

	need(hit != nullptr, "this file has no parameter block");
	return hit;
}

const Mpb* only_mpb(const Document& doc)
{
	const Mpb* hit = nullptr;
	auto take = [&](const Mpb* m) {
		need(hit == nullptr, "name the bank with entry[N] or slot[N]");
		hit = m;
	};

	if (doc.mpb) take(&*doc.mpb);
	for (const Entry& e : doc.entries) {
		if (e.mpb) take(&*e.mpb);
	}

	need(hit != nullptr, "this file has no MPB");
	return hit;
}

const Msb* only_msb(const Document& doc)
{
	const Msb* hit = nullptr;
	auto take = [&](const Msb* m) {
		need(hit == nullptr, "name the bank with entry[N] or slot[N]");
		hit = m;
	};

	if (doc.msb) take(&*doc.msb);
	for (const Entry& e : doc.entries) {
		if (e.msb) take(&*e.msb);
	}

	need(hit != nullptr, "this file has no MSB");
	return hit;
}

const Entry* entry_by_slot(const Document& doc, uint32_t slot)
{
	const Entry* hit = nullptr;
	for (const Entry& e : doc.entries) {
		if (e.kind == EntryKind::Hang) continue;

		if (entry_slot(doc, e) == slot) {
			need(hit == nullptr, "more than one entry uses slot " + std::to_string(slot));
			hit = &e;
		}
	}

	need(hit != nullptr, "no entry uses slot " + std::to_string(slot));
	return hit;
}

uint32_t one_index(const Tok& t)
{
	need(t.idx.size() == 1, t.name + " takes one index");
	return t.idx[0];
}

void descend(const Document& doc, State& st, const Tok& t)
{
	auto need_idx0 = [&]() { need(t.idx.empty(), t.name + " takes no index"); };
	if (t.name == "entry") {
		need(st.at == At::Root, "entry is only valid at the start of a path");
		need(doc.kind == FileKind::Mlt, "this file has no MLT entries");
		uint32_t i = one_index(t);
		need(i < doc.entries.size(), "entry index is out of range");
		st.at = At::Entry;
		st.entry = &doc.entries[i];
		return;
	}

	if (t.name == "slot") {
		need(st.at == At::Root, "slot is only valid at the start of a path");
		st.at = At::Entry;
		st.entry = entry_by_slot(doc, one_index(t));
		return;
	}

	if (t.name == "prog") {
		const Params* p = nullptr;
		if (st.at == At::Root) p = only_params(doc);
		else if (st.at == At::Entry) {
			need(st.entry->mpb && st.entry->mpb->has_param, "entry has no parameter block");
			p = &st.entry->mpb->params;
		} else if (st.at == At::Params) p = st.params;
		else fail("prog is not inside " + t.name);

		uint32_t i = one_index(t);
		need(i < p->progs.size(), "program index is out of range");
		st.at = At::Prog;
		st.params = p;
		st.prog = i;
		return;
	}

	if (t.name == "layer") {
		need(st.at == At::Prog, "layer follows prog[N]");
		uint32_t i = one_index(t);
		need(i < st.params->progs[st.prog].layers.size(), "layer index is out of range");
		st.at = At::Layer;
		st.layer = i;
		return;
	}

	if (t.name == "region") {
		need(st.at == At::Layer, "region follows layer[N]");
		uint32_t i = one_index(t);
		const Layer& layer = st.params->progs[st.prog].layers[st.layer];
		need(i < layer.regions.size(), "region index is out of range");
		st.at = At::Region;
		st.sample = i;
		return;
	}

	if (t.name == "sample" && st.at != At::Region) {
		const Params* p = nullptr;
		if (st.at == At::Root) p = only_params(doc);
		else if (st.at == At::Entry) {
			need(st.entry->mpb && st.entry->mpb->has_param, "entry has no parameter block");
			p = &st.entry->mpb->params;
		} else if (st.at == At::Params) p = st.params;
		else fail("sample is not valid here");

		uint32_t i = one_index(t);
		need(i < p->samples.size(), "sample index is out of range");
		st.at = At::Sample;
		st.params = p;
		st.sample = i;
		return;
	}

	if (t.name == "mpb" || t.name == "mpbw" || t.name == "mpbp" || t.name == "mlt" || t.name == "mltm" || t.name == "msb") {
		need_idx0();
		const Mpb* m = nullptr;
		const Msb* s = nullptr;
		if (st.at == At::Entry) {
			m = st.entry->mpb ? &*st.entry->mpb : nullptr;
			s = st.entry->msb ? &*st.entry->msb : nullptr;
		} else if (st.at == At::Root) {
			if (t.name == "mlt" || t.name == "mltm") need(doc.kind == FileKind::Mlt, "this file is not an MLT");
			else if (t.name == "msb") s = only_msb(doc);
			else m = only_mpb(doc);
		} else fail(t.name + " is not valid here");

		st.at = At::Chunk;
		if (t.name == "mlt") st.chunk = 0;
		else if (t.name == "mltm") st.chunk = 0x10;
		else if (t.name == "mpb") {
			need(m != nullptr, "no MPB chunk here");
			st.chunk = m->head;
		} else if (t.name == "mpbw") {
			need(m != nullptr && m->has_wave, "no MPBW chunk here");
			st.chunk = m->wave;
		} else if (t.name == "mpbp") {
			need(m != nullptr && m->has_param, "no MPBP chunk here");
			st.chunk = m->param;
		} else if (t.name == "msb") {
			need(s != nullptr, "no MSB chunk here");
			st.chunk = s->head;
		}

		return;
	}

	fail("unknown path component '" + t.name + "'");
}

struct FieldDef {
	const char* name;
	uint32_t off;
	LocKind kind;
	Show show;
};

const FieldDef kSampleFields[] = {
	{"length", 0x00, LocKind::U32, Show::Dec},
	{"nibbles", 0x04, LocKind::U32, Show::Dec},
	{"hdr04", 0x04, LocKind::U32, Show::Hex},
	{"rate", 0x08, LocKind::U32, Show::Dec},
	{"hdr08", 0x08, LocKind::U32, Show::Hex},
	{"loop", 0x0C, LocKind::U16, Show::Dec},
	{"dsp_format", 0x0E, LocKind::U16, Show::Dec},
	{"hdr0e", 0x0E, LocKind::U16, Show::Hex},
	{"loop_adr", 0x10, LocKind::U32, Show::Hex},
	{"end_adr", 0x14, LocKind::U32, Show::Hex},
	{"cur_adr", 0x18, LocKind::U32, Show::Hex},
	{"gain", 0x3C, LocKind::U16, Show::Dec},
	{"pred_scale", 0x3E, LocKind::U16, Show::Dec},
	{"yn1", 0x40, LocKind::U16, Show::Dec},
	{"yn2", 0x42, LocKind::U16, Show::Dec},
	{"loop_pred_scale", 0x44, LocKind::U16, Show::Dec},
	{"loop_yn1", 0x46, LocKind::U16, Show::Dec},
	{"loop_yn2", 0x48, LocKind::U16, Show::Dec},
	{"format", 0x4A, LocKind::U8, Show::Dec},
	{"pad", 0x4B, LocKind::U8, Show::Hex},
	{"offset", 0x4C, LocKind::U32, Show::Hex},
};

const FieldDef kRegionFields[] = {
	{"sample", 0x00, LocKind::U32, Show::Dec},
	{"unk04", 0x04, LocKind::U8, Show::Dec},
	{"velcurve", 0x05, LocKind::U8, Show::Dec},
	{"vel_lo", 0x06, LocKind::U8, Show::Dec},
	{"vel_hi", 0x07, LocKind::U8, Show::Dec},
	{"key_lo", 0x08, LocKind::U8, Show::Dec},
	{"key_hi", 0x09, LocKind::U8, Show::Dec},
	{"level", 0x0A, LocKind::U8, Show::Dec},
	{"pan", 0x0B, LocKind::U8, Show::Dec},
	{"root", 0x0C, LocKind::U8, Show::Dec},
	{"fine", 0x0D, LocKind::U8, Show::Dec},
	{"fx_invert", 0x0E, LocKind::U8, Show::Dec},
	{"fx_send", 0x0F, LocKind::U8, Show::Dec},
	{"atk", 0x10, LocKind::U8, Show::Dec},
	{"dec", 0x11, LocKind::U8, Show::Dec},
	{"sus", 0x12, LocKind::U8, Show::Dec},
	{"sus_level", 0x13, LocKind::U8, Show::Dec},
	{"rel", 0x14, LocKind::U8, Show::Dec},
	{"rate_bias", 0x15, LocKind::U8, Show::Dec},
	{"atten", 0x16, LocKind::U8, Show::Dec},
};

Loc field_from(const FieldDef* defs, size_t n, uint32_t base, const Tok& t)
{
	need(t.idx.empty(), std::string(t.name) + " takes no index");
	for (size_t i = 0; i < n; i++) {
		if (t.name == defs[i].name) {
			Loc loc;
			loc.off = base + defs[i].off;
			loc.kind = defs[i].kind;
			loc.show = defs[i].show;
			return loc;
		}
	}

	std::string names;
	for (size_t i = 0; i < n; i++) {
		if (i) names += " ";
		names += defs[i].name;
	}

	fail("unknown field '" + t.name + "' (known: " + names + ")");
}

Loc field_of(const Document& doc, State st, const Tok& t)
{
	if (t.name == "unk" && t.idx.empty() &&
	    (st.at == At::Root || st.at == At::Chunk) &&
	    (doc.kind == FileKind::Mpbp || doc.kind == FileKind::Mpbw || doc.kind == FileKind::Msb) && st.at == At::Root) {
		Loc loc;
		loc.off = 8;
		loc.kind = LocKind::U32;
		loc.show = Show::Hex;
		return loc;
	}

	if (t.name == "size" && t.idx.empty() && st.at == At::Root &&
	    (doc.kind == FileKind::Mpbp || doc.kind == FileKind::Mpbw || doc.kind == FileKind::Msb)) {
		Loc loc;
		loc.off = 12;
		loc.kind = LocKind::U32;
		loc.show = Show::Hex;
		return loc;
	}

	if ((t.name == "sample_table" || t.name == "vel_table" || t.name == "prog_table") && st.at != At::Sample) {
		const Params* p = nullptr;
		if (st.at == At::Root) p = only_params(doc);
		else if (st.at == At::Entry) {
			need(st.entry->mpb && st.entry->mpb->has_param, "entry has no parameter block");
			p = &st.entry->mpb->params;
		} else if (st.at == At::Params) p = st.params;
		else fail(t.name + " is a field of the parameter block");

		need(p->size >= 12, "parameter block has no table words");
		need(t.idx.empty(), t.name + " takes no index");
		Loc loc;
		loc.kind = LocKind::U32;
		loc.show = Show::Hex;
		if (t.name == "sample_table") loc.off = p->base;
		else if (t.name == "vel_table") loc.off = p->base + 4;
		else loc.off = p->base + 8;

		return loc;
	}

	if (t.name == "vel") {
		const Params* p = nullptr;
		if (st.at == At::Root) p = only_params(doc);
		else if (st.at == At::Entry) {
			need(st.entry->mpb && st.entry->mpb->has_param, "entry has no parameter block");
			p = &st.entry->mpb->params;
		} else if (st.at == At::Params) p = st.params;
		else fail("vel is a field of the parameter block");

		need(!t.idx.empty() && t.idx.size() <= 2, "vel takes vel[curve] or vel[curve][byte]");
		need(t.idx[0] < p->curves.size(), "velocity curve index is out of range");
		Loc loc;
		loc.off = p->curves[t.idx[0]].off;
		if (t.idx.size() == 2) {
			need(t.idx[1] < CURVE_BYTES, "velocity byte is out of range");
			loc.off += t.idx[1];
			loc.kind = LocKind::U8;
			loc.show = Show::Dec;
		} else {
			loc.kind = LocKind::Hex;
			loc.size = CURVE_BYTES;
		}

		return loc;
	}

	if (st.at == At::Sample && t.name == "coef") {
		uint32_t base = st.params->samples[st.sample].off + 0x1C;
		if (t.idx.empty()) {
			Loc loc;
			loc.off = base;
			loc.kind = LocKind::Hex;
			loc.size = 32;
			return loc;
		}

		need(t.idx.size() == 1, "coef takes coef[N]");
		need(t.idx[0] < 16, "coef index is out of range");
		Loc loc;
		loc.off = base + t.idx[0] * 2;
		loc.kind = LocKind::U16;
		loc.show = Show::Dec;
		return loc;
	}

	if (st.at == At::Entry && (t.name == "type" || t.name == "bank" || t.name == "type_word" || t.name == "bank_word" ||
	                           t.name == "extra" || t.name == "offset")) {
		need(t.idx.empty(), t.name + " takes no index");
		Loc loc;
		loc.show = Show::Hex;
		if (t.name == "type") {
			loc.off = st.entry->off;
			loc.kind = LocKind::Type;
		} else if (t.name == "bank") {
			loc.off = st.entry->off + 4;
			loc.kind = LocKind::Bank;
		} else if (t.name == "type_word") {
			loc.off = st.entry->off;
			loc.kind = LocKind::U32;
		} else if (t.name == "bank_word") {
			loc.off = st.entry->off + 4;
			loc.kind = LocKind::U32;
		} else if (t.name == "offset") {
			loc.off = st.entry->off + 8;
			loc.kind = LocKind::U32;
		} else {
			loc.off = st.entry->off + 12;
			loc.kind = LocKind::U32;
		}

		return loc;
	}

	if (st.at == At::Chunk && (t.name == "unk" || t.name == "size")) {
		need(t.idx.empty(), t.name + " takes no index");
		Loc loc;
		loc.off = st.chunk + (t.name == "unk" ? 8u : 12u);
		loc.kind = LocKind::U32;
		loc.show = Show::Hex;
		return loc;
	}

	if (st.at == At::Sample) {
		return field_from(kSampleFields, sizeof kSampleFields / sizeof kSampleFields[0],
		                  st.params->samples[st.sample].off, t);
	}

	if (st.at == At::Region) {
		const Layer& layer = st.params->progs[st.prog].layers[st.layer];
		if (t.name == "unk") {
			need(t.idx.empty(), "unk takes no index");
			Loc loc;
			loc.off = layer.regions[st.sample].off + 0x17;
			loc.kind = LocKind::Hex;
			loc.size = 0x29;
			return loc;
		}

		return field_from(kRegionFields, sizeof kRegionFields / sizeof kRegionFields[0], layer.regions[st.sample].off, t);
	}

	if (st.at == At::Layer) {
		uint32_t base = st.params->progs[st.prog].layers[st.layer].off;
		need(t.idx.empty(), t.name + " takes no index");
		if (t.name == "bend_up") {
			Loc loc;
			loc.off = base + 4;
			loc.kind = LocKind::U8;
			return loc;
		}

		if (t.name == "bend_down") {
			Loc loc;
			loc.off = base + 5;
			loc.kind = LocKind::U8;
			return loc;
		}

		if (t.name == "unk") {
			Loc loc;
			loc.off = base + 6;
			loc.kind = LocKind::Hex;
			loc.size = 0x0A;
			return loc;
		}

		fail("unknown layer field '" + t.name + "' (known: bend_up bend_down unk)");
	}

	if (t.name == "sample" || t.name == "prog" || t.name == "layer" || t.name == "region" || t.name == "entry") fail("name a field under " + t.name);
	fail("unknown field '" + t.name + "'");
}

Loc resolve(const Document& doc, const std::string& path)
{
	need(doc.ok, "file has no structure to edit");
	std::vector<Tok> toks = tokenize(path);
	State st;
	for (size_t i = 0; i + 1 < toks.size(); i++) descend(doc, st, toks[i]);
	return field_of(doc, st, toks.back());
}

std::string hex_blob(const std::vector<uint8_t>& b, uint32_t off, uint32_t n)
{
	need(static_cast<uint64_t>(off) + n <= b.size(), "field runs past the end of the file");
	std::string s;
	s.resize(n * 2);
	static const char* hexd = "0123456789abcdef";
	for (uint32_t i = 0; i < n; i++) {
		s[i * 2] = hexd[b[off + i] >> 4];
		s[i * 2 + 1] = hexd[b[off + i] & 0xF];
	}

	return s;
}

long long parse_int(const std::string& text, long long lo, long long hi)
{
	need(!text.empty(), "missing value");
	size_t used = 0;
	long long v = 0;
	try {
		v = std::stoll(text, &used, 0);
	} catch (const std::exception&) {
		fail("bad number '" + text + "'");
	}

	need(used == text.size(), "bad number '" + text + "'");
	need(v >= lo && v <= hi, "value " + text + " is out of range");
	return v;
}

void write_hex(std::vector<uint8_t>& b, uint32_t off, uint32_t n, const std::string& text)
{
	std::string hex;
	hex.reserve(text.size());
	for (char c : text) {
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;

		hex.push_back(c);
	}

	if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex.erase(0, 2);
	need(hex.size() == n * 2, "hex value must be " + std::to_string(n) + " bytes");
	need(static_cast<uint64_t>(off) + n <= b.size(), "field runs past the end of the file");
	auto nyb = [](char c) -> int {
		if (c >= '0' && c <= '9') return c - '0';

		if (c >= 'a' && c <= 'f') return c - 'a' + 10;

		if (c >= 'A' && c <= 'F') return c - 'A' + 10;

		return -1;
	};

	for (uint32_t i = 0; i < n; i++) {
		int hi = nyb(hex[i * 2]);
		int lo = nyb(hex[i * 2 + 1]);
		need(hi >= 0 && lo >= 0, "bad hex digit");
		b[off + i] = uint8_t((hi << 4) | lo);
	}
}

uint32_t type_value(const std::string& text)
{
	if (text == "mpb" || text == "MPB" || text == "1") return 1;

	if (text == "msb" || text == "MSB" || text == "4") return 4;

	long long v = parse_int(text, 0, 15);
	return uint32_t(v);
}

void write_loc(std::vector<uint8_t>& b, const Loc& loc, const std::string& text)
{
	if (loc.kind == LocKind::Hex) {
		write_hex(b, loc.off, loc.size, text);
		return;
	}

	if (loc.kind == LocKind::Type || loc.kind == LocKind::Bank) {
		uint32_t nibble = loc.kind == LocKind::Type ? type_value(text) : uint32_t(parse_int(text, 0, 15));
		uint32_t w = ru32(b, loc.off);
		w = (w & 0xF0FFFFFFu) | (nibble << 24);
		wu32(b, loc.off, w);
		return;
	}

	if (loc.kind == LocKind::U8) {
		wu8(b, loc.off, uint8_t(parse_int(text, -128, 255)));
		return;
	}

	if (loc.kind == LocKind::U16) {
		wu16(b, loc.off, uint16_t(parse_int(text, -32768, 65535)));
		return;
	}

	wu32(b, loc.off, uint32_t(parse_int(text, -2147483647LL - 1, 4294967295LL)));
}

std::string read_loc(const std::vector<uint8_t>& b, const Loc& loc)
{
	if (loc.kind == LocKind::Hex) return hex_blob(b, loc.off, loc.size);

	if (loc.kind == LocKind::Type) {
		uint32_t n = (ru32(b, loc.off) >> 24) & 0xF;
		if (n == 1) return "mpb";

		if (n == 4) return "msb";

		return std::to_string(n);
	}

	if (loc.kind == LocKind::Bank) return std::to_string((ru32(b, loc.off) >> 24) & 0xF);

	if (loc.kind == LocKind::U8) return std::to_string(ru8(b, loc.off));

	if (loc.kind == LocKind::U16) return std::to_string(ru16(b, loc.off));

	uint32_t v = ru32(b, loc.off);
	if (loc.show == Show::Hex) return hex8(v);

	return std::to_string(v);
}

void refresh(Document& doc)
{
	auto name = doc.name;
	auto bytes = doc.bytes;
	try {
		doc = parse(std::move(bytes), name);
	} catch (const std::exception& ex) {
		doc.ok = false;
		doc.kind = FileKind::None;
		doc.entries.clear();
		doc.mpb.reset();
		doc.params.reset();
		doc.msb.reset();
		doc.warnings.push_back(ex.what());
	}
}

/* ---- rebuild -------------------------------------------------------------------------------- */

void append32(std::vector<uint8_t>& b, uint32_t v)
{
	b.push_back(uint8_t(v >> 24));
	b.push_back(uint8_t(v >> 16));
	b.push_back(uint8_t(v >> 8));
	b.push_back(uint8_t(v));
}

void append_chunk(std::vector<uint8_t>& b, uint32_t type, uint32_t unk, uint32_t size)
{
	append32(b, MAGIC);
	append32(b, type);
	append32(b, unk);
	append32(b, size);
}

std::vector<uint8_t> rebuild_mpb(const Document& doc, const Mpb& m, const std::vector<uint8_t>* wave,
                                 const std::vector<uint8_t>* params)
{
	need(m.has_wave && m.has_param, "MPB is missing its wave or parameter chunk");
	std::vector<uint8_t> wave_bytes;
	std::vector<uint8_t> param_bytes;
	if (wave) wave_bytes = *wave;
	else {
		uint32_t n = ru32(doc.bytes, m.wave + 12);
		uint32_t off = m.wave + CHUNK_BYTES;
		need(static_cast<uint64_t>(off) + n <= doc.bytes.size(), "MPBW payload is truncated");
		wave_bytes.assign(doc.bytes.begin() + off, doc.bytes.begin() + off + n);
	}

	if (params) param_bytes = *params;
	else param_bytes.assign(doc.bytes.begin() + m.params.base, doc.bytes.begin() + m.params.base + m.params.size);

	uint32_t body = CHUNK_BYTES + uint32_t(wave_bytes.size()) + CHUNK_BYTES + uint32_t(param_bytes.size());
	std::vector<uint8_t> out;
	append_chunk(out, T_MPB, ru32(doc.bytes, m.head + 8), body);
	append_chunk(out, T_MPBW, ru32(doc.bytes, m.wave + 8), uint32_t(wave_bytes.size()));
	out.insert(out.end(), wave_bytes.begin(), wave_bytes.end());
	append_chunk(out, T_MPBP, ru32(doc.bytes, m.param + 8), uint32_t(param_bytes.size()));
	out.insert(out.end(), param_bytes.begin(), param_bytes.end());
	return out;
}

std::vector<uint8_t> payload_of(std::vector<uint8_t> in, uint32_t type, const char* whole)
{
	if (in.size() >= CHUNK_BYTES && ru32(in, 0) == MAGIC) {
		uint32_t got = ru32(in, 4);
		if (got == T_MPB || got == T_MLT) fail(std::string("that file is a whole ") + chunk_label(got) + "; " + whole);
		if (got == type) {
			uint32_t sz = ru32(in, 12);
			uint64_t end = uint64_t(CHUNK_BYTES) + sz;
			if (end > in.size()) end = in.size();
			return std::vector<uint8_t>(in.begin() + CHUNK_BYTES, in.begin() + static_cast<std::ptrdiff_t>(end));
		}
	}

	return in;
}

void splice_bank(Document& doc, uint32_t pos, uint32_t old_len, std::vector<uint8_t> neu, int replaced)
{
	need(static_cast<uint64_t>(pos) + old_len <= doc.bytes.size(), "bank extent runs past the file");
	while (neu.size() % 32 != old_len % 32) neu.push_back(0);
	int64_t delta = int64_t(neu.size()) - int64_t(old_len);
	std::vector<uint8_t> out;
	out.reserve(doc.bytes.size() + neu.size());
	out.insert(out.end(), doc.bytes.begin(), doc.bytes.begin() + pos);
	out.insert(out.end(), neu.begin(), neu.end());
	out.insert(out.end(), doc.bytes.begin() + pos + old_len, doc.bytes.end());
	if (doc.kind == FileKind::Mlt && out.size() >= CHUNK_BYTES) {
		uint32_t sz = ru32(out, 12);
		wu32(out, 12, uint32_t(int64_t(sz) + delta));
		for (const Entry& e : doc.entries) {
			if (int(e.index) == replaced || e.off + 12 > out.size()) continue;

			uint32_t rel = ru32(out, e.off + 8);
			uint64_t bpos = 0x20ull + rel;
			if (bpos >= uint64_t(pos) + old_len) wu32(out, e.off + 8, uint32_t(int64_t(rel) + delta));
		}
	}

	doc.bytes = std::move(out);
	refresh(doc);
}

const Mpb* mpb_for(const Document& doc, int entry)
{
	if (entry < 0) {
		need(doc.mpb.has_value(), "an MLT needs -e ENTRY");
		return &*doc.mpb;
	}

	need(doc.kind == FileKind::Mlt, "-e is for an MLT bundle");
	need(entry >= 0 && uint32_t(entry) < doc.entries.size(), "entry index is out of range");
	need(doc.entries[entry].mpb.has_value(), "entry is not an MPB");
	return &*doc.entries[entry].mpb;
}

} // namespace

std::vector<uint8_t> read_file(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	need(bool(in), "cannot open " + path);
	in.seekg(0, std::ios::end);
	std::streamoff n = in.tellg();
	need(n >= 0, "cannot read " + path);
	in.seekg(0);
	std::vector<uint8_t> b(static_cast<size_t>(n));
	if (n > 0) {
		in.read(reinterpret_cast<char*>(b.data()), n);
		need(bool(in), "cannot read " + path);
	}

	return b;
}

void save_file(const Document& doc, const std::string& path)
{
	std::ofstream out(path, std::ios::binary);
	need(bool(out), "cannot write " + path);
	if (!doc.bytes.empty()) out.write(reinterpret_cast<const char*>(doc.bytes.data()), std::streamsize(doc.bytes.size()));
	need(bool(out), "cannot write " + path);
}

Document parse(std::vector<uint8_t> bytes, const std::string& name)
{
	Document doc;
	doc.bytes = std::move(bytes);
	doc.name = name;
	parse_root_chunk(doc);
	return doc;
}

Document load_file(const std::string& path)
{
	return parse(read_file(path), path);
}

std::string info_text(const Document& doc)
{
	std::ostringstream o;
	o << "EMALT " << doc.name << "\n";
	const char* kind = "unknown";
	if (doc.kind == FileKind::Mlt) kind = "MLT";
	else if (doc.kind == FileKind::Mpb) kind = "MPB";
	else if (doc.kind == FileKind::Mpbp) kind = "MPBP";
	else if (doc.kind == FileKind::Mpbw) kind = "MPBW";
	else if (doc.kind == FileKind::Msb) kind = "MSB";

	o << kind << "  " << doc.bytes.size() << " bytes\n";
	if (doc.kind == FileKind::Mlt && doc.bytes.size() >= 0x20) {
		o << "MLT unk " << hex8(ru32(doc.bytes, 8)) << "  size " << hex8(ru32(doc.bytes, 12)) << "\n";
		o << "MLTM unk " << hex8(ru32(doc.bytes, 0x18)) << "  size " << hex8(ru32(doc.bytes, 0x1C)) << "  entries "
		  << doc.entries.size() << "\n";
		for (const Entry& e : doc.entries) {
			o << "  [" << e.index << "] ";
			if (e.kind == EntryKind::Hang) {
				o << "bank word 0xFFFFFFFF\n";
				continue;
			}

			uint32_t n = entry_type_nibble(doc, e);
			if (const char* nm = nibble_name(n)) o << nm;
			else o << "type " << n;

			o << "  slot " << entry_slot(doc, e);
			if (e.has_pos) o << "  at " << hex8(e.bank_pos);
			if (e.mpb) {
				o << "  programs " << e.mpb->params.progs.size() << "  samples " << e.mpb->params.samples.size();
				if (e.mpb->has_wave) o << "  wave " << ru32(doc.bytes, e.mpb->wave + 12);
			} else if (e.msb) o << "  payload " << (e.msb->span >= CHUNK_BYTES ? e.msb->span - CHUNK_BYTES : 0);

			o << "\n";
		}
	} else if (doc.mpb) {
		o << "programs " << doc.mpb->params.progs.size() << "  samples " << doc.mpb->params.samples.size()
		  << "  curves " << doc.mpb->params.curves.size() << "\n";
	} else if (doc.params) {
		o << "programs " << doc.params->progs.size() << "  samples " << doc.params->samples.size() << "\n";
	} else if (doc.kind == FileKind::Mpbw && doc.bytes.size() >= CHUNK_BYTES) {
		o << "wave " << ru32(doc.bytes, 12) << " bytes\n";
	} else if (doc.msb) o << "payload " << (doc.msb->span >= CHUNK_BYTES ? doc.msb->span - CHUNK_BYTES : 0) << " bytes\n";

	for (const std::string& w : doc.warnings) {
		o << "warning: " << w << "\n";
	}

	return o.str();
}

std::string dump_text(const Document& doc, bool full)
{
	std::ostringstream o;
	o << info_text(doc);
	if (doc.kind == FileKind::Mlt) {
		for (const Entry& e : doc.entries) {
			if (e.mpb) {
				o << "entry " << e.index << "\n";
				dump_mpb(o, doc, *e.mpb, "  ", full);
			}
		}
	} else if (doc.mpb) dump_mpb(o, doc, *doc.mpb, "", full);
	else if (doc.params) dump_params(o, doc, *doc.params, "", full);

	return o.str();
}

std::string get_field(const Document& doc, const std::string& path)
{
	return read_loc(doc.bytes, resolve(doc, path));
}

void set_field(Document& doc, const std::string& path, const std::string& value, bool reparse)
{
	Loc loc = resolve(doc, path);
	write_loc(doc.bytes, loc, value);
	if (reparse) refresh(doc);
}

std::string peek_bytes(const Document& doc, uint32_t off, uint32_t count)
{
	need(count > 0 && count <= 4096, "peek count must be 1..4096");
	need(static_cast<uint64_t>(off) + count <= doc.bytes.size(), "peek runs past the end of the file");
	std::ostringstream o;
	o << hex8(off);
	for (uint32_t i = 0; i < count; i++) {
		char buf[8];
		std::snprintf(buf, sizeof buf, " %02X", doc.bytes[off + i]);
		o << buf;
	}

	o << "\n";
	return o.str();
}

void poke_bytes(Document& doc, uint32_t off, const std::string& type, const std::string& value)
{
	Loc loc;
	loc.off = off;
	loc.show = Show::Hex;
	if (type == "u8") loc.kind = LocKind::U8;
	else if (type == "u16") loc.kind = LocKind::U16;
	else if (type == "u32") loc.kind = LocKind::U32;
	else fail("poke type must be u8, u16, or u32");

	write_loc(doc.bytes, loc, value);
	refresh(doc);
}

std::string extract_to(const Document& doc, const std::string& dir)
{
	need(doc.ok, "file has no structure to extract");
	fs::create_directories(dir);
	std::ostringstream listed;
	auto write_span = [&](const std::string& leaf, uint32_t off, uint32_t n) {
		need(static_cast<uint64_t>(off) + n <= doc.bytes.size(), "extract runs past the file");
		fs::path path = fs::path(dir) / leaf;
		std::ofstream out(path, std::ios::binary);
		need(bool(out), "cannot write " + path.string());
		if (n) out.write(reinterpret_cast<const char*>(doc.bytes.data() + off), std::streamsize(n));
		need(bool(out), "cannot write " + path.string());
		listed << path.string() << "\n";
	};

	if (doc.kind == FileKind::Mlt) {
		for (const Entry& e : doc.entries) {
			if (e.kind == EntryKind::Hang || !e.has_pos) continue;

			char leaf[64];
			const char* ext = "bin";
			if (e.kind == EntryKind::Mpb) ext = "mpb";
			else if (e.kind == EntryKind::Msb) ext = "msb";

			std::snprintf(leaf, sizeof leaf, "entry%02u_slot%02u.%s", e.index, entry_slot(doc, e), ext);
			write_span(leaf, e.bank_pos, e.bank_len);
		}
	} else if (doc.mpb) {
		if (doc.mpb->has_wave) {
			uint32_t n = ru32(doc.bytes, doc.mpb->wave + 12);
			write_span("wave.bin", doc.mpb->wave + CHUNK_BYTES, n);
		}

		if (doc.mpb->has_param) write_span("params.bin", doc.mpb->params.base, doc.mpb->params.size);
	} else if (doc.params) write_span("params.bin", doc.params->base, doc.params->size);
	else if (doc.kind == FileKind::Mpbw) {
		uint32_t n = doc.bytes.size() >= CHUNK_BYTES ? ru32(doc.bytes, 12) : 0;
		if (uint64_t(CHUNK_BYTES) + n > doc.bytes.size()) {
			n = doc.bytes.size() > CHUNK_BYTES ? uint32_t(doc.bytes.size() - CHUNK_BYTES) : 0;
		}

		write_span("wave.bin", CHUNK_BYTES, n);
	} else if (doc.msb) {
		uint32_t n = doc.msb->span >= CHUNK_BYTES ? doc.msb->span - CHUNK_BYTES : 0;
		write_span("payload.bin", doc.msb->head + CHUNK_BYTES, n);
	}

	return listed.str();
}

void replace_wave(Document& doc, std::vector<uint8_t> wave, int entry)
{
	wave = payload_of(std::move(wave), T_MPBW, "use bank-replace");
	const Mpb* m = mpb_for(doc, entry);
	uint32_t pos = m->head;
	uint32_t old = m->span;
	std::vector<uint8_t> neu = rebuild_mpb(doc, *m, &wave, nullptr);
	splice_bank(doc, pos, old, std::move(neu), entry);
}

void install_sample_wave(Document& doc, int entry, int sample, SampleWave wave)
{
	const Mpb* m = mpb_for(doc, entry);
	need(m->has_wave && m->has_param, "this bank has no wave to replace");
	const Params& params = m->params;
	need(sample >= 0 && sample < (int)params.samples.size(), "sample index is out of range");
	uint32_t wave_n = ru32(doc.bytes, m->wave + 12);
	uint32_t wave_at = m->wave + CHUNK_BYTES;
	need(uint64_t(wave_at) + wave_n <= doc.bytes.size(), "MPBW payload is truncated");
	need(params.base <= params.samples[sample].off, "sample record is outside the parameters");
	need(params.samples[sample].off + SAMPLE_BYTES <= params.base + params.size, "sample record is outside the parameters");

	struct Group {
		uint32_t off = 0;
		uint32_t len = 0;
		std::vector<int> index;
	};

	std::vector<Group> groups;
	for (int i = 0; i < (int)params.samples.size(); i++) {
		uint32_t off = ru32(doc.bytes, params.samples[i].off + 0x4C);
		auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.off == off; });
		if (it == groups.end()) groups.push_back(Group{off, 0, {i}});
		else it->index.push_back(i);
	}

	std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.off < b.off; });
	for (size_t i = 0; i < groups.size(); i++) {
		uint32_t end = (i + 1 < groups.size()) ? groups[i + 1].off : wave_n;
		if (groups[i].off < wave_n && end >= groups[i].off) groups[i].len = end - groups[i].off;
	}

	std::vector<uint8_t> payload;
	std::vector<uint8_t> param_bytes(doc.bytes.begin() + params.base, doc.bytes.begin() + params.base + params.size);
	auto patch32 = [&](int index, uint32_t rel, uint32_t v) {
		wu32(param_bytes, params.samples[index].off - params.base + rel, v);
	};

	auto patch16 = [&](int index, uint32_t rel, uint16_t v) {
		wu16(param_bytes, params.samples[index].off - params.base + rel, v);
	};

	for (const Group& g : groups) {
		uint32_t at = (uint32_t)payload.size();
		bool replaced = std::find(g.index.begin(), g.index.end(), sample) != g.index.end();
		if (replaced) payload.insert(payload.end(), wave.data.begin(), wave.data.end());
		else if (g.len && uint64_t(g.off) + g.len <= wave_n) {
			payload.insert(payload.end(), doc.bytes.begin() + wave_at + g.off, doc.bytes.begin() + wave_at + g.off + g.len);
		}

		for (int index : g.index) patch32(index, 0x4C, at);
	}

	patch32(sample, 0x00, wave.length);
	patch32(sample, 0x04, wave.nibbles);
	patch32(sample, 0x08, wave.rate);
	patch16(sample, 0x0C, wave.loop);
	patch32(sample, 0x10, wave.loop_adr);
	patch32(sample, 0x14, wave.end_adr);
	patch32(sample, 0x18, wave.cur_adr);
	for (int i = 0; i < 16; i++) patch16(sample, 0x1C + (uint32_t)i * 2, (uint16_t)wave.coef[i]);
	patch16(sample, 0x3C, 0);
	patch16(sample, 0x3E, wave.pred_scale);
	patch16(sample, 0x40, (uint16_t)wave.yn1);
	patch16(sample, 0x42, (uint16_t)wave.yn2);
	patch16(sample, 0x44, wave.loop_pred_scale);
	patch16(sample, 0x46, (uint16_t)wave.loop_yn1);
	patch16(sample, 0x48, (uint16_t)wave.loop_yn2);
	param_bytes[params.samples[sample].off - params.base + 0x4A] = wave.format;

	uint32_t pos = m->head;
	uint32_t old = m->span;
	std::vector<uint8_t> neu = rebuild_mpb(doc, *m, &payload, &param_bytes);
	splice_bank(doc, pos, old, std::move(neu), entry);
}

struct ParamBlock {
	const Mpb* mpb = nullptr;
	bool mpbp = false;
	std::vector<uint8_t> bytes;
	std::vector<uint32_t> words;
};

ParamBlock load_params(Document& doc, int entry, const Params*& params)
{
	ParamBlock block;
	if (doc.kind == FileKind::Mpbp && entry < 0) {
		need(doc.params.has_value(), "this file has no parameter block");
		params = &*doc.params;
		block.mpbp = true;
	} else {
		block.mpb = mpb_for(doc, entry);
		need(block.mpb->has_param, "this bank has no parameter block");
		params = &block.mpb->params;
	}

	block.bytes.assign(doc.bytes.begin() + params->base, doc.bytes.begin() + params->base + params->size);
	auto add_word = [&](uint32_t rel) {
		need(uint64_t(rel) + 4 <= block.bytes.size(), "a table word is outside the parameters");
		for (uint32_t have : block.words) {
			if (have == rel) return;
		}

		block.words.push_back(rel);
	};

	add_word(0);
	add_word(4);
	add_word(8);
	uint32_t poff = ru32(block.bytes, 8) & 0xFFFFFFu;
	need(uint64_t(poff) + uint64_t(params->progs.size()) * 4 <= block.bytes.size(), "program table does not fit");
	for (uint32_t i = 0; i < params->progs.size(); i++) add_word(poff + i * 4);
	for (const Program& pr : params->progs) {
		for (const Layer& ly : pr.layers) {
			need(ly.off >= params->base, "layer record is outside the parameters");
			add_word(ly.off - params->base);
		}
	}

	return block;
}

void retarget(ParamBlock& block, uint32_t at, int delta)
{
	uint32_t cut = delta < 0 ? uint32_t(-delta) : 0;
	std::vector<uint32_t> next;
	for (uint32_t loc : block.words) {
		if (delta < 0 && loc < at + cut && loc + 4 > at) {
			bool inside = loc >= at && loc + 4 <= at + cut;
			need(inside, "a table word straddles the deleted bytes");
			continue;
		}

		uint32_t moved = loc;
		if (delta > 0 && loc >= at) moved = loc + uint32_t(delta);
		if (delta < 0 && loc >= at + cut) moved = loc - cut;
		uint32_t packed = ru32(block.bytes, moved);
		uint32_t off = packed & 0xFFFFFFu;
		uint32_t count = packed >> 24;
		if (delta > 0 && off >= at) off += uint32_t(delta);
		if (delta < 0 && off >= at + cut) {
			off -= cut;
		}

		wu32(block.bytes, moved, (count << 24) | off);
		bool seen = false;
		for (uint32_t have : next) {
			if (have == moved) seen = true;
		}

		if (!seen) next.push_back(moved);
	}

	block.words = std::move(next);
}

void insert_span(ParamBlock& block, uint32_t at, const uint8_t* data, uint32_t n)
{
	need(at <= block.bytes.size(), "insert is outside the parameters");
	need(block.bytes.size() + n <= 0xFFFFFFu, "parameter block cannot grow");
	block.bytes.insert(block.bytes.begin() + at, data, data + n);
	retarget(block, at, int(n));
}

void erase_span(ParamBlock& block, uint32_t at, uint32_t n)
{
	need(uint64_t(at) + n <= block.bytes.size(), "delete runs past the parameters");
	block.bytes.erase(block.bytes.begin() + at, block.bytes.begin() + at + n);
	retarget(block, at, -int(n));
}

void slide(uint32_t& pos, uint32_t at, int delta)
{
	if (delta > 0) {
		if (pos >= at) pos += uint32_t(delta);
		return;
	}

	uint32_t n = uint32_t(-delta);
	if (pos >= at + n) {
		pos -= n;
	}
}

void commit_params(Document& doc, int entry, ParamBlock& block)
{
	if (block.mpbp) {
		std::vector<uint8_t> out;
		append_chunk(out, T_MPBP, ru32(doc.bytes, 8), uint32_t(block.bytes.size()));
		out.insert(out.end(), block.bytes.begin(), block.bytes.end());
		doc.bytes = std::move(out);
		refresh(doc);
		return;
	}

	uint32_t pos = block.mpb->head;
	uint32_t old = block.mpb->span;
	std::vector<uint8_t> neu = rebuild_mpb(doc, *block.mpb, nullptr, &block.bytes);
	splice_bank(doc, pos, old, std::move(neu), entry);
}

void add_region(Document& doc, int entry, int prog, int layer)
{
	const Params* params = nullptr;
	ParamBlock block = load_params(doc, entry, params);
	need(prog >= 0 && prog < (int)params->progs.size(), "program index is out of range");
	const Program& program = params->progs[prog];
	need(layer >= 0 && layer < (int)program.layers.size(), "layer index is out of range");
	const Layer& target = program.layers[layer];
	need(target.off >= params->base, "layer record is outside the parameters");
	uint32_t layer_rel = target.off - params->base;
	need(uint64_t(layer_rel) + 4 <= block.bytes.size(), "layer record is outside the parameters");

	uint32_t word = ru32(block.bytes, layer_rel);
	uint32_t rcount = word >> 24;
	uint32_t roff = word & 0xFFFFFFu;
	need(rcount < 255, "a layer holds at most 255 regions");
	need(rcount == 0 || uint64_t(roff) + uint64_t(rcount) * REGION_BYTES <= block.bytes.size(), "region list does not fit");

	std::vector<uint8_t> born(REGION_BYTES, 0);
	uint32_t insert = 0;
	if (rcount == 0) {
		insert = uint32_t(block.bytes.size());
		born[6] = 0;
		born[7] = 127;
		born[8] = 60;
		born[9] = 72;
		born[0x0A] = 100;
		born[0x0B] = 15;
		born[0x0C] = 60;
		born[0x10] = 127;
		born[0x11] = 90;
		born[0x12] = 40;
		born[0x14] = 70;
	} else {
		insert = roff + rcount * REGION_BYTES;
		uint32_t src = roff + (rcount - 1) * REGION_BYTES;
		born.assign(block.bytes.begin() + src, block.bytes.begin() + src + REGION_BYTES);
		int lo = born[8];
		int hi = born[9];
		if (hi < lo) std::swap(lo, hi);
		if (hi + 12 <= 127) {
			born[8] = uint8_t(lo + 12);
			born[9] = uint8_t(hi + 12);
		} else if (lo >= 12) {
			born[8] = uint8_t(lo - 12);
			born[9] = uint8_t(hi - 12);
		}
	}

	insert_span(block, insert, born.data(), REGION_BYTES);
	uint32_t layer_at = layer_rel;
	slide(layer_at, insert, int(REGION_BYTES));
	uint32_t new_off = rcount == 0 ? insert : roff;
	wu32(block.bytes, layer_at, ((rcount + 1) << 24) | new_off);
	commit_params(doc, entry, block);
}

void delete_region(Document& doc, int entry, int prog, int layer, int region)
{
	const Params* params = nullptr;
	ParamBlock block = load_params(doc, entry, params);
	need(prog >= 0 && prog < (int)params->progs.size(), "program index is out of range");
	const Program& program = params->progs[prog];
	need(layer >= 0 && layer < (int)program.layers.size(), "layer index is out of range");
	const Layer& target = program.layers[layer];
	need(target.off >= params->base, "layer record is outside the parameters");
	uint32_t layer_rel = target.off - params->base;
	uint32_t word = ru32(block.bytes, layer_rel);
	uint32_t rcount = word >> 24;
	uint32_t roff = word & 0xFFFFFFu;
	need(region >= 0 && uint32_t(region) < rcount, "region index is out of range");
	need(uint64_t(roff) + uint64_t(rcount) * REGION_BYTES <= block.bytes.size(), "region list does not fit");
	uint32_t cut = roff + uint32_t(region) * REGION_BYTES;
	erase_span(block, cut, REGION_BYTES);
	uint32_t layer_at = layer_rel;
	slide(layer_at, cut, -int(REGION_BYTES));
	uint32_t new_off = rcount == 1 ? 0 : roff;
	wu32(block.bytes, layer_at, ((rcount - 1) << 24) | new_off);
	commit_params(doc, entry, block);
}

void add_layer(Document& doc, int entry, int prog)
{
	const Params* params = nullptr;
	ParamBlock block = load_params(doc, entry, params);
	need(prog >= 0 && prog < (int)params->progs.size(), "program index is out of range");
	const Program& program = params->progs[prog];
	uint32_t poff = ru32(block.bytes, 8) & 0xFFFFFFu;
	uint32_t prog_at = poff + uint32_t(prog) * 4;
	uint32_t word = ru32(block.bytes, prog_at);
	uint32_t lcount = uint32_t(program.layers.size());
	need(lcount < 255, "a program holds at most 255 layers");
	need((word >> 24) == lcount, "layer count does not match the program");
	uint32_t loff = word & 0xFFFFFFu;
	std::vector<uint8_t> born(LAYER_BYTES, 0);
	uint32_t insert = 0;
	if (lcount == 0) {
		insert = uint32_t(block.bytes.size());
		born[4] = 2;
		born[5] = 2;
	} else {
		need(uint64_t(loff) + uint64_t(lcount) * LAYER_BYTES <= block.bytes.size(), "layer list does not fit");
		insert = loff + lcount * LAYER_BYTES;
		uint32_t prev = loff + (lcount - 1) * LAYER_BYTES;
		born[4] = block.bytes[prev + 4];
		born[5] = block.bytes[prev + 5];
	}

	insert_span(block, insert, born.data(), LAYER_BYTES);
	slide(prog_at, insert, int(LAYER_BYTES));
	uint32_t new_off = lcount == 0 ? insert : loff;
	wu32(block.bytes, prog_at, ((lcount + 1) << 24) | new_off);
	commit_params(doc, entry, block);
}

void remove_layer(Document& doc, int entry, int prog, int layer)
{
	const Params* params = nullptr;
	ParamBlock block = load_params(doc, entry, params);
	need(prog >= 0 && prog < (int)params->progs.size(), "program index is out of range");
	const Program& program = params->progs[prog];
	need(layer >= 0 && layer < (int)program.layers.size(), "layer index is out of range");
	uint32_t poff = ru32(block.bytes, 8) & 0xFFFFFFu;
	uint32_t prog_at = poff + uint32_t(prog) * 4;
	const Layer& target = program.layers[layer];
	need(target.off >= params->base, "layer record is outside the parameters");
	uint32_t layer_rel = target.off - params->base;
	uint32_t word = ru32(block.bytes, layer_rel);
	uint32_t rcount = word >> 24;
	uint32_t roff = word & 0xFFFFFFu;
	need(rcount == 0 || uint64_t(roff) + uint64_t(rcount) * REGION_BYTES <= block.bytes.size(), "region list does not fit");
	uint32_t region_n = rcount * REGION_BYTES;
	if (region_n != 0 && layer_rel < roff + region_n && roff < layer_rel + LAYER_BYTES) {
		fail("layer record overlaps its region list");
	}

	struct Cut {
		uint32_t at;
		uint32_t n;
	};

	Cut cuts[2];
	int ncut = 0;
	if (region_n != 0) {
		cuts[ncut++] = Cut{roff, region_n};
	}

	cuts[ncut++] = Cut{layer_rel, LAYER_BYTES};
	if (ncut == 2 && cuts[0].at < cuts[1].at) std::swap(cuts[0], cuts[1]);
	for (int i = 0; i < ncut; i++) {
		erase_span(block, cuts[i].at, cuts[i].n);
		slide(prog_at, cuts[i].at, -int(cuts[i].n));
		for (int k = i + 1; k < ncut; k++) slide(cuts[k].at, cuts[i].at, -int(cuts[i].n));
	}

	uint32_t packed = ru32(block.bytes, prog_at);
	uint32_t count = packed >> 24;
	uint32_t off = packed & 0xFFFFFFu;
	need(count > 0, "program has no layer to remove");
	wu32(block.bytes, prog_at, ((count - 1) << 24) | off);
	commit_params(doc, entry, block);
}

void add_program(Document& doc, int entry)
{
	const Params* params = nullptr;
	ParamBlock block = load_params(doc, entry, params);
	uint32_t pcount = uint32_t(params->progs.size());
	need(pcount < 255, "a bank holds at most 255 programs");
	uint32_t head = ru32(block.bytes, 8);
	need((head >> 24) == pcount, "program count does not match the bank");
	uint32_t poff = head & 0xFFFFFFu;
	uint32_t insert = 0;
	if (pcount == 0) insert = uint32_t(block.bytes.size());
	else {
		need(uint64_t(poff) + uint64_t(pcount) * 4 <= block.bytes.size(), "program table does not fit");
		insert = poff + pcount * 4;
	}

	uint8_t born[4] = {0, 0, 0, 0};
	insert_span(block, insert, born, 4);
	uint32_t head_at = 8;
	slide(head_at, insert, 4);
	uint32_t new_off = pcount == 0 ? insert : poff;
	wu32(block.bytes, head_at, ((pcount + 1) << 24) | new_off);
	commit_params(doc, entry, block);
}

void replace_params(Document& doc, std::vector<uint8_t> params, int entry)
{
	params = payload_of(std::move(params), T_MPBP, "use bank-replace");
	if (doc.kind == FileKind::Mpbp && entry < 0) {
		std::vector<uint8_t> out;
		append_chunk(out, T_MPBP, ru32(doc.bytes, 8), uint32_t(params.size()));
		out.insert(out.end(), params.begin(), params.end());
		doc.bytes = std::move(out);
		refresh(doc);
		return;
	}

	const Mpb* m = mpb_for(doc, entry);
	uint32_t pos = m->head;
	uint32_t old = m->span;
	std::vector<uint8_t> neu = rebuild_mpb(doc, *m, nullptr, &params);
	splice_bank(doc, pos, old, std::move(neu), entry);
}

void replace_bank(Document& doc, int entry, std::vector<uint8_t> bank)
{
	need(doc.kind == FileKind::Mlt, "bank-replace edits an MLT bundle");
	need(entry >= 0 && uint32_t(entry) < doc.entries.size(), "entry index is out of range");
	const Entry& e = doc.entries[entry];
	need(e.kind != EntryKind::Hang && e.has_pos, "that entry has no bank to replace");
	need(e.bank_pos >= 0x20 + doc.entries.size() * 16, "bank offset falls inside the MLT table");
	need(bank.size() >= CHUNK_BYTES && ru32(bank, 0) == MAGIC, "replacement is not a gcax chunk");
	uint32_t t = ru32(bank, 4);
	need(t == T_MPB || t == T_MSB, "replacement must be an MPB or an MSB");
	uint32_t nibble = t == T_MPB ? 1u : 4u;
	uint32_t w = ru32(doc.bytes, e.off);
	wu32(doc.bytes, e.off, (w & 0xF0FFFFFFu) | (nibble << 24));
	splice_bank(doc, e.bank_pos, e.bank_len, std::move(bank), entry);
}

/* ---- self test ------------------------------------------------------------------------------ */

static void expect(bool cond, const char* m)
{
	if (!cond) fail(std::string("selftest: ") + m);
}

static void put32(std::vector<uint8_t>& b, uint32_t o, uint32_t v)
{
	b[o] = uint8_t(v >> 24);
	b[o + 1] = uint8_t(v >> 16);
	b[o + 2] = uint8_t(v >> 8);
	b[o + 3] = uint8_t(v);
}

static std::vector<uint8_t> sample_params(bool declared)
{
	std::vector<uint8_t> p(0x144, 0);
	uint32_t sc = declared ? 1u : 0u;
	put32(p, 0, (sc << 24) | 0x20);
	put32(p, 4, (sc << 24) | 0x70);
	put32(p, 8, (1u << 24) | 0xF0);
	put32(p, 0x20, 100);
	put32(p, 0x24, 200);
	put32(p, 0x28, 32000);
	p[0x20 + 0x0C] = 0;
	p[0x20 + 0x0D] = 1;
	p[0x20 + 0x1C] = 0x12;
	p[0x20 + 0x1D] = 0x34;
	p[0x20 + 0x4A] = 0;
	p[0x20 + 0x4B] = 0x5A;
	put32(p, 0x20 + 0x4C, 0x80);
	p[0x70 + 3] = 9;
	put32(p, 0xF0, (1u << 24) | 0xF4);
	put32(p, 0xF4, (1u << 24) | 0x104);
	p[0xF4 + 4] = 2;
	p[0xF4 + 5] = 3;
	p[0xF4 + 6] = 0xAB;
	put32(p, 0x104, 0);
	p[0x104 + 7] = 127;
	p[0x104 + 8] = 10;
	p[0x104 + 9] = 20;
	p[0x104 + 0x0A] = 100;
	p[0x104 + 0x0C] = 60;
	return p;
}

static std::vector<uint8_t> make_mpb(const std::vector<uint8_t>& params, uint8_t wave_byte, uint32_t wave_n)
{
	std::vector<uint8_t> m;
	uint32_t body = CHUNK_BYTES + wave_n + CHUNK_BYTES + uint32_t(params.size());
	append_chunk(m, T_MPB, 0xA5A5A5A5u, body);
	append_chunk(m, T_MPBW, 0, wave_n);
	m.insert(m.end(), wave_n, wave_byte);
	append_chunk(m, T_MPBP, 7, uint32_t(params.size()));
	m.insert(m.end(), params.begin(), params.end());
	return m;
}

static std::vector<uint8_t> make_bundle()
{
	auto mpb = make_mpb(sample_params(true), 0xAB, 32);
	std::vector<uint8_t> msb;
	append_chunk(msb, T_MSB, 0x22222222u, 8);
	const uint8_t payload[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
	msb.insert(msb.end(), payload, payload + 8);

	std::vector<uint8_t> file;
	append_chunk(file, T_MLT, 0, 0);
	append_chunk(file, T_MLTM, 0, 32);
	append32(file, 0x01000000u);
	append32(file, 0x06000000u);
	append32(file, 0x20);
	append32(file, 0x11111111u);
	append32(file, 0x04000000u);
	append32(file, 0x01000000u);
	append32(file, 0);
	append32(file, 0);
	expect(file.size() == 0x40, "table size");
	file.insert(file.end(), mpb.begin(), mpb.end());
	uint32_t off1 = uint32_t(file.size() - 0x20);
	file.insert(file.end(), msb.begin(), msb.end());
	put32(file, 0x30 + 8, off1);
	put32(file, 12, uint32_t(file.size() - 16));
	return file;
}

int selftest()
{
	try {
		parse(std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}, "x");
		expect(false, "bad magic should fail");
	} catch (const std::runtime_error&) {
	}

	{
		auto params = sample_params(false);
		std::vector<uint8_t> chunk;
		append_chunk(chunk, T_MPBP, 0, uint32_t(params.size()));
		chunk.insert(chunk.end(), params.begin(), params.end());
		Document bare = parse(chunk, "bare.mpbp");
		expect(bare.params && bare.params->samples.size() == 1, "inferred sample");
		expect(bare.params->curves.size() == 1, "inferred curve");
		expect(bare.warnings.empty(), "inference warnings");
	}

	Document doc = parse(make_bundle(), "synth.mlt");
	expect(doc.kind == FileKind::Mlt, "kind");
	expect(doc.entries.size() == 2, "two entries");
	expect(doc.entries[0].mpb && doc.entries[0].mpb->params.samples.size() == 1, "sample count");
	expect(doc.entries[0].mpb->params.progs.size() == 1, "prog count");
	expect(doc.entries[0].mpb->params.progs[0].layers[0].regions.size() == 1, "region count");
	expect(doc.entries[1].msb.has_value(), "msb");
	expect(get_field(doc, "entry[0].sample[0].length") == "100", "length");
	expect(get_field(doc, "slot[6].sample[0].coef[0]") == "4660", "coef");
	expect(get_field(doc, "entry[0].prog[0].layer[0].region[0].key_lo") == "10", "key");
	expect(get_field(doc, "entry[0].vel[0][3]") == "9", "vel byte");
	expect(get_field(doc, "entry[0].mpb.unk") == "0xA5A5A5A5", "mpb unk");
	expect(get_field(doc, "entry[0].sample[0].pad") == "90", "pad");
	expect(dump_text(doc, false).find("key 10-20") != std::string::npos, "dump key");
	expect(dump_text(doc, false).find("slot 6") != std::string::npos, "dump slot");

	auto orig = doc.bytes;
	fs::remove_all("build/emalt-selftest");
	std::string listed = extract_to(doc, "build/emalt-selftest");
	expect(listed.find("entry00_slot06.mpb") != std::string::npos, "extract name");
	auto part = read_file("build/emalt-selftest/entry00_slot06.mpb");
	Document round = parse(orig, "synth.mlt");
	replace_bank(round, 0, part);
	expect(round.bytes == orig, "bank replace round trip");
	fs::remove_all("build/emalt-selftest");

	auto before = doc.bytes;
	set_field(doc, "entry[0].prog[0].layer[0].region[0].key_lo", "40");
	expect(get_field(doc, "entry[0].prog[0].layer[0].region[0].key_lo") == "40", "set key");
	int diffs = 0;
	expect(before.size() == doc.bytes.size(), "set keeps size");
	for (size_t i = 0; i < before.size(); i++) {
		if (before[i] != doc.bytes[i]) diffs++;
	}

	expect(diffs == 1, "set changes one byte");

	set_field(doc, "entry[0].type_word", "0x0100ABCD");
	set_field(doc, "entry[0].type", "msb");
	expect(get_field(doc, "entry[0].type_word") == "0x0400ABCD", "type nibble");
	bool saw = false;
	for (const auto& w : doc.warnings) {
		if (w.find("type nibble") != std::string::npos) saw = true;
	}

	expect(saw, "type mismatch warning");
	set_field(doc, "entry[0].type", "mpb");
	expect(get_field(doc, "entry[0].type_word") == "0x0100ABCD", "type restored");
	expect(get_field(doc, "entry[0].sample[0].length") == "100", "still a sample");

	std::vector<uint8_t> wave(64, 0xCD);
	replace_wave(doc, wave, 0);
	expect(doc.entries[0].mpb && ru32(doc.bytes, doc.entries[0].mpb->wave + 12) == 64, "wave size");
	expect(doc.bytes[doc.entries[0].mpb->wave + 16] == 0xCD, "wave byte");
	expect(get_field(doc, "entry[0].sample[0].length") == "100", "params kept");
	expect(get_field(doc, "entry[0].mpb.unk") == "0xA5A5A5A5", "unk kept");
	expect(doc.entries[1].msb && doc.bytes[doc.entries[1].msb->head + 16] == 0x11, "msb followed the shift");

	set_field(doc, "entry[1].bank_word", "0xFFFFFFFF");
	expect(doc.entries[1].kind == EntryKind::Hang, "hang entry");
	expect(get_field(doc, "entry[0].sample[0].length") == "100", "earlier entry still reads");

	Document again = parse(make_bundle(), "synth.mlt");
	auto params = sample_params(true);
	params[0x20] = 0;
	params[0x21] = 0;
	params[0x22] = 0;
	params[0x23] = 7;
	replace_params(again, params, 0);
	expect(get_field(again, "entry[0].sample[0].length") == "7", "params replace");
	expect(again.bytes[again.entries[0].mpb->wave + 16] == 0xAB, "wave kept");

	Document grown = parse(make_bundle(), "synth.mlt");
	std::string wave_off = get_field(grown, "entry[0].sample[0].offset");
	add_region(grown, 0, 0, 0);
	expect(grown.entries[0].mpb->params.progs[0].layers[0].regions.size() == 2, "added region");
	expect(get_field(grown, "entry[0].prog[0].layer[0].region[0].key_lo") == "10", "range kept");
	expect(get_field(grown, "entry[0].prog[0].layer[0].region[0].key_hi") == "20", "range hi kept");
	expect(get_field(grown, "entry[0].prog[0].layer[0].region[1].key_lo") == "22", "new range shifted");
	expect(get_field(grown, "entry[0].prog[0].layer[0].region[1].key_hi") == "32", "new range hi shifted");
	expect(get_field(grown, "entry[0].sample[0].offset") == wave_off, "wave offset stayed");
	expect(get_field(grown, "entry[0].sample[0].length") == "100", "sample stayed");
	expect(get_field(grown, "entry[0].prog[0].layer[0].bend_up") == "2", "bend stayed");
	expect(grown.entries[1].msb && grown.bytes[grown.entries[1].msb->head + 16] == 0x11, "msb followed the region");

	std::vector<uint8_t> shifted(0x144, 0);
	put32(shifted, 0, (1u << 24) | 0x20);
	put32(shifted, 4, (1u << 24) | 0xC4);
	put32(shifted, 8, (1u << 24) | 0x70);
	put32(shifted, 0x20, 100);
	put32(shifted, 0x20 + 0x4C, 0x80);
	put32(shifted, 0x70, (1u << 24) | 0x74);
	put32(shifted, 0x74, (1u << 24) | 0x84);
	shifted[0x78] = 4;
	shifted[0x79] = 5;
	shifted[0x84 + 8] = 10;
	shifted[0x84 + 9] = 20;
	shifted[0xC4 + 3] = 9;
	Document mid = parse(make_mpb(shifted, 0xAB, 32), "shift.mpb");
	expect(mid.mpb && mid.mpb->params.curves.size() == 1, "curve before insert");
	add_region(mid, -1, 0, 0);
	expect(mid.mpb->params.progs[0].layers[0].regions.size() == 2, "inserted region");
	expect((ru32(mid.bytes, mid.mpb->params.base + 4) & 0xFFFFFFu) == 0x104, "curve offset moved");
	expect(mid.bytes[mid.mpb->params.base + 0x104 + 3] == 9, "curve byte moved");
	expect(get_field(mid, "sample[0].offset") == "0x00000080", "sample wave offset");
	expect(get_field(mid, "prog[0].layer[0].bend_up") == "4", "later bend");
	expect(get_field(mid, "prog[0].layer[0].region[0].key_lo") == "10", "earlier region");

	std::vector<uint8_t> empty(0x20, 0);
	put32(empty, 8, (1u << 24) | 0x0C);
	put32(empty, 0x0C, (1u << 24) | 0x10);
	empty[0x14] = 4;
	empty[0x15] = 5;
	Document blank = parse(make_mpb(empty, 0x11, 32), "empty.mpb");
	expect(blank.mpb->params.progs[0].layers[0].regions.empty(), "no regions yet");
	add_region(blank, -1, 0, 0);
	expect(blank.mpb->params.progs[0].layers[0].regions.size() == 1, "first region");
	expect(get_field(blank, "prog[0].layer[0].region[0].key_lo") == "60", "default lo");
	expect(get_field(blank, "prog[0].layer[0].region[0].key_hi") == "72", "default hi");
	expect(get_field(blank, "prog[0].layer[0].bend_down") == "5", "empty layer bend");

	Document trimmed = parse(make_bundle(), "synth.mlt");
	std::string kept_off = get_field(trimmed, "entry[0].sample[0].offset");
	add_region(trimmed, 0, 0, 0);
	delete_region(trimmed, 0, 0, 0, 1);
	expect(trimmed.entries[0].mpb->params.progs[0].layers[0].regions.size() == 1, "region removed");
	expect(get_field(trimmed, "entry[0].prog[0].layer[0].region[0].key_lo") == "10", "surviving range");
	expect(get_field(trimmed, "entry[0].prog[0].layer[0].region[0].key_hi") == "20", "surviving hi");
	expect(get_field(trimmed, "entry[0].sample[0].offset") == kept_off, "offset after delete");
	expect(trimmed.entries[1].msb && trimmed.bytes[trimmed.entries[1].msb->head + 16] == 0x11, "msb after delete");

	add_layer(trimmed, 0, 0);
	expect(trimmed.entries[0].mpb->params.progs[0].layers.size() == 2, "layer added");
	expect(trimmed.entries[0].mpb->params.progs[0].layers[1].regions.empty(), "new layer is empty");
	expect(get_field(trimmed, "entry[0].prog[0].layer[1].bend_up") == "2", "bend copied");
	expect(get_field(trimmed, "entry[0].prog[0].layer[0].region[0].key_lo") == "10", "old layer stayed");
	remove_layer(trimmed, 0, 0, 1);
	expect(trimmed.entries[0].mpb->params.progs[0].layers.size() == 1, "layer removed");
	expect(get_field(trimmed, "entry[0].prog[0].layer[0].region[0].key_hi") == "20", "region survived the layer");

	add_program(trimmed, 0);
	expect(trimmed.entries[0].mpb->params.progs.size() == 2, "program added");
	expect(trimmed.entries[0].mpb->params.progs[1].layers.empty(), "new program is empty");
	expect(get_field(trimmed, "entry[0].prog[0].layer[0].region[0].key_lo") == "10", "first program stayed");
	expect(get_field(trimmed, "entry[0].sample[0].length") == "100", "sample after program");

	Document cleared = parse(make_mpb(shifted, 0xAB, 32), "clear.mpb");
	delete_region(cleared, -1, 0, 0, 0);
	expect(cleared.mpb->params.progs[0].layers[0].regions.empty(), "only region removed");
	expect((ru32(cleared.bytes, cleared.mpb->params.base + 4) & 0xFFFFFFu) == 0x84, "curve followed the delete");
	expect(cleared.bytes[cleared.mpb->params.base + 0x84 + 3] == 9, "curve byte followed the delete");
	expect(get_field(cleared, "sample[0].offset") == "0x00000080", "wave offset after delete");
	remove_layer(cleared, -1, 0, 0);
	expect(cleared.mpb->params.progs[0].layers.empty(), "layer and its gap removed");
	expect(get_field(cleared, "vel[0][3]") == "9", "curve still readable");
	expect(get_field(cleared, "sample[0].length") == "100", "sample after layer delete");
	return 0;
}
