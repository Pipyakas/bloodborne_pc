/* Native menu additions: what the PC release of a FromSoftware game has, done inside the
 * game's own menus the way its code builds the PS4 ones (same Scaleform movies, text tables
 * and callbacks), not drawn over them.
 *
 * Title screen (v1.09). Two command lists, each built into a step of the title dialog:
 *   - first screen (guest 0x1b39030, called by FrpgMenuDlgTitle's constructor): Play Online,
 *     Play Offline;
 *   - main menu (0x1b4a3d0, the invoke of the callback the Offline step schedules):
 *     Continue (only with a save), Load Game, New Game, System.
 * Each builder adds rows to a list, then turns the list into the on-screen dialog
 * (0x1bea820). Both of those calls are redirected here, to add a "Quit Game" row last.
 *
 * Launch shortcuts (bbport.ini "launch", --launch): a row's callback returns the step that
 * selecting the row runs. At the first build of each list, the step of the row on the way
 * to the destination replaces the list's own step, as if the row had been selected:
 * Play Offline, then Continue / Load Game / New Game / System.
 *
 * Options screen (title System and in-game Options; builder 0x1bb3ad0): a "Graphics" row
 * after Brightness opens a sub-screen the way Environment does (the same sprite, the
 * game's on/off, choice and slider rows) whose rows edit the port's settings
 * (bbgpu_native_settings). Its texts are the game's text objects pointing at our own
 * strings, so no game file changes.
 *
 * A row is two text objects (label and one-line help, 0x40 bytes each, built from the
 * message repository by category and id) and a callback: the game's std::function-like
 * object, a 0x20-byte inline buffer {vtable, capture, ...} followed by a pointer to the
 * active object. The add-row function copies the texts and clones the callback, so the
 * caller keeps (and destroys) its own. Steps are reference counted (count at +8). */
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Guest code and data (eboot v1.09 image offsets). */
#define ADD_ROW 0x1b4c9a0             /* (list, texts, callback, flags) -> list */
#define MAKE_TEXT 0x1ae8cc0           /* (text, category, id) -> text */
#define TO_DIALOG 0x1bea820           /* (dialog out, list, sprite) -> dialog out */
#define FIRST_MENU 0x1b39030          /* (step out, title dialog) -> step out */
#define FIRST_MENU_CALL 0x1b3871c     /* in the title dialog's constructor */
#define FIRST_MENU_DIALOG 0x1b392fe   /* to_dialog in the first screen's builder */
#define MAIN_MENU 0x1b4a3d0           /* invoke (step out, callback) of the vtable below */
#define MAIN_MENU_TABLE 0x533bf20
#define MAIN_MENU_DIALOG 0x1b4adb3    /* to_dialog in the main menu's builder */
static const uint64_t main_menu_rows[] = {0x1b4a502, 0x1b4a604, 0x1b4a669, 0x1b4a81e, 0x1b4a920};
#define MAIN_MENU_LOG_IN 0x1b4b1f4    /* add_row for PSN "Log In": title.gfx has five rows */
/* Options screen. */
#define OPTION_ADD_ROW 0x1b4e2a0        /* (list, texts, callback, flags*) -> list */
#define OPTION_BRIGHTNESS_ADD 0x1bb410e /* add_row for Brightness */
#define OPTION_NETWORK_LABEL 0x1bb41e5  /* make_text for the Network row's label */
#define OPTION_ROW_TABLE 0x533d120      /* callback wrapping a function (step out, arg) */
#define OPTION_ROW_INVOKE 0x1b6ee10
#define OPEN_SCREEN 0x1bb4c70           /* (step out, arg, screen factory) */
#define SCREEN_FACTORY_TABLE 0x5343880  /* callback wrapping a function (a, b) */
#define SCREEN_FACTORY_INVOKE 0x1bdb270
#define NEW_SCREEN 0x1b20900            /* (a, b, sprite, content member fn, adjust, flag) */
#define ON_OFF_CHOICES 0x1b2b3b0        /* (two-choice list) */
#define ADD_TOGGLE 0x1b2a100            /* (screen, texts, u8 *value, list2, u8 *default) */
#define ADD_CHOICE 0x1b29370            /* (screen, texts, u8 *value, list32, u8 *default) */
#define ADD_SLIDER 0x1b2ac00            /* (screen, texts, u8 *value 0..10, u8 *default) */
#define MSG_MENU 200                    /* SP_メニューテキスト */
/* Movie texts: a walker passes each named text field to a localiser (StaticText_<id> gets
 * message <id>), which sets it through the movie: (*movie)[0x138](movie, path, text, 1). */
