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
 * Copyright (c) 2003 Fabrice Bellard
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
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <xenctrl.h>

#include "debug.h"
#include "ps2.h"
#include "demu.h"

#define PS2_CMD_READ_MODE	    0x20	/* Read mode bits */
#define PS2_CMD_WRITE_MODE	    0x60	/* Write mode bits */
#define PS2_CMD_GET_VERSION	    0xA1	/* Get controller version */
#define PS2_CMD_AUX_DISABLE	    0xA7	/* Disable mouse interface */
#define PS2_CMD_AUX_ENABLE	    0xA8	/* Enable mouse interface */
#define PS2_CMD_AUX_TEST	    0xA9	/* Mouse interface test */
#define PS2_CMD_SELF_TEST	    0xAA	/* Controller self test */
#define PS2_CMD_KBD_TEST	    0xAB	/* Keyboard interface test */
#define PS2_CMD_KBD_DISABLE	    0xAD	/* Keyboard interface disable */
#define PS2_CMD_KBD_ENABLE	    0xAE	/* Keyboard interface enable */
#define PS2_CMD_READ_INPORT     0xC0    /* read input port */
#define PS2_CMD_READ_OUTPORT	0xD0    /* read output port */
#define PS2_CMD_WRITE_OUTPORT	0xD1    /* write output port */
#define PS2_CMD_WRITE_KBD_OBUF  0xD2
#define PS2_CMD_WRITE_AUX_OBUF	0xD3    /* Write to output buffer as if
                                           initiated by the auxiliary device */
#define PS2_CMD_WRITE_AUX	    0xD4	/* Write the following byte to the mouse */
#define PS2_CMD_DISABLE_A20     0xDD    /* HP vectra only ? */
#define PS2_CMD_ENABLE_A20      0xDF    /* HP vectra only ? */
#define PS2_CMD_PULSE_BITS_3_0  0xF0    /* Pulse bits 3-0 of the output port P2. */
#define PS2_CMD_RESET           0xFE    /* Pulse bit 0 of the output port P2 = CPU reset. */
#define PS2_CMD_NO_OP           0xFF    /* Pulse no bits of the output port P2. */

static const char *
ps2_cmd(uint8_t val)
{
#define PS2_CMD(_cmd)   \
    case PS2_CMD_ ## _cmd:  \
        return #_cmd;

    switch (val) {
    PS2_CMD(READ_MODE);
    PS2_CMD(WRITE_MODE);
    PS2_CMD(GET_VERSION);
    PS2_CMD(AUX_DISABLE);
    PS2_CMD(AUX_ENABLE);
    PS2_CMD(AUX_TEST);
    PS2_CMD(SELF_TEST);
    PS2_CMD(KBD_TEST);
    PS2_CMD(KBD_DISABLE);
    PS2_CMD(KBD_ENABLE);
    PS2_CMD(READ_INPORT);
    PS2_CMD(READ_OUTPORT);
    PS2_CMD(WRITE_OUTPORT);
    PS2_CMD(WRITE_KBD_OBUF);
    PS2_CMD(WRITE_AUX_OBUF);
    PS2_CMD(WRITE_AUX);
    PS2_CMD(DISABLE_A20);
    PS2_CMD(ENABLE_A20);
    PS2_CMD(PULSE_BITS_3_0);
    PS2_CMD(RESET);
    PS2_CMD(NO_OP);
    default:
        break;
    }

    return "UNKNOWN";

#undef  PS2_CMD
}

#define PS2_STAT_KBD_OBF 		0x01	/* Keyboard output buffer full */
#define PS2_STAT_KBD_IBF 		0x02	/* Keyboard input buffer full */
#define PS2_STAT_SELFTEST	    0x04	/* Self test successful */
#define PS2_STAT_CMD		    0x08	/* Last write was a command write (0=data) */
#define PS2_STAT_UNLOCKED	    0x10	/* Zero if keyboard locked */
#define PS2_STAT_AUX_OBF	    0x20	/* Mouse output buffer full */
#define PS2_STAT_GTO 		    0x40	/* General receive/xmit timeout */
#define PS2_STAT_PERR 		    0x80	/* Parity error */

