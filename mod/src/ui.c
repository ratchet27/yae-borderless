/*
 * "Display Mode" dropdown on the video options page:
 * Windowed / Fullscreen (exclusive) / Borderless.
 *
 * Layout: ds2ui builds widgets from the ui_windows_ini table that
 * gameres/.../ui/ui_ini.lua defines, and labels from ui_strings.txt. Right
 * after ds2ui runs ui_ini.lua, a Lua addition clones the "Max FPS" dropdown
 * into the "Full Screen" row as "Display Mode" and moves the original
 * checkbox offscreen. Texts are literal in the language of ui_strings.txt
 * (ds2ui shows text that is not a string key as is). Game files stay
 * untouched.
 *
 * Binding: the game still owns the hidden "Full Screen" checkbox and saves it
 * as `fullscreen`. Picking Windowed or a fullscreen mode clicks that checkbox
 * the way the mouse would. Exclusive vs borderless is applied when OK is
 * clicked in the options menu; Cancel leaves it as it was.
 *
 * ds2ui.dll is from 2006 and not updated by the remaster; the CDropDownList
 * vtable and the CItems/CCheckBox layouts used here come from its exports.
 */
#include <windows.h>

#include "borderless.h"

/* ---- strings appended to the game files ---------------------------------- */

#define UI_STRINGS_PATH "gameres\\scripts\\you_are_empty\\ui\\ui_strings.txt"
#define LANG_RUSSIAN_MARKER "\xcd\xce\xc2\xc0\xdf \xc8\xc3\xd0\xc0" /* НОВАЯ ИГРА */

static const struct {
    const char *label, *windowed, *fullscreen, *borderless;
} texts[] = {
    {"Display Mode", "Windowed", "Fullscreen", "Borderless"},
    {"\xd0\xe5\xe6\xe8\xec \xfd\xea\xf0\xe0\xed\xe0",   /* Режим экрана */
     "\xce\xea\xee\xed\xed\xfb\xe9",                      /* Оконный */
     "\xcf\xee\xeb\xed\xfb\xe9 \xfd\xea\xf0\xe0\xed",     /* Полный экран */
     "\xc1\xe5\xe7 \xf0\xe0\xec\xea\xe8"},                /* Без рамки */
};

static const char lua_addition_head[] =
    "\r\n-- yae_borderless: Display Mode dropdown\r\n"
    "do\r\n"
    "  local function find(t, name, class)\r\n"
    "    for i, v in ipairs(t) do\r\n"
    "      if type(v) == \"table\" then\r\n"
    "        if v.name == name and v.class_name == class then return t, i end\r\n"
    "        local p, j = find(v, name, class)\r\n"
    "        if p then return p, j end\r\n"
    "      end\r\n"
    "    end\r\n"
    "  end\r\n"
    "  local function copy(v)\r\n"
    "    if type(v) ~= \"table\" then return v end\r\n"
    "    local r = {}\r\n"
    "    for k, x in pairs(v) do r[k] = copy(x) end\r\n"
    "    return r\r\n"
    "  end\r\n"
    "  local label_page, label_i = find(ui_windows_ini, \"Full Screen\", \"StaticText\")\r\n"
    "  local check_page, check_i = find(ui_windows_ini, \"Full Screen\", \"CheckBox\")\r\n"
    "  local fps_label_page, fps_label_i = find(ui_windows_ini, \"Max FPS\", \"StaticText\")\r\n"
    "  local fps_page, fps_i = find(ui_windows_ini, \"Max FPS\", \"DropDownList\")\r\n"
    "  if label_page and check_page and fps_label_page and fps_page then\r\n"
    "    local label, check = label_page[label_i], check_page[check_i]\r\n"
    "    local fps_label, fps = fps_label_page[fps_label_i], fps_page[fps_i]\r\n"
    "    local mode = copy(fps)\r\n"
    "    mode.name = \"Display Mode\"\r\n"
    "    mode.pos = { fps.pos[1], label.pos[2] + fps.pos[2] - fps_label.pos[2] }\r\n"
    "    for _, child in ipairs(mode) do\r\n"
    "      if type(child) == \"table\" and child.class_name == \"ListBox\" then\r\n"
    "        child.items = {\r\n";

