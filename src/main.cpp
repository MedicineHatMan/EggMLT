#include "emalt.hpp"
#include "audio.hpp"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int gui_main(const char* path);

static void attach_console()
{
	HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
	if (out != nullptr && out != INVALID_HANDLE_VALUE) return;

	if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;

	FILE* fp = nullptr;
	freopen_s(&fp, "CONOUT$", "w", stdout);
	freopen_s(&fp, "CONOUT$", "w", stderr);
	std::ios::sync_with_stdio();
}

static bool is_command(const std::string& cmd)
{
	return cmd == "info" || cmd == "dump" || cmd == "get" || cmd == "set" || cmd == "peek" || cmd == "poke" ||
	       cmd == "extract" || cmd == "wave-replace" || cmd == "params-replace" || cmd == "bank-replace" ||
	       cmd == "selftest" || cmd == "help" || cmd == "-h" || cmd == "--help";
}

namespace {

const char* kHelp =
	"EMALT - edit GCAX .mlt / .mpb banks\n"
	"\n"
	"  emalt                 open the editor\n"
	"  emalt FILE            open FILE in the editor\n"
	"\n"
	"Reads a bank the way the game does. gcaxLoaderLoad walks an MLT: a gcax MLT chunk,\n"
	"a gcax MLTM table, then 16-byte entries. Type 1 is an MPB and type 4 is an MSB; the\n"
	"slot and the type are the low nibble of the top byte of the first two words. The\n"
	"third word is the bank's offset from the end of the MLT header. gcaxSndLoadMpb wants\n"
	"an MPBW wave chunk and then an MPBP parameter chunk. gcaxSndStartVoices walks the\n"
	"parameter head: sample records, 128-byte velocity curves, and programs of layers of\n"
	"regions. The record layouts are GCAX_SAMPLE, GCAX_LAYER and GCAX_REGION in\n"
	"include/gcax/gcax.h. .mpb is the usual name; a .mbp with the same magic opens too.\n"
	"\n"
	"  emalt info FILE\n"
	"  emalt dump FILE [--full]\n"
	"  emalt get FILE PATH\n"
	"  emalt set FILE (-o OUT | --in-place) PATH VALUE [PATH VALUE ...]\n"
	"  emalt peek FILE OFFSET [COUNT]\n"
	"  emalt poke FILE (-o OUT | --in-place) OFFSET u8|u16|u32 VALUE\n"
	"  emalt extract FILE DIR\n"
	"  emalt wave-replace FILE (-o OUT | --in-place) WAVE [-e N]\n"
	"  emalt params-replace FILE (-o OUT | --in-place) PARAMS [-e N]\n"
	"  emalt bank-replace FILE (-o OUT | --in-place) ENTRY BANK\n"
	"  emalt selftest\n"
	"\n"
	"Field edits keep every other byte. Wave, parameter and bank replacement rebuild the\n"
	"chunk sizes and, inside an MLT, the later entries' offsets. --in-place keeps the\n"
	"previous bytes in FILE.bak the first time.\n"
	"\n"
	"Paths: entry[N] or slot[N], then sample[N], prog[N].layer[N].region[N], vel[N] or\n"
	"vel[N][byte], mpb, mpbw, mpbp, msb, mlt, mltm. On a file with one bank the entry[]\n"
	"prefix can be left off.\n"
	"\n"
	"Region fields: sample unk04 velcurve vel_lo vel_hi key_lo key_hi level pan root fine\n"
	"fx_invert fx_send atk dec sus sus_level rel rate_bias atten, and unk (41 bytes of hex).\n"
	"Layer fields: bend_up bend_down unk. Sample fields: length nibbles rate loop dsp_format\n"
	"loop_adr end_adr cur_adr coef[N] gain pred_scale yn1 yn2 loop_pred_scale loop_yn1\n"
	"loop_yn2 format pad offset. nibbles, rate and dsp_format are the DSP header slots in\n"
	"the sample record; the voice player does not read them. hdr04, hdr08 and hdr0e name\n"
	"those same bytes. Entry fields: type (mpb or msb), bank (slot 0-15), type_word,\n"
	"bank_word, offset, extra. Chunk fields: unk, size. Table words: sample_table,\n"
	"vel_table, prog_table (top byte is the count, low 24 bits are the offset).\n"
	"\n"
	"An MSB payload is the blob gcaxSndLoadMsb copies. The sequence player reads MIDI-style\n"
	"events from it, and this tool extracts, replaces, peeks and pokes those bytes.\n"
	"\n"
	"Sample format is the AX format gcaxVoiceStart switches on: 0 adpcm, 10 pcm16, 25 pcm8.\n"
	"Numbers accept a 0x prefix. A bank word of 0xFFFFFFFF is stored as written; the loader\n"
	"stays on that entry.\n";

struct Opt {
	std::string out;
	bool in_place = false;
	bool full = false;
	bool help = false;
	int entry = -1;
};

void need_arg(int i, int argc, const std::string& a)
{
	if (i + 1 >= argc) throw std::runtime_error("missing value after " + a);
}

std::vector<std::string> positionals(int argc, char** argv, int begin, Opt& opt)
{
	std::vector<std::string> pos;
	for (int i = begin; i < argc; i++) {
		std::string a = argv[i];
		if (a == "-h" || a == "--help") opt.help = true;
		else if (a == "-o" || a == "--out") {
			need_arg(i, argc, a);
			opt.out = argv[++i];
		} else if (a == "--in-place") opt.in_place = true;
		else if (a == "--full") opt.full = true;
		else if (a == "-e" || a == "--entry") {
			need_arg(i, argc, a);
			opt.entry = std::stoi(argv[++i]);
		} else if (!a.empty() && a[0] == '-') throw std::runtime_error("unknown option " + a);
		else pos.push_back(a);
	}

	return pos;
}

void commit(const Document& doc, const std::string& src, const Opt& opt)
{
	bool has_out = !opt.out.empty();
	if (opt.in_place == has_out) throw std::runtime_error("pass exactly one of -o FILE or --in-place");

	if (opt.in_place) {
		namespace fs = std::filesystem;
		fs::path bak = src;
		bak += ".bak";
		if (!fs::exists(bak)) {
			fs::copy_file(src, bak);
			std::cout << "backup " << bak.string() << "\n";
		}

		save_file(doc, src);
		std::cout << "wrote " << src << " (" << doc.bytes.size() << " bytes)\n";
	} else {
		save_file(doc, opt.out);
		std::cout << "wrote " << opt.out << " (" << doc.bytes.size() << " bytes)\n";
	}
}

} // namespace

