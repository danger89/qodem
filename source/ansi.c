/*
 * ansi.c
 *
 * This module is licensed under the GNU General Public License
 * Version 2.  Please see the file "COPYING" in this directory for
 * more information about the GNU General Public License Version 2.
 *
 *     Copyright (C) 2015  Kevin Lamonte
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

/*
 * ANSI music:  ESC [ M <notes> ^N
 *
 * I actually saw a Synchronet BBS using ESC n, which linux calls
 * LS2 (invoke G2 character set).  What's that about?
 *
 * Also saw DECAWM (CSI ? 7 h) - turn line wrap on
 */

#include "common.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "qodem.h"
#include "music.h"
#include "states.h"
#include "screen.h"
#include "netclient.h"
#include "ansi.h"

/* #define DEBUG_ANSI 1 */
#undef DEBUG_ANSI
#ifdef DEBUG_ANSI
#include <stdio.h>
#define ANSI_DEBUG_FILE "debug_ansi.txt"
static FILE * ANSI_DEBUG_FILE_HANDLE = NULL;
#endif /* DEBUG_ANSI */

#define ANSI_RESPONSE_LENGTH    16
#define ANSI_MUSIC_BUFFER_SIZE  1024

/* Scan states. */
typedef enum SCAN_STATES {
        SCAN_NONE,
        SCAN_ESC,
        SCAN_START_SEQUENCE,
        SCAN_COUNT,
        SCAN_COUNT_TWO,
        SCAN_COUNT_MANY,
        SCAN_MUSIC
} SCAN_STATE;

/* Current scanning state. */
static SCAN_STATE scan_state;

static int saved_cursor_x;
static int saved_cursor_y;
static unsigned char rep_character;
static Q_BOOL private_mode_flag;
static Q_BOOL dec_private_mode_flag;

static unsigned char music_buffer[ANSI_MUSIC_BUFFER_SIZE];
static int music_buffer_n;

/*
 * For ANSI animation support, we do the following:
 * If I print a character and the attribute is different
 * from the previous printed character's attribute, then
 * flush the screen.
 */
static attr_t old_character_color;

/*
 * ansi_reset - reset the emulation state
 */
void ansi_reset() {
        scan_state = SCAN_NONE;
        saved_cursor_x = q_status.cursor_x;
        saved_cursor_y = q_status.cursor_y;
        old_character_color = q_current_color;
        private_mode_flag = Q_FALSE;
        dec_private_mode_flag = Q_FALSE;
#ifdef DEBUG_ANSI
        if (ANSI_DEBUG_FILE_HANDLE == NULL) {
                ANSI_DEBUG_FILE_HANDLE = fopen(ANSI_DEBUG_FILE, "w");
        }
        fprintf(ANSI_DEBUG_FILE_HANDLE, "ansi_reset()\n");
#endif /* DEBUG_ANSI */
} /* ---------------------------------------------------------------------- */

/*
 * Reset the scan state for a new sequence
 */
static void clear_state(wchar_t * to_screen) {
        private_mode_flag = Q_FALSE;
        dec_private_mode_flag = Q_FALSE;
        q_emul_buffer_n = 0;
        q_emul_buffer_i = 0;
        memset(q_emul_buffer, 0, sizeof(q_emul_buffer));
        scan_state = SCAN_NONE;
        *to_screen = 1;
} /* ---------------------------------------------------------------------- */

/*
 * Hang onto one character in the buffer
 */
static void save_char(unsigned char keep_char, wchar_t * to_screen) {
        q_emul_buffer[q_emul_buffer_n] = keep_char;
        q_emul_buffer_n++;
        *to_screen = 1;
} /* ---------------------------------------------------------------------- */


/*
 * ansi_ps - figure out next numbered parameter from buffer.  -1 on error.
 */
static int ansi_ps(unsigned char ** count) {
        /* It's possible a terminal will have over 100 columns,
           but unlikely to have over 1000.  So we'll only
           count up to 999. */
        char ch[4] = "\0\0\0\0";

        /* First digit */
        if (isdigit(**count)) {
                ch[0] = **count;
                (*count)++;
        } else {
                return -1;
        }

        /* See if the second char is a digit */
        if (isdigit(**count)) {
                ch[1] = **count;
                (*count)++;
        }

        /* See if the third char is a digit */
        if (isdigit(**count)) {
                ch[2] = **count;
                (*count)++;
        }

        /* Error out from here on */
        if (isdigit(**count)) {
                return -1;
        }

        return atoi(ch);
} /* ---------------------------------------------------------------------- */

/*
 * ansi_position - figure out position from buffer.  true means parsing
 *              OK, false means parsing error.
 */
static Q_BOOL ansi_position(unsigned char ** count) {
        int ps;
        int new_row = -1;
        int new_col = -1;

        while (*count < q_emul_buffer + sizeof(q_emul_buffer)) {
                if ((new_row != -1) && (new_col != -1)) {
                        return Q_FALSE;
                }

                /* Check row */
                if ((new_row == -1) && (**count == ';' )) {
                        /* Omitted row means assume 0 */
                        new_row = 0;
                        (*count)++;
                        continue;
                } else if (new_row == -1) {
                        ps = ansi_ps(count);
                        if (ps == -1) {
                                return Q_FALSE;
                        }
                        new_row = ps - 1;
                        if (new_row < 0) {
                                /* I've seen a live BBS send a "CSI 0;31f" so I have
                                 * to support this...
                                 */
                                new_row = 0;
                        }
                }

                /* Check column */
                if (strlen((char *)*count) == 0) {
                        /* Column wasn't specified, so assume 0 */
                        new_col = 0;
                } else if (**count != ';') {
                        /* Error:  code was "CSI [ Pn Xf" where X isn't semicolon */
                        return Q_FALSE;
                } else {
                        /* Push past the semicolon */
                        (*count)++;

                        /* Grab the number argument */
                        ps = ansi_ps(count);
                        if (ps == -1) {
                                /* Number argument is invalid */
                                return Q_FALSE;
                        }
                        new_col = ps - 1;
                        if (new_col < 0) {
                                /* I've seen a live BBS send a "CSI 0;31f" so I have
                                 * to support this...
                                 */
                                new_col = 0;
                        }
                }

                /* Push past the string */
                (*count)++;
                if (**count == '\0') {
                        /* We are at the correct exit point */
                        break;
                }
        } /* while (*count < q_emul_buffer + sizeof(q_emul_buffer)) */

        cursor_position(new_row, new_col);
#ifdef DEBUG_ANSI
        fprintf(ANSI_DEBUG_FILE_HANDLE, "cursor_position(): %d %d\n", new_row, new_col);
#endif /* DEBUG_ANSI */
        return Q_TRUE;
} /* ---------------------------------------------------------------------- */