#define PS2_MODE_KBD_INT	    0x01	/* Keyboard data generate IRQ1 */
#define PS2_MODE_AUX_INT	    0x02	/* Mouse data generate IRQ12 */
#define PS2_MODE_SYS 		    0x04	/* The system flag (?) */
#define PS2_MODE_NO_KEYLOCK	    0x08	/* The keylock doesn't affect the keyboard if set */
#define PS2_MODE_DISABLE_KBD	0x10	/* Disable keyboard interface */
#define PS2_MODE_DISABLE_AUX	0x20	/* Disable mouse interface */
#define PS2_MODE_KCC 		    0x40	/* Scan code conversion to PC format */
#define PS2_MODE_RFU		    0x80

#define PS2_OUT_RESET           0x01    /* 1=normal mode, 0=reset */
#define PS2_OUT_A20             0x02    /* x86 only */
#define PS2_OUT_KBD_OBF         0x10    /* Keyboard output buffer full */
#define PS2_OUT_AUX_OBF         0x20    /* Mouse output buffer full */

#define PS2_PENDING_KBD         1
#define PS2_PENDING_AUX         2

#define PS2_KBD_IRQ             1
#define PS2_AUX_IRQ             12

#define KBD_SET_LEDS	        0xED	/* Set keyboard leds */
#define KBD_ECHO     	        0xEE
#define KBD_SCANCODE	        0xF0	/* Get/set scancode set */
#define KBD_GET_ID 	            0xF2	/* get keyboard ID */
#define KBD_SET_RATE	        0xF3	/* Set typematic rate */
#define KBD_ENABLE		        0xF4	/* Enable scanning */
#define KBD_RESET_DISABLE	    0xF5	/* reset and disable scanning */
#define KBD_RESET_ENABLE   	    0xF6    /* reset and enable scanning */
#define KBD_RESET		        0xFF	/* Reset */

static const char *
kbd_cmd(uint8_t val)
{
#define KBD_CMD(_cmd)   \
    case KBD_ ## _cmd:  \
        return #_cmd;

    switch (val) {
    KBD_CMD(SET_LEDS);
    KBD_CMD(ECHO);
    KBD_CMD(SCANCODE);
    KBD_CMD(GET_ID);
    KBD_CMD(SET_RATE);
    KBD_CMD(ENABLE);
    KBD_CMD(RESET_DISABLE);
    KBD_CMD(RESET_ENABLE);
    KBD_CMD(RESET);
    default:
        break;
    }

    return "UNKNOWN";

#undef  KBD_CMD
}

#define KBD_REPLY_POR		    0xAA	/* Power on reset */
#define KBD_REPLY_ID		    0xAB	/* Keyboard ID */
#define KBD_REPLY_ACK		    0xFA	/* Command ACK */
#define KBD_REPLY_RESEND	    0xFE	/* Command NACK, send the cmd again */

#define AUX_SET_SCALE11		    0xE6	/* Set 1:1 scaling */
#define AUX_SET_SCALE21		    0xE7	/* Set 2:1 scaling */
#define AUX_SET_RES		        0xE8	/* Set resolution */
#define AUX_GET_SCALE		    0xE9	/* Get scaling factor */
#define AUX_SET_STREAM		    0xEA	/* Set stream mode */
#define AUX_POLL		        0xEB	/* Poll */
#define AUX_RESET_WRAP		    0xEC	/* Reset wrap mode */
#define AUX_SET_WRAP		    0xEE	/* Set wrap mode */
#define AUX_SET_REMOTE		    0xF0	/* Set remote mode */
#define AUX_GET_TYPE		    0xF2	/* Get type */
#define AUX_SET_SAMPLE		    0xF3	/* Set sample rate */
#define AUX_ENABLE_DEV		    0xF4	/* Enable aux device */
#define AUX_DISABLE_DEV		    0xF5	/* Disable aux device */
#define AUX_SET_DEFAULT		    0xF6
#define AUX_RESET		        0xFF	/* Reset aux device */
#define AUX_ACK			        0xFA	/* Command byte ACK. */

static const char *
aux_cmd(uint8_t val)
{
#define AUX_CMD(_cmd)   \
    case AUX_ ## _cmd:  \
        return #_cmd;

    switch (val) {
    AUX_CMD(SET_SCALE11);
    AUX_CMD(SET_SCALE21);
    AUX_CMD(SET_RES);
    AUX_CMD(GET_SCALE);
    AUX_CMD(SET_STREAM);
    AUX_CMD(POLL);
    AUX_CMD(RESET_WRAP);
    AUX_CMD(SET_WRAP);
    AUX_CMD(SET_REMOTE);
    AUX_CMD(GET_TYPE);
    AUX_CMD(SET_SAMPLE);
    AUX_CMD(ENABLE_DEV);
    AUX_CMD(DISABLE_DEV);
    AUX_CMD(SET_DEFAULT);
    AUX_CMD(RESET);
    default:
        break;
    }

    return "UNKNOWN";

#undef  AUX_CMD
}

