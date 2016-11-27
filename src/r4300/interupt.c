/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - interupt.c                                              *
 *   Mupen64Plus homepage: http://code.google.com/p/mupen64plus/           *
 *   Copyright (C) 2002 Hacktarux                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#define M64P_CORE_PROTOTYPES 1

#include "interupt.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ai/ai_controller.h"
#include "api/callbacks.h"
#include "api/m64p_types.h"
#include "cached_interp.h"
#include "exception.h"
#include "main/main.h"
#include "main/savestates.h"
#include "mi_controller.h"
#include "new_dynarec/new_dynarec.h"
#include "pi/pi_controller.h"
#include "r4300.h"
#include "r4300_core.h"
#include "rdp/rdp_core.h"
#include "recomp.h"
#include "reset.h"
#include "rsp/rsp_core.h"
#include "si/si_controller.h"
#include "vi/vi_controller.h"

int interupt_unsafe_state = 0;

struct interrupt_event
{
    int type;
    unsigned int count;
};


/***************************************************************************
 * Pool of Single Linked List Nodes
 **************************************************************************/
#define POOL_CAPACITY 16

struct node
{
    struct interrupt_event data;
    struct node *next;
};

struct pool
{
    struct node nodes[POOL_CAPACITY];
    struct node* stack[POOL_CAPACITY];
    size_t index;
};

static struct node* alloc_node(struct pool* p);
static void free_node(struct pool* p, struct node* node);
static void clear_pool(struct pool* p);


/* node allocation/deallocation on a given pool */
static struct node* alloc_node(struct pool* p)
{
    /* return NULL if pool is too small */
    if (p->index >= POOL_CAPACITY)
        return NULL;

    return p->stack[p->index++];
}

static void free_node(struct pool* p, struct node* node)
{
    if (p->index == 0 || node == NULL)
        return;

    p->stack[--p->index] = node;
}

/* release all nodes */
static void clear_pool(struct pool* p)
{
    size_t i;

    for(i = 0; i < POOL_CAPACITY; ++i)
        p->stack[i] = &p->nodes[i];

    p->index = 0;
}

/***************************************************************************
 * Interrupt Queue
 **************************************************************************/

struct interrupt_queue
{
    struct pool pool;
    struct node* first;
};

static struct interrupt_queue q;


static void clear_queue(void)
{
    q.first = NULL;
    clear_pool(&q.pool);
}


static int SPECIAL_done = 0;

static int before_event(unsigned int evt1, unsigned int evt2, int type2)
{
    if(evt1 - g_dev.r4300.cp0.regs[CP0_COUNT_REG] < UINT32_C(0x80000000))
    {
        if(evt2 - g_dev.r4300.cp0.regs[CP0_COUNT_REG] < UINT32_C(0x80000000))
        {
            if((evt1 - g_dev.r4300.cp0.regs[CP0_COUNT_REG]) < (evt2 - g_dev.r4300.cp0.regs[CP0_COUNT_REG])) return 1;
            else return 0;
        }
        else
        {
            if((g_dev.r4300.cp0.regs[CP0_COUNT_REG] - evt2) < UINT32_C(0x10000000))
            {
                switch(type2)
                {
                    case SPECIAL_INT:
                        if(SPECIAL_done) return 1;
                        else return 0;
                        break;
                    default:
                        return 0;
                }
            }
            else return 1;
        }
    }
    else return 0;
}

void add_interupt_event(int type, unsigned int delay)
{
    add_interupt_event_count(type, g_dev.r4300.cp0.regs[CP0_COUNT_REG] + delay);
}

void add_interupt_event_count(int type, unsigned int count)
{
    struct node* event;
    struct node* e;
    int special;

    special = (type == SPECIAL_INT);
   
    if(g_dev.r4300.cp0.regs[CP0_COUNT_REG] > UINT32_C(0x80000000)) SPECIAL_done = 0;
   
    if (get_event(type)) {
        DebugMessage(M64MSG_WARNING, "two events of type 0x%x in interrupt queue", type);
        /* FIXME: hack-fix for freezing in Perfect Dark
         * http://code.google.com/p/mupen64plus/issues/detail?id=553
         * https://github.com/mupen64plus-ae/mupen64plus-ae/commit/802d8f81d46705d64694d7a34010dc5f35787c7d
         */
        return;
    }

    event = alloc_node(&q.pool);
    if (event == NULL)
    {
        DebugMessage(M64MSG_ERROR, "Failed to allocate node for new interrupt event");
        return;
    }

    event->data.count = count;
    event->data.type = type;

    if (q.first == NULL)
    {
        q.first = event;
        event->next = NULL;
        g_dev.r4300.cp0.next_interrupt = q.first->data.count;
    }
    else if (before_event(count, q.first->data.count, q.first->data.type) && !special)
    {
        event->next = q.first;
        q.first = event;
        g_dev.r4300.cp0.next_interrupt = q.first->data.count;
    }
    else
    {
        for(e = q.first;
            e->next != NULL &&
            (!before_event(count, e->next->data.count, e->next->data.type) || special);
            e = e->next);

        if (e->next == NULL)
        {
            e->next = event;
            event->next = NULL;
        }
        else
        {
            if (!special)
                for(; e->next != NULL && e->next->data.count == count; e = e->next);

            event->next = e->next;
            e->next = event;
        }
    }
}