#define LOCALIZE_FIELD 0x1f58c30        /* (instance name, field) */
#define LOCALIZE_FIELD_LEA 0x1f58a9d    /* lea rax, [localize_field] in the walker */
#define ENV_CONTENT 0x1b21840           /* Environment's content builder */
#define ENV_CONTENT_LEA 0x1bdb987       /* lea rcx, [env_content] in its screen factory */
/* The Environment sprite's texts the Graphics screen replaces: heading, two section labels. */
#define TEXT_ENV_HEADING 111020
#define TEXT_ENV_DISPLAY 111010
#define TEXT_ENV_SOUND 111011
/* Row callbacks (their vtables). */
#define ROW_OFFLINE 0x533be30
#define ROW_CONTINUE 0x533bcf0
#define ROW_LOAD 0x533bca0
#define ROW_NEW_GAME 0x533bc50
#define ROW_NEW_GAME_FIRST 0x533bd40  /* New Game without any save */
#define ROW_SYSTEM 0x533bc00
/* Message categories (menu.msgbnd ids) and ids. */
#define MSG_INGAME_MENU 70   /* インゲームメニュー */
#define MSG_HELP 201         /* SP_一行ヘルプ: the title rows have no help line */
#define TEXT_QUIT_GAME 115002
#define TEXT_PLAY_OFFLINE 401010
/* bbgpu_launch_destination (BbSettings::Launch). */
enum { LAUNCH_TITLE, LAUNCH_OFFLINE, LAUNCH_CONTINUE, LAUNCH_LOAD, LAUNCH_NEW_GAME, LAUNCH_SYSTEM };

typedef void *(ABI *AddRow)(void *list, void *texts, void *callback, uint64_t flags);
typedef void *(ABI *MakeText)(void *text, int32_t category, int32_t id);
typedef void *(ABI *ToDialog)(void *out, void *list, void *sprite);
typedef void *(ABI *StepBuilder)(void **out, void *owner);

static unsigned char *guest;
static int launch, first_menu_done, main_menu_done;

/* ---- callbacks ---- */
typedef struct Callback Callback;
struct Callback {
    const void *const *table;
    void *capture;
    uint64_t unused[2];
    Callback *active; /* the guest's: points at the inline object or a heap copy */
};

/* Runs a row callback: the step that selecting the row would run (owned by the caller). */
static void *row_step(const void *table, void *capture) {
    Callback row = {.table = table, .capture = capture};
    row.active = &row;
    void *step = NULL;
    ((StepBuilder)row.table[2])(&step, &row);
    return step;
}

static void step_release(void *step) {
    if (!step) return;
    if (__atomic_fetch_sub((int32_t *)((char *)step + 8), 1, __ATOMIC_ACQ_REL) == 1) {
        typedef void (ABI *Destroy)(void *);
        ((Destroy)(*(void ***)step)[0])(step);
    }
}

/* Replaces a list's step with the step of one of its rows. */
static void select_row(void **step, const void *table, void *capture, const char *what) {
    void *selected = row_step(table, capture);
    if (!selected) {
        printf("Runtime: launch shortcut: %s returned no step; the menu stays\n", what);
        return;
    }
    step_release(*step);
    *step = selected;
    printf("Runtime: launch shortcut: %s\n", what);
}

/* The Quit Game row's callback. */
static ABI Callback *quit_clone(const Callback *self, Callback *into) {
    if (!into && !(into = calloc(1, sizeof(*into)))) return NULL;
    into->table = self->table;
    into->capture = self->capture;
    return into;
}
static ABI void quit_invoke(void **next_step, const Callback *self) {
    (void)self;
    *next_step = NULL;
    /* As closing the window does (bbgpu.cpp): no destructors, GPU and guest threads run. */
    puts("Runtime: Quit Game selected on the title screen");
    fflush(NULL);
    _Exit(0);
}
static ABI void *quit_type(void) { return NULL; }
static ABI void quit_destroy(Callback *self, int deallocate) {
    if ((deallocate & 1) && self) free(self);
}
static ABI void quit_destructor(Callback *self) { (void)self; }
static ABI void quit_deallocate(Callback *self) { free(self); }
static ABI void *quit_target(Callback *self) { return &self->capture; }
/* Slots: clone(into), clone(into), invoke, type, destroy(bool), destructor, deallocate, target. */
static const void *const quit_table[8] = {
    (const void *)quit_clone, (const void *)quit_clone, (const void *)quit_invoke,
    (const void *)quit_type, (const void *)quit_destroy, (const void *)quit_destructor,
    (const void *)quit_deallocate, (const void *)quit_target,
};

/* ---- texts ---- */
typedef struct { _Alignas(8) unsigned char bytes[0x40]; } GuestText;
/* The text's own string: SSO up to 7 UTF-16 units, else freed through its allocator. */
static void text_release(GuestText *text) {
    uint64_t capacity, buffer;
    void **allocator;
    memcpy(&capacity, text->bytes + 0x28, 8);
    memcpy(&buffer, text->bytes + 0x10, 8);
    memcpy(&allocator, text->bytes + 0x30, 8);
    if (capacity >= 8 && allocator) {
        typedef void (ABI *Free)(void *, uint64_t);
        ((Free)((void **)*allocator)[0x70 / 8])(allocator, buffer);
    }
}

