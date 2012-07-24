/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// cl.input.c  -- builds an intended movement command to send to the server

#include "client.h"
#include "system/lirc.h"

static cvar_t    *cl_nodelta;
static cvar_t    *cl_maxpackets;
static cvar_t    *cl_packetdup;
static cvar_t    *cl_fuzzhack;
#ifdef _DEBUG
static cvar_t    *cl_showpackets;
#endif
static cvar_t    *cl_instantpacket;
static cvar_t    *cl_batchcmds;

static cvar_t    *m_filter;
static cvar_t    *m_accel;
static cvar_t    *m_autosens;

static cvar_t    *cl_upspeed;
static cvar_t    *cl_forwardspeed;
static cvar_t    *cl_sidespeed;
static cvar_t    *cl_yawspeed;
static cvar_t    *cl_pitchspeed;
static cvar_t    *cl_run;
static cvar_t    *cl_anglespeedkey;

static cvar_t    *freelook;
static cvar_t    *lookspring;
static cvar_t    *lookstrafe;
static cvar_t    *sensitivity;

static cvar_t    *m_pitch;
static cvar_t    *m_yaw;
static cvar_t    *m_forward;
static cvar_t    *m_side;

/*
===============================================================================

INPUT SUBSYSTEM

===============================================================================
*/

typedef enum {
    HIDE_NOT,
    HIDE_HIDE,
    HIDE_SHOW,
    HIDE_SHOW_NC
} hide_t;

typedef struct {
    qboolean    initialized;
    inputAPI_t  api;
    qboolean    modified;
    hide_t      hide_cursor;
    unsigned    last_motion;
    unsigned    hide_delay;
    int         old_dx;
    int         old_dy;
} in_state_t;

static in_state_t   input;

static cvar_t    *in_enable;
#if USE_DINPUT
static cvar_t    *in_direct;
#endif
static cvar_t    *in_smart_grab;
static cvar_t    *in_hide_delay;

static inline grab_t get_grab_mode(void)
{
    // never grab if main window doesn't have focus
    if (cls.active != ACT_ACTIVATED)
        return IN_FREE;

    // always grab in full screen
    if (r_config.flags & QVF_FULLSCREEN)
        return IN_GRAB;

    // show cursor if menu is up
    if (cls.key_dest & KEY_MENU)
        return IN_SHOW;

    // hide cursor, but don't grab if console is up
    if (cls.key_dest & KEY_CONSOLE)
        return IN_HIDE;
    if (sv_paused->integer)
        return IN_HIDE;
    if (cls.state < ca_active)
        return IN_HIDE;

    // don't grab if mouse input is not needed
    if (in_smart_grab->integer) {
        // playing a demo (and not using freelook)
        if (cls.demo.playback && !Key_IsDown(K_SHIFT))
            return IN_HIDE;
        // spectator mode
        if (cl.frame.ps.pmove.pm_type == PM_FREEZE)
            return IN_HIDE;
    }

    // regular playing mode, grab
    return IN_GRAB;
}

/*
============
IN_Activate
============
*/
void IN_Activate(void)
{
    grab_t grab;

    if (!input.initialized) {
        return;
    }

    grab = get_grab_mode();

    // set up cursor hiding policy
    if (grab == IN_HIDE && in_hide_delay->value > 0.1f) {
        input.hide_cursor = HIDE_SHOW;
        input.last_motion = com_localTime;
        input.hide_delay = in_hide_delay->value * 1000;
        grab = IN_SHOW;
    } else {
        input.hide_cursor = HIDE_NOT;
        input.last_motion = com_localTime;
        input.hide_delay = 0;
        if (grab == IN_HIDE)
            grab = IN_SHOW;
    }

    input.api.Grab(grab);
}

/*
============
IN_Restart_f
============
*/
static void IN_Restart_f(void)
{
    IN_Shutdown();
    IN_Init();
}

/*
============
IN_Frame
============
*/
void IN_Frame(void)
{
    if (input.modified) {
        IN_Restart_f();
        input.modified = qfalse;
        return;
    }

    if (input.initialized) {
        if (input.api.GetEvents) {
            input.api.GetEvents();
        }
        if (input.hide_cursor == HIDE_SHOW &&
            com_localTime - input.last_motion > input.hide_delay) {
            input.api.Grab(IN_HIDE);
            input.hide_cursor = HIDE_HIDE;
        }
    }

#if USE_LIRC
    Lirc_GetEvents();
#endif
}