static const char lua_addition_tail[] =
    "        }\r\n"
    "      end\r\n"
    "    end\r\n"
    "    label.text = \"";

static const char lua_addition_end[] =
    "\"\r\n"
    "    check.pos = { -10000, -10000 }\r\n"
    "    table.insert(check_page, mode)\r\n"
    "  end\r\n"
    "end\r\n";

/* ---- ds2ui access ----------------------------------------------------------- */

/* thiscall methods called through __fastcall: ecx = this, edx unused. */
typedef const char *(__fastcall *get_name_fn)(void *, void *);
typedef void *(__fastcall *get_items_fn)(void *, void *);
typedef int(__fastcall *get_checked_idx_fn)(void *, void *);
typedef void(__fastcall *check_fn)(void *, void *, unsigned);
typedef void(__fastcall *event_fn)(void *, void *, const void *);
typedef void(__fastcall *wnd_event_ctor_fn)(void *, void *, int);
typedef void(__fastcall *wnd_event_dtor_fn)(void *, void *);
typedef void *(__fastcall *checkbox_ctor_fn)(void *, void *, void *, const char *);
typedef void(__fastcall *checkbox_dtor_fn)(void *, void *);
typedef int(__fastcall *do_file_fn)(void *, void *, int, const char *, BOOL);
typedef int(__fastcall *do_string_fn)(void *, void *, const char *, BOOL);

#define CHECKBOX_CHECKED_OFFSET 0x152 /* CCheckBox::on_event_click flips it */
#define WND_EVENT_SIZE 0x20           /* CWndEvent::CWndEvent(EEventType) */
#define EVENT_CLICK 0x0b              /* registered as "EVENT_CLICK" in ds2ui */
#define ITEM_WINDOWED 0
#define ITEM_FULLSCREEN 1
#define ITEM_BORDERLESS 2
#define OPTIONS_CLOSED_AFTER_MS 500

static struct {
    get_name_fn get_name;
    get_items_fn get_items;
    get_checked_idx_fn get_checked_idx;
    check_fn check;
    event_fn checkbox_click;
    wnd_event_ctor_fn event_ctor;
    wnd_event_dtor_fn event_dtor;
    void *(__fastcall *get_parent)(void *, void *);
    event_fn draw, list_box_checked; /* original CDropDownList vtable entries */
    event_fn window_click;           /* CWindow::on_event_click, inherited by CButton */
} ui;

static checkbox_ctor_fn real_checkbox_ctor;
static checkbox_dtor_fn real_checkbox_dtor;
static do_file_fn real_do_file;
static do_string_fn do_string;

static BOOL ui_hooked, gameui_hooked;
static BOOL committed_borderless, pending_borderless;
static void *fullscreen_checkbox;
static DWORD last_draw;
static BOOL syncing;

static BOOL is_display_mode(void *dropdown)
{
    const char *name = ui.get_name(dropdown, NULL);
    return name && lstrcmpA(name, "Display Mode") == 0;
}

static int selected_item(void *dropdown)
{
    return ui.get_checked_idx(ui.get_items(dropdown, NULL), NULL);
}

static BOOL checkbox_checked(void)
{
    return ((BYTE *)fullscreen_checkbox)[CHECKBOX_CHECKED_OFFSET] != 0;
}

/* Toggle the hidden checkbox through its click handler, so the game sees
 * exactly what a mouse click produces. */