/*
 * ansi_ich - insert blank characters at current position.  true means parsing
 *              OK, false means parsing error.
 */
static Q_BOOL ansi_ich(unsigned char ** count) {
        int ps;
        int insert_count = -1;

        while (*count < q_emul_buffer + sizeof(q_emul_buffer)) {
                /* Technically we can have more than one parameter,
                 * but this is ANSI.SYS, not real ANSI.  Error out
                 * if we see more than one parameter.
                 */
                if (insert_count != -1) {
                        return Q_FALSE;
                }

                /* Check insert_count */
                if ((insert_count == -1) && (**count == ';' )) {
                        /* Omitted parameter means assume 1 */
                        insert_count = 1;
                        (*count)++;
                        continue;
                } else if (insert_count == -1) {
                        ps = ansi_ps(count);
                        if (ps == -1) {
                                return Q_FALSE;
                        }
                        insert_count = ps;
                        if (insert_count < 0) {
                                return Q_FALSE;
                        }
                }

                /* Check end of string */
                if (strlen((char *)*count) == 0) {
                        /* The correct exit point */
                        break;
                } else if (**count != ';') {
                        /* Error:  code was "CSI [ Pn X@" where X isn't semicolon */
                        return Q_FALSE;
                } else {
                        /* Push past the semicolon */
                        (*count)++;
                }

                /* Push past the string */
                (*count)++;
                if (**count == '\0') {
                        /* We are at the correct exit point */
                        break;
                }
        } /* while (*count < q_emul_buffer + sizeof(q_emul_buffer)) */

        insert_blanks(insert_count);
        return Q_TRUE;
} /* ---------------------------------------------------------------------- */

/*
 * ansi_dch - delete characters at current position.  true means parsing
 *              OK, false means parsing error.
 */
static Q_BOOL ansi_dch(unsigned char ** count) {
        int ps;
        int delete_count = -1;

        while (*count < q_emul_buffer + sizeof(q_emul_buffer)) {
                /* Technically we can have more than one parameter,
                 * but this is ANSI.SYS, not real ANSI.  Error out
                 * if we see more than one parameter.
                 */
                if (delete_count != -1) {
                        return Q_FALSE;
                }

                /* Check delete_count */
                if ((delete_count == -1) && (**count == ';' )) {
                        /* Omitted parameter means assume 1 */
                        delete_count = 1;
                        (*count)++;
                        continue;
                } else if (delete_count == -1) {
                        ps = ansi_ps(count);
                        if (ps == -1) {
                                return Q_FALSE;
                        }
                        delete_count = ps;
                        if (delete_count < 0) {
                                return Q_FALSE;
                        }
                }

                /* Check end of string */
                if (strlen((char *)*count) == 0) {
                        /* The correct exit point */
                        break;
                } else if (**count != ';') {
                        /* Error:  code was "CSI [ Pn XP" where X isn't semicolon */
                        return Q_FALSE;
                } else {
                        /* Push past the semicolon */
                        (*count)++;
                }

                /* Push past the string */
                (*count)++;
                if (**count == '\0') {
                        /* We are at the correct exit point */
                        break;
                }
        } /* while (*count < q_emul_buffer + sizeof(q_emul_buffer)) */

        delete_character(delete_count);
        return Q_TRUE;
} /* ---------------------------------------------------------------------- */

/*
 * ansi_il - insert lines.  true means parsing
 *              OK, false means parsing error.
 */
static Q_BOOL ansi_il(unsigned char ** count) {
        int ps;
        int insert_count = -1;

        while (*count < q_emul_buffer + sizeof(q_emul_buffer)) {
                /* Technically we can have more than one parameter,
                 * but this is ANSI.SYS, not real ANSI.  Error out
                 * if we see more than one parameter.
                 */
                if (insert_count != -1) {
                        return Q_FALSE;
                }

                /* Check insert_count */
                if ((insert_count == -1) && (**count == ';' )) {
                        /* Omitted parameter means assume 1 */
                        insert_count = 1;
                        (*count)++;
                        continue;
                } else if (insert_count == -1) {
                        ps = ansi_ps(count);
                        if (ps == -1) {
                                return Q_FALSE;
                        }
                        insert_count = ps;
                        if (insert_count < 0) {
                                return Q_FALSE;
                        }
                }

                /* Check end of string */
                if (strlen((char *)*count) == 0) {
                        /* The correct exit point */
                        break;
                } else if (**count != ';') {
                        /* Error:  code was "CSI [ Pn XL" where X isn't semicolon */
                        return Q_FALSE;
                } else {
                        /* Push past the semicolon */
                        (*count)++;
                }

                /* Push past the string */
                (*count)++;
                if (**count == '\0') {
                        /* We are at the correct exit point */
                        break;
                }
        } /* while (*count < q_emul_buffer + sizeof(q_emul_buffer)) */

        /* I can get the same effect with a scroll-down */
        scrolling_region_scroll_down(q_status.cursor_y, q_status.scroll_region_bottom, insert_count);

        return Q_TRUE;
} /* ---------------------------------------------------------------------- */

/*
 * ansi_dl - delete lines.  true means parsing
 *              OK, false means parsing error.
 */