#define AUX_STATUS_REMOTE       0x40
#define AUX_STATUS_ENABLED      0x20
#define AUX_STATUS_SCALE21      0x10

typedef struct ps2 {
    uint8_t cmd;    /* if non zero, write data to port 60 is expected */
    uint8_t status;
    uint8_t mode;
    uint8_t outport;

    /* Bitmask of devices with data available.  */
    uint8_t pending;
} ps2_t;

static ps2_t    ps2_state;

#define PS2_QUEUE_SIZE 256

typedef struct ps2_queue {
    uint8_t data[PS2_QUEUE_SIZE];
    int     rptr;
    int     wptr;
    int     count;
} ps2_queue_t;

typedef struct kbd {
    ps2_queue_t queue;
    int         cmd;
    int         scan_enabled;
    int         translate;
    int         scancode_set; /* 1=XT, 2=AT, 3=PS/2 */
    int         ledstate;
} kbd_t;

static kbd_t    kbd_state;

static const uint8_t ps2_raw_keycode[] = {
  0, 118,  22,  30,  38,  37,  46,  54,  61,  62,  70,  69,  78,  85, 102,  13,
 21,  29,  36,  45,  44,  53,  60,  67,  68,  77,  84,  91,  90,  20,  28,  27,
 35,  43,  52,  51,  59,  66,  75,  76,  82,  14,  18,  93,  26,  34,  33,  42,
 50,  49,  58,  65,  73,  74,  89, 124,  17,  41,  88,   5,   6,   4,  12,   3,
 11,   2,  10,   1,   9, 119, 126, 108, 117, 125, 123, 107, 115, 116, 121, 105,
114, 122, 112, 113, 127,  96,  97, 120,   7,  15,  23,  31,  39,  47,  55,  63,
 71,  79,  86,  94,   8,  16,  24,  32,  40,  48,  56,  64,  72,  80,  87, 111,
 19,  25,  57,  81,  83,  92,  95,  98,  99, 100, 101, 103, 104, 106, 109, 110
};

static const uint8_t ps2_raw_keycode_set3[] = {
  0,   8,  22,  30,  38,  37,  46,  54,  61,  62,  70,  69,  78,  85, 102,  13,
 21,  29,  36,  45,  44,  53,  60,  67,  68,  77,  84,  91,  90,  17,  28,  27,
 35,  43,  52,  51,  59,  66,  75,  76,  82,  14,  18,  92,  26,  34,  33,  42,
 50,  49,  58,  65,  73,  74,  89, 126,  25,  41,  20,   7,  15,  23,  31,  39,
 47,   2,  63,  71,  79, 118,  95, 108, 117, 125, 132, 107, 115, 116, 124, 105,
114, 122, 112, 113, 127,  96,  97,  86,  94,  15,  23,  31,  39,  47,  55,  63,
 71,  79,  86,  94,   8,  16,  24,  32,  40,  48,  56,  64,  72,  80,  87, 111,
 19,  25,  57,  81,  83,  92,  95,  98,  99, 100, 101, 103, 104, 106, 109, 110
};

typedef struct aux {
    ps2_queue_t queue;
    int         cmd;
    uint8_t     status;
    uint8_t     resolution;
    uint8_t     sample_rate;
    uint8_t     wrap;
    uint8_t     type; /* 0 = PS2, 3 = IMPS/2, 4 = IMEX */
    uint8_t     detect_state;
    int         dx; /* current values, needed for 'poll' mode */
    int         dy;
    int         dz;
    int         lb;
    int         mb;
    int         rb;
} aux_t;

static aux_t    aux_state;

