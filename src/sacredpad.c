// sacredpad: gamepad support for Sacred Gold (Steam build 2.0.2.28).
// Loaded by the game as ijl15.dll (the real one is renamed ijl15_real.dll; exports forwarded via ijl15.def).
// Pad input becomes a virtual mouse/keyboard inside the game process; the real cursor is never moved.
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <ddraw.h>
#include <xinput.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static FILE *logfile;
static DWORD t0;
#define LOG(...) (logfile && (fprintf(logfile, "%7.3f ", (GetTickCount() - t0) / 1000.0), fprintf(logfile, __VA_ARGS__), \
                              fputc('\n', logfile), fflush(logfile)))

// ---------------------------------------------------------------- settings (sacredpad.ini next to the game)
static struct { int deadzone, cursor_speed, target_range, hints, d2r_loot, manual_aim_ms; } cfg = {24, 12, 480, 1, 0, 2000};
static const char default_ini[] =
    "; sacredpad settings. Delete this file to get the defaults back.\n"
    "[sacredpad]\n"
    "; Stick dead zone, percent (5-50)\n"
    "deadzone=24\n"
    "; Cursor speed in menus and for manual aiming (1-30)\n"
    "cursor_speed=12\n"
    "; How far auto-targeting looks, in screen pixels (200-700)\n"
    "target_range=480\n"
    "; Button icons on the HUD, above targets and in menus (0 = off)\n"
    "hints=1\n"
    "; 0 = A attacks/talks/opens and LB+A picks up loot; 1 = D2R style, A also picks up loot\n"
    "d2r_loot=0\n"
    "; After using the right stick as a cursor, auto-targeting comes back after this many milliseconds\n"
    "manual_aim_ms=2000\n";
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void load_settings(void) {
    const char *f = ".\\sacredpad.ini";
    FILE *o;
    if (GetFileAttributesA(f) == INVALID_FILE_ATTRIBUTES && (o = fopen(f, "w"))) fputs(default_ini, o), fclose(o);
    cfg.deadzone = clampi(GetPrivateProfileIntA("sacredpad", "deadzone", cfg.deadzone, f), 5, 50);
    cfg.cursor_speed = clampi(GetPrivateProfileIntA("sacredpad", "cursor_speed", cfg.cursor_speed, f), 1, 30);
    cfg.target_range = clampi(GetPrivateProfileIntA("sacredpad", "target_range", cfg.target_range, f), 200, 700);
    cfg.hints = GetPrivateProfileIntA("sacredpad", "hints", cfg.hints, f) != 0;
    cfg.d2r_loot = GetPrivateProfileIntA("sacredpad", "d2r_loot", cfg.d2r_loot, f) != 0;
    cfg.manual_aim_ms = clampi(GetPrivateProfileIntA("sacredpad", "manual_aim_ms", cfg.manual_aim_ms, f), 0, 30000);
}

// ---------------------------------------------------------------- pad
typedef struct { float lx, ly, rx, ry, lt, rt; WORD btn; } Pad; // sticks -1..1 (y up), triggers 0..1
static Pad pad, prev;
static char ctl_path[MAX_PATH]; // test mode: pad state comes from this file instead of XInput
static DWORD (WINAPI *xget)(DWORD, XINPUT_STATE *);
static int ctl_cx = -1, ctl_cy = -1; // test mode: CX=/CY= put the cursor at an exact client position
static volatile int ctl_dump;          // test mode: DUMP logs the classified objects around the hero
static volatile int ctl_far;           // test mode: FAR lifts the target radius / on-screen limit
static volatile int ctl_key;           // test mode: KEY=<vk> taps a key once
static volatile int ctl_uidump;        // test mode: UIDUMP logs the visible UI tree
static volatile unsigned target, locked; // soft target; R3 lock
static volatile unsigned mount_ref;         // the horse the hero rides (hero +0x1ec), 0 on foot
static volatile int hero_level;             // hero +0x3fe: decides how many HUD slots are unlocked
static volatile int aiming;                 // an art button is held and the right stick places a ground marker
static volatile POINT reticle;

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
            if (!strcmp(t, "UIDUMP")) ctl_uidump = 1;
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
    static int seen = -1;
    for (DWORD i = 0; i < 4; i++) {
        if (xget(i, &s)) continue;
        if (seen != (int)i) LOG("xinput: pad %lu connected", i), seen = i;
        XINPUT_GAMEPAD *g = &s.Gamepad;
        pad = (Pad){g->sThumbLX / 32767.f, g->sThumbLY / 32767.f, g->sThumbRX / 32767.f, g->sThumbRY / 32767.f,
                    g->bLeftTrigger / 255.f, g->bRightTrigger / 255.f, g->wButtons};
        deadzone(&pad.lx, &pad.ly, cfg.deadzone / 100.f);
        deadzone(&pad.rx, &pad.ry, cfg.deadzone / 100.f);
        if (pad.lt < 0.1f) pad.lt = 0;
        if (pad.rt < 0.1f) pad.rt = 0;
        return;
    }
}

