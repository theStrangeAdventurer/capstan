#include "popup_internal.h"
#include "finder.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>

#ifdef POPUP_NCURSES
#include <ncursesw/curses.h>
#endif

PopupState g_popup = {0};
MsgPopup g_msgpopup = {0};

static void (*g_win_cleanup_fn)(void *) = NULL;

void popup_set_win_cleanup(void (*fn)(void *)) { g_win_cleanup_fn = fn; }

int popup_row_prefix_width(int multi) { return multi ? 4 : 0; }

PopupScrollbar popup_scrollbar_calc(int item_count, int visible, int scroll) {
  PopupScrollbar bar = {0, 0, 0};
  if (item_count <= 0 || visible <= 0 || item_count <= visible)
    return bar;

  if (scroll < 0)
    scroll = 0;
  int max_scroll = item_count - visible;
  if (scroll > max_scroll)
    scroll = max_scroll;

  int height = (visible * visible + item_count - 1) / item_count;
  if (height < 1)
    height = 1;
  if (height > visible)
    height = visible;

  int top = 0;
  if (max_scroll > 0 && visible > height)
    top = (scroll * (visible - height) + max_scroll / 2) / max_scroll;

  bar.visible = 1;
  bar.top = top;
  bar.height = height;
  return bar;
}

static void popup_win_cleanup(void) {
  if (g_popup.win) {
    if (g_win_cleanup_fn)
      g_win_cleanup_fn(g_popup.win);
    g_popup.win = NULL;
  }
}

static void popup_sync_single_selection(void) {
  if (g_popup.multi || !g_popup.selected || g_popup.item_count <= 0)
    return;

  for (int i = 0; i < g_popup.item_count; i++)
    g_popup.selected[i] = (i == g_popup.cursor);
}

static void popup_items_free(PopupItem *items, int count) {
  for (int i = 0; i < count; i++) {
    free(items[i].text);
    free(items[i].value);
  }
  free(items);
}

typedef struct {
  int index;
  int score;
} PopupMatch;

static int compare_matches(const void *a, const void *b) {
  const PopupMatch *ma = (const PopupMatch *)a;
  const PopupMatch *mb = (const PopupMatch *)b;
  if (ma->score != mb->score)
    return mb->score - ma->score;
  return ma->index - mb->index;
}

static void popup_apply_filter(void) {
  popup_items_free(g_popup.items, g_popup.item_count);
  free(g_popup.selected);
  g_popup.items = NULL;
  g_popup.selected = NULL;
  g_popup.item_count = 0;
  g_popup.cursor = 0;
  g_popup.scroll = 0;

  if (!g_popup.filterable)
    return;

  PopupMatch *matches = malloc(g_popup.all_item_count * sizeof(PopupMatch));
  if (!matches)
    return;

  int match_count = 0;
  for (int i = 0; i < g_popup.all_item_count; i++) {
    int score = finder_fuzzy_score(g_popup.all_items[i].text, g_popup.query);
    if (score > 0) {
      matches[match_count].index = i;
      matches[match_count].score = score;
      match_count++;
    }
  }
  qsort(matches, match_count, sizeof(PopupMatch), compare_matches);

  g_popup.item_count = match_count;
  g_popup.items = malloc(match_count * sizeof(PopupItem));
  g_popup.selected = malloc(match_count * sizeof(int));
  if ((!g_popup.items || !g_popup.selected) && match_count > 0) {
    free(matches);
    popup_items_free(g_popup.items, g_popup.item_count);
    free(g_popup.selected);
    g_popup.items = NULL;
    g_popup.selected = NULL;
    g_popup.item_count = 0;
    return;
  }

  for (int i = 0; i < match_count; i++) {
    PopupItem *src = &g_popup.all_items[matches[i].index];
    g_popup.items[i].text = my_strdup(src->text ? src->text : "");
    g_popup.items[i].value = my_strdup(src->value ? src->value : "");
    g_popup.selected[i] = 0;
  }

  free(matches);
  popup_sync_single_selection();
}

void popup_open(PopupItem *items, int count, const char *title,
                int max_visible, int multi) {
  popup_open_with_plugin(items, count, title, max_visible, multi, NULL, 0);
}

