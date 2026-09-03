/* json.h — thin yyjson helpers so call sites stay short. */
#ifndef TNY_JSON_H
#define TNY_JSON_H

#include <stdbool.h>
#include <stdint.h>
#include "yyjson.h"
#include "util/util.h"

/* Read helpers (immutable docs). All tolerate NULL nodes. */
const char *jget_str(yyjson_val *obj, const char *key); /* NULL if absent */
const char *jget_strn(yyjson_val *obj, const char *key, size_t *len_out);
int64_t jget_int(yyjson_val *obj, const char *key, int64_t dflt);
double jget_num(yyjson_val *obj, const char *key, double dflt);
bool jget_bool(yyjson_val *obj, const char *key, bool dflt);
yyjson_val *jget(yyjson_val *obj, const char *key);

/* The shared allocator lets libtny observe yyjson exhaustion in the same
 * public-call scope as ordinary C allocations. */
const yyjson_alc *jallocator(void);

/* Parse a whole document; caller must yyjson_doc_free. NULL on error. */
yyjson_doc *jparse(const char *data, size_t len);
/* Parse file. */
yyjson_doc *jparse_file(const char *path);

/* Serialize a mutable doc to a malloc'd string (compact). */
char *jwrite(yyjson_mut_doc *doc);
char *jwrite_pretty(yyjson_mut_doc *doc);
/* Serialize any immutable value. */
char *jwrite_val(yyjson_val *val);
char *jwrite_mut_val(yyjson_mut_val *val);

/* Decode the payload segment of a JWT (base64url JSON) without verifying
 * the signature: for display/claim lookups on tokens that arrived over a
 * direct TLS channel only. Caller frees; NULL on malformed input. */
yyjson_doc *jwt_payload_doc(const char *jwt);

/* Append a JSON string escape of s into b (with surrounding quotes). */
void jescape(buf_t *b, const char *s);

#endif
