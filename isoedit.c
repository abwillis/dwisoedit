/*
 * isoedit.c - ISO image editor GUI
 *
 * GUI:        Dynamic Windows (box packing, cross platform)
 * ISO engine: bkisofs (ISO9660 + Rock Ridge + Joliet + El Torito)
 *
 * Layout:
 *   +-----------------------------------------------------------+
 *   | Menu: File  Image  Boot  Help                             |
 *   | Toolbar: New Open Save As | Add>> <<Extract | ...         |
 *   +----------------------------+------------------------------+
 *   | Local filesystem           | ISO image                    |
 *   | [path............][Go][Up] | [path.............][Go][Up]  |
 *   | container (multi select)   | container (multi select)     |
 *   +----------------------------+------------------------------+
 *   | status: image name | estimated size | boot record          |
 *   +-----------------------------------------------------------+
 *
 * Long operations (open, add, extract, save) run in a worker thread.
 *
 * Threading model (GTK): every Dynamic Windows call made from a non-GUI
 * thread takes the GDK lock internally (DW_MUTEX_LOCK), which is the
 * supported way to update the GUI from a thread. The worker therefore
 * updates the progress window and finishes the job itself (job_finish()).
 *
 * No DW timer is used. On GTK3, DW timer callbacks run on the GUI thread
 * WITHOUT the GDK lock, so GUI work there is not serialized with GDK
 * event dispatch; doing the job completion from a timer is what left
 * the GDK lock held and froze the result dialog after a save.
 *
 * Nothing that runs on the worker may open a modal dialog
 * (dw_messagebox(), dw_file_browse(), dw_dialog_wait()); results are
 * shown in the pre-created, non-modal notice window instead.
 *
 * IMPORTANT: bkisofs is built with -D_FILE_OFFSET_BITS=64 and VolInfo
 * contains off_t members, so this file MUST be compiled with the same
 * setting (it is forced below as a safety net).
 */

#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "bk.h"
#include "dw.h"

#define APP_NAME        "ISO Editor"
#define APP_VERSION     "1.0"
#define FN_TYPES        (FNTYPE_9660 | FNTYPE_ROCKRIDGE | FNTYPE_JOLIET)
#define WORKER_STACK    (256 * 1024)
#define MSG_MAX         900

/* ------------------------------------------------------------------ */
/* Types                                                              */
/* ------------------------------------------------------------------ */

typedef struct
{
    HWND cont;          /* filesystem style container       */
    HWND pathentry;     /* path entry field                 */
    char **pool;        /* strings handed to the container  */
    int poolcount;
    int poolalloc;
} Pane;

typedef struct
{
    const char *name;
    int isdir;
    unsigned long size;
    unsigned mode;      /* POSIX style mode incl. type bits */
} Entry;

enum { JOB_OPEN, JOB_ADD, JOB_EXTRACT, JOB_SAVE };

typedef struct
{
    int type;
    char **items;           /* source paths (add/extract)       */
    int count;
    char *target;           /* image dir, local dir or filename */
    volatile int current;   /* index being processed            */
    int rc;                 /* last bkisofs return code         */
    int successes;
    int failures;
    char *log;              /* accumulated warnings/errors      */
    size_t loglen;
} Job;

/* ------------------------------------------------------------------ */
/* Globals                                                            */
/* ------------------------------------------------------------------ */

static HWND g_mainwin, g_split, g_status, g_isolabel;
static Pane g_local, g_iso;
static VolInfo *g_vol;              /* heap: contains 2 x 100KB buffers */
static char *g_isofile;             /* NULL == untitled new image      */
static char *g_isopath;             /* current image dir, "/a/b/"      */
static char *g_localpath;           /* current local dir               */
static int g_modified;
static HICN g_foldericon, g_fileicon;
static char *g_context_text;        /* row under a context menu        */

static Job *g_job;                  /* non-NULL while a job is running */
static HWND g_progwin, g_progbar, g_progtext;
static HWND g_notewin, g_notemle;   /* reusable non-modal notice window */
static volatile int g_cancel;
static int g_lastpct;               /* worker only: last % shown        */

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static char *xstrdup(const char *s)
{
    char *r = malloc(strlen(s ? s : "") + 1);
    if(!r)
    {
        fprintf(stderr, APP_NAME ": out of memory\n");
        exit(1);
    }
    strcpy(r, s ? s : "");
    return r;
}

static char *concat3(const char *a, const char *b, const char *c)
{
    size_t la = strlen(a), lb = strlen(b), lc = c ? strlen(c) : 0;
    char *r = malloc(la + lb + lc + 1);
    if(!r)
    {
        fprintf(stderr, APP_NAME ": out of memory\n");
        exit(1);
    }
    memcpy(r, a, la);
    memcpy(r + la, b, lb);
    if(c)
        memcpy(r + la + lb, c, lc);
    r[la + lb + lc] = '\0';
    return r;
}

static void set_string(char **dst, const char *src)
{
    char *old = *dst;
    *dst = src ? xstrdup(src) : NULL;
    free(old);
}