static void
ps2_update_irq(void)
{
    int kbd_level, aux_level;

    kbd_level = 0;
    aux_level = 0;

    ps2_state.status &= ~(PS2_STAT_KBD_OBF | PS2_STAT_AUX_OBF);
    ps2_state.outport &= ~(PS2_OUT_KBD_OBF | PS2_OUT_AUX_OBF);

    if (ps2_state.pending) {
        ps2_state.status |= PS2_STAT_KBD_OBF;
        ps2_state.outport |= PS2_OUT_KBD_OBF;

        /* kbd data takes priority over aux data.  */
        if (ps2_state.pending == PS2_PENDING_AUX) {
            ps2_state.status |= PS2_STAT_AUX_OBF;
            ps2_state.outport |= PS2_OUT_AUX_OBF;
            if (ps2_state.mode & PS2_MODE_AUX_INT)
                aux_level = 1;
        } else {
            if ((ps2_state.mode & PS2_MODE_KBD_INT) &&
                !(ps2_state.mode & PS2_MODE_DISABLE_KBD))
                kbd_level = 1;
        }
    }

    demu_set_irq(PS2_KBD_IRQ, kbd_level);
    demu_set_irq(PS2_AUX_IRQ, aux_level);
}

static void
ps2_putq(ps2_queue_t *q, uint8_t val)
{
    if (q->count >= PS2_QUEUE_SIZE)
        return;
    q->data[q->wptr] = val;
    if (++q->wptr == PS2_QUEUE_SIZE)
        q->wptr = 0;
    q->count++;
}

static void
kbd_putq(uint8_t val)
{
    DBG("%02x\n", val);
    
    ps2_putq(&kbd_state.queue, val);
    ps2_state.pending |= PS2_PENDING_KBD;
    ps2_update_irq();
}

static void
aux_putq(uint8_t val)
{
    DBG("%02x\n", val);

    ps2_putq(&aux_state.queue, val);
    ps2_state.pending |= PS2_PENDING_AUX;
    ps2_update_irq();
}

static uint8_t
ps2_getq(ps2_queue_t *q)
{
    uint8_t val;

    if (q->count == 0) {
        int i;

        i = q->rptr - 1;
        if (i < 0)
            i = PS2_QUEUE_SIZE - 1;
        val = q->data[i];
    } else {
        val = q->data[q->rptr];
        if (++q->rptr == PS2_QUEUE_SIZE)
            q->rptr = 0;
        q->count--;
    }

    return val;
}

static uint8_t
kbd_getq(void)
{
    uint8_t val = ps2_getq(&kbd_state.queue);

    ps2_state.pending &= ~PS2_PENDING_KBD;
    ps2_update_irq();

    if (kbd_state.queue.count != 0) {
        ps2_state.pending |= PS2_PENDING_KBD;
        ps2_update_irq();
    }

    return val;
}

static uint8_t
aux_getq(void)
{
    uint8_t val = ps2_getq(&aux_state.queue);

    ps2_state.pending &= ~PS2_PENDING_AUX;
    ps2_update_irq();

    if (aux_state.queue.count != 0) {
        ps2_state.pending |= PS2_PENDING_AUX;
        ps2_update_irq();
    }

    return val;
}

static uint8_t
ps2_read_data(void *priv, uint64_t addr)
{
    uint8_t val;

    if (ps2_state.pending == PS2_PENDING_AUX)
        val = aux_getq();
    else
        val = kbd_getq();

    return val;
}

static void
kbd_put_keycode(uint8_t keycode)
{
    if (!kbd_state.translate &&
        keycode < 0xe0 &&
        kbd_state.scancode_set > 1) {
        if (keycode & 0x80)
            kbd_putq(0xf0);
        if (kbd_state.scancode_set == 2)
            keycode = ps2_raw_keycode[keycode & 0x7f];
        else if (kbd_state.scancode_set == 3)
            keycode = ps2_raw_keycode_set3[keycode & 0x7f];
    }
    kbd_putq(keycode);
}

static void
kbd_reset(void)
{
    kbd_state.scan_enabled = 1;
    kbd_state.scancode_set = 2;
    kbd_state.ledstate = 0;
}