static void remove_interupt_event(void)
{
    struct node* e;

    e = q.first;
    q.first = e->next;
    free_node(&q.pool, e);

    g_dev.r4300.cp0.next_interrupt = (q.first != NULL
         && (q.first->data.count > g_dev.r4300.cp0.regs[CP0_COUNT_REG]
         || (g_dev.r4300.cp0.regs[CP0_COUNT_REG] - q.first->data.count) < UINT32_C(0x80000000)))
        ? q.first->data.count
        : 0;
}

unsigned int get_event(int type)
{
    struct node* e = q.first;

    if (e == NULL)
        return 0;

    if (e->data.type == type)
        return e->data.count;

    for(; e->next != NULL && e->next->data.type != type; e = e->next);

    return (e->next != NULL)
        ? e->next->data.count
        : 0;
}

int get_next_event_type(void)
{
    return (q.first == NULL)
        ? 0
        : q.first->data.type;
}

void remove_event(int type)
{
    struct node* to_del;
    struct node* e = q.first;

    if (e == NULL)
        return;

    if (e->data.type == type)
    {
        q.first = e->next;
        free_node(&q.pool, e);
    }
    else
    {
        for(; e->next != NULL && e->next->data.type != type; e = e->next);

        if (e->next != NULL)
        {
            to_del = e->next;
            e->next = to_del->next;
            free_node(&q.pool, to_del);
        }
    }
}

void translate_event_queue(unsigned int base)
{
    struct node* e;

    remove_event(COMPARE_INT);
    remove_event(SPECIAL_INT);

    for(e = q.first; e != NULL; e = e->next)
    {
        e->data.count = (e->data.count - g_dev.r4300.cp0.regs[CP0_COUNT_REG]) + base;
    }
    add_interupt_event_count(COMPARE_INT, g_dev.r4300.cp0.regs[CP0_COMPARE_REG]);
    add_interupt_event_count(SPECIAL_INT, 0);
}

int save_eventqueue_infos(char *buf)
{
    int len;
    struct node* e;

    len = 0;

    for(e = q.first; e != NULL; e = e->next)
    {
        memcpy(buf + len    , &e->data.type , 4);
        memcpy(buf + len + 4, &e->data.count, 4);
        len += 8;
    }

    *((unsigned int*)&buf[len]) = 0xFFFFFFFF;
    return len+4;
}

void load_eventqueue_infos(char *buf)
{
    int len = 0;
    clear_queue();
    while (*((unsigned int*)&buf[len]) != 0xFFFFFFFF)
    {
        int type = *((unsigned int*)&buf[len]);
        unsigned int count = *((unsigned int*)&buf[len+4]);
        add_interupt_event_count(type, count);
        len += 8;
    }
}

void init_interupt(void)
{
    SPECIAL_done = 1;

    g_dev.vi.delay = g_dev.vi.next_vi = 5000;

    clear_queue();
    add_interupt_event_count(VI_INT, g_dev.vi.next_vi);
    add_interupt_event_count(SPECIAL_INT, 0);
}

