// sacredpad: gamepad support for Sacred Gold (Steam build 2.0.2.28).
// Loaded by the game as ijl15.dll (the real one is renamed ijl15_real.dll; exports forwarded via ijl15.def).
// Pad input becomes a virtual mouse/keyboard inside the game process; the real cursor is never moved.
#include <windows.h>
#include <windowsx.h>
#include <xinput.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static FILE *logfile;
#define LOG(...) (logfile && (fprintf(logfile, __VA_ARGS__), fputc('\n', logfile), fflush(logfile)))

// ---------------------------------------------------------------- pad
typedef struct { float lx, ly, rx, ry, lt, rt; WORD btn; } Pad; // sticks -1..1 (y up), triggers 0..1
static Pad pad, prev;
static char ctl_path[MAX_PATH]; // test mode: pad state comes from this file instead of XInput
static DWORD (WINAPI *xget)(DWORD, XINPUT_STATE *);
static int ctl_cx = -1, ctl_cy = -1; // test mode: CX=/CY= put the cursor at an exact client position
static volatile int ctl_dump;          // test mode: DUMP logs the classified objects around the hero
static volatile int ctl_far;           // test mode: FAR lifts the target radius / on-screen limit
static volatile int ctl_key;           // test mode: KEY=<vk> taps a key once
static volatile unsigned target, locked; // soft target; R3 lock