/*
================
IN_MouseEvent
================
*/
void IN_MouseEvent(int x, int y)
{
    input.last_motion = com_localTime;
    if (input.hide_cursor == HIDE_HIDE) {
        input.api.Grab(IN_SHOW);
        input.hide_cursor = HIDE_SHOW;
    }

    if (x < 0 && y < 0) {
        // cursor is over non-client area
        if (input.hide_cursor == HIDE_SHOW)
            input.hide_cursor = HIDE_SHOW_NC;
        return;
    }

    // cursor is over client area
    if (input.hide_cursor == HIDE_SHOW_NC)
        input.hide_cursor = HIDE_SHOW;

#if USE_UI
    UI_MouseEvent(x, y);
#endif
}

/*
================
IN_WarpMouse
================
*/
void IN_WarpMouse(int x, int y)
{
    if (input.initialized && input.api.Warp) {
        input.api.Warp(x, y);
    }
}

/*
============
IN_Shutdown
============
*/
void IN_Shutdown(void)
{
    if (input.initialized) {
#if USE_DINPUT
        in_direct->changed = NULL;
#endif
        input.api.Shutdown();
        memset(&input, 0, sizeof(input));
    }
#if USE_LIRC
    Lirc_Shutdown();
#endif
}

static void in_changed_hard(cvar_t *self)
{
    input.modified = qtrue;
}

static void in_changed_soft(cvar_t *self)
{
    IN_Activate();
}

/*
============
IN_Init
============
*/
void IN_Init(void)
{
    qboolean ret = qfalse;

#if USE_LIRC
    Lirc_Init();
#endif

    in_enable = Cvar_Get("in_enable", "1", 0);
    in_enable->changed = in_changed_hard;
    if (!in_enable->integer) {
        Com_Printf("Mouse input disabled.\n");
        return;
    }

#if USE_DINPUT
    in_direct = Cvar_Get("in_direct", "1", 0);
    if (in_direct->integer) {
        DI_FillAPI(&input.api);
        ret = input.api.Init();
        if (!ret) {
            Cvar_Set("in_direct", "0");
        }
    }
#endif

    if (!ret) {
        VID_FillInputAPI(&input.api);
        ret = input.api.Init();
        if (!ret) {
            Cvar_Set("in_enable", "0");
            return;
        }
    }

#if USE_DINPUT
    in_direct->changed = in_changed_hard;
#endif

    in_smart_grab = Cvar_Get("in_smart_grab", "0", 0);
    in_smart_grab->changed = in_changed_soft;

    in_hide_delay = Cvar_Get("in_hide_delay", "0", 0);
    in_hide_delay->changed = in_changed_soft;

    input.initialized = qtrue;

    IN_Activate();
}


/*
===============================================================================

KEY BUTTONS

Continuous button event tracking is complicated by the fact that two different
input sources (say, mouse button 1 and the control key) can both press the
same button, but the button should only be released when both of the
pressing key have been released.

When a key event issues a button command (+forward, +attack, etc), it appends
its key number as a parameter to the command so it can be matched up with
the release.

state bit 0 is the current state of the key
state bit 1 is edge triggered on the up to down transition
state bit 2 is edge triggered on the down to up transition


Key_Event (int key, qboolean down, unsigned time);

  +mlook src time

===============================================================================
*/

typedef struct kbutton_s {
    int         down[2];        // key nums holding it down
    unsigned    downtime;        // msec timestamp
    unsigned    msec;            // msec down this frame
    int         state;
} kbutton_t;

static kbutton_t    in_klook;
static kbutton_t    in_left, in_right, in_forward, in_back;
static kbutton_t    in_lookup, in_lookdown, in_moveleft, in_moveright;
static kbutton_t    in_strafe, in_speed, in_use, in_attack;
static kbutton_t    in_up, in_down;

static int          in_impulse;
static qboolean     in_mlooking;

static void KeyDown(kbutton_t *b)
{
    int k;
    char *c;

    c = Cmd_Argv(1);
    if (c[0])
        k = atoi(c);
    else
        k = -1;        // typed manually at the console for continuous down

    if (k == b->down[0] || k == b->down[1])
        return;        // repeating key

    if (!b->down[0])
        b->down[0] = k;
    else if (!b->down[1])
        b->down[1] = k;
    else {
        Com_WPrintf("Three keys down for a button!\n");
        return;
    }

    if (b->state & 1)
        return;        // still down

    // save timestamp
    c = Cmd_Argv(2);
    b->downtime = atoi(c);
    if (!b->downtime) {
        b->downtime = com_eventTime - 100;
    }

    b->state |= 1 + 2;    // down + impulse down
}