static Q_BOOL ansi_dl(unsigned char ** count) {
        int ps;
        int delete_count = -1;

        while (*count < q_emul_buffer + sizeof(q_emul_buffer)) {
                /* Technically we can have more than one parameter,
                 * but this is ANSI.SYS, not real ANSI.  Error out
                 * if we see more than one parameter.
                 */
                if (delete_count != -1) {
                        return Q_FALSE;
                }

                /* Check delete_count */
                if ((delete_count == -1) && (**count == ';' )) {
                        /* Omitted parameter means assume 1 */
                        delete_count = 1;
                        (*count)++;
                        continue;
                } else if (delete_count == -1) {
                        ps = ansi_ps(count);
                        if (ps == -1) {
                                return Q_FALSE;
                        }
                        delete_count = ps;
                        if (delete_count < 0) {
                                return Q_FALSE;
                        }
                }

                /* Check end of string */
                if (strlen((char *)*count) == 0) {
                        /* The correct exit point */
                        break;
                } else if (**count != ';') {
                        /* Error:  code was "CSI [ Pn XM" where X isn't semicolon */
                        return Q_FALSE;
                } else {
                        /* Push past the semicolon */
                        (*count)++;
                }

                /* Push past the string */
                (*count)++;
                if (**count == '\0') {
                        /* We are at the correct exit point */
                        break;
                }
        } /* while (*count < q_emul_buffer + sizeof(q_emul_buffer)) */

        /* I can get the same effect with a scroll-up */
        scrolling_region_scroll_up(q_status.cursor_y, q_status.scroll_region_bottom, delete_count);

        return Q_TRUE;
} /* ---------------------------------------------------------------------- */

/*
 * ansi_vpa - switch to row #, same column.  true means parsing
 *              OK, false means parsing error.
 */
static Q_BOOL ansi_vpa(unsigned char ** count) {
        int ps;
        int new_row = -1;

        while (*count < q_emul_buffer + sizeof(q_emul_buffer)) {
                /* Technically we can have more than one parameter,
                 * but this is ANSI.SYS, not real ANSI.  Error out
                 * if we see more than one parameter.
                 */
                if (new_row != -1) {
                        return Q_FALSE;
                }

                /* Check new_row */
                if ((new_row == -1) && (**count == ';' )) {
                        /* Omitted parameter means assume 0 */
                        new_row = 0;
                        (*count)++;
                        continue;
                } else if (new_row == -1) {
                        ps = ansi_ps(count);
                        if (ps == -1) {
                                return Q_FALSE;
                        }
                        new_row = ps - 1;
                        if (new_row < 0) {
                                return Q_FALSE;
                        }
                }

                /* Check end of string */
                if (strlen((char *)*count) == 0) {
                        /* The correct exit point */
                        break;
                } else if (**count != ';') {
                        /* Error:  code was "CSI [ Pn Xd" where X isn't semicolon */
                        return Q_FALSE;
                } else {
                        /* Push past the semicolon */
                        (*count)++;
                }

                /* Push past the string */
                (*count)++;
                if (**count == '\0') {
                        /* We are at the correct exit point */
                        break;
                }
        } /* while (*count < q_emul_buffer + sizeof(q_emul_buffer)) */

        cursor_position(new_row, q_status.cursor_x);
#ifdef DEBUG_ANSI
        fprintf(ANSI_DEBUG_FILE_HANDLE, "cursor_position(): %d %d\n", new_row, q_status.cursor_x);
#endif /* DEBUG_ANSI */
        return Q_TRUE;
} /* ---------------------------------------------------------------------- */

/*
 * ansi_cht - horizontal tab.  true means parsing
 *              OK, false means parsing error.
 */
static Q_BOOL ansi_cht(unsigned char ** count) {
        int ps;
        int tab_count = -1;
        int i;

        while (*count < q_emul_buffer + sizeof(q_emul_buffer)) {
                /* Technically we can have more than one parameter,
                 * but this is ANSI.SYS, not real ANSI.  Error out
                 * if we see more than one parameter.
                 */
                if (tab_count != -1) {
                        return Q_FALSE;
                }

                /* Check tab_count */
                if ((tab_count == -1) && (**count == ';' )) {
                        /* Omitted parameter means assume 1 */
                        tab_count = 1;
                        (*count)++;
                        continue;
                } else if (tab_count == -1) {
                        ps = ansi_ps(count);
                        if (ps == -1) {
                                return Q_FALSE;
                        }
                        tab_count = ps - 1;
                        if (tab_count < 0) {
                                return Q_FALSE;
                        }
                }

                /* Check end of string */
                if (strlen((char *)*count) == 0) {
                        /* The correct exit point */
                        break;
                } else if (**count != ';') {
                        /* Error:  code was "CSI [ Pn Xd" where X isn't semicolon */
                        return Q_FALSE;
                } else {
                        /* Push past the semicolon */
                        (*count)++;
                }

                /* Push past the string */
                (*count)++;
                if (**count == '\0') {
                        /* We are at the correct exit point */
                        break;
                }
        } /* while (*count < q_emul_buffer + sizeof(q_emul_buffer)) */

#ifdef DEBUG_ANSI
        fprintf(ANSI_DEBUG_FILE_HANDLE, "CHT: %d\n", tab_count);
#endif /* DEBUG_ANSI */
        for (i=0; i<tab_count; i++) {
                while (q_status.cursor_x < 79) {
                        print_character(' ');
                        if (q_status.cursor_x % 8 == 0) {
                                break;
                        }
                }
        }

        return Q_TRUE;
} /* ---------------------------------------------------------------------- */

/*
 * ansi_cha - switch to column #, same row.  true means parsing
 *              OK, false means parsing error.
 */