static int ci_compare(const char *a, const char *b)
{
    while(*a && *b)
    {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if(ca != cb)
            return ca - cb;
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

static int has_extension(const char *name, const char *ext)
{
    size_t ln = strlen(name), le = strlen(ext);
    return ln > le && ci_compare(name + ln - le, ext) == 0;
}

static const char *base_name(const char *path)
{
    const char *p = path + strlen(path);
    while(p > path && p[-1] != '/' && p[-1] != '\\')
        p--;
    return p;
}

static void free_list(char **list, int count)
{
    int i;
    if(!list)
        return;
    for(i = 0; i < count; i++)
        free(list[i]);
    free(list);
}

static void mode_string(unsigned mode, char *buf)
{
    static const char rwx[] = "rwxrwxrwx";
    int i;

    if(IS_DIR(mode))
        buf[0] = 'd';
    else if(IS_SYMLINK(mode))
        buf[0] = 'l';
    else
        buf[0] = '-';
    for(i = 0; i < 9; i++)
        buf[i + 1] = (mode & (0400 >> i)) ? rwx[i] : '-';
    buf[10] = '\0';
}

static const char *type_string(unsigned mode)
{
    if(IS_DIR(mode))
        return "Folder";
    if(IS_SYMLINK(mode))
        return "Link";
    if(IS_REG_FILE(mode))
        return "File";
    return "Other";
}

static const char *boot_type_string(void)
{
    switch(g_vol->bootMediaType)
    {
    case BOOT_MEDIA_NO_EMULATION: return "No emulation";
    case BOOT_MEDIA_1_2_FLOPPY:   return "1.2 MB floppy";
    case BOOT_MEDIA_1_44_FLOPPY:  return "1.44 MB floppy";
    case BOOT_MEDIA_2_88_FLOPPY:  return "2.88 MB floppy";
    case BOOT_MEDIA_HARD_DISK:    return "Hard disk";
    default:                      return "None";
    }
}

static void error_box(const char *fmt, ...)
{
    char buf[MSG_MAX + 1];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    dw_messagebox(APP_NAME, DW_MB_OK | DW_MB_ERROR, "%s", buf);
}

static void info_box(const char *fmt, ...)
{
    char buf[MSG_MAX + 1];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    dw_messagebox(APP_NAME, DW_MB_OK | DW_MB_INFORMATION, "%s", buf);
}

static int ask_yes_no(const char *fmt, ...)
{
    char buf[MSG_MAX + 1];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return dw_messagebox(APP_NAME, DW_MB_YESNO | DW_MB_QUESTION, "%s", buf) == DW_MB_RETURN_YES;
}

static void bk_error(const char *what, const char *item, int rc)
{
    error_box("%s%s%s failed:\n%s", what, item ? " " : "", item ? item : "",
              bk_get_error_string(rc));
}

/* ------------------------------------------------------------------ */
/* Path helpers                                                       */
/* ------------------------------------------------------------------ */

/* Image paths: directories always "/" or "/a/b/" (bkisofs requirement). */
static char *iso_child(const char *dir, const char *name)
{
    return concat3(dir, name, NULL);
}

static char *iso_parent(const char *dir)
{
    char *r = xstrdup(dir);
    size_t len = strlen(r);

    if(len <= 1)
        return r;
    r[len - 1] = '\0';                  /* drop trailing '/' */
    *(strrchr(r, '/') + 1) = '\0';      /* keep through previous '/' */
    return r;
}

/* Normalize a user typed image path to "/x/y/" form. */
static char *iso_normalize(const char *in)
{
    size_t len = strlen(in);
    char *r = malloc(len + 3);
    size_t j = 0, i;

    if(!r)
        exit(1);
    r[j++] = '/';
    for(i = 0; i < len; i++)
    {
        char c = (in[i] == '\\') ? '/' : in[i];
        if(c == '/' && r[j - 1] == '/')
            continue;
        r[j++] = c;
    }
    if(r[j - 1] != '/')
        r[j++] = '/';
    r[j] = '\0';
    return r;
}

static int is_sep(char c)
{
    return c == '/' || c == '\\';
}

/* bkisofs splits names on '/' only, so local paths use '/' everywhere. */
static void local_normalize(char *p)
{
    for(; *p; p++)
        if(*p == '\\')
            *p = '/';
}

static int local_is_root(const char *p)
{
    if(strcmp(p, "/") == 0)
        return 1;
    if(isalpha((unsigned char)p[0]) && p[1] == ':' &&
       (p[2] == '\0' || (is_sep(p[2]) && p[3] == '\0')))
        return 1;
    return 0;
}

static char *local_join(const char *dir, const char *name)
{
    size_t len = strlen(dir);
    if(len && is_sep(dir[len - 1]))
        return concat3(dir, name, NULL);
    return concat3(dir, "/", name);
}

static char *local_parent(const char *p)
{
    char *r = xstrdup(p), *sep;
    size_t len = strlen(r);

    if(local_is_root(r))
        return r;
    while(len > 1 && is_sep(r[len - 1]))
        r[--len] = '\0';
    sep = strrchr(r, '/');
    if(!sep)
        return r;
    if(sep == r)
        r[1] = '\0';                    /* "/name"   -> "/"   */
    else if(sep == r + 2 && r[1] == ':')
        r[3] = '\0';                    /* "C:/name" -> "C:/" */
    else
        *sep = '\0';
    return r;
}

static int local_is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int local_exists(const char *p)
{
    struct stat st;
#ifdef S_ISLNK
    return lstat(p, &st) == 0;
#else
    return stat(p, &st) == 0;
#endif
}

/* ------------------------------------------------------------------ */
/* Volume handling                                                    */
/* ------------------------------------------------------------------ */

static bool bk_warning_cb(const char *msg);

static void vol_reset(void)
{
    static int initialized = 0;

    if(initialized)
        bk_destroy_vol_info(g_vol);
    bk_init_vol_info(g_vol, true);      /* memsets the whole struct */
    g_vol->warningCbk = bk_warning_cb;
    initialized = 1;
}

static BkDir *iso_current_dir(void)
{
    BkDir *dir = NULL;

    if(bk_get_dir_from_string(g_vol, g_isopath, &dir) <= 0 || !dir)
    {
        set_string(&g_isopath, "/");
        if(bk_get_dir_from_string(g_vol, g_isopath, &dir) <= 0)
            dir = NULL;
    }
    return dir;
}

static BkFileBase *iso_find(const char *name)
{
    BkDir *dir = iso_current_dir();
    BkFileBase *child;

    for(child = dir ? dir->children : NULL; child; child = child->next)
        if(strcmp(child->name, name) == 0)
            return child;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Container panes                                                    */
/* ------------------------------------------------------------------ */

static char *pool_add(Pane *p, const char *s)
{
    if(p->poolcount == p->poolalloc)
    {
        int n = p->poolalloc ? p->poolalloc * 2 : 64;
        char **np = realloc(p->pool, n * sizeof(char *));
        if(!np)
            exit(1);
        p->pool = np;
        p->poolalloc = n;
    }
    p->pool[p->poolcount] = xstrdup(s);
    return p->pool[p->poolcount++];
}

static void pool_free(Pane *p)
{
    int i;
    for(i = 0; i < p->poolcount; i++)
        free(p->pool[i]);
    p->poolcount = 0;
}

static int entry_compare(const void *a, const void *b)
{
    const Entry *ea = a, *eb = b;
    int r;

    if(ea->isdir != eb->isdir)
        return ea->isdir ? -1 : 1;
    r = ci_compare(ea->name, eb->name);
    return r ? r : strcmp(ea->name, eb->name);
}

static void pane_set_row(Pane *p, void *mem, int row, const char *name,
                         int isdir, unsigned long size, const char *type, const char *mode)
{
    char *pname = pool_add(p, name);
    char *pmode = pool_add(p, mode);
    char *ptype = (char *)type;     /* static strings */
    ULONG usize = (ULONG)size;

    dw_filesystem_set_file(p->cont, mem, row, pname, isdir ? g_foldericon : g_fileicon);
    dw_filesystem_set_item(p->cont, mem, 0, row, &usize);
    dw_filesystem_set_item(p->cont, mem, 1, row, &ptype);
    dw_filesystem_set_item(p->cont, mem, 2, row, &pmode);
    dw_container_set_row_title(mem, row, pname);
}

static void pane_fill(Pane *p, Entry *e, int count, int addparent)
{
    int rows = count + (addparent ? 1 : 0), row = 0, i;
    void *mem;

    dw_container_clear(p->cont, TRUE);
    pool_free(p);
    if(rows == 0)
        return;

    qsort(e, count, sizeof(Entry), entry_compare);
    mem = dw_container_alloc(p->cont, rows);
    if(addparent)
        pane_set_row(p, mem, row++, "..", 1, 0, "Parent", "");
    for(i = 0; i < count; i++)
    {
        char mbuf[11];
        mode_string(e[i].mode, mbuf);
        pane_set_row(p, mem, row++, e[i].name, e[i].isdir, e[i].size,
                     type_string(e[i].mode), mbuf);
    }
    dw_container_insert(p->cont, mem, rows);
    dw_container_optimize(p->cont);
}

/* Returns selected row titles (".." excluded). Falls back to the row
 * under the context menu when invoked from a popup and nothing is selected. */
static char **pane_selection(Pane *p, int fromcontext, int *count)
{
    char **list = NULL, *text;
    int n = 0, alloc = 0;

    *count = 0;
    text = dw_container_query_start(p->cont, DW_CRA_SELECTED);
    while(text)
    {
        if(strcmp(text, "..") != 0)
        {
            if(n == alloc)
            {
                alloc = alloc ? alloc * 2 : 16;
                list = realloc(list, alloc * sizeof(char *));
                if(!list)
                    exit(1);
            }
            list[n++] = xstrdup(text);
        }
        dw_free(text);
        text = dw_container_query_next(p->cont, DW_CRA_SELECTED);
    }
    if(n == 0 && fromcontext && g_context_text && strcmp(g_context_text, "..") != 0)
    {
        list = malloc(sizeof(char *));
        if(!list)
            exit(1);
        list[n++] = xstrdup(g_context_text);
    }
    *count = n;
    return list;
}

/* ------------------------------------------------------------------ */
/* Refresh                                                            */
/* ------------------------------------------------------------------ */

static void update_status(void)
{
    char buf[512];
    double est = (double)bk_estimate_iso_size(g_vol, FN_TYPES) / (1024.0 * 1024.0);
    const char *name = g_isofile ? base_name(g_isofile) : "Untitled";

    snprintf(buf, sizeof(buf), "Image: %s%s   |   Estimated size: %.1f MB   |   Boot record: %s",
             name, g_modified ? " (modified)" : "", est, boot_type_string());
    dw_window_set_text(g_status, buf);

    snprintf(buf, sizeof(buf), "%s%s - " APP_NAME, g_modified ? "*" : "", name);
    dw_window_set_text(g_mainwin, buf);

    snprintf(buf, sizeof(buf), "ISO image: %s", name);
    dw_window_set_text(g_isolabel, buf);
}

static void iso_refresh(void)
{
    BkDir *dir = iso_current_dir();
    BkFileBase *child;
    Entry *e = NULL;
    int n = 0;

    for(child = dir ? dir->children : NULL; child; child = child->next)
        n++;
    if(n)
    {
        e = calloc(n, sizeof(Entry));
        if(!e)
            exit(1);
    }
    n = 0;
    for(child = dir ? dir->children : NULL; child; child = child->next)
    {
        e[n].name = child->name;
        e[n].isdir = IS_DIR(child->posixFileMode);
        e[n].size = IS_REG_FILE(child->posixFileMode) ? BK_FILE_PTR(child)->size : 0;
        e[n].mode = child->posixFileMode;
        n++;
    }
    pane_fill(&g_iso, e, n, strcmp(g_isopath, "/") != 0);
    free(e);
    dw_window_set_text(g_iso.pathentry, g_isopath);
    update_status();
}

static void notice_show(const char *title, const char *fmt, ...);

static void local_refresh(void)
{
    DIR *d;
    struct dirent *de;
    Entry *e = NULL;
    int n = 0, alloc = 0, i;

    d = opendir(g_localpath);
    if(d)
    {
        while((de = readdir(d)) != NULL)
        {
            struct stat st;
            char *full;
            unsigned type = 0100000;

            if(strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            if(n == alloc)
            {
                alloc = alloc ? alloc * 2 : 64;
                e = realloc(e, alloc * sizeof(Entry));
                if(!e)
                    exit(1);
            }
            full = local_join(g_localpath, de->d_name);
            memset(&e[n], 0, sizeof(Entry));
            memset(&st, 0, sizeof(st));
            e[n].name = xstrdup(de->d_name);
            if(stat(full, &st) == 0)
            {
                if(S_ISDIR(st.st_mode))
                    type = 0040000;
                e[n].isdir = S_ISDIR(st.st_mode);
                if(S_ISREG(st.st_mode))
                    e[n].size = (st.st_size > (off_t)ULONG_MAX) ? ULONG_MAX : (unsigned long)st.st_size;
            }
#ifdef S_ISLNK
            if(lstat(full, &st) == 0 && S_ISLNK(st.st_mode))
                type = 0120000;
#endif
            e[n].mode = type | (unsigned)(st.st_mode & 0777);
            free(full);
            n++;
        }
        closedir(d);
    }
    pane_fill(&g_local, e, n, !local_is_root(g_localpath));
    for(i = 0; i < n; i++)
        free((char *)e[i].name);
    free(e);
    dw_window_set_text(g_local.pathentry, g_localpath);
    if(!d)  /* may run on the worker via job_finish(): no modal box */
        notice_show(APP_NAME " - Error", "Unable to read folder:\n%s", g_localpath);
}

static void iso_chdir(const char *newpath)
{
    set_string(&g_isopath, newpath);
    iso_refresh();
}

static void local_chdir(const char *newpath)
{
    set_string(&g_localpath, newpath);
    local_normalize(g_localpath);
    local_refresh();
}

/* ------------------------------------------------------------------ */
/* Generic modal form dialog                                          */
/* ------------------------------------------------------------------ */

typedef struct
{
    DWDialog *dialog;
    int done;
} DlgState;

static int DWSIGNAL dlg_ok_cb(HWND window, void *data)
{
    DlgState *s = data;
    if(!s->done)
    {
        s->done = 1;
        dw_dialog_dismiss(s->dialog, DW_INT_TO_POINTER(1));
    }
    return TRUE;
}

static int DWSIGNAL dlg_cancel_cb(HWND window, void *data)
{
    DlgState *s = data;
    if(!s->done)
    {
        s->done = 1;
        dw_dialog_dismiss(s->dialog, NULL);
    }
    return TRUE;
}

/*
 * Shows a dialog with optional read-only info text and n labeled entry
 * fields. values[] are in/out (malloc'd strings, may start NULL).
 * Returns 1 on OK, 0 on Cancel.
 */
static int form_dialog(const char *title, const char *info, int n,
                       const char **labels, char **values, const int *limits)
{
    HWND win, vbox, hbox, ok, cancel, entries[8];
    DlgState state;
    void *result;
    int i;

    if(n > 8)
        n = 8;
    state.done = 0;
    state.dialog = dw_dialog_new(NULL);

    win = dw_window_new(HWND_DESKTOP, title,
                        DW_FCF_TITLEBAR | DW_FCF_SYSMENU | DW_FCF_DLGBORDER | DW_FCF_SIZEBORDER);
    vbox = dw_box_new(DW_VERT, 6);
    dw_box_pack_start(win, vbox, 0, 0, TRUE, TRUE, 0);

    if(info)
    {
        HWND mle = dw_mle_new(0);
        dw_box_pack_start(vbox, mle, 420, 120, TRUE, TRUE, 2);
        dw_mle_import(mle, info, -1);
        dw_mle_set_editable(mle, FALSE);
    }

    for(i = 0; i < n; i++)
    {
        HWND label;

        hbox = dw_box_new(DW_HORZ, 0);
        dw_box_pack_start(vbox, hbox, 0, 0, TRUE, FALSE, 0);
        label = dw_text_new(labels[i], 0);
        dw_window_set_style(label, DW_DT_VCENTER, DW_DT_VCENTER);
        dw_box_pack_start(hbox, label, 130, DW_SIZE_AUTO, FALSE, TRUE, 2);
        entries[i] = dw_entryfield_new(values[i] ? values[i] : "", 0);
        if(limits && limits[i] > 0)
            dw_entryfield_set_limit(entries[i], limits[i]);
        dw_box_pack_start(hbox, entries[i], 260, DW_SIZE_AUTO, TRUE, FALSE, 2);
    }

    hbox = dw_box_new(DW_HORZ, 0);
    dw_box_pack_start(vbox, hbox, 0, 0, TRUE, FALSE, 0);
    dw_box_pack_start(hbox, 0, 1, 1, TRUE, FALSE, 0);
    ok = dw_button_new("OK", 0);
    dw_box_pack_start(hbox, ok, 90, DW_SIZE_AUTO, FALSE, FALSE, 2);
    cancel = dw_button_new("Cancel", 0);
    dw_box_pack_start(hbox, cancel, 90, DW_SIZE_AUTO, FALSE, FALSE, 2);

    dw_signal_connect(ok, DW_SIGNAL_CLICKED, DW_SIGNAL_FUNC(dlg_ok_cb), &state);
    dw_signal_connect(cancel, DW_SIGNAL_CLICKED, DW_SIGNAL_FUNC(dlg_cancel_cb), &state);
    dw_signal_connect(win, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(dlg_cancel_cb), &state);

    for(i = 0; i < n; i++)
        dw_window_click_default(entries[i], (i + 1 < n) ? entries[i + 1] : ok);
    if(n > 0)
        dw_window_default(win, entries[0]);

    dw_window_set_gravity(win, DW_GRAV_CENTER, DW_GRAV_CENTER);
    dw_window_set_pos_size(win, 0, 0, 460, (info ? 200 : 70) + 36 * n);
    dw_window_show(win);

    result = dw_dialog_wait(state.dialog);

    if(result)
    {
        for(i = 0; i < n; i++)
        {
            char *t = dw_window_get_text(entries[i]);
            free(values[i]);
            values[i] = xstrdup(t ? t : "");
            if(t)
                dw_free(t);
        }
    }
    dw_window_destroy(win);
    return result ? 1 : 0;
}

/* Returns malloc'd text or NULL if cancelled/empty. */
static char *prompt_text(const char *title, const char *label, const char *initial, int limit)
{
    char *value = initial ? xstrdup(initial) : NULL;
    const char *labels[1];
    int limits[1];

    labels[0] = label;
    limits[0] = limit;
    if(!form_dialog(title, NULL, 1, labels, &value, limits) || !value || !*value)
    {
        free(value);
        return NULL;
    }
    return value;
}

/* ------------------------------------------------------------------ */
/* Background jobs                                                    */
/* ------------------------------------------------------------------ */

/* Worker thread only */
static void job_log(const char *fmt, ...)
{
    char line[600];
    size_t len;
    char *nl;
    va_list args;

    if(!g_job)
        return;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    len = strlen(line);
    nl = realloc(g_job->log, g_job->loglen + len + 2);
    if(!nl)
        return;
    g_job->log = nl;
    memcpy(g_job->log + g_job->loglen, line, len);
    g_job->loglen += len;
    g_job->log[g_job->loglen++] = '\n';
    g_job->log[g_job->loglen] = '\0';
}

/* Called by bkisofs for nested failures during add/extract.
 * Record it and keep going unless the user cancelled. */
static bool bk_warning_cb(const char *msg)
{
    job_log("%s", msg);
    return g_cancel ? false : true;
}

static void progress_cb(VolInfo *vol)
{
    if(g_cancel)
        vol->stopOperation = true;
}

/* Worker thread: DW calls lock GDK internally */
static void job_status(int percent, const char *fmt, ...)
{
    char buf[400];
    va_list args;

    if(g_cancel)
        return;     /* keep "Cancelling..." visible */
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    dw_window_set_text(g_progtext, buf);
    if(percent >= 0)
        dw_percent_set_pos(g_progbar, (unsigned int)percent);
}

static void write_progress_cb(VolInfo *vol, double percent)
{
    int pct = (int)percent;

    if(g_cancel)
        vol->stopOperation = true;
    else if(pct != g_lastpct && g_job)
    {
        g_lastpct = pct;
        job_status(pct, "Writing %s (%d%%)", base_name(g_job->target), pct);
    }
}

static void job_finish(void);

static void DWSIGNAL job_thread(void *data)
{
    Job *j = data;
    int i, rc = 1;

    switch(j->type)
    {
    case JOB_OPEN:
        rc = bk_open_image(g_vol, j->target);
        if(rc > 0)
            rc = bk_read_vol_info(g_vol);
        if(rc > 0)
        {
            if(g_vol->filenameTypes & FNTYPE_ROCKRIDGE)
                rc = bk_read_dir_tree(g_vol, FNTYPE_ROCKRIDGE, true, progress_cb);
            else if(g_vol->filenameTypes & FNTYPE_JOLIET)
                rc = bk_read_dir_tree(g_vol, FNTYPE_JOLIET, false, progress_cb);
            else
                rc = bk_read_dir_tree(g_vol, FNTYPE_9660, false, progress_cb);
        }
        break;

    case JOB_ADD:
    case JOB_EXTRACT:
        for(i = 0; i < j->count; i++)
        {
            j->current = i;
            if(g_cancel)
            {
                rc = BKERROR_OPER_CANCELED_BY_USER;
                break;
            }
            job_status(i * 100 / j->count, "%s %d of %d: %s",
                       j->type == JOB_ADD ? "Adding" : "Extracting",
                       i + 1, j->count, base_name(j->items[i]));
            if(j->type == JOB_ADD)
                rc = bk_add(g_vol, j->items[i], j->target, progress_cb);
            else
                rc = bk_extract(g_vol, j->items[i], j->target,
                                (g_vol->filenameTypes & FNTYPE_ROCKRIDGE) ? true : false,
                                progress_cb);
            if(rc <= 0 && rc != BKWARNING_OPER_PARTLY_FAILED)
            {
                j->failures++;
                job_log("%s: %s", base_name(j->items[i]), bk_get_error_string(rc));
                if(rc == BKERROR_OPER_CANCELED_BY_USER)
                    break;
            }
            else
                j->successes++;
        }
        break;

    case JOB_SAVE:
        rc = bk_write_image(j->target, g_vol, time(NULL), FN_TYPES, write_progress_cb);
        break;
    }

    j->rc = rc;
    job_finish();   /* still on the worker thread */
}

static int DWSIGNAL prog_cancel_cb(HWND window, void *data)
{
    if(g_job && !g_cancel)
    {
        g_cancel = 1;
        bk_cancel_operation(g_vol);
        dw_window_set_text(g_progtext, "Cancelling...");
    }
    return TRUE;
}

/*
 * The progress window is created once and reused (shown/hidden), never
 * destroyed while the main loop runs. DW_PERCENT_INDETERMINATE is not
 * used: on GTK it starts a lock-less 100 ms g_timeout holding a raw
 * pointer to the progress bar (the cause of the earlier crash), and that
 * timeout would also race with the worker's locked GUI updates.
 */
static void progress_window_create(void)
{
    HWND vbox, hbox, cancel;

    g_progwin = dw_window_new(HWND_DESKTOP, APP_NAME, DW_FCF_TITLEBAR | DW_FCF_DLGBORDER);
    vbox = dw_box_new(DW_VERT, 8);
    dw_box_pack_start(g_progwin, vbox, 0, 0, TRUE, TRUE, 0);
    g_progtext = dw_text_new("Starting...", 0);
    dw_box_pack_start(vbox, g_progtext, 380, DW_SIZE_AUTO, TRUE, FALSE, 2);
    g_progbar = dw_percent_new(0);
    dw_box_pack_start(vbox, g_progbar, 380, 20, TRUE, FALSE, 2);
    hbox = dw_box_new(DW_HORZ, 0);
    dw_box_pack_start(vbox, hbox, 0, 0, TRUE, FALSE, 0);
    dw_box_pack_start(hbox, 0, 1, 1, TRUE, FALSE, 0);
    cancel = dw_button_new("Cancel", 0);
    dw_box_pack_start(hbox, cancel, 90, DW_SIZE_AUTO, FALSE, FALSE, 2);
    dw_signal_connect(cancel, DW_SIGNAL_CLICKED, DW_SIGNAL_FUNC(prog_cancel_cb), NULL);
    dw_signal_connect(g_progwin, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(prog_cancel_cb), NULL);
    dw_window_set_gravity(g_progwin, DW_GRAV_CENTER, DW_GRAV_CENTER);
    dw_window_set_pos_size(g_progwin, 0, 0, 440, 140);
    /* Created hidden; job_start() shows it, job_finish() hides it */
}

/*
 * Non-modal notice window, created once at startup on the GUI thread
 * (window sizing on GTK/X11 may run a short nested event loop, which is
 * only safe there) and then just filled and shown/hidden. Safe to call
 * from the worker thread and from GUI signal handlers.
 */
static int DWSIGNAL notice_close_cb(HWND window, void *data)
{
    dw_window_hide(g_notewin);
    return TRUE;    /* DELETE: handled, keep the window for reuse */
}

static void notice_create(void)
{
    HWND vbox, hbox, ok;

    g_notewin = dw_window_new(HWND_DESKTOP, APP_NAME,
                              DW_FCF_TITLEBAR | DW_FCF_SYSMENU | DW_FCF_DLGBORDER | DW_FCF_SIZEBORDER);
    vbox = dw_box_new(DW_VERT, 6);
    dw_box_pack_start(g_notewin, vbox, 0, 0, TRUE, TRUE, 0);
    g_notemle = dw_mle_new(0);
    dw_box_pack_start(vbox, g_notemle, 420, 110, TRUE, TRUE, 2);
    dw_mle_set_editable(g_notemle, FALSE);
    dw_mle_set_word_wrap(g_notemle, TRUE);
    hbox = dw_box_new(DW_HORZ, 0);
    dw_box_pack_start(vbox, hbox, 0, 0, TRUE, FALSE, 0);
    dw_box_pack_start(hbox, 0, 1, 1, TRUE, FALSE, 0);
    ok = dw_button_new("OK", 0);
    dw_box_pack_start(hbox, ok, 90, DW_SIZE_AUTO, FALSE, FALSE, 2);
    dw_signal_connect(ok, DW_SIGNAL_CLICKED, DW_SIGNAL_FUNC(notice_close_cb), NULL);
    dw_signal_connect(g_notewin, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(notice_close_cb), NULL);
    dw_window_default(g_notewin, ok);
    dw_window_set_gravity(g_notewin, DW_GRAV_CENTER, DW_GRAV_CENTER);
    dw_window_set_pos_size(g_notewin, 0, 0, 460, 190);
    /* Created hidden; notice_show() fills and shows it */
}

static void notice_show(const char *title, const char *fmt, ...)
{
    char buf[MSG_MAX * 2 + 1];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    dw_window_set_text(g_notewin, title);
    dw_mle_clear(g_notemle);
    dw_mle_import(g_notemle, buf, -1);
    dw_mle_set_cursor(g_notemle, 0);
    dw_window_show(g_notewin);
}

static void job_free(Job *j)
{
    free_list(j->items, j->count);
    free(j->target);
    free(j->log);
    free(j);
}

/*
 * Runs on the WORKER thread at the end of job_thread(). The main window
 * stays disabled and g_job stays set until the very end, so no GUI
 * handler can touch g_vol or the panes while this runs.
 */
static void job_finish(void)
{
    Job *j = g_job;
    int cancelled = g_cancel;
    char msg[MSG_MAX * 2 + 1] = "";
    const char *title = APP_NAME;

    dw_window_hide(g_progwin);

    switch(j->type)
    {
    case JOB_OPEN:
        if(j->rc > 0)
        {
            set_string(&g_isofile, j->target);
            g_modified = 0;
        }
        else
        {
            vol_reset();
            set_string(&g_isofile, NULL);
            g_modified = 0;
            if(!cancelled)
            {
                title = APP_NAME " - Error";
                snprintf(msg, sizeof(msg), "Opening %s failed:\n%s",
                         j->target, bk_get_error_string(j->rc));
            }
        }
        set_string(&g_isopath, "/");
        iso_refresh();
        break;

    case JOB_ADD:
        if(j->successes)
            g_modified = 1;
        iso_refresh();
        break;

    case JOB_EXTRACT:
        local_refresh();
        break;

    case JOB_SAVE:
        if(j->rc > 0)
        {
            g_modified = 0;
            update_status();
            snprintf(msg, sizeof(msg), "Image written to:\n%s", j->target);
        }
        else if(!cancelled)
        {
            title = APP_NAME " - Error";
            snprintf(msg, sizeof(msg), "Writing %s failed:\n%s",
                     j->target, bk_get_error_string(j->rc));
        }
        break;
    }

    if(j->log && j->loglen)
    {
        size_t used = strlen(msg);

        if(j->loglen > MSG_MAX - 80)
            strcpy(j->log + MSG_MAX - 84, "\n...");
        if(strcmp(title, APP_NAME) == 0)
            title = APP_NAME " - Warning";
        snprintf(msg + used, sizeof(msg) - used, "%s%d item(s) had problems:\n\n%s",
                 used ? "\n\n" : "", j->failures ? j->failures : 1, j->log);
    }
    if(*msg)
        notice_show(title, "%s", msg);

    job_free(j);
    g_job = NULL;
    dw_window_enable(g_mainwin);
}

/* Takes ownership of items and target. */
static void job_start(int type, char **items, int count, char *target)
{
    static const char *titles[] = { "Opening image", "Adding to image", "Extracting", "Writing image" };
    Job *j = calloc(1, sizeof(Job));

    if(!j)
        exit(1);
    j->type = type;
    j->items = items;
    j->count = count;
    j->target = target;
    g_cancel = 0;
    g_lastpct = -1;
    g_job = j;

    dw_window_disable(g_mainwin);
    dw_window_set_text(g_progwin, titles[type]);
    if(type == JOB_OPEN)
        dw_window_set_text(g_progtext, "Reading image...");
    else if(type == JOB_SAVE)
        dw_window_set_text(g_progtext, "Writing image...");
    else
        dw_window_set_text(g_progtext, "Starting...");
    dw_percent_set_pos(g_progbar, 0);
    dw_window_show(g_progwin);
    /* Last GUI access on this thread for the job; the worker owns it now */
    dw_thread_new(DW_SIGNAL_FUNC(job_thread), j, WORKER_STACK);
}

/* ------------------------------------------------------------------ */
/* Image level actions                                                */
/* ------------------------------------------------------------------ */

#define BUSY_CHECK() do { if(g_job) return TRUE; } while(0)

static int confirm_discard(void)
{
    if(!g_modified)
        return 1;
    return dw_messagebox(APP_NAME, DW_MB_YESNO | DW_MB_WARNING,
                         "The current image has unsaved changes.\nDiscard them?") == DW_MB_RETURN_YES;
}

static void open_image(const char *path)
{
    vol_reset();
    set_string(&g_isofile, NULL);
    set_string(&g_isopath, "/");
    g_modified = 0;
    iso_refresh();
    job_start(JOB_OPEN, NULL, 0, xstrdup(path));
}

static int DWSIGNAL act_new(HWND window, void *data)
{
    BUSY_CHECK();
    if(!confirm_discard())
        return TRUE;
    vol_reset();
    set_string(&g_isofile, NULL);
    set_string(&g_isopath, "/");
    g_modified = 0;
    iso_refresh();
    return TRUE;
}

static int DWSIGNAL act_open(HWND window, void *data)
{
    char *file;

    BUSY_CHECK();
    if(!confirm_discard())
        return TRUE;
    file = dw_file_browse("Open ISO image", g_localpath, "iso", DW_FILE_OPEN);
    if(file)
    {
        char *path = xstrdup(file);
        dw_free(file);
        local_normalize(path);
        open_image(path);
        free(path);
    }
    return TRUE;
}

static int DWSIGNAL act_save_as(HWND window, void *data)
{
    char *file, *path;

    BUSY_CHECK();
    file = dw_file_browse("Save ISO image as", g_localpath, "iso", DW_FILE_SAVE);
    if(!file)
        return TRUE;
    path = xstrdup(file);
    dw_free(file);
    local_normalize(path);

    if(!strchr(base_name(path), '.'))
    {
        char *withext = concat3(path, ".iso", NULL);
        free(path);
        path = withext;
    }
    if(g_isofile && strcmp(path, g_isofile) == 0)
    {
        error_box("The image cannot be saved over the file it was opened from.\n"
                  "Choose a different file name.");
        free(path);
        return TRUE;
    }
    if(local_exists(path) && !ask_yes_no("%s\nalready exists. Replace it?", path))
    {
        free(path);
        return TRUE;
    }
    job_start(JOB_SAVE, NULL, 0, path);
    return TRUE;
}

static int DWSIGNAL act_properties(HWND window, void *data)
{
    char info[1024], created[64], types[64] = "";
    const char *labels[2] = { "Volume name:", "Publisher:" };
    const int limits[2] = { 32, 128 };
    char *values[2];
    time_t ct = bk_get_creation_time(g_vol);
    struct tm *tm = ct ? localtime(&ct) : NULL;

    BUSY_CHECK();
    if(tm)
        strftime(created, sizeof(created), "%Y-%m-%d %H:%M:%S", tm);
    else
        strcpy(created, "(new image)");
    if(g_isofile)
    {
        if(g_vol->filenameTypes & FNTYPE_9660)
            strcat(types, "ISO9660 ");
        if(g_vol->filenameTypes & FNTYPE_ROCKRIDGE)
            strcat(types, "RockRidge ");
        if(g_vol->filenameTypes & FNTYPE_JOLIET)
            strcat(types, "Joliet");
    }
    else
        strcpy(types, "(written as ISO9660 + RockRidge + Joliet)");

    snprintf(info, sizeof(info),
             "File: %s\nCreated: %s\nFilename types: %s\nBoot record: %s%s\n"
             "Estimated size: %.1f MB\nData preparer: %s",
             g_isofile ? g_isofile : "Untitled", created, types, boot_type_string(),
             g_vol->bootRecordIsVisible ? " (file on image)" : "",
             (double)bk_estimate_iso_size(g_vol, FN_TYPES) / (1024.0 * 1024.0),
             g_vol->dataPreparer);

    values[0] = xstrdup(bk_get_volume_name(g_vol));
    values[1] = xstrdup(bk_get_publisher(g_vol));
    if(form_dialog("Image properties", info, 2, labels, values, limits))
    {
        if(strcmp(values[0], bk_get_volume_name(g_vol)) != 0)
        {
            bk_set_vol_name(g_vol, values[0]);
            g_modified = 1;
        }
        if(strcmp(values[1], bk_get_publisher(g_vol)) != 0)
        {
            bk_set_publisher(g_vol, values[1]);
            g_modified = 1;
        }
        update_status();
    }
    free(values[0]);
    free(values[1]);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Item actions                                                       */
/* ------------------------------------------------------------------ */

static int DWSIGNAL act_add(HWND window, void *data)
{
    int count, i, n = 0;
    char **sel, **items;

    BUSY_CHECK();
    sel = pane_selection(&g_local, DW_POINTER_TO_INT(data), &count);
    if(!count)
    {
        info_box("Select one or more files or folders in the local pane to add.");
        return TRUE;
    }
    items = calloc(count, sizeof(char *));
    if(!items)
        exit(1);
    for(i = 0; i < count; i++)
    {
        if(iso_find(sel[i]))
        {
            char *ipath = iso_child(g_isopath, sel[i]);
            int rc;

            if(!ask_yes_no("'%s' already exists in %s\nReplace it?", sel[i], g_isopath))
            {
                free(ipath);
                continue;
            }
            rc = bk_delete(g_vol, ipath);
            free(ipath);
            if(rc <= 0)
            {
                bk_error("Replacing", sel[i], rc);
                continue;
            }
            g_modified = 1;
        }
        items[n++] = local_join(g_localpath, sel[i]);
    }
    free_list(sel, count);
    if(n)
        job_start(JOB_ADD, items, n, xstrdup(g_isopath));
    else
    {
        free(items);
        iso_refresh();
    }
    return TRUE;
}

static int DWSIGNAL act_extract(HWND window, void *data)
{
    int count, i, n = 0;
    char **sel, **items;

    BUSY_CHECK();
    sel = pane_selection(&g_iso, DW_POINTER_TO_INT(data), &count);
    if(!count)
    {
        info_box("Select one or more items in the image pane to extract.");
        return TRUE;
    }
    items = calloc(count, sizeof(char *));
    if(!items)
        exit(1);
    for(i = 0; i < count; i++)
    {
        char *dest = local_join(g_localpath, sel[i]);
        int exists = local_exists(dest);

        free(dest);
        if(exists)
        {
            error_box("'%s' already exists in\n%s\nIt was skipped.", sel[i], g_localpath);
            continue;
        }
        items[n++] = iso_child(g_isopath, sel[i]);
    }
    free_list(sel, count);
    if(n)
        job_start(JOB_EXTRACT, items, n, xstrdup(g_localpath));
    else
        free(items);
    return TRUE;
}

static int DWSIGNAL act_new_folder(HWND window, void *data)
{
    char *name;
    int rc;

    BUSY_CHECK();
    name = prompt_text("New folder", "Folder name:", "", NCHARS_FILE_ID_MAX_STORE - 1);
    if(!name)
        return TRUE;
    rc = bk_create_dir(g_vol, g_isopath, name);
    if(rc <= 0)
        bk_error("Creating folder", name, rc);
    else
    {
        g_modified = 1;
        iso_refresh();
    }
    free(name);
    return TRUE;
}

static int DWSIGNAL act_rename(HWND window, void *data)
{
    int count, rc;
    char **sel, *name, *path;

    BUSY_CHECK();
    sel = pane_selection(&g_iso, DW_POINTER_TO_INT(data), &count);
    if(count != 1)
    {
        info_box("Select exactly one item in the image pane to rename.");
        free_list(sel, count);
        return TRUE;
    }
    name = prompt_text("Rename", "New name:", sel[0], NCHARS_FILE_ID_MAX_STORE - 1);
    if(name && strcmp(name, sel[0]) != 0)
    {
        path = iso_child(g_isopath, sel[0]);
        rc = bk_rename(g_vol, path, name);
        if(rc <= 0)
            bk_error("Renaming", sel[0], rc);
        else
        {
            g_modified = 1;
            iso_refresh();
        }
        free(path);
    }
    free(name);
    free_list(sel, count);
    return TRUE;
}

static int DWSIGNAL act_delete(HWND window, void *data)
{
    int count, i, rc;
    char **sel;

    BUSY_CHECK();
    sel = pane_selection(&g_iso, DW_POINTER_TO_INT(data), &count);
    if(!count)
    {
        info_box("Select one or more items in the image pane to delete.");
        return TRUE;
    }
    if((count == 1 && ask_yes_no("Remove '%s' from the image?", sel[0])) ||
       (count > 1 && ask_yes_no("Remove %d items from the image?", count)))
    {
        for(i = 0; i < count; i++)
        {
            char *path = iso_child(g_isopath, sel[i]);
            rc = bk_delete(g_vol, path);
            if(rc <= 0)
                bk_error("Deleting", sel[i], rc);
            else
                g_modified = 1;
            free(path);
        }
        iso_refresh();
    }
    free_list(sel, count);
    return TRUE;
}

static int DWSIGNAL act_permissions(HWND window, void *data)
{
    int count, i, rc;
    char **sel, *path, *value, initial[8], *end;
    mode_t mode = 0;
    unsigned long newmode;

    BUSY_CHECK();
    sel = pane_selection(&g_iso, DW_POINTER_TO_INT(data), &count);
    if(!count)
    {
        info_box("Select one or more items in the image pane.");
        return TRUE;
    }
    path = iso_child(g_isopath, sel[0]);
    if(bk_get_permissions(g_vol, path, &mode) <= 0)
        mode = 0644;
    free(path);
    snprintf(initial, sizeof(initial), "%03o", (unsigned)(mode & 0777));

    value = prompt_text("Permissions", "Mode (octal):", initial, 4);
    if(value)
    {
        newmode = strtoul(value, &end, 8);
        if(*end || newmode > 0777)
            error_box("'%s' is not a valid octal mode (000 - 777).", value);
        else
        {
            for(i = 0; i < count; i++)
            {
                path = iso_child(g_isopath, sel[i]);
                rc = bk_set_permissions(g_vol, path, (mode_t)newmode);
                if(rc <= 0)
                    bk_error("Setting permissions on", sel[i], rc);
                free(path);
            }
            g_modified = 1;
            iso_refresh();
        }
        free(value);
    }
    free_list(sel, count);
    return TRUE;
}

static int DWSIGNAL act_refresh(HWND window, void *data)
{
    BUSY_CHECK();
    local_refresh();
    iso_refresh();
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Boot record actions                                                */
/* ------------------------------------------------------------------ */

static int DWSIGNAL act_boot_info(HWND window, void *data)
{
    BUSY_CHECK();
    if(g_vol->bootMediaType == BOOT_MEDIA_NONE)
        info_box("This image has no El Torito boot record.");
    else
        info_box("Boot record type: %s\nSize: %u bytes\nLocation: %s",
                 boot_type_string(), g_vol->bootRecordSize,
                 g_vol->bootRecordIsVisible ? "file in the image tree" :
                 g_vol->bootRecordIsOnImage ? "hidden, on the original image" :
                 g_vol->bootRecordPathAndName ? g_vol->bootRecordPathAndName : "unknown");
    return TRUE;
}

static int DWSIGNAL act_boot_extract(HWND window, void *data)
{
    char *file, *path;
    int rc;

    BUSY_CHECK();
    if(g_vol->bootMediaType == BOOT_MEDIA_NONE)
    {
        info_box("This image has no El Torito boot record.");
        return TRUE;
    }
    file = dw_file_browse("Save boot record as", g_localpath, "img", DW_FILE_SAVE);
    if(!file)
        return TRUE;
    path = xstrdup(file);
    dw_free(file);
    local_normalize(path);
    if(!local_exists(path) || ask_yes_no("%s\nalready exists. Replace it?", path))
    {
        rc = bk_extract_boot_record(g_vol, path, 0644);
        if(rc <= 0)
            bk_error("Extracting boot record to", path, rc);
        else
            local_refresh();
    }
    free(path);
    return TRUE;
}

static int DWSIGNAL act_boot_add(HWND window, void *data)
{
    int type = DW_POINTER_TO_INT(data), rc;
    char *file, *path;

    BUSY_CHECK();
    file = dw_file_browse("Select boot image", g_localpath, "img", DW_FILE_OPEN);
    if(!file)
        return TRUE;
    path = xstrdup(file);
    dw_free(file);
    local_normalize(path);
    rc = bk_add_boot_record(g_vol, path, type);
    if(rc <= 0)
        bk_error("Adding boot record", path, rc);
    else
    {
        g_modified = 1;
        update_status();
    }
    free(path);
    return TRUE;
}

static int DWSIGNAL act_boot_use_selected(HWND window, void *data)
{
    int count, rc;
    char **sel, *path;
    BkFileBase *item;

    BUSY_CHECK();
    sel = pane_selection(&g_iso, DW_POINTER_TO_INT(data), &count);
    if(count != 1 || !(item = iso_find(sel[0])) || !IS_REG_FILE(item->posixFileMode))
    {
        info_box("Select exactly one file in the image pane (for example isolinux.bin).");
        free_list(sel, count);
        return TRUE;
    }
    path = iso_child(g_isopath, sel[0]);
    rc = bk_set_boot_file(g_vol, path);
    if(rc <= 0)
        bk_error("Setting boot file", sel[0], rc);
    else
    {
        g_modified = 1;
        update_status();
        info_box("'%s' will be used as a no-emulation boot record.", path);
    }
    free(path);
    free_list(sel, count);
    return TRUE;
}

static int DWSIGNAL act_boot_remove(HWND window, void *data)
{
    BUSY_CHECK();
    if(g_vol->bootMediaType == BOOT_MEDIA_NONE)
    {
        info_box("This image has no El Torito boot record.");
        return TRUE;
    }
    if(ask_yes_no("Remove the boot record? The image will no longer be bootable."))
    {
        bk_delete_boot_record(g_vol);
        g_modified = 1;
        update_status();
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Navigation                                                         */
/* ------------------------------------------------------------------ */

static int DWSIGNAL iso_up_cb(HWND window, void *data)
{
    char *parent;

    BUSY_CHECK();
    parent = iso_parent(g_isopath);
    iso_chdir(parent);
    free(parent);
    return TRUE;
}

static int DWSIGNAL iso_go_cb(HWND window, void *data)
{
    char *text, *path;
    BkDir *dir;

    BUSY_CHECK();
    text = dw_window_get_text(g_iso.pathentry);
    path = iso_normalize(text ? text : "/");
    if(text)
        dw_free(text);
    if(bk_get_dir_from_string(g_vol, path, &dir) <= 0)
    {
        error_box("Folder not found in image:\n%s", path);
        dw_window_set_text(g_iso.pathentry, g_isopath);
    }
    else
        iso_chdir(path);
    free(path);
    return TRUE;
}

static int DWSIGNAL local_up_cb(HWND window, void *data)
{
    char *parent;

    BUSY_CHECK();
    parent = local_parent(g_localpath);
    local_chdir(parent);
    free(parent);
    return TRUE;
}

static int DWSIGNAL local_go_cb(HWND window, void *data)
{
    char *text, *path;

    BUSY_CHECK();
    text = dw_window_get_text(g_local.pathentry);
    path = xstrdup(text ? text : "");
    if(text)
        dw_free(text);
    local_normalize(path);
    if(!*path || !local_is_dir(path))
    {
        error_box("Folder not found:\n%s", path);
        dw_window_set_text(g_local.pathentry, g_localpath);
    }
    else
        local_chdir(path);
    free(path);
    return TRUE;
}

static int DWSIGNAL iso_enter_cb(HWND window, char *text, void *data, void *itemdata)
{
    BkFileBase *item;

    BUSY_CHECK();
    if(!text)
        return TRUE;
    if(strcmp(text, "..") == 0)
        return iso_up_cb(window, NULL);
    item = iso_find(text);
    if(item && IS_DIR(item->posixFileMode))
    {
        char *path = concat3(g_isopath, text, "/");
        iso_chdir(path);
        free(path);
    }
    return TRUE;
}

static int DWSIGNAL local_enter_cb(HWND window, char *text, void *data, void *itemdata)
{
    char *full;

    BUSY_CHECK();
    if(!text)
        return TRUE;
    if(strcmp(text, "..") == 0)
        return local_up_cb(window, NULL);
    full = local_join(g_localpath, text);
    if(local_is_dir(full))
        local_chdir(full);
    else if((has_extension(full, ".iso") || has_extension(full, ".nrg")) &&
            ask_yes_no("Open '%s' as the current image?", text) && confirm_discard())
        open_image(full);
    free(full);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Menus                                                              */
/* ------------------------------------------------------------------ */

static HWND menu_item(HMENUI menu, const char *title, ULONG id, void *func, void *data)
{
    HWND item = dw_menu_append_item(menu, title, id, 0, TRUE, FALSE, DW_NOMENU);
    if(func)
        dw_signal_connect(item, DW_SIGNAL_CLICKED, func, data);
    return item;
}

static void menu_separator(HMENUI menu, ULONG id)
{
    dw_menu_append_item(menu, DW_MENU_SEPARATOR, id, 0, TRUE, FALSE, DW_NOMENU);
}

static void set_context_text(const char *text)
{
    set_string(&g_context_text, text);
}

#define CTX DW_INT_TO_POINTER(1)

static int DWSIGNAL iso_context_cb(HWND window, char *text, int x, int y, void *data, void *itemdata)
{
    HMENUI menu;

    BUSY_CHECK();
    set_context_text(text);
    menu = dw_menu_new(0);
    menu_item(menu, "~Extract to Local Folder", DW_MENU_POPUP, DW_SIGNAL_FUNC(act_extract), CTX);
    menu_item(menu, "~Rename...", DW_MENU_POPUP, DW_SIGNAL_FUNC(act_rename), CTX);
    menu_item(menu, "~Delete", DW_MENU_POPUP, DW_SIGNAL_FUNC(act_delete), CTX);
    menu_item(menu, "~Permissions...", DW_MENU_POPUP, DW_SIGNAL_FUNC(act_permissions), CTX);
    menu_item(menu, "Use as ~Boot File", DW_MENU_POPUP, DW_SIGNAL_FUNC(act_boot_use_selected), CTX);
    menu_separator(menu, DW_MENU_POPUP);
    menu_item(menu, "~New Folder...", DW_MENU_POPUP, DW_SIGNAL_FUNC(act_new_folder), NULL);
    menu_item(menu, "Re~fresh", DW_MENU_POPUP, DW_SIGNAL_FUNC(act_refresh), NULL);
    dw_menu_popup(&menu, g_mainwin, x, y);
    return TRUE;
}

static int DWSIGNAL local_context_cb(HWND window, char *text, int x, int y, void *data, void *itemdata)
{
    HMENUI menu;

    BUSY_CHECK();
    set_context_text(text);
    menu = dw_menu_new(0);
    menu_item(menu, "~Add to Image", DW_MENU_POPUP, DW_SIGNAL_FUNC(act_add), CTX);
    menu_separator(menu, DW_MENU_POPUP);
    menu_item(menu, "Re~fresh", DW_MENU_POPUP, DW_SIGNAL_FUNC(act_refresh), NULL);
    dw_menu_popup(&menu, g_mainwin, x, y);
    return TRUE;
}

static int DWSIGNAL act_about(HWND window, void *data)
{
    DWEnv env;

    dw_environment_query(&env);
    info_box(APP_NAME " " APP_VERSION "\n\n"
             "ISO9660 / Rock Ridge / Joliet / El Torito image editor.\n"
             "ISO engine: bkisofs\nGUI: Dynamic Windows %d.%d.%d on %s",
             env.DWMajorVersion, env.DWMinorVersion, env.DWSubVersion, env.osName);
    return TRUE;
}

static int DWSIGNAL act_exit(HWND window, void *data)
{
    if(g_job)
    {
        info_box("Please wait for the current operation to finish or cancel it.");
        return TRUE;
    }
    if(confirm_discard())
        dw_main_quit();
    return TRUE;
}

static void menus_create(void)
{
    HMENUI menubar = dw_menubar_new(g_mainwin);
    HMENUI file, image, boot, bootadd, help;

    file = dw_menu_new(0);
    menu_item(file, "~New Image", DW_MENU_AUTO, DW_SIGNAL_FUNC(act_new), NULL);
    menu_item(file, "~Open Image...", DW_MENU_AUTO, DW_SIGNAL_FUNC(act_open), NULL);
    menu_item(file, "Save Image ~As...", DW_MENU_AUTO, DW_SIGNAL_FUNC(act_save_as), NULL);
    menu_separator(file, DW_MENU_AUTO);
    menu_item(file, "~Properties...", DW_MENU_AUTO, DW_SIGNAL_FUNC(act_properties), NULL);
    menu_separator(file, DW_MENU_AUTO);
    menu_item(file, "E~xit", DW_MENU_AUTO, DW_SIGNAL_FUNC(act_exit), NULL);
    dw_menu_append_item(menubar, "~File", DW_MENU_AUTO, 0, TRUE, FALSE, file);

    image = dw_menu_new(0);
    menu_item(image, "~Add Selected Local Items", DW_MENU_AUTO, DW_SIGNAL_FUNC(act_add), NULL);
    menu_item(image, "~Extract Selected Items", DW_MENU_AUTO, DW_SIGNAL_FUNC(act_extract), NULL);
    menu_separator(image, DW_MENU_AUTO);
    menu_item(image, "~New Folder...", DW_MENU_AUTO, DW_SIGNAL_FUNC(act_new_folder), NULL);
    menu_item(image, "~Rename...", DW_MENU_AUTO, DW_SIGNAL_FUNC(act_rename), NULL);
    menu_item(image, "~Delete", DW_MENU_AUTO, DW_SIGNAL_FUNC(act_delete), NULL);
    menu_item(image, "~Permissions...", DW_MENU_AUTO, DW_SIGNAL_FUNC(act_permissions), NULL);
    menu_separator(image, DW_MENU_AUTO);
    menu_item(image, "Re~fresh", DW_MENU_AUTO, DW_SIGNAL_FUNC(act_refresh), NULL);
    dw_menu_append_item(menubar, "~Image", DW_MENU_AUTO, 0, TRUE, FALSE, image);

    boot = dw_menu_new(0);
    bootadd = dw_menu_new(0);
    menu_item(bootadd, "~No Emulation...", DW_MENU_AUTO, DW_SIGNAL_FUNC(act_boot_add),
              DW_INT_TO_POINTER(BOOT_MEDIA_NO_EMULATION));
    menu_item(bootadd, "1.~2 MB Floppy Image...", DW_MENU_AUTO, DW_SIGNAL_FUNC(act_boot_add),
              DW_INT_TO_POINTER(BOOT_MEDIA_1_2_FLOPPY));
    menu_item(bootadd, "1.~44 MB Floppy Image...", DW_MENU_AUTO, DW_SIGNAL_FUNC(act_boot_add),
              DW_INT_TO_POINTER(BOOT_MEDIA_1_44_FLOPPY));
    menu_item(bootadd, "2.~88 MB Floppy Image...", DW_MENU_AUTO, DW_SIGNAL_FUNC(act_boot_add),
              DW_INT_TO_POINTER(BOOT_MEDIA_2_88_FLOPPY));
    menu_item(boot, "Boot Record ~Info", DW_MENU_AUTO, DW_SIGNAL_FUNC(act_boot_info), NULL);
    menu_item(boot, "~Extract Boot Record...", DW_MENU_AUTO, DW_SIGNAL_FUNC(act_boot_extract), NULL);
    dw_menu_append_item(boot, "~Add Boot Record from File", DW_MENU_AUTO, 0, TRUE, FALSE, bootadd);
    menu_item(boot, "~Use Selected File as Boot Record", DW_MENU_AUTO,
              DW_SIGNAL_FUNC(act_boot_use_selected), NULL);
    menu_separator(boot, DW_MENU_AUTO);
    menu_item(boot, "~Remove Boot Record", DW_MENU_AUTO, DW_SIGNAL_FUNC(act_boot_remove), NULL);
    dw_menu_append_item(menubar, "~Boot", DW_MENU_AUTO, 0, TRUE, FALSE, boot);

    help = dw_menu_new(0);
    menu_item(help, "~About...", DW_MENU_AUTO, DW_SIGNAL_FUNC(act_about), NULL);
    dw_menu_append_item(menubar, "~Help", DW_MENU_AUTO, 0, TRUE, FALSE, help);
}

/* ------------------------------------------------------------------ */
/* Window construction                                                */
/* ------------------------------------------------------------------ */

static HWND tool_button(HWND box, const char *text, const char *tip, void *func)
{
    HWND b = dw_button_new(text, 0);
    dw_box_pack_start(box, b, DW_SIZE_AUTO, DW_SIZE_AUTO, FALSE, FALSE, 1);
    dw_window_set_tooltip(b, tip);
    dw_signal_connect(b, DW_SIGNAL_CLICKED, func, NULL);
    return b;
}

static HWND pane_create(Pane *p, const char *title, HWND *label, ULONG id,
                        void *gofunc, void *upfunc, void *enterfunc, void *contextfunc)
{
    static unsigned long flags[3] = {
        DW_CFA_ULONG | DW_CFA_RIGHT | DW_CFA_HORZSEPARATOR | DW_CFA_SEPARATOR,
        DW_CFA_STRING | DW_CFA_LEFT | DW_CFA_HORZSEPARATOR | DW_CFA_SEPARATOR,
        DW_CFA_STRING | DW_CFA_LEFT | DW_CFA_HORZSEPARATOR | DW_CFA_SEPARATOR
    };
    static char *titles[3] = { "Size", "Type", "Mode" };
    HWND box, hbox, text, go, up;

    box = dw_box_new(DW_VERT, 2);
    text = dw_text_new(title, 0);
    dw_window_set_style(text, DW_DT_VCENTER, DW_DT_VCENTER);
    dw_box_pack_start(box, text, DW_SIZE_AUTO, DW_SIZE_AUTO, TRUE, FALSE, 2);
    if(label)
        *label = text;

    hbox = dw_box_new(DW_HORZ, 0);
    dw_box_pack_start(box, hbox, 0, 0, TRUE, FALSE, 0);
    p->pathentry = dw_entryfield_new("", 0);
    dw_box_pack_start(hbox, p->pathentry, 100, DW_SIZE_AUTO, TRUE, FALSE, 1);
    go = dw_button_new("Go", 0);
    dw_box_pack_start(hbox, go, DW_SIZE_AUTO, DW_SIZE_AUTO, FALSE, FALSE, 1);
    up = dw_button_new("Up", 0);
    dw_box_pack_start(hbox, up, DW_SIZE_AUTO, DW_SIZE_AUTO, FALSE, FALSE, 1);
    dw_window_click_default(p->pathentry, go);

    p->cont = dw_container_new(id, TRUE);
    dw_box_pack_start(box, p->cont, 300, 300, TRUE, TRUE, 1);
    dw_filesystem_set_column_title(p->cont, "Name");
    dw_filesystem_setup(p->cont, flags, titles, 3);

    dw_signal_connect(go, DW_SIGNAL_CLICKED, gofunc, NULL);
    dw_signal_connect(up, DW_SIGNAL_CLICKED, upfunc, NULL);
    dw_signal_connect(p->cont, DW_SIGNAL_ITEM_ENTER, enterfunc, NULL);
    dw_signal_connect(p->cont, DW_SIGNAL_ITEM_CONTEXT, contextfunc, NULL);
    return box;
}

static HICN icon_load(const char *name)
{
    HICN icon = dw_icon_load_from_file(name);
    if(!icon)
    {
        char *dir = dw_app_dir();
        if(dir && *dir)
        {
            char *path = local_join(dir, name);
            icon = dw_icon_load_from_file(path);
            free(path);
        }
    }
    return icon;
}

static void mainwindow_create(void)
{
    HWND mainbox, toolbar, left, right;

    g_mainwin = dw_window_new(HWND_DESKTOP, APP_NAME,
                              DW_FCF_SYSMENU | DW_FCF_TITLEBAR | DW_FCF_TASKLIST |
                              DW_FCF_DLGBORDER | DW_FCF_SIZEBORDER | DW_FCF_MINMAX);
    menus_create();

    mainbox = dw_box_new(DW_VERT, 2);
    dw_box_pack_start(g_mainwin, mainbox, 0, 0, TRUE, TRUE, 0);

    toolbar = dw_box_new(DW_HORZ, 2);
    dw_box_pack_start(mainbox, toolbar, 0, 0, TRUE, FALSE, 0);
    tool_button(toolbar, "New", "Start a new empty image", DW_SIGNAL_FUNC(act_new));
    tool_button(toolbar, "Open...", "Open an ISO or NRG image", DW_SIGNAL_FUNC(act_open));
    tool_button(toolbar, "Save As...", "Write the edited image to a new file", DW_SIGNAL_FUNC(act_save_as));
    dw_box_pack_start(toolbar, 0, 10, 1, FALSE, FALSE, 0);
    tool_button(toolbar, "Add >>", "Add selected local items to the current image folder", DW_SIGNAL_FUNC(act_add));
    tool_button(toolbar, "<< Extract", "Extract selected image items to the current local folder", DW_SIGNAL_FUNC(act_extract));
    dw_box_pack_start(toolbar, 0, 10, 1, FALSE, FALSE, 0);
    tool_button(toolbar, "New Folder", "Create a folder in the image", DW_SIGNAL_FUNC(act_new_folder));
    tool_button(toolbar, "Rename", "Rename the selected image item", DW_SIGNAL_FUNC(act_rename));
    tool_button(toolbar, "Delete", "Remove selected items from the image", DW_SIGNAL_FUNC(act_delete));
    tool_button(toolbar, "Properties", "Volume name, publisher and image details", DW_SIGNAL_FUNC(act_properties));
    dw_box_pack_start(toolbar, 0, 1, 1, TRUE, FALSE, 0);

    left = pane_create(&g_local, "Local filesystem", NULL, 100,
                       DW_SIGNAL_FUNC(local_go_cb), DW_SIGNAL_FUNC(local_up_cb),
                       DW_SIGNAL_FUNC(local_enter_cb), DW_SIGNAL_FUNC(local_context_cb));
    right = pane_create(&g_iso, "ISO image: Untitled", &g_isolabel, 101,
                        DW_SIGNAL_FUNC(iso_go_cb), DW_SIGNAL_FUNC(iso_up_cb),
                        DW_SIGNAL_FUNC(iso_enter_cb), DW_SIGNAL_FUNC(iso_context_cb));
    g_split = dw_splitbar_new(DW_HORZ, left, right, 0);
    dw_box_pack_start(mainbox, g_split, 600, 400, TRUE, TRUE, 0);
    dw_splitbar_set(g_split, 50.0);

    g_status = dw_status_text_new("", 0);
    dw_box_pack_start(mainbox, g_status, 100, DW_SIZE_AUTO, TRUE, FALSE, 1);

    dw_signal_connect(g_mainwin, DW_SIGNAL_DELETE, DW_SIGNAL_FUNC(act_exit), NULL);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

int dwmain(int argc, char *argv[])
{
    char cwd[PATH_MAX + 1];

    dw_init(TRUE, argc, argv);

    g_vol = calloc(1, sizeof(VolInfo));
    if(!g_vol)
    {
        dw_messagebox(APP_NAME, DW_MB_OK | DW_MB_ERROR, "Out of memory.");
        return 1;
    }
    vol_reset();
    set_string(&g_isopath, "/");
    if(getcwd(cwd, sizeof(cwd)))
        set_string(&g_localpath, cwd);
    else
        set_string(&g_localpath, dw_user_dir());
    local_normalize(g_localpath);

    g_foldericon = icon_load("folder");
    g_fileicon = icon_load("file");

    mainwindow_create();
    progress_window_create();
    notice_create();
    local_refresh();
    iso_refresh();

    dw_window_set_gravity(g_mainwin, DW_GRAV_CENTER | DW_GRAV_OBSTACLES,
                          DW_GRAV_CENTER | DW_GRAV_OBSTACLES);
    dw_window_set_pos_size(g_mainwin, 0, 0, 980, 620);
    dw_window_show(g_mainwin);

    if(argc > 1)
    {
        char *path = xstrdup(argv[1]);
        local_normalize(path);
        open_image(path);
        free(path);
    }

    dw_main();

    /* Main loop has ended, so no pending GTK timeouts can fire any more */
    dw_window_destroy(g_notewin);
    dw_window_destroy(g_progwin);
    dw_window_destroy(g_mainwin);
    bk_destroy_vol_info(g_vol);
    free(g_vol);
    pool_free(&g_local);
    pool_free(&g_iso);
    free(g_local.pool);
    free(g_iso.pool);
    free(g_isofile);
    free(g_isopath);
    free(g_localpath);
    free(g_context_text);
    if(g_foldericon)
        dw_icon_free(g_foldericon);
    if(g_fileicon)
        dw_icon_free(g_fileicon);
    dw_exit(0);
    return 0;
}
