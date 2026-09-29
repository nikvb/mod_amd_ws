/*
 * local_amd.c -- see local_amd.h.  Algorithm: Asterisk app_amd via
 * seanbright/mod_amd (CC0 1.0).
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 1.1.  See LICENSE.
 */
#include "local_amd.h"

#include <stdlib.h>
#include <string.h>

void local_amd_defaults(local_amd_params_t *p)
{
	p->initial_silence = 2500;
	p->greeting = 1500;
	p->after_greeting_silence = 800;
	p->total_analysis_time = 5000;
	p->min_word_length = 100;
	p->between_words_silence = 50;
	p->maximum_number_of_words = 3;
	p->maximum_word_length = 5000;
	p->silence_threshold = 256;
}

void local_amd_init(local_amd_t *a, const local_amd_params_t *p)
{
	memset(a, 0, sizeof(*a));
	a->p = *p;
	a->in_word = 1;              /* mod_amd starts in VAD_STATE_IN_WORD */
	a->in_initial_silence = 1;
}

const char *local_amd_status_str(local_amd_verdict_t v)
{
	switch (v) {
	case LOCAL_AMD_HUMAN: return "HUMAN";
	case LOCAL_AMD_MACHINE: return "MACHINE";
	case LOCAL_AMD_NOTSURE: return "NOTSURE";
	default: return "PENDING";
	}
}

static local_amd_verdict_t decide(local_amd_t *a, local_amd_verdict_t v, const char *cause)
{
	a->verdict = v;
	a->cause = cause;
	a->decided_ms = a->total_ms;
	return v;
}

local_amd_verdict_t local_amd_feed(local_amd_t *a, const int16_t *s, uint32_t n)
{
	uint64_t energy = 0;
	uint32_t i, frame_ms, score;

	if (a->verdict != LOCAL_AMD_PENDING || n == 0) return a->verdict;

	frame_ms = n / 8;                    /* 8 samples per ms */
	if (frame_ms == 0) frame_ms = 1;
	a->total_ms += frame_ms;

	if (a->p.total_analysis_time && a->total_ms >= a->p.total_analysis_time) {
		return decide(a, LOCAL_AMD_NOTSURE, "TOOLONG");
	}

	for (i = 0; i < n; i++) energy += (uint64_t) abs((int) s[i]);
	score = (uint32_t) (energy / n);

	if (score < a->p.silence_threshold) {
		/* silence frame */
		a->silence_ms += frame_ms;
		if (a->silence_ms >= a->p.between_words_silence) {
			a->in_word = 0;
			a->voice_ms = 0;
		}
		if (a->in_initial_silence && a->silence_ms >= a->p.initial_silence) {
			return decide(a, LOCAL_AMD_MACHINE, "INITIALSILENCE");
		}
		if (a->in_greeting && a->silence_ms >= a->p.after_greeting_silence) {
			return decide(a, LOCAL_AMD_HUMAN, "HUMAN");
		}
		return LOCAL_AMD_PENDING;
	}

	/* voiced frame */
	a->voice_ms += frame_ms;
	if (a->voice_ms >= a->p.min_word_length && !a->in_word) {
		a->words++;
		a->in_word = 1;
	}
	if (a->voice_ms >= a->p.maximum_word_length) {
		return decide(a, LOCAL_AMD_MACHINE, "MAXWORDLENGTH");
	}
	if (a->words >= a->p.maximum_number_of_words) {
		return decide(a, LOCAL_AMD_MACHINE, "MAXWORDS");
	}
	if (a->in_greeting && a->voice_ms >= a->p.greeting) {
		return decide(a, LOCAL_AMD_MACHINE, "LONGGREETING");
	}
	if (a->voice_ms >= a->p.min_word_length) {
		a->silence_ms = 0;
		if (!a->in_greeting) {
			a->in_initial_silence = 0;
			a->in_greeting = 1;
		}
	}
	return LOCAL_AMD_PENDING;
}
