#ifndef HTTP_H
#define HTTP_H
#include <lua.h>
void http_init(lua_State *L);
int  http_poll(lua_State *L);
int  http_poll_limited(lua_State *L, int max_callbacks);
void http_cleanup(void);
int  http_is_loading(void);
int  http_has_background_work(void);
void http_set_headless(int headless);
int  http_cancel_streams(lua_State *L);
#ifdef CAPSTAN_HTTP_TESTING
enum {
  HTTP_TEST_FAIL_EASY_INIT = 1,
  HTTP_TEST_FAIL_REGISTRY,
  HTTP_TEST_FAIL_ADD_HANDLE
};
void http_test_fail_next(int failure);
int http_test_deliver_chunk(int async_id, const char *chunk);
#endif
#endif