static Q_BOOL ansi_cha(unsigned char ** count) {
        int ps;
        int new_col = -1;

        while (*count < q_emul_buffer + sizeof(q_emul_buffer)) {
                /* Technically we can have more than one parameter,
                 * but this is ANSI.SYS, not real ANSI.  Error out
                 * if we see more than one parameter.
                 */
                if (new_col != -1) {
                        return Q_FALSE;
                }

                /* Check new_col */
                if ((new_col == -1) && (**count == ';' )) {
                        /* Omitted parameter means assume 0 */
                        new_col = 0;
                        (*count)++;
                        continue;
                } else if (new_col == -1) {
                        ps = ansi_ps(count);
                        if (ps == -1) {
                                return Q_FALSE;
                        }
                        new_col = ps - 1;
                        if (new_col < 0) {
                                return Q_FALSE;
                        }
                }

                /* Check end of string */
                if (strlen((char *)*count) == 0) {
                        /* The correct exit point */
                        break;
                } else if (**count != ';') {
                        /* Error:  code was "CSI [ Pn XG" where X isn't semicolon */
                        return Q_FALSE;
                } else {
                        /* Push past the semicolon */
                        (*count)++;
                }

                /* Push past the string */
                (*count)++;
                if (**count == '\0') {
                        /* We are at the correct exit point */
                        break;
                }
        } /* while (*count < q_emul_buffer + sizeof(q_emul_buffer)) */

        cursor_position(q_status.cursor_y, new_col);
#ifdef DEBUG_ANSI
        fprintf(ANSI_DEBUG_FILE_HANDLE, "cursor_position(): %d %d\n", q_status.cursor_y, new_col);
#endif /* DEBUG_ANSI */
        return Q_TRUE;
} /* ---------------------------------------------------------------------- */

/*
 * ansi_rep - repeat character.  true means parsing
 *              OK, false means parsing error.
 */
static Q_BOOL ansi_rep(unsigned char ** count) {
        int i;
        int ps;
        int rep_count = -1;

        while (*count < q_emul_buffer + sizeof(q_emul_buffer)) {
                /* Technically we can have more than one parameter,
                 * but this is ANSI.SYS, not real ANSI.  Error out
                 * if we see more than one parameter.
                 */
                if (rep_count != -1) {
                        return Q_FALSE;
                }

                /* Check rep_count */
                if ((rep_count == -1) && (**count == ';' )) {
                        /* Omitted parameter means assume 1 */
                        rep_count = 1;
                        (*count)++;
                        continue;
                } else if (rep_count == -1) {
                        ps = ansi_ps(count);
                        if (ps == -1) {
                                return Q_FALSE;
                        }
                        rep_count = ps;
                        if (rep_count < 0) {
                                return Q_FALSE;
                        }
                }

                /* Check end of string */
                if (strlen((char *)*count) == 0) {
                        /* The correct exit point */
                        break;
                } else if (**count != ';') {
                        /* Error:  code was "CSI [ Pn Xb" where X isn't semicolon */
                        return Q_FALSE;
                } else {
                        /* Push past the semicolon */
                        (*count)++;
                }

                /* Push past the string */
                (*count)++;
                if (**count == '\0') {
                        /* We are at the correct exit point */
                        break;
                }
        } /* while (*count < q_emul_buffer + sizeof(q_emul_buffer)) */

#ifdef DEBUG_ANSI
        fprintf(ANSI_DEBUG_FILE_HANDLE, "ANSI REP: %d\n", rep_count);
#endif /* DEBUG_ANSI */
        for (i=0; i<rep_count; i++) {
                print_character(rep_character);
        }

        return Q_TRUE;
} /* ---------------------------------------------------------------------- */

/*
 * ansi_set_mode - set an ANSI.SYS mode.  true means parsing
 *              OK, false means parsing error.
 */
static Q_BOOL ansi_set_mode(unsigned char ** count, Q_BOOL set_mode) {
        int ps;
        int new_mode = -1;

        while (*count < q_emul_buffer + sizeof(q_emul_buffer)) {
                /* Technically we can have more than one parameter,
                 * but this is ANSI.SYS, not real ANSI.  Error out
                 * if we see more than one parameter.
                 */
                if (new_mode != -1) {
                        return Q_FALSE;
                }

                /* Check new_mode */
                if ((new_mode == -1) && (**count == ';' )) {
                        /* Omitted parameter means assume 0 */
                        new_mode = 0;
                        (*count)++;
                        continue;
                } else if (new_mode == -1) {
                        ps = ansi_ps(count);
                        if (ps == -1) {
                                return Q_FALSE;
                        }
                        new_mode = ps;
                        if (new_mode < 0) {
                                return Q_FALSE;
                        }
                }

                /* Check end of string */
                if (strlen((char *)*count) == 0) {
                        /* The correct exit point */
                        break;
                } else if (**count != ';') {
                        /* Error:  code was "CSI [ Pn XG" where X isn't semicolon */
                        return Q_FALSE;
                } else {
                        /* Push past the semicolon */
                        (*count)++;
                }

                /* Push past the string */
                (*count)++;
                if (**count == '\0') {
                        /* We are at the correct exit point */
                        break;
                }
        } /* while (*count < q_emul_buffer + sizeof(q_emul_buffer)) */



        switch (new_mode) {
        case 7:
                if ((set_mode == Q_TRUE) && ((private_mode_flag == Q_TRUE) || (dec_private_mode_flag == Q_TRUE))) {

                        /* Enable line wrap */
#ifdef DEBUG_ANSI
                        fprintf(ANSI_DEBUG_FILE_HANDLE, "SET: %d ENABLE LINE WRAP\n", new_mode);
#endif /* DEBUG_ANSI */
                        q_status.line_wrap = Q_TRUE;
                        break;
                } else if ((set_mode == Q_FALSE) && ((private_mode_flag == Q_TRUE) || (dec_private_mode_flag == Q_TRUE))) {

                        /* Disable line wrap */
#ifdef DEBUG_ANSI
                        fprintf(ANSI_DEBUG_FILE_HANDLE, "RESET: %d DISABLE LINE WRAP\n", new_mode);
#endif /* DEBUG_ANSI */
                        q_status.line_wrap = Q_FALSE;
                        break;
                }

                /* Fall through... */

        default:
#ifdef DEBUG_ANSI
                if (set_mode == Q_TRUE) {
                        fprintf(ANSI_DEBUG_FILE_HANDLE, "SET: %d NOP\n", new_mode);
                } else {
                        fprintf(ANSI_DEBUG_FILE_HANDLE, "RESET: %d NOP\n", new_mode);
                }
#endif /* DEBUG_ANSI */
                break;
        }

        return Q_TRUE;
} /* ---------------------------------------------------------------------- */

/*
 * ansi_color - figure out color from buffer.  true means parsing
 *              OK, false means parsing error.
 */