static void KeyUp(kbutton_t *b)
{
    int k;
    char *c;
    unsigned uptime;

    c = Cmd_Argv(1);
    if (c[0])
        k = atoi(c);
    else {
        // typed manually at the console, assume for unsticking, so clear all
        b->down[0] = b->down[1] = 0;
        b->state = 0;    // impulse up
        return;
    }

    if (b->down[0] == k)
        b->down[0] = 0;
    else if (b->down[1] == k)
        b->down[1] = 0;
    else
        return;        // key up without coresponding down (menu pass through)
    if (b->down[0] || b->down[1])
        return;        // some other key is still holding it down

    if (!(b->state & 1))
        return;        // still up (this should not happen)

    // save timestamp
    c = Cmd_Argv(2);
    uptime = atoi(c);
    if (!uptime) {
        b->msec += 10;
    } else if (uptime > b->downtime) {
        b->msec += uptime - b->downtime;
    }

    b->state &= ~1;        // now up
}

static void KeyClear(kbutton_t *b)
{
    b->msec = 0;
    b->state &= ~2;        // clear impulses
    if (b->state & 1) {
        b->downtime = com_eventTime; // still down
    }
}

static void IN_KLookDown(void) { KeyDown(&in_klook); }
static void IN_KLookUp(void) { KeyUp(&in_klook); }
static void IN_UpDown(void) { KeyDown(&in_up); }
static void IN_UpUp(void) { KeyUp(&in_up); }
static void IN_DownDown(void) { KeyDown(&in_down); }
static void IN_DownUp(void) { KeyUp(&in_down); }
static void IN_LeftDown(void) { KeyDown(&in_left); }
static void IN_LeftUp(void) { KeyUp(&in_left); }
static void IN_RightDown(void) { KeyDown(&in_right); }
static void IN_RightUp(void) { KeyUp(&in_right); }
static void IN_ForwardDown(void) { KeyDown(&in_forward); }
static void IN_ForwardUp(void) { KeyUp(&in_forward); }
static void IN_BackDown(void) { KeyDown(&in_back); }
static void IN_BackUp(void) { KeyUp(&in_back); }
static void IN_LookupDown(void) { KeyDown(&in_lookup); }
static void IN_LookupUp(void) { KeyUp(&in_lookup); }
static void IN_LookdownDown(void) { KeyDown(&in_lookdown); }
static void IN_LookdownUp(void) { KeyUp(&in_lookdown); }
static void IN_MoveleftDown(void) { KeyDown(&in_moveleft); }
static void IN_MoveleftUp(void) { KeyUp(&in_moveleft); }
static void IN_MoverightDown(void) { KeyDown(&in_moveright); }
static void IN_MoverightUp(void) { KeyUp(&in_moveright); }
static void IN_SpeedDown(void) { KeyDown(&in_speed); }
static void IN_SpeedUp(void) { KeyUp(&in_speed); }
static void IN_StrafeDown(void) { KeyDown(&in_strafe); }
static void IN_StrafeUp(void) { KeyUp(&in_strafe); }

static void IN_AttackDown(void)
{
    KeyDown(&in_attack);
    if (cl_instantpacket->integer) {
        cl.sendPacketNow = qtrue;
    }
}

static void IN_AttackUp(void)
{
    KeyUp(&in_attack);
}

static void IN_UseDown(void)
{
    KeyDown(&in_use);
    if (cl_instantpacket->integer) {
        cl.sendPacketNow = qtrue;
    }
}

static void IN_UseUp(void)
{
    KeyUp(&in_use);
}

static void IN_Impulse(void)
{
    in_impulse = atoi(Cmd_Argv(1));
}

static void IN_CenterView(void)
{
    cl.viewangles[PITCH] = -SHORT2ANGLE(cl.frame.ps.pmove.delta_angles[PITCH]);
}

static void IN_MLookDown(void)
{
    in_mlooking = qtrue;
}

static void IN_MLookUp(void)
{
    in_mlooking = qfalse;

    if (!freelook->integer && lookspring->integer)
        IN_CenterView();
}

/*
===============
CL_KeyState

Returns the fraction of the frame that the key was down
===============
*/
static float CL_KeyState(kbutton_t *key)
{
    unsigned msec = key->msec;
    float val;

    if (key->state & 1) {
        // still down
        if (com_eventTime > key->downtime) {
            msec += com_eventTime - key->downtime;
        }
    }

    if (!cl.cmd.msec) {
        return 0;
    }

    val = (float)msec / cl.cmd.msec;

    return clamp(val, 0, 1);
}

//==========================================================================

