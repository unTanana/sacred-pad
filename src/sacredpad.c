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
        ctl_cx = ctl_cy = -1;
        while (f && fscanf(f, "%31s", t) == 1) {
            if (sscanf(t, "%7[A-Z]=%f", k, &v) == 2) {
                float *ax[] = {&pad.lx, &pad.ly, &pad.rx, &pad.ry, &pad.lt, &pad.rt};
                const char *an[] = {"LX", "LY", "RX", "RY", "LT", "RT"};
                for (int i = 0; i < 6; i++) if (!strcmp(k, an[i])) *ax[i] = v;
                if (!strcmp(k, "CX")) ctl_cx = v;
                if (!strcmp(k, "CY")) ctl_cy = v;
            }
            for (size_t i = 0; i < sizeof btn_names / sizeof *btn_names; i++)
                if (!strcmp(t, btn_names[i].name)) pad.btn |= btn_names[i].bit;
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

// ---------------------------------------------------------------- engine patches (Steam 2.0.2.28 addresses)
// The world picker: which object is under a screen point. Returning 0 makes a left click a plain ground move.
static int walk_only; // set while the left stick walks: no talking to / attacking whatever the cursor passes over
static int (__fastcall *pick)(void *, void *, int, int, unsigned, void *) = (void *)0x626c50;
static int __fastcall h_pick(void *view, void *edx, int x, int y, unsigned a, void *b) {
    return walk_only ? 0 : pick(view, edx, x, y, a, b);
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
    patch_call(0x611f64, h_pick); // hover
    patch_call(0x6174fb, h_pick); // left click
    patch_call(0x618003, h_pick);
}

// ---------------------------------------------------------------- per-frame logic
#define BTN_LT 0x0400 // spare XInput bits: triggers as buttons
#define BTN_RT 0x0800
enum { A = XINPUT_GAMEPAD_A, B = XINPUT_GAMEPAD_B, X = XINPUT_GAMEPAD_X, Y = XINPUT_GAMEPAD_Y,
       LB = XINPUT_GAMEPAD_LEFT_SHOULDER, RB = XINPUT_GAMEPAD_RIGHT_SHOULDER, L3 = XINPUT_GAMEPAD_LEFT_THUMB };

static POINT hero_screen(void) { return (POINT){512, 384}; } // ponytail: the camera keeps the hero's feet at the centre; projection in phase 2

static void tap(int k) { key(k, 1), key(k, 0); }

static void update(void) {
    int lt = DOWN(BTN_LT);
    static int walking, stand;

    // Left stick: hold the left button on the ground ahead of the hero (the game's own click-and-hold walking).
    float m = hypotf(pad.lx, pad.ly);
    if (m > 0.2f) {
        POINT h = hero_screen();
        move_cursor(h.x + lroundf(pad.lx / m * 160), h.y - lroundf(pad.ly / m * 120));
        key(VK_LBUTTON, 1), walking = 1;
    } else if (walking) {
        key(VK_LBUTTON, 0), walking = 0;
    }
    if (pad.rx || pad.ry) move_cursor(vcur.x + lroundf(pad.rx * 14), vcur.y - lroundf(pad.ry * 14));

    // A: left click at the cursor (attack / talk / open / pick up). LB+A: collect everything.
    if (!walking) key(VK_LBUTTON, DOWN(A) && !DOWN(LB));
    if (DOWN(LB) && PRESSED(A)) tap('A');

    // X Y B RB RT: select combat-art slot 6..0 and hold the right button to use it; with LT: weapon slot 1..5.
    static const WORD slot_btn[] = {X, Y, B, RB, BTN_RT};
    int using = 0;
    for (int i = 0; i < 5; i++) {
        if (PRESSED(slot_btn[i])) tap(lt ? '1' + i : "67890"[i]);
        using |= DOWN(slot_btn[i]);
    }
    key(VK_RBUTTON, using && !lt);
    walk_only = walking && !DOWN(A) && !using;

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
    if (ctl_cx >= 0) pad_mode = 1, move_cursor(ctl_cx, ctl_cy);
    if (!pad_mode) return;
    update();
    if (pad.btn != prev.btn) LOG("pad btn=%04x cur=%ld,%ld", pad.btn, vcur.x, vcur.y);
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