Q_BOOL ansi_color(attr_t * output, unsigned char ** count) {
        int ps;
        short foreground, background;
        short curses_color;

        /* Strip the color off *output */
        *output &= NO_COLOR_MASK;

        /* Pull the current foreground and background */
        curses_color = color_from_attr(q_current_color);
        foreground = (curses_color & 0x38) >> 3;
        background = curses_color & 0x07;

#ifdef DEBUG_ANSI
        fprintf(ANSI_DEBUG_FILE_HANDLE, "sgr(): foreground=%02x background=%02x initial *output=%08x\n", foreground, background, *output);
        fprintf(ANSI_DEBUG_FILE_HANDLE, "sgr(): Pn...Pn = ");
#endif /* DEBUG_ANSI */

        while (*count < q_emul_buffer + sizeof(q_emul_buffer)) {
                ps = ansi_ps(count);
                if (ps == -1) {
                        return Q_FALSE;
                }
#ifdef DEBUG_ANSI
                fprintf(ANSI_DEBUG_FILE_HANDLE, "%d ", ps);
#endif /* DEBUG_ANSI */

                switch (ps) {

                case 0:
                        *output = Q_A_NORMAL;
                        foreground = q_text_colors[Q_COLOR_CONSOLE_TEXT].fg;
                        background = q_text_colors[Q_COLOR_CONSOLE_TEXT].bg;
                        if (q_text_colors[Q_COLOR_CONSOLE_TEXT].bold == Q_TRUE) {
                                *output |= Q_A_BOLD;
                        }
                        break;
                case 1:
                        *output |= Q_A_BOLD;
                        break;
                case 2:
                        *output |= Q_A_DIM;
                        break;
                case 4:
                        *output |= Q_A_UNDERLINE;
                        break;
                case 5:
                        *output |= Q_A_BLINK;
                        break;
                case 7:
                        *output |= Q_A_REVERSE;
                        break;
                case 21:
                case 22:
                        *output &= ~(Q_A_DIM | Q_A_BOLD);
                        break;
                case 24:
                        *output &= ~Q_A_UNDERLINE;
                        break;
                case 25:
                        *output &= ~Q_A_BLINK;
                        break;
                case 27:
                        *output &= ~Q_A_REVERSE;
                        break;
                case 30:
                        foreground = Q_COLOR_BLACK;
                        break;
                case 31:
                        foreground = Q_COLOR_RED;
                        break;
                case 32:
                        foreground = Q_COLOR_GREEN;
                        break;
                case 33:
                        foreground = Q_COLOR_YELLOW;
                        break;
                case 34:
                        foreground = Q_COLOR_BLUE;
                        break;
                case 35:
                        foreground = Q_COLOR_MAGENTA;
                        break;
                case 36:
                        foreground = Q_COLOR_CYAN;
                        break;
                case 37:
                        foreground = Q_COLOR_WHITE;
                        break;
                case 38:
                        foreground = q_text_colors[Q_COLOR_CONSOLE_TEXT].fg;
                        if (q_text_colors[Q_COLOR_CONSOLE_TEXT].bold == Q_TRUE) {
                                *output |= Q_A_BOLD;
                        }
                        *output |= Q_A_UNDERLINE;
                        break;
                case 39:
                        foreground = q_text_colors[Q_COLOR_CONSOLE_TEXT].fg;
                        if (q_text_colors[Q_COLOR_CONSOLE_TEXT].bold == Q_TRUE) {
                                *output |= Q_A_BOLD;
                        }
                        *output &= ~Q_A_UNDERLINE;
                        break;
                case 40:
                        background = Q_COLOR_BLACK;
                        break;
                case 41:
                        background = Q_COLOR_RED;
                        break;
                case 42:
                        background = Q_COLOR_GREEN;
                        break;
                case 43:
                        background = Q_COLOR_YELLOW;
                        break;
                case 44:
                        background = Q_COLOR_BLUE;
                        break;
                case 45:
                        background = Q_COLOR_MAGENTA;
                        break;
                case 46:
                        background = Q_COLOR_CYAN;
                        break;
                case 47:
                        background = Q_COLOR_WHITE;
                        break;
                case 49:
                        background = q_text_colors[Q_COLOR_CONSOLE_TEXT].bg;
                        *output &= ~Q_A_UNDERLINE;
                        break;
                default:
                        /* Ignore unknown options */
                        break;
                }

                if (**count != ';') {

#ifdef DEBUG_ANSI
                        fprintf(ANSI_DEBUG_FILE_HANDLE, "\n");
#endif /* DEBUG_ANSI */

                        /* No more processing */
                        curses_color = (foreground << 3) | background;

#ifdef DEBUG_ANSI
                        fprintf(ANSI_DEBUG_FILE_HANDLE, "sgr(): new foreground=%02x new background=%02x\n", foreground, background);
                        fprintf(ANSI_DEBUG_FILE_HANDLE, "SGR: old color=%08x ", q_current_color);
#endif /* DEBUG_ANSI */

                        *output |= color_to_attr(curses_color);
#ifdef DEBUG_ANSI
                        fprintf(ANSI_DEBUG_FILE_HANDLE, "curses_color=%02x *output=%08x\n", curses_color, *output);
#endif /* DEBUG_ANSI */
                        return Q_TRUE;
                }

                (*count)++;
        } /* while (*count < q_emul_buffer + sizeof(q_emul_buffer)) */

        return Q_FALSE;
} /* ---------------------------------------------------------------------- */

/*
 * ansi - process through ANSI emulator.
 */