/*
================
CL_MouseMove
================
*/
static void CL_MouseMove(void)
{
    int dx, dy;
    float mx, my;
    float speed;

    if (!input.initialized) {
        return;
    }
    if (cls.key_dest & (KEY_MENU | KEY_CONSOLE)) {
        return;
    }
    if (!input.api.GetMotion(&dx, &dy)) {
        return;
    }

    if (m_filter->integer) {
        mx = (dx + input.old_dx) * 0.5f;
        my = (dy + input.old_dy) * 0.5f;
    } else {
        mx = dx;
        my = dy;
    }

    input.old_dx = dx;
    input.old_dy = dy;

    if (!mx && !my) {
        return;
    }

    Cvar_ClampValue(m_accel, 0, 1);

    speed = sqrt(mx * mx + my * my);
    speed = sensitivity->value + speed * m_accel->value;

    mx *= speed;
    my *= speed;

    if (m_autosens->integer) {
        mx *= cl.fov_x / 90.0f;
        my *= V_CalcFov(cl.fov_x, 4, 3) / 73.739795291688f;
    }

// add mouse X/Y movement
    if ((in_strafe.state & 1) || (lookstrafe->integer && !in_mlooking)) {
        cl.mousemove[1] += m_side->value * mx;
    } else {
        cl.viewangles[YAW] -= m_yaw->value * mx;
    }

    if ((in_mlooking || freelook->integer) && !(in_strafe.state & 1)) {
        cl.viewangles[PITCH] += m_pitch->value * my;
    } else {
        cl.mousemove[0] -= m_forward->value * my;
    }
}


/*
================
CL_AdjustAngles

Moves the local angle positions
================
*/
static void CL_AdjustAngles(int msec)
{
    float speed;

    if (in_speed.state & 1)
        speed = msec * cl_anglespeedkey->value * 0.001f;
    else
        speed = msec * 0.001f;

    if (!(in_strafe.state & 1)) {
        cl.viewangles[YAW] -= speed * cl_yawspeed->value * CL_KeyState(&in_right);
        cl.viewangles[YAW] += speed * cl_yawspeed->value * CL_KeyState(&in_left);
    }
    if (in_klook.state & 1) {
        cl.viewangles[PITCH] -= speed * cl_pitchspeed->value * CL_KeyState(&in_forward);
        cl.viewangles[PITCH] += speed * cl_pitchspeed->value * CL_KeyState(&in_back);
    }

    cl.viewangles[PITCH] -= speed * cl_pitchspeed->value * CL_KeyState(&in_lookup);
    cl.viewangles[PITCH] += speed * cl_pitchspeed->value * CL_KeyState(&in_lookdown);
}

/*
================
CL_BaseMove

Build the intended movement vector
================
*/
static void CL_BaseMove(vec3_t move)
{
    if (in_strafe.state & 1) {
        move[1] += cl_sidespeed->value * CL_KeyState(&in_right);
        move[1] -= cl_sidespeed->value * CL_KeyState(&in_left);
    }

    move[1] += cl_sidespeed->value * CL_KeyState(&in_moveright);
    move[1] -= cl_sidespeed->value * CL_KeyState(&in_moveleft);

    move[2] += cl_upspeed->value * CL_KeyState(&in_up);
    move[2] -= cl_upspeed->value * CL_KeyState(&in_down);

    if (!(in_klook.state & 1)) {
        move[0] += cl_forwardspeed->value * CL_KeyState(&in_forward);
        move[0] -= cl_forwardspeed->value * CL_KeyState(&in_back);
    }

// adjust for speed key / running
    if ((in_speed.state & 1) ^ cl_run->integer) {
        VectorScale(move, 2, move);
    }
}

static void CL_ClampSpeed(vec3_t move)
{
    float speed = cl.pmp.maxspeed;

    clamp(move[0], -speed, speed);
    clamp(move[1], -speed, speed);
    clamp(move[2], -speed, speed);
}

static void CL_ClampPitch(void)
{
    float pitch;

    pitch = SHORT2ANGLE(cl.frame.ps.pmove.delta_angles[PITCH]);
    if (pitch > 180)
        pitch -= 360;

    if (cl.viewangles[PITCH] + pitch < -360)
        cl.viewangles[PITCH] += 360; // wrapped
    if (cl.viewangles[PITCH] + pitch > 360)
        cl.viewangles[PITCH] -= 360; // wrapped

    if (cl.viewangles[PITCH] + pitch > 89)
        cl.viewangles[PITCH] = 89 - pitch;
    if (cl.viewangles[PITCH] + pitch < -89)
        cl.viewangles[PITCH] = -89 - pitch;
}

