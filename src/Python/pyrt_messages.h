//
// Created by victor on 9/29/26.
//

#ifndef SA_PYRT_MESSAGES_H
#define SA_PYRT_MESSAGES_H

#include <stdint.h>

/* Module-defined message types for the Python execution layer, carried in
   the generic message_t envelope (uint32_t type). Messages are transient
   control; only effects (WaveDB, later slice) persist. */
typedef enum pyrt_message_type_e {
  PYRT_EXECUTE = 0,   /* actor -> pyrt thread: run this code cell */
  PYRT_RESULT,        /* pyrt thread -> owning actor: cell finished */
  PYRT_LOG,           /* python -> owning actor: live narration */
  PYRT_STATUS,        /* python -> owning actor: current status */
  PYRT_EMIT,          /* python -> owning actor: durable-payload candidate */
  PYRT_INTERRUPT      /* actor -> pyrt backend: stop the running cell */
} pyrt_message_type_e;

/* EXECUTE. Ownership of `code` transfers with the message. `corr` is the
   request correlation id echoed on the RESULT. */
typedef struct pyrt_execute_payload_t {
  uint64_t corr;
  char* code;
} pyrt_execute_payload_t;

void pyrt_execute_payload_destroy(void* payload);

/* RESULT. status 0 = ok (text = repr of the trailing expression; empty when
   None); status 1 = error (text = traceback / exit text). One per EXECUTE. */
typedef struct pyrt_result_payload_t {
  uint64_t corr;
  uint8_t status;
  char* text;
} pyrt_result_payload_t;

void pyrt_result_payload_destroy(void* payload);

/* LOG / STATUS / EMIT. Ownership of `text` transfers with the message. */
typedef struct pyrt_text_payload_t {
  char* text;
} pyrt_text_payload_t;

void pyrt_text_payload_destroy(void* payload);

#endif // SA_PYRT_MESSAGES_H