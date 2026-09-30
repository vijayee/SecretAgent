//
// Created by victor on 9/30/26.
//

#ifndef SA_STREAMS_MESSAGES_H
#define SA_STREAMS_MESSAGES_H

/* Message vocabulary of the ported streams/HTTP core, carried in the generic
   message_t envelope. Values mirror liboffs's shared catalog numbering (the
   gaps are other modules' entries there); per project style the module owns
   its message_type_e in its own header. */
typedef enum message_type_e {
  READABLE_PUSH = 26,
  READABLE_READ = 27,
  WRITEABLE_WRITE = 29,
  CLOSE_STREAM = 30,
  READABLE_PULL = 31,
  DEFERRED_DEREF = 32,
  STREAM_NOTIFY = 47,
  HTTP_CONNECTION_DATA = 61,
  HTTP_CONNECTION_READABLE = 62,
  HTTP_CONNECTION_HANGUP = 63,
  HTTP_CONNECTION_ERROR = 64,
  HTTP_CONNECTION_WRITE = 65,
  HTTP_CONNECTION_WRITABLE = 66,
  HTTP_CONNECTION_CLOSE = 67,
  HTTP_SERVER_UPDATE_WATCHER = 68,
  HTTP_SERVER_STOP_WATCHER = 69,
  STREAM_SUBSCRIBE = 76,
  STREAM_UNSUBSCRIBE = 77,
  STREAM_PIPE = 79,
  STREAM_PIPED = 80,
  STREAM_SET_PULLING = 82
} message_type_e;

#endif // SA_STREAMS_MESSAGES_H