// sacredpad: gamepad support for Sacred Gold (Steam build 2.0.2.28).
// Loaded by the game as ijl15.dll (the real one is renamed ijl15_real.dll; exports forwarded via ijl15.def).
// Pad input becomes a virtual mouse/keyboard inside the game process; the real cursor is never moved.
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <ddraw.h>
#include <xinput.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static FILE *logfile;
static DWORD t0;
#define LOG(...) (logfile && (fprintf(logfile, "%7.3f ", (GetTickCount() - t0) / 1000.0), fprintf(logfile, __VA_ARGS__), \
                              fputc('\n', logfile), fflush(logfile)))

// ---------------------------------------------------------------- code patching
static void *patch_ptr(void *at, void *fn) { // a function pointer in a vtable, the IAT or a delay-load slot
    DWORD old;
    void *was = *(void **)at;
    VirtualProtect(at, 4, PAGE_READWRITE, &old), *(void **)at = fn, VirtualProtect(at, 4, old, &old);
    return was;
}
static void patch_rel(DWORD at, void *fn, BYTE op) { // a call (E8) or jmp (E9) at `at`
    BYTE *p = (BYTE *)at;
    DWORD old;
    if (op == 0xE8 && *p != 0xE8) { LOG("patch: no call at %08lx", at); return; }
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    p[0] = op, *(DWORD *)(p + 1) = (DWORD)fn - (at + 5);
    VirtualProtect(p, 5, old, &old);
}
#define patch_call(at, fn) patch_rel(at, fn, 0xE8)

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
static volatile WORD pad_shown; // the buttons as of the last complete tick, for the render thread
static char ctl_path[MAX_PATH]; // test mode: pad state comes from this file instead of XInput
static DWORD (WINAPI *xget)(DWORD, XINPUT_STATE *);
static int ctl_cx = -1, ctl_cy = -1; // test mode: CX=/CY= put the cursor at an exact client position
static volatile int ctl_dump;          // test mode: DUMP logs the classified objects around the hero
static volatile int ctl_far;           // test mode: FAR lifts the target radius / on-screen limit
static volatile int ctl_uiscan;        // test mode: UISCAN logs controls found by scanning window memory
static volatile int ctl_focus;         // test mode: FOCUS logs the focus list
static volatile unsigned target, locked; // soft target; R3 lock
static volatile unsigned mount_ref;         // the horse the hero rides (hero +0x1ec), 0 on foot
static volatile int hero_level;             // hero +0x3fe: decides how many HUD slots are unlocked
static volatile int aiming;                 // an art button is held and the right stick places a ground marker
static volatile POINT reticle;
// Hotbar art slots (1024x768 HUD): slot i (keys 6..0, pad X Y B RB RT) is centred at (670 + 67 i, 716);
// one slot at level 1, another at levels 2, 8, 16 and 30.
static POINT art_slot(int i) { return (POINT){670 + 67 * i, 716}; }
static int art_slots(void) { int l = hero_level; return 1 + (l >= 2) + (l >= 8) + (l >= 16) + (l >= 30); }
// Weapon slots (keys 1..5, pad LT + X Y B RB RT) mirror them: they end at x = 362.
static POINT weapon_slot(int i) { return (POINT){362 - 67 * (art_slots() - 1 - i), 716}; }
static volatile int assigning;   // X on an art / Y on an item: the next X/Y/B/RB/RT puts it on that slot, A cancels
static volatile int assign_kind; // 1: art -> art slot, 2: item -> weapon slot
static volatile int focus_kind = -1, focus_tag; // of the focused entry (for the legend)

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
            if (sscanf(t, "%7[A-Z]=%f", k, &v) == 2 && fabsf(v) < 1e9f) { // (in range for the int conversions below)
                float *ax[] = {&pad.lx, &pad.ly, &pad.rx, &pad.ry, &pad.lt, &pad.rt};
                const char *an[] = {"LX", "LY", "RX", "RY", "LT", "RT"};
                for (int i = 0; i < 6; i++) if (!strcmp(k, an[i])) *ax[i] = v;
                if (!strcmp(k, "LOCK")) locked = v > 0 ? v : 0; // test mode: lock a specific ref
                if (!strcmp(k, "CX")) ctl_cx = v;
                if (!strcmp(k, "CY")) ctl_cy = v;
            }
            for (size_t i = 0; i < sizeof btn_names / sizeof *btn_names; i++)
                if (!strcmp(t, btn_names[i].name)) pad.btn |= btn_names[i].bit;
            if (!strcmp(t, "DUMP")) ctl_dump = 1;
            if (!strcmp(t, "FAR")) ctl_far = 1;
            if (!strcmp(t, "UISCAN")) ctl_uiscan = 1;
            if (!strcmp(t, "FOCUS")) ctl_focus = 1;
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
    static int slot = -1; // the connected pad; empty slots are slow to query, so they are scanned once a second
    static DWORD rescan;
    if (slot < 0 && GetTickCount() - rescan < 1000) return;
    for (int i = slot < 0 ? 0 : slot; i < 4; i++) {
        if (xget(i, &s)) continue;
        if (slot != i) LOG("xinput: pad %d connected", i), slot = i;
        XINPUT_GAMEPAD *g = &s.Gamepad;
        pad = (Pad){g->sThumbLX / 32767.f, g->sThumbLY / 32767.f, g->sThumbRX / 32767.f, g->sThumbRY / 32767.f,
                    g->bLeftTrigger / 255.f, g->bRightTrigger / 255.f, g->wButtons};
        deadzone(&pad.lx, &pad.ly, cfg.deadzone / 100.f);
        deadzone(&pad.rx, &pad.ry, cfg.deadzone / 100.f);
        if (pad.lt < 0.1f) pad.lt = 0;
        if (pad.rt < 0.1f) pad.rt = 0;
        return;
    }
    slot = -1, rescan = GetTickCount();
}

#define BTN_LT 0x0400 // spare XInput bits: triggers as buttons
#define BTN_RT 0x0800
enum { A = XINPUT_GAMEPAD_A, B = XINPUT_GAMEPAD_B, X = XINPUT_GAMEPAD_X, Y = XINPUT_GAMEPAD_Y,
       LB = XINPUT_GAMEPAD_LEFT_SHOULDER, RB = XINPUT_GAMEPAD_RIGHT_SHOULDER, L3 = XINPUT_GAMEPAD_LEFT_THUMB,
       R3 = XINPUT_GAMEPAD_RIGHT_THUMB };

#define DOWN(b) ((pad.btn & (b)) != 0)
#define PRESSED(b) (DOWN(b) && !(prev.btn & (b)))

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

#define MK_OURS 0x8000 // tags the mouse messages the pad posts; stripped before the game sees them
static WPARAM mk(void) {
    return (vk[VK_LBUTTON] ? MK_LBUTTON : 0) | (vk[VK_RBUTTON] ? MK_RBUTTON : 0) |
           (vk[VK_SHIFT] ? MK_SHIFT : 0) | (vk[VK_CONTROL] ? MK_CONTROL : 0) | MK_OURS;
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
    BYTE *mouse = *(BYTE **)0xcddb5c; // the game's mouse: +4/+8 is where its cursor sprite is drawn
    if (mouse) *(int *)(mouse + 4) = x, *(int *)(mouse + 8) = y; // the fullscreen menus never refresh it themselves
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

static SHORT WINAPI h_GetAsyncKeyState(int k) { return real_GetAsyncKeyState(k) | (vk[k & 255] ? SHRT_MIN : 0); }

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
        if (can_attack(h, NULL, o)) return T_ENEMY;
        if (*(int *)((BYTE *)o + 0x1f0) == 4) return (unsigned)o->ref == mount_ref ? T_NONE : T_OBJECT; // horse: mount it
        // Talkable: a dialog is available, or a trader (creature flags 2: 0x1000 blacksmith, 0x2000 merchant,
        // 0x4000 combat master, 0x4000000 horse trader), the same tests the game's hover cursor uses.
        if (o->ref >= 0x11 && (*(DWORD *)((BYTE *)o + 0x200) & 0x4007000)) return T_NPC;
        return o->ref >= 0x11 && has_dialog(o) > 0 ? T_NPC : T_NONE;
    }
    if (is_item_type(TYPES, NULL, o->type)) return T_ITEM;
    return is_usable(o->type) ? T_OBJECT : T_NONE;
}

// Screen <-> ground through the current view, measured each frame with the view's own pick (vtable +0x18):
// ground = g0 + J * (screen - centre). Linear because the camera is a fixed isometric projection.
static float Ji[4];
static Ground g0;
static void view_pick(void *view, int sx, int sy, Ground *g) {
    ((void (__fastcall *)(void *, void *, int, int, Ground *))(*(void ***)view)[6])(view, NULL, sx, sy, g);
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
                   } Ctl; // cUI_Control2 prefix