#define BTN_LT 0x0400 // spare XInput bits: triggers as buttons
#define BTN_RT 0x0800
enum { A = XINPUT_GAMEPAD_A, B = XINPUT_GAMEPAD_B, X = XINPUT_GAMEPAD_X, Y = XINPUT_GAMEPAD_Y,
       LB = XINPUT_GAMEPAD_LEFT_SHOULDER, RB = XINPUT_GAMEPAD_RIGHT_SHOULDER, L3 = XINPUT_GAMEPAD_LEFT_THUMB,
       R3 = XINPUT_GAMEPAD_RIGHT_THUMB };

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
        if (*(int *)((BYTE *)o + 0x1f0) == 4) return (unsigned)o->ref == mount_ref ? T_NONE : T_OBJECT; // horse: mount it
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

// ---------------------------------------------------------------- UI (cUI_Manager and its windows)
#define UIMGR (*(BYTE **)0x17eebbc) // +0x80..+0xd8: one window pointer per UI_WND_*
typedef struct Ctl { void **vt; int _4, _8, _c, flags, _14[4]; int x, y; short w, h; char name[32]; struct Ctl *parent;
                     BYTE _54[0x24]; struct Ctl **kids, **kids_end; } Ctl; // cUI_Control2 / cUI_Window2 prefix
// UI pointers are walked from the engine thread while the game builds and tears windows down, and +0x78 is
// only a child list on some window classes: validate everything before following it.
static int ctl_ok(Ctl *c) {
    return (DWORD)c > 0x10000 && !((DWORD)c & 3) && !IsBadReadPtr(c, sizeof *c) &&
           (DWORD)c->vt >= 0x88f000 && (DWORD)c->vt < 0x8e7000; // a vtable in the game's .rdata
}
static Ctl **kids(Ctl *c, Ctl ***end) {
    *end = c->kids_end;
    if (!c->kids || c->kids_end <= c->kids || c->kids_end - c->kids > 256 || ((DWORD)c->kids & 3) ||
        IsBadReadPtr(c->kids, (c->kids_end - c->kids) * sizeof *c->kids))
        return *end = NULL;
    return c->kids;
}
static Ctl **dialogs(BYTE *m, Ctl ***end) { // cUI_Manager's [+0x124, +0x128) list of open dialogs
    Ctl **b = *(Ctl ***)(m + 0x124);
    *end = *(Ctl ***)(m + 0x128);
    if (!b || *end <= b || *end - b > 64 || IsBadReadPtr(b, (*end - b) * sizeof *b)) return *end = NULL;
    return b;
}
static POINT ctl_pos(Ctl *c) { // absolute: x/y are relative to the parent
    POINT p = {0, 0};
    for (int i = 0; ctl_ok(c) && i < 16; c = c->parent, i++) p.x += c->x, p.y += c->y;
    return p;
}
static void ui_dump(Ctl *c, int depth) {
    if (!ctl_ok(c) || !(c->flags & 1) || depth > 6) return;
    POINT p = ctl_pos(c);
    LOG("ui %*s%.32s vt=%p at %ld,%ld %dx%d flags=%x", depth * 2, "", c->name, (void *)c->vt, p.x, p.y, c->w, c->h, c->flags);
    Ctl **end, **k = kids(c, &end);
    for (; k && k < end; k++) ui_dump(*k, depth + 1);
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
    mount_ref = *(unsigned *)((BYTE *)h + 0x1ec), hero_level = *(WORD *)((BYTE *)h + 0x3fe);
    unsigned n, best_ref = 0;
    Obj **t = objs(&n);
    float best = 1e9f;
    POINT best_pos = {0};
    for (unsigned r = 1; r < n; r++) {
        Obj *o = t[r];
        if (!o || o == h || !o->sector) continue; // sector 0: carried
        POINT s = to_screen(o->x, o->y);
        float dx = s.x - hp.x, dy = s.y - hp.y, d = hypotf(dx, dy);
        if (!ctl_far && (d > cfg.target_range || s.x < 8 || s.x > 1015 || s.y < 8 || s.y > 760)) continue; // on screen
        int k = classify(h, o);
        if (!k || (loot_mode && k != T_ITEM) || (!loot_mode && k == T_ITEM && !cfg.d2r_loot)) continue;
        float c = d > 1 ? (dx * aim_x + dy * aim_y) / d : 1;
        float score = d + 220 * (1 - c) + (k == T_ENEMY ? 0 : k == T_ITEM && !loot_mode ? 600 : 400); // cone toward the stick; enemies first
        if (r == target) score = holding ? -1 : score * 0.75f;      // sticky; fixed while acting
        if (r == locked) score = -2;
        if (score < best) best = score, best_ref = r, best_pos = s, target_kind = k;
    }
    if (locked && best_ref != locked) locked = 0; // lock lost (dead, gone, off screen)
    if (ctl_uidump && UIMGR) {
        ctl_uidump = 0;
        for (int i = 0; i < 23; i++) ui_dump(((Ctl **)(UIMGR + 0x80))[i], 0);
        for (int i = 0; i < 23; i++) {
            Ctl *w = ((Ctl **)(UIMGR + 0x80))[i];
            LOG("uiw %d %.32s vt=%p vis=%d", i, w ? w->name : "-", w ? (void *)w->vt : 0, w ? w->flags & 1 : 0);
        }
        Ctl **end, **w = dialogs(UIMGR, &end);
        for (; w && w < end; w++) ui_dump(*w, 0);
    }
    if (ctl_dump) {
        ctl_dump = 0;
        LOG("dump: target=%u kind=%d best=%f aim=%f,%f far=%d locked=%u", best_ref, target_kind, best, aim_x, aim_y, ctl_far, locked);
        POINT at = to_screen(*(int *)((BYTE *)h + 0x108), *(int *)((BYTE *)h + 0x10c));
        LOG("dump: hero action=%d target=%u at screen %ld,%ld level=%d mount=%u", *(short *)((BYTE *)h + 0xfc),
            *(unsigned *)((BYTE *)h + 0x100), at.x, at.y, hero_level, mount_ref);
        for (unsigned r = 1; r < n; r++) {
            Obj *o = t[r];
            if (!o || o == h || !o->sector) continue;
            POINT s = to_screen(o->x, o->y);
            int k = classify(h, o);
            if (family(o->type) == 2 || family(o->type) == 3 || family(o->type) == 0x20)
                if (*(int *)((BYTE *)o + 0x1f0) == 4) LOG("dump horse ref=%u screen=%ld,%ld rider=%u", r, s.x, s.y, *(unsigned *)((BYTE *)o + 0x1ec));
            if (k == T_ENEMY || (k && abs(s.x - hp.x) < 4000 && abs(s.y - hp.y) < 4000))
                LOG("dump ref=%u type=%d kind=%d screen=%ld,%ld hp=%d", r, o->type, k, s.x, s.y, k == T_ENEMY ? *(int *)((BYTE *)o + 0x4d8) : 0);
        }
    }
    target = best_ref, target_pos = best_pos;
}