static void add_quit_row(void *list) {
    GuestText row[2];
    memset(row, 0, sizeof(row));
    ((MakeText)(guest + MAKE_TEXT))(&row[0], MSG_INGAME_MENU, TEXT_QUIT_GAME);
    ((MakeText)(guest + MAKE_TEXT))(&row[1], MSG_HELP, TEXT_PLAY_OFFLINE);
    Callback quit = {.table = quit_table};
    quit.active = &quit;
    ((AddRow)(guest + ADD_ROW))(list, row, &quit, 0);
    text_release(&row[1]);
    text_release(&row[0]);
}

/* A text object showing our own (static, UTF-16) string. */
static void custom_text(GuestText *text, const uint16_t *string) {
    memset(text, 0, sizeof(*text));
    ((MakeText)(guest + MAKE_TEXT))(text, MSG_MENU, 0);
    memcpy(text->bytes, &string, 8);
}

/* ---- movie texts ---- */
typedef void (ABI *LocalizeField)(const char *name, void *field);
typedef void (ABI *SetMovieText)(void *movie, void *object, const uint16_t *text, int32_t html);
static int menu_log = -1;

/* The Environment sprite's text objects per loaded movie (a movie loads them again each time
 * it is created, so entries are replaced; only a movie found in a live screen is used). */
static struct { void *movie, *object; int32_t id; } env_texts[48];
static unsigned env_text_next;

static ABI void localize_field(const char *name, void *field) {
    ((LocalizeField)(guest + LOCALIZE_FIELD))(name, field);
    if (!name || strncmp(name, "StaticText_", 11)) return;
    const int32_t id = (int32_t)strtol(name + 11, NULL, 10);
    if (id != TEXT_ENV_HEADING && id != TEXT_ENV_DISPLAY && id != TEXT_ENV_SOUND) return;
    void *movie = *(void **)((char *)field + 0x18), *object = *(void **)((char *)field + 0x28);
    unsigned slot = env_text_next;
    for (unsigned i = 0; i < sizeof(env_texts) / sizeof(*env_texts); ++i)
        if (env_texts[i].movie == movie && env_texts[i].id == id) slot = i;
    if (slot == env_text_next) env_text_next = (env_text_next + 1) % (sizeof(env_texts) / sizeof(*env_texts));
    env_texts[slot].movie = movie;
    env_texts[slot].object = object;
    env_texts[slot].id = id;
    if (menu_log < 0) menu_log = getenv("BB_MENU_LOG") != NULL;
    if (menu_log) printf("Runtime: menu text %d of movie %p\n", id, movie);
}

/* The movie a screen draws in: a recorded movie pointer within the screen object (or one
 * level below). NULL when none is found. */
static void *screen_movie(void *screen) {
    void **words = screen;
    for (int depth = 0; depth < 2; ++depth) {
        for (int w = 0; w < 0x100; ++w) {
            void *value = depth ? NULL : words[w];
            if (depth) {
                void **inner = words[w];
                if ((uintptr_t)inner < 0x10000 || ((uintptr_t)inner & 7) ||
                    !runtime_memory_is_mapped((uintptr_t)inner, 0x200)) continue;
                for (int v = 0; v < 0x40 && !value; ++v)
                    for (unsigned i = 0; i < sizeof(env_texts) / sizeof(*env_texts); ++i)
                        if (env_texts[i].movie && inner[v] == env_texts[i].movie) value = inner[v];
            }
            for (unsigned i = 0; value && i < sizeof(env_texts) / sizeof(*env_texts); ++i)
                if (env_texts[i].movie && value == env_texts[i].movie) return value;
        }
    }
    return NULL;
}

/* Sets the Environment sprite's texts of `movie`: a heading of ours without its two section
 * labels (heading != NULL), or the game's own. */
static void set_env_texts(void *movie, const uint16_t *heading) {
    static const uint16_t empty[] = u"";
    for (unsigned i = 0; i < sizeof(env_texts) / sizeof(*env_texts); ++i) {
        if (env_texts[i].movie != movie) continue;
        GuestText text;
        const uint16_t *string = empty;
        if (heading) {
            if (env_texts[i].id == TEXT_ENV_HEADING) string = heading;
        } else {
            memset(&text, 0, sizeof(text));
            ((MakeText)(guest + MAKE_TEXT))(&text, MSG_MENU, env_texts[i].id);
            memcpy(&string, text.bytes, 8);
        }
        ((SetMovieText)((*(void ***)movie)[0x138 / 8]))(movie, env_texts[i].object, string, 1);
        if (!heading) text_release(&text);
    }
}