static void
kbd_write(uint8_t val)
{
    switch(kbd_state.cmd) {
    default:
    case -1:
        DBG("%s\n", kbd_cmd(val));

        switch (val) {
        case 0x00:
            kbd_putq(KBD_REPLY_ACK);
            break;
        case 0x05:
            kbd_putq(KBD_REPLY_RESEND);
            break;
        case KBD_GET_ID:
            kbd_putq(KBD_REPLY_ACK);
            /* We emulate a MF2 AT keyboard here */
            kbd_putq(KBD_REPLY_ID);
            if (kbd_state.translate)
                kbd_putq(0x41);
            else
                kbd_putq(0x83);
            break;
        case KBD_ECHO:
            kbd_putq(KBD_ECHO);
            break;
        case KBD_ENABLE:
            kbd_state.scan_enabled = 1;
            kbd_putq(KBD_REPLY_ACK);
            break;
        case KBD_SCANCODE:
        case KBD_SET_LEDS:
        case KBD_SET_RATE:
            kbd_state.cmd = val;
            kbd_putq(KBD_REPLY_ACK);
            break;
        case KBD_RESET_DISABLE:
            kbd_reset();
            kbd_state.scan_enabled = 0;
            kbd_putq(KBD_REPLY_ACK);
            break;
        case KBD_RESET_ENABLE:
            kbd_reset();
            kbd_state.scan_enabled = 1;
            kbd_putq(KBD_REPLY_ACK);
            break;
        case KBD_RESET:
            kbd_reset();
            kbd_putq(KBD_REPLY_ACK);
            kbd_putq(KBD_REPLY_POR);
            break;
        default:
            kbd_putq(KBD_REPLY_ACK);
            break;
        }
        break;

    case KBD_SCANCODE:
        if (val == 0) {
            DBG("get scancode set (%d)\n", kbd_state.scancode_set);
            if (kbd_state.scancode_set == 1)
                kbd_put_keycode(0x43);
            else if (kbd_state.scancode_set == 2)
                kbd_put_keycode(0x41);
            else if (kbd_state.scancode_set == 3)
                kbd_put_keycode(0x3f);
        } else {
            if (val >= 1 && val <= 3) {
                DBG("set scancode set (%d)\n", val);
                kbd_state.scancode_set = val;
            }
            kbd_putq(KBD_REPLY_ACK);
        }
        kbd_state.cmd = -1;
        break;

    case KBD_SET_LEDS:
        DBG("ledstate = %02x\n", val);
        kbd_state.ledstate = val;
        kbd_putq(KBD_REPLY_ACK);
        kbd_state.cmd = -1;
        break;

    case KBD_SET_RATE:
        DBG("rate = %02x\n", val);
        kbd_putq(KBD_REPLY_ACK);
        kbd_state.cmd = -1;
        break;
    }
}

static void
aux_send_packet(void)
{
    int dx, dy, dz;
    uint8_t buttons;
    uint8_t val;

    dx = aux_state.dx;
    dy = aux_state.dy;
    dz = aux_state.dz;
    buttons = (!!aux_state.lb) | (!!aux_state.rb < 1) | (!!aux_state.mb << 2);

    DBG("%8d %8d %8d\n", dx, dy, dz);

    if (dx > 127)
        dx = 127;
    else if (dx < -127)
        dx = -127;

    if (dy > 127)
        dy = 127;
    else if (dy < -127)
        dy = -127;

    val = 0x08 |
        (!!(dx < 0) << 4) |
        (!!(dy < 0) << 5) |
        (buttons & 0x07);

    aux_putq(val);
    aux_putq(dx & 0xff);
    aux_putq(dy & 0xff);

    /* extra byte for IMPS/2 or IMEX */
    switch(aux_state.type) {
    default:
        break;
    case 3:
        if (dz > 127)
            dz = 127;
        else if (dz < -127)
            dz = -127;
        aux_putq(dz & 0xff);
        break;
    case 4:
        if (dz > 7)
            dz = 7;
        else if (dz < -7)
            dz = -7;
        aux_putq(dz & 0xff);
        break;
    }

    /* update deltas */
    aux_state.dx -= dx;
    aux_state.dy -= dy;
    aux_state.dz -= dz;
}