// UI pointers are read on the render threads while the game builds and tears windows down: validate before use.
static int ctl_ok(Ctl *c) {
    return (DWORD)c > 0x10000 && !((DWORD)c & 3) && !IsBadReadPtr(c, sizeof *c) &&
           (DWORD)c->vt >= 0x88f000 && (DWORD)c->vt < 0x8e7000; // a vtable in the game's .rdata
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
// Discovery: controls owned by w, found by scanning w's memory for pointers / std::vector ranges of them.
static int owned_by(Ctl *c, Ctl *w) {
    for (int i = 0; ctl_ok(c) && i < 8; i++, c = c->parent) if (c->parent == w) return 1;
    return 0;
}
static void ui_scan(Ctl *w, int depth) {
    if (!ctl_ok(w) || depth > 3) return;
    POINT wp = ctl_pos(w);
    LOG("scan %*s%.32s vt=%p at %ld,%ld %dx%d flags=%x", depth * 2, "", w->name, (void *)w->vt, wp.x, wp.y, w->w, w->h, w->flags);
    for (int off = 0x54; off < 0x600; off += 4) {
        DWORD *f = (DWORD *)((BYTE *)w + off);
        if (IsBadReadPtr(f, 8)) break;
        Ctl *c = (Ctl *)f[0];
        if (ctl_ok(c) && c != w && owned_by(c, w)) {
            POINT p = ctl_pos(c);
            LOG("scan %*s +%03x ptr %.32s vt=%p at %ld,%ld %dx%d flags=%x", depth * 2, "", off, c->name, (void *)c->vt, p.x, p.y, c->w, c->h, c->flags);
            if (depth < 3 && c->vt != w->vt) ui_scan(c, depth + 1);
            continue;
        }
        Ctl **b = (Ctl **)f[0], **e = (Ctl **)f[1];
        if ((DWORD)b > 0x10000 && e > b && e - b <= 512 && !IsBadReadPtr(b, (e - b) * 4) && ctl_ok(*b) && owned_by(*b, w)) {
            LOG("scan %*s +%03x vec[%d]", depth * 2, "", off, (int)(e - b));
            for (Ctl **k = b; k < e; k++) {
                if (!ctl_ok(*k)) continue;
                POINT p = ctl_pos(*k);
                LOG("scan %*s    %.32s vt=%p at %ld,%ld %dx%d flags=%x", depth * 2, "", (*k)->name, (void *)(*k)->vt, p.x, p.y, (*k)->w, (*k)->h, (*k)->flags);
            }
            off += 4;
        }
    }
}
static void ui_scan_all(void) {
    BYTE *m = UIMGR;
    if (!m) return;
    for (int off = 0; off < 0x2f4; off += 4) { // every visible window the manager points to
        Ctl *w = *(Ctl **)(m + off);
        if (ctl_ok(w) && (w->flags & 1) && !w->parent && w->vt != (void **)0x896ac0 && w->vt != (void **)0x8968cc)
            LOG("scan manager +%03x", off), ui_scan(w, 0);
    }
    Ctl **end, **d = dialogs(m, &end);
    for (; d && d < end; d++) if (ctl_ok(*d) && ((*d)->flags & 1)) LOG("scan dialog"), ui_scan(*d, 0);
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
static volatile int steering;               // the left stick is pushed
static volatile int stand;                  // L3: stand still (attacks don't walk up to the target)
static int cursor_idle;                     // ticks since the cursor was last placed by hand

static void update_targets(void *view) {
    Obj *h = hero();
    if (!h) { target = 0; return; }
    measure_view(view);
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
        float c = d > 1 ? (dx * aim_x + dy * aim_y) / d : 1;
        float score = d + 400 * (1 - c); // cone toward the stick
        if (score >= best && r != target && r != locked) continue; // can't win: skip the game's (costly) checks
        int k = classify(h, o);
        if (!k || (loot_mode && k != T_ITEM) || (!loot_mode && k == T_ITEM && !cfg.d2r_loot)) continue;
        static const float penalty[] = {0, 0, 250, 400, 600}; // enemies, then people, then objects, loot last
        score += k == T_ITEM && loot_mode ? 0 : penalty[k];
        if (r == target) score = holding ? -1 : score * (steering ? 0.9f : 0.75f); // sticky; fixed while acting
        if (r == locked) score = -2;
        if (score < best) best = score, best_ref = r, best_pos = s, target_kind = k;
    }
    if (locked && best_ref != locked) locked = 0; // lock lost (dead, gone, off screen)
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
            if (k == T_ENEMY || (k && labs(s.x - hp.x) < 4000 && labs(s.y - hp.y) < 4000))
                LOG("dump ref=%u type=%d kind=%d screen=%ld,%ld hp=%d", r, o->type, k, s.x, s.y, k == T_ENEMY ? *(int *)((BYTE *)o + 0x4d8) : 0);
        }
    }
    target = best_ref, target_pos = best_pos;
}

// ---------------------------------------------------------------- focus: what can be selected on screen
// Clickable controls are recorded as the game draws them (their render, vtable slot 5, is hooked per class), so the
// list is exactly what is on screen: hidden pages, tabs and closed windows never draw. Item grids come from the
// inventory data. The list is built on the render thread and published at each present.
typedef struct { RECT r; short kind, tag; POINT a, b; } Focus; // a/b: where a click decreases / increases a slider
enum { F_CTL, F_ITEM, F_ART, F_SLIDER }; // F_ITEM: an item in a grid (new screens start there); F_ART: a combat art
enum { TG_NONE, TG_QUIT, TG_PREF, TG_ANCARIA, TG_UNDERWORLD, TG_EDIT, TG_PEDESTAL, TG_WEAPON }; // what an entry is
#define MAXF 256
static Focus fbuf[2][MAXF];
static volatile int fpub, fcount[2]; // fbuf[fpub] is the published list, fbuf[!fpub] is being built
static volatile int fmodal[2];       // the list belongs to 1: a confirmation pop-up, 2: an NPC conversation
static volatile int carrying;        // an item hangs on the cursor (mouse +0x68)
static volatile DWORD wins;          // visible windows: bit i = cUI_Manager window slot i (11 save/load, 20 heroes)
static volatile int diary_tab;       // the quest log's open tab
static RECT diary_tabs[4];           // ... and where its four tabs are
static volatile int campaign_uw;     // the hero screen was opened for the Underworld campaign
static volatile int legend_covered; // an open window reaches into the top-left corner where the menu legend goes
static volatile POINT npc_ok;        // the only answer of an open NPC dialog (OK), for B; x < 0 when none
static RECT focus;                   // current menu focus; empty = none
static int free_cursor;              // the right stick took over in a menu
static volatile int cursor_hidden;   // the pad drives: the game does not draw its cursor

static Focus *add_focus(RECT r, int kind, int tag) {
    int w = !fpub, n = fcount[w];
    if (n >= MAXF || r.right <= r.left || r.bottom <= r.top) return NULL;
    for (int i = 0; i < n; i++) {
        RECT *o = &fbuf[w][i].r;
        if (abs(o->left - r.left) <= 8 && abs(o->top - r.top) <= 8 && o->right - o->left == r.right - r.left)
            return NULL; // text is drawn twice (shadow pass, a few pixels off): keep one
        if (tag == TG_EDIT && fbuf[w][i].tag == TG_EDIT && o->left == r.left && o->right == r.right &&
            r.top - o->bottom < 14 && o->top - r.bottom < 14)
            return UnionRect(o, o, &r), NULL; // a save's name / file / date fields: one row
    }
    fbuf[w][n] = (Focus){r, kind, tag}, fcount[w] = n + 1;
    return &fbuf[w][n];
}

static const struct { DWORD vt; int cmd; } clickable[] = { // cmd: offset of the command a click runs, 0 = any
    {0x897308, 0}, {0x89733c, 0}, {0x897700, 0x98}, {0x897758, 0x88}, {0x897534, 0}, {0x8974d8, 0},
    {0x8973c0, 0}, {0x8975e8, 0}, {0x897620, 0}, {0, 0}};