static void set_checkbox(BOOL checked)
{
    BYTE event[WND_EVENT_SIZE];
    if (!fullscreen_checkbox || checkbox_checked() == checked)
        return;
    ui.event_ctor(event, NULL, EVENT_CLICK);
    ui.checkbox_click(fullscreen_checkbox, NULL, event);
    ui.event_dtor(event, NULL);
}

static void __fastcall hook_draw(void *self, void *unused, const void *event)
{
    if (fullscreen_checkbox && is_display_mode(self)) {
        DWORD now = GetTickCount();
        int wanted;
        /* Options reopened: forget a choice that was cancelled. */
        if (now - last_draw > OPTIONS_CLOSED_AFTER_MS)
            pending_borderless = committed_borderless;
        last_draw = now;

        wanted = !checkbox_checked() ? ITEM_WINDOWED
                 : pending_borderless ? ITEM_BORDERLESS
                                      : ITEM_FULLSCREEN;
        if (selected_item(self) != wanted) {
            syncing = TRUE;
            ui.check(self, NULL, wanted);
            syncing = FALSE;
        }
    }
    ui.draw(self, unused, event);
}

static void __fastcall hook_list_box_checked(void *self, void *unused, const void *event)
{
    ui.list_box_checked(self, unused, event);
    if (syncing || !fullscreen_checkbox || !is_display_mode(self))
        return;
    switch (selected_item(self)) {
    case ITEM_WINDOWED:
        set_checkbox(FALSE);
        break;
    case ITEM_FULLSCREEN:
        pending_borderless = FALSE;
        set_checkbox(TRUE);
        break;
    case ITEM_BORDERLESS:
        pending_borderless = TRUE;
        set_checkbox(TRUE);
        break;
    }
}

static void *__fastcall hook_checkbox_ctor(void *self, void *unused, void *desktop,
                                           const char *name)
{
    void *result = real_checkbox_ctor(self, unused, desktop, name);
    if (name && lstrcmpA(name, "Full Screen") == 0)
        fullscreen_checkbox = self;
    return result;
}

static void __fastcall hook_checkbox_dtor(void *self, void *unused)
{
    if (self == fullscreen_checkbox)
        fullscreen_checkbox = NULL;
    real_checkbox_dtor(self, unused);
}

static BOOL ends_with(const char *path, const char *suffix);

static BOOL has_name(void *window, const char *name)
{
    const char *actual = window ? ui.get_name(window, NULL) : NULL;
    return actual && lstrcmpA(actual, name) == 0;
}

/* OK in the options menu applies the Display Mode choice. The game itself
 * only reacts to its own settings, which a fullscreen/borderless switch does
 * not change. */
static void __fastcall hook_button_click(void *self, void *unused, const void *event)
{
    ui.window_click(self, unused, event);
    if (has_name(self, "Ok") && has_name(ui.get_parent(self, NULL), "options_menu") &&
        pending_borderless != committed_borderless) {
        committed_borderless = pending_borderless;
        on_display_mode_committed(committed_borderless);
    }
}

static const char *layout_addition(void);

/* ds2ui runs ui_ini.lua with LuaState::DoFile; the Display Mode entry is
 * added to its ui_windows_ini table right after, in the same Lua state. */
static int __fastcall hook_do_file(void *state, void *unused, int arg, const char *name,
                                   BOOL flag)
{
    int result = real_do_file(state, unused, arg, name, flag);
    if (result == 0 && name && ends_with(name, "ui_ini.lua")) {
        int added = do_string(state, NULL, layout_addition(), FALSE);
        log_line(added == 0 ? "ui: Display Mode option added"
                            : "ui: adding the Display Mode option failed (%d)", added);
    }
    return result;
}

/* Replace the vtable entry that points at `original`; FALSE if not found. */
static BOOL patch_vtable(void **vtable, void *original, void *hook)
{
    for (int i = 0; i < 128; i++) {
        DWORD old;
        if (vtable[i] != original)
            continue;
        if (!VirtualProtect(&vtable[i], sizeof(void *), PAGE_READWRITE, &old))
            return FALSE;
        vtable[i] = hook;
        VirtualProtect(&vtable[i], sizeof(void *), old, &old);
        return TRUE;
    }
    return FALSE;
}