// Snapshot of the open panels/dialogs, taken on the engine thread (the input thread never walks UI pointers).
static struct { volatile int open, nwin, nbtn; RECT win[24]; int grid[24]; POINT btn[64]; volatile void *dlg; POINT ok; } ui;
static void collect_buttons(Ctl *c, int depth) {
    if (!ctl_ok(c) || !(c->flags & 1) || depth > 6) return;
    if ((c->vt == (void **)0x897308 || c->vt == (void **)0x89733c) && ui.nbtn < 64) { // cUI_Button2 / RadioButton2
        POINT p = ctl_pos(c);
        ui.btn[ui.nbtn++] = (POINT){p.x + c->w / 2, p.y + c->h / 2};
    }
    Ctl **end, **k = kids(c, &end);
    for (; k && k < end; k++) collect_buttons(*k, depth + 1);
}
static void add_window(Ctl *w, int grid) {
    if (!ctl_ok(w) || !(w->flags & 1) || ui.nwin >= 24) return;
    POINT p = ctl_pos(w);
    ui.grid[ui.nwin] = grid;
    ui.win[ui.nwin++] = (RECT){p.x, p.y, p.x + w->w, p.y + w->h};
    collect_buttons(w, 0);
}
static void snapshot_ui(void) {
    BYTE *m = UIMGR;
    int nwin = 0, nbtn = 0;
    ui.nwin = ui.nbtn = 0;
    if (m) {
        Ctl **w = (Ctl **)(m + 0x80);
        for (int i = 0; i < 23; i++) // not the taskbar (0), party portraits (7) or slot 19: they are always up
            if (i != 0 && i != 7 && i != 19)
                add_window(w[i], i == 1 || i == 2 || i == 3 || i == 4 || i == 13 || i == 16 || i == 21 || i == 22);
        void *dlg = 0;
        Ctl **end, **d = dialogs(m, &end);
        for (; d && d < end; d++) {
            add_window(*d, 0);
            // NPC text (a big cUI_Tooltip; small ones are hover tooltips): its OK is drawn bottom centre
            if (ctl_ok(*d) && ((*d)->flags & 1) && (*d)->vt == (void **)0x896b3c && (*d)->w >= 300 && (*d)->h >= 100 &&
                (!dlg || (*d)->w * (*d)->h > ((Ctl *)dlg)->w * ((Ctl *)dlg)->h)) {
                POINT p = ctl_pos(*d);
                ui.ok = (POINT){p.x + (*d)->w / 2, p.y + (*d)->h - 19}, dlg = *d;
            }
        }
        ui.dlg = dlg;
        nwin = ui.nwin, nbtn = ui.nbtn;
    }
    ui.open = nwin > 0;
    (void)nbtn;
}

