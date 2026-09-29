//
// Created by victor on 9/29/26.
//

#ifndef SA_MESSAGE_H
#define SA_MESSAGE_H

#include <stdint.h>

/* Generic message. `type` semantics are application-defined (each module
   defines its own message_type_e in its own header, per the project style
   guide).
   Payload ownership transfers with the message; payload_destroy frees it. */
typedef struct message_t {
  uint32_t type;
  void* payload;
  void (*payload_destroy)(void*);
} message_t;

#endif // SA_MESSAGE_H