/*
=================
CL_UpdateCmd

Updates msec, angles and builds interpolated movement vector for local prediction.
Doesn't touch command forward/side/upmove, these are filled by CL_FinalizeCmd.
=================
*/
void CL_UpdateCmd(int msec)
{
    VectorClear(cl.localmove);

    if (sv_paused->integer) {
        return;
    }

    // add to milliseconds of time to apply the move
    cl.cmd.msec += msec;

    // adjust viewangles
    CL_AdjustAngles(msec);

    // get basic movement from keyboard
    CL_BaseMove(cl.localmove);

    // allow mice to add to the move
    CL_MouseMove();

    // add accumulated mouse forward/side movement
    cl.localmove[0] += cl.mousemove[0];
    cl.localmove[1] += cl.mousemove[1];

    // clamp to server defined max speed
    CL_ClampSpeed(cl.localmove);

    CL_ClampPitch();

    cl.cmd.angles[0] = ANGLE2SHORT(cl.viewangles[0]);
    cl.cmd.angles[1] = ANGLE2SHORT(cl.viewangles[1]);
    cl.cmd.angles[2] = ANGLE2SHORT(cl.viewangles[2]);
}


/*
============
CL_RegisterInput
============
*/
void CL_RegisterInput(void)
{
    Cmd_AddCommand("centerview", IN_CenterView);

    Cmd_AddCommand("+moveup", IN_UpDown);
    Cmd_AddCommand("-moveup", IN_UpUp);
    Cmd_AddCommand("+movedown", IN_DownDown);
    Cmd_AddCommand("-movedown", IN_DownUp);
    Cmd_AddCommand("+left", IN_LeftDown);
    Cmd_AddCommand("-left", IN_LeftUp);
    Cmd_AddCommand("+right", IN_RightDown);
    Cmd_AddCommand("-right", IN_RightUp);
    Cmd_AddCommand("+forward", IN_ForwardDown);
    Cmd_AddCommand("-forward", IN_ForwardUp);
    Cmd_AddCommand("+back", IN_BackDown);
    Cmd_AddCommand("-back", IN_BackUp);
    Cmd_AddCommand("+lookup", IN_LookupDown);
    Cmd_AddCommand("-lookup", IN_LookupUp);
    Cmd_AddCommand("+lookdown", IN_LookdownDown);
    Cmd_AddCommand("-lookdown", IN_LookdownUp);
    Cmd_AddCommand("+strafe", IN_StrafeDown);
    Cmd_AddCommand("-strafe", IN_StrafeUp);
    Cmd_AddCommand("+moveleft", IN_MoveleftDown);
    Cmd_AddCommand("-moveleft", IN_MoveleftUp);
    Cmd_AddCommand("+moveright", IN_MoverightDown);
    Cmd_AddCommand("-moveright", IN_MoverightUp);
    Cmd_AddCommand("+speed", IN_SpeedDown);
    Cmd_AddCommand("-speed", IN_SpeedUp);
    Cmd_AddCommand("+attack", IN_AttackDown);
    Cmd_AddCommand("-attack", IN_AttackUp);
    Cmd_AddCommand("+use", IN_UseDown);
    Cmd_AddCommand("-use", IN_UseUp);
    Cmd_AddCommand("impulse", IN_Impulse);
    Cmd_AddCommand("+klook", IN_KLookDown);
    Cmd_AddCommand("-klook", IN_KLookUp);
    Cmd_AddCommand("+mlook", IN_MLookDown);
    Cmd_AddCommand("-mlook", IN_MLookUp);

    Cmd_AddCommand("in_restart", IN_Restart_f);

    cl_nodelta = Cvar_Get("cl_nodelta", "0", 0);
    cl_maxpackets = Cvar_Get("cl_maxpackets", "30", 0);
    cl_fuzzhack = Cvar_Get("cl_fuzzhack", "0", 0);
    cl_packetdup = Cvar_Get("cl_packetdup", "1", 0);
#ifdef _DEBUG
    cl_showpackets = Cvar_Get("cl_showpackets", "0", 0);
#endif
    cl_instantpacket = Cvar_Get("cl_instantpacket", "1", 0);
    cl_batchcmds = Cvar_Get("cl_batchcmds", "1", 0);

    cl_upspeed = Cvar_Get("cl_upspeed", "200", 0);
    cl_forwardspeed = Cvar_Get("cl_forwardspeed", "200", 0);
    cl_sidespeed = Cvar_Get("cl_sidespeed", "200", 0);
    cl_yawspeed = Cvar_Get("cl_yawspeed", "140", 0);
    cl_pitchspeed = Cvar_Get("cl_pitchspeed", "150", CVAR_CHEAT);
    cl_anglespeedkey = Cvar_Get("cl_anglespeedkey", "1.5", CVAR_CHEAT);
    cl_run = Cvar_Get("cl_run", "1", CVAR_ARCHIVE);

    freelook = Cvar_Get("freelook", "1", CVAR_ARCHIVE);
    lookspring = Cvar_Get("lookspring", "0", CVAR_ARCHIVE);
    lookstrafe = Cvar_Get("lookstrafe", "0", CVAR_ARCHIVE);
    sensitivity = Cvar_Get("sensitivity", "3", CVAR_ARCHIVE);

    m_pitch = Cvar_Get("m_pitch", "0.022", CVAR_ARCHIVE);
    m_yaw = Cvar_Get("m_yaw", "0.022", 0);
    m_forward = Cvar_Get("m_forward", "1", 0);
    m_side = Cvar_Get("m_side", "1", 0);
    m_filter = Cvar_Get("m_filter", "0", 0);
    m_accel = Cvar_Get("m_accel", "0", 0);
    m_autosens = Cvar_Get("m_autosens", "0", 0);
}

