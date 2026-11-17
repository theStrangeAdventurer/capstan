#include "oauth_native.h"
#include "munit.h"
#include <lauxlib.h>
#include <lualib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

static void run(lua_State *L, const char *code) {
    if (luaL_dostring(L, code) != LUA_OK) munit_errorf("Lua: %s", lua_tostring(L, -1));
}
static lua_State *state(void) {
    lua_State *L = luaL_newstate(); munit_assert_not_null(L);
    luaL_openlibs(L); run(L, "capstan={preserved=true}"); oauth_native_init(L);
    run(L, "assert(capstan.preserved); o=capstan.oauth"); return L;
}
static MunitResult crypto(const MunitParameter params[], void *data) {
    (void)params; (void)data; lua_State *L = state();
    run(L, "assert(o.pkce_s256('dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk') == 'E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM'); "
        "assert(o.base64url_decode('AA') == string.char(0)); assert(o.base64url_decode('') == ''); "
        "for _,v in ipairs({'AB','A','AA=','+w','AA\\0A'}) do local v,e=o.base64url_decode(v); assert(v==nil and type(e)=='string') end; "
        "assert(o.pkce_s256('short')==nil); assert(o.pkce_s256({})==nil); "
        "for _,v in ipairs({0,-1,16385,1.5,'32',false}) do local r,e=o.random_base64url(v); assert(r==nil and type(e)=='string') end; "
        "local r=o.random_base64url(32); assert(#r==43 and #o.base64url_decode(r)==32); "
        "assert(o.verify_rs256('bad','bad','bad','AQAB')==false); assert(o.verify_rs256({})==false)");
    run(L,
        "local input='eyJhbGciOiJSUzI1NiJ9.eyJpc3MiOiJqb2UiLA0KICJleHAiOjEzMDA4MTkzODAsDQogImh0dHA6Ly9leGFtcGxlLmNvbS9pc19yb290Ijp0cnVlfQ'; "
        "local n='ofgWCuLjybRlzo0tZWJjNiuSfb4p4fAkd_wWJcyQoTbji9k0l8W26mPddxHmfHQp-Vaw-4qPCJrcS2mJPMEzP1Pt0Bm4d4QlL-yRT-SFd2lZS-pCgNMsD1W_YpRPEwOWvG6b32690r2jZ47soMZo9wGzjb_7OMg0LOL-bSf63kpaSHSXndS5z5rexMdbBYUsLA9e-KXBdQOS-UTo7WTBEMa2R2CapHg665xsmtdVMTBQY4uDZlxvb3qCo5ZwKh9kG4LT6_I5IhlJH7aGhyxXFvUK-DWNmoudF8NAco9_h9iaGNj8q2ethFkMLs91kzk2PAcDTW9gb54h4FRWyuXpoQ'; "
        "local sig='cC4hiUPoj9Eetdgtv3hF80EGrhuB__dzERat0XF9g2VtQgr9PJbu3XOiZj5RZmh7AAuHIm4Bh-0Qc_lF5YKt_O8W2Fp5jujGbds9uJdbF9CUAr7t1dnZcAcQjbKBYNX4BAynRFdiuB--f_nZLgrnbyTyWzO75vRK5h6xBArLIARNPvkSjtQBMHlb1L07Qe7K0GarZRmB_eSN9383LcOLn6_dO--xi12jzDwusC-eOkHWEsqtFZESc6BfI7noOPqvhJ1phCnvWh6IeYI2w9QOYEUipUTI8np6LbgGY9Fs98rqVt5AXLIhWkWywlVmtVrBp0igcN_IoypGlUPQGe77Rw'; "
        "assert(o.verify_rs256(input,sig,n,'AQAB')); assert(not o.verify_rs256(input..'x',sig,n,'AQAB'))");
    lua_close(L); return MUNIT_OK;
}
static unsigned port(lua_State *L) {
    lua_getglobal(L, "uri"); unsigned p = 0;
    munit_assert_int(sscanf(lua_tostring(L, -1), "http://127.0.0.1:%u/auth/callback", &p), ==, 1);
    lua_pop(L, 1); return p;
}
static int connect_local(unsigned p) {
    int fd = socket(AF_INET, SOCK_STREAM, 0); munit_assert_int(fd, >=, 0);
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(0x7f000001UL); addr.sin_port = htons(p);
    munit_assert_int(connect(fd, (struct sockaddr *)&addr, sizeof(addr)), ==, 0); return fd;
}
static void transmit(int fd, const char *s, size_t len) {
    while (len) { ssize_t n = send(fd, s, len, 0); munit_assert_int(n, >, 0); s += n; len -= (size_t)n; }
}
static void poll_result(lua_State *L) {
    run(L, "r,e=nil,nil; for i=1,10000 do r,e=h:poll(); if r~=nil or e~=nil then break end end; assert(r~=nil or e~=nil)");
}
static MunitResult listener(const MunitParameter params[], void *data) {
    (void)params; (void)data; lua_State *L = state();
    run(L, "h,uri=o.listen(); assert(h,uri); assert(h:poll()==nil)"); unsigned p = port(L);
    char req[17000];
    const char *bad[] = {
        "GET /wrong?code=x HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n",
        "POST /auth/callback?code=x HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n",
        "GET /auth/callback?code=x HTTP/1.1\r\nHost: evil:%u\r\n\r\n",
        "GET /auth/callback?code=x HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nHost: evil\r\n\r\n",
        "GET /auth/callback?code=x HTTP/1.1\r\nX-Port: %u\r\n\r\n",
        "GET /auth/callback?code=x HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nTransfer-Encoding: chunked\r\n\r\n"
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        int fd = connect_local(p); snprintf(req, sizeof(req), bad[i], p);
        transmit(fd, req, strlen(req)); poll_result(L);
        run(L, "assert(r==nil and e=='invalid callback request')"); close(fd);
    }
    int fd = connect_local(p); memset(req, 'X', sizeof(req)); transmit(fd, req, sizeof(req));
    poll_result(L); run(L, "assert(r==nil and e=='invalid callback request')"); close(fd);
    fd = connect_local(p); transmit(fd, "GET /auth/", 10);
    run(L, "local r,e=h:poll(); assert(r==nil and e==nil)");
    snprintf(req, sizeof(req), "callback?code=a%%2Bb&state=raw+value HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n\r\n", p);
    transmit(fd, req, strlen(req)); poll_result(L);
    run(L, "assert(r=='code=a%2Bb&state=raw+value'); local r,e=h:poll(); assert(r==nil and e=='listener closed'); assert(h:close()); assert(h:close())");
    ssize_t n = recv(fd, req, sizeof(req)-1, 0); munit_assert_int(n, >, 0); req[n] = 0;
    munit_assert_not_null(strstr(req, "200 OK")); munit_assert_not_null(strstr(req, "Cache-Control: no-store"));
    munit_assert_null(strstr(req, "raw+value")); close(fd);
    run(L, "h,uri=o.listen(); assert(h)"); fd = connect_local(port(L));
    run(L, "h:poll(); h=nil; collectgarbage('collect')");
    munit_assert_int(recv(fd, req, sizeof(req), 0), ==, 0); close(fd);
    lua_close(L); return MUNIT_OK;
}
static MunitTest tests[] = {
    {"/crypto", crypto, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {"/listener", listener, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
    {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};
MunitSuite oauth_native_suite = {"/oauth_native", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
#ifdef OAUTH_NATIVE_TEST_MAIN
int main(int argc, char **argv) { return munit_suite_main(&oauth_native_suite, NULL, argc, argv); }
#endif
