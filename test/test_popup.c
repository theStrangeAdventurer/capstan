#include "munit.h"
#include "popup.h"
#include "popup_internal.h"
#include "utils.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void open_popup(int count, int multi) {
  PopupItem *items = malloc(count * sizeof(PopupItem));
  for (int i = 0; i < count; i++) {
    char buf[32];
    snprintf(buf, sizeof(buf), "item%d", i);
    items[i].text = my_strdup(buf);
    items[i].value = my_strdup(buf);
  }
  popup_open_with_plugin(items, count, "Test", 5, multi, NULL, 0);
  for (int i = 0; i < count; i++) {
    free(items[i].text);
    free(items[i].value);
  }
  free(items);
}

static MunitResult test_open_active(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(3, 0);
  munit_assert_int(popup_is_active(), ==, 1);
  munit_assert_int(g_popup.selected[0], ==, 1);
  munit_assert_int(g_popup.selected[1], ==, 0);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_close_data_inactive(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(3, 0);
  popup_close_data();
  munit_assert_int(popup_is_active(), ==, 0);
  return MUNIT_OK;
}

static MunitResult test_error_message_auto_closes(const MunitParameter p[],
                                                  void *d) {
  (void)p; (void)d;
  popup_show_message("Error", "boom", 1);
  munit_assert_int(popup_is_message_active(), ==, 1);
  munit_assert_int(g_msgpopup.auto_close_after_ms, ==, 8000);
  popup_close_message();

  popup_show_message("Info", "ok", 0);
  munit_assert_int(g_msgpopup.auto_close_after_ms, ==, 0);
  popup_close_message();
  return MUNIT_OK;
}

static MunitResult test_compact_message_is_non_modal(const MunitParameter p[],
                                                     void *d) {
  (void)p; (void)d;
  popup_show_message_ms("Copied", "Text copied", 0, 500);
  munit_assert_int(g_msgpopup.active, ==, 1);
  munit_assert_int(g_msgpopup.compact, ==, 1);
  munit_assert_int(g_msgpopup.auto_close_after_ms, ==, 500);
  munit_assert_int(popup_is_message_active(), ==, 0);
  popup_close_message();
  return MUNIT_OK;
}

static MunitResult test_message_popup_scroll_keys(const MunitParameter p[],
                                                  void *d) {
  (void)p; (void)d;
  popup_show_message("Info", "one\ntwo\nthree", 0);
  munit_assert_int(g_msgpopup.scroll, ==, 0);
  munit_assert_int(popup_message_handle_key('j'), ==, 1);
  munit_assert_int(g_msgpopup.scroll, ==, 1);
  munit_assert_int(popup_message_handle_key(POPUP_KEY_DOWN), ==, 1);
  munit_assert_int(g_msgpopup.scroll, ==, 2);
  munit_assert_int(popup_message_handle_key(POPUP_KEY_UP), ==, 1);
  munit_assert_int(g_msgpopup.scroll, ==, 1);
  munit_assert_int(popup_message_handle_key(0x04), ==, 1);
  munit_assert_int(g_msgpopup.scroll, ==, 6);
  munit_assert_int(popup_message_handle_key('k'), ==, 1);
  munit_assert_int(g_msgpopup.scroll, ==, 5);
  munit_assert_int(popup_message_handle_key(0x15), ==, 1);
  munit_assert_int(g_msgpopup.scroll, ==, 0);
  munit_assert_int(popup_message_handle_key('G'), ==, 1);
  munit_assert_int(g_msgpopup.scroll, ==, INT_MAX);
  munit_assert_int(popup_message_handle_key('g'), ==, 1);
  munit_assert_int(g_msgpopup.scroll, ==, 0);
  popup_close_message();
  return MUNIT_OK;
}

static MunitResult test_message_popup_close_resets_scroll(
    const MunitParameter p[], void *d) {
  (void)p; (void)d;
  popup_show_message("Info", "one\ntwo", 0);
  g_msgpopup.scroll = 3;
  munit_assert_int(popup_message_handle_key('\n'), ==, 0);
  munit_assert_int(popup_is_message_active(), ==, 0);
  munit_assert_int(g_msgpopup.scroll, ==, 0);
  return MUNIT_OK;
}

static MunitResult test_error_message_copy_key_shows_copied_ack(
    const MunitParameter p[], void *d) {
  (void)p; (void)d;
  setenv("CAPSTAN_DISABLE_CLIPBOARD", "1", 1);
  popup_show_message("API Error", "HTTP 400", 1);
  munit_assert_int(popup_is_message_active(), ==, 1);
  munit_assert_int(popup_message_handle_key('c'), ==, 0);
  munit_assert_int(g_msgpopup.active, ==, 1);
  munit_assert_int(g_msgpopup.compact, ==, 1);
  munit_assert_string_equal(g_msgpopup.title, "Copied");
  munit_assert_string_equal(g_msgpopup.text, "Error copied to clipboard");
  popup_close_message();
  unsetenv("CAPSTAN_DISABLE_CLIPBOARD");
  return MUNIT_OK;
}

static MunitResult test_j_down(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(5, 0);
  munit_assert_int(g_popup.cursor, ==, 0);
  popup_handle_key('j');
  munit_assert_int(g_popup.cursor, ==, 1);
  munit_assert_int(g_popup.selected[0], ==, 0);
  munit_assert_int(g_popup.selected[1], ==, 1);
  popup_handle_key('j');
  munit_assert_int(g_popup.cursor, ==, 2);
  munit_assert_int(g_popup.selected[1], ==, 0);
  munit_assert_int(g_popup.selected[2], ==, 1);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_k_up(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(5, 0);
  g_popup.cursor = 2;
  popup_handle_key('k');
  munit_assert_int(g_popup.cursor, ==, 1);
  popup_handle_key('k');
  munit_assert_int(g_popup.cursor, ==, 0);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_j_clamp_bottom(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(3, 0);
  g_popup.cursor = 2;
  popup_handle_key('j');
  munit_assert_int(g_popup.cursor, ==, 2);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_k_clamp_top(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(3, 0);
  popup_handle_key('k');
  munit_assert_int(g_popup.cursor, ==, 0);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_g_top(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(10, 0);
  g_popup.cursor = 7;
  popup_handle_key('g');
  munit_assert_int(g_popup.cursor, ==, 0);
  munit_assert_int(g_popup.scroll, ==, 0);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_G_bottom(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(10, 0);
  popup_handle_key('G');
  munit_assert_int(g_popup.cursor, ==, 9);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_ctrl_u(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(20, 0);
  g_popup.cursor = 10;
  popup_handle_key(0x15);
  munit_assert_int(g_popup.cursor, ==, 5);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_ctrl_d(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(20, 0);
  g_popup.cursor = 3;
  popup_handle_key(0x04);
  munit_assert_int(g_popup.cursor, ==, 8);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_ctrl_u_clamp(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(20, 0);
  g_popup.cursor = 2;
  popup_handle_key(0x15);
  munit_assert_int(g_popup.cursor, ==, 0);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_ctrl_d_clamp(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(8, 0);
  g_popup.cursor = 6;
  popup_handle_key(0x04);
  munit_assert_int(g_popup.cursor, ==, 7);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_scroll_down(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(10, 0);
  g_popup.max_visible = 5;
  g_popup.cursor = 4;
  popup_handle_key(POPUP_KEY_DOWN);
  munit_assert_int(g_popup.cursor, ==, 5);
  munit_assert_int(g_popup.scroll, ==, 1);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_scroll_up(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(10, 0);
  g_popup.max_visible = 5;
  g_popup.cursor = 1;
  g_popup.scroll = 1;
  popup_handle_key(POPUP_KEY_UP);
  munit_assert_int(g_popup.cursor, ==, 0);
  munit_assert_int(g_popup.scroll, ==, 0);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_l_confirms_non_multi(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(3, 0);
  int ret = popup_handle_key('l');
  munit_assert_int(ret, ==, 0);
  munit_assert_int(popup_is_active(), ==, 0);
  munit_assert_int(g_popup.selected[0], ==, 1);
  munit_assert_int(g_popup.selected[1], ==, 0);
  munit_assert_int(g_popup.selected[2], ==, 0);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_l_only_one_selected(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(3, 0);
  popup_handle_key('j');
  munit_assert_int(g_popup.selected[0], ==, 0);
  munit_assert_int(g_popup.selected[1], ==, 1);
  munit_assert_int(g_popup.selected[2], ==, 0);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_h_keeps_non_multi_selected(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(3, 0);
  munit_assert_int(g_popup.selected[0], ==, 1);
  popup_handle_key('h');
  munit_assert_int(g_popup.selected[0], ==, 1);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_l_confirms_when_selected(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(3, 0);
  int ret = popup_handle_key('l');
  munit_assert_int(ret, ==, 0);
  munit_assert_int(popup_is_active(), ==, 0);
  int sel_count;
  char **sel = popup_get_selected(&sel_count);
  munit_assert_int(sel_count, ==, 1);
  munit_assert_string_equal(sel[0], "item0");
  popup_free_selected(sel, sel_count);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_enter_selects_none(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(3, 0);
  popup_handle_key('j');
  int ret = popup_handle_key('\n');
  munit_assert_int(ret, ==, 0);
  munit_assert_int(g_popup.selected[1], ==, 1);
  int sel_count;
  char **sel = popup_get_selected(&sel_count);
  munit_assert_int(sel_count, ==, 1);
  munit_assert_string_equal(sel[0], "item1");
  popup_free_selected(sel, sel_count);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_tab_confirms_current(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(3, 0);
  popup_handle_key('j');
  int ret = popup_handle_key('\t');
  munit_assert_int(ret, ==, 0);
  munit_assert_int(popup_is_active(), ==, 0);
  munit_assert_int(g_popup.selected[1], ==, 1);
  int sel_count;
  char **sel = popup_get_selected(&sel_count);
  munit_assert_int(sel_count, ==, 1);
  munit_assert_string_equal(sel[0], "item1");
  popup_free_selected(sel, sel_count);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_l_selects_multi(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(3, 1);
  popup_handle_key('l');
  munit_assert_int(g_popup.selected[0], ==, 1);
  popup_handle_key('j');
  popup_handle_key('l');
  munit_assert_int(g_popup.selected[1], ==, 1);
  munit_assert_int(g_popup.selected[2], ==, 0);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_h_deselects_multi(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(3, 1);
  popup_handle_key('l');
  munit_assert_int(g_popup.selected[0], ==, 1);
  popup_handle_key('h');
  munit_assert_int(g_popup.selected[0], ==, 0);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_multi_multiple(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(3, 1);
  popup_handle_key('l');
  popup_handle_key('j');
  popup_handle_key('l');
  int sel_count;
  char **sel = popup_get_selected(&sel_count);
  munit_assert_int(sel_count, ==, 2);
  munit_assert_string_equal(sel[0], "item0");
  munit_assert_string_equal(sel[1], "item1");
  popup_free_selected(sel, sel_count);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_l_confirms_multi(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(3, 1);
  popup_handle_key('l');
  popup_handle_key('j');
  popup_handle_key('l');
  int ret = popup_handle_key('l');
  munit_assert_int(ret, ==, 0);
  munit_assert_int(popup_is_active(), ==, 0);
  int sel_count;
  char **sel = popup_get_selected(&sel_count);
  munit_assert_int(sel_count, ==, 2);
  popup_free_selected(sel, sel_count);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_esc_cancels(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(3, 0);
  int ret = popup_handle_key(27);
  munit_assert_int(ret, ==, 0);
  munit_assert_int(popup_is_active(), ==, 0);
  int sel_count;
  char **sel = popup_get_selected(&sel_count);
  munit_assert_ptr_null(sel);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_get_selected_values(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(3, 0);
  popup_handle_key('j');
  int sel_count;
  char **sel = popup_get_selected(&sel_count);
  munit_assert_int(sel_count, ==, 1);
  munit_assert_string_equal(sel[0], "item1");
  popup_free_selected(sel, sel_count);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_drill_down_replaces(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(3, 0);
  popup_handle_key('j');
  PopupItem items[2] = {
    {my_strdup("new0"), my_strdup("new0")},
    {my_strdup("new1"), my_strdup("new1")},
  };
  popup_drill_down(items, 2, "New");
  for (int i = 0; i < 2; i++) { free(items[i].text); free(items[i].value); }
  munit_assert_int(popup_is_active(), ==, 1);
  munit_assert_int(g_popup.item_count, ==, 2);
  munit_assert_int(g_popup.cursor, ==, 0);
  munit_assert_int(g_popup.scroll, ==, 0);
  munit_assert_int(g_popup.selected[0], ==, 1);
  munit_assert_int(g_popup.selected[1], ==, 0);
  munit_assert_string_equal(g_popup.items[0].text, "new0");
  munit_assert_string_equal(g_popup.items[1].text, "new1");
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_drill_down_clears_filterable_state(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  PopupItem items[2] = {
    {my_strdup("alpha"), my_strdup("alpha")},
    {my_strdup("beta"), my_strdup("beta")},
  };
  popup_open_filterable_with_plugin(items, 2, "Find", 5, 0, NULL, 0);
  popup_handle_key('b');

  PopupItem next[1] = {{my_strdup("next"), my_strdup("next")}};
  popup_drill_down(next, 1, "Next");

  munit_assert_int(g_popup.filterable, ==, 0);
  munit_assert_int(g_popup.query_len, ==, 0);
  munit_assert_string_equal(g_popup.query, "");
  munit_assert_ptr_null(g_popup.all_items);
  munit_assert_int(g_popup.all_item_count, ==, 0);

  for (int i = 0; i < 2; i++) { free(items[i].text); free(items[i].value); }
  free(next[0].text);
  free(next[0].value);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_enter_multi_no_selection(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(3, 1);
  popup_handle_key('j');
  int ret = popup_handle_key('\n');
  munit_assert_int(ret, ==, 0);
  munit_assert_int(g_popup.selected[1], ==, 1);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_G_scroll(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(10, 0);
  g_popup.max_visible = 5;
  popup_handle_key('G');
  munit_assert_int(g_popup.cursor, ==, 9);
  munit_assert_int(g_popup.scroll, ==, 5);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_ctrl_d_scroll(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  open_popup(20, 0);
  g_popup.max_visible = 5;
  g_popup.cursor = 3;
  popup_handle_key(0x04);
  munit_assert_int(g_popup.cursor, ==, 8);
  munit_assert_int(g_popup.scroll, ==, 4);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_filterable_typing_filters_items(const MunitParameter p[],
                                                        void *d) {
  (void)p; (void)d;
  PopupItem items[3] = {
    {my_strdup("src/main.c"), my_strdup("/repo/src/main.c")},
    {my_strdup("README.md"), my_strdup("/repo/README.md")},
    {my_strdup("test/test_main.c"), my_strdup("/repo/test/test_main.c")},
  };
  popup_open_filterable_with_plugin(items, 3, "Files", 5, 0, NULL, 0);
  for (int i = 0; i < 3; i++) { free(items[i].text); free(items[i].value); }

  popup_handle_key('r');
  popup_handle_key('e');
  munit_assert_string_equal(g_popup.query, "re");
  munit_assert_int(g_popup.item_count, ==, 1);
  munit_assert_string_equal(g_popup.items[0].text, "README.md");
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_filterable_fuzzy_finds_command_name(
    const MunitParameter p[], void *d) {
  (void)p;
  (void)d;
  PopupItem items[4] = {
      {my_strdup("/editor  Edit prompt in $EDITOR"), my_strdup("/editor")},
      {my_strdup("/new  Start a new session"), my_strdup("/new")},
      {my_strdup("/models  Select provider and model"), my_strdup("/models")},
      {my_strdup("/compact  Compact conversation context"),
       my_strdup("/compact")},
  };
  popup_open_filterable_with_plugin(items, 4, "Commands", 10, 0, NULL, 0);
  for (int i = 0; i < 4; i++) {
    free(items[i].text);
    free(items[i].value);
  }

  popup_handle_key('m');
  popup_handle_key('d');
  popup_handle_key('l');
  popup_handle_key('s');
  munit_assert_int(g_popup.item_count, ==, 1);
  munit_assert_string_equal(g_popup.items[0].value, "/models");

  munit_assert_int(popup_handle_key('\n'), ==, 0);
  int sel_count;
  char **selected = popup_get_selected(&sel_count);
  munit_assert_int(sel_count, ==, 1);
  munit_assert_string_equal(selected[0], "/models");
  popup_free_selected(selected, sel_count);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_filterable_backspace_refilters(const MunitParameter p[],
                                                       void *d) {
  (void)p; (void)d;
  PopupItem items[2] = {
    {my_strdup("alpha.c"), my_strdup("/repo/alpha.c")},
    {my_strdup("beta.c"), my_strdup("/repo/beta.c")},
  };
  popup_open_filterable_with_plugin(items, 2, "Files", 5, 0, NULL, 0);
  for (int i = 0; i < 2; i++) { free(items[i].text); free(items[i].value); }

  popup_handle_key('z');
  munit_assert_int(g_popup.item_count, ==, 0);
  popup_handle_key(127);
  munit_assert_string_equal(g_popup.query, "");
  munit_assert_int(g_popup.item_count, ==, 2);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_filterable_enter_selects_current(
    const MunitParameter p[], void *d) {
  (void)p; (void)d;
  PopupItem items[2] = {
    {my_strdup("alpha.c"), my_strdup("/repo/alpha.c")},
    {my_strdup("beta.c"), my_strdup("/repo/beta.c")},
  };
  popup_open_filterable_with_plugin(items, 2, "Files", 5, 0, NULL, 0);
  for (int i = 0; i < 2; i++) { free(items[i].text); free(items[i].value); }

  popup_handle_key('b');
  int ret = popup_handle_key('\n');
  munit_assert_int(ret, ==, 0);
  int sel_count;
  char **sel = popup_get_selected(&sel_count);
  munit_assert_int(sel_count, ==, 1);
  munit_assert_string_equal(sel[0], "/repo/beta.c");
  popup_free_selected(sel, sel_count);
  popup_close_data();
  return MUNIT_OK;
}

static MunitResult test_row_prefix_width(const MunitParameter p[], void *d) {
  (void)p; (void)d;
  munit_assert_int(popup_row_prefix_width(0), ==, 0);
  munit_assert_int(popup_row_prefix_width(1), ==, 4);
  return MUNIT_OK;
}

static MunitResult test_scrollbar_hidden_when_all_items_fit(
    const MunitParameter p[], void *d) {
  (void)p; (void)d;
  PopupScrollbar bar = popup_scrollbar_calc(5, 5, 0);
  munit_assert_int(bar.visible, ==, 0);
  bar = popup_scrollbar_calc(4, 5, 0);
  munit_assert_int(bar.visible, ==, 0);
  return MUNIT_OK;
}

static MunitResult test_scrollbar_top_middle_bottom(const MunitParameter p[],
                                                    void *d) {
  (void)p; (void)d;
  PopupScrollbar bar = popup_scrollbar_calc(20, 5, 0);
  munit_assert_int(bar.visible, ==, 1);
  munit_assert_int(bar.top, ==, 0);
  munit_assert_int(bar.height, ==, 2);

  bar = popup_scrollbar_calc(20, 5, 8);
  munit_assert_int(bar.visible, ==, 1);
  munit_assert_int(bar.top, ==, 2);
  munit_assert_int(bar.height, ==, 2);

  bar = popup_scrollbar_calc(20, 5, 15);
  munit_assert_int(bar.visible, ==, 1);
  munit_assert_int(bar.top, ==, 3);
  munit_assert_int(bar.height, ==, 2);
  return MUNIT_OK;
}

static MunitResult test_scrollbar_clamps_scroll_and_thumb(
    const MunitParameter p[], void *d) {
  (void)p; (void)d;
  PopupScrollbar bar = popup_scrollbar_calc(100, 3, 999);
  munit_assert_int(bar.visible, ==, 1);
  munit_assert_int(bar.top, ==, 2);
  munit_assert_int(bar.height, ==, 1);

  bar = popup_scrollbar_calc(100, 3, -10);
  munit_assert_int(bar.visible, ==, 1);
  munit_assert_int(bar.top, ==, 0);
  munit_assert_int(bar.height, ==, 1);
  return MUNIT_OK;
}

static MunitTest tests[] = {
  {"/open_active", test_open_active, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/close_data_inactive", test_close_data_inactive, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/error_message_auto_closes", test_error_message_auto_closes, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/compact_message_is_non_modal", test_compact_message_is_non_modal, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/message_popup_scroll_keys", test_message_popup_scroll_keys, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/message_popup_close_resets_scroll", test_message_popup_close_resets_scroll, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/error_message_copy_key_shows_copied_ack", test_error_message_copy_key_shows_copied_ack, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/j_down", test_j_down, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/k_up", test_k_up, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/j_clamp_bottom", test_j_clamp_bottom, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/k_clamp_top", test_k_clamp_top, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/g_top", test_g_top, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/G_bottom", test_G_bottom, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/G_scroll", test_G_scroll, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/ctrl_u", test_ctrl_u, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/ctrl_d", test_ctrl_d, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/ctrl_u_clamp", test_ctrl_u_clamp, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/ctrl_d_clamp", test_ctrl_d_clamp, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/ctrl_d_scroll", test_ctrl_d_scroll, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/scroll_down", test_scroll_down, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/scroll_up", test_scroll_up, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/l_confirms_non_multi", test_l_confirms_non_multi, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/l_only_one_selected", test_l_only_one_selected, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/h_keeps_non_multi_selected", test_h_keeps_non_multi_selected, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/l_confirms_when_selected", test_l_confirms_when_selected, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/enter_selects_none", test_enter_selects_none, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/tab_confirms_current", test_tab_confirms_current, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/l_selects_multi", test_l_selects_multi, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/h_deselects_multi", test_h_deselects_multi, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/multi_multiple", test_multi_multiple, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/l_confirms_multi", test_l_confirms_multi, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/enter_multi_no_selection", test_enter_multi_no_selection, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/esc_cancels", test_esc_cancels, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/get_selected_values", test_get_selected_values, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/drill_down_replaces", test_drill_down_replaces, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/drill_down_clears_filterable_state", test_drill_down_clears_filterable_state, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/filterable_typing_filters_items", test_filterable_typing_filters_items, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/filterable_fuzzy_finds_command_name",
   test_filterable_fuzzy_finds_command_name, NULL, NULL,
   MUNIT_TEST_OPTION_NONE, NULL},
  {"/filterable_backspace_refilters", test_filterable_backspace_refilters, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/filterable_enter_selects_current", test_filterable_enter_selects_current, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/row_prefix_width", test_row_prefix_width, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/scrollbar_hidden_when_all_items_fit", test_scrollbar_hidden_when_all_items_fit, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/scrollbar_top_middle_bottom", test_scrollbar_top_middle_bottom, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {"/scrollbar_clamps_scroll_and_thumb", test_scrollbar_clamps_scroll_and_thumb, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
  {NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};

MunitSuite popup_suite = {"/popup", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE};
