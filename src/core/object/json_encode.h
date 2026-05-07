// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// json_encode.h
// Minimal JSON builder used by object-model methods that return map-shaped
// results (e.g. rom.identify, machine.profile) packed inside a V_STRING.
//
// Lifetime: the builder owns a growable buffer.  json_finish hands the
// completed JSON document over as a heap-owned V_STRING; the builder is
// consumed and must not be reused.  When value_t grows a V_MAP kind, this
// helper can be retired and the per-method bodies switch to direct map
// construction.

#ifndef GS_OBJECT_JSON_ENCODE_H
#define GS_OBJECT_JSON_ENCODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "value.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque builder state.  Created via json_builder_new, consumed by json_finish.
typedef struct json_builder json_builder_t;

// Allocate a fresh builder.  Returns NULL on OOM.
json_builder_t *json_builder_new(void);

// Open / close a JSON object.  Nest freely; debug builds assert balance.
void json_open_obj(json_builder_t *b);
void json_close_obj(json_builder_t *b);

// Open / close a JSON array.
void json_open_arr(json_builder_t *b);
void json_close_arr(json_builder_t *b);

// Emit a key.  The next value call (json_str / json_int / json_bool / json_null
// / json_open_obj / json_open_arr) becomes its value.
void json_key(json_builder_t *b, const char *key);

// Emit primitive values.  Strings are escaped per RFC 8259.
void json_str(json_builder_t *b, const char *value);
void json_int(json_builder_t *b, int64_t value);
void json_bool(json_builder_t *b, bool value);
void json_null(json_builder_t *b);

// Finalise the document.  Returns a heap-owned V_STRING holding the JSON
// payload; on OOM or imbalanced opens/closes, returns V_ERROR.  The builder
// is freed by this call regardless of outcome.
value_t json_finish(json_builder_t *b);

#ifdef __cplusplus
}
#endif

#endif // GS_OBJECT_JSON_ENCODE_H