int main(int argc, char** argv)
{
	try {
		if (argc < 2) return gui_main(nullptr);

		std::string cmd = argv[1];
		if (!is_command(cmd)) return gui_main(cmd.c_str());

		attach_console();
		if (cmd == "-h" || cmd == "--help" || cmd == "help") {
			std::fputs(kHelp, stdout);
			return 0;
		}

		if (cmd == "selftest") {
			selftest();
			audio_selftest();
			std::cout << "emalt selftest ok\n";
			return 0;
		}

		Opt opt;
		auto args = positionals(argc, argv, 2, opt);
		if (opt.help) {
			std::fputs(kHelp, stdout);
			return 0;
		}

		if (cmd == "info" || cmd == "dump") {
			if (args.size() != 1) throw std::runtime_error(cmd + " FILE");

			Document doc = load_file(args[0]);
			std::cout << (cmd == "info" ? info_text(doc) : dump_text(doc, opt.full));
			return 0;
		}

		if (cmd == "get") {
			if (args.size() != 2) throw std::runtime_error("get FILE PATH");

			Document doc = load_file(args[0]);
			std::cout << get_field(doc, args[1]) << "\n";
			return 0;
		}

		if (cmd == "set") {
			if (args.size() < 3 || (args.size() % 2) == 0) {
				throw std::runtime_error("set FILE (-o OUT | --in-place) PATH VALUE [PATH VALUE ...]");
			}

			Document doc = load_file(args[0]);
			for (size_t i = 1; i + 1 < args.size(); i += 2) set_field(doc, args[i], args[i + 1]);
			commit(doc, args[0], opt);
			return 0;
		}

		if (cmd == "peek") {
			if (args.size() < 2 || args.size() > 3) throw std::runtime_error("peek FILE OFFSET [COUNT]");

			Document doc = load_file(args[0]);
			uint32_t off = uint32_t(std::stoul(args[1], nullptr, 0));
			uint32_t n = args.size() == 3 ? uint32_t(std::stoul(args[2], nullptr, 0)) : 64;
			std::cout << peek_bytes(doc, off, n);
			return 0;
		}

		if (cmd == "poke") {
			if (args.size() != 4) throw std::runtime_error("poke FILE (-o OUT | --in-place) OFFSET u8|u16|u32 VALUE");

			Document doc = load_file(args[0]);
			poke_bytes(doc, uint32_t(std::stoul(args[1], nullptr, 0)), args[2], args[3]);
			commit(doc, args[0], opt);
			return 0;
		}

		if (cmd == "extract") {
			if (args.size() != 2) throw std::runtime_error("extract FILE DIR");

			Document doc = load_file(args[0]);
			std::cout << extract_to(doc, args[1]);
			return 0;
		}

		if (cmd == "wave-replace") {
			if (args.size() != 2) throw std::runtime_error("wave-replace FILE (-o OUT | --in-place) WAVE [-e N]");

			Document doc = load_file(args[0]);
			replace_wave(doc, read_file(args[1]), opt.entry);
			commit(doc, args[0], opt);
			return 0;
		}

		if (cmd == "params-replace") {
			if (args.size() != 2) throw std::runtime_error("params-replace FILE (-o OUT | --in-place) PARAMS [-e N]");

			Document doc = load_file(args[0]);
			replace_params(doc, read_file(args[1]), opt.entry);
			commit(doc, args[0], opt);
			return 0;
		}

		if (cmd == "bank-replace") {
			if (args.size() != 3) throw std::runtime_error("bank-replace FILE (-o OUT | --in-place) ENTRY BANK");

			Document doc = load_file(args[0]);
			replace_bank(doc, std::stoi(args[1]), read_file(args[2]));
			commit(doc, args[0], opt);
			return 0;
		}

		throw std::runtime_error("unknown command " + cmd + " (try emalt --help)");
	} catch (const std::exception& ex) {
		std::cerr << "emalt: " << ex.what() << "\n";
		return 1;
	}
}