/*
=================
CL_FinalizeCmd

Builds the actual movement vector for sending to server. Assumes that msec
and angles are already set for this frame by CL_UpdateCmd.
=================
*/
void CL_FinalizeCmd(void)
{
    vec3_t move;

    // command buffer ticks in sync with cl_maxfps
    if (cmd_buffer.waitCount > 0) {
        cmd_buffer.waitCount--;
    }
    if (cl_cmdbuf.waitCount > 0) {
        cl_cmdbuf.waitCount--;
    }

    if (cls.state < ca_active) {
        return; // not talking to a server
    }

    if (sv_paused->integer) {
        return;
    }

//
// figure button bits
//
    if (in_attack.state & 3)
        cl.cmd.buttons |= BUTTON_ATTACK;
    if (in_use.state & 3)
        cl.cmd.buttons |= BUTTON_USE;

    in_attack.state &= ~2;
    in_use.state &= ~2;

    if (cls.key_dest == KEY_GAME && Key_AnyKeyDown()) {
        cl.cmd.buttons |= BUTTON_ANY;
    }

    if (cl.cmd.msec > 250) {
        cl.cmd.msec = 100;        // time was unreasonable
    }

    // rebuild the movement vector
    VectorClear(move);

    // get basic movement from keyboard
    CL_BaseMove(move);

    // add mouse forward/side movement
    move[0] += cl.mousemove[0];
    move[1] += cl.mousemove[1];

    // clamp to server defined max speed
    CL_ClampSpeed(move);

    // store the movement vector
    cl.cmd.forwardmove = move[0];
    cl.cmd.sidemove = move[1];
    cl.cmd.upmove = move[2];

    // clear all states
    cl.mousemove[0] = 0;
    cl.mousemove[1] = 0;

    KeyClear(&in_right);
    KeyClear(&in_left);

    KeyClear(&in_moveright);
    KeyClear(&in_moveleft);

    KeyClear(&in_up);
    KeyClear(&in_down);

    KeyClear(&in_forward);
    KeyClear(&in_back);

    KeyClear(&in_lookup);
    KeyClear(&in_lookdown);

    cl.cmd.impulse = in_impulse;
    in_impulse = 0;

    // save this command off for prediction
    cl.cmdNumber++;
    cl.cmds[cl.cmdNumber & CMD_MASK] = cl.cmd;

    // clear pending cmd
    memset(&cl.cmd, 0, sizeof(cl.cmd));
}

static inline qboolean ready_to_send(void)
{
    unsigned msec;

    if (cl.sendPacketNow) {
        return qtrue;
    }
    if (cls.netchan->message.cursize || cls.netchan->reliable_ack_pending) {
        return qtrue;
    }
    if (!cl_maxpackets->integer) {
        return qtrue;
    }

    if (cl_maxpackets->integer < 10) {
        Cvar_Set("cl_maxpackets", "10");
    }

    msec = 1000 / cl_maxpackets->integer;
    if (msec) {
        msec = 100 / (100 / msec);
    }
    if (cls.realtime - cl.lastTransmitTime < msec) {
        return qfalse;
    }

    return qtrue;
}

static inline qboolean ready_to_send_hacked(void)
{
    if (!cl_fuzzhack->integer) {
        return qtrue; // packet drop hack disabled
    }

    if (cl.cmdNumber - cl.lastTransmitCmdNumberReal > 2) {
        return qtrue; // can't drop more than 2 cmds
    }

    return ready_to_send();
}

