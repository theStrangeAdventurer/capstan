#ifndef CAPSTAN_OTLP_WIRE_H
#define CAPSTAN_OTLP_WIRE_H

#include <stddef.h>
#include <stdint.h>

/* Caller-owned bounded buffer. Overflow is sticky, including nested failures;
 * a bad buffer must never be exported. Initialize n and bad to zero. */
typedef struct { unsigned char *p; size_t n, cap; int bad; } OtlpWire;
void otlp_wire_bytes(OtlpWire *b, const void *p, size_t n);
void otlp_wire_var(OtlpWire *b, uint64_t v);
void otlp_wire_num(OtlpWire *b, unsigned f, uint64_t v);
void otlp_wire_fixed(OtlpWire *b, unsigned f, uint64_t v);
void otlp_wire_blob(OtlpWire *b, unsigned f, const void *p, size_t n);
void otlp_wire_nested(OtlpWire *b, unsigned f, const OtlpWire *v);
/* Parse an OTLP export response, ignoring collector text and unknown fields.
 * Empty responses succeed with zero rejections. Failure leaves output intact.
 * p may be NULL only when n is zero; rejected must be non-NULL. */
int otlp_wire_response(const unsigned char *p, size_t n, uint64_t *rejected);

#endif