static void (__fastcall *render_orig[16])(void *, void *, void *);
static void __fastcall h_render(Ctl *c, void *edx, void *dev) {
    int i = 0;
    while (clickable[i].vt && clickable[i].vt != (DWORD)c->vt) i++;
    if (clickable[i].vt && (c->flags & 5) == 5 && (!clickable[i].cmd || *(void **)((BYTE *)c + clickable[i].cmd))) {
        Ctl *root = c;
        for (int k = 0; k < 16 && ctl_ok(root->parent); k++) root = root->parent;
        // Not the HUD: the taskbar, the party portraits, and the compass potions (parentless 32x32 buttons that
        // stay up in the world; screens like the character select and the save menu use parentless buttons too).
        if (root->vt != (void **)0x896ac0 && root->vt != (void **)0x8968cc && !(root == c && c->w <= 34 && c->h <= 34)) {
            static const struct { const char *name; int tag; } named[] = {{"UI_LABEL_QUIT", TG_QUIT},
                {"UI_ESC_CONTINUE", TG_PREF}, {"UI_LABEL_NEWGAME", TG_ANCARIA}, {"UI_LABEL_NEWADDON", TG_UNDERWORLD}};
            int tag = c->vt == (void **)0x897534 ? TG_EDIT : TG_NONE;
            for (unsigned k = 0; k < sizeof named / sizeof *named; k++) if (!strcmp(c->name, named[k].name)) tag = named[k].tag;
            POINT p = ctl_pos(c);
            add_focus((RECT){p.x, p.y, p.x + c->w, p.y + c->h}, F_CTL, tag);
        }
    }
    render_orig[i](c, edx, dev);
}
// Tab sheets (cUI_TabSheet: backpack/combat arts/combo, the merchant's weapons/armour/misc...) draw their own
// header row 26 px above the sheet; tabs are centred at +73, +229, +391. ponytail: three tabs assumed, read the
// tab count from the sheet if a screen with fewer/more ever turns up.
static volatile RECT sheets[4];
static volatile int nsheets, sheet_tab[4]; // sheet_tab: the tab shown (tab control at +0x78, index at +0x84)
static RECT pub_sheets[4];
static volatile int pub_nsheets, pub_sheet_tab[4];
static int inventory_sheet(void) { // the inventory's tab sheet (the lower one when a merchant shows its own too)
    for (int i = 0; i < pub_nsheets; i++) if (pub_sheets[i].top > 300) return i;
    return -1;
}
static void (__fastcall *sheet_render)(void *, void *, void *) = (void *)0x7297f0;
static void __fastcall h_sheet_render(Ctl *c, void *edx, void *dev) {
    if ((c->flags & 1) && nsheets < 4) {
        POINT p = ctl_pos(c);
        BYTE *tc = *(BYTE **)((BYTE *)c + 0x78);
        sheet_tab[nsheets] = tc && !IsBadReadPtr(tc + 0x84, 2) ? *(WORD *)(tc + 0x84) % 3 : 0;
        sheets[nsheets++] = (RECT){p.x, p.y - 26, p.x + c->w, p.y + c->h};
        static const int tab_x[] = {73, 229, 391};
        for (int i = 0; i < 3; i++) add_focus((RECT){p.x + tab_x[i] - 70, p.y - 38, p.x + tab_x[i] + 70, p.y - 14}, F_CTL, 0);
    }
    sheet_render(c, edx, dev);
}

// The inventory's Combat Arts tab (cUI_SpellTab) draws its art icons itself: 16-byte records at +0x2cc..+0x2d0
// (+0 bit 0 present, +4/+6 screen x/y, 64x64). The Combo tab's four slots are controls at +0x154..+0x160.
static void (__fastcall *spell_render)(void *, void *, void *) = (void *)0x6c25a0;
static void __fastcall h_spell_render(Ctl *c, void *edx, void *dev) {
    BYTE *b = *(BYTE **)((BYTE *)c + 0x2cc), *e = *(BYTE **)((BYTE *)c + 0x2d0);
    if ((c->flags & 1) && b && e > b && e - b <= 64 * 16 && !IsBadReadPtr(b, e - b))
        for (; b < e; b += 16)
            if (*b & 1) add_focus((RECT){*(short *)(b + 4), *(short *)(b + 6), *(short *)(b + 4) + 64, *(short *)(b + 6) + 64}, F_ART, 0);
    spell_render(c, edx, dev);
}
static void (__fastcall *combo_render)(void *, void *, void *) = (void *)0x6bf6f0;
static void __fastcall h_combo_render(Ctl *c, void *edx, void *dev) {
    if (c->flags & 1)
        for (Ctl **k = (Ctl **)((BYTE *)c + 0x154); k <= (Ctl **)((BYTE *)c + 0x160); k++)
            if (ctl_ok(*k)) {
                POINT p = ctl_pos(*k);
                add_focus((RECT){p.x, p.y, p.x + (*k)->w, p.y + (*k)->h}, F_ART, 0);
            }
    combo_render(c, edx, dev);
}

// Sliders (cUI_Scrollbar: the volume and map-transparency bars, the save list's scroll bar): two embedded arrow
// buttons at +0x94 / +0x110 step the value; the pad clicks them (or the track, see below).
static void (__fastcall *slider_render)(void *, void *, void *) = (void *)0x752ee0;
static void __fastcall h_slider_render(Ctl *c, void *edx, void *dev) {
    if ((c->flags & 5) == 5) {
        POINT p = ctl_pos(c), arrow[2];
        for (int i = 0; i < 2; i++) {
            Ctl *a = (Ctl *)((BYTE *)c + (i ? 0x110 : 0x94));
            POINT q = ctl_pos(a);
            arrow[i] = (POINT){q.x + a->w / 2, q.y + a->h / 2};
        }
        Focus *f = add_focus((RECT){p.x, p.y, p.x + c->w, p.y + c->h}, F_SLIDER, 0);
        if (f) f->a = arrow[0], f->b = arrow[1];
        // A horizontal slider steps by a tenth of its range instead (an arrow click barely moves it): a click on its
        // track (+0x18c) centres the knob (+0x208) there, so the step points sit beside the knob, clear of it.
        Ctl *track = (Ctl *)((BYTE *)c + 0x18c), *knob = (Ctl *)((BYTE *)c + 0x208);
        if (f && c->w > c->h && track->w > knob->w) {
            POINT t = ctl_pos(track), k = ctl_pos(knob);
            int kc = k.x + knob->w / 2, step = (track->w - knob->w) / 10, y = t.y + track->h / 2;
            if (step < knob->w / 2 + 2) step = knob->w / 2 + 2;
            f->a = (POINT){kc - step > t.x ? kc - step : t.x + 1, y};
            f->b = (POINT){kc + step < t.x + track->w - 1 ? kc + step : t.x + track->w - 2, y};
        }
    }
    slider_render(c, edx, dev);
}

// The confirmation pop-up's draw (cUI_BusyDlg, vtable slot 5): what is recorded after it starts is the pop-up's.
static volatile int modal_mark = -1;
static void (__fastcall *busy_render)(void *, void *, void *) = (void *)0x721b10;
static void __fastcall h_busy_render(void *dlg, void *edx, void *dev) {
    if (((Ctl *)dlg)->flags & 1) modal_mark = fcount[!fpub]; // it is called every frame, visible or not
    busy_render(dlg, edx, dev);
}
static void hook_renders(void) { // vtable slot 5 = render(dev)
    patch_ptr((void **)0x897134 + 5, h_busy_render), patch_ptr((void **)0x897258 + 5, h_sheet_render);
    patch_ptr((void **)0x896610 + 5, h_spell_render), patch_ptr((void **)0x896578 + 5, h_combo_render);
    patch_ptr((void **)0x897664 + 5, h_slider_render);
    for (int i = 0; clickable[i].vt; i++) render_orig[i] = patch_ptr((void **)clickable[i].vt + 5, h_render);
}

// Open windows (for "is a panel open") and the data-driven focusables, then publish.
static volatile int ui_open;
// Item grids: every grid window (backpack, chest, merchant, trade, cube...) writes its grid's screen origin into the
// cInventoryEntry it shows (+0x1164/+0x1166) and draws it with 0x5dc430; that call is hooked, so each frame we know
// every grid on screen. Entry: +0x18 cells of 12 bytes (+0 item ref in the top-left cell, +5/+6 item width/height),
// +0x115c/+0x115e grid width/height.
static BYTE *grids[8];
static volatile int ngrids;
static void (__fastcall *grid_draw)(void *, void *, void *, int) = (void *)0x5dc430;
static void __fastcall h_grid_draw(BYTE *entry, void *edx, void *dev, int flag) {
    int n = ngrids;
    if (n < 8) grids[n] = entry, ngrids = n + 1;
    grid_draw(entry, edx, dev, flag);
}

