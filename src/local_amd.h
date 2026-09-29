/*
 * local_amd.h -- the energy/word-count answering machine detector of
 * Asterisk's app_amd as ported to FreeSWITCH by mod_amd
 * (github.com/seanbright/mod_amd, CC0 1.0), rewritten as a pure C state
 * machine over 8 kHz 16-bit mono frames so it can run inside mod_amd_ws
 * next to the streaming detector (modes "local", "compare", fallback).
 *
 * Differences from mod_amd 1:1 behaviour, all bug fixes:
 *  - the analysis-time limit is measured in milliseconds of audio seen, and
 *    is honoured when total_analysis_time is 0 (mod_amd read an
 *    uninitialised counter in that case);
 *  - frame length is taken from the frame's sample count on every frame.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 1.1.  See LICENSE.
 */
#ifndef MOD_AMD_WS_LOCAL_AMD_H
#define MOD_AMD_WS_LOCAL_AMD_H

#include <stdint.h>

typedef struct {
	uint32_t initial_silence;          /* 2500 */
	uint32_t greeting;                 /* 1500 */
	uint32_t after_greeting_silence;   /*  800 */
	uint32_t total_analysis_time;      /* 5000 */
	uint32_t min_word_length;          /*  100 */
	uint32_t between_words_silence;    /*   50 */
	uint32_t maximum_number_of_words;  /*    3 */
	uint32_t maximum_word_length;      /* 5000 */
	uint32_t silence_threshold;        /*  256 */
} local_amd_params_t;

typedef enum {
	LOCAL_AMD_PENDING = 0,
	LOCAL_AMD_HUMAN,
	LOCAL_AMD_MACHINE,
	LOCAL_AMD_NOTSURE
} local_amd_verdict_t;

typedef struct {
	local_amd_params_t p;
	int in_word;             /* VAD state: 1 = in word, 0 = in silence */
	uint32_t silence_ms;
	uint32_t voice_ms;
	uint32_t words;
	uint32_t total_ms;       /* audio analysed */
	int in_initial_silence;
	int in_greeting;
	local_amd_verdict_t verdict;
	const char *cause;       /* HUMAN INITIALSILENCE MAXWORDLENGTH MAXWORDS LONGGREETING TOOLONG */
	uint32_t decided_ms;     /* total_ms at the decision */
} local_amd_t;

void local_amd_defaults(local_amd_params_t *p);
void local_amd_init(local_amd_t *a, const local_amd_params_t *p);

/* Feed one frame of 8 kHz slin.  Returns the verdict (PENDING until decided;
 * once decided further frames are ignored). */
local_amd_verdict_t local_amd_feed(local_amd_t *a, const int16_t *samples, uint32_t nsamples);

const char *local_amd_status_str(local_amd_verdict_t v);

#endif