void popup_open_with_plugin(PopupItem *items, int count, const char *title,
                            int max_visible, int multi, struct Plugin *plugin,
                            size_t cmd_end) {
  if (g_msgpopup.active)
    return;
  g_popup.active = 1;
  g_popup.cancelled = 0;
  g_popup.cursor = 0;
  g_popup.scroll = 0;
  g_popup.max_visible = max_visible > 0 ? max_visible : POPUP_DEFAULT_LIMIT;
  g_popup.multi = multi;
  g_popup.filterable = 0;
  g_popup.query[0] = '\0';
  g_popup.query_len = 0;
  g_popup.last_rows = -1;
  g_popup.last_cols = -1;
  g_popup.plugin = plugin;
  g_popup.cmd_end = cmd_end;

  popup_win_cleanup();
  popup_items_free(g_popup.all_items, g_popup.all_item_count);
  g_popup.all_items = NULL;
  g_popup.all_item_count = 0;

  free(g_popup.title);
  g_popup.title = title ? my_strdup(title) : my_strdup("");

  g_popup.item_count = count;
  g_popup.items = malloc(count * sizeof(PopupItem));
  g_popup.selected = malloc(count * sizeof(int));
  for (int i = 0; i < count; i++) {
    g_popup.items[i].text = my_strdup(items[i].text ? items[i].text : "");
    g_popup.items[i].value = my_strdup(items[i].value ? items[i].value : "");
    g_popup.selected[i] = 0;
  }
  popup_sync_single_selection();
}

void popup_open_filterable_with_plugin(PopupItem *items, int count,
                                       const char *title, int max_visible,
                                       int multi, struct Plugin *plugin,
                                       size_t cmd_end) {
  if (g_msgpopup.active)
    return;
  popup_open_with_plugin(NULL, 0, title, max_visible, multi, plugin, cmd_end);
  g_popup.filterable = 1;
  g_popup.all_item_count = count;
  g_popup.all_items = malloc(count * sizeof(PopupItem));
  if (!g_popup.all_items && count > 0) {
    g_popup.all_item_count = 0;
    return;
  }
  for (int i = 0; i < count; i++) {
    g_popup.all_items[i].text = my_strdup(items[i].text ? items[i].text : "");
    g_popup.all_items[i].value = my_strdup(items[i].value ? items[i].value : "");
  }
  popup_apply_filter();
}

int popup_is_active(void) { return g_popup.active; }

void popup_drill_down(PopupItem *items, int count, const char *title) {
  if (g_msgpopup.active)
    return;

  for (int i = 0; i < g_popup.item_count; i++) {
    free(g_popup.items[i].text);
    free(g_popup.items[i].value);
  }
  free(g_popup.items);
  free(g_popup.selected);
  free(g_popup.title);

  g_popup.active = 1;
  g_popup.cancelled = 0;
  g_popup.cursor = 0;
  g_popup.scroll = 0;
  g_popup.item_count = count;
  g_popup.items = malloc(count * sizeof(PopupItem));
  g_popup.selected = malloc(count * sizeof(int));
  for (int i = 0; i < count; i++) {
    g_popup.items[i].text = my_strdup(items[i].text ? items[i].text : "");
    g_popup.items[i].value = my_strdup(items[i].value ? items[i].value : "");
    g_popup.selected[i] = 0;
  }
  popup_sync_single_selection();
  g_popup.title = title ? my_strdup(title) : my_strdup("");

  popup_win_cleanup();
  g_popup.last_rows = -1;
  g_popup.last_cols = -1;
}

int popup_handle_key(int ch) {
  if (g_popup.filterable) {
    if (ch == 127 || ch == 8 || ch == 0407) {
      if (g_popup.query_len > 0) {
        g_popup.query[--g_popup.query_len] = '\0';
        popup_apply_filter();
      }
      return 1;
    }
    if (ch >= 32 && ch <= 126) {
      if (g_popup.query_len < (int)sizeof(g_popup.query) - 1) {
        g_popup.query[g_popup.query_len++] = (char)ch;
        g_popup.query[g_popup.query_len] = '\0';
        popup_apply_filter();
      }
      return 1;
    }
  }

  switch (ch) {
  case POPUP_KEY_UP:
  case 0x10:
  case 'k':
    if (g_popup.cursor > 0) {
      g_popup.cursor--;
      if (g_popup.cursor < g_popup.scroll)
        g_popup.scroll = g_popup.cursor;
    }
    break;
  case POPUP_KEY_DOWN:
  case 0x0e:
  case 'j':
    if (g_popup.cursor < g_popup.item_count - 1) {
      g_popup.cursor++;
      if (g_popup.cursor >= g_popup.scroll + g_popup.max_visible)
        g_popup.scroll = g_popup.cursor - g_popup.max_visible + 1;
    }
    break;
  case POPUP_KEY_LEFT:
  case 'h':
    if (g_popup.multi && g_popup.selected[g_popup.cursor])
      g_popup.selected[g_popup.cursor] = 0;
    break;
  case POPUP_KEY_RIGHT:
  case 'l':
    if (g_popup.multi && !g_popup.selected[g_popup.cursor])
      g_popup.selected[g_popup.cursor] = 1;
    else if (g_popup.selected[g_popup.cursor]) {
      g_popup.cancelled = 0;
      g_popup.active = 0;
      return 0;
    } else if (!g_popup.multi) {
      for (int i = 0; i < g_popup.item_count; i++)
        g_popup.selected[i] = 0;
      g_popup.selected[g_popup.cursor] = 1;
    }
    break;
  case 'g':
    g_popup.cursor = 0;
    g_popup.scroll = 0;
    break;
  case 'G':
    g_popup.cursor = g_popup.item_count - 1;
    if (g_popup.cursor >= g_popup.max_visible)
      g_popup.scroll = g_popup.cursor - g_popup.max_visible + 1;
    else
      g_popup.scroll = 0;
    break;
  case 0x15:
    if (g_popup.cursor > 0) {
      g_popup.cursor -= 5;
      if (g_popup.cursor < 0)
        g_popup.cursor = 0;
      if (g_popup.cursor < g_popup.scroll)
        g_popup.scroll = g_popup.cursor;
    }
    break;
  case 0x04:
    if (g_popup.cursor < g_popup.item_count - 1) {
      g_popup.cursor += 5;
      if (g_popup.cursor >= g_popup.item_count)
        g_popup.cursor = g_popup.item_count - 1;
      if (g_popup.cursor >= g_popup.scroll + g_popup.max_visible)
        g_popup.scroll = g_popup.cursor - g_popup.max_visible + 1;
    }
    break;
  case '\t':
  case '\n':
  case '\r': {
    if (g_popup.item_count <= 0)
      return 1;
    int any = 0;
    if (g_popup.multi) {
      for (int i = 0; i < g_popup.item_count; i++)
        if (g_popup.selected[i]) {
          any = 1;
          break;
        }
    }
    if (!any)
      g_popup.selected[g_popup.cursor] = 1;
    g_popup.cancelled = 0;
    g_popup.active = 0;
    return 0;
  }
  case 27:
    g_popup.cancelled = 1;
    g_popup.active = 0;
    return 0;
  }
  popup_sync_single_selection();
  return 1;
}

