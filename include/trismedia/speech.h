/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2006, Digium, Inc.
 *
 * Joshua Colp <jcolp@digium.com>
 *
 * See http://www.trismedia.org for more information about
 * the Trismedia project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 * \brief Generic Speech Recognition API
 */

#ifndef _TRISMEDIA_SPEECH_H
#define _TRISMEDIA_SPEECH_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

/* Speech structure flags */
enum tris_speech_flags {
	TRIS_SPEECH_QUIET = (1 << 0),        /* Quiet down output... they are talking */
	TRIS_SPEECH_SPOKE = (1 << 1),        /* Speaker spoke! */
	TRIS_SPEECH_HAVE_RESULTS = (1 << 2), /* Results are present */
};

/* Speech structure states - in order of expected change */
enum tris_speech_states {
	TRIS_SPEECH_STATE_NOT_READY = 0, /* Not ready to accept audio */
	TRIS_SPEECH_STATE_READY, /* Accepting audio */
	TRIS_SPEECH_STATE_WAIT, /* Wait for results to become available */
	TRIS_SPEECH_STATE_DONE, /* Processing is all done */
};

enum tris_speech_results_type {
	TRIS_SPEECH_RESULTS_TYPE_NORMAL = 0,
	TRIS_SPEECH_RESULTS_TYPE_NBEST,
};

/* Speech structure */
struct tris_speech {
	/*! Structure lock */
	tris_mutex_t lock;
	/*! Set flags */
	unsigned int flags;
	/*! Processing sound (used when engine is processing audio and getting results) */
	char *processing_sound;
	/*! Current state of structure */
	int state;
	/*! Expected write format */
	int format;
	/*! Data for speech engine */
	void *data;
	/*! Cached results */
	struct tris_speech_result *results;
	/*! Type of results we want */
	enum tris_speech_results_type results_type;
	/*! Pointer to the engine used by this speech structure */
	struct tris_speech_engine *engine;
};
  
/* Speech recognition engine structure */
struct tris_speech_engine {
	/*! Name of speech engine */
	char *name;
	/*! Set up the speech structure within the engine */
	int (*create)(struct tris_speech *speech, int format);
	/*! Destroy any data set on the speech structure by the engine */
	int (*destroy)(struct tris_speech *speech);
	/*! Load a local grammar on the speech structure */
	int (*load)(struct tris_speech *speech, char *grammar_name, char *grammar);
	/*! Unload a local grammar */
	int (*unload)(struct tris_speech *speech, char *grammar_name);
	/*! Activate a loaded grammar */
	int (*activate)(struct tris_speech *speech, char *grammar_name);
	/*! Deactivate a loaded grammar */
	int (*deactivate)(struct tris_speech *speech, char *grammar_name);
	/*! Write audio to the speech engine */
	int (*write)(struct tris_speech *speech, void *data, int len);
	/*! Signal DTMF was received */
	int (*dtmf)(struct tris_speech *speech, const char *dtmf);
	/*! Prepare engine to accept audio */
	int (*start)(struct tris_speech *speech);
	/*! Change an engine specific setting */
	int (*change)(struct tris_speech *speech, char *name, const char *value);
	/*! Change the type of results we want back */
	int (*change_results_type)(struct tris_speech *speech, enum tris_speech_results_type results_type);
	/*! Try to get results */
	struct tris_speech_result *(*get)(struct tris_speech *speech);
	/*! Accepted formats by the engine */
	int formats;
	TRIS_LIST_ENTRY(tris_speech_engine) list;
};

/* Result structure */
struct tris_speech_result {
	/*! Recognized text */
	char *text;
	/*! Result score */
	int score;
	/*! NBest Alternative number if in NBest results type */
	int nbest_num;
	/*! Matched grammar */
	char *grammar;
	/*! List information */
	TRIS_LIST_ENTRY(tris_speech_result) list;
};

/*! \brief Activate a grammar on a speech structure */
int tris_speech_grammar_activate(struct tris_speech *speech, char *grammar_name);
/*! \brief Deactivate a grammar on a speech structure */
int tris_speech_grammar_deactivate(struct tris_speech *speech, char *grammar_name);
/*! \brief Load a grammar on a speech structure (not globally) */
int tris_speech_grammar_load(struct tris_speech *speech, char *grammar_name, char *grammar);
/*! \brief Unload a grammar */
int tris_speech_grammar_unload(struct tris_speech *speech, char *grammar_name);
/*! \brief Get speech recognition results */
struct tris_speech_result *tris_speech_results_get(struct tris_speech *speech);
/*! \brief Free a set of results */
int tris_speech_results_free(struct tris_speech_result *result);
/*! \brief Indicate to the speech engine that audio is now going to start being written */
void tris_speech_start(struct tris_speech *speech);
/*! \brief Create a new speech structure */
struct tris_speech *tris_speech_new(char *engine_name, int formats);
/*! \brief Destroy a speech structure */
int tris_speech_destroy(struct tris_speech *speech);
/*! \brief Write audio to the speech engine */
int tris_speech_write(struct tris_speech *speech, void *data, int len);
/*! \brief Signal to the engine that DTMF was received */
int tris_speech_dtmf(struct tris_speech *speech, const char *dtmf);
/*! \brief Change an engine specific attribute */
int tris_speech_change(struct tris_speech *speech, char *name, const char *value);
/*! \brief Change the type of results we want */
int tris_speech_change_results_type(struct tris_speech *speech, enum tris_speech_results_type results_type);
/*! \brief Change state of a speech structure */
int tris_speech_change_state(struct tris_speech *speech, int state);
/*! \brief Register a speech recognition engine */
int tris_speech_register(struct tris_speech_engine *engine);
/*! \brief Unregister a speech recognition engine */
int tris_speech_unregister(char *engine_name);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _TRISMEDIA_SPEECH_H */