// The backpack's biggest empty square (centre, size in cells): where an item left on the cursor goes (a weapon
// swapped out of a hotbar slot, or one that doesn't fit back where it came from).
static volatile POINT free_spot = {-1, -1};
static int spot_size;
static POINT spot;
static void add_grid(BYTE *e) {
    int gw = *(WORD *)(e + 0x115c), gh = *(WORD *)(e + 0x115e), x0 = *(short *)(e + 0x1164), y0 = *(short *)(e + 0x1166);
    if (gw <= 0 || gh <= 0 || gw > 256 || gh > 256 || gw * gh > 256) return;
    int sh = inventory_sheet();
    if (sh >= 0 && PtInRect(&pub_sheets[sh], (POINT){x0 + 1, y0 + 1})) { // the backpack (cells: +0 item at its top-left)
        BYTE occ[256] = {0}, sq[256];
        for (int i = 0; i < gw * gh; i++) {
            BYTE *c = e + 0x18 + i * 12;
            for (int y = 0; *(int *)c && y < c[6]; y++)
                for (int x = 0; x < c[5]; x++)
                    if (i % gw + x < gw && i / gw + y < gh) occ[i + y * gw + x] = 1;
        }
        for (int i = gw * gh - 1; i >= 0; i--) {
            int last = i % gw == gw - 1 || i / gw == gh - 1, m = last ? 0 : sq[i + 1] < sq[i + gw] ? sq[i + 1] : sq[i + gw];
            if (!last && sq[i + gw + 1] < m) m = sq[i + gw + 1];
            sq[i] = occ[i] ? 0 : 1 + m;
            if (sq[i] >= spot_size) spot_size = sq[i], spot = (POINT){x0 + i % gw * 32 + sq[i] * 16, y0 + i / gw * 32 + sq[i] * 16};
        }
    }
    for (int i = 0; i < gw * gh; i++) {
        BYTE *cell = e + 0x18 + i * 12;
        RECT r = {x0 + i % gw * 32, y0 + i / gw * 32, x0 + i % gw * 32 + 32, y0 + i / gw * 32 + 32};
        if (carrying) add_focus(r, F_CTL, 0); // placing: every cell
        else if (*(int *)cell) { // +0 the item's ref, +5/+6 its size in cells
            Obj *o = obj(*(unsigned *)cell);
            r.right = r.left + 32 * cell[5], r.bottom = r.top + 32 * cell[6];
            add_focus(r, F_ITEM, o && family(o->type) == 5 ? TG_WEAPON : 0); // family 5: weapons (bows and staves too)
        }
    }
}
static void snapshot_ui(void) {
    BYTE *m = UIMGR, *mouse = *(BYTE **)0xcddb5c;
    int open = 0, answers = 0;
    POINT ok = {-1, -1};
    RECT ans[4];
    carrying = mouse && *(int *)(mouse + 0x68);
    if (m) {
        Ctl **w = (Ctl **)(m + 0x80);
        int covered = 0;
        DWORD vis = 0;
        for (int i = 0; i < 23; i++) // not the taskbar (0), party portraits (7) or slot 19: they are always up
            if (i != 0 && i != 7 && i != 19 && ctl_ok(w[i]) && (w[i]->flags & 1)) {
                POINT p = ctl_pos(w[i]);
                open = 1, vis |= 1u << i, covered |= p.x < 440 && p.y < 40 && w[i]->w > 0 && w[i]->h > 0; // e.g. a merchant's tabs
            }
        Ctl *menu_save = *(Ctl **)(m + 0x13c), *menu_heroes = *(Ctl **)(m + 0x134); // the main menu's own copies
        if (ctl_ok(menu_save) && (menu_save->flags & 1)) vis |= 1u << 11;
        if (ctl_ok(menu_heroes) && menu_heroes->vt == (void **)0x896fa8 && (menu_heroes->flags & 1)) vis |= 1u << 20;
        wins = vis;
        legend_covered = covered;
        Ctl **end, **d = dialogs(m, &end);
        for (; d && d < end; d++)
            if (ctl_ok(*d) && ((*d)->flags & 1)) {
                open = 1;
                if ((*d)->vt == (void **)0x896b3c) { // NPC text (cUI_Tooltip): up to 4 answers it draws itself
                    POINT p = ctl_pos(*d);       // records of 0x14 at +0x2180: +0 id, +4 x, +8 y, +0xc w, +0xe h
                    for (BYTE *a = (BYTE *)*d + 0x2180; a < (BYTE *)*d + 0x2180 + 4 * 0x14 && answers < 4; a += 0x14) {
                        int x = *(int *)(a + 4), y = *(int *)(a + 8), aw = *(short *)(a + 0xc), ah = *(short *)(a + 0xe);
                        if (aw > 0 && ah > 0)
                            ans[answers++] = (RECT){p.x + x, p.y + y, p.x + x + aw, p.y + y + ah},
                            ok = (POINT){p.x + x + aw / 2, p.y + y + ah / 2};
                    }
                }
            }
        // Hero select / import: the heroes stand on eight pedestals (3D models, not controls), placed differently in
        // the two campaigns' scenes. Points measured by hovering each (the game highlights the slot).
        if (vis & 1u << 20) {
            static const POINT pedestal[2][8] = {
                {{100, 470}, {216, 360}, {380, 400}, {524, 440}, {630, 340}, {730, 420}, {810, 350}, {890, 480}},
                {{95, 466}, {220, 350}, {360, 360}, {528, 416}, {630, 340}, {718, 396}, {806, 416}, {906, 600}}};
            const POINT *pt = pedestal[campaign_uw != 0];
            for (int i = 0; i < 8; i++) add_focus((RECT){pt[i].x - 30, pt[i].y - 60, pt[i].x + 30, pt[i].y + 60}, F_CTL, TG_PEDESTAL);
        }
        // The quest log (cUI_Diary) draws its four tab icons from embedded controls at +0x538 (0x7c apart); the open
        // tab is +0x7a4. ponytail: the quest entries themselves aren't decoded yet (right stick reaches them).
        if (vis & 1u << 10) {
            BYTE *dy = (BYTE *)w[10];
            for (int i = 0; i < 4; i++) {
                Ctl *t = (Ctl *)(dy + 0x538 + i * 0x7c);
                POINT p = ctl_pos(t);
                diary_tabs[i] = (RECT){p.x, p.y, p.x + t->w, p.y + 64};
                add_focus(diary_tabs[i], F_CTL, 0);
            }
            diary_tab = *(WORD *)(dy + 0x7a4);
        }
        spot_size = 0;
        for (int i = 0; i < ngrids; i++) add_grid(grids[i]);
        free_spot = spot_size ? spot : (POINT){-1, -1};
        Ctl *eq = w[2];
        if (ctl_ok(eq) && (eq->flags & 1)) // equipment slots: the UI_EQ_* controls at cUI_Equipment +0x15c..+0x1a0
            for (Ctl **slot = (Ctl **)((BYTE *)eq + 0x15c); slot <= (Ctl **)((BYTE *)eq + 0x1a0); slot++)
                if (ctl_ok(*slot) && ((*slot)->flags & 1)) {
                    POINT p = ctl_pos(*slot);
                    add_focus((RECT){p.x, p.y, p.x + (*slot)->w, p.y + (*slot)->h}, F_CTL, 0);
                }
    }
    // A confirmation pop-up (cUI_BusyDlg: Quit? Load game? ...) is modal: only what it drew is focusable.
    int nb = !fpub, modal = modal_mark >= 0;
    if (modal) {
        int k = modal_mark, n = fcount[nb] - k;
        memmove(fbuf[nb], fbuf[nb] + k, (n > 0 ? n : 0) * sizeof *fbuf[nb]);
        fcount[nb] = n > 0 ? n : 0, open = 1, modal_mark = -1;
    }
    // A merchant's message box (cUI_Purchase, window 13: "You do not have enough gold!") is modal too; its buttons
    // are 12-byte screen rects at +0x160, +0x16c, +0x178 (x, y ints, w, h shorts).
    Ctl *buy = m ? ((Ctl **)(m + 0x80))[13] : NULL;
    if (ctl_ok(buy) && (buy->flags & 1)) {
        fcount[nb] = 0, modal = 1, open = 1;
        for (BYTE *r = (BYTE *)buy + 0x160; r <= (BYTE *)buy + 0x178; r += 12) {
            int x = *(int *)r, y = *(int *)(r + 4), bw = *(short *)(r + 8), bh = *(short *)(r + 10);
            if (bw > 0 && bh > 0 && x >= 0 && y >= 0 && x < 1024 && y < 768) add_focus((RECT){x, y, x + bw, y + bh}, F_CTL, 0);
        }
    }
    if (answers && !modal) { // an NPC conversation (answers drawn by the dialog itself) holds the focus as well
        fcount[nb] = 0, modal = 2;
        for (int i = 0; i < answers; i++) add_focus(ans[i], F_CTL, 0);
    }
    fmodal[nb] = modal;
    ui_open = open;
    npc_ok = answers == 1 ? ok : (POINT){-1, -1};
    int w = !fpub;
    for (int i = 0; i < nsheets; i++) pub_sheets[i] = sheets[i], pub_sheet_tab[i] = sheet_tab[i];
    pub_nsheets = nsheets;
    MemoryBarrier(); // the list is complete before it is published (volatile alone doesn't order the plain writes)
    fpub = w, fcount[!w] = 0, ngrids = 0, nsheets = 0; // publish; start the next list
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
static int __cdecl h_movie_open(void *a, void *b, void **stream) { return *stream = NULL, E_FAIL; }

// ---------------------------------------------------------------- overlay: button icons and the aim marker
// Glyphs are painted once with GDI into small colour-keyed DirectDraw surfaces, then blitted onto the back
// buffer right before the game flips (cDxDriver7 0x645e40: +0xb4 IDirectDraw7, +0xbc back buffer).
enum { G_A, G_B, G_X, G_Y, G_LB, G_RB, G_LT, G_RT, G_L3, G_RS, G_START, G_BACK, G_UP, G_DOWN, G_LEFT, G_RIGHT,
       G_RING, G_LOCK, G_LEGEND }; // then one glyph per legend
enum { L_PANEL, L_WEAPON, L_ARTS, L_SHOP, L_PAGES, L_SELECT, L_CARRY, L_ASSIGN, L_LT, L_STAND, L_N };
#define G_N (G_LEGEND + L_N)
static IDirectDrawSurface7 *glyph[G_N], *glyph_back;
static SIZE gsize[G_N];
static DWORD frame_colour; // the focus frame's gold in the back buffer's pixel format
static const int art_g[] = {G_X, G_Y, G_B, G_RB, G_RT}; // the buttons of art slots 1..5

static void paint_button(HDC dc, int x, int y, int g) { // one button icon with its top-left at x, y
    static const COLORREF face[] = {RGB(60, 176, 67), RGB(214, 56, 46), RGB(47, 120, 222), RGB(240, 192, 0)};
    static const char *label[] = {"A", "B", "X", "Y", "LB", "RB", "LT", "RT", "L3", "RS", "", ""};
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
    if (g == G_START) // Menu: three lines
        for (int i = 0; i < 3; i++) PatBlt(dc, x + 9, y + 6 + 4 * i, 12, 2, WHITENESS);
    if (g == G_BACK) { // View: two overlapping windows
        SelectObject(dc, GetStockObject(WHITE_PEN)), SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, x + 8, y + 5, x + 18, y + 13), Rectangle(dc, x + 12, y + 9, x + 22, y + 17);
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

// Legends: what the buttons do right now, top-left (the main menu's at the bottom).
static const struct { int n, g[6]; const char *t[6]; } legends[L_N] = {
    [L_PANEL] = {4, {G_A, G_X, G_B, G_LB}, {"Select", "Use / equip", "Close", "/ RB  pages"}},
    [L_WEAPON] = {5, {G_A, G_X, G_Y, G_B, G_LB}, {"Select", "Equip", "Put on a button", "Close", "/ RB  pages"}},
    [L_ARTS] = {3, {G_A, G_B, G_LB}, {"Put on a button", "Close", "/ RB  pages"}},
    [L_SHOP] = {4, {G_A, G_X, G_B, G_LB}, {"Select", "Use / equip", "Close", "/ RB  shop tabs"}},
    [L_PAGES] = {3, {G_A, G_B, G_LB}, {"Select", "Close", "/ RB  pages"}},
    [L_SELECT] = {2, {G_A, G_B}, {"Select", "Back"}}, // menus, dialogs
    [L_CARRY] = {2, {G_A, G_B}, {"Place", "Put back"}},
    [L_ASSIGN] = {2, {G_X, G_A}, {"Y  B  RB  RT: put it on that button", "Cancel"}},
    [L_LT] = {6, {G_LT, G_START, G_BACK, G_UP, G_A, G_RS},
              {"held:", "Quick save", "Minimap (hold)", "Mentor potion", "Dismount", "Zoom"}},
    [L_STAND] = {1, {G_L3}, {"Standing still"}},
};
#define LEGEND(dc, l, paint) legend_width(dc, legends[l].g, legends[l].t, legends[l].n, paint)

static void paint_glyph(HDC dc, int g, int w, int h) {
    if (g == G_RING || g == G_LOCK) {
        COLORREF c = g == G_RING ? RGB(255, 214, 90) : RGB(230, 60, 50);
        HPEN pen = CreatePen(PS_SOLID, 3, c), op = SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, 2, 2 + h / 4, w - 2, h - 2 - h / 4); // flat ellipse: a circle on the isometric ground
        if (g == G_RING) MoveToEx(dc, w / 2 - 6, h / 2, NULL), LineTo(dc, w / 2 + 7, h / 2), MoveToEx(dc, w / 2, h / 2 - 5, NULL), LineTo(dc, w / 2, h / 2 + 6);
        SelectObject(dc, op), SelectObject(dc, ob), DeleteObject(pen);
    } else if (g >= G_LEGEND) LEGEND(dc, g - G_LEGEND, 1);
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
        if (!sz.cx) sz.cx = LEGEND(sdc, g - G_LEGEND, 0); // legends: measure the text first
        DDSURFACEDESC2 d = {sizeof d};
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY; // GPU blits: system memory costs ~2 ms/frame on wined3d
        d.dwWidth = sz.cx, d.dwHeight = sz.cy, d.ddpfPixelFormat = pf;
        if (IDirectDraw7_CreateSurface(dd, &d, &glyph[g], NULL)) {
            d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
            if (IDirectDraw7_CreateSurface(dd, &d, &glyph[g], NULL)) { LOG("overlay: CreateSurface failed"); break; }
        }
        HDC dc;
        if (!IDirectDrawSurface7_GetDC(glyph[g], &dc)) {
            RECT all = {0, 0, sz.cx, sz.cy};
            HBRUSH key = CreateSolidBrush(RGB(255, 0, 255));
            FillRect(dc, &all, key), DeleteObject(key);
            paint_glyph(dc, g, sz.cx, sz.cy);
            IDirectDrawSurface7_ReleaseDC(glyph[g], dc);
        }
        DWORD k = pf.dwRBitMask | pf.dwBBitMask; // magenta in any RGB format
        const DWORD masks[3] = {pf.dwRBitMask, pf.dwGBitMask, pf.dwBBitMask}, gold[3] = {255, 200, 64};
        frame_colour = 0;
        for (int i = 0; i < 3; i++) frame_colour |= (DWORD)((unsigned long long)masks[i] * gold[i] / 255) & masks[i];
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

// A two-pixel gold frame around r (colour fills: no glyph needed for arbitrary sizes).
static void frame(IDirectDrawSurface7 *back, RECT r) {
    DDBLTFX fx = {sizeof fx};
    fx.dwFillColor = frame_colour;
    InflateRect(&r, 2, 2);
    RECT e[4] = {{r.left, r.top, r.right, r.top + 2}, {r.left, r.bottom - 2, r.right, r.bottom},
                 {r.left, r.top, r.left + 2, r.bottom}, {r.right - 2, r.top, r.right, r.bottom}};
    for (int i = 0; i < 4; i++) {
        RECT x = e[i], screen = {0, 0, 1024, 768};
        if (IntersectRect(&x, &x, &screen)) IDirectDrawSurface7_Blt(back, &x, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    }
}

// HUD geometry (1024x768): art slots 6..0 grow right from x=670, weapon slots 1..n end at x=362; one slot more at
// levels 2, 8, 16 and 30. Potions sit on the compass at the top of the HUD.
static void draw_overlay(BYTE *drv, IDirectDrawSurface7 *back) {
    static volatile LONG busy;
    IDirectDraw7 *dd = *(IDirectDraw7 **)(drv + 0xb4);
    if (!dd || !back || InterlockedExchange(&busy, 1)) return;
    IDirectDrawSurface7 *ref = *(IDirectDrawSurface7 **)(drv + 0xbc); // glyph format follows the back buffer
    if (ref != glyph_back) free_glyphs(), glyph_back = ref;
    if (!glyph[0] && !make_glyphs(dd, ref)) { InterlockedExchange(&busy, 0); return; }
    int lt = (pad_shown & BTN_LT) != 0, in_world = hero() != NULL, world = in_world && !ui_open;
    if (aiming) put(back, G_RING, reticle.x, reticle.y);
    if (pad_mode && !world && !free_cursor && !IsRectEmpty(&focus)) frame(back, focus);
    if (cfg.hints && pad_mode) {
        int legend = -1;
        if (world) {
            int n = art_slots();
            for (int i = 0; i < n; i++) put(back, art_g[i], (lt ? weapon_slot(i) : art_slot(i)).x, 690);
            put(back, G_LT, 362 - n * 67 + 12, 712);
            put(back, G_UP, lt ? 513 : 452, 652), put(back, G_RIGHT, 483, 652), put(back, G_LEFT, 545, 652), put(back, G_DOWN, 576, 652);
            if (target && !cursor_mode && !aiming) {
                int above = target_kind == T_ENEMY || target_kind == T_NPC ? 85 : 30;
                put(back, G_A, target_pos.x, target_pos.y - above);
            }
            if (locked && target == locked) put(back, G_LOCK, target_pos.x, target_pos.y);
            legend = lt ? L_LT : stand ? L_STAND : -1;
        } else if (assigning) { // which button gets it: the slots' buttons, and the prompt
            for (int i = 0, n = art_slots(); i < n; i++) put(back, art_g[i], (assign_kind == 2 ? weapon_slot(i) : art_slot(i)).x, 690);
            legend = L_ASSIGN;
        } else if (!in_world) legend = L_SELECT;
        else if (carrying) legend = L_CARRY;
        else if (fmodal[fpub] || npc_ok.x >= 0) legend = L_SELECT;
        else if (wins & (1u << 1 | 1u << 4))
            legend = legend_covered ? -1 : wins & 1u << 4 ? L_SHOP : focus_tag == TG_WEAPON ? L_WEAPON : focus_kind == F_ART ? L_ARTS : L_PANEL;
        else legend = wins & (1u << 10 | 1u << 12) ? L_PAGES : L_SELECT; // quest log, game menu
        if (legend >= 0) {
            int g = G_LEGEND + legend;
            put(back, g, 12 + gsize[g].cx / 2, in_world ? 20 : 748);
        }
    }
    InterlockedExchange(&busy, 0); // a release the other render thread sees after our glyph writes
}

// The game's cursor draw (mouse, device, glow): skipped while the pad hides the cursor.
static void (__fastcall *cursor_draw)(void *, void *, void *, int) = (void *)0x655450;
static void __fastcall h_cursor_draw(void *mouse, void *edx, void *dev, int glow) {
    if (!cursor_hidden) cursor_draw(mouse, edx, dev, glow);
}

static void (__fastcall *flip)(void *) = (void *)0x645e40;
static void __fastcall h_flip(void *drv) {
    BYTE *d = drv;
    snapshot_ui(); // every present: the world's engine thread and the menus' own thread both come through here
    if (ctl_uiscan) ctl_uiscan = 0, ui_scan_all();
    // The fullscreen main menu (menu thread, 0x757250) does not show what is blitted onto its back buffer:
    // there the overlay goes onto the front surface right after the flip.
    if (__builtin_return_address(0) == (void *)0x757255 && *(int *)(d + 0x90) == 1) {
        draw_overlay(d, *(IDirectDrawSurface7 **)(d + 0xbc));
        flip(drv);
        draw_overlay(d, *(IDirectDrawSurface7 **)(d + 0xb8));
        return;
    }
    draw_overlay(d, *(IDirectDrawSurface7 **)(d + 0xbc));
    flip(drv);
}

static void patch_engine(void) { // after the SteamStub has decrypted .text, i.e. once the window exists
    patch_call(0x64490d, h_device_setup);
    if (GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version")) patch_rel(0x6a0d80, h_movie_open, 0xE9);
    patch_call(0x60ea8a, h_flip);        // world frame present
    patch_call(0x757250, h_flip);        // menu-thread present (main menu)
    static const DWORD grid_calls[] = {0x6a347d, 0x6a348b, 0x6acc3f, 0x6c14f5, 0x6daaa9, 0x6e8c3d, 0x6eafb1, 0x6eafc2};
    for (int i = 0; i < 8; i++) patch_call(grid_calls[i], h_grid_draw);
    patch_call(0x612e6a, h_cursor_draw); // the world's cursor
    patch_call(0x612e91, h_cursor_draw);
    patch_call(0x757227, h_cursor_draw); // the menus' cursor
    hook_renders();
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

static int walking, acting;

// Menus: focus moves between the published focusables; the (hidden) cursor sits on the focused one so the game's own
// hover effects and tooltips show, and A/X click there. The right stick is a free cursor for anything not covered.

static POINT centre(RECT r) { return (POINT){(r.left + r.right) / 2, (r.top + r.bottom) / 2}; }

static int focus_list(Focus *out) { // copy the published list; again if the render thread republished meanwhile
    int p, n;
    do p = fpub, MemoryBarrier(), n = fcount[p], memcpy(out, fbuf[p], n * sizeof *out), MemoryBarrier(); while (p != fpub);
    return n;
}

// The focusable nearest in direction (dx, dy) from the current focus: distance along the direction plus twice the
// sideways gap, within a cone; entries straight ahead win over diagonal ones.
static int tall(RECT r) { return r.bottom - r.top > r.right - r.left; }
static int step_focus(Focus *f, int n, int dx, int dy) {
    POINT c = centre(focus);
    int best = -1;
    float bs = 1e9f;
    for (int i = 0; i < n; i++) {
        if (EqualRect(&f[i].r, &focus)) continue;
        POINT q = centre(f[i].r);
        RECT *r = &f[i].r; // sideways: the gap to the entry's span (0 when it lies straight ahead, e.g. a wide slider)
        int lo = dy ? r->left : r->top, hi = dy ? r->right : r->bottom, at = dy ? c.x : c.y;
        float along = (float)(q.x - c.x) * dx + (float)(q.y - c.y) * dy, across = at < lo ? lo - at : at > hi ? at - hi : 0;
        if (along < 4 || across > along * 2.5f) continue;
        if (along + 2 * across < bs) bs = along + 2 * across, best = i;
    }
    return best;
}

// Scripted UI steps (drag and drop, panel switching): a click moves the cursor, waits so the game sees it arrive,
// then presses and releases (~190 ms); a key is tapped, then the next step waits ~130 ms for the screen to change.
static struct { POINT p; int vk; } steps[8];
static int nsteps, step_t;
static void queue_click(int x, int y) { if (nsteps < 8) steps[nsteps].vk = 0, steps[nsteps++].p = (POINT){x, y}; }
static void queue_key(int k) { if (nsteps < 8) steps[nsteps++].vk = k; }
static int run_steps(void) { // 1 while busy
    if (!nsteps) return 0;
    int k = steps[0].vk, t = step_t++;
    POINT c = steps[0].p;
    if (k) { if (t == 0) tap(k); }
    else {
        if (t == 0) move_cursor(c.x - 1, c.y);
        if (t == 6) move_cursor(c.x, c.y);
        if (t == 12) key(VK_LBUTTON, 1);
        if (t == 16) key(VK_LBUTTON, 0);
    }
    if (step_t >= (k ? 16 : 24)) memmove(steps, steps + 1, --nsteps * sizeof *steps), step_t = 0;
    return 1;
}

// The D2R-style panel cycle (Start opens it, LB/RB step through): backpack, combat arts, combo (the inventory's
// tabs), the quest log's four tabs, then the game menu.
enum { PG_INV = 0, PG_QUEST = 3, PG_MENU = 7, PG_N = 8 };
static int cur_page(void) {
    DWORD w = wins;
    int s = inventory_sheet(); // once: the render thread republishes the sheets meanwhile
    if (w & 1u << 12) return PG_MENU;
    if (w & 1u << 10) return PG_QUEST + diary_tab % 4;
    if ((w & 1u << 1) && !(w & 1u << 4) && s >= 0) return PG_INV + pub_sheet_tab[s];
    return -1;
}
static int page_id(void) { // what LB / RB page through: a panel page, or the tab of a merchant's wares
    int pg = cur_page();
    for (int i = 0; i < pub_nsheets && (wins & 1u << 4); i++) if (pub_sheets[i].top < 100) pg = 100 + pub_sheet_tab[i];
    return pg;
}
static int pending_page = -1;
static DWORD pending_until, pending_click;
static int past(DWORD t) { return GetTickCount() - t < 0x80000000u; } // now >= t, across the 49.7-day wrap
static void click_tab(int page) { // a tab of the panel already open
    POINT c;
    if (page < PG_QUEST) {
        int s = inventory_sheet();
        if (s < 0) return; // the inventory closed meanwhile
        RECT r = pub_sheets[s];
        c = (POINT){r.left + (int[]){73, 229, 391}[page], r.top};
    } else c = centre(diary_tabs[page - PG_QUEST]);
    free_cursor = 0;
    queue_click(c.x, c.y);
}
static void goto_page(int page) {
    int cur = cur_page();
    if (cur < 0 || page == cur) return;
    if (page != PG_MENU && (page < PG_QUEST) == (cur < PG_QUEST) && cur != PG_MENU) { click_tab(page); return; }
    if (cur == PG_MENU) queue_key(VK_ESCAPE);                   // leave the game menu first
    if (page == PG_MENU) queue_key(VK_ESCAPE), queue_key(VK_ESCAPE); // close the panel, open the menu
    else queue_key(page < PG_QUEST ? 'I' : 'L');                 // the game swaps inventory <-> quest log itself
    pending_page = page, pending_until = GetTickCount() + 2000, pending_click = GetTickCount();
}
static void settle_page(void) { // after a panel switch: click the wanted tab once the panel is up
    if (pending_page < 0) return;
    int cur = cur_page();
    if (cur == pending_page || past(pending_until)) { pending_page = -1; return; }
    if (cur >= 0 && (cur < PG_QUEST) == (pending_page < PG_QUEST) && pending_page != PG_MENU && past(pending_click))
        click_tab(pending_page), pending_click = GetTickCount() + 400;
}

// Per-screen memory: the focus last used on a screen comes back when the screen does (backing out of a sub-menu).
static struct { int sig; RECT r; } screens[8];
static int screen_sig(Focus *f, int n) { return n ? n * 7919 + f[0].r.left * 31 + f[0].r.top : 0; }
static RECT *remembered(int sig) {
    for (int i = 0; i < 8; i++) if (screens[i].sig == sig) return &screens[i].r;
    return NULL;
}
static void remember(int sig, RECT r) {
    static int next;
    RECT *have = remembered(sig);
    if (have) *have = r;
    else screens[next].sig = sig, screens[next].r = r, next = (next + 1) % 8;
}

static int nav_held;          // a direction is held (also set on waking the pad, so that press doesn't move)
static POINT want;            // focus the entry under this point when it shows up (e.g. Start Game after a hero)
static DWORD want_until;
static POINT origin;          // where the carried item was picked up: B / Start / Back put it back there
static int a_consumed;        // A did something special this press: no plain click
static int stow;              // after a scripted drop: if an item is left on the cursor, it goes to the backpack's free spot

static int pick_initial(Focus *f, int n, int modal) { // a screen just appeared: where does the focus start?
    RECT *r = modal == 1 ? NULL : remembered(screen_sig(f, n)); // pop-ups look alike: always start on Cancel
    int best = 0;
    float bs = 1e9f;
    for (int i = 0; i < n; i++) {
        if (r && EqualRect(r, &f[i].r)) return i;
        POINT q = centre(f[i].r);
        float sc = f[i].tag == TG_PREF ? -1e9f                                  // e.g. the game menu's Continue
                 : modal == 1 ? -q.x                                            // pop-ups: the rightmost (Cancel)
                 : (wins & 1u << 10) && EqualRect(&f[i].r, &diary_tabs[diary_tab % 4]) ? -1e8f // the quest log's open tab
                 : q.y * 4.f + q.x + (f[i].kind == F_ITEM || f[i].kind == F_ART ? 0 : 1e6f); // the first item / art, else top-left
        if (sc < bs) bs = sc, best = i;
    }
    return best;
}

static void update_menu(void) {
    static Focus f[MAXF], last[MAXF];
    static int nlast, repeat, was_modal;
    settle_page();
    if (stow && !--stow && carrying && free_spot.x >= 0) queue_click(free_spot.x, free_spot.y);
    int n = focus_list(f), modal = fmodal[fpub];

    // Keep the focus if it is still on screen. Otherwise: a new screen (nothing in common with the last frame's
    // list) starts at its remembered / preferred entry; on the same screen, the nearest entry takes over.
    int cur = -1, overlap = 0;
    for (int i = 0; i < n; i++) {
        if (EqualRect(&f[i].r, &focus)) cur = i;
        for (int k = 0; k < nlast && !overlap; k++) overlap = EqualRect(&f[i].r, &last[k].r);
    }
    static int was_page = -1;
    int page = page_id(); // another page / tab (LB, RB, A on a tab): start on its content, not where the focus was
    if (modal != was_modal || (!overlap && nlast) || (page != was_page && page >= 0 && was_page >= 0)) SetRectEmpty(&focus), cur = -1;
    was_modal = modal, was_page = page;
    static int fresh; // a screen just appeared: its parts draw over a few frames, so the starting focus is re-picked
    if (IsRectEmpty(&focus)) fresh = 12;
    else if (fresh && n && --fresh) cur = -1, SetRectEmpty(&focus);
    if (cur < 0 && n) {
        if (IsRectEmpty(&focus)) cur = pick_initial(f, n, modal);
        else {
            POINT c = centre(focus);
            float bd = 1e9f;
            cur = 0;
            for (int i = 0; i < n; i++) {
                POINT q = centre(f[i].r);
                float d = hypotf(q.x - c.x, q.y - c.y);
                if (d < bd) bd = d, cur = i;
            }
        }
        focus = f[cur].r;
        if (!free_cursor) move_cursor(centre(focus).x, centre(focus).y);
    }
    if (!n) SetRectEmpty(&focus);
    memcpy(last, f, n * sizeof *f), nlast = n;
    if (want_until) // a follow-up entry (Start Game, Load / Save) appeared: go there
        for (int i = 0; i < n; i++)
            if (PtInRect(&f[i].r, want) || past(want_until)) {
                if (!past(want_until)) focus = f[cur = i].r, move_cursor(centre(focus).x, centre(focus).y), fresh = 0;
                want_until = 0;
                break;
            }
    if (cur >= 0) remember(screen_sig(f, n), focus);
    int kind = cur >= 0 ? f[cur].kind : -1, tag = cur >= 0 ? f[cur].tag : 0;
    focus_kind = kind, focus_tag = tag;

    // D-pad or left stick: move the focus (auto-repeat while held). On a slider, its direction changes the value;
    // past the end of a scrolled list (the saves), up/down scroll it.
    int dx = 0, dy = 0;
    if (DOWN(XINPUT_GAMEPAD_DPAD_LEFT) || pad.lx < -0.5f) dx = -1;
    if (DOWN(XINPUT_GAMEPAD_DPAD_RIGHT) || pad.lx > 0.5f) dx = 1;
    if (DOWN(XINPUT_GAMEPAD_DPAD_UP) || pad.ly > 0.5f) dy = -1;
    if (DOWN(XINPUT_GAMEPAD_DPAD_DOWN) || pad.ly < -0.5f) dy = 1;
    if (dy) dx = 0; // one axis at a time: vertical wins
    if (!dx && !dy) nav_held = 0;
    else if (n && (!nav_held || --repeat <= 0)) {
        int k = cur, d = dx + dy;
        if (kind != F_SLIDER || !(tall(f[cur].r) ? dy : dx)) k = step_focus(f, n, dx, dy);
        if (k < 0 && dy) // nothing further: a scroll bar beside the list?
            for (int i = 0; i < n; i++) if (f[i].kind == F_SLIDER && tall(f[i].r)) k = i;
        if (k >= 0 && f[k].kind == F_SLIDER && (tall(f[k].r) ? dy : dx) && (k == cur || !dx))
            queue_click(d < 0 ? f[k].a.x : f[k].b.x, d < 0 ? f[k].a.y : f[k].b.y);
        else {
            if (k >= 0) focus = f[k].r;
            free_cursor = 0;
            move_cursor(centre(focus).x, centre(focus).y);
        }
        repeat = nav_held ? 12 : 30, nav_held = 1, fresh = 0; // 240 ms before repeating, then every 96 ms
    }
    if (!free_cursor && n && cur >= 0) { // follow a focusable that moved (windows sliding in)
        POINT c = centre(focus);
        if (c.x != vcur.x || c.y != vcur.y) move_cursor(c.x, c.y);
    }

    // Right stick: free cursor. Where nothing is focusable (the map), the left stick drives it too.
    float cx = pad.rx + (n ? 0 : pad.lx), cy = pad.ry + (n ? 0 : pad.ly);
    if (cx || cy) {
        static float fx, fy;
        if (!free_cursor) fx = vcur.x, fy = vcur.y;
        free_cursor = 1;
        float m = fminf(hypotf(cx, cy), 1), v = (1.5f + 10 * m) * cfg.cursor_speed / 12.f;
        fx += cx * v, fy -= cy * v;
        move_cursor(lroundf(fx), lroundf(fy));
    }
    cursor_hidden = n && !free_cursor && !carrying;
    if (ctl_focus) {
        ctl_focus = 0;
        LOG("focus: %d items, focus %ld,%ld-%ld,%ld hidden=%d free=%d carrying=%d", n, focus.left, focus.top, focus.right, focus.bottom, cursor_hidden, free_cursor, carrying);
        for (int i = 0; i < n; i++) LOG("focus  %d kind=%d %ld,%ld-%ld,%ld tag=%d a=%ld,%ld b=%ld,%ld", i, f[i].kind, f[i].r.left, f[i].r.top, f[i].r.right, f[i].r.bottom, f[i].tag, f[i].a.x, f[i].a.y, f[i].b.x, f[i].b.y);
    }

    // Choosing a hotbar button for an art (X on an art) or a weapon (Y on an item).
    if (assigning) {
        static const WORD slot_btn[] = {X, Y, B, RB, BTN_RT};
        for (int i = 0; i < 5; i++)
            if (PRESSED(slot_btn[i]) && i < art_slots()) {
                POINT a = centre(focus), s = assign_kind == 2 ? weapon_slot(i) : art_slot(i);
                queue_click(a.x, a.y), queue_click(s.x, s.y), origin = a, stow = 8; // pick it up, drop it on the slot
                assigning = 0;
            }
        if (PRESSED(A) || PRESSED(XINPUT_GAMEPAD_START) || PRESSED(XINPUT_GAMEPAD_BACK) || PRESSED(LB)) assigning = 0;
        a_consumed = DOWN(A);
        return;
    }
    if ((PRESSED(X) || PRESSED(A)) && kind == F_ART) { assigning = 1, assign_kind = 1, a_consumed = DOWN(A); return; }
    if (PRESSED(Y) && tag == TG_WEAPON && (wins & 1u << 1) && !(wins & 1u << 4)) { assigning = 1, assign_kind = 2; return; }

    // A: click. Some entries do more: Quit asks first (the game's own question), a campaign picks the hero scene,
    // a hero moves the focus to Start Game, a save moves it to Load (main menu) or Save (in the game).
    if (PRESSED(A) && !carrying) origin = centre(focus);
    if (PRESSED(A) && tag == TG_QUIT) tap(VK_ESCAPE), a_consumed = 1;
    if (PRESSED(A) && (tag == TG_ANCARIA || tag == TG_UNDERWORLD)) campaign_uw = tag == TG_UNDERWORLD;
    if (PRESSED(A) && tag == TG_PEDESTAL) want = (POINT){608, 716}, want_until = GetTickCount() + 1500;
    if (PRESSED(A) && tag == TG_EDIT && (wins & 1u << 11))
        want = (POINT){866, hero() ? 368 : 304}, want_until = GetTickCount() + 1500;
    if (!DOWN(A)) a_consumed = 0;
    key(VK_LBUTTON, DOWN(A) && !a_consumed);
    key(VK_RBUTTON, DOWN(X) && kind != F_ART); // use / equip

    // B, Start, Back: put a carried item back first; never close a panel with something on the cursor (a click in
    // the world would drop it on the ground).
    int back = PRESSED(B), start = PRESSED(XINPUT_GAMEPAD_START), view = PRESSED(XINPUT_GAMEPAD_BACK);
    if ((back || start || view) && carrying) { // (where it came from, else the free spot)
        if (origin.x || origin.y) queue_click(origin.x, origin.y), stow = 8, want = origin, want_until = GetTickCount() + 1500;
        else if (free_spot.x >= 0) queue_click(free_spot.x, free_spot.y);
        return;
    }
    if (back && modal == 1 && n) { // pop-ups: B is their last button (Cancel, or the only one)
        int k = 0;
        for (int i = 1; i < n; i++) if (centre(f[i].r).x > centre(f[k].r).x) k = i;
        queue_click(centre(f[k].r).x, centre(f[k].r).y);
    } else if (back && npc_ok.x >= 0) queue_click(npc_ok.x, npc_ok.y); // Esc doesn't close NPC text: its OK does
    else if (back || start) tap(VK_ESCAPE);
    if (view && hero()) (wins & 1u << 5) ? tap(VK_ESCAPE) : (queue_key(VK_ESCAPE), queue_key('M')); // map

    // LB/RB: a merchant's tabs while trading; otherwise the panel cycle; otherwise the tabs under the focus.
    if ((PRESSED(LB) || PRESSED(RB)) && !carrying && !modal) {
        int dir = PRESSED(RB) ? 1 : -1, pg = cur_page();
        if (!(wins & 1u << 4) && pg >= 0) goto_page((pg + dir + PG_N) % PG_N);
        else if (pub_nsheets) {
            POINT c = centre(focus);
            int k = 0;
            for (int i = 0; i < pub_nsheets; i++)
                if ((wins & 1u << 4) ? pub_sheets[i].top < 100 : PtInRect(&pub_sheets[i], c)) k = i;
            int t = (pub_sheet_tab[k] + dir + 3) % 3;
            free_cursor = 0;
            queue_click(pub_sheets[k].left + (int[]){73, 229, 391}[t], pub_sheets[k].top); // header centre
        }
    }
}

static int woke; // the pad just took over from the mouse

static void update(void) {
    int lt = DOWN(BTN_LT);
    static int was_menu;
    int menu = !hero() || ui_open;
    static WORD swallow; // buttons held across a world/menu switch do nothing until released
    if (menu != was_menu) { // hand over cleanly between world and menu controls
        key(VK_LBUTTON, 0), key(VK_RBUTTON, 0), key(VK_MENU, 0), key(VK_CONTROL, 0), key(VK_TAB, 0);
        walking = acting = 0, walk_only = 0, force = -1, holding = 0;
        swallow = pad.btn & prev.btn;
    }
    if (woke && menu) { // the first press only shows the focus, next to where the mouse left the cursor
        swallow = pad.btn, nav_held = 1, free_cursor = 0;
        focus = (RECT){vcur.x, vcur.y, vcur.x + 1, vcur.y + 1};
    }
    woke = 0;
    swallow &= pad.btn;
    pad.btn &= ~swallow;
    if (run_steps()) return; // a scripted step owns the cursor until it is done (closing a panel on the way included)
    if (menu) {
        cursor_mode = 1, cursor_idle = 0;
        if (!was_menu) SetRectEmpty(&focus), free_cursor = ctl_cx >= 0, assigning = 0; // (a test-placed cursor stays free)
        update_menu(), was_menu = 1;
        return;
    }
    was_menu = 0;
    cursor_hidden = !cursor_mode; // the world has no cursor on the pad; the right stick's manual aim shows it

    float m = hypotf(pad.lx, pad.ly);
    steering = m > 0.2f;
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
    key(VK_CONTROL, stand && holding); // stand still: Ctrl makes the game attack in place instead of walking up
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

    if (PRESSED(XINPUT_GAMEPAD_START)) tap(lt ? VK_F9 : 'I');  // the panels (LB / RB page through them); quick save
    if (PRESSED(XINPUT_GAMEPAD_BACK) && !lt) tap('M');    // map
    key(VK_TAB, lt && DOWN(XINPUT_GAMEPAD_BACK));          // minimap, while held
    key(VK_MENU, DOWN(LB)); // show names
    if (PRESSED(L3)) stand = !stand;
}

static void tick(void) {
    prev = pad;
    read_pad();
    if (pad.lt > 0.5f) pad.btn |= BTN_LT;
    if (pad.rt > 0.5f) pad.btn |= BTN_RT;
    pad_shown = pad.btn;
    if (!pad_mode && (pad.btn || pad.lx || pad.ly || pad.rx || pad.ry || pad.lt || pad.rt)) {
        pad_mode = 1, woke = 1;
        POINT p;
        if (real_GetCursorPos(&p) && ScreenToClient(game, &p)) vcur = p;
    }
    if (ctl_cx >= 0) pad_mode = 1, cursor_mode = 1, cursor_idle = 0, free_cursor = 1, move_cursor(ctl_cx, ctl_cy);
    static int last_open = -1;
    if (ui_open != last_open) LOG("ui open=%d focusables=%d", ui_open, fcount[fpub]), last_open = ui_open;
    if (!pad_mode) { cursor_hidden = 0; return; }
    update();
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
    if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) {
        if (w & MK_OURS) w &= ~MK_OURS;
        else if (m == WM_MOUSEMOVE && pad_mode && !*ctl_path) { // the real mouse: hand the cursor back once it moves
            static LPARAM last;
            if (last && l != last) pad_mode = 0, LOG("mouse moved: cursor back to the mouse");
            last = l;
        }
    }
    return CallWindowProcA(game_proc, h, m, w, l);
}

static LONG (WINAPI *real_SetWindowLongA)(HWND, int, LONG);
static LONG WINAPI h_SetWindowLongA(HWND h, int i, LONG v) {
    if (h == game && i == GWL_WNDPROC && game_proc) { // menus <-> world: the game's procedure goes behind ours
        LONG old = (LONG)game_proc;
        return game_proc = (WNDPROC)v, old;
    }
    return real_SetWindowLongA(h, i, v);
}

static HWND WINAPI h_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h,
                                     HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    HWND r = real_CreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    LOG("CreateWindowExA cls=%s name=%s style=%08lx -> %p", HIWORD(cls) ? cls : "#", name ? name : "", style, (void *)r);
    if (r && !parent && !game) {
        game = r;
        patch_engine();
        game_proc = (WNDPROC)SetWindowLongA(r, GWL_WNDPROC, (LONG)game_wndproc);
        real_SetWindowLongA = (void *)GetProcAddress(GetModuleHandleA("user32.dll"), "SetWindowLongA");
        patch_ptr((void *)0xa23fb4, h_SetWindowLongA); // the game's delay-loaded SetWindowLongA
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
    for (void **p = (void **)(base + d->VirtualAddress); p < (void **)(base + d->VirtualAddress + d->Size); p++)
        if (*p == real) patch_ptr(p, fn), n++;
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
    HMODULE m = NULL;
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
    LOG("  stack:%s  pad=%04x ui=%d", calls, pad.btn, ui_open);
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
    DWORD envn = GetEnvironmentVariableA("SACREDPAD_CTL", env, sizeof env);
    if (envn && envn < sizeof env) // (too long: env is left unterminated)
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