static void
aux_write(uint8_t val)
{
    switch(aux_state.cmd) {
    default:
    case -1:
        DBG("%s\n", aux_cmd(val));

        /* mouse command */
        if (aux_state.wrap) {
            if (val == AUX_RESET_WRAP) {
                aux_state.wrap = 0;
                aux_putq(AUX_ACK);
                return;
            } else if (val != AUX_RESET) {
                aux_putq(val);
                return;
            }
        }
        switch(val) {
        case AUX_SET_SCALE11:
            aux_state.status &= ~AUX_STATUS_SCALE21;
            aux_putq(AUX_ACK);
            break;
        case AUX_SET_SCALE21:
            aux_state.status |= AUX_STATUS_SCALE21;
            aux_putq(AUX_ACK);
            break;
        case AUX_SET_STREAM:
            aux_state.status &= ~AUX_STATUS_REMOTE;
            aux_putq(AUX_ACK);
            break;
        case AUX_SET_WRAP:
            aux_state.wrap = 1;
            aux_putq(AUX_ACK);
            break;
        case AUX_SET_REMOTE:
            aux_state.status |= AUX_STATUS_REMOTE;
            aux_putq(AUX_ACK);
            break;
        case AUX_GET_TYPE:
            aux_putq(AUX_ACK);
            aux_putq(aux_state.type);
            break;
        case AUX_SET_RES:
        case AUX_SET_SAMPLE:
            aux_state.cmd = val;
            aux_putq(AUX_ACK);
            break;
        case AUX_GET_SCALE:
            aux_putq(AUX_ACK);
            aux_putq(aux_state.status);
            aux_putq(aux_state.resolution);
            aux_putq(aux_state.sample_rate);
            break;
        case AUX_POLL:
            aux_putq(AUX_ACK);
            aux_send_packet();
            break;
        case AUX_ENABLE_DEV:
            aux_state.status |= AUX_STATUS_ENABLED;
            aux_putq(AUX_ACK);
            break;
        case AUX_DISABLE_DEV:
            aux_state.status &= ~AUX_STATUS_ENABLED;
            aux_putq(AUX_ACK);
            break;
        case AUX_SET_DEFAULT:
            aux_state.sample_rate = 100;
            aux_state.resolution = 2;
            aux_state.status = 0;
            aux_putq(AUX_ACK);
            break;
        case AUX_RESET:
            aux_state.sample_rate = 100;
            aux_state.resolution = 2;
            aux_state.status = 0;
            aux_state.type = 0;
            aux_putq(AUX_ACK);
            aux_putq(0xAA);
            aux_putq(aux_state.type);
            break;
        default:
            break;
        }
        break;

    case AUX_SET_SAMPLE:
        DBG("sample_rate = %d\n", val);
        aux_state.sample_rate = val;

        switch(aux_state.detect_state) {
        default:
        case 0:
            if (val == 200) {
                DBG("detect_state -> 1\n");
                aux_state.detect_state = 1;
            }
            break;
        case 1:
            if (val == 100) {
                DBG("detect_state -> 2\n");
                aux_state.detect_state = 2;
            } else if (val == 200) {
                DBG("detect_state -> 3\n");
                aux_state.detect_state = 3;
            } else {
                DBG("detect_state -> 0\n");
                aux_state.detect_state = 0;
            }
            break;
        case 2:
            if (val == 80) {
                DBG("type -> 3\n");
                aux_state.type = 3; /* IMPS/2 */
            }
            DBG("detect_state -> 0\n");
            aux_state.detect_state = 0;
            break;
        case 3:
            if (val == 80) {
                DBG("type -> 4\n");
                aux_state.type = 4; /* IMEX */
            }
            DBG("detect_state -> 0\n");
            aux_state.detect_state = 0;
            break;
        }

        aux_putq(AUX_ACK);
        aux_state.cmd = -1;
        break;

    case AUX_SET_RES:
        DBG("resolution = %02x\n", val);

        aux_state.resolution = val;
        aux_putq(AUX_ACK);
        aux_state.cmd = -1;
        break;
    }
}

static void
ps2_write_data(void *priv, uint64_t addr, uint8_t val)
{
    uint8_t cmd = ps2_state.cmd;

    ps2_state.cmd = 0;

    switch (cmd) {
    case 0:
        kbd_write(val);
        break;
    case PS2_CMD_WRITE_MODE:
        DBG("mode = %02x\n", val);
        ps2_state.mode = val;
        kbd_state.translate = !!(ps2_state.mode & PS2_MODE_KCC);
        ps2_update_irq();
        break;
    case PS2_CMD_WRITE_KBD_OBUF:
        kbd_putq(val);
        break;
    case PS2_CMD_WRITE_AUX_OBUF:
        aux_putq(val);
        break;
    case PS2_CMD_WRITE_OUTPORT:
        DBG("outport = %02x\n", val);
        ps2_state.outport = val;
        break;
    case PS2_CMD_WRITE_AUX:
        aux_write(val);
        break;
    default:
        break;
    }
}

static io_ops_t ps2_data_ops = {
    .readb = ps2_read_data,
    .writeb = ps2_write_data
};

