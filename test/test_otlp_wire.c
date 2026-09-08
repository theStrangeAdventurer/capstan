#include "munit.h"
#include "otlp_wire.h"

static MunitResult encoding(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  unsigned char buffer[64], inner[8];
  OtlpWire b={buffer,0,sizeof(buffer),0}, child={inner,0,sizeof(inner),0};
  otlp_wire_num(&b,1,150);
  otlp_wire_fixed(&b,2,UINT64_C(0x0807060504030201));
  otlp_wire_blob(&b,3,"hi",2);
  otlp_wire_num(&child,1,1); otlp_wire_nested(&b,4,&child);
  const unsigned char expected[]={0x08,0x96,0x01,0x11,1,2,3,4,5,6,7,8,
                                  0x1a,2,'h','i',0x22,2,8,1};
  munit_assert_false(b.bad);
  munit_assert_size(b.n,==,sizeof(expected));
  munit_assert_memory_equal(b.n,buffer,expected);
  b.n=0; otlp_wire_var(&b,UINT64_MAX);
  const unsigned char maximum[]={255,255,255,255,255,255,255,255,255,1};
  munit_assert_size(b.n,==,sizeof(maximum));
  munit_assert_memory_equal(b.n,buffer,maximum);
  return MUNIT_OK;
}
static MunitResult overflow(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  unsigned char buffer[]={0xaa,0xbb,0xcc};
  OtlpWire b={buffer,0,2,0};
  otlp_wire_bytes(&b,"ab",2);
  munit_assert_false(b.bad);
  otlp_wire_bytes(&b,"c",1);
  munit_assert_true(b.bad); munit_assert_size(b.n,==,2);
  otlp_wire_num(&b,1,1);
  munit_assert_size(b.n,==,2); munit_assert_uint(buffer[2],==,0xcc);
  b.n=3; b.bad=0; otlp_wire_bytes(&b,"x",1);
  munit_assert_true(b.bad); munit_assert_uint(buffer[2],==,0xcc);
  return MUNIT_OK;
}
static MunitResult nested_overflow(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  unsigned char outer[8], middle[4], inner[1];
  OtlpWire a={outer,0,sizeof(outer),0}, b={middle,0,sizeof(middle),0},
           c={inner,0,sizeof(inner),0};
  otlp_wire_num(&c,1,150); otlp_wire_nested(&b,1,&c); otlp_wire_nested(&a,1,&b);
  munit_assert_true(c.bad); munit_assert_true(b.bad); munit_assert_true(a.bad);
  munit_assert_size(a.n,==,0);
  c.n=0; c.bad=0; otlp_wire_var(&c,1);
  a.n=0; a.cap=2; a.bad=0; otlp_wire_nested(&a,1,&c);
  munit_assert_true(a.bad); munit_assert_false(c.bad);
  return MUNIT_OK;
}
static MunitResult response(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  uint64_t rejected=99;
  munit_assert_true(otlp_wire_response(NULL,0,&rejected));
  munit_assert_uint64(rejected,==,0);
  const unsigned char partial[]={10,8,8,3,18,4,'o','o','p','s',16,1};
  munit_assert_true(otlp_wire_response(partial,sizeof(partial),&rejected));
  munit_assert_uint64(rejected,==,3);
  const unsigned char empty[]={10,0};
  munit_assert_true(otlp_wire_response(empty,sizeof(empty),&rejected));
  munit_assert_uint64(rejected,==,0);
  return MUNIT_OK;
}
static MunitResult malformed(const MunitParameter params[], void *data) {
  (void)params; (void)data;
  const unsigned char cases[][16]={
    {0x80}, {10,0x80}, {10,2,8,0x80},
    {16,255,255,255,255,255,255,255,255,255,2},
    {16,128,128,128,128,128,128,128,128,128,128,0},
    {10,11,8,255,255,255,255,255,255,255,255,255,1},
    {10,3,8,1}, {0}, {8,0}, {10,2,10,0}, {19}, {17,1},
    {10,2,8,3,0x80}
  };
  const size_t sizes[]={1,2,4,11,12,13,4,1,2,4,1,2,5};
  for (size_t i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++) {
    uint64_t rejected=42;
    munit_assert_false(otlp_wire_response(cases[i],sizes[i],&rejected));
    munit_assert_uint64(rejected,==,42);
  }
  return MUNIT_OK;
}
static MunitTest tests[]={
  {"/encoding",encoding,NULL,NULL,MUNIT_TEST_OPTION_NONE,NULL},
  {"/overflow",overflow,NULL,NULL,MUNIT_TEST_OPTION_NONE,NULL},
  {"/nested-overflow",nested_overflow,NULL,NULL,MUNIT_TEST_OPTION_NONE,NULL},
  {"/response",response,NULL,NULL,MUNIT_TEST_OPTION_NONE,NULL},
  {"/malformed",malformed,NULL,NULL,MUNIT_TEST_OPTION_NONE,NULL},
  {NULL,NULL,NULL,NULL,MUNIT_TEST_OPTION_NONE,NULL}
};
MunitSuite otlp_wire_suite={"/otlp_wire",tests,NULL,1,MUNIT_SUITE_OPTION_NONE};