static const struct { const char *name; WORD bit; } btn_names[] = {
    {"UP", XINPUT_GAMEPAD_DPAD_UP}, {"DOWN", XINPUT_GAMEPAD_DPAD_DOWN}, {"LEFT", XINPUT_GAMEPAD_DPAD_LEFT},
    {"RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT}, {"START", XINPUT_GAMEPAD_START}, {"BACK", XINPUT_GAMEPAD_BACK},
    {"L3", XINPUT_GAMEPAD_LEFT_THUMB}, {"R3", XINPUT_GAMEPAD_RIGHT_THUMB}, {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER},
    {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER}, {"A", XINPUT_GAMEPAD_A}, {"B", XINPUT_GAMEPAD_B},
    {"X", XINPUT_GAMEPAD_X}, {"Y", XINPUT_GAMEPAD_Y},
};

static void deadzone(float *x, float *y, float dz) {
    float m = sqrtf(*x * *x + *y * *y);
    float s = m < dz ? 0 : (fminf(m, 1) - dz) / (1 - dz) / m;
    *x *= s, *y *= s;
}

static void read_pad(void) {
    memset(&pad, 0, sizeof pad);
    if (*ctl_path) { // tokens like: LX=0.7 LY=-1 RT=1 A LB
        FILE *f = fopen(ctl_path, "r");
        char t[32], k[8];
        float v;
        ctl_cx = ctl_cy = -1, ctl_far = 0;
        while (f && fscanf(f, "%31s", t) == 1) {
            if (sscanf(t, "%7[A-Z]=%f", k, &v) == 2) {
                float *ax[] = {&pad.lx, &pad.ly, &pad.rx, &pad.ry, &pad.lt, &pad.rt};
                const char *an[] = {"LX", "LY", "RX", "RY", "LT", "RT"};
                for (int i = 0; i < 6; i++) if (!strcmp(k, an[i])) *ax[i] = v;
                if (!strcmp(k, "LOCK")) locked = v; // test mode: lock a specific ref
                if (!strcmp(k, "KEY")) ctl_key = v;
                if (!strcmp(k, "CX")) ctl_cx = v;
                if (!strcmp(k, "CY")) ctl_cy = v;
            }
            for (size_t i = 0; i < sizeof btn_names / sizeof *btn_names; i++)
                if (!strcmp(t, btn_names[i].name)) pad.btn |= btn_names[i].bit;
            if (!strcmp(t, "DUMP")) ctl_dump = 1;
            if (!strcmp(t, "FAR")) ctl_far = 1;
        }
        if (f) fclose(f);
        return;
    }
    if (!xget) {
        const char *dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
        for (int i = 0; i < 3 && !xget; i++) {
            HMODULE m = LoadLibraryA(dlls[i]);
            if (m) xget = (void *)GetProcAddress(m, "XInputGetState");
        }
        if (!xget) return;
    }
    XINPUT_STATE s;
    for (DWORD i = 0; i < 4; i++) {
        if (xget(i, &s)) continue;
        XINPUT_GAMEPAD *g = &s.Gamepad;
        pad = (Pad){g->sThumbLX / 32767.f, g->sThumbLY / 32767.f, g->sThumbRX / 32767.f, g->sThumbRY / 32767.f,
                    g->bLeftTrigger / 255.f, g->bRightTrigger / 255.f, g->wButtons};
        deadzone(&pad.lx, &pad.ly, 0.24f);
        deadzone(&pad.rx, &pad.ry, 0.24f);
        return;
    }
}

#define DOWN(b) ((pad.btn & (b)) != 0)
#define PRESSED(b) (DOWN(b) && !(prev.btn & (b)))
#define RELEASED(b) (!DOWN(b) && (prev.btn & (b)))

// ---------------------------------------------------------------- virtual mouse/keyboard
static HWND game;
static WNDPROC game_proc;
static POINT vcur = {512, 384}; // client coords (game is 1024x768)
static BYTE vk[256];            // 0x80 = held by the pad
static int pad_mode;            // 1 = pad owns the cursor; real mouse movement switches back

static BOOL (WINAPI *real_GetCursorPos)(LPPOINT);
static BOOL (WINAPI *real_SetCursorPos)(int, int);
static SHORT (WINAPI *real_GetAsyncKeyState)(int);
static BOOL (WINAPI *real_GetKeyboardState)(PBYTE);
static HWND (WINAPI *real_GetForegroundWindow)(void);
static HWND (WINAPI *real_GetFocus)(void);
static HWND (WINAPI *real_CreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);

static WPARAM mk(void) {
    return (vk[VK_LBUTTON] ? MK_LBUTTON : 0) | (vk[VK_RBUTTON] ? MK_RBUTTON : 0) |
           (vk[VK_SHIFT] ? MK_SHIFT : 0) | (vk[VK_CONTROL] ? MK_CONTROL : 0);
}

static void key(int k, int down) {
    if (!vk[k] == !down) return;
    vk[k] = down ? 0x80 : 0;
    UINT m = 0;
    if (k == VK_LBUTTON) m = down ? WM_LBUTTONDOWN : WM_LBUTTONUP;
    else if (k == VK_RBUTTON) m = down ? WM_RBUTTONDOWN : WM_RBUTTONUP;
    if (m) { PostMessageA(game, m, mk(), MAKELPARAM(vcur.x, vcur.y)); return; }
    if (k == VK_MENU) return; // polled via GetAsyncKeyState; Alt messages would open the system menu
    LPARAM l = 1 | MapVirtualKeyA(k, 0) << 16 | (down ? 0 : 0xC0000000u);
    PostMessageA(game, down ? WM_KEYDOWN : WM_KEYUP, k, l);
}

static void move_cursor(int x, int y) {
    x = x < 0 ? 0 : x > 1023 ? 1023 : x;
    y = y < 0 ? 0 : y > 767 ? 767 : y;
    if (x == vcur.x && y == vcur.y) return;
    vcur = (POINT){x, y};
    PostMessageA(game, WM_MOUSEMOVE, mk(), MAKELPARAM(x, y));
}

static BOOL WINAPI h_GetCursorPos(LPPOINT p) {
    if (!pad_mode || !game) return real_GetCursorPos(p);
    *p = vcur;
    return ClientToScreen(game, p);
}

static BOOL WINAPI h_SetCursorPos(int x, int y) {
    if (!pad_mode || !game) return real_SetCursorPos(x, y);
    POINT q = {x, y};
    ScreenToClient(game, &q);
    vcur = q;
    return TRUE;
}

static SHORT WINAPI h_GetAsyncKeyState(int k) { return real_GetAsyncKeyState(k) | (vk[k & 255] ? 0x8000 : 0); }

static BOOL WINAPI h_GetKeyboardState(PBYTE ks) {
    BOOL r = real_GetKeyboardState(ks);
    for (int i = 0; i < 256; i++) ks[i] |= vk[i];
    return r;
}

static HWND WINAPI h_GetForegroundWindow(void) { return game ? game : real_GetForegroundWindow(); }
static HWND WINAPI h_GetFocus(void) { return game ? game : real_GetFocus(); }

// ---------------------------------------------------------------- world model (Steam 2.0.2.28 addresses)
typedef struct { void *vt; int _4, _8, ref, type, flags; WORD sector, _1a; int x, y; } Obj; // every world object
typedef struct { int sector, x, y, layer; } Ground;
#define OM (*(int **)0xad5c40)          // cObjectManager: [+4, +8) is the ref -> object table
#define GAMESTATE (*(BYTE **)0x182ebe8) // +0x14: the local hero's ref
#define TYPES (*(BYTE **)0xaab5e4)      // type records, 0x80 bytes: +0 flags, +0x2e family
static char (__fastcall *can_attack)(void *hero, void *edx, void *c) = (void *)0x548f60; // alive and hostile
static char (__fastcall *is_item_type)(void *types, void *edx, int type) = (void *)0x427920;
static int (__fastcall *has_dialog)(void *creature) = (void *)0x549920;
static int (__fastcall *pick)(void *, void *, int, int, unsigned, void *) = (void *)0x626c50; // object under a screen point

static Obj **objs(unsigned *n) {
    int *om = OM;
    if (!om || !om[1]) return *n = 0, NULL;
    *n = (om[2] - om[1]) / 4;
    return (Obj **)om[1];
}
static Obj *obj(unsigned ref) {
    unsigned n;
    Obj **t = objs(&n);
    return ref && ref < n ? t[ref] : NULL;
}
static Obj *hero(void) { return GAMESTATE ? obj(*(unsigned *)(GAMESTATE + 0x14)) : NULL; }
static int family(int t) { return t > 0 && t < 0x7e60 ? TYPES[t * 0x80 + 0x2e] : 0; }
static int is_usable(int t) { return t > 0 && t < 0x7e60 && (*(DWORD *)(TYPES + t * 0x80) >> 5 & 1); }

enum { T_NONE, T_ENEMY, T_NPC, T_OBJECT, T_ITEM };
static int classify(Obj *h, Obj *o) {
    int f = family(o->type);
    if (f == 2 || f == 3 || f == 0x20) {
        if (can_attack(h, 0, o)) return T_ENEMY;
        return o->ref >= 0x11 && has_dialog(o) > 0 ? T_NPC : T_NONE;
    }
    if (is_item_type(TYPES, 0, o->type)) return T_ITEM;
    return is_usable(o->type) ? T_OBJECT : T_NONE;
}

// Screen <-> ground through the current view, measured each frame with the view's own pick (vtable +0x18):
// ground = g0 + J * (screen - centre). Linear because the camera is a fixed isometric projection.
static float Ji[4];
static Ground g0;
static void view_pick(void *view, int sx, int sy, Ground *g) {
    ((void (__fastcall *)(void *, void *, int, int, Ground *))(*(void ***)view)[6])(view, 0, sx, sy, g);
}
static void measure_view(void *view) {
    Ground a, b;
    view_pick(view, 512, 384, &g0), view_pick(view, 612, 384, &a), view_pick(view, 512, 484, &b);
    float j0 = (a.x - g0.x) / 100.f, j1 = (b.x - g0.x) / 100.f, j2 = (a.y - g0.y) / 100.f, j3 = (b.y - g0.y) / 100.f;
    float d = j0 * j3 - j1 * j2;
    if (fabsf(d) > 1e-6f) Ji[0] = j3 / d, Ji[1] = -j1 / d, Ji[2] = -j2 / d, Ji[3] = j0 / d;
}
static POINT to_screen(int x, int y) {
    float dx = x - g0.x, dy = y - g0.y;
    return (POINT){512 + lroundf(Ji[0] * dx + Ji[1] * dy), 384 + lroundf(Ji[2] * dx + Ji[3] * dy)};
}

// ---------------------------------------------------------------- targeting (runs on the engine thread, per frame)
static volatile int target_kind;
static volatile POINT target_pos, hero_pos = {512, 384};
static volatile float aim_x = 1, aim_y = 0; // last stick direction, screen space (y down)
static volatile int loot_mode;              // LB held: only items are targets
static volatile int walk_only;              // left stick walking: clicks are plain ground moves
static volatile int force = -1;             // what clicks pick: -1 the game decides, 0 ground, else this ref
static volatile int cursor_mode;            // right stick in use: the game picks under the cursor as usual
static volatile int holding;                // an action button is held: keep the target while it is valid
static int cursor_idle;                     // ticks since the cursor was last placed by hand

static void update_targets(void *view) {
    static int dbg;
    int d = dbg++ < 3;
    if (d) LOG("ut: view=%p om=%p gs=%p types=%p", view, OM, GAMESTATE, TYPES);
    Obj *h = hero();
    if (d) LOG("ut: hero=%p", h);
    if (!h) { target = 0; return; }
    measure_view(view);
    if (d) LOG("ut: g0=%d,%d Ji=%f %f %f %f", g0.x, g0.y, Ji[0], Ji[1], Ji[2], Ji[3]);
    POINT hp = to_screen(h->x, h->y);
    hero_pos = hp;
    unsigned n, best_ref = 0;
    Obj **t = objs(&n);
    float best = 1e9f;
    POINT best_pos = {0};
    for (unsigned r = 1; r < n; r++) {
        Obj *o = t[r];
        if (!o || o == h || !o->sector) continue; // sector 0: carried
        POINT s = to_screen(o->x, o->y);
        float dx = s.x - hp.x, dy = s.y - hp.y, d = hypotf(dx, dy);
        if (!ctl_far && (d > 480 || s.x < 8 || s.x > 1015 || s.y < 8 || s.y > 760)) continue; // on screen
        int k = classify(h, o);
        if (!k || loot_mode != (k == T_ITEM)) continue;
        float c = d > 1 ? (dx * aim_x + dy * aim_y) / d : 1;
        float score = d + 220 * (1 - c) + (k == T_ENEMY ? 0 : 400); // cone toward the stick; enemies first
        if (r == target) score = holding ? -1 : score * 0.75f;      // sticky; fixed while acting
        if (r == locked) score = -2;
        if (score < best) best = score, best_ref = r, best_pos = s, target_kind = k;
    }
    if (locked && best_ref != locked) locked = 0; // lock lost (dead, gone, off screen)
    if (ctl_dump) {
        ctl_dump = 0;
        LOG("dump: target=%u kind=%d best=%f aim=%f,%f far=%d locked=%u", best_ref, target_kind, best, aim_x, aim_y, ctl_far, locked);
        for (unsigned r = 1; r < n; r++) {
            Obj *o = t[r];
            if (!o || o == h || !o->sector) continue;
            POINT s = to_screen(o->x, o->y);
            int k = classify(h, o);
            if (k == T_ENEMY || (k && abs(s.x - hp.x) < 4000 && abs(s.y - hp.y) < 4000))
                LOG("dump ref=%u type=%d kind=%d screen=%ld,%ld hp=%d", r, o->type, k, s.x, s.y, k == T_ENEMY ? *(int *)((BYTE *)o + 0x4d8) : 0);
        }
    }
    target = best_ref, target_pos = best_pos;
}

// The engine's per-frame cursor pass (engine thread). Targeting runs here, before the pass skips its pick
// because the cursor is over the HUD.
static int (__fastcall *cursor_pass)(void *, void *, int, int) = (void *)0x611e70;
static int __fastcall h_cursor_pass(void *engine, void *edx, int a, int b) {
    BYTE *e = engine;
    update_targets(*(void **)(e + 8 + 4 * *(WORD *)(e + 0x48))); // the current cWorldView
    return cursor_pass(engine, edx, a, b);
}

static int __fastcall h_pick_hover(void *view, void *edx, int x, int y, unsigned a, void *b) {
    if (!cursor_mode && target) return target; // the game highlights it as if hovered
    return walk_only ? 0 : pick(view, edx, x, y, a, b);
}
static int __fastcall h_pick_click(void *view, void *edx, int x, int y, unsigned a, void *b) {
    if (walk_only) return 0;
    return force >= 0 ? force : pick(view, edx, x, y, a, b);
}

static void patch_call(DWORD at, void *fn) {
    BYTE *p = (BYTE *)at;
    DWORD old;
    if (*p != 0xE8) { LOG("patch_call: no call at %08lx", at); return; }
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    *(DWORD *)(p + 1) = (DWORD)fn - (at + 5);
    VirtualProtect(p, 5, old, &old);
}

static void patch_engine(void) { // after the SteamStub has decrypted .text, i.e. once the window exists
    patch_call(0x60e9c7, h_cursor_pass); // engine frame loop
    patch_call(0x60ea01, h_cursor_pass);
    patch_call(0x611f64, h_pick_hover);  // hover inside the cursor pass
    patch_call(0x6174fb, h_pick_click); // mouse-button handler
    patch_call(0x618003, h_pick_click);
}

// ---------------------------------------------------------------- per-frame logic (input thread)
#define BTN_LT 0x0400 // spare XInput bits: triggers as buttons
#define BTN_RT 0x0800
enum { A = XINPUT_GAMEPAD_A, B = XINPUT_GAMEPAD_B, X = XINPUT_GAMEPAD_X, Y = XINPUT_GAMEPAD_Y,
       LB = XINPUT_GAMEPAD_LEFT_SHOULDER, RB = XINPUT_GAMEPAD_RIGHT_SHOULDER, L3 = XINPUT_GAMEPAD_LEFT_THUMB,
       R3 = XINPUT_GAMEPAD_RIGHT_THUMB };

static void tap(int k) { key(k, 1), key(k, 0); }

// Put the cursor on p, pulled toward the hero until it is over the world (not off screen, not on the HUD):
// the click must reach the world view, while the forced pick decides what it hits.
static void cursor_toward(POINT p) {
    float dx = p.x - hero_pos.x, dy = p.y - hero_pos.y, f = 1;
    if (p.x < 16) f = fminf(f, (16 - hero_pos.x) / dx);
    if (p.x > 1008) f = fminf(f, (1008 - hero_pos.x) / dx);
    if (p.y < 16) f = fminf(f, (16 - hero_pos.y) / dy);
    if (p.y > 650) f = fminf(f, (650 - hero_pos.y) / dy);
    move_cursor(hero_pos.x + lroundf(dx * f), hero_pos.y + lroundf(dy * f));
}

// Point the cursor where an action should land: the target, or the ground ahead of the hero.
static void aim_action(void) {
    if (cursor_mode || !hero()) { force = -1; return; } // menus / manual aim: click where the cursor is
    if (target) force = target, cursor_toward(target_pos);
    else force = 0, cursor_toward((POINT){hero_pos.x + lroundf(aim_x * 180), hero_pos.y + lroundf(aim_y * 135)});
}

static void update(void) {
    int lt = DOWN(BTN_LT);
    static int walking, stand, acting;

    float m = hypotf(pad.lx, pad.ly);
    if (m > 0.2f) aim_x = pad.lx / m, aim_y = -pad.ly / m, cursor_mode = 0;
    loot_mode = DOWN(LB);

    // LT + right stick up/down: zoom (the mouse wheel). Otherwise the right stick is a free cursor
    // (manual aim), back to auto-targeting 2 s after it rests.
    static int wheel_wait;
    if (lt && fabsf(pad.ry) > 0.5f) {
        if (--wheel_wait <= 0) {
            POINT p = vcur;
            ClientToScreen(game, &p);
            PostMessageA(game, WM_MOUSEWHEEL, MAKEWPARAM(mk(), pad.ry > 0 ? WHEEL_DELTA : -WHEEL_DELTA), MAKELPARAM(p.x, p.y));
            wheel_wait = 15;
        }
    } else if (pad.rx || pad.ry) {
        cursor_mode = 1, cursor_idle = 0;
        move_cursor(vcur.x + lroundf(pad.rx * 14), vcur.y - lroundf(pad.ry * 14));
    } else if (cursor_mode && ++cursor_idle > 250) cursor_mode = 0;

    static const WORD slot_btn[] = {X, Y, B, RB, BTN_RT};
    int art = 0;
    for (int i = 0; i < 5; i++) {
        if (PRESSED(slot_btn[i])) tap(lt ? '1' + i : "67890"[i]); // select slot; with LT: weapon slot 1..5
        if (DOWN(slot_btn[i]) && !lt) art = 1;
    }
    int act = DOWN(A) && !(DOWN(LB) && !target); // LB+A with no item in reach: collect everything
    if (DOWN(LB) && PRESSED(A) && !target) tap('A');

    // Left stick walks (the game's click-and-hold walking toward a point ahead of the hero),
    // unless an action is held: then it only steers the aim, like D2R.
    if (m > 0.2f && !act && !art) {
        move_cursor(hero_pos.x + lroundf(aim_x * 160), hero_pos.y + lroundf(aim_y * 120));
        walk_only = 1, key(VK_LBUTTON, 1), walking = 1;
    } else if (walking) {
        key(VK_LBUTTON, 0), walking = 0, walk_only = 0;
    }

    // A: left click on the target (attack / talk / open / pick up); X Y B RB RT: right click with the selected art.
    holding = act || art;
    if (act || art) {
        aim_action();
        if (!acting) acting = 1; // first frame: cursor placed above, buttons below
        key(VK_LBUTTON, act);
        key(VK_RBUTTON, art);
    } else if (acting) {
        key(VK_LBUTTON, 0), key(VK_RBUTTON, 0);
        if (++acting > 12) acting = 0, force = -1; // keep the forced pick until the game has seen the release
    }

    if (PRESSED(R3)) locked = locked ? 0 : target;

    // D-pad: potions (heal, undead death, antidote, concentration; LT+up: mentor).
    if (PRESSED(XINPUT_GAMEPAD_DPAD_UP)) tap(lt ? 'W' : VK_SPACE);
    if (PRESSED(XINPUT_GAMEPAD_DPAD_RIGHT)) tap('Q');
    if (PRESSED(XINPUT_GAMEPAD_DPAD_LEFT)) tap('E');
    if (PRESSED(XINPUT_GAMEPAD_DPAD_DOWN)) tap('R');

    if (PRESSED(XINPUT_GAMEPAD_START)) tap(lt ? 'L' : VK_ESCAPE);
    if (PRESSED(XINPUT_GAMEPAD_BACK)) tap(lt ? 'M' : 'I');
    key(VK_MENU, DOWN(LB)); // show names
    if (PRESSED(L3)) stand = !stand;
    key(VK_CONTROL, stand); // Ctrl: stop and attack instead of walking
}

static void tick(void) {
    prev = pad;
    read_pad();
    if (pad.lt > 0.5f) pad.btn |= BTN_LT;
    if (pad.rt > 0.5f) pad.btn |= BTN_RT;
    if (pad.btn || pad.lx || pad.ly || pad.rx || pad.ry || pad.lt || pad.rt) pad_mode = 1;
    if (ctl_cx >= 0) pad_mode = 1, cursor_mode = 1, cursor_idle = 0, move_cursor(ctl_cx, ctl_cy);
    if (!pad_mode) return;
    update();
    static int last_key;
    if (ctl_key && ctl_key != last_key) tap(ctl_key);
    last_key = ctl_key;
    if (pad.btn != prev.btn)
        LOG("pad btn=%04x cur=%ld,%ld hero=%ld,%ld target=%u kind=%d at %ld,%ld", pad.btn, vcur.x, vcur.y,
            hero_pos.x, hero_pos.y, target, target_kind, target_pos.x, target_pos.y);
}

static DWORD WINAPI tick_thread(LPVOID unused) {
    for (;;) Sleep(8), tick(); // own thread: the game's queue is too busy for WM_TIMER to be delivered in-world
}

static LRESULT CALLBACK game_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // Keep running and accepting input while unfocused (alt-tab, or a hidden workspace while testing).
    if ((m == WM_ACTIVATEAPP && !w) || (m == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE) || m == WM_KILLFOCUS) return 0;
    if (m == WM_MOUSEMOVE && pad_mode && !*ctl_path && (GET_X_LPARAM(l) != vcur.x || GET_Y_LPARAM(l) != vcur.y))
        pad_mode = 0; // real mouse moved: hand the cursor back
    return CallWindowProcA(game_proc, h, m, w, l);
}

