#pragma once

#include <cstdint>
#include <string>
#include <vector>

/* Host playback and the GameCube DSP-ADPCM codec the voice player consumes.
 * gcaxVoiceStart reads coef / yn / loop history and never asks the user for them,
 * so import writes that header and the editor leaves it alone. */

struct EnvView {
	float atk_ms = 0;
	float dec_ms = 0;
	float sus_ms = 0;
	float rel_ms = 0;
	float sus_env = 1;
};

struct DecodedSample {
	std::vector<int16_t> pcm;
	int loop_from = 0;
	int loop_to = 0;
	bool loop = false;
};

struct EncodedAdpcm {
	std::vector<uint8_t> data;
	uint32_t length = 0;
	uint32_t nibbles = 0;
	uint32_t rate = 44100;
	uint16_t loop = 0;
	uint32_t loop_adr = 2;
	uint32_t end_adr = 2;
	uint32_t cur_adr = 2;

	int16_t coef[16] = {};
	uint16_t pred_scale = 0;
	int16_t yn1 = 0;
	int16_t yn2 = 0;
	uint16_t loop_pred_scale = 0;
	int16_t loop_yn1 = 0;
	int16_t loop_yn2 = 0;
};

struct Note {
	std::vector<int16_t> pcm;
	int loop_from = 0;
	int loop_to = 0;
	bool loop = false;
	double step = 1;
	int key = 60;

	bool shaped = true;
	float atk_rate = 1;
	float dec_rate = 0;
	float sus_rate = 0;
	float rel_rate = 0;
	float sus_db = 0;
	float level = 1;
};

EnvView env_view(int rate_bias, int root, int fine, int key, int atk, int dec, int sus, int sus_level, int rel);
int env_time_to_byte(bool attack, int rate_bias, int root, int fine, int key, float ms);
int env_level_to_byte(float sus_env);
void env_rates(const EnvView& view, float& atk_rate, float& dec_rate, float& sus_rate, float& rel_rate, float& sus_db);

bool decode_gcax_sample(const uint8_t* sample, const uint8_t* wave, size_t wave_n, DecodedSample& out, std::string& error);
bool encode_wav(const uint8_t* wav, size_t size, int loop_sample, EncodedAdpcm& out, std::string& error);

void audio_init();
void audio_shutdown();
bool audio_play(Note note, std::string& error);
void audio_release();
void audio_stop();
int audio_key();

void audio_selftest();
