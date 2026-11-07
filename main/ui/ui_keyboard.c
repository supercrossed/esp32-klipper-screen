/*
 * Keyboard layouts: number row on the text layouts, and an IP/port numpad
 * (LV_KEYBOARD_MODE_USER_1) with '.' and ':' for addresses. "123" jumps to the
 * numpad, "abc" back to letters.
 */
#include "ui.h"
#include <string.h>

#define POP(w) (LV_BUTTONMATRIX_CTRL_POPOVER | (w))
#define FN(w)  (LV_KEYBOARD_CTRL_BUTTON_FLAGS | (w))
#define CHK(w) (LV_BUTTONMATRIX_CTRL_CHECKED | (w))

static const char *const kb_lc[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "1#", "z", "x", "c", "v", "b", "n", "m", ".", "-", "_", "\n",
    LV_SYMBOL_KEYBOARD, "123", LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};
static const char *const kb_uc[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    "1#", "Z", "X", "C", "V", "B", "N", "M", ".", "-", "_", "\n",
    LV_SYMBOL_KEYBOARD, "123", LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};
static const lv_buttonmatrix_ctrl_t kb_txt_ctrl[] = {
    POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), CHK(6),
    POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4),
    FN(6), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4),
    FN(6), POP(3), POP(3), POP(3), POP(3), POP(3), POP(3), POP(3), POP(3), POP(3), POP(3),
    FN(2), FN(2), CHK(2), 6, CHK(2), FN(3),
};

static const char *const kb_sym[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "+", "&", "/", "*", "=", "%", "!", "?", "#", "<", ">", "\n",
    "\\", "@", "$", "(", ")", "{", "}", "[", "]", ";", "\"", "'", "\n",
    "abc", "~", "^", "`", "|", ",", ".", ":", "-", "_", "\n",
    LV_SYMBOL_KEYBOARD, "123", LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};
static const lv_buttonmatrix_ctrl_t kb_sym_ctrl[] = {
    POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), CHK(6),
    POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4),
    POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4), POP(4),
    FN(6), POP(3), POP(3), POP(3), POP(3), POP(3), POP(3), POP(3), POP(3), POP(3),
    FN(2), FN(2), CHK(2), 6, CHK(2), FN(3),
};

/* IP / host:port numpad */
static const char *const kb_ip[] = {
    "1", "2", "3", LV_SYMBOL_BACKSPACE, "\n",
    "4", "5", "6", ".", "\n",
    "7", "8", "9", ":", "\n",
    "abc", "0", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};
static const lv_buttonmatrix_ctrl_t kb_ip_ctrl[] = {
    3, 3, 3, CHK(3),
    3, 3, 3, 3,
    3, 3, 3, 3,
    FN(3), 3, CHK(2), CHK(2), FN(2),
};

static void kb_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    const char *txt = id == LV_BUTTONMATRIX_BUTTON_NONE ? NULL : lv_buttonmatrix_get_button_text(kb, id);
    if (txt && !strcmp(txt, "123")) {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_1);
        return;
    }
    lv_keyboard_def_event_cb(e);   /* everything else: LVGL's normal handling (abc/ABC/1#, OK, etc.) */
}

void ui_keyboard_setup(lv_obj_t *kb)
{
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, kb_lc, kb_txt_ctrl);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, kb_uc, kb_txt_ctrl);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, kb_sym, kb_sym_ctrl);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1, kb_ip, kb_ip_ctrl);
    lv_keyboard_set_popovers(kb, true);
    lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_VALUE_CHANGED, NULL);
}
