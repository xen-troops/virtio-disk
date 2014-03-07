/*  
 * Copyright (c) 2014, Citrix Systems Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 * Copyright (c) 2004 Johannes Schindelin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <err.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <rfb/rfb.h>
#include <rfb/keysym.h>

#include "debug.h"
#include "kbd.h"
#include "ps2.h"

//#define DEBUG_LAYOUT

#define SCANCODE_GREY       0x80
#define SCANCODE_EMUL0      0xE0
#define SCANCODE_KEYMASK    0x7F
#define SCANCODE_UP         0x80

#define SCANCODE_SHIFT  0x100
#define SCANCODE_CTRL   0x200
#define SCANCODE_ALT    0x400
#define SCANCODE_ALTGR  0x800

#define MAX_NORMAL_SYM  512
#define MAX_EXTRA_SYM   256

typedef struct kbd_extra {
    rfbKeySym   sym;
    uint16_t    code;
} kbd_extra_t;

typedef struct kbd_range {
    struct kbd_range    *next;
    int                 start;
    int                 end;
} kbd_range_t;

typedef struct kbd_layout {
    uint16_t        sym2code[MAX_NORMAL_SYM];
    kbd_extra_t     sym2code_extra[MAX_EXTRA_SYM];
    int             extra;
    kbd_range_t     *numlock_range;
} kbd_layout_t;

typedef struct kbd {
    kbd_layout_t    layout;
    int             shift;
    int             numlock;
    int             capslock;
} kbd_t;

static kbd_t    kbd_state;

typedef struct kbd_name2sym_entry_t {
    const char  *name;
    rfbKeySym   value;
} kbd_name2sym_entry_t;

#define _ENTRY(_name)    { #_name, XK_ ## _name }

static kbd_name2sym_entry_t   name2keysym[] = {
    _ENTRY(Select),
    _ENTRY(Print),
    _ENTRY(Execute),
    _ENTRY(Insert),
    _ENTRY(Undo),
    _ENTRY(Redo),
    _ENTRY(Menu),
    _ENTRY(Find),
    _ENTRY(Cancel),
    _ENTRY(Help),
    _ENTRY(Break),
    _ENTRY(Num_Lock),
    _ENTRY(KP_Enter),
    _ENTRY(KP_Home),
    _ENTRY(KP_Left),
    _ENTRY(KP_Up),
    _ENTRY(KP_Right),
    _ENTRY(KP_Down),
    _ENTRY(KP_Prior),
    _ENTRY(KP_Page_Up),
    _ENTRY(KP_Next),
    _ENTRY(KP_Page_Down),
    _ENTRY(KP_End),
    _ENTRY(KP_Begin),
    _ENTRY(KP_Insert),
    _ENTRY(KP_Delete),
    _ENTRY(KP_Equal),
    _ENTRY(KP_Multiply),
    _ENTRY(KP_Add),
    _ENTRY(KP_Separator),
    _ENTRY(KP_Subtract),
    _ENTRY(KP_Decimal),
    _ENTRY(KP_Divide),
    _ENTRY(KP_0),
    _ENTRY(KP_1),
    _ENTRY(KP_2),
    _ENTRY(KP_3),
    _ENTRY(KP_4),
    _ENTRY(KP_5),
    _ENTRY(KP_6),
    _ENTRY(KP_7),
    _ENTRY(KP_8),
    _ENTRY(KP_9),
    _ENTRY(Shift_L),
    _ENTRY(Shift_R),
    _ENTRY(Control_L),
    _ENTRY(Control_R),
    _ENTRY(Caps_Lock),
    _ENTRY(Shift_Lock),
    _ENTRY(Meta_L),
    _ENTRY(Meta_R),
    _ENTRY(Alt_L),
    _ENTRY(Alt_R),
    _ENTRY(Super_L),
    _ENTRY(Super_R),
    _ENTRY(Hyper_L),
    _ENTRY(Hyper_R),
    _ENTRY(space),
    _ENTRY(exclam),
    _ENTRY(quotedbl),
    _ENTRY(numbersign),
    _ENTRY(dollar),
    _ENTRY(percent),
    _ENTRY(ampersand),
    _ENTRY(apostrophe),
    _ENTRY(parenleft),
    _ENTRY(parenright),
    _ENTRY(asterisk),
    _ENTRY(plus),
    _ENTRY(comma),
    _ENTRY(minus),
    _ENTRY(period),
    _ENTRY(slash),
    _ENTRY(0),
    _ENTRY(1),
    _ENTRY(2),
    _ENTRY(3),
    _ENTRY(4),
    _ENTRY(5),
    _ENTRY(6),
    _ENTRY(7),
    _ENTRY(8),
    _ENTRY(9),
    _ENTRY(colon),
    _ENTRY(semicolon),
    _ENTRY(less),
    _ENTRY(equal),
    _ENTRY(greater),
    _ENTRY(question),
    _ENTRY(at),
    _ENTRY(A),
    _ENTRY(B),
    _ENTRY(C),
    _ENTRY(D),
    _ENTRY(E),
    _ENTRY(F),
    _ENTRY(G),
    _ENTRY(H),
    _ENTRY(I),
    _ENTRY(J),
    _ENTRY(K),
    _ENTRY(L),
    _ENTRY(M),
    _ENTRY(N),
    _ENTRY(O),
    _ENTRY(P),
    _ENTRY(Q),
    _ENTRY(R),
    _ENTRY(S),
    _ENTRY(T),
    _ENTRY(U),
    _ENTRY(V),
    _ENTRY(W),
    _ENTRY(X),
    _ENTRY(Y),
    _ENTRY(Z),
    _ENTRY(bracketleft),
    _ENTRY(backslash),
    _ENTRY(bracketright),
    _ENTRY(asciicircum),
    _ENTRY(underscore),
    _ENTRY(grave),
    _ENTRY(a),
    _ENTRY(b),
    _ENTRY(c),
    _ENTRY(d),
    _ENTRY(e),
    _ENTRY(f),
    _ENTRY(g),
    _ENTRY(h),
    _ENTRY(i),
    _ENTRY(j),
    _ENTRY(k),
    _ENTRY(l),
    _ENTRY(m),
    _ENTRY(n),
    _ENTRY(o),
    _ENTRY(p),
    _ENTRY(q),
    _ENTRY(r),
    _ENTRY(s),
    _ENTRY(t),
    _ENTRY(u),
    _ENTRY(v),
    _ENTRY(w),
    _ENTRY(x),
    _ENTRY(y),
    _ENTRY(z),
    _ENTRY(braceleft),
    _ENTRY(bar),
    _ENTRY(braceright),
    _ENTRY(asciitilde),
    _ENTRY(nobreakspace),
    _ENTRY(exclamdown),
    _ENTRY(cent),
    _ENTRY(sterling),
    _ENTRY(currency),
    _ENTRY(yen),
    _ENTRY(brokenbar),
    _ENTRY(section),
    _ENTRY(diaeresis),
    _ENTRY(copyright),
    _ENTRY(ordfeminine),
    _ENTRY(guillemotleft),
    _ENTRY(notsign),
    _ENTRY(hyphen),
    _ENTRY(registered),
    _ENTRY(macron),
    _ENTRY(degree),
    _ENTRY(plusminus),
    _ENTRY(twosuperior),
    _ENTRY(threesuperior),
    _ENTRY(acute),
    _ENTRY(mu),
    _ENTRY(paragraph),
    _ENTRY(periodcentered),
    _ENTRY(cedilla),
    _ENTRY(onesuperior),
    _ENTRY(masculine),
    _ENTRY(guillemotright),
    _ENTRY(onequarter),
    _ENTRY(onehalf),
    _ENTRY(threequarters),
    _ENTRY(questiondown),
    _ENTRY(Agrave),
    _ENTRY(Aacute),
    _ENTRY(Acircumflex),
    _ENTRY(Atilde),
    _ENTRY(Adiaeresis),
    _ENTRY(Aring),
    _ENTRY(AE),
    _ENTRY(Ccedilla),
    _ENTRY(Egrave),
    _ENTRY(Eacute),
    _ENTRY(Ecircumflex),
    _ENTRY(Ediaeresis),
    _ENTRY(Igrave),
    _ENTRY(Iacute),
    _ENTRY(Icircumflex),
    _ENTRY(Idiaeresis),
    _ENTRY(ETH),
    _ENTRY(Eth),
    _ENTRY(Ntilde),
    _ENTRY(Ograve),
    _ENTRY(Oacute),
    _ENTRY(Ocircumflex),
    _ENTRY(Otilde),
    _ENTRY(Odiaeresis),
    _ENTRY(multiply),
    _ENTRY(Ooblique),
    _ENTRY(Ugrave),
    _ENTRY(Uacute),
    _ENTRY(Ucircumflex),
    _ENTRY(Udiaeresis),
    _ENTRY(Yacute),
    _ENTRY(THORN),
    _ENTRY(Thorn),
    _ENTRY(ssharp),
    _ENTRY(agrave),
    _ENTRY(aacute),
    _ENTRY(acircumflex),
    _ENTRY(atilde),
    _ENTRY(adiaeresis),
    _ENTRY(aring),
    _ENTRY(ae),
    _ENTRY(ccedilla),
    _ENTRY(egrave),
    _ENTRY(eacute),
    _ENTRY(ecircumflex),
    _ENTRY(ediaeresis),
    _ENTRY(igrave),
    _ENTRY(iacute),
    _ENTRY(icircumflex),
    _ENTRY(idiaeresis),
    _ENTRY(eth),
    _ENTRY(ntilde),
    _ENTRY(ograve),
    _ENTRY(oacute),
    _ENTRY(ocircumflex),
    _ENTRY(otilde),
    _ENTRY(odiaeresis),
    _ENTRY(division),
    _ENTRY(oslash),
    _ENTRY(ugrave),
    _ENTRY(uacute),
    _ENTRY(ucircumflex),
    _ENTRY(udiaeresis),
    _ENTRY(yacute),
    _ENTRY(thorn),
    _ENTRY(ydiaeresis),
    _ENTRY(BackSpace),
    _ENTRY(Tab),
    _ENTRY(Return),
    _ENTRY(Right),
    _ENTRY(Left),
    _ENTRY(Up),
    _ENTRY(Down),
    _ENTRY(Page_Down),
    _ENTRY(Page_Up),
    _ENTRY(Insert),
    _ENTRY(Delete),
    _ENTRY(Home),
    _ENTRY(End),
    _ENTRY(F1),
    _ENTRY(F2),
    _ENTRY(F3),
    _ENTRY(F4),
    _ENTRY(F5),
    _ENTRY(F6),
    _ENTRY(F7),
    _ENTRY(F8),
    _ENTRY(F9),
    _ENTRY(F10),
    _ENTRY(F11),
    _ENTRY(F12),
    _ENTRY(F13),
    _ENTRY(F14),
    _ENTRY(F15),
    _ENTRY(F16),
    _ENTRY(F17),
    _ENTRY(F18),
    _ENTRY(F19),
    _ENTRY(F20),
    _ENTRY(F21),
    _ENTRY(F22),
    _ENTRY(F23),
    _ENTRY(F24),
    _ENTRY(Escape),
    _ENTRY(Mode_switch),
    _ENTRY(ISO_Level3_Shift),
    _ENTRY(ISO_Left_Tab),
    _ENTRY(Sys_Req),
    _ENTRY(Scroll_Lock),
    _ENTRY(Multi_key),
};

#undef  _ENTRY

#ifdef  DEBUG_LAYOUT
static const char *
kbd_sym2name(rfbKeySym keySym)
{
    int i;

    for (i = 0; i < sizeof (name2keysym) / sizeof (name2keysym[0]); i++) {
        kbd_name2sym_entry_t *entry = &name2keysym[i];

        if (entry->value == keySym)
            return entry->name;
    }

    return NULL;
}
#endif

static rfbKeySym
kbd_name2sym(const char *name)
{
    int i;

    for (i = 0; i < sizeof (name2keysym) / sizeof (name2keysym[0]); i++) {
        kbd_name2sym_entry_t *entry = &name2keysym[i];

        if (!strcmp(entry->name, name))
            return entry->value;
    }

    return XK_VoidSymbol;
}

static int
kbd_range_add(kbd_range_t **rangep, int val)
{
    kbd_range_t *range;

    for (range = *rangep; range != NULL; range = range->next) {
        if (val >= range->start && val <= range->end)
            break;

        if (val == range->start - 1) {
            range->start--;
            break;
        } else if (val == range->end + 1) {
            range->end++;
            break;
        }
    }

    if (range != NULL)
        goto done;

	range = malloc(sizeof(kbd_range_t));
    if (range == NULL)
        goto fail1;

    range->start = range->end = val;
    range->next = *rangep;
    *rangep = range;

done:
    return 0;

fail1:
    DBG("fail1\n");
    warn("fail");

    return -1;
}

static void
kbd_range_free(kbd_range_t **rangep)
{
    kbd_range_t *range;

    while ((range = *rangep) != NULL) {
        *rangep = range->next;
        free(range);
    }
}

static void
kbd_sym2code_add(const char *name, rfbKeySym sym, uint16_t code)
{
    kbd_layout_t    *layout = &kbd_state.layout;

#ifdef  DEBUG_LAYOUT
    DBG("%08X(%s) -> %04X\n", sym, name, code);
#endif

    if (sym < MAX_NORMAL_SYM) {
        layout->sym2code[sym] = code;
        goto done;
    }

    assert(layout->extra <= MAX_EXTRA_SYM);
    if (layout->extra == MAX_EXTRA_SYM)
        goto fail1;

    layout->sym2code_extra[layout->extra].sym = sym;
    layout->sym2code_extra[layout->extra].code = code;
    layout->extra++;

done:
    return;

fail1:
    DBG("fail1\n");
}

static uint16_t
kbd_sym2code(rfbKeySym sym)
{
    kbd_layout_t    *layout = &kbd_state.layout;
    uint16_t        code;

    if (sym < MAX_NORMAL_SYM) {
        code = layout->sym2code[sym];
    } else {
        int i;

        code = 0xFFFF;
        for (i = 0; i < layout->extra; i++) {
            if (sym == layout->sym2code_extra[i].sym) {
                code = layout->sym2code_extra[i].code;
                break;
            }
        }
    }

    return code;
}


#define MAX_LINE_LENGTH 1024

static int
kbd_parse_layout(const char *language)
{
    char    *name;
    FILE    *f;
    int     rc;

    rc = asprintf(&name, "keymaps/%s", language);
    if (rc < 0)
        goto fail1;

    f = fopen(name, "r");
    if (f == NULL)
        goto fail2;

    for (;;) {
        char        line[MAX_LINE_LENGTH];
        const char  *delim = " \t\n";
        char        *tok;
        char        *name;
        const char  *modifier;
        rfbKeySym   sym;
        uint16_t    code;
        int         addupper;

        memset(line, 0, sizeof (line));

        if (fgets(line, sizeof (line), f) == NULL)
            break;

        if (line[0] == '#')
            continue;

        tok = strtok(line, delim);
        if (tok == NULL)
            continue;

        if (!strcmp(tok, "map"))
            continue;

        if (!strcmp(tok, "include")) {
            kbd_parse_layout(strtok(NULL, delim));
            continue;
        }

        name = tok;

        if (!strcmp(name, "ISO_Left_Tab"))
            name = "Tab";

        sym = kbd_name2sym(name);
        if (sym == XK_VoidSymbol) {
            DBG("unknown keysym %s\n", name);
            continue;
        }

        tok = strtok(NULL, delim);
        if (tok == NULL)
            continue;

        code = strtol(tok, NULL, 0);

        tok = strtok(NULL, delim);

        modifier = tok;

        addupper = FALSE;
        if (modifier != NULL) {
            if (!strcmp(tok, "numlock")) {
                rc = kbd_range_add(&kbd_state.layout.numlock_range, sym);
                if (rc < 0)
                    goto fail3;
            }

            if (!strcmp(tok, "shift")) {
                code |= SCANCODE_SHIFT;
            } else if (!strcmp(tok, "altgr")) {
                code |= SCANCODE_ALTGR;
            } else if (!strcmp(tok, "ctrl")) {
                code |= SCANCODE_CTRL;
            } else if (!strcmp(modifier, "addupper")) {
                addupper = TRUE;
            }
        }

        kbd_sym2code_add(name, sym, code);

        if (addupper) {
            char *c;

            for (c = name; *c != '\0'; c++)
                *c = toupper(*c);

            sym = kbd_name2sym(name);
            if (sym != XK_VoidSymbol)
                kbd_sym2code_add(name, sym, code | SCANCODE_SHIFT);

        }
    }

    fclose(f);
    free(name);

    return 0;

fail3:
    DBG("fail3\n");

    kbd_range_free(&kbd_state.layout.numlock_range);

    fclose(f);

fail2:
    DBG("fail2\n");

    free(name);

fail1:
    DBG("fail1\n");

    return -1;
}

static int
kbd_sym_is_numlocked(rfbKeySym sym)
{
    kbd_range_t *range; 

    for (range = kbd_state.layout.numlock_range;
         range != NULL;
         range = range->next) {
        if (sym >= range->start && sym <= range->end)
            return 1;
    }

    return 0;
}

static int
kbd_sym_is_uppercase(rfbKeySym sym)
{
    return !!(sym >= XK_A && sym <= XK_Z);
}

static void
kbd_press(rfbKeySym sym)
{
    uint16_t    code = kbd_sym2code(sym);

    if (code & SCANCODE_GREY)
        ps2_kbd_event(SCANCODE_EMUL0);

    ps2_kbd_event(code & SCANCODE_KEYMASK);
}

static void
kbd_release(rfbKeySym sym)
{
    uint16_t    code = kbd_sym2code(sym);

    if (code & SCANCODE_GREY)
        ps2_kbd_event(SCANCODE_EMUL0);

    ps2_kbd_event((code & SCANCODE_KEYMASK) | SCANCODE_UP);
}

void
kbd_event(rfbKeySym sym, int down)
{
    switch (sym) {
    case XK_Shift_L:
    case XK_Shift_R:
        kbd_state.shift = !!down;
        break;

    case XK_Num_Lock:
        kbd_state.numlock ^= !!down;
        break;

    case XK_Caps_Lock:
        kbd_state.capslock ^= !!down;
        break;
    }

    /* Check whether we missed a numlock or capslock press */
    if (down) {
        int numlock = kbd_sym_is_numlocked(sym);
        int capslock = kbd_sym_is_uppercase(sym) && !kbd_state.shift;

        if (kbd_state.numlock != numlock) {
            kbd_press(XK_Num_Lock);
            kbd_release(XK_Num_Lock);
            kbd_state.numlock = numlock;
        }
        
        if (kbd_state.capslock != capslock) {
            kbd_press(XK_Caps_Lock);
            kbd_release(XK_Caps_Lock);
            kbd_state.capslock = capslock;
        }

        kbd_press(sym);
    } else {
        kbd_release(sym);
    }
}

int
kbd_initialize(const char *language)
{
    int rc;

    memset(kbd_state.layout.sym2code,
           0xFF,
           sizeof (kbd_state.layout.sym2code));

    rc = kbd_parse_layout(language);
    if (rc < 0)
        goto fail1;

    return 0;

fail1:
    DBG("fail1\n");

    return -1;
}

void
kbd_teardown(void)
{
    kbd_range_free(&kbd_state.layout.numlock_range);
}

/*
 * Local variables:
 * mode: C
 * c-tab-always-indent: nil
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * c-basic-indent: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