// The engine's per-frame cursor pass (engine thread). Targeting runs here, before the pass skips its pick
// because the cursor is over the HUD.
static int (__fastcall *cursor_pass)(void *, void *, int, int) = (void *)0x611e70;
static int __fastcall h_cursor_pass(void *engine, void *edx, int a, int b) {
    BYTE *e = engine;
    snapshot_ui();
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

// Direct3D device setup (cDxDriver7, 0x645260): under Proton 11 the chosen device record's GUID pointer (+4) can
// be stack garbage; the game then hands it to EnumZBufferFormats / CreateDevice and Proton's ddraw crashes on it.
static char (__fastcall *device_setup)(void *, void *, BYTE *, void *, void *) = (void *)0x645260;
static const GUID hal_device = {0x84e63de0, 0x46aa, 0x11cf, {0x81, 0x6f, 0x00, 0x00, 0xc0, 0x20, 0x15, 0x6e}};
static char __fastcall h_device_setup(void *drv, void *edx, BYTE *rec, void *a2, void *a3) {
    const GUID **dev = (const GUID **)(rec + 4);
    if (IsBadReadPtr(*dev, sizeof(GUID))) LOG("device guid %p is bad: using the HAL device", (void *)*dev), *dev = &hal_device;
    return device_setup(drv, edx, rec, a2, a3);
}

// Movies (intro, chapter transitions, extro) go through DirectShow's amstream, which crashes under Proton 11.
// On Wine the opener (0x6a0d80, cdecl) reports "no stream", and every caller then skips the movie.
static int __cdecl h_movie_open(void *a, void *b, void **stream) { return *stream = 0, E_FAIL; }

static void patch_jmp(DWORD at, void *fn) {
    BYTE *p = (BYTE *)at;
    DWORD old;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9, *(DWORD *)(p + 1) = (DWORD)fn - (at + 5);
    VirtualProtect(p, 5, old, &old);
}

// ---------------------------------------------------------------- overlay: button icons and the aim marker
// Glyphs are painted once with GDI into small colour-keyed DirectDraw surfaces, then blitted onto the back
// buffer right before the game flips (cDxDriver7 0x645e40: +0xb4 IDirectDraw7, +0xbc back buffer).
enum { G_A, G_B, G_X, G_Y, G_LB, G_RB, G_LT, G_RT, G_UP, G_DOWN, G_LEFT, G_RIGHT, G_RING, G_LOCK,
       G_MENU_LEGEND, G_WORLD_MENU_LEGEND, G_N };
static IDirectDrawSurface7 *glyph[G_N], *glyph_back;
static SIZE gsize[G_N];

static void paint_button(HDC dc, int x, int y, int g) { // one button icon with its top-left at x, y
    static const COLORREF face[] = {RGB(60, 176, 67), RGB(214, 56, 46), RGB(47, 120, 222), RGB(240, 192, 0)};
    static const char *label[] = {"A", "B", "X", "Y", "LB", "RB", "LT", "RT"};
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(20, 20, 20)), op = SelectObject(dc, pen);
    HFONT font = CreateFontA(g < G_LB ? -15 : -11, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, "Tahoma");
    HFONT of = SelectObject(dc, font);
    HBRUSH br = CreateSolidBrush(g < G_LB ? face[g] : RGB(70, 70, 76)), ob = SelectObject(dc, br);
    RECT r;
    if (g < G_LB) Ellipse(dc, x + 1, y + 1, x + 23, y + 23), r = (RECT){x, y + 1, x + 24, y + 24};
    else if (g < G_UP) RoundRect(dc, x + 1, y + 1, x + 29, y + 21, 8, 8), r = (RECT){x, y + 1, x + 30, y + 22};
    else {
        Ellipse(dc, x + 1, y + 1, x + 21, y + 21);
        SelectObject(dc, GetStockObject(WHITE_BRUSH));
        static const POINT tri[4][3] = {{{11, 5}, {5, 14}, {17, 14}}, {{11, 16}, {5, 7}, {17, 7}},
                                        {{5, 11}, {14, 5}, {14, 17}}, {{17, 11}, {8, 5}, {8, 17}}};
        POINT t[3];
        for (int i = 0; i < 3; i++) t[i] = (POINT){x + tri[g - G_UP][i].x - 1, y + tri[g - G_UP][i].y - 1};
        Polygon(dc, t, 3);
    }
    if (g < G_UP) {
        SetBkMode(dc, TRANSPARENT), SetTextColor(dc, g == G_Y ? RGB(30, 30, 30) : RGB(255, 255, 255));
        DrawTextA(dc, label[g], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, op), SelectObject(dc, of), SelectObject(dc, ob);
    DeleteObject(pen), DeleteObject(font), DeleteObject(br);
}

static int legend_width(HDC dc, const int *gl, const char *const *txt, int n, int paint) {
    HFONT font = CreateFontA(-13, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, "Tahoma");
    int x = 4;
    for (int i = 0; i < n; i++) {
        if (paint) paint_button(dc, x, gl[i] < G_LB ? 3 : 5, gl[i]);
        x += gl[i] < G_LB ? 26 : gl[i] < G_UP ? 32 : 24;
        HFONT of = SelectObject(dc, font);
        SIZE sz;
        GetTextExtentPoint32A(dc, txt[i], strlen(txt[i]), &sz);
        if (paint) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(10, 10, 10)), TextOutA(dc, x + 1, 7, txt[i], strlen(txt[i])); // shadow
            SetTextColor(dc, RGB(235, 225, 200)), TextOutA(dc, x, 6, txt[i], strlen(txt[i]));
        }
        SelectObject(dc, of);
        x += sz.cx + 14;
    }
    DeleteObject(font);
    return x;
}