typedef void (ABI *ContentBuilder)(void *screen, void *settings);
/* Environment's screen: its own texts back (our screens replace them). */
__attribute__((aligned(16))) static ABI void env_content(void *screen, void *settings) {
    void *movie = screen_movie(screen);
    if (movie) set_env_texts(movie, NULL);
    ((ContentBuilder)(guest + ENV_CONTENT))(screen, settings);
}

/* ---- the port's screens (Graphics, Effects) ---- */
static void watch_screen(void *screen);
/* Choice lists as the game keeps them: entries {value; text}, then the count. The on/off
 * list's values are bytes, the choice list's int32 (the row's value is an int32 then). */
typedef struct { int32_t value; uint8_t pad[4]; GuestText text; } Choice;
typedef struct { _Alignas(16) Choice entry[2]; uint64_t slack, count; } Choices2;
typedef struct { _Alignas(16) Choice entry[32]; uint64_t slack, count; } Choices32;
_Static_assert(__builtin_offsetof(Choices2, count) == 0x98, "two-choice list layout");
_Static_assert(__builtin_offsetof(Choices32, count) == 0x908, "choice list layout");
_Static_assert(__builtin_offsetof(Choice, text) == 8, "choice entry layout");

static void choices_release(Choice *entry, uint64_t count) {
    for (uint64_t i = 0; i < count; ++i) text_release(&entry[i].text);
}

typedef void (ABI *AddRowWidget)(void *, void *, void *value, void *, const void *def);
typedef void (ABI *AddSlider)(void *, void *, void *value, const void *def);
typedef void (ABI *NewChoices)(void *);

/* The game's row widgets: one array in a child of the screen object, 0xd00 bytes per row,
 * each holding the pointer to its row's value; 8 bytes before it, a byte that reads 1 while
 * the row's dropdown is open. Found by the value pointers (rows[k].value) of the screen's
 * leading choice rows (the screens put their choices first). */
#define ROW_WIDGET_STRIDE 0xd00
#define ROW_DROPDOWN_OPEN (-8)

static void watch_dropdowns(void *screen, const BbNativeSetting *rows, int count) {
    /* The leading choice rows (the widgets of other kinds have other sizes). */
    int choices = 0;
    while (choices < count && rows[choices].kind == BB_NATIVE_CHOICE) ++choices;
    if (!choices) return;
    void **words = screen;
    for (int w = 0; w < 0x400; ++w) {
        unsigned char *inner = words[w];
        if ((uintptr_t)inner < 0x10000 || ((uintptr_t)inner & 7) ||
            !runtime_memory_is_mapped((uintptr_t)inner, (uint64_t)ROW_WIDGET_STRIDE * (uint64_t)choices))
            continue;
        for (size_t at = 0; at < ROW_WIDGET_STRIDE; at += 8) {
            int all = 1;
            for (int r = 0; r < choices && all; ++r) {
                void *value;
                memcpy(&value, inner + at + (size_t)r * ROW_WIDGET_STRIDE, 8);
                all = value == (void *)rows[r].value;
            }
            if (!all) continue;
            for (int r = 0; r < choices; ++r)
                bbgpu_native_settings_dropdown(&rows[r], (const volatile uint8_t *)(inner + at +
                    (size_t)r * ROW_WIDGET_STRIDE + ROW_DROPDOWN_OPEN));
            return;
        }
    }
    puts("Runtime: settings screen: row widgets not found; choices apply when it closes");
}

/* One game row per port setting of screen `id`, under our heading. */
static void build_screen(int32_t id, void *screen) {
    const BbNativeSetting *rows;
    const int count = bbgpu_native_settings(id, &rows);
    watch_screen(screen);
    void *movie = screen_movie(screen);
    if (movie) set_env_texts(movie, bbgpu_native_screen_text(id, 0));
    else puts("Runtime: settings screen: its movie was not found; the heading stays");
    for (int r = 0; r < count && r < 5; ++r) { /* the sprite has five row slots */
        GuestText texts[2];
        custom_text(&texts[0], rows[r].label);
        custom_text(&texts[1], rows[r].help);
        if (rows[r].kind == BB_NATIVE_TOGGLE) {
            Choices2 list;
            memset(&list, 0, sizeof(list));
            ((NewChoices)(guest + ON_OFF_CHOICES))(&list);
            ((AddRowWidget)(guest + ADD_TOGGLE))(screen, texts, rows[r].value, &list, rows[r].default_value);
            choices_release(list.entry, list.count);
        } else if (rows[r].kind == BB_NATIVE_CHOICE) {
            static Choices32 list; /* 2.3 KB: off the guest stack */
            memset(&list, 0, sizeof(list));
            for (int c = 0; c < rows[r].choice_count && c < 32; ++c) {
                list.entry[c].value = c;
                custom_text(&list.entry[c].text, rows[r].choices[c]);
                list.count = (uint64_t)c + 1;
            }
            ((AddRowWidget)(guest + ADD_CHOICE))(screen, texts, rows[r].value, &list, rows[r].default_value);
            choices_release(list.entry, list.count);
        } else {
            ((AddSlider)(guest + ADD_SLIDER))(screen, texts, rows[r].value, rows[r].default_value);
        }
        text_release(&texts[1]);
        text_release(&texts[0]);
    }
    watch_dropdowns(screen, rows, count < 5 ? count : 5);
}