void check_interupt(void)
{
    struct node* event;

    if (g_dev.r4300.mi.regs[MI_INTR_REG] & g_dev.r4300.mi.regs[MI_INTR_MASK_REG])
        g_dev.r4300.cp0.regs[CP0_CAUSE_REG] = (g_dev.r4300.cp0.regs[CP0_CAUSE_REG] | CP0_CAUSE_IP2) & ~CP0_CAUSE_EXCCODE_MASK;
    else
        g_dev.r4300.cp0.regs[CP0_CAUSE_REG] &= ~CP0_CAUSE_IP2;
    if ((g_dev.r4300.cp0.regs[CP0_STATUS_REG] & (CP0_STATUS_IE | CP0_STATUS_EXL | CP0_STATUS_ERL)) != CP0_STATUS_IE) return;
    if (g_dev.r4300.cp0.regs[CP0_STATUS_REG] & g_dev.r4300.cp0.regs[CP0_CAUSE_REG] & UINT32_C(0xFF00))
    {
        event = alloc_node(&q.pool);

        if (event == NULL)
        {
            DebugMessage(M64MSG_ERROR, "Failed to allocate node for new interrupt event");
            return;
        }

        event->data.count = g_dev.r4300.cp0.next_interrupt = g_dev.r4300.cp0.regs[CP0_COUNT_REG];
        event->data.type = CHECK_INT;

        if (q.first == NULL)
        {
            q.first = event;
            event->next = NULL;
        }
        else
        {
            event->next = q.first;
            q.first = event;

        }
    }
}

static void wrapped_exception_general(void)
{
#ifdef NEW_DYNAREC
    if (r4300emu == CORE_DYNAREC) {
        g_dev.r4300.cp0.regs[CP0_EPC_REG] = (pcaddr&~3)-(pcaddr&1)*4;
        pcaddr = 0x80000180;
        g_dev.r4300.cp0.regs[CP0_STATUS_REG] |= CP0_STATUS_EXL;
        if(pcaddr&1)
          g_dev.r4300.cp0.regs[CP0_CAUSE_REG] |= CP0_CAUSE_BD;
        else
          g_dev.r4300.cp0.regs[CP0_CAUSE_REG] &= ~CP0_CAUSE_BD;
        pending_exception=1;
    } else {
        exception_general();
    }
#else
    exception_general();
#endif
}

void raise_maskable_interrupt(uint32_t cause)
{
    g_dev.r4300.cp0.regs[CP0_CAUSE_REG] = (g_dev.r4300.cp0.regs[CP0_CAUSE_REG] | cause) & ~CP0_CAUSE_EXCCODE_MASK;

    if (!(g_dev.r4300.cp0.regs[CP0_STATUS_REG] & g_dev.r4300.cp0.regs[CP0_CAUSE_REG] & UINT32_C(0xff00)))
        return;

    if ((g_dev.r4300.cp0.regs[CP0_STATUS_REG] & (CP0_STATUS_IE | CP0_STATUS_EXL | CP0_STATUS_ERL)) != CP0_STATUS_IE)
        return;

    wrapped_exception_general();
}

static void special_int_handler(void)
{
    if (g_dev.r4300.cp0.regs[CP0_COUNT_REG] > UINT32_C(0x10000000))
        return;


    SPECIAL_done = 1;
    remove_interupt_event();
    add_interupt_event_count(SPECIAL_INT, 0);
}

static void compare_int_handler(void)
{
    remove_interupt_event();
    g_dev.r4300.cp0.regs[CP0_COUNT_REG]+=g_dev.r4300.cp0.count_per_op;
    add_interupt_event_count(COMPARE_INT, g_dev.r4300.cp0.regs[CP0_COMPARE_REG]);
    g_dev.r4300.cp0.regs[CP0_COUNT_REG]-=g_dev.r4300.cp0.count_per_op;

    raise_maskable_interrupt(CP0_CAUSE_IP7);
}

static void hw2_int_handler(void)
{
    // Hardware Interrupt 2 -- remove interrupt event from queue
    remove_interupt_event();

    g_dev.r4300.cp0.regs[CP0_STATUS_REG] = (g_dev.r4300.cp0.regs[CP0_STATUS_REG] & ~(CP0_STATUS_SR | CP0_STATUS_TS | UINT32_C(0x00080000))) | CP0_STATUS_IM4;
    g_dev.r4300.cp0.regs[CP0_CAUSE_REG] = (g_dev.r4300.cp0.regs[CP0_CAUSE_REG] | CP0_CAUSE_IP4) & ~CP0_CAUSE_EXCCODE_MASK;

    wrapped_exception_general();
}