static void paint_glyph(HDC dc, int g, int w, int h) {
    static const int menu_g[] = {G_A, G_X, G_B, G_LB};
    static const char *const menu_t[] = {"Select", "Use / equip", "Back", "/ RB  tabs"};
    static const int world_g[] = {G_A, G_X, G_B};
    static const char *const world_t[] = {"Select", "Use", "Back"};
    if (g == G_RING || g == G_LOCK) {
        COLORREF c = g == G_RING ? RGB(255, 214, 90) : RGB(230, 60, 50);
        HPEN pen = CreatePen(PS_SOLID, 3, c), op = SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, 2, 2 + h / 4, w - 2, h - 2 - h / 4); // flat ellipse: a circle on the isometric ground
        if (g == G_RING) MoveToEx(dc, w / 2 - 6, h / 2, 0), LineTo(dc, w / 2 + 7, h / 2), MoveToEx(dc, w / 2, h / 2 - 5, 0), LineTo(dc, w / 2, h / 2 + 6);
        SelectObject(dc, op), SelectObject(dc, ob), DeleteObject(pen);
    } else if (g == G_MENU_LEGEND) legend_width(dc, menu_g, menu_t, 4, 1);
    else if (g == G_WORLD_MENU_LEGEND) legend_width(dc, world_g, world_t, 3, 1);
    else paint_button(dc, 0, 0, g);
}

static void free_glyphs(void) {
    for (int i = 0; i < G_N; i++)
        if (glyph[i]) IDirectDrawSurface7_Release(glyph[i]), glyph[i] = NULL;
}

static int make_glyphs(IDirectDraw7 *dd, IDirectDrawSurface7 *back) {
    DDPIXELFORMAT pf = {sizeof pf};
    if (IDirectDrawSurface7_GetPixelFormat(back, &pf)) return 0;
    HDC sdc = GetDC(NULL);
    for (int g = 0; g < G_N; g++) {
        SIZE sz = g < G_LB ? (SIZE){24, 24} : g < G_UP ? (SIZE){30, 22} : g < G_RING ? (SIZE){22, 22}
                : g <= G_LOCK ? (SIZE){g == G_RING ? 64 : 52, g == G_RING ? 64 : 52} : (SIZE){0, 30};
        if (!sz.cx) { // legends: measure the text first
            static const int menu_g[] = {G_A, G_X, G_B, G_LB}, world_g[] = {G_A, G_X, G_B};
            static const char *const menu_t[] = {"Select", "Use / equip", "Back", "/ RB  tabs"}, *const world_t[] = {"Select", "Use", "Back"};
            sz.cx = g == G_MENU_LEGEND ? legend_width(sdc, menu_g, menu_t, 4, 0) : legend_width(sdc, world_g, world_t, 3, 0);
        }
        DDSURFACEDESC2 d = {sizeof d};
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
        d.dwWidth = sz.cx, d.dwHeight = sz.cy, d.ddpfPixelFormat = pf;
        if (IDirectDraw7_CreateSurface(dd, &d, &glyph[g], NULL)) { LOG("overlay: CreateSurface failed"); break; }
        HDC dc;
        if (!IDirectDrawSurface7_GetDC(glyph[g], &dc)) {
            RECT all = {0, 0, sz.cx, sz.cy};
            HBRUSH key = CreateSolidBrush(RGB(255, 0, 255));
            FillRect(dc, &all, key), DeleteObject(key);
            paint_glyph(dc, g, sz.cx, sz.cy);
            IDirectDrawSurface7_ReleaseDC(glyph[g], dc);
        }
        DWORD k = pf.dwRBitMask | pf.dwBBitMask; // magenta in any RGB format
        DDCOLORKEY ck = {k, k};
        IDirectDrawSurface7_SetColorKey(glyph[g], DDCKEY_SRCBLT, &ck);
        gsize[g] = sz;
    }
    ReleaseDC(NULL, sdc);
    if (!glyph[G_N - 1]) return free_glyphs(), 0;
    LOG("overlay: glyphs ready (%lu bpp)", pf.dwRGBBitCount);
    return 1;
}

static void put(IDirectDrawSurface7 *back, int g, int cx, int cy) { // centred on cx, cy, clipped to the screen
    RECT src = {0, 0, gsize[g].cx, gsize[g].cy};
    int x = cx - gsize[g].cx / 2, y = cy - gsize[g].cy / 2;
    if (x < 0) src.left -= x, x = 0;
    if (y < 0) src.top -= y, y = 0;
    if (x + src.right - src.left > 1024) src.right = src.left + 1024 - x;
    if (y + src.bottom - src.top > 768) src.bottom = src.top + 768 - y;
    if (src.right <= src.left || src.bottom <= src.top) return;
    if (IDirectDrawSurface7_BltFast(back, x, y, glyph[g], &src, DDBLTFAST_SRCCOLORKEY | DDBLTFAST_WAIT) == DDERR_SURFACELOST)
        free_glyphs(); // a mode switch: rebuild next frame
}