/* Content builders (called as member functions: 16-byte aligned, so they read as non-virtual). */
__attribute__((aligned(16))) static ABI void graphics_content(void *screen, void *settings) {
    (void)settings;
    build_screen(BB_NATIVE_GRAPHICS, screen);
}
__attribute__((aligned(16))) static ABI void effects_content(void *screen, void *settings) {
    (void)settings;
    build_screen(BB_NATIVE_EFFECTS, screen);
}

/* A screen's choices apply when it closes (its dropdowns write while hovering): the screen
 * object gets a copy of its vtable whose destructors commit first. */
static const void *const *screen_table;   /* the game's */
static const void *screen_copy[2 + 96];   /* offset-to-top, RTTI, then the slots */
static void *screen_open;
typedef void (ABI *Destructor)(void *);

static void screen_closed(void *self) {
    if (self == screen_open) {
        *(const void *const **)self = screen_table;
        screen_open = NULL;
        bbgpu_native_settings_commit();
    }
}
static ABI void screen_destroy0(void *self) { screen_closed(self); ((Destructor)screen_table[0])(self); }
static ABI void screen_destroy1(void *self) { screen_closed(self); ((Destructor)screen_table[1])(self); }

static void watch_screen(void *screen) {
    const void *const *table = *(const void *const **)screen;
    const uintptr_t at = (uintptr_t)table - (uintptr_t)guest;
    if (screen_open || at < 0x5000000 || at > 0x5600000) {
        puts("Runtime: settings screen: not watched; choices apply at its next opening");
        return;
    }
    screen_table = table;
    memcpy(screen_copy, table - 2, sizeof(screen_copy));
    screen_copy[2] = (const void *)screen_destroy0;
    screen_copy[3] = (const void *)screen_destroy1;
    *(const void *const **)screen = screen_copy + 2;
    screen_open = screen;
}

typedef void *(ABI *NewScreen)(void *, void *, const char *, void *, uint64_t, int32_t);
/* The screens: Environment's sprite (five rows; its rows' stacking reversed in our copy of
 * the movie, so a dropdown is drawn over the rows below it), our rows. */
static ABI void *graphics_screen(void *a, void *b) {
    return ((NewScreen)(guest + NEW_SCREEN))(a, b, "EnvironmentSetting", (void *)graphics_content, 0, 0);
}
static ABI void *effects_screen(void *a, void *b) {
    return ((NewScreen)(guest + NEW_SCREEN))(a, b, "EnvironmentSetting", (void *)effects_content, 0, 0);
}

typedef void *(ABI *OpenScreen)(void **, void *, void *);
/* A row selected: open its screen as the Environment row does. */
static void **open_screen(void **step, void *arg, void *factory_function) {
    Callback factory = {.table = (const void *const *)(guest + SCREEN_FACTORY_TABLE),
                        .capture = factory_function};
    factory.active = &factory;
    ((OpenScreen)(guest + OPEN_SCREEN))(step, arg, &factory);
    return step;
}
static ABI void **graphics_row(void **step, void *arg) { return open_screen(step, arg, (void *)graphics_screen); }
static ABI void **effects_row(void **step, void *arg) { return open_screen(step, arg, (void *)effects_screen); }

typedef void *(ABI *OptionAddRow)(void *list, void *texts, void *callback, uint64_t *flags);
static void *options_list;

static ABI void *options_after_brightness(void *list, void *texts, void *callback, uint64_t *flags) {
    options_list = ((OptionAddRow)(guest + OPTION_ADD_ROW))(list, texts, callback, flags);
    return options_list;
}

static void add_option_row(void *list, int32_t screen, void *row_function) {
    GuestText texts[2];
    custom_text(&texts[0], bbgpu_native_screen_text(screen, 0));
    custom_text(&texts[1], bbgpu_native_screen_text(screen, 1));
    Callback row = {.table = (const void *const *)(guest + OPTION_ROW_TABLE), .capture = row_function};
    row.active = &row;
    uint64_t flags = 0;
    ((OptionAddRow)(guest + OPTION_ADD_ROW))(list, texts, &row, &flags);
    text_release(&texts[1]);
    text_release(&texts[0]);
}

