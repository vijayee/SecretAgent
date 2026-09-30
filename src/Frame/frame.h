//
// Created by victor on 9/29/26.
//

#ifndef SA_FRAME_H
#define SA_FRAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef SA_HAS_WDB
#include "../Actor/actor.h"

typedef struct wave_database_root_t wave_database_root_t;   /* opaque; owns ONE root db */

/* Config (immutable after create): */
typedef struct frame_config_t {
  const char* model_base_url;    /* e.g. http://127.0.0.1:11434 (Ollama) */
  const char* model_api_key;     /* may be NULL/empty for Ollama */
  const char* model_name;        /* e.g. a local model tag */
  unsigned max_depth;            /* SA_FRAME_MAX_DEPTH equivalent (default 4) */
} frame_config_t;

wave_database_root_t* wave_db_open(const char* location /* NULL = in-memory */);
void wave_db_close(wave_database_root_t* db);

typedef struct frame_t frame_t;

/* parent NULL = top-level session (sid generated); goal may be NULL. */
frame_t* frame_create(wave_database_root_t* db, frame_t* parent,
                      const char* goal, const frame_config_t* cfg);
const char* frame_sid(const frame_t* f);            /* full subtree path, e.g. sessions/<sid> */
uint8_t frame_is_done(const frame_t* f);
void frame_destroy(frame_t* f);

/* Store operations (called by frame behaviors; ONE root batch per effect). */
int frame_remember_local(frame_t* f, const char* key, const char* json_value);   /* state/local/<key> */
int frame_remember_ctx(frame_t* f, const char* key, const char* json_value);     /* state/ctx/<key> */
/* Resolve: own local/ first, then ctx/ up the lineage chain (shadowing). */
char* frame_recall(frame_t* f, const char* key);        /* malloc'd JSON text; NULL if unresolvable */
int frame_append_msg(frame_t* f, const char* role, const char* content);   /* msg.append event */

#endif /* SA_HAS_WDB */

#endif // SA_FRAME_H