static BOOL hook_ds2ui(HMODULE ds2ui)
{
    void **vtable, **button_vtable;

#define EXPORT(field, symbol) *(FARPROC *)&ui.field = GetProcAddress(ds2ui, symbol);
    EXPORT(get_name, "?get_name@CWindow@ds2ui@@QBEPBDXZ")
    EXPORT(get_items, "?get_items@CDropDownList@ds2ui@@QAEAAVCItems@2@XZ")
    EXPORT(get_checked_idx, "?get_checked_idx@CItems@ds2ui@@QAEHXZ")
    EXPORT(check, "?check@CDropDownList@ds2ui@@QAEXI@Z")
    EXPORT(checkbox_click, "?on_event_click@CCheckBox@ds2ui@@MAEXPBVCWndEvent@2@@Z")
    EXPORT(event_ctor, "??0CWndEvent@ds2ui@@QAE@W4EEventType@1@@Z")
    EXPORT(event_dtor, "??1CWndEvent@ds2ui@@UAE@XZ")
    EXPORT(get_parent, "?get_parent@CWindow@ds2ui@@QAEPAV12@XZ")
    EXPORT(window_click, "?on_event_click@CWindow@ds2ui@@MAEXPBVCWndEvent@2@@Z")
    EXPORT(draw, "?on_event_draw@CDropDownList@ds2ui@@MAEXPBVCWndEvent@2@@Z")
    EXPORT(list_box_checked,
           "?on_event_list_box_checked@CDropDownList@ds2ui@@MAEXPBVCWndEvent@2@@Z")
#undef EXPORT
    vtable = (void **)GetProcAddress(ds2ui, "??_7CDropDownList@ds2ui@@6B@");
    button_vtable = (void **)GetProcAddress(ds2ui, "??_7CButton@ds2ui@@6B@");

    for (void **p = (void **)&ui; p < (void **)(&ui + 1); p++)
        if (!*p)
            return FALSE;
    if (!vtable || !button_vtable ||
        !patch_vtable(vtable, (void *)ui.draw, (void *)hook_draw) ||
        !patch_vtable(vtable, (void *)ui.list_box_checked, (void *)hook_list_box_checked) ||
        !patch_vtable(button_vtable, (void *)ui.window_click, (void *)hook_button_click))
        return FALSE;
    return TRUE;
}

void ui_hook_modules(void *(*patch)(HMODULE, const char *, const char *, void *))
{
    HMODULE module;

    if (!ui_hooked && (module = GetModuleHandleA("ds2ui.dll"))) {
        ui_hooked = TRUE;
        if (!hook_ds2ui(module)) {
            log_line("ui: ds2ui.dll exports not as expected, display mode option disabled");
            fullscreen_checkbox = NULL;
            return;
        }
        do_string = (do_string_fn)GetProcAddress(GetModuleHandleA("ds2scriptsystem.dll"),
                                                 "?DoString@LuaState@LuaPlus@@QAEHPBD_N@Z");
        if (do_string)
            real_do_file = (do_file_fn)patch(module, "ds2scriptsystem.dll",
                                             "?DoFile@LuaState@LuaPlus@@QAEHHPBD_N@Z",
                                             (void *)hook_do_file);
        log_line("ui: ds2ui.dll hooked (DoFile %s)", real_do_file ? "ok" : "missing");
    }
    if (ui_hooked && !gameui_hooked && ui.draw && (module = GetModuleHandleA("ds2GameUI_YAE.dll"))) {
        gameui_hooked = TRUE;
        real_checkbox_ctor = (checkbox_ctor_fn)patch(
            module, "ds2ui.dll", "??0CCheckBox@ds2ui@@QAE@PAVCDesktop@1@PBD@Z",
            (void *)hook_checkbox_ctor);
        real_checkbox_dtor = (checkbox_dtor_fn)patch(module, "ds2ui.dll",
                                                     "??1CCheckBox@ds2ui@@UAE@XZ",
                                                     (void *)hook_checkbox_dtor);
        /* ds2GameUI_YAE's own window classes reach the inherited click
         * handler through their import. */
        patch(module, "ds2ui.dll", "?on_event_click@CWindow@ds2ui@@MAEXPBVCWndEvent@2@@Z",
              (void *)hook_button_click);
        log_line("ui: ds2GameUI_YAE.dll hooked (checkbox %s)",
                 real_checkbox_ctor && real_checkbox_dtor ? "ok" : "missing");
    }
}