/* Before the Network row (after Brightness and its own check): Graphics and Effects. */
static ABI void *options_network_label(void *text, int32_t category, int32_t id) {
    if (options_list) {
        add_option_row(options_list, BB_NATIVE_GRAPHICS, (void *)graphics_row);
        add_option_row(options_list, BB_NATIVE_EFFECTS, (void *)effects_row);
        options_list = NULL;
    }
    return ((MakeText)(guest + MAKE_TEXT))(text, category, id);
}

/* ---- hooks ---- */
static void *quit_row_list; /* the list that has its Quit Game row */

static ABI void *list_to_dialog(void *out, void *list, void *sprite) {
    if (quit_row_list != list) add_quit_row(list);
    quit_row_list = NULL;
    return ((ToDialog)(guest + TO_DIALOG))(out, list, sprite);
}

/* The main menu's fifth row: Quit Game instead of the PSN Log In (no PSN here). */
static ABI void *main_menu_log_in(void *list, void *texts, void *callback, uint64_t flags) {
    (void)texts; (void)callback; (void)flags;
    if (quit_row_list != list) add_quit_row(list);
    quit_row_list = list;
    return list;
}

static ABI void *first_menu(void **step, void *title) {
    ((StepBuilder)(guest + FIRST_MENU))(step, title);
    if (launch != LAUNCH_TITLE && !first_menu_done) {
        first_menu_done = 1;
        select_row(step, guest + ROW_OFFLINE, title, "Play Offline");
    }
    return step;
}

/* The main menu's rows as added: their callbacks' vtables and captures. */
static struct { const void *table; void *capture; } rows[8];
static unsigned row_count;

static ABI void *record_row(void *list, void *texts, void *callback, uint64_t flags) {
    const Callback *row = ((const Callback *)callback)->active;
    if (row && row_count < sizeof(rows) / sizeof(*rows)) {
        rows[row_count].table = row->table;
        rows[row_count].capture = row->capture;
        ++row_count;
    }
    return ((AddRow)(guest + ADD_ROW))(list, texts, callback, flags);
}

static ABI void *main_menu(void **step, void *callback) {
    row_count = 0;
    ((StepBuilder)(guest + MAIN_MENU))(step, callback);
    if (launch < LAUNCH_CONTINUE || main_menu_done) return step;
    main_menu_done = 1;
    /* Load Game, New Game and System run their steps inside the menu's dialog (selected
     * without it, System faults): they stay on the menu until selected in the dialog. */
    if (launch != LAUNCH_CONTINUE) {
        puts("Runtime: launch shortcut: this row needs the menu; the menu stays");
        return step;
    }
    static const struct { int launch; uint64_t table; const char *what; } wanted[] = {
        {LAUNCH_CONTINUE, ROW_CONTINUE, "Continue"},
        {LAUNCH_LOAD, ROW_LOAD, "Load Game"},
        {LAUNCH_NEW_GAME, ROW_NEW_GAME, "New Game"},
        {LAUNCH_NEW_GAME, ROW_NEW_GAME_FIRST, "New Game"},
        {LAUNCH_SYSTEM, ROW_SYSTEM, "System"},
    };
    for (size_t w = 0; w < sizeof(wanted) / sizeof(*wanted); ++w) {
        if (wanted[w].launch != launch) continue;
        for (unsigned r = 0; r < row_count; ++r) {
            if (rows[r].table == guest + wanted[w].table) {
                select_row(step, rows[r].table, rows[r].capture, wanted[w].what);
                return step;
            }
        }
    }
    puts("Runtime: launch shortcut: the row is not on this menu (no save?); the menu stays");
    return step;
}

/* ---- the options movie ---- */
/* EnvironmentSetting (sprite 107 of menu/OptionSetting.gfx) stacks its five rows from the
 * first (depth 64) up to the last (100), so a row's dropdown is drawn under the rows below
 * it, and its sixth row (the game's Defaults button, 109) over them all; LanguageSetting, the
 * screen made for dropdowns, stacks them the other way. Our copy of the movie reverses the six
 * rows' depths (64<->109, 73<->100, 82<->91) in every PlaceObject/RemoveObject of that sprite:
 * same names, positions and animation, reversed stacking. */
#define OPTION_MOVIE_SPRITE 107
static const uint16_t row_depths[6] = {64, 73, 82, 91, 100, 109};

static uint16_t swapped_depth(uint16_t depth) {
    for (int i = 0; i < 6; ++i) if (depth == row_depths[i]) return row_depths[5 - i];
    return depth;
}