static HWND WINAPI h_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h,
                                     HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    HWND r = real_CreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    LOG("CreateWindowExA cls=%s name=%s style=%08lx -> %p", HIWORD(cls) ? cls : "#", name ? name : "", style, r);
    if (r && !parent && !game) {
        game = r;
        patch_engine();
        game_proc = (WNDPROC)SetWindowLongA(r, GWL_WNDPROC, (LONG)game_wndproc);
        CreateThread(NULL, 0, tick_thread, NULL, 0, NULL);
    }
    return r;
}

// ---------------------------------------------------------------- IAT hooks
static void *hook(const char *dll, const char *name, void *fn) {
    void *real = GetProcAddress(GetModuleHandleA(dll), name);
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (void *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
    int n = 0;
    for (void **p = (void **)(base + d->VirtualAddress); p < (void **)(base + d->VirtualAddress + d->Size); p++) {
        if (*p != real) continue;
        DWORD old;
        VirtualProtect(p, sizeof *p, PAGE_READWRITE, &old);
        *p = fn;
        VirtualProtect(p, sizeof *p, old, &old);
        n++;
    }
    if (!n) LOG("hook: %s!%s not found in IAT", dll, name);
    return real;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    logfile = fopen("sacredpad.log", "w");
    char env[MAX_PATH];
    if (GetEnvironmentVariableA("SACREDPAD_CTL", env, sizeof env))
        snprintf(ctl_path, sizeof ctl_path, "%s%s", env[0] == '/' ? "Z:" : "", env);
    real_GetCursorPos = hook("user32.dll", "GetCursorPos", h_GetCursorPos);
    real_SetCursorPos = hook("user32.dll", "SetCursorPos", h_SetCursorPos);
    real_GetAsyncKeyState = hook("user32.dll", "GetAsyncKeyState", h_GetAsyncKeyState);
    real_GetKeyboardState = hook("user32.dll", "GetKeyboardState", h_GetKeyboardState);
    real_GetForegroundWindow = hook("user32.dll", "GetForegroundWindow", h_GetForegroundWindow);
    real_GetFocus = hook("user32.dll", "GetFocus", h_GetFocus);
    real_CreateWindowExA = hook("user32.dll", "CreateWindowExA", h_CreateWindowExA);
    LOG("sacredpad loaded, ctl=%s", ctl_path);
    return TRUE;
}