static void nmi_int_handler(void)
{
    // Non Maskable Interrupt -- remove interrupt event from queue
    remove_interupt_event();
    // setup r4300 Status flags: reset TS and SR, set BEV, ERL, and SR
    g_dev.r4300.cp0.regs[CP0_STATUS_REG] = (g_dev.r4300.cp0.regs[CP0_STATUS_REG] & ~(CP0_STATUS_SR | CP0_STATUS_TS | UINT32_C(0x00080000))) | (CP0_STATUS_ERL | CP0_STATUS_BEV | CP0_STATUS_SR);
    g_dev.r4300.cp0.regs[CP0_CAUSE_REG]  = 0x00000000;
    // simulate the soft reset code which would run from the PIF ROM
    r4300_reset_soft();
    // clear all interrupts, reset interrupt counters back to 0
    g_dev.r4300.cp0.regs[CP0_COUNT_REG] = 0;
    g_gs_vi_counter = 0;
    init_interupt();
    // clear the audio status register so that subsequent write_ai() calls will work properly
    g_dev.ai.regs[AI_STATUS_REG] = 0;
    // set ErrorEPC with the last instruction address
    g_dev.r4300.cp0.regs[CP0_ERROREPC_REG] = PC->addr;
    // reset the r4300 internal state
    if (r4300emu != CORE_PURE_INTERPRETER)
    {
        // clear all the compiled instruction blocks and re-initialize
        free_blocks();
        init_blocks();
    }
    // adjust ErrorEPC if we were in a delay slot, and clear the g_dev.r4300.delay_slot and dyna_interp flags
    if(g_dev.r4300.delay_slot==1 || g_dev.r4300.delay_slot==3)
    {
        g_dev.r4300.cp0.regs[CP0_ERROREPC_REG]-=4;
    }
    g_dev.r4300.delay_slot = 0;
    dyna_interp = 0;
    // set next instruction address to reset vector
    g_dev.r4300.cp0.last_addr = UINT32_C(0xa4000040);
    generic_jump_to(UINT32_C(0xa4000040));

#ifdef NEW_DYNAREC
    if (r4300emu == CORE_DYNAREC)
    {
        g_dev.r4300.cp0.regs[CP0_ERROREPC_REG]=(pcaddr&~3)-(pcaddr&1)*4;
        pcaddr = 0xa4000040;
        pending_exception = 1;
        invalidate_all_pages();
    }
#endif
}


void gen_interupt(void)
{
    if (stop == 1)
    {
        g_gs_vi_counter = 0; // debug
        dyna_stop();
    }

    if (!interupt_unsafe_state)
    {
        if (savestates_get_job() == savestates_job_load)
        {
            savestates_load();
            return;
        }

        if (reset_hard_job)
        {
            reset_hard();
            reset_hard_job = 0;
            return;
        }
    }
   
    if (skip_jump)
    {
        uint32_t dest = skip_jump;
        skip_jump = 0;

        g_dev.r4300.cp0.next_interrupt = (q.first->data.count > g_dev.r4300.cp0.regs[CP0_COUNT_REG]
                || (g_dev.r4300.cp0.regs[CP0_COUNT_REG] - q.first->data.count) < UINT32_C(0x80000000))
            ? q.first->data.count
            : 0;

        g_dev.r4300.cp0.last_addr = dest;
        generic_jump_to(dest);
        return;
    } 

    switch(q.first->data.type)
    {
        case SPECIAL_INT:
            special_int_handler();
            break;

        case VI_INT:
            remove_interupt_event();
            vi_vertical_interrupt_event(&g_dev.vi);
            break;
    
        case COMPARE_INT:
            compare_int_handler();
            break;
    
        case CHECK_INT:
            remove_interupt_event();
            wrapped_exception_general();
            break;
    
        case SI_INT:
            remove_interupt_event();
            si_end_of_dma_event(&g_dev.si);
            break;
    
        case PI_INT:
            remove_interupt_event();
            pi_end_of_dma_event(&g_dev.pi);
            break;
    
        case AI_INT:
            remove_interupt_event();
            ai_end_of_dma_event(&g_dev.ai);
            break;

        case SP_INT:
            remove_interupt_event();
            rsp_interrupt_event(&g_dev.sp);
            break;
    
        case DP_INT:
            remove_interupt_event();
            rdp_interrupt_event(&g_dev.dp);
            break;

        case HW2_INT:
            hw2_int_handler();
            break;

        case NMI_INT:
            nmi_int_handler();
            break;

        default:
            DebugMessage(M64MSG_ERROR, "Unknown interrupt queue event type %.8X.", q.first->data.type);
            remove_interupt_event();
            wrapped_exception_general();
            break;
    }

    if (!interupt_unsafe_state)
    {
        if (savestates_get_job() == savestates_job_save)
        {
            savestates_save();
            return;
        }
    }
}