/* Walks the tags of [p, end); returns the number of depths swapped, -1 when malformed. */
static int swap_row_depths(unsigned char *data, size_t p, size_t end, int *named) {
    int swapped = 0;
    while (p + 2 <= end) {
        uint16_t header;
        memcpy(&header, data + p, 2);
        p += 2;
        const unsigned code = header >> 6;
        uint32_t length = header & 0x3f;
        if (length == 0x3f) {
            if (p + 4 > end) return -1;
            memcpy(&length, data + p, 4);
            p += 4;
        }
        if (p + length > end) return -1;
        unsigned char *body = data + p;
        size_t at = code == 26 ? 1 : code == 70 ? 2 : code == 28 ? 0 : SIZE_MAX;
        if (at != SIZE_MAX && length >= at + 2) {
            uint16_t depth;
            memcpy(&depth, body + at, 2);
            /* The rows' first placement carries their names. */
            char name[] = "Item_0_0";
            for (int i = 0; i < 6; ++i) {
                name[5] = (char)('0' + i);
                if (depth != row_depths[i]) continue;
                for (uint32_t k = 0; k + sizeof(name) <= length; ++k)
                    if (!memcmp(body + k, name, sizeof(name))) { ++*named; break; }
            }
            const uint16_t to = swapped_depth(depth);
            if (to != depth) {
                memcpy(body + at, &to, 2);
                ++swapped;
            }
        }
        if (code == 0) break;
        p += length;
    }
    return swapped;
}

/* Writes our copy of the options movie to the user folder and mounts it over the game's. */
void runtime_menu_files(const char *user_dir) {
    static const char *const guests[] = {"/app0/dvdroot_ps4/menu/OptionSetting.gfx",
                                         "/app0/dvdroot_ps4/menu/optionsetting.gfx"};
    char source[1024], target[1024];
    if (runtime_file_translate(guests[1], source, sizeof(source))) return;
    FILE *in = fopen(source, "rb");
    if (!in) return;
    unsigned char *data = NULL;
    size_t size = 0;
    if (!fseek(in, 0, SEEK_END)) {
        const long length = ftell(in);
        if (length > 32 && length < (16 << 20) && !fseek(in, 0, SEEK_SET) && (data = malloc((size_t)length)))
            size = fread(data, 1, (size_t)length, in) == (size_t)length ? (size_t)length : 0;
    }
    fclose(in);
    int named = 0, swapped = -1;
    if (size && !memcmp(data, "GFX", 3)) {
        const size_t rect_bits = 5 + 4 * (size_t)(data[8] >> 3);
        size_t p = 8 + (rect_bits + 7) / 8 + 4;
        swapped = 0;
        while (p + 2 <= size) {
            uint16_t header;
            memcpy(&header, data + p, 2);
            const unsigned code = header >> 6;
            uint32_t length = header & 0x3f;
            size_t body = p + 2;
            if (length == 0x3f) { memcpy(&length, data + body, 4); body += 4; }
            if (body + length > size) { swapped = -1; break; }
            uint16_t id = 0;
            if (code == 39 && length >= 4) memcpy(&id, data + body, 2);
            if (code == 39 && id == OPTION_MOVIE_SPRITE) {
                swapped = swap_row_depths(data, body + 4, body + length, &named);
                break;
            }
            if (code == 0) break;
            p = body + length;
        }
    }
    if (swapped <= 0 || named != 6) {
        printf("Runtime: options movie not recognised (%d depths, %d rows): dropdowns stay under rows\n",
               swapped, named);
        free(data);
        return;
    }
    snprintf(target, sizeof(target), "%s/menu", user_dir);
    mkdir(target, 0755);
    snprintf(target, sizeof(target), "%s/menu/OptionSetting.gfx", user_dir);
    FILE *out = fopen(target, "wb");
    const int written = out && fwrite(data, 1, size, out) == size;
    if (out) fclose(out);
    free(data);
    if (!written) {
        printf("Runtime: cannot write %s: dropdowns stay under rows\n", target);
        return;
    }
    for (size_t g = 0; g < sizeof(guests) / sizeof(*guests); ++g) runtime_file_mount(guests[g], target);
    printf("Runtime: options movie: rows restacked for dropdowns (%d depths) -> %s\n", swapped, target);
}

/* ---- installation ---- */
static unsigned char *stub_next;

static int call_targets(uint64_t site, uint64_t target) {
    int32_t rel;
    memcpy(&rel, guest + site + 1, 4);
    return guest[site] == 0xe8 && site + 5 + (int64_t)rel == target;
}

/* lea r64, [rip + rel32] (48 8d /r, mod 00 rm 101) */
static int lea_targets(uint64_t site, uint64_t target) {
    int32_t rel;
    memcpy(&rel, guest + site + 3, 4);
    return guest[site] == 0x48 && guest[site + 1] == 0x8d && (guest[site + 2] & 0xc7) == 0x05 &&
           site + 7 + (int64_t)rel == target;
}

/* call rel32 -> stub: movabs rax, host; jmp rax. */
static int redirect_call(uint64_t site, const void *host) {
    unsigned char *stub = stub_next;
    const uintptr_t address = (uintptr_t)host;
    const int64_t rel = (int64_t)(stub - (guest + site + 5));
    if (rel != (int32_t)rel) return 0;
    stub[0] = 0x48; stub[1] = 0xb8; memcpy(stub + 2, &address, 8);
    stub[10] = 0xff; stub[11] = 0xe0;
    stub_next += 16;
    const int32_t rel32 = (int32_t)rel;
    memcpy(guest + site + 1, &rel32, 4);
    return 1;
}

