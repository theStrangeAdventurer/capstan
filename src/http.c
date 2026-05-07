#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/typecheck-gcc.h>
#include <lauxlib.h>
#include <lua.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *data;
  size_t size;
  size_t cap;
} RespBuf;

/**
 * Колбек с контрактом  CURLOPT_WRITEFUNCTION
 * libcurl может дёрнуть коллбек несколько раз за один запрос
 * на каждый пришедший TCP-пакет.
 * Должен вернуть точное число полученных байт иначе curl посчитает запрос
 * ошибочным и прервет его
 */
static size_t
write_cb(char *chunk_ptr,    // Только что полученный чанк данных
         size_t size,        // Почти всегда равен 1
         size_t count,       // Количество элементов (символов) в указателе
         void *resp_data_ptr // указатель структуру в которой мы храним весь
                             // респонс, курл просто передает его в колбек и
                             // вообще не знает ничего про его тип
) {
  RespBuf *buf = resp_data_ptr;
  size_t total = size * count;
  char *new_ptr = realloc(buf->data, buf->size + total + 1);

  if (!new_ptr)
    return 0;

  buf->data = new_ptr;
  memcpy(buf->data + buf->size, chunk_ptr, total);
  buf->size += total;
  buf->data[buf->size] = '\0';

  return total;
}

static struct curl_slist *parse_headers(lua_State *L, int index) {
  struct curl_slist *headers = NULL;

  if (lua_istable(L, index)) {
    lua_pushnil(
        L); // nil как указатель для lua_next что надо начинать с начала таблицы
    while (lua_next(L, index) != 0) {
      const char *key =
          lua_tostring(L, -2); // Фикс позиция не завязанная на позицию таблицы
      const char *val = lua_tostring(L, -1); // Фикс позиция
      char header[2048];
      snprintf(header, sizeof(header), "%s: %s", key, val); // "Header: value"
      headers = curl_slist_append(headers, header);
      lua_pop(L, 1); // Снимаем со стека значение оставляя только key как курсор
                     // для следующей итерации
    }
  }
  return headers;
}

static int l_http_post(lua_State *L) {
  const char *url = luaL_checkstring(L, 1);
  const char *body = NULL;
  size_t body_len = 0;

  if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
    body = luaL_checklstring(L, 2, &body_len);
  }

  struct curl_slist *headers = parse_headers(L, 3);

  CURL *easy = curl_easy_init();
  curl_easy_setopt(easy, CURLOPT_URL, url);
  curl_easy_setopt(easy, CURLOPT_POST, 1L);

  if (body) {
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)body_len);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body);
  }

  if (headers) {
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
  }

  RespBuf response = {0};
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, &response);
  curl_easy_perform(easy);
  long status = 0;
  curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
  lua_pushinteger(L, status);
  lua_pushstring(L, response.data ? response.data : "");
  free(response.data);
  curl_slist_free_all(headers);
  curl_easy_cleanup(easy);
  return 2;
}

static int l_http_get(lua_State *L) {
  const char *url = luaL_checkstring(L, 1);
  struct curl_slist *headers = parse_headers(L, 2);
  // easy это простой синхронный вызов curl
  CURL *easy = curl_easy_init();
  curl_easy_setopt(easy, CURLOPT_URL, url);
  if (headers) {
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
  }

  RespBuf response = {0};

  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, &response);

  curl_easy_perform(easy);

  long status = 0;

  curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
  lua_pushinteger(L, status);
  lua_pushstring(L, response.data ? response.data : "");

  free(response.data);
  curl_slist_free_all(headers);
  curl_easy_cleanup(easy);
  return 2; // Два результата положили на стек - status -2, response_data -1
}

void http_init(lua_State *L) {
  curl_global_init(CURL_GLOBAL_DEFAULT); // один раз на процесс
  lua_newtable(L);
  lua_pushcfunction(L, l_http_get);
  lua_setfield(
      L, -2,
      "get"); // В этот момент  l_http_get уже не лежит на стеке, она
              // переместилась в таблицу в качестве значения для ключа get
  lua_pushcfunction(L, l_http_post);
  lua_setfield(L, -2, "post");

  lua_setglobal(L, "http");
}

void http_cleanup(void) { curl_global_cleanup(); }