// HUD geometry (1024x768): art slots 6..0 grow right from x=670, weapon slots 1..n end at x=362; one slot more at
// levels 2, 8, 16 and 30. Potions sit on the compass at the top of the HUD.
static void draw_overlay(BYTE *drv) {
    static volatile LONG busy;
    IDirectDraw7 *dd = *(IDirectDraw7 **)(drv + 0xb4);
    IDirectDrawSurface7 *back = *(IDirectDrawSurface7 **)(drv + 0xbc);
    if (!dd || !back || InterlockedExchange(&busy, 1)) return;
    if (back != glyph_back) free_glyphs(), glyph_back = back;
    if (!glyph[0] && !make_glyphs(dd, back)) { busy = 0; return; }
    int lt = DOWN(BTN_LT), world = hero() && !ui.open;
    if (aiming) put(back, G_RING, reticle.x, reticle.y);
    if (cfg.hints && pad_mode) {
        if (world) {
            int lvl = hero_level, n = 1 + (lvl >= 2) + (lvl >= 8) + (lvl >= 16) + (lvl >= 30);
            static const int art_g[] = {G_X, G_Y, G_B, G_RB, G_RT};
            for (int i = 0; i < n; i++) put(back, art_g[i], lt ? 362 - (n - 1 - i) * 67 : 670 + i * 67, 690);
            put(back, G_LT, 362 - n * 67 + 12, 712);
            put(back, G_UP, lt ? 513 : 452, 652), put(back, G_RIGHT, 483, 652), put(back, G_LEFT, 545, 652), put(back, G_DOWN, 576, 652);
            if (target && !cursor_mode && !aiming) {
                int above = target_kind == T_ENEMY || target_kind == T_NPC ? 85 : 30;
                put(back, G_A, target_pos.x, target_pos.y - above);
            }
            if (locked && target == locked) put(back, G_LOCK, target_pos.x, target_pos.y);
        } else put(back, hero() ? G_MENU_LEGEND : G_WORLD_MENU_LEGEND, 12 + gsize[hero() ? G_MENU_LEGEND : G_WORLD_MENU_LEGEND].cx / 2, hero() ? 20 : 748);
    }
    busy = 0;
}

static void (__fastcall *flip)(void *) = (void *)0x645e40;
static void __fastcall h_flip(void *drv) { draw_overlay(drv), flip(drv); }

