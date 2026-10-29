/* Beopylar QG characters for the HID usages this keymap can send.
 *
 * Generated from the Kalamine layout (beopylar.toml), against the xkb position
 * each ZMK keycode lands on. "plain" is the character with no extra modifier,
 * "shift" with Shift added, "altgr" with AltGr (Option) added. Characters the
 * 1-bit fonts cannot draw are spelled out in ASCII ("dia", "_1", ...).
 *
 * Regenerate when the keymap starts sending a keycode that is not listed here.
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/keys.h>

struct beopylar_char {
    uint32_t code;
    const char *plain;
    const char *shift;
    const char *altgr;
};

static const struct beopylar_char beopylar_chars[] = {
    {A, "a", "A", "{"},
    {B, "w", "W", "#"},
    {C, "x", "X", "]"},
    {COMMA, "g", "G", ";"},
    {D, "e", "E", ")"},
    {DOT, "h", "H", ":"},
    {E, "o", "O", ">"},
    {F, "i", "I", "}"},
    {FSLH, "f", "F", "?"},
    {G, ",", ";", "="},
    {H, "c", "C", "\\"},
    {I, "d", "D", "*"},
    {J, "t", "T", "+"},
    {K, "s", "S", "-"},
    {L, "r", "R", "/"},
    {M, "q", "Q", "!"},
    {N, "m", "M", "|"},
    {N0, "0", ")", "_0"},
    {N1, "1", "!", "_1"},
    {N2, "2", "@", "_2"},
    {N3, "3", "#", "_3"},
    {N4, "4", "$", "_4"},
    {N5, "5", "%", "_5"},
    {N6, "6", "^", "_6"},
    {N7, "7", "&", "_7"},
    {N8, "8", "*", "_8"},
    {N9, "9", "(", "_9"},
    {O, "l", "L", "'"},
    {P, "j", "J", "`"},
    {Q, "b", "B", "@"},
    {R, "p", "P", "$"},
    {S, "u", "U", "("},
    {SEMI, "n", "N", "\""},
    {T, "y", "Y", "%"},
    {U, "v", "V", "&"},
    {V, ".", ":", "_"},
    {W, "dia", "!", "<"},
    {X, "k", "K", "["},
    {Y, "-", "?", "^"},
    {Z, "z", "Z", "~"},
};

static inline const struct beopylar_char *beopylar_lookup(uint16_t usage_page, uint16_t code) {
    for (size_t i = 0; i < ARRAY_SIZE(beopylar_chars); i++) {
        if (ZMK_HID_USAGE_PAGE(beopylar_chars[i].code) == usage_page &&
            ZMK_HID_USAGE_ID(beopylar_chars[i].code) == code) {
            return &beopylar_chars[i];
        }
    }
    return NULL;
}