Q_EMULATION_STATUS ansi(const unsigned char from_modem, wchar_t * to_screen) {
        static unsigned char * count;
        static attr_t attributes;
        int i;
        char response_buffer[ANSI_RESPONSE_LENGTH];

#ifdef DEBUG_ANSI
        fprintf(ANSI_DEBUG_FILE_HANDLE, "STATE: %d CHAR: 0x%02x '%c'\n", scan_state, from_modem, from_modem);
        fflush(ANSI_DEBUG_FILE_HANDLE);
        /* render_screen_to_debug_file(ANSI_DEBUG_FILE_HANDLE);*/
#endif /* DEBUG_ANSI */

        switch (scan_state) {

        case SCAN_NONE:
                /* ESC */
                if (from_modem == KEY_ESCAPE) {
                        save_char(from_modem, to_screen);
                        scan_state = SCAN_ESC;
                        attributes = q_current_color;
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                /* Control characters */
                if (iscntrl(from_modem)) {
#ifdef DEBUG_ANSI
                        fprintf(ANSI_DEBUG_FILE_HANDLE, "generic_handle_control_char(): control_char = 0x%02x\n", from_modem);
#endif /* DEBUG_ANSI */
                        generic_handle_control_char(from_modem);

                        /* Consume */
                        *to_screen = 1;
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                /* Print character */
                *to_screen = codepage_map_char(from_modem);
                rep_character = codepage_map_char(from_modem);

                /*
                 * Special case:  for ANSI animation, force the screen to
                 * repaint if this character has a different color than the
                 * last character.
                 */
                if ((q_status.ansi_animate == Q_TRUE) && (old_character_color != q_current_color)) {
                        q_screen_dirty = Q_TRUE;
                        refresh_handler();
                        old_character_color = q_current_color;
                }

                return Q_EMUL_FSM_ONE_CHAR;

        case SCAN_ESC:
                /* Looking for '[' */
                if (from_modem == '[') {
                        save_char(from_modem, to_screen);
                        scan_state = SCAN_START_SEQUENCE;
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'Z') {
                        /* Identify */

                        /* Send string directly to remote side */
                        qodem_write(q_child_tty_fd, "\033[?1;2c", 3, Q_TRUE);

                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                /* Believe it or not I actually saw a site use
                 * ESC CSI <code>.  So if we see ESC again,
                 * stay in SCAN_ESC state.
                 */
                if (from_modem == KEY_ESCAPE) {
                        /* Consume */
                        *to_screen = 1;
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                break;

        case SCAN_START_SEQUENCE:
                /* Looking for digit or code. */
                if (isdigit(from_modem)) {
                        /* Looks like a counter */
                        count = q_emul_buffer + q_emul_buffer_n;
                        save_char(from_modem, to_screen);
                        scan_state = SCAN_COUNT;
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }
                if (from_modem == 'K') {
                        /* Erase from here to end of line */
                        erase_line(q_status.cursor_x, q_scrollback_current->length, Q_FALSE);

                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }
                if (from_modem == 'J') {
                        /* Erase from here to end of screen */
                        erase_screen(q_status.cursor_y, q_status.cursor_x, HEIGHT - STATUS_HEIGHT - 1, WIDTH - 1, Q_FALSE);

                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'm') {
#ifdef DEBUG_ANSI
                        fprintf(ANSI_DEBUG_FILE_HANDLE, "SGR: reset\n");
#endif /* DEBUG_ANSI */
                        /* ESC [ m mean ESC [ 0 m, all attributes off */
                        q_current_color = Q_A_NORMAL | scrollback_full_attr(Q_COLOR_CONSOLE_TEXT);

                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'A') {
                        /* Cursor up */
                        cursor_up(1, Q_FALSE);
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'B') {
                        /* Cursor down */
                        cursor_down(1, Q_FALSE);
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'C') {
                        /* Cursor right */
                        cursor_right(1, Q_FALSE);
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'D') {
                        /* Cursor left */
                        cursor_left(1, Q_FALSE);
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if ((from_modem == 'H') || (from_modem == 'f')) {
                        /* Cursor position to (0,0) */
                        cursor_position(0, 0);

                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == ';') {
                        /* Continue scanning, more numbers coming. */
                        count = q_emul_buffer + q_emul_buffer_n;
                        save_char(from_modem, to_screen);
                        scan_state = SCAN_COUNT_TWO;
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'c') {
                        /* Identify */

                        /* Send string directly to remote side */
                        qodem_write(q_child_tty_fd, "\033[?1;2c", 3, Q_TRUE);

                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 's') {
                        /* Save cursor position */
                        saved_cursor_x = q_status.cursor_x;
                        saved_cursor_y = q_status.cursor_y;
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'u') {
                        /* Restore cursor position */
                        cursor_position(saved_cursor_y, saved_cursor_x);

                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'I') {
                        /* CHT */
                        /* No parameter means assume 1 */
                        while (q_status.cursor_x < 79) {
                                print_character(' ');
                                if (q_status.cursor_x % 8 == 0) {
                                        break;
                                }
                        }

                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == '@') {
                        /* ICH */
                        /* No parameter means assume 1 */
                        insert_blanks(1);

                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'd') {
                        /* VPA */
                        /* No parameter means assume 0 */
                        cursor_position(0, q_status.cursor_x);

                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'G') {
                        /* CHA */
                        /* No parameter means assume 0 */
                        cursor_position(q_status.cursor_y, 0);

                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'L') {
                        /* IL */
                        /* No parameter means assume 1 */
                        /* I can get the same effect with a scroll-down */
                        scrolling_region_scroll_down(q_status.cursor_y, q_status.scroll_region_bottom, 1);

                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'P') {
                        /* DCH */
                        /* No parameter means assume 1 */
                        delete_character(1);

                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'b') {
                        /* REP */
                        /* No parameter means assume 1 */
                        print_character(rep_character);

                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'M') {

                        if (q_status.ansi_music == Q_FALSE) {
                                /* DL */
                                /* No parameter means assume 1 */
                                /* I can get the same effect with a scroll-up */
                                scrolling_region_scroll_up(q_status.cursor_y, q_status.scroll_region_bottom, 1);

                                clear_state(to_screen);
                                return Q_EMUL_FSM_NO_CHAR_YET;
                        }

                        /* ANSI Music */
                        memset(music_buffer, 0, sizeof(music_buffer));
                        music_buffer[0] = from_modem;
                        music_buffer_n = 1;

                        scan_state = SCAN_MUSIC;
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == '=') {
                        /* This will be a DOS private mode change (CSI = Pn { h | l } ) */
                        private_mode_flag = Q_TRUE;
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == '?') {
                        /* This will be a DEC private mode change (CSI = Pn { h | l } ) */
                        dec_private_mode_flag = Q_TRUE;
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == '!') {
                        /* This is a RIPScript query command, discard it */
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                break;

        case SCAN_COUNT:
                /* Looking for digit, ';', or code */
                if (isdigit(from_modem)) {
                        save_char(from_modem, to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'K') {
                        /* Erase from here to end of line */
                        i = ansi_ps(&count);
                        if (i == -1) {
                                break;
                        }
                        if (i == 1) {
                                /* Erase from beginning of line to here */
                                erase_line(0, q_status.cursor_x, Q_FALSE);
                        }
                        if (i == 2) {
                                /* Erase entire line */
                                erase_line(0, q_scrollback_current->length, Q_FALSE);
                        }

                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'J') {
                        i = ansi_ps(&count);
                        if (i == -1) {
                                break;
                        }
                        if (i == 1) {
                                /* Erase from beginning of screen to here */
                                erase_screen(0, 0, q_status.cursor_y, q_status.cursor_x, Q_FALSE);
                        }
                        if (i == 2) {
                                /* Erase entire screen, and move cursor to home position */
                                /* erase_screen(0, 0, HEIGHT - STATUS_HEIGHT - 1, WIDTH - 1, Q_FALSE); */
                                cursor_formfeed();
                        }

                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'A') {
                        /* Cursor up */
                        i = ansi_ps(&count);
                        if (i == -1) {
                                break;
                        }
                        cursor_up(i, Q_FALSE);
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'B') {
                        /* Cursor down */
                        i = ansi_ps(&count);
                        if (i == -1) {
                                break;
                        }
                        cursor_down(i, Q_FALSE);
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'C') {
                        /* Cursor right */
                        i = ansi_ps(&count);
                        if (i == -1) {
                                break;
                        }
                        cursor_right(i, Q_FALSE);
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'D') {
                        /* Cursor left */
                        i = ansi_ps(&count);
                        if (i == -1) {
                                break;
                        }
                        cursor_left(i, Q_FALSE);
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == ';') {
                        /* Continue scanning, more numbers coming. */
                        save_char(from_modem, to_screen);
                        scan_state = SCAN_COUNT_TWO;
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if ((from_modem == 'H') || (from_modem == 'f')) {
                        /* Cursor position */
                        if (ansi_position(&count) == Q_FALSE) {
                                break;
                        }
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'm') {
#ifdef DEBUG_ANSI
                        fprintf(ANSI_DEBUG_FILE_HANDLE, "SGR: change text attributes\n");
#endif /* DEBUG_ANSI */
                        /* Text attributes */
                        if (ansi_color(&attributes, &count) == Q_TRUE) {
                                q_current_color = attributes;
                        } else {
                                break;
                        }

                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                /* DSR special case:  it MUST be exactly "<ESC>[6n" */
                if (from_modem == 'n') {
                        /*

                         Device Status Report

                         It was "common knowledge" circa 1992 that the way to
                         "autodetect" ANSI was to issue DSR 6 (Cursor Position).
                         I'm adding support for it here only because it was
                         so frequently used by ANSI BBSes.

                         It was also "common knowledge" to return the SIZE of the
                         screen rather than the current cursor position.  Most of
                         the servers/BBSes just look for the return CSI code.
                         So screw it, I'll actually put the real position in the
                         response.

                         A *real* ANSI X3.64 terminal would respond to lots more
                         DSR's.

                         */
                        if ((q_emul_buffer_n == 3) && (q_emul_buffer[2] == '6')) {
#ifdef DEBUG_ANSI
                                fprintf(ANSI_DEBUG_FILE_HANDLE, "dsr() 6\n");
#endif /* DEBUG_ANSI */
                                memset(response_buffer, 0, sizeof(response_buffer));
                                snprintf(response_buffer, sizeof(response_buffer), "\033[%u;%uR", q_status.cursor_y + 1, q_status.cursor_x + 1);
                                /* Send string directly to remote side */
                                qodem_write(q_child_tty_fd, response_buffer, strlen(response_buffer), Q_TRUE);
                        } else {
#ifdef DEBUG_ANSI
                                fprintf(ANSI_DEBUG_FILE_HANDLE, "dsr() UNSUPPORTED: %sn\n", q_emul_buffer);
#endif /* DEBUG_ANSI */
                        }

                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == '@') {
                        /* ICH */
                        if (ansi_ich(&count) == Q_FALSE) {
                                break;
                        }
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'I') {
                        /* CHT */
                        if (ansi_cht(&count) == Q_FALSE) {
                                break;
                        }
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'd') {
                        /* VPA */
                        if (ansi_vpa(&count) == Q_FALSE) {
                                break;
                        }
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'G') {
                        /* CHA */
                        if (ansi_cha(&count) == Q_FALSE) {
                                break;
                        }
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'L') {
                        /* IL */
                        if (ansi_il(&count) == Q_FALSE) {
                                break;
                        }
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'M') {
                        /* DL */
                        if (ansi_dl(&count) == Q_FALSE) {
                                break;
                        }
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'P') {
                        /* DCH */
                        if (ansi_dch(&count) == Q_FALSE) {
                                break;
                        }
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'b') {
                        /* REP */
                        if (ansi_rep(&count) == Q_FALSE) {
                                break;
                        }
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'h') {
                        /* Set mode */
                        if (ansi_set_mode(&count, Q_TRUE) == Q_FALSE) {
                                break;
                        }
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'l') {
                        /* Reset mode */
                        if (ansi_set_mode(&count, Q_FALSE) == Q_FALSE) {
                                break;
                        }
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == '!') {
                        /* This is a RIPScript query command, discard it */
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                break;

        case SCAN_COUNT_TWO:
                /* We can see ONLY:  digit, ';', H, f, m */
                if (isdigit(from_modem)) {
                        save_char(from_modem, to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == ';') {
                        /* Continue scanning, more numbers coming. */
                        save_char(from_modem, to_screen);
                        scan_state = SCAN_COUNT_MANY;
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if ((from_modem == 'H') || (from_modem == 'f')) {
                        /* Cursor position */
                        if (ansi_position(&count) == Q_FALSE) {
                                break;
                        }
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'm') {
#ifdef DEBUG_ANSI
                        fprintf(ANSI_DEBUG_FILE_HANDLE, "SGR: change text attributes\n");
#endif /* DEBUG_ANSI */
                        /* Text attributes */
                        if (ansi_color(&attributes, &count) == Q_TRUE) {
                                q_current_color = attributes;
                        } else {
                                break;
                        }

                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                break;

        case SCAN_COUNT_MANY:
                /* We can see ONLY:  digit, ';', m */
                if (isdigit(from_modem)) {
                        save_char(from_modem, to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == ';') {
                        /* Continue scanning, more numbers coming. */
                        save_char(from_modem, to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                if (from_modem == 'm') {
                        /* Text attributes */
                        if (ansi_color(&attributes, &count) == Q_TRUE) {
                                q_current_color = attributes;
                        } else {
                                break;
                        }

                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                break;

        case SCAN_MUSIC:

                /* Generally we are looking for ^N or CR */
                if ((from_modem == 0x0E) || (from_modem == C_CR)) {
                        /* Force the screen to refresh before playing the music */
                        q_screen_dirty = Q_TRUE;
                        refresh_handler();

                        play_ansi_music(music_buffer, music_buffer_n, Q_TRUE);
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                music_buffer[music_buffer_n] = from_modem;
                music_buffer_n++;

                if (music_buffer_n >= sizeof(music_buffer)) {
                        /* Throw it away */
                        clear_state(to_screen);
                        return Q_EMUL_FSM_NO_CHAR_YET;
                }

                /* Keep collecting characters */
                return Q_EMUL_FSM_NO_CHAR_YET;
        }

        /* This point means we got most, but not all, of a
           sequence.  Start sending what we had, but kill the ESCAPES
           so they don't interfere with the real console. */

#ifdef DEBUG_ANSI
        fprintf(ANSI_DEBUG_FILE_HANDLE, "UNKNOWN SEQUENCE: \"%s\"\n", q_emul_buffer);
#endif /* DEBUG_ANSI */

        q_emul_buffer[q_emul_buffer_n] = from_modem;
        q_emul_buffer_n++;
        *to_screen = codepage_map_char(q_emul_buffer[q_emul_buffer_i]);
        q_emul_buffer_i++;
        scan_state = SCAN_NONE;

        /* Special case: one character returns Q_EMUL_FSM_ONE_CHAR */
        if (q_emul_buffer_n == 1) {
                q_emul_buffer_i = 0;
                q_emul_buffer_n = 0;
                return Q_EMUL_FSM_ONE_CHAR;
        }

        return Q_EMUL_FSM_MANY_CHARS;
} /* ---------------------------------------------------------------------- */

/*
 * ansi_keystroke
 */
wchar_t * ansi_keystroke(const int keystroke) {

        switch (keystroke) {
        case Q_KEY_BACKSPACE:
                if (q_status.hard_backspace == Q_TRUE) {
                        return L"\010";
                } else {
                        return L"\177";
                }

        case Q_KEY_LEFT:
                return L"\033[D";

        case Q_KEY_RIGHT:
                return L"\033[C";

        case Q_KEY_UP:
                return L"\033[A";

        case Q_KEY_DOWN:
                return L"\033[B";

        case Q_KEY_PPAGE:
                return L"\033[5~";
        case Q_KEY_NPAGE:
                return L"\033[6~";
        case Q_KEY_IC:
                return L"\033[2~";
        case Q_KEY_DC:
                return L"\033[3~";
        case Q_KEY_SIC:
                return L"\033[2;2~";
        case Q_KEY_SDC:
                return L"\033[3;2~";
        case Q_KEY_HOME:
        case Q_KEY_END:
        case Q_KEY_F(1):
        case Q_KEY_F(2):
        case Q_KEY_F(3):
        case Q_KEY_F(4):
        case Q_KEY_F(5):
        case Q_KEY_F(6):
        case Q_KEY_F(7):
        case Q_KEY_F(8):
        case Q_KEY_F(9):
        case Q_KEY_F(10):
        case Q_KEY_F(11):
        case Q_KEY_F(12):
        case Q_KEY_F(13):
        case Q_KEY_F(14):
        case Q_KEY_F(15):
        case Q_KEY_F(16):
        case Q_KEY_F(17):
        case Q_KEY_F(18):
        case Q_KEY_F(19):
        case Q_KEY_F(20):
        case Q_KEY_F(21):
        case Q_KEY_F(22):
        case Q_KEY_F(23):
        case Q_KEY_F(24):
        case Q_KEY_F(25):
        case Q_KEY_F(26):
        case Q_KEY_F(27):
        case Q_KEY_F(28):
        case Q_KEY_F(29):
        case Q_KEY_F(30):
        case Q_KEY_F(31):
        case Q_KEY_F(32):
        case Q_KEY_F(33):
        case Q_KEY_F(34):
        case Q_KEY_F(35):
        case Q_KEY_F(36):
                return L"";

        case Q_KEY_PAD0:
                return L"0";
        case Q_KEY_C1:
        case Q_KEY_PAD1:
                return L"1";
        case Q_KEY_C2:
        case Q_KEY_PAD2:
                return L"2";
        case Q_KEY_C3:
        case Q_KEY_PAD3:
                return L"3";
        case Q_KEY_B1:
        case Q_KEY_PAD4:
                return L"4";
        case Q_KEY_B2:
        case Q_KEY_PAD5:
                return L"5";
        case Q_KEY_B3:
        case Q_KEY_PAD6:
                return L"6";
        case Q_KEY_A1:
        case Q_KEY_PAD7:
                return L"7";
        case Q_KEY_A2:
        case Q_KEY_PAD8:
                return L"8";
        case Q_KEY_A3:
        case Q_KEY_PAD9:
                return L"9";
        case Q_KEY_PAD_STOP:
                return L".";
        case Q_KEY_PAD_SLASH:
                return L"/";
        case Q_KEY_PAD_STAR:
                return L"*";
        case Q_KEY_PAD_MINUS:
                return L"-";
        case Q_KEY_PAD_PLUS:
                return L"+";
        case Q_KEY_PAD_ENTER:
        case Q_KEY_ENTER:
                if (telnet_is_ascii()) {
                        return L"\015\012";
                }
                return L"\015";

        default:
                break;

        }

        return NULL;
} /* ---------------------------------------------------------------------- */