static void patch_engine(void) { // after the SteamStub has decrypted .text, i.e. once the window exists
    patch_call(0x64490d, h_device_setup);
    if (GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version")) patch_jmp(0x6a0d80, h_movie_open);
    patch_call(0x60ea8a, h_flip);        // world frame present
    patch_call(0x757250, h_flip);        // menu-thread present (main menu)
    patch_call(0x60e9c7, h_cursor_pass); // engine frame loop
    patch_call(0x60ea01, h_cursor_pass);
    patch_call(0x611f64, h_pick_hover);  // hover inside the cursor pass
    patch_call(0x6174fb, h_pick_click); // mouse-button handler
    patch_call(0x618003, h_pick_click);
}

// ---------------------------------------------------------------- per-frame logic (input thread)

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

static int walking, stand, acting;

// D-pad in menus: jump to the nearest button that way; inside item grids (or with no button that way) step one
// 32-pixel cell, snapped to the grid of the window under the cursor.
static void menu_step(int dx, int dy) {
    int gi = -1;
    for (int i = 0; i < ui.nwin; i++)
        if (ui.grid[i] && PtInRect(&ui.win[i], vcur)) gi = i;
    if (gi < 0) {
        float best = 1e9f;
        POINT to = {-1, -1};
        for (int i = 0; i < ui.nbtn; i++) {
            float bx = ui.btn[i].x - vcur.x, by = ui.btn[i].y - vcur.y, along = bx * dx + by * dy;
            float across = fabsf(bx * dy - by * dx);
            if (along < 8 || across > along * 1.5f) continue; // roughly that way
            if (along + 2 * across < best) best = along + 2 * across, to = ui.btn[i];
        }
        if (to.x >= 0) { move_cursor(to.x, to.y); return; }
    }
    POINT p = {vcur.x + dx * 32, vcur.y + dy * 32};
    for (int i = 0; i < ui.nwin; i++)
        if (ui.grid[i] && PtInRect(&ui.win[i], p)) {
            p.x = ui.win[i].left + 16 + (p.x - ui.win[i].left) / 32 * 32;
            p.y = ui.win[i].top + 16 + (p.y - ui.win[i].top) / 32 * 32;
        }
    move_cursor(p.x, p.y);
}

// Panels, dialogs and the menus: the sticks drive the cursor, the D-pad snaps, A/X click.
static void update_menu(int entered) {
    static float fx, fy;
    static int repeat, tab;
    static volatile void *last_dlg;
    if (ui.dlg != last_dlg && (last_dlg = ui.dlg)) { // a dialog appeared: on its OK
        LOG("warp dialog ok %ld,%ld", ui.ok.x, ui.ok.y);
        move_cursor(ui.ok.x, ui.ok.y);
    }
    else if (entered && hero()) { // a panel just opened: start on its first button, or its middle
        if (ui.nbtn) move_cursor(ui.btn[0].x, ui.btn[0].y);
        else if (ui.nwin) move_cursor((ui.win[0].left + ui.win[0].right) / 2, (ui.win[0].top + ui.win[0].bottom) / 2);
    }
    float lx = pad.lx + pad.rx, ly = pad.ly + pad.ry, m = fminf(hypotf(lx, ly), 1);
    if (m > 0) {
        if (!fx && !fy) fx = vcur.x, fy = vcur.y;
        float v = (1.5f + 10 * m) * cfg.cursor_speed / 12.f; // slow near the centre for precise aiming
        fx += lx * v, fy -= ly * v;
        move_cursor(lroundf(fx), lroundf(fy));
    }
    fx = vcur.x, fy = vcur.y;

    static const WORD dirs[] = {XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT};
    static const int ddx[] = {0, 0, -1, 1}, ddy[] = {-1, 1, 0, 0};
    for (int i = 0; i < 4; i++)
        if (PRESSED(dirs[i]) || (DOWN(dirs[i]) && --repeat == 0)) {
            menu_step(ddx[i], ddy[i]);
            repeat = PRESSED(dirs[i]) ? 30 : 12; // auto-repeat after 240 ms, then every 96 ms
        }

    key(VK_LBUTTON, DOWN(A));
    key(VK_RBUTTON, DOWN(X)); // equip / use
    if (PRESSED(B) || PRESSED(XINPUT_GAMEPAD_START)) tap(VK_ESCAPE);
    if (PRESSED(XINPUT_GAMEPAD_BACK)) tap('I');
    if (PRESSED(LB) || PRESSED(RB)) { // inventory tabs: backpack, combat arts, combo
        static const POINT tabs[] = {{73, 362}, {229, 362}, {391, 362}};
        tab = (tab + (PRESSED(RB) ? 1 : 2)) % 3;
        POINT back = vcur;
        move_cursor(tabs[tab].x, tabs[tab].y), key(VK_LBUTTON, 1), key(VK_LBUTTON, 0), move_cursor(back.x, back.y);
    }
}

static void update(void) {
    int lt = DOWN(BTN_LT);
    static int was_menu;
    int menu = !hero() || ui.open;
    static WORD swallow; // buttons held across a world/menu switch do nothing until released
    if (menu != was_menu) { // hand over cleanly between world and menu controls
        key(VK_LBUTTON, 0), key(VK_RBUTTON, 0), key(VK_MENU, 0), key(VK_CONTROL, 0);
        walking = acting = 0, walk_only = 0, force = -1, holding = 0;
        swallow = pad.btn & prev.btn;
    }
    swallow &= pad.btn;
    pad.btn &= ~swallow;
    if (menu) {
        cursor_mode = 1, cursor_idle = 0;
        update_menu(!was_menu), was_menu = 1;
        return;
    }
    was_menu = 0;

    float m = hypotf(pad.lx, pad.ly);
    if (m > 0.2f) aim_x = pad.lx / m, aim_y = -pad.ly / m, cursor_mode = 0;
    loot_mode = DOWN(LB);

    static const WORD slot_btn[] = {X, Y, B, RB, BTN_RT};
    int art = 0;
    for (int i = 0; i < 5; i++) {
        if (PRESSED(slot_btn[i])) tap(lt ? '1' + i : "67890"[i]); // select slot; with LT: weapon slot 1..5
        if (DOWN(slot_btn[i]) && !lt) art = 1;
    }

    // Hold-to-aim: an art button held while the right stick is pushed puts a marker on the ground instead of
    // casting; releasing the art button casts there. Without the right stick, holding keeps casting at the target.
    static int cast_pulse;
    float rm = hypotf(pad.rx, pad.ry);
    if (art && (aiming || rm > 0.35f)) {
        if (!aiming) aiming = 1, key(VK_RBUTTON, 0);
        if (rm > 0.1f) reticle = (POINT){hero_pos.x + lroundf(pad.rx * 300), hero_pos.y - lroundf(pad.ry * 225)};
        cursor_toward(reticle), force = -1; // the game's own hover shows what is under the marker
    } else if (aiming) { // released: cast at the marker, on whatever stands there
        aiming = 0;
        cursor_toward(reticle), force = -1, key(VK_RBUTTON, 1), cast_pulse = 4;
    }
    if (cast_pulse && !--cast_pulse) key(VK_RBUTTON, 0);
    int casting = art && !aiming;

    // LT + right stick up/down: zoom (the mouse wheel). Otherwise the right stick is a free cursor
    // (manual aim), back to auto-targeting a while after it rests.
    static int wheel_wait;
    if (lt && fabsf(pad.ry) > 0.5f) {
        if (--wheel_wait <= 0) {
            POINT p = vcur;
            ClientToScreen(game, &p);
            PostMessageA(game, WM_MOUSEWHEEL, MAKEWPARAM(mk(), pad.ry > 0 ? WHEEL_DELTA : -WHEEL_DELTA), MAKELPARAM(p.x, p.y));
            wheel_wait = 15;
        }
    } else if ((pad.rx || pad.ry) && !art && !aiming && !cast_pulse) {
        cursor_mode = 1, cursor_idle = 0;
        move_cursor(vcur.x + lroundf(pad.rx * (cfg.cursor_speed + 2)), vcur.y - lroundf(pad.ry * (cfg.cursor_speed + 2)));
    } else if (cursor_mode && ++cursor_idle > cfg.manual_aim_ms / 8) cursor_mode = 0;

    // LT + A on horseback: dismount (the game's Shift+click on the horse).
    static int dismount;
    if (lt && PRESSED(A) && mount_ref && !dismount) dismount = 1;
    if (dismount) {
        if (walking) key(VK_LBUTTON, 0), walking = 0;
        walk_only = 0;
        if (dismount == 1) key(VK_SHIFT, 1), force = mount_ref, move_cursor(hero_pos.x, hero_pos.y - 30), key(VK_LBUTTON, 1);
        if (dismount == 3) key(VK_LBUTTON, 0);
        if (++dismount > 12) key(VK_SHIFT, 0), force = -1, dismount = 0;
        return;
    }

    int act = DOWN(A) && !(lt && mount_ref) && !(DOWN(LB) && !target); // LB+A with no item in reach: collect all
    if (DOWN(LB) && PRESSED(A) && !target) tap('A');

    // Left stick walks (the game's click-and-hold walking toward a point ahead of the hero),
    // unless an action is held: then it only steers the aim, like D2R.
    if (m > 0.2f && !act && !art && !cast_pulse) {
        move_cursor(hero_pos.x + lroundf(aim_x * 160), hero_pos.y + lroundf(aim_y * 120));
        walk_only = 1, key(VK_LBUTTON, 1), walking = 1;
    } else if (walking) {
        key(VK_LBUTTON, 0), walking = 0, walk_only = 0;
    }

    // A: left click on the target (attack / talk / open / pick up); X Y B RB RT: right click with the selected art.
    holding = act || casting;
    if (act || casting) {
        aim_action();
        if (!acting) acting = 1; // first frame: cursor placed above, buttons below
        key(VK_LBUTTON, act);
        key(VK_RBUTTON, casting);
    } else if (acting) {
        key(VK_LBUTTON, 0);
        if (!cast_pulse) key(VK_RBUTTON, 0);
        if (++acting > 12) acting = 0, force = cast_pulse ? force : -1; // keep the forced pick until the game has seen the release
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
    static int last_open = -1;
    if (ui.open != last_open) LOG("ui open=%d windows=%d buttons=%d", ui.open, ui.nwin, ui.nbtn), last_open = ui.open;
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

// Crash reports: the first few real faults (any thread, before the game's own handlers) with the module and
// the game return addresses on the stack. Wine's own probes (IsBadReadPtr etc.) fault inside system DLLs: skipped.
static LONG CALLBACK crash_log(EXCEPTION_POINTERS *e) {
    static volatile LONG n;
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    CONTEXT *c = e->ContextRecord;
    DWORD code = r->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    HMODULE m = 0;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           r->ExceptionAddress, &m))
        GetModuleFileNameA(m, name, sizeof name);
    char *base = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
    if (m != GetModuleHandleA(NULL) && m != GetModuleHandleA("ijl15.dll") && _stricmp(base, "ddraw.dll") &&
        _stricmp(base, "amstream.dll") && _stricmp(base, "granny.dll") && _stricmp(base, "mss32.dll"))
        return EXCEPTION_CONTINUE_SEARCH; // a probe inside a system DLL
    if (InterlockedIncrement(&n) > 5) return EXCEPTION_CONTINUE_SEARCH;
    LOG("FAULT %08lx at %p (%s+%lx) addr=%08lx eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx esp=%08lx ebp=%08lx",
        code, r->ExceptionAddress, base, (DWORD)r->ExceptionAddress - (DWORD)m, r->NumberParameters > 1 ? r->ExceptionInformation[1] : 0,
        c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Esp, c->Ebp);
    DWORD *sp = (DWORD *)c->Esp;
    char calls[256] = "", *o = calls;
    for (int i = 0; i < 64 && o < calls + sizeof calls - 12 && !IsBadReadPtr(sp + i, 4); i++)
        if (sp[i] > 0x401000 && sp[i] < 0x890000) o += sprintf(o, " %08lx", sp[i]); // game return addresses (likely)
    LOG("  stack:%s  pad=%04x ui=%d hero_ok=%d", calls, pad.btn, ui.open, GetModuleHandleA(NULL) != 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res) {
    if (reason == DLL_PROCESS_DETACH) LOG("process exit%s", res ? " (terminating)" : "");
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    t0 = GetTickCount();
    load_settings();
    AddVectoredExceptionHandler(1, crash_log);
    DisableThreadLibraryCalls(inst);
    logfile = fopen("sacredpad.log", "w");
    char env[MAX_PATH];
    if (GetEnvironmentVariableA("SACREDPAD_CTL", env, sizeof env))
        snprintf(ctl_path, sizeof ctl_path, "%s%s", env[0] == '/' ? "Z:" : "", env);
    else if (GetFileAttributesA("sacredpad_ctl.txt") != INVALID_FILE_ATTRIBUTES) // test hook for Steam launches
        strcpy(ctl_path, "sacredpad_ctl.txt");
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