char **popup_get_selected(int *count) {
  *count = 0;
  if (g_popup.cancelled)
    return NULL;
  int n = 0;
  for (int i = 0; i < g_popup.item_count; i++)
    if (g_popup.selected[i])
      n++;
  if (n == 0)
    return NULL;
  char **result = malloc(n * sizeof(char *));
  int idx = 0;
  for (int i = 0; i < g_popup.item_count; i++)
    if (g_popup.selected[i])
      result[idx++] = my_strdup(g_popup.items[i].value);
  *count = n;
  return result;
}

void popup_free_selected(char **selected, int count) {
  for (int i = 0; i < count; i++)
    free(selected[i]);
  free(selected);
}

void popup_close_data(void) {
  popup_items_free(g_popup.items, g_popup.item_count);
  popup_items_free(g_popup.all_items, g_popup.all_item_count);
  free(g_popup.selected);
  free(g_popup.title);
  g_popup.items = NULL;
  g_popup.all_items = NULL;
  g_popup.selected = NULL;
  g_popup.title = NULL;
  g_popup.item_count = 0;
  g_popup.all_item_count = 0;
  g_popup.active = 0;
  g_popup.cancelled = 0;
  g_popup.plugin = NULL;
  g_popup.cmd_end = 0;
  g_popup.multi = 0;
  g_popup.filterable = 0;
  g_popup.query[0] = '\0';
  g_popup.query_len = 0;
  g_popup.cursor = 0;
  g_popup.scroll = 0;
  g_popup.max_visible = POPUP_DEFAULT_LIMIT;
}

struct Plugin *popup_get_plugin(void) { return g_popup.plugin; }

size_t popup_get_cmd_end(void) { return g_popup.cmd_end; }

void popup_show_message(const char *title, const char *text, int is_error) {
  popup_close_message();
  g_msgpopup.active = 1;
  g_msgpopup.is_error = is_error;
  g_msgpopup.title = title ? my_strdup(title) : my_strdup("");
  g_msgpopup.text = text ? my_strdup(text) : my_strdup("");
  g_msgpopup.win = NULL;
  g_msgpopup.last_rows = -1;
  g_msgpopup.last_cols = -1;
  g_msgpopup.created_at = time(NULL);
  g_msgpopup.auto_close_after_sec = is_error ? 8 : 0;
}

int popup_is_message_active(void) { return g_msgpopup.active; }

void popup_close_message(void) {
#ifdef POPUP_NCURSES
  if (g_msgpopup.win) {
    werase(g_msgpopup.win);
    wnoutrefresh(g_msgpopup.win);
    delwin(g_msgpopup.win);
    g_msgpopup.win = NULL;
  }
#endif
  free(g_msgpopup.title);
  free(g_msgpopup.text);
  g_msgpopup.title = NULL;
  g_msgpopup.text = NULL;
  g_msgpopup.active = 0;
  g_msgpopup.is_error = 0;
  g_msgpopup.last_rows = -1;
  g_msgpopup.last_cols = -1;
  g_msgpopup.created_at = 0;
  g_msgpopup.auto_close_after_sec = 0;
}

int popup_message_handle_key(int ch) {
  (void)ch;
  if (ch == '\n' || ch == '\r' || ch == 27) {
    popup_close_message();
    return 0;
  }
  return 1;
}