/* ---- file redirection ------------------------------------------------------- */

static BOOL ends_with(const char *path, const char *suffix)
{
    int path_len = lstrlenA(path), suffix_len = lstrlenA(suffix);
    if (path_len < suffix_len)
        return FALSE;
    path += path_len - suffix_len;
    return lstrcmpiA(path, suffix) == 0 &&
           (path_len == suffix_len || path[-1] == '\\' || path[-1] == '/');
}

static BOOL read_whole(const char *path, char **data, DWORD *size)
{
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD read = 0;
    if (file == INVALID_HANDLE_VALUE)
        return FALSE;
    *size = GetFileSize(file, NULL);
    *data = HeapAlloc(GetProcessHeap(), 0, *size + 1);
    if (*data && !ReadFile(file, *data, *size, &read, NULL))
        read = 0;
    CloseHandle(file);
    if (!*data || read != *size)
        return FALSE;
    (*data)[*size] = 0;
    return TRUE;
}

static BOOL contains(const char *data, DWORD size, const char *needle)
{
    DWORD len = lstrlenA(needle);
    for (DWORD i = 0; i + len <= size; i++) {
        DWORD j = 0;
        while (j < len && data[i + j] == needle[j])
            j++;
        if (j == len)
            return TRUE;
    }
    return FALSE;
}

/* The language of the installed UI string table (ds2base.cfg: scripts path). */
static BOOL detect_russian(void)
{
    char path[MAX_PATH], *data;
    DWORD size, len = GetModuleFileNameA(NULL, path, MAX_PATH);
    BOOL result;

    while (len > 0 && path[len - 1] != '\\')
        len--;
    path[len] = 0;
    if (len + sizeof(UI_STRINGS_PATH) > MAX_PATH)
        return FALSE;
    lstrcatA(path, UI_STRINGS_PATH);
    if (!read_whole(path, &data, &size))
        return FALSE;
    result = contains(data, size, LANG_RUSSIAN_MARKER);
    HeapFree(GetProcessHeap(), 0, data);
    return result;
}

static void append_item(char *lua, int index, const char *text)
{
    char line[128];
    wsprintfA(line, "          item_%d = { column_0 = \"%s\" },\r\n", index, text);
    lstrcatA(lua, line);
}

static const char *layout_addition(void)
{
    static char lua[sizeof(lua_addition_head) + sizeof(lua_addition_tail) +
                    sizeof(lua_addition_end) + 512];
    BOOL russian = detect_russian();
    lstrcpyA(lua, lua_addition_head);
    append_item(lua, ITEM_WINDOWED, texts[russian].windowed);
    append_item(lua, ITEM_FULLSCREEN, texts[russian].fullscreen);
    append_item(lua, ITEM_BORDERLESS, texts[russian].borderless);
    lstrcatA(lua, lua_addition_tail);
    lstrcatA(lua, texts[russian].label);
    lstrcatA(lua, lua_addition_end);
    return lua;
}

void ui_set_mode(BOOL borderless)
{
    committed_borderless = pending_borderless = borderless;
}

BOOL ui_mode_borderless(void)
{
    return committed_borderless;
}
