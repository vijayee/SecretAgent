//
// Created by victor on 9/29/26.
//

#ifndef SA_JSON_H
#define SA_JSON_H

#include <stddef.h>
#include <stdint.h>

/* Minimal JSON codec for event records. Values are one JSON document.
   Escapes supported: \" \\ \/ \b \f \n \r \t \uXXXX (BMP). */
typedef struct json_value_t json_value_t;

/* Types */
typedef enum json_type_e {
  JSON_NULL = 0, JSON_BOOL, JSON_INT, JSON_DOUBLE, JSON_STRING, JSON_OBJECT, JSON_ARRAY
} json_type_e;

/* Parse: returns NULL on malformed input; *error_msg (optional) gets a
   one-line reason (heap-owned, free() it). */
json_value_t* json_parse(const char* text, size_t len, char** error_msg);

/* Serialize one value into a freshly malloc'd NUL-terminated string. */
char* json_serialize(const json_value_t* value);

/* Accessors (borrowed): */
json_type_e json_type(const json_value_t* v);
int64_t     json_as_int(const json_value_t* v);
double      json_as_double(const json_value_t* v);
int         json_as_bool(const json_value_t* v);
const char* json_as_string(const json_value_t* v);
size_t      json_size(const json_value_t* v);            /* object/array length */
json_value_t* json_get(const json_value_t* obj, const char* key);   /* NULL if absent */
json_value_t* json_at(const json_value_t* array, size_t index);

/* Construction (heap-owned values; json_value_destroy frees recursively): */
json_value_t* json_new_null(void);
json_value_t* json_new_bool(int b);
json_value_t* json_new_int(int64_t i);
json_value_t* json_new_double(double d);
json_value_t* json_new_string(const char* s);            /* copies */
json_value_t* json_new_object(void);
json_value_t* json_new_array(void);
int json_object_set(json_value_t* obj, const char* key, json_value_t* value);   /* takes value */
int json_array_append(json_value_t* array, json_value_t* value);                /* takes value */

void json_value_destroy(json_value_t* v);

#endif // SA_JSON_H