/*
=================
CL_SendDefaultCmd
=================
*/
static void CL_SendDefaultCmd(void)
{
    size_t cursize q_unused, checksumIndex;
    usercmd_t *cmd, *oldcmd;
    client_history_t *history;

    // archive this packet
    history = &cl.history[cls.netchan->outgoing_sequence & CMD_MASK];
    history->cmdNumber = cl.cmdNumber;
    history->sent = cls.realtime;    // for ping calculation
    history->rcvd = 0;

    cl.lastTransmitCmdNumber = cl.cmdNumber;

    // see if we are ready to send this packet
    if (!ready_to_send_hacked()) {
        cls.netchan->outgoing_sequence++; // just drop the packet
        return;
    }

    cl.lastTransmitTime = cls.realtime;
    cl.lastTransmitCmdNumberReal = cl.cmdNumber;

    // begin a client move command
    MSG_WriteByte(clc_move);

    // save the position for a checksum byte
    checksumIndex = 0;
    if (cls.serverProtocol <= PROTOCOL_VERSION_DEFAULT) {
        checksumIndex = msg_write.cursize;
        SZ_GetSpace(&msg_write, 1);
    }

    // let the server know what the last frame we
    // got was, so the next message can be delta compressed
    if (cl_nodelta->integer || !cl.frame.valid /*|| cls.demowaiting*/) {
        MSG_WriteLong(-1);   // no compression
    } else {
        MSG_WriteLong(cl.frame.number);
    }

    // send this and the previous cmds in the message, so
    // if the last packet was dropped, it can be recovered
    cmd = &cl.cmds[(cl.cmdNumber - 2) & CMD_MASK];
    MSG_WriteDeltaUsercmd(NULL, cmd, cls.protocolVersion);
    MSG_WriteByte(cl.lightlevel);
    oldcmd = cmd;

    cmd = &cl.cmds[(cl.cmdNumber - 1) & CMD_MASK];
    MSG_WriteDeltaUsercmd(oldcmd, cmd, cls.protocolVersion);
    MSG_WriteByte(cl.lightlevel);
    oldcmd = cmd;

    cmd = &cl.cmds[cl.cmdNumber & CMD_MASK];
    MSG_WriteDeltaUsercmd(oldcmd, cmd, cls.protocolVersion);
    MSG_WriteByte(cl.lightlevel);

    if (cls.serverProtocol <= PROTOCOL_VERSION_DEFAULT) {
        // calculate a checksum over the move commands
        msg_write.data[checksumIndex] = COM_BlockSequenceCRCByte(
                                            msg_write.data + checksumIndex + 1,
                                            msg_write.cursize - checksumIndex - 1,
                                            cls.netchan->outgoing_sequence);
    }

    P_FRAMES++;

    //
    // deliver the message
    //
    cursize = cls.netchan->Transmit(cls.netchan, msg_write.cursize, msg_write.data, 1);
#ifdef _DEBUG
    if (cl_showpackets->integer) {
        Com_Printf("%"PRIz" ", cursize);
    }
#endif

    SZ_Clear(&msg_write);
}

/*
=================
CL_SendBatchedCmd
=================
*/
static void CL_SendBatchedCmd(void)
{
    int i, j, seq, bits q_unused;
    int numCmds, numDups;
    int totalCmds, totalMsec;
    size_t cursize q_unused;
    usercmd_t *cmd, *oldcmd;
    client_history_t *history, *oldest;
    byte *patch;

    // see if we are ready to send this packet
    if (!ready_to_send()) {
        return;
    }

    // archive this packet
    seq = cls.netchan->outgoing_sequence;
    history = &cl.history[seq & CMD_MASK];
    history->cmdNumber = cl.cmdNumber;
    history->sent = cls.realtime;    // for ping calculation
    history->rcvd = 0;

    cl.lastTransmitTime = cls.realtime;
    cl.lastTransmitCmdNumber = cl.cmdNumber;
    cl.lastTransmitCmdNumberReal = cl.cmdNumber;

    // begin a client move command
    patch = SZ_GetSpace(&msg_write, 1);

    // let the server know what the last frame we
    // got was, so the next message can be delta compressed
    if (cl_nodelta->integer || !cl.frame.valid /*|| cls.demowaiting*/) {
        *patch = clc_move_nodelta; // no compression
    } else {
        *patch = clc_move_batched;
        MSG_WriteLong(cl.frame.number);
    }

    Cvar_ClampInteger(cl_packetdup, 0, MAX_PACKET_FRAMES - 1);
    numDups = cl_packetdup->integer;

    *patch |= numDups << SVCMD_BITS;

    // send lightlevel
    MSG_WriteByte(cl.lightlevel);

    // send this and the previous cmds in the message, so
    // if the last packet was dropped, it can be recovered
    oldcmd = NULL;
    totalCmds = 0;
    totalMsec = 0;
    for (i = seq - numDups; i <= seq; i++) {
        oldest = &cl.history[(i - 1) & CMD_MASK];
        history = &cl.history[i & CMD_MASK];

        numCmds = history->cmdNumber - oldest->cmdNumber;
        if (numCmds >= MAX_PACKET_USERCMDS) {
            Com_WPrintf("%s: MAX_PACKET_USERCMDS exceeded\n", __func__);
            SZ_Clear(&msg_write);
            break;
        }
        totalCmds += numCmds;
        MSG_WriteBits(numCmds, 5);
        for (j = oldest->cmdNumber + 1; j <= history->cmdNumber; j++) {
            cmd = &cl.cmds[j & CMD_MASK];
            totalMsec += cmd->msec;
            bits = MSG_WriteDeltaUsercmd_Enhanced(oldcmd, cmd, cls.protocolVersion);
#ifdef _DEBUG
            if (cl_showpackets->integer == 3) {
                MSG_ShowDeltaUsercmdBits_Enhanced(bits);
            }
#endif
            oldcmd = cmd;
        }
    }

    P_FRAMES++;

    //
    // deliver the message
    //
    cursize = cls.netchan->Transmit(cls.netchan, msg_write.cursize, msg_write.data, 1);
#ifdef _DEBUG
    if (cl_showpackets->integer == 1) {
        Com_Printf("%"PRIz"(%i) ", cursize, totalCmds);
    } else if (cl_showpackets->integer == 2) {
        Com_Printf("%"PRIz"(%i) ", cursize, totalMsec);
    } else if (cl_showpackets->integer == 3) {
        Com_Printf(" | ");
    }
#endif

    SZ_Clear(&msg_write);
}

