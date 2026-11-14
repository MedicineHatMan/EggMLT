#pragma once

/* EMALT edits the GCAX bank files this game loads.
 * MLT / MLTM: src/gcax/gcaxSndApi.c gcaxLoaderLoad.
 * MPB / MPBW / MPBP / MSB: src/gcax/gcaxSndBank.c gcaxSndLoadMpb, gcaxSndLoadMsb.
 * Programs, layers, regions, samples, velocity curves: include/gcax/gcax.h,
 * walked by gcaxSndStartVoices in src/gcax/gcaxSndVoice.c.
 * Multi-byte integers in the file are big-endian, as on the GameCube. */

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct Region {
	uint32_t off = 0;
};

struct Layer {
	uint32_t off = 0;
	std::vector<Region> regions;
};

struct Program {
	std::vector<Layer> layers;
};

struct Sample {
	uint32_t off = 0;
};

struct Curve {
	uint32_t off = 0;
};

struct Params {
	uint32_t base = 0;
	uint32_t size = 0;
	std::vector<Sample> samples;
	std::vector<Curve> curves;
	std::vector<Program> progs;
};

struct Mpb {
	uint32_t head = 0;
	uint32_t span = 0;

	bool has_wave = false;
	uint32_t wave = 0;
	bool has_param = false;
	uint32_t param = 0;

	Params params;
};

struct Msb {
	uint32_t head = 0;
	uint32_t span = 0;
};

enum class EntryKind { Hang, Mpb, Msb, Other };

struct Entry {
	uint32_t index = 0;
	uint32_t off = 0;
	bool has_pos = false;
	uint32_t bank_pos = 0;
	uint32_t bank_len = 0;
	EntryKind kind = EntryKind::Other;
	std::optional<Mpb> mpb;
	std::optional<Msb> msb;
};

enum class FileKind { None, Mlt, Mpb, Mpbp, Mpbw, Msb };

struct Document {
	std::vector<uint8_t> bytes;
	std::string name;
	FileKind kind = FileKind::None;
	bool ok = false;

	std::vector<Entry> entries;
	std::optional<Mpb> mpb;
	std::optional<Params> params;
	std::optional<Msb> msb;

	std::vector<std::string> warnings;
};

std::vector<uint8_t> read_file(const std::string& path);
Document load_file(const std::string& path);
Document parse(std::vector<uint8_t> bytes, const std::string& name);

std::string info_text(const Document& doc);
std::string dump_text(const Document& doc, bool full);
std::string get_field(const Document& doc, const std::string& path);
/* reparse walks the file again. Leave it false while dragging a slider; the bytes change and the
 * existing offsets stay valid. Pass true when the edit moves a table or a chunk. */
void set_field(Document& doc, const std::string& path, const std::string& value, bool reparse = true);
std::string peek_bytes(const Document& doc, uint32_t off, uint32_t count);
void poke_bytes(Document& doc, uint32_t off, const std::string& type, const std::string& value);

std::string extract_to(const Document& doc, const std::string& dir);
void replace_wave(Document& doc, std::vector<uint8_t> wave, int entry);
void replace_params(Document& doc, std::vector<uint8_t> params, int entry);
void replace_bank(Document& doc, int entry, std::vector<uint8_t> bank);

/* One sample's wave bytes plus the DSP header an encoder fills in. install_sample_wave
 * splices the bytes into the MPBW and writes the header. Coefficients and predictor
 * history stay out of the editor; they come from here. */
struct SampleWave {
	std::vector<uint8_t> data;
	uint32_t length = 0;
	uint32_t nibbles = 0;
	uint32_t rate = 0;
	uint16_t loop = 0;
	uint32_t loop_adr = 0;
	uint32_t end_adr = 0;
	uint32_t cur_adr = 0;

	int16_t coef[16] = {};
	uint16_t pred_scale = 0;
	int16_t yn1 = 0;
	int16_t yn2 = 0;
	uint16_t loop_pred_scale = 0;
	int16_t loop_yn1 = 0;
	int16_t loop_yn2 = 0;

	uint8_t format = 0;
};

void install_sample_wave(Document& doc, int entry, int sample, SampleWave wave);
/* These rewrite the parameter block and shift every later table offset. */
void add_region(Document& doc, int entry, int prog, int layer);
void delete_region(Document& doc, int entry, int prog, int layer, int region);
void add_layer(Document& doc, int entry, int prog);
void remove_layer(Document& doc, int entry, int prog, int layer);
void add_program(Document& doc, int entry);
void save_file(const Document& doc, const std::string& path);

int selftest();