static uint8_t
ps2_read_status(void *priv, uint64_t addr)
{
    return ps2_state.status;
}

static void
ps2_write_cmd(void *priv, uint64_t addr, uint8_t val)
{
    DBG("%s\n", ps2_cmd(val));

    if((val & PS2_CMD_PULSE_BITS_3_0) == PS2_CMD_PULSE_BITS_3_0) {
        if(!(val & 1))
            val = PS2_CMD_RESET;
        else
            val = PS2_CMD_NO_OP;
    }

    switch (val) {
    case PS2_CMD_READ_MODE:
        kbd_putq(ps2_state.mode);
        break;
    case PS2_CMD_WRITE_MODE:
    case PS2_CMD_WRITE_KBD_OBUF:
    case PS2_CMD_WRITE_AUX_OBUF:
    case PS2_CMD_WRITE_AUX:
    case PS2_CMD_WRITE_OUTPORT:
        ps2_state.cmd = val;
        break;
    case PS2_CMD_AUX_DISABLE:
        ps2_state.mode |= PS2_MODE_DISABLE_AUX;
        break;
    case PS2_CMD_AUX_ENABLE:
        ps2_state.mode &= ~PS2_MODE_DISABLE_AUX;
        break;
    case PS2_CMD_AUX_TEST:
        kbd_putq(0x00);
        break;
    case PS2_CMD_SELF_TEST:
        ps2_state.status |= PS2_STAT_SELFTEST;
        kbd_putq(0x55);
        break;
    case PS2_CMD_KBD_TEST:
        kbd_putq(0x00);
        break;
    case PS2_CMD_KBD_DISABLE:
        ps2_state.mode |= PS2_MODE_DISABLE_KBD;
        ps2_update_irq();
        break;
    case PS2_CMD_KBD_ENABLE:
        ps2_state.mode &= ~PS2_MODE_DISABLE_KBD;
        ps2_update_irq();
        break;
    case PS2_CMD_READ_INPORT:
        kbd_putq(0x00);
        break;
    case PS2_CMD_READ_OUTPORT:
        kbd_putq(ps2_state.outport);
        break;
    case PS2_CMD_ENABLE_A20:
        ps2_state.outport |= PS2_OUT_A20;
        break;
    case PS2_CMD_DISABLE_A20:
        ps2_state.outport &= ~PS2_OUT_A20;
        break;
    case PS2_CMD_RESET:
    case PS2_CMD_NO_OP:
        break;
    default:
        break;
    }
}

static io_ops_t ps2_cmd_status_ops = {
    .readb = ps2_read_status,
    .writeb = ps2_write_cmd
};

int
ps2_initialize(void)
{
    int rc;

    rc = demu_register_port_space(0x60, 1, &ps2_data_ops, NULL);
    if (rc < 0)
        goto fail1;

    rc = demu_register_port_space(0x64, 1, &ps2_cmd_status_ops, NULL);
    if (rc < 0)
        goto fail2;

    ps2_state.mode = PS2_MODE_KBD_INT | PS2_MODE_AUX_INT;
    ps2_state.status = PS2_STAT_CMD | PS2_STAT_UNLOCKED;
    ps2_state.outport = PS2_OUT_RESET | PS2_OUT_A20;

    kbd_state.cmd = -1;
    aux_state.cmd = -1;

    return 0;

fail2:
    DBG("fail2\n");

    demu_deregister_port_space(0x60);

fail1:
    DBG("fail1\n");

    return -1;

}

void
ps2_mouse_event(int dx, int dy, int dz, int lb, int mb, int rb)
{
    if (!(aux_state.status & AUX_STATUS_ENABLED))
        return;

    DBG("%8d %8d %8d (%01d%01d%01d)\n", dx, dy, dz, lb, mb, rb);

    aux_state.dx += dx;
    aux_state.dy -= dy;
    aux_state.dz += dz;
    aux_state.lb = lb;
    aux_state.mb = mb;
    aux_state.rb = rb;

    if (!(aux_state.status & AUX_STATUS_REMOTE) &&
        (aux_state.queue.count < (PS2_QUEUE_SIZE - 16))) {
        for (;;) {
            aux_send_packet();
            if (aux_state.dx == 0 &&
                aux_state.dy == 0 &&
                aux_state.dz == 0)
                break;
        }
    }
}

void
ps2_teardown(void)
{
    demu_deregister_port_space(0x64);
    demu_deregister_port_space(0x60);
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