static void CL_SendKeepAlive(void)
{
    client_history_t *history;
    size_t cursize q_unused;

    // archive this packet
    history = &cl.history[cls.netchan->outgoing_sequence & CMD_MASK];
    history->cmdNumber = cl.cmdNumber;
    history->sent = cls.realtime;    // for ping calculation
    history->rcvd = 0;

    cl.lastTransmitTime = cls.realtime;
    cl.lastTransmitCmdNumber = cl.cmdNumber;
    cl.lastTransmitCmdNumberReal = cl.cmdNumber;

    cursize = cls.netchan->Transmit(cls.netchan, 0, NULL, 1);
#ifdef _DEBUG
    if (cl_showpackets->integer) {
        Com_Printf("%"PRIz" ", cursize);
    }
#endif
}

static void CL_SendUserinfo(void)
{
    char userinfo[MAX_INFO_STRING];
    cvar_t *var;
    int i;

    if (!cls.userinfo_modified) {
        return;
    }

    if (cls.userinfo_modified == MAX_PACKET_USERINFOS) {
        size_t len = Cvar_BitInfo(userinfo, CVAR_USERINFO);
        Com_DDPrintf("%s: %u: full update\n", __func__, com_framenum);
        MSG_WriteByte(clc_userinfo);
        MSG_WriteData(userinfo, len + 1);
        MSG_FlushTo(&cls.netchan->message);
    } else if (cls.serverProtocol == PROTOCOL_VERSION_Q2PRO) {
        Com_DDPrintf("%s: %u: %d updates\n", __func__, com_framenum,
                     cls.userinfo_modified);
        for (i = 0; i < cls.userinfo_modified; i++) {
            var = cls.userinfo_updates[i];
            MSG_WriteByte(clc_userinfo_delta);
            MSG_WriteString(var->name);
            if (var->flags & CVAR_USERINFO) {
                MSG_WriteString(var->string);
            } else {
                // no longer in userinfo
                MSG_WriteString(NULL);
            }
        }
        MSG_FlushTo(&cls.netchan->message);
    } else {
        Com_WPrintf("%s: update count is %d, should never happen.\n",
                    __func__, cls.userinfo_modified);
    }

    cls.userinfo_modified = 0;
}

void CL_SendCmd(void)
{
    if (cls.state < ca_connected) {
        return; // not talking to a server
    }

    // generate usercmds while playing a demo,
    // but do not send them
    if (!cls.netchan) {
        return;
    }

    if (cls.state < ca_active || sv_paused->integer) {
        // send a userinfo update if needed
        CL_SendUserinfo();

        // just keepalive or update reliable
        if (cls.netchan->ShouldUpdate(cls.netchan)) {
            CL_SendKeepAlive();
        }
        return;
    }

    // are there any new usercmds to send after all?
    if (cl.lastTransmitCmdNumber == cl.cmdNumber) {
        return; // nothing to send
    }

    // send a userinfo update if needed
    CL_SendUserinfo();

    if (cls.serverProtocol == PROTOCOL_VERSION_Q2PRO && cl_batchcmds->integer) {
        CL_SendBatchedCmd();
    } else {
        CL_SendDefaultCmd();
    }

    cl.sendPacketNow = qfalse;
}