/* lea reg, [function] -> lea reg, [stub] (the stub jumps to host; stubs are 16-aligned). */
static int redirect_lea(uint64_t site, const void *host) {
    unsigned char *stub = stub_next;
    const uintptr_t address = (uintptr_t)host;
    const int64_t rel = (int64_t)(stub - (guest + site + 7));
    if (rel != (int32_t)rel) return 0;
    stub[0] = 0x48; stub[1] = 0xb8; memcpy(stub + 2, &address, 8);
    stub[10] = 0xff; stub[11] = 0xe0;
    stub_next += 16;
    const int32_t rel32 = (int32_t)rel;
    memcpy(guest + site + 3, &rel32, 4);
    return 1;
}

/* Rewrites the call sites before the guest runs. stubs: one writable page within rel32
 * range of the image (the loader makes it executable afterwards). Returns the hooks set. */
unsigned runtime_menu_install(unsigned char *image, uint64_t image_size, unsigned char *stubs) {
    guest = image;
    stub_next = stubs;
    launch = bbgpu_launch_destination();
    void **main_slot = (void **)(image + MAIN_MENU_TABLE + 2 * 8);
    int known = image_size > MAIN_MENU_TABLE + 0x40 &&
                call_targets(FIRST_MENU_CALL, FIRST_MENU) &&
                call_targets(FIRST_MENU_DIALOG, TO_DIALOG) &&
                call_targets(MAIN_MENU_DIALOG, TO_DIALOG) &&
                *main_slot == (void *)(image + MAIN_MENU);
    for (size_t i = 0; i < sizeof(main_menu_rows) / sizeof(*main_menu_rows); ++i)
        known = known && call_targets(main_menu_rows[i], ADD_ROW);
    known = known && call_targets(MAIN_MENU_LOG_IN, ADD_ROW);
    static const unsigned char network_id[] = {0xba, 0xb3, 0xad, 0x01, 0x00}; /* mov edx, 110003 */
    known = known && call_targets(OPTION_BRIGHTNESS_ADD, OPTION_ADD_ROW) &&
            call_targets(OPTION_NETWORK_LABEL, MAKE_TEXT) &&
            !memcmp(image + OPTION_NETWORK_LABEL - 8, network_id, sizeof(network_id)) &&
            *(void **)(image + OPTION_ROW_TABLE + 16) == (void *)(image + OPTION_ROW_INVOKE) &&
            *(void **)(image + SCREEN_FACTORY_TABLE + 16) == (void *)(image + SCREEN_FACTORY_INVOKE) &&
            !((uintptr_t)graphics_content & 1) && !((uintptr_t)effects_content & 1) && lea_targets(LOCALIZE_FIELD_LEA, LOCALIZE_FIELD) &&
            lea_targets(ENV_CONTENT_LEA, ENV_CONTENT);
    if (!known) {
        puts("Runtime: menus not recognised (not v1.09?): no Quit Game, Graphics or launch shortcut");
        return 0;
    }
    unsigned hooks = (unsigned)(redirect_call(FIRST_MENU_DIALOG, (const void *)list_to_dialog) +
                                redirect_call(MAIN_MENU_DIALOG, (const void *)list_to_dialog) +
                                redirect_call(FIRST_MENU_CALL, (const void *)first_menu));
    for (size_t i = 0; i < sizeof(main_menu_rows) / sizeof(*main_menu_rows); ++i)
        hooks += (unsigned)redirect_call(main_menu_rows[i], (const void *)record_row);
    *main_slot = (void *)main_menu;
    ++hooks;
    hooks += (unsigned)redirect_call(MAIN_MENU_LOG_IN, (const void *)main_menu_log_in);
    hooks += (unsigned)(redirect_lea(LOCALIZE_FIELD_LEA, (const void *)localize_field) +
                        redirect_lea(ENV_CONTENT_LEA, (const void *)env_content));
    hooks += (unsigned)(redirect_call(OPTION_BRIGHTNESS_ADD, (const void *)options_after_brightness) +
                        redirect_call(OPTION_NETWORK_LABEL, (const void *)options_network_label));
    printf("Runtime: title menus: Quit Game rows; options: Graphics, Effects; launch shortcut \"%s\"\n",
           launch == LAUNCH_TITLE ? "title" : launch == LAUNCH_OFFLINE ? "offline" :
           launch == LAUNCH_CONTINUE ? "continue" : launch == LAUNCH_LOAD ? "load" :
           launch == LAUNCH_NEW_GAME ? "new_game" : "system");
    return hooks;
}
