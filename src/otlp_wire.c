#include "otlp_wire.h"
#include <string.h>

void otlp_wire_bytes(OtlpWire *b, const void *p, size_t n) {
  if (b->bad || b->n > b->cap || n > b->cap - b->n) { b->bad = 1; return; }
  if (n) memcpy(b->p + b->n, p, n);
  b->n += n;
}
void otlp_wire_var(OtlpWire *b, uint64_t v) {
  do { unsigned char c = (unsigned char)(v & 127); v >>= 7;
    if (v) c |= 128;
    otlp_wire_bytes(b, &c, 1);
  } while (v);
}
void otlp_wire_num(OtlpWire *b, unsigned f, uint64_t v) {
  otlp_wire_var(b, (uint64_t)f << 3); otlp_wire_var(b, v);
}
void otlp_wire_fixed(OtlpWire *b, unsigned f, uint64_t v) {
  otlp_wire_var(b, ((uint64_t)f << 3) | 1);
  for (int i = 0; i < 8; i++) {
    unsigned char c = v & 255; otlp_wire_bytes(b, &c, 1); v >>= 8;
  }
}
void otlp_wire_blob(OtlpWire *b, unsigned f, const void *p, size_t n) {
  otlp_wire_var(b, ((uint64_t)f << 3) | 2);
  otlp_wire_var(b, n); otlp_wire_bytes(b, p, n);
}
void otlp_wire_nested(OtlpWire *b, unsigned f, const OtlpWire *v) {
  if (v->bad) b->bad = 1; else otlp_wire_blob(b, f, v->p, v->n);
}

/* Bounded protobuf response reader; never exposes collector-provided strings. */
static int readvar(const unsigned char **p, const unsigned char *end, uint64_t *v) {
  *v = 0;
  for (unsigned shift = 0; shift < 64; shift += 7) {
    if (*p == end) return 0;
    unsigned c = *(*p)++;
    if (shift == 63 && c > 1) return 0;
    *v |= (uint64_t)(c & 127) << shift;
    if (!(c & 128)) return 1;
  }
  return 0;
}
static int response_fields(const unsigned char *p, size_t n, int partial,
                           uint64_t *rejected) {
  const unsigned char *end = p + n;
  while (p < end) {
    uint64_t tag, v;
    if (!readvar(&p, end, &tag) || !(tag >> 3)) return 0;
    unsigned wire = tag & 7;
    if (wire == 0) {
      if (!readvar(&p, end, &v)) return 0;
      if (partial && tag == 8) { if (v > INT64_MAX) return 0; *rejected = v; }
    } else if (wire == 2) {
      if (!readvar(&p, end, &v) || v > (uint64_t)(end - p)) return 0;
      if (!partial && tag == 10 && !response_fields(p, (size_t)v, 1, rejected)) return 0;
      p += (size_t)v;
    } else if (wire == 1 || wire == 5) {
      size_t k = wire == 1 ? 8 : 4;
      if ((size_t)(end - p) < k) return 0;
      p += k;
    } else return 0;
    if ((tag >> 3) == 1 && wire != (partial ? 0u : 2u)) return 0;
  }
  return 1;
}
int otlp_wire_response(const unsigned char *p, size_t n, uint64_t *rejected) {
  uint64_t value = 0;
  if (!rejected || (!p && n)) return 0;
  if (n && !response_fields(p, n, 0, &value)) return 0;
  *rejected = value;
  return 1;
}
