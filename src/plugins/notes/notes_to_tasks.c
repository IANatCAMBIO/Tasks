/* ===========================================================================
 * notes_to_tasks.c — the Notes action-item mirror, as a plugin.
 *
 * Mirrors the companion Notes app's action items ('!' lines) as ORDINARY
 * tasks, so each one carries notes, subtasks, attachments, a pin and a
 * priority like anything else — and, living in a real list, syncs on to
 * Google Tasks too.  notes_api.c underneath is the CLI wrapper.
 *
 * FIELD OWNERSHIP.  TITLE, DONE and DUE are SHARED — each is pushed when
 * it drifts from the baseline and pulled from Notes otherwise, with a
 * local change winning a tie.  Everything else is Tasks-only and never
 * leaves.  Notes' DONE is BINARY, so the mirror speaks only in the
 * done-ness of `status`: a New <-> In Progress move is not a pending
 * write and has nothing to push.
 *
 * TITLE became two-way once the Notes CLI grew `action text UID TEXT`
 * (it keeps the item's uid, its done state and its due date).  Before
 * that there was no verb to rewrite an item's text, so a title edited
 * here was simply overwritten on the next pass.
 *
 * WRITES ARE CACHED, NOT LIVE.  notes_task.text/done/due hold what Notes
 * was last known to have, so the rows whose text, done-ness or due date
 * differs from that baseline ARE the pending-write set — no queue table
 * to corrupt, and it survives a crash.
 *
 * IDENTITY IS THE UID, never the position: NOTEID:ORD renumbers whenever
 * a note gains or loses a '!' line, so a stored positional ref silently
 * comes to mean a different item and a "done" tick would strike the
 * wrong line.
 *
 * The plugin reaches the app ONLY through the host table (plugin.h).
 * =========================================================================== */

#include "plugin_ctx.h"
#include "notes_api.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The host table and this plugin's identity — defined here, declared in
 * plugin_ctx.h so notes_api.c shares exactly one of each.                    */
const TaskHostApi *host = NULL;
const TaskPlugin  *self = NULL;

/* The mirror's in-flight guard and timer.  These used to be fields on
 * TaskApp; a plugin owns its own, which is the point — the app no longer
 * carries a slot per integration.                                        */
static gboolean bn_running = FALSE;
static guint    bn_timer   = 0;

/* bn_status() — task_app_status's printf shape over the host's plain
 * status(), so the call sites below read as they always did.  The status
 * bar is a PLAIN-TEXT label, so nothing here is markup-escaped.          */
static void
bn_status(TaskApp *app, const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);

static void
bn_status(TaskApp *app, const gchar *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    host->notify->status(app, msg);
    g_free(msg);
}

/* The list mirrored items land in when Settings names none.  Created on
 * first use; the emoji marks it the way the sync marks Google's default
 * list.                                                                    */
#define BN_LIST_NAME  "Action Items"
#define BN_LIST_EMOJI "\xe2\x9d\x97"          /* ❗                          */

/* ===========================================================================
 * The side tables (schema v9).
 *
 * A mirrored task's Notes identity — its stable uid — and the BASELINE of
 * what Notes was last known to hold live in notes_task, keyed by task id.
 * None of it is on the core rows any more.
 *
 * The baseline is what makes the write-back a bulk diff rather than a
 * queue: a row whose done-ness or due differs from it IS pending, which
 * survives a crash and cannot drift out of step with the task.
 * =========================================================================== */

/* collect_i64() — exec_query callback appending the first column to a
 * GArray of gint64.                                                      */
static gint
collect_i64(gpointer data, gint n_cols, gchar **values, gchar **names)
{
    (void)names;
    GArray *out = data;
    if (n_cols > 0 && values[0] != NULL) {
        gint64 v = g_ascii_strtoll(values[0], NULL, 10);
        g_array_append_val(out, v);
    }
    return 0;
}

/* bn_tasks_for() — run an id-yielding query and load those tasks.
 *
 * Two steps rather than one join returning task columns: the row shape
 * belongs to db.c and is not the plugin's to reproduce.  The id list is
 * one query; the loads are by primary key.                               */
static GPtrArray *
bn_tasks_for(TaskDatabase *db, const gchar *id_sql)
{
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    host->db->exec_query(db, id_sql, collect_i64, ids);
    GPtrArray *out = g_ptr_array_new();
    for (guint i = 0; i < ids->len; i++) {
        Task *t = host->db->task_get(db, g_array_index(ids, gint64, i));
        if (t != NULL)
            g_ptr_array_add(out, t);
    }
    g_array_free(ids, TRUE);
    return out;
}

/* bn_uid_of() — a task's Notes uid, or 0.                                 */
static gint64
bn_uid_of(TaskDatabase *db, gint64 task_id)
{
    gchar *sql = g_strdup_printf(
        "SELECT uid FROM notes_task WHERE task_id = %" G_GINT64_FORMAT,
        task_id);
    gint64 uid = host->db->scalar(db, sql);
    g_free(sql);
    return uid > 0 ? uid : 0;
}

/* bn_task_for_uid() — the visible mirror task carrying `uid`, or NULL.   */
static Task *
bn_task_for_uid(TaskDatabase *db, gint64 uid)
{
    gchar *sql = g_strdup_printf(
        "SELECT n.task_id FROM notes_task n JOIN tasks t ON t.id = n.task_id"
        " WHERE n.uid = %" G_GINT64_FORMAT " AND t.deleted = 0 LIMIT 1",
        uid);
    gint64 id = host->db->scalar(db, sql);
    g_free(sql);
    return id > 0 ? host->db->task_get(db, id) : NULL;
}

/* bn_take_text() — exec_query callback taking the first column as a
 * string into *data (a gchar** the caller frees).  Only the FIRST row is
 * kept: every caller selects by primary key.                             */
static gint
bn_take_text(gpointer data, gint n_cols, gchar **values, gchar **names)
{
    (void)names;
    gchar **out = data;
    if (*out == NULL && n_cols > 0 && values[0] != NULL)
        *out = g_strdup(values[0]);
    return 0;
}

/* ---------------------------------------------------------------------------
 * bn_baseline() — what Notes was last known to hold for this task.
 *
 * `text` receives a newly allocated string the caller frees, EMPTY (not
 * NULL) when there is no row or the column is NULL.  "" is the
 * NO-BASELINE-YET sentinel, and sync_item must read it as "adopt what
 * Notes holds", never as "the item's text was empty": a task bound
 * before this column existed would otherwise look locally renamed on the
 * first pass after the upgrade and push its possibly-stale title over a
 * rewording Notes had made in the meantime.
 *
 * "" is safe as that sentinel because Notes cannot hold an item whose
 * text is blank — a '!' line with no text is not an action item at all
 * (action_rest_real), and `action text` refuses a blank rename.
 * ------------------------------------------------------------------------- */
static void
bn_baseline(TaskDatabase *db, gint64 task_id, gchar **text, gboolean *done,
            gint64 *due)
{
    gchar *sql = g_strdup_printf(
        "SELECT done FROM notes_task WHERE task_id = %" G_GINT64_FORMAT,
        task_id);
    *done = host->db->scalar(db, sql) > 0;
    g_free(sql);
    sql = g_strdup_printf(
        "SELECT due FROM notes_task WHERE task_id = %" G_GINT64_FORMAT,
        task_id);
    gint64 d = host->db->scalar(db, sql);
    *due = d > 0 ? d : 0;
    g_free(sql);
    /* Text needs a row-shaped read: host->db->scalar answers in gint64.   */
    *text = NULL;
    sql = g_strdup_printf(
        "SELECT text FROM notes_task WHERE task_id = %" G_GINT64_FORMAT,
        task_id);
    host->db->exec_query(db, sql, bn_take_text, text);
    g_free(sql);
    if (*text == NULL)
        *text = g_strdup("");
}

/* bn_set() — bind a task to a uid and record the baseline.  Deliberately
 * does NOT stamp updated_at: the binding is local bookkeeping, not a
 * change to the task.  `text` is quoted through the host (never
 * hand-escaped) and a NULL is stored as ''.                              */
static void
bn_set(TaskDatabase *db, gint64 task_id, gint64 uid, const gchar *text,
       gboolean done, gint64 due)
{
    gchar *q = host->db->quote(text != NULL ? text : "");
    gchar *sql = g_strdup_printf(
        "INSERT INTO notes_task (task_id, uid, text, done, due)"
        " VALUES (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", %s, %d,"
        "         %" G_GINT64_FORMAT ")"
        " ON CONFLICT(task_id) DO UPDATE SET uid = excluded.uid,"
        "   text = excluded.text, done = excluded.done, due = excluded.due",
        task_id, uid, q, done ? 1 : 0, due);
    host->db->exec(db, sql);
    g_free(sql);
    g_free(q);
}

/* bn_mirror_tasks() — every visible mirrored task.                       */
static GPtrArray *
bn_mirror_tasks(TaskDatabase *db)
{
    return bn_tasks_for(db,
        "SELECT n.task_id FROM notes_task n JOIN tasks t ON t.id = n.task_id"
        " WHERE t.deleted = 0"
        " ORDER BY t.priority DESC, t.list_id, t.position, t.id");
}

/* ---------------------------------------------------------------------------
 * One mirror pass in flight.  Built on the main thread, handed to the
 * worker, freed by the completion callback.
 * ------------------------------------------------------------------------- */
typedef struct {
    TaskApp          *app;
    gchar          *db_path;         /* worker opens its OWN connection     */

    gboolean        ok;
    gchar          *message;         /* summary or error (owned)            */
    gint            n_created;
    gint            n_updated;
    gint            n_removed;
    gint            n_pushed;
    gint            n_failed;        /* pushes Notes refused              */
    gint            n_unreaped;      /* reap refused — see reap_missing()   */

    /* Mirror tasks Notes REWORDED this pass.  A filing rule matches on
     * the item's TEXT, so a changed text can change where the item
     * belongs — but ops->move_to_list is main-thread only, so the worker
     * collects the ids and bn_apply does the moving.                    */
    GArray         *refile;
} BnJob;

/* The `position` column's default, in ONE place: it is declared in the
 * CREATE and again in the ALTER, and two spellings of one value is how
 * they come to disagree.                                                 */
#define BN_POSITION_DEFAULT "DEFAULT 0"

/* Same rule for notes_task.text — the TEXT baseline, declared in the
 * CREATE and again in the ALTER.  NOT NULL DEFAULT '' so a row migrated
 * from before it existed reads as "" rather than NULL, which is the
 * "no baseline yet" value bn_baseline already answers with.             */
#define BN_TEXT_COLUMN "text TEXT NOT NULL DEFAULT ''"

/* bn_col_seen() — exec_query callback over PRAGMA table_info, looking
 * for the name in `data`.  Column 1 is the name; the callback keeps
 * scanning either way, because aborting a PRAGMA mid-read buys nothing
 * on a table of four columns.                                           */
typedef struct { const gchar *want; gboolean found; } BnColLook;

static gint
bn_col_seen(gpointer data, gint n_cols, gchar **values, gchar **names)
{
    (void)names;
    BnColLook *look = data;
    if (n_cols > 1 && values[1] != NULL &&
        g_strcmp0(values[1], look->want) == 0)
        look->found = TRUE;
    return 0;
}

/* ---------------------------------------------------------------------------
 * bn_table_has_column() — does `table` already carry `column`?
 *
 * FALSE when it does not AND when the PRAGMA could not run at all, which
 * is the conservative answer for the only caller: a migration that then
 * tries the ALTER and reports sqlite's own message, rather than one that
 * silently decides the column is there and moves on.  (db.c's own
 * table_has_column makes the same choice; a plugin cannot call it —
 * nothing sqlite-shaped crosses the ABI.)
 * ------------------------------------------------------------------------- */
static gboolean
bn_table_has_column(TaskDatabase *db, const gchar *table, const gchar *column)
{
    BnColLook look = { column, FALSE };
    gchar    *sql  = g_strdup_printf("PRAGMA table_info(%s)", table);
    host->db->exec_query(db, sql, bn_col_seen, &look);
    g_free(sql);
    return look.found;
}

/* ---------------------------------------------------------------------------
 * bn_managed_find() — the mirror's own "Action Items" list, or 0 when it
 * does not exist.  READ-ONLY, so the Settings table can name it without
 * bringing it into being just by being looked at.
 * ------------------------------------------------------------------------- */
static gint64
bn_managed_find(TaskDatabase *db)
{
    GPtrArray *lists = host->db->lists(db, FALSE);
    gint64     found = 0;
    for (guint i = 0; i < lists->len && found == 0; i++) {
        TaskList *l = g_ptr_array_index(lists, i);
        if (g_strcmp0(l->name, BN_LIST_NAME) == 0)
            found = l->id;
    }
    host->db->lists_free(lists);
    return found;
}

/* ---------------------------------------------------------------------------
 * bn_managed_list() — the same list, created on first use.  0 only when
 * the create failed.
 * ------------------------------------------------------------------------- */
static gint64
bn_managed_list(TaskDatabase *db)
{
    gint64 found = bn_managed_find(db);
    return found != 0 ? found
                      : host->db->list_create(db, BN_LIST_NAME, BN_LIST_EMOJI);
}

/* ---------------------------------------------------------------------------
 * THE DEFAULT RULE — the row that catches everything no other rule does.
 *
 * It is an ORDINARY notes_rule row whose pattern is EMPTY, and that is
 * the whole mechanism: there is no flag column, no second table and no
 * separate setting.  It used to be one — the ini key `notes_embed_list`
 * behind a combo of its own — which meant the answer to "where does this
 * item go?" was written in two places, one of them not in the table that
 * claims to say.  The rules table is now the whole answer, read top to
 * bottom, ending in "Anything else".
 *
 * An empty pattern is not matched against: bn_rules_load drops it and
 * BnFiling.target carries it instead, which is the SAME answer by a
 * shorter route — bn_filing_dest already falls back to target when no
 * rule claims an item, and an empty pattern is exactly the rule that
 * claims everything.  (It would also be correct to leave it in: it
 * matches every text at length 0, so longest-match-wins would let every
 * real rule beat it.  Carrying it as target is one comparison fewer per
 * item and one guaranteed answer rather than a rule that could be
 * deleted.)
 *
 * There is exactly ONE such row, and the Settings table is what keeps
 * that true: the default row's pattern cell is not editable, so it
 * cannot be turned into an ordinary rule, and blanking an ordinary
 * rule's pattern DELETES it rather than leaving a second empty row.
 * ------------------------------------------------------------------------- */

/* bn_default_set() — point the default rule at `list_id`, creating the
 * row if this is the first time anything has asked.  One statement: the
 * subquery names the existing row when there is one, and NULL — which
 * sqlite auto-assigns — when there is not.                              */
static void
bn_default_set(TaskDatabase *db, gint64 list_id)
{
    gchar *sql = g_strdup_printf(
        "INSERT INTO notes_rule (id, pattern, list_id) VALUES ("
        "  (SELECT id FROM notes_rule WHERE pattern = '' ORDER BY id LIMIT 1),"
        "  '', %" G_GINT64_FORMAT ")"
        " ON CONFLICT(id) DO UPDATE SET list_id = excluded.list_id",
        list_id);
    host->db->exec(db, sql);
    g_free(sql);
}

/* ---------------------------------------------------------------------------
 * bn_default_list() — the list the default rule names: where an item no
 * rule claims is filed.
 *
 * Seeded with the managed "Action Items" list on first use, and RESEEDED
 * the same way when the list it named has gone — falling back rather
 * than stranding every unclaimed item in nothing.  Returns 0 only when
 * even the managed list could not be created.
 * ------------------------------------------------------------------------- */
static gint64
bn_default_list(TaskDatabase *db)
{
    gint64 id = host->db->scalar(db,
        "SELECT list_id FROM notes_rule WHERE pattern = '' ORDER BY id LIMIT 1");
    if (id > 0) {
        TaskList *l = host->db->list_get(db, id);
        if (l != NULL) {
            gboolean alive = !l->deleted;
            host->db->list_free(l);
            if (alive)
                return id;
        }
    }
    gint64 managed = bn_managed_list(db);
    if (managed != 0)
        bn_default_set(db, managed);
    return managed;
}

/* ===========================================================================
 * FILING RULES — "when the text contains X, file it in list Y".
 *
 * A rule is a plain SUBSTRING of an action item's text plus the list
 * that item is filed in; `notes_rule` holds them and the Notes section
 * of Settings is where they are written.  They are EXCEPTIONS to the
 * "Mirror action items into:" list, which stays the destination for
 * everything no rule claims.
 *
 * THE FIRST MATCH WINS, reading the table TOP DOWN, and matching STOPS
 * there.  Two rules can plainly both match one item — "scotia" and
 * "scotia bank" — so something has to decide, and the order the user
 * put them in is the answer they can SEE and CHANGE: Move Up / Move
 * Down in Settings, persisted in `notes_rule.position`.
 *
 * That ordering UI is not optional.  Precedence living anywhere but on
 * screen — insertion order, pattern length, id — leaves "why did this go
 * there?" unanswerable from the table that claims to say.  If the
 * buttons ever go, the rule has to go back to something the table
 * states by itself.
 *
 * Matching is `g_utf8_casefold`, the same rule the search box follows
 * (src/search.c) and NOT ASCII tolower, so a pattern with an accent in
 * it behaves like one without.  A rule matches ANYWHERE in the line,
 * because an action item reads "Scotia — Ian to update the config" and
 * the customer's name is not reliably at the front.
 *
 * A rule is INERT when its pattern is BLANK (a row in the table that has
 * not been typed into yet) or when the list it names is gone: both are
 * dropped by bn_rules_load, which keeps the matcher a pure comparison
 * and means a deleted list can never strand an item in nothing.  The
 * table in Settings shows such a rule as naming a deleted list rather
 * than hiding it, so it can be repointed or removed.
 * =========================================================================== */

typedef struct {
    gint64  list_id;                 /* where a match is filed             */
    gchar  *fold;                    /* casefolded pattern — matched as-is */
} BnRule;

/* bn_rule_free() — GDestroyNotify for the array below.                    */
static void
bn_rule_free(gpointer data)
{
    BnRule *r = data;
    g_free(r->fold);
    g_free(r);
}

/* bn_rule_row() — exec_query callback: one rule row, casefolded once
 * here so the matcher never folds a pattern per item.  An EMPTY pattern
 * is the DEFAULT RULE and is skipped here on purpose: BnFiling.target
 * carries it, and bn_filing_dest falls back to target when nothing
 * claims an item — which is the same answer without matching every
 * item against a pattern that cannot fail.                              */
static gint
bn_rule_row(gpointer data, gint n_cols, gchar **values, gchar **names)
{
    (void)names;
    GPtrArray *out = data;
    if (n_cols < 3 || values[1] == NULL || values[2] == NULL)
        return 0;
    if (*values[1] == '\0')
        return 0;                    /* inert: nothing typed in yet        */
    BnRule *r  = g_new0(BnRule, 1);
    r->list_id = g_ascii_strtoll(values[2], NULL, 10);
    r->fold    = g_utf8_casefold(values[1], -1);
    g_ptr_array_add(out, r);
    return 0;
}

/* ---------------------------------------------------------------------------
 * bn_rules_load() — the rules that can actually file something, IN THE
 * ORDER THE USER PUT THEM IN.
 *
 * That order is the precedence (first match wins), so this ORDER BY and
 * the Settings table's must agree — `id` breaks the tie for rows that
 * have never been moved, which is what leaves an untouched table reading
 * in the order it was typed.
 * ------------------------------------------------------------------------- */
static GPtrArray *
bn_rules_load(TaskDatabase *db)
{
    GPtrArray *rules = g_ptr_array_new_with_free_func(bn_rule_free);
    host->db->exec_query(db,
        "SELECT id, pattern, list_id FROM notes_rule"
        " ORDER BY position, id",
        bn_rule_row, rules);
    if (rules->len == 0)
        return rules;

    /* Liveness in ONE query rather than a list_get per rule — and NOT
     * from inside the callback above, which would run a statement on a
     * connection that is mid-query.  This drops rules naming a
     * TOMBSTONED list too, which is why the table carries no foreign
     * key: a tombstone is not a delete, so no cascade could see it.    */
    GHashTable *live = g_hash_table_new(g_direct_hash, g_direct_equal);
    GPtrArray  *lists = host->db->lists(db, FALSE);
    for (guint i = 0; i < lists->len; i++) {
        TaskList *l = g_ptr_array_index(lists, i);
        g_hash_table_add(live, GSIZE_TO_POINTER((gsize)l->id));
    }
    host->db->lists_free(lists);

    /* Backwards, so removing one does not shift an index not yet read.  */
    for (guint i = rules->len; i > 0; i--) {
        BnRule *r = g_ptr_array_index(rules, i - 1);
        if (!g_hash_table_contains(live, GSIZE_TO_POINTER((gsize)r->list_id)))
            g_ptr_array_remove_index(rules, i - 1);
    }
    g_hash_table_destroy(live);
    return rules;
}

/* ---------------------------------------------------------------------------
 * The filing decision in force: the default target list and the rules
 * that override it.  Loaded ONCE per pass — never per item — and the
 * only thing that answers "which list does this item belong in".
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64     target;               /* 0 = could not be resolved          */
    GPtrArray *rules;
} BnFiling;

/* bn_filing_load() — resolve both halves.  FALSE when the DEFAULT RULE
 * could not be resolved or seeded, which is the one state nothing can be
 * filed in; `f` is safe to clear either way.                             */
static gboolean
bn_filing_load(TaskDatabase *db, BnFiling *f)
{
    f->target = bn_default_list(db);
    f->rules  = bn_rules_load(db);
    return f->target != 0;
}

static void
bn_filing_clear(BnFiling *f)
{
    g_clear_pointer(&f->rules, g_ptr_array_unref);
    f->target = 0;
}

/* ---------------------------------------------------------------------------
 * bn_filing_dest() — the list an item whose text is `text` belongs in.
 * ------------------------------------------------------------------------- */
static gint64
bn_filing_dest(const BnFiling *f, const gchar *text)
{
    if (text == NULL || f->rules->len == 0)
        return f->target;

    gchar  *fold = g_utf8_casefold(text, -1);
    gint64  dest = f->target;
    /* TOP DOWN, and STOP at the first match: the array is in the user's
     * own order, so the rule that claims an item is the highest one that
     * could.                                                            */
    for (guint i = 0; i < f->rules->len; i++) {
        const BnRule *r = g_ptr_array_index(f->rules, i);
        if (strstr(fold, r->fold) != NULL) {
            dest = r->list_id;
            break;
        }
    }
    g_free(fold);
    return dest;
}

/* ---------------------------------------------------------------------------
 * bn_filing_stamp() — the whole filing decision as one comparable
 * string, for sync_state.bn_filing.
 *
 * bn_refile() re-files the items already mirrored only when this
 * CHANGES, which is what lets a per-task move made by hand stand:
 * nothing drags it back until the user touches the target list or a
 * rule.  So the RULES have to be in the stamp — a stamp of the target
 * alone would leave a newly written rule with nothing to trigger it,
 * and the rule would read as doing nothing until the next new item.
 * ------------------------------------------------------------------------- */
static gchar *
bn_filing_stamp(const BnFiling *f)
{
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, "t=%" G_GINT64_FORMAT, f->target);
    for (guint i = 0; i < f->rules->len; i++) {
        const BnRule *r = g_ptr_array_index(f->rules, i);
        g_string_append_printf(s, ";%" G_GINT64_FORMAT ":%s",
                               r->list_id, r->fold);
    }
    return g_string_free(s, FALSE);
}

/* ---------------------------------------------------------------------------
 * bn_file_tasks() — move each task to where the filing decision says it
 * belongs.  Returns how many actually moved.
 *
 * MAIN THREAD: ops->move_to_list writes through the app's own connection
 * and fires the moved hooks, which is how the Google copy follows a
 * cross-list move — a bare list_id update would strand it.  It declines
 * subtasks and no-op moves, so a task already in the right place costs
 * nothing and needs no test here.
 * ------------------------------------------------------------------------- */
static guint
bn_file_tasks(TaskApp *app, const BnFiling *f, GPtrArray *tasks)
{
    guint moved = 0;
    for (guint i = 0; i < tasks->len; i++) {
        Task *t = g_ptr_array_index(tasks, i);
        if (host->ops->move_to_list(app, t->id, bn_filing_dest(f, t->title)))
            moved++;
    }
    return moved;
}

/* ---------------------------------------------------------------------------
 * bn_refile() — apply a CHANGED filing decision to the items already
 * mirrored.
 *
 * The target list and the rules both name where matching items LIVE, not
 * merely where the next one lands: a setting that only affected new
 * items would read as doing nothing at all to someone who has just
 * pointed a rule at their existing work.
 *
 * Gated on the stamp, and that gate is the reason a per-task move made
 * BY HAND sticks: nothing re-files anything until the user changes the
 * target or a rule.  An ABSENT applied value counts as "not applied
 * yet" rather than "the same as now" — that is the upgrade case, where
 * items were mirrored by a build that only honored the setting at
 * creation time and are sitting in the wrong list.
 * ------------------------------------------------------------------------- */
static void
bn_refile(TaskApp *app)
{
    if (!host->config->get_bool(self, "sync", FALSE))
        return;
    TaskDatabase *db = host->db->main_db(app);

    BnFiling f;
    gboolean ok = bn_filing_load(db, &f);
    if (!ok) {
        bn_filing_clear(&f);
        return;                      /* the list could not be created      */
    }

    gchar    *now     = bn_filing_stamp(&f);
    gchar    *applied = host->db->state_get(db, "bn_filing");
    gboolean  same    = applied != NULL && g_strcmp0(applied, now) == 0;
    g_free(applied);
    if (same) {
        g_free(now);
        bn_filing_clear(&f);
        return;                      /* nothing changed — leave hand moves */
    }

    GPtrArray *mirror = bn_mirror_tasks(db);
    guint      moved  = bn_file_tasks(app, &f, mirror);
    host->db->tasks_free(mirror);

    host->db->state_set(db, "bn_filing", now);
    g_free(now);
    bn_filing_clear(&f);

    if (moved > 0) {
        bn_status(app, "Moved %u action item%s", moved,
                        moved == 1 ? "" : "s");
        host->notify->notify_changed(app);
    }
}

/* ---------------------------------------------------------------------------
 * bn_refile_ids() — re-file EXACTLY these mirror tasks, ungated.
 *
 * For the items Notes reworded during a pass: the text is what a rule
 * matches on, so a changed text can change where the item belongs.  Only
 * those items, because a sweep of the whole mirror would drag every
 * hand-moved task back with them — the property bn_refile's stamp exists
 * to protect.
 * ------------------------------------------------------------------------- */
static void
bn_refile_ids(TaskApp *app, const GArray *ids)
{
    TaskDatabase *db = host->db->main_db(app);

    BnFiling f;
    gboolean ok = bn_filing_load(db, &f);
    if (!ok) {
        bn_filing_clear(&f);
        return;
    }

    GPtrArray *tasks = g_ptr_array_new();
    for (guint i = 0; i < ids->len; i++) {
        Task *t = host->db->task_get(db, g_array_index(ids, gint64, i));
        if (t != NULL)
            g_ptr_array_add(tasks, t);
    }
    /* Silent: the pass's own summary already says what it changed, and
     * bn_apply refreshes right after this.                             */
    bn_file_tasks(app, &f, tasks);
    host->db->tasks_free(tasks);
    bn_filing_clear(&f);
}

/* ---------------------------------------------------------------------------
 * sync_item() — reconcile ONE listed action item with its mirror task.
 * Creates the task when it is new, otherwise pushes any cached local
 * done/due change and then applies whatever Notes holds.  Counts land
 * in `job`.
 *
 * `f` says which list a NEW item is filed in — a matching rule's, else
 * the default target.  An item Notes has REWORDED has its id parked in
 * job->refile instead of being moved here, because moving is main-thread
 * work and this runs on the worker.
 * ------------------------------------------------------------------------- */
static void
sync_item(BnJob *job, TaskDatabase *db, const TaskNoteAction *it,
          const BnFiling *f)
{
    Task *t = bn_task_for_uid(db, it->uid);

    if (t == NULL) {                 /* new item → new mirror task          */
        gint64 id = host->db->task_create(db, bn_filing_dest(f, it->text),
                                          0, it->text);
        if (id == 0) {
            job->n_failed++;         /* create failures must not be silent  */
            return;
        }
        host->db->task_apply_done_source(db, id, it->text, it->done, it->due);
        bn_set(db, id, it->uid, it->text, it->done, it->due);
        job->n_created++;
        return;
    }

    /* Notes has no third state, so the whole exchange speaks in the
     * DONE-ness of the status: a New ↔ In Progress move is not a
     * pending write and has nothing to push.                              */
    gboolean local_done = t->status == TASK_STATUS_DONE;

    /* The baseline: what Notes was last known to hold for this task.     */
    gchar   *base_have_text;         /* owned                               */
    gboolean base_have_done;
    gint64   base_have_due;
    bn_baseline(db, t->id, &base_have_text, &base_have_done, &base_have_due);

    /* The pending-write set: fields that drifted from the baseline since
     * the last successful push.  An UNKNOWN text baseline ("" — a task
     * bound before the column existed) is not a drift: there is nothing
     * to have drifted from, so Notes wins this once and the row gets a
     * real baseline below.                                                 */
    gboolean text_known = *base_have_text != '\0';
    gboolean text_dirty = text_known &&
                          g_strcmp0(t->title, base_have_text) != 0;
    gboolean done_dirty = local_done != base_have_done;
    gboolean due_dirty  = t->due  != base_have_due;
    gboolean text_sent  = FALSE;     /* did Notes accept the push?        */
    gboolean done_sent  = FALSE;
    gboolean due_sent   = FALSE;

    /* Text goes FIRST: `action text` keeps the item's done state and due
     * date, so a rename cannot undo a done/due push made after it, while
     * the reverse order would have the rename rewrite the line those two
     * had just touched.                                                    */
    if (text_dirty) {
        gchar *err = NULL;
        text_sent = task_notes_action_set_text(it->uid, t->title, &err);
        if (text_sent) job->n_pushed++; else job->n_failed++;
        g_free(err);
    }
    if (done_dirty) {
        gchar *err = NULL;
        done_sent = task_notes_action_set_done(it->uid, local_done, &err);
        if (done_sent) job->n_pushed++; else job->n_failed++;
        g_free(err);
    }
    if (due_dirty) {
        gchar *err = NULL;
        due_sent = task_notes_action_set_due(it->uid, t->due, &err);
        if (due_sent) job->n_pushed++; else job->n_failed++;
        g_free(err);
    }

    /* What the task should hold: a local change stands whether or not
     * the push landed (a refused push must never discard the user's
     * edit), otherwise Notes wins.                                        */
    const gchar *new_text = text_dirty ? t->title : it->text;
    gboolean new_done = done_dirty ? local_done : it->done;
    gint64   new_due  = due_dirty  ? t->due     : it->due;

    /* What Notes now holds: the pushed value only if it was accepted;
     * an unsent change keeps the old baseline so it is retried.
     *
     * A rename Notes ACCEPTED may still not read back verbatim: a text
     * ending in a parseable "due <date>" is taken by Notes as a due date
     * (the same rule as typing it into the note).  The baseline records
     * what was SENT, so the next pass sees the difference as a Notes-side
     * rewording and pulls it, rather than pushing the same text forever.  */
    const gchar *base_text = text_dirty
                           ? (text_sent ? t->title : base_have_text)
                           : it->text;
    gboolean base_done = done_dirty
                       ? (done_sent ? local_done : base_have_done)
                       : it->done;
    gint64   base_due  = due_dirty
                       ? (due_sent  ? t->due : base_have_due)
                       : it->due;

    /* A reworded item is the one change that can move a task between
     * lists, since the text is what a filing rule matches on — and only
     * a rewording NOTES made, which is what `new_text != t->title` says.
     * A rename the user just made HERE leaves the title untouched, so it
     * files nothing: they can see the task and put it where they meant.  */
    gboolean retitled = g_strcmp0(t->title, new_text) != 0;
    gboolean content  = retitled ||
                        new_done != local_done || new_due != t->due;

    if (content) {
        /* Stamps updated_at, so the change reaches Google too.             */
        host->db->task_apply_done_source(db, t->id, new_text, new_done,
                                       new_due);
        bn_set(db, t->id, it->uid, base_text, base_done, base_due);
        if (retitled)
            g_array_append_val(job->refile, t->id);
        job->n_updated++;
    } else if (g_strcmp0(base_text, base_have_text) != 0 ||
               base_done != base_have_done || base_due != base_have_due) {
        /* Nothing the user can see changed — only the push baseline —
         * so this must NOT stamp updated_at, or every pass would dirty
         * the row and buy a no-op Google PATCH.  This is also the arm a
         * pushed rename lands on, and the arm that gives a task bound
         * before the text baseline existed its first real one.             */
        bn_set(db, t->id, it->uid, base_text, base_done, base_due);
    }
    g_free(base_have_text);
    host->db->task_free(t);
}

/* ---------------------------------------------------------------------------
 * reap_missing() — tombstone mirror tasks whose item has left Notes.
 *
 * NOTES IS AUTHORITATIVE FOR EXISTENCE, and since 2026-09-08 that is the
 * WHOLE rule: what the listing holds is what the mirror holds.  There is
 * no suppression list any more — a mirror task deleted in Tasks comes
 * back on the next pass, because the item is still in Notes and Notes is
 * what says so.  Tick an action item off in Notes instead; the mirror is
 * not a place to tidy up.
 *
 * `present` holds every uid in the listing.  The listing is always FULL
 * — Notes has no incremental form — so absence really does mean gone,
 * unlike the Google pass where a partial listing makes absence
 * meaningless.
 *
 * EXCEPT when the listing is EMPTY.  That is not a hypothetical: a CLI
 * call is answered by whichever Notes instance owns the socket, not by
 * the binary on disk (gotcha 17), and a stale one answers `action list`
 * with NO ROWS AND EXIT 0 — indistinguishable, here, from "the user
 * deleted every action item".  Believing it tombstones every mirrored
 * task, and because a tombstone is what the Google sync pushes, those
 * deletes then propagate off this machine.  So an empty listing that
 * would reap ANYTHING is refused: the tasks are left exactly as they
 * are and the pass says so.
 *
 * This is the app's own "ABSENCE NEVER DELETES" rule, which the Google
 * sync already follows, and the same shape as the v8/v9 migrations —
 * a copy that does not verify drops nothing and reports.  The cost is
 * accepted deliberately: a Notes that HAS genuinely been emptied leaves
 * its mirrored tasks behind, and the user deletes them in Tasks.  That
 * direction is recoverable; the other is not.
 *
 * ------------------------------------------------------------------------- */
static void
reap_missing(BnJob *job, TaskDatabase *db, GHashTable *present)
{
    GPtrArray *mirror = bn_mirror_tasks(db);

    if (g_hash_table_size(present) == 0 && mirror->len > 0) {
        job->n_unreaped = (gint)mirror->len;
        /* Logged as well as reported: the status-bar line fades after a
         * few seconds, and "we declined to delete %u tasks" is the kind
         * of thing someone needs to find afterwards.                    */
        g_warning("notes: the listing was EMPTY \xe2\x80\x94 refusing to "
                  "reap %u mirrored task%s; nothing was deleted",
                  mirror->len, mirror->len == 1 ? "" : "s");
        host->db->tasks_free(mirror);
        return;
    }

    for (guint i = 0; i < mirror->len; i++) {
        Task *t = g_ptr_array_index(mirror, i);
        gint64 uid = bn_uid_of(db, t->id);
        if (g_hash_table_contains(present, GSIZE_TO_POINTER(uid)))
            continue;
        host->db->task_delete(db, t->id);
        job->n_removed++;
    }
    host->db->tasks_free(mirror);
}

/* ---------------------------------------------------------------------------
 * bn_apply() — main-thread completion: clear the guard, report, refresh.
 * ------------------------------------------------------------------------- */
static gboolean
bn_apply(gpointer data)
{
    BnJob *job = data;
    bn_running = FALSE;
    /* However the pass was started — timer, Sync Now, the toolbar — it
     * is over, so the button comes back.  Doing it here rather than in
     * the click handler is what makes a FAILED pass give it back too. */
    host->ui->tool_set_sensitive("notes-sync", TRUE);
    /* The moves come FIRST, so the notify below carries them and the
     * status line the pass wrote is the last thing said.  Main thread is
     * the whole reason they waited for this callback: ops->move_to_list
     * writes through the app's own connection.                           */
    if (job->refile->len > 0)
        bn_refile_ids(job->app, job->refile);
    if (job->message != NULL)
        bn_status(job->app, "%s", job->message);
    /* Structural: tasks appeared or vanished, and a first pass may have
     * created the list they live in.                                       */
    host->notify->notify_changed(job->app);
    g_array_free(job->refile, TRUE);
    g_free(job->db_path);
    g_free(job->message);
    g_free(job);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * bn_thread() — the worker: list, reconcile, reap.  Owns its SQLite
 * connection for the whole pass.
 * ------------------------------------------------------------------------- */
static gpointer
bn_thread(gpointer data)
{
    BnJob *job = data;
    GError *gerr = NULL;
    TaskDatabase *db = host->db->open(job->db_path, &gerr);
    if (db == NULL) {
        job->message = g_strdup_printf("Notes sync failed: %s",
                                       gerr != NULL ? gerr->message : "?");
        g_clear_error(&gerr);
        g_idle_add(bn_apply, job);
        return NULL;
    }

    gchar *err = NULL;
    GPtrArray *items = task_notes_actions(&err);
    if (items == NULL) {
        /* Tell "Notes is too old" apart from every other failure —
         * the fix is entirely different, and positional addressing is
         * NOT an acceptable fallback (it would bind tasks to whichever
         * item happens to sit at that position).                           */
        if (!task_notes_supports_uid())
            job->message = g_strdup("Notes is too old for action-item "
                                    "sync \xe2\x80\x94 update Notes");
        else
            job->message = g_strdup_printf("Notes sync failed: %s",
                                           err != NULL ? err : "unknown");
        g_free(err);
        host->db->close(db);
        g_idle_add(bn_apply, job);
        return NULL;
    }

    /* Loaded ONCE for the whole pass: the default list, and the rules
     * that send a matching item somewhere else.                          */
    BnFiling filing;
    if (!bn_filing_load(db, &filing)) {
        bn_filing_clear(&filing);
        job->message = g_strdup("Notes sync failed: cannot create the "
                                "Action Items list");
        task_notes_actions_free(items);
        host->db->close(db);
        g_idle_add(bn_apply, job);
        return NULL;
    }

    GHashTable *present = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (guint i = 0; i < items->len; i++) {
        TaskNoteAction *it = g_ptr_array_index(items, i);
        g_hash_table_add(present, GSIZE_TO_POINTER(it->uid));
    }
    /* EVERY listed item, with nothing skipped: the listing IS the set of
     * action items, so anything in it gets a task.                      */
    for (guint i = 0; i < items->len; i++)
        sync_item(job, db, g_ptr_array_index(items, i), &filing);
    bn_filing_clear(&filing);
    reap_missing(job, db, present);

    gchar *stamp = g_strdup_printf("%lld", (long long)time(NULL));
    host->db->state_set(db, "bn_last_sync", stamp);
    g_free(stamp);

    job->ok = job->n_failed == 0 && job->n_unreaped == 0;
    if (job->n_unreaped > 0) {
        /* Its OWN message, not a count folded in with the others: this
         * is the pass declining to do something, and it names the likely
         * cause because the fix is a restart of the other Notes rather
         * than anything in Tasks.                                        */
        job->message = g_strdup_printf(
            "Notes listed no action items \xe2\x80\x94 left %d mirrored "
            "task%s alone.  Is an old Notes still running?",
            job->n_unreaped, job->n_unreaped == 1 ? "" : "s");
    } else if (job->n_created == 0 && job->n_updated == 0 &&
               job->n_removed == 0 && job->n_pushed == 0 &&
               job->n_failed == 0) {
        job->message = g_strdup("Action items up to date");
    } else {
        GString *s = g_string_new("Action items:");
        if (job->n_created > 0)
            g_string_append_printf(s, " %d added,", job->n_created);
        if (job->n_updated > 0)
            g_string_append_printf(s, " %d updated,", job->n_updated);
        if (job->n_removed > 0)
            g_string_append_printf(s, " %d removed,", job->n_removed);
        if (job->n_pushed > 0)
            g_string_append_printf(s, " %d sent to Notes,", job->n_pushed);
        if (job->n_failed > 0)
            g_string_append_printf(s, " %d failed,", job->n_failed);
        if (s->len > 0 && s->str[s->len - 1] == ',')
            g_string_truncate(s, s->len - 1);
        job->message = g_string_free(s, FALSE);
    }

    g_hash_table_destroy(present);
    task_notes_actions_free(items);
    host->db->close(db);
    g_idle_add(bn_apply, job);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * bn_start() — kick off one mirror pass on a worker thread.
 *
 * Two early-outs: a pass already in flight (silent — the guard is there
 * to stop a tick piling onto a slow CLI round trip), and the integration
 * switched off in Settings, which says so.  Main thread only.
 * ------------------------------------------------------------------------- */
static void
bn_start(TaskApp *app, const gchar *db_path)
{
    if (bn_running)
        return;                      /* silent: a pass is already running   */
    if (!host->config->get_bool(self, "sync", FALSE)) {
        bn_status(app, "Notes integration is off");
        return;
    }

    BnJob *job = g_new0(BnJob, 1);
    job->app       = app;
    job->db_path   = g_strdup(db_path);
    job->refile    = g_array_new(FALSE, FALSE, sizeof(gint64));

    bn_running = TRUE;
    bn_status(app, "Syncing action items\xe2\x80\xa6");
    GThread *th = g_thread_new("task-bnsync", bn_thread, job);
    g_thread_unref(th);
}

/* ---------------------------------------------------------------------------
 * bn_sync_now() — mirror the action items now, by hand.
 *
 * The toolbar button AND the Notes menu's Sync Now both land here:
 * TaskUiToolDef and TaskUiMenuDef take the same callback shape, which is
 * what lets one function be the single answer to "sync now" rather than
 * two that can drift.  It greys the button either way — the menu started
 * the same pass, and a button left live during it would invite a second
 * press that bn_start would silently drop.
 *
 * The mirror being switched OFF is reported rather than ignored: the
 * button is hidden in that state, but the MENU item is not (see below),
 * so this is a reachable press with nothing to do.
 * ------------------------------------------------------------------------- */
static void
bn_sync_now(TaskApp *app, gpointer user_data)
{
    (void)user_data;
    if (!host->config->get_bool(self, "sync", FALSE)) {
        bn_status(app, "Notes action items are not being mirrored "
                       "\xe2\x80\x94 switch it on in File \xe2\x86\x92 "
                       "Settings\xe2\x80\xa6");
        return;
    }
    host->ui->tool_set_sensitive("notes-sync", FALSE);
    bn_start(app, app->db != NULL ? app->db->path : NULL);
}

/* Shown only when the mirror is ON and the user wants the button.
 * Re-asked on every full refresh, so flipping either setting is enough. */
static gboolean
bn_toolbar_visible(TaskApp *app, gpointer user_data)
{
    (void)app;
    (void)user_data;
    return host->config->get_bool(self, "sync", FALSE) &&
           host->config->get_bool(self, "toolbar_button", TRUE);
}

/* The icon is composition.png in icons/ beside the binary — `icon` names
 * the file WITHOUT its extension (task_ui.h).  The fallback glyph is the
 * memo, not a sync arrow: it is what the button shows when the PNG is
 * missing, and it should still say WHOSE sync this is — the Google
 * button beside it already wears the round arrow.
 *
 * `sort` 5 puts it LEFT of Google's 10, the order the two passes
 * actually run in (the mirror is worker sort -10, ahead of the sync), so
 * the toolbar reads the way one press of each would work.               */
static const TaskUiToolDef bn_sync_tool = {
    .id              = "notes-sync",
    .icon            = "composition",
    .fallback_markup = "\xf0\x9f\x93\x9d",     /* 📝                       */
    .label           = "Sync Notes",
    .tooltip         = "Mirror Notes action items now",
    .sort            = 5,
    .clicked         = bn_sync_now,
    .visible         = bn_toolbar_visible,
};

/* Notes -> Sync Now: the menu twin of the button, in a top-level menu of
 * this plugin's own (TASK_UI_MENU_OWN, see task_ui.h).  NOT gated on
 * `toolbar_button` the way the button is — that setting is about the
 * TOOLBAR, and someone who turned the button off to reclaim the space
 * still needs a way to sync by hand.  The menu is the one that always
 * exists, which is why its label can be the plain verb: the menu it sits
 * in already says Notes.                                                 */
static const TaskUiMenuDef bn_sync_menu_item = {
    .id         = "notes-sync-now",
    .menu       = TASK_UI_MENU_OWN,
    .menu_title = "Notes",
    .label      = "Sync Now",
    .sort       = 5,
    .activate   = bn_sync_now,
};

/* ---------------------------------------------------------------------------
 * Periodic mirror pass — the scheduler drives it (see task_worker.h).
 * ------------------------------------------------------------------------- */

/* bn_run() — start one pass.                                              */
static void
bn_run(TaskApp *app, const gchar *db_path)
{
    bn_start(app, db_path);
}

/* bn_on_arm() — before the timer goes in: a target list or a filing rule
 * changed while the mirror was switched off (or by an earlier build that
 * only honored the setting at creation time) still has to reach the
 * items already mirrored.                                                 */
static void
bn_on_arm(TaskApp *app)
{
    bn_refile(app);
}

static const TaskWorkerDef bn_worker = {
    .id               = "notes",
    /* Before the Google sync (which takes the default 0): a new action
     * item is mirrored and then pushed on to Google by ONE press of
     * Sync.  Stated here rather than left to plugin load order, which is
     * whatever the loader's directory read happened to hand back.      */
    .sort             = -10,
    .enabled_key      = "notes_sync",
    .enabled_default  = FALSE,
    .interval_key     = "notes_sync_interval_min",
    .interval_default = 5,
    /* ALWAYS, not ARMED: at interval 0 the user asked for manual passes,
     * but the All Action Items view is EMPTY until one has run — so "manual
     * only" still has to mean "populate it now".                          */
    .initial          = TASK_WORKER_INITIAL_ALWAYS,
    .running          = NULL,        /* completed by task_bnsync_init       */
    .timer            = NULL,
    .run              = bn_run,
    .ready            = NULL,        /* the CLI is always worth asking      */
    .on_arm           = bn_on_arm,
};

static TaskWorkerDef bn_worker_live;

/* ---------------------------------------------------------------------------
 * bn_auto_start() — (re)arm the mirror timer from the interval setting
 * (default 5 minutes; 0 = only when Sync is pressed).
 * ------------------------------------------------------------------------- */
static void
bn_auto_start(TaskApp *app, const gchar *db_path)
{
    host->worker->arm(app, &bn_worker_live, db_path);
}

/* ---------------------------------------------------------------------------
 * The "All Action Items" sidebar view.
 *
 * A FILTERED view over every mirrored task, wherever each one actually
 * lives — not a list of its own.  That is why virtual_rows is TRUE: each
 * row keeps its "in <list>" line, which is the only thing that says
 * where the task really sits.
 *
 * ALL, because "Action Items" is also the name of the LIST the default
 * filing rule sends items to, and the two are different things: the list
 * is where unclaimed items are FILED, this view is every mirrored item
 * wherever a rule put it.  Two rows reading the same would be a puzzle,
 * and the meta rows it sits among ("All Tasks") already say it this
 * way.
 * ------------------------------------------------------------------------- */
static gboolean
bn_view_visible(TaskApp *app, gpointer d)
{
    (void)app;
    (void)d;
    return host->config->get_bool(self, "sync", FALSE) &&
           host->config->get_bool(self, "meta_row", TRUE);
}

static GPtrArray *
bn_view_query(TaskApp *app, gpointer d)
{
    (void)d;
    return bn_mirror_tasks(host->db->main_db(app));
}

/* The id is "bn_actions" because that is what manual_order_bn_actions and
 * kanban_order_bn_actions already say in users' ini files — the order
 * keys are derived from it (see task_view.h).                             */
static const TaskView bn_view = {
    .id           = "bn_actions",
    .label        = "\xe2\x9d\x97\xef\xb8\x8f  All Action Items",
    .name         = "All Action Items",
    .unit         = "action item",
    .sort         = 30,
    .visible      = bn_view_visible,
    .query        = bn_view_query,
    .virtual_rows = TRUE,
    .not_a_list   = "All Action Items is a view, not a list \xe2\x80\x94 "
                    "hide it in File \xe2\x86\x92 Settings\xe2\x80\xa6",
};

/* ---------------------------------------------------------------------------
 * The ❗ glyph on a mirrored task's row.
 *
 * It used to be a hard-coded `t->bn_uid != 0` test inside the row
 * renderer — the renderer knowing what a Notes item was.  It is now a
 * registered decoration (task_rows.h), collected in ONE query per
 * refresh rather than asked per row.
 *
 * It sorts below the app's own glyphs (favourite 100, priority 200) so it
 * lands INNERMOST, nearest the title: it describes what the row IS, not
 * how the user has flagged it.  The full stack reads ↳ 🚨 ⭐️ ❗ Title.
 * ------------------------------------------------------------------------- */
static GHashTable *
bn_decor_collect(TaskApp *app, gpointer user_data)
{
    (void)user_data;
    if (!host->config->get_bool(self, "sync", FALSE))
        return NULL;                 /* integration off: nothing to mark   */

    GHashTable *set = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, NULL);
    GPtrArray *mirror = bn_mirror_tasks(host->db->main_db(app));
    for (guint i = 0; i < mirror->len; i++) {
        Task *t = g_ptr_array_index(mirror, i);
        gint64 *k = g_new(gint64, 1);
        *k = t->id;
        g_hash_table_add(set, k);
    }
    host->db->tasks_free(mirror);
    return set;
}

static const TaskRowDecorDef bn_decor = {
    .id      = "notes-action-item",
    .sort    = 50,                   /* inside favourite (100)             */
    .collect = bn_decor_collect,
    .prefix  = "\xe2\x9d\x97  ",       /* ❗                               */
};

/* ===========================================================================
 * The Notes section of the Settings window.
 *
 * Contributed through host->settings->add_section() rather than written
 * into settings_window.c, for the reason every other integration setting
 * is: the window should not know what a Notes action item is.  The
 * mirror owns its own controls, so switching it off is a matter of not
 * registering this.
 *
 * The builder runs afresh every time Settings is opened, against a
 * window that is destroyed each time — so nothing here is remembered
 * between calls.  Widget values are set BEFORE the handlers are
 * connected, which is what removes the need for the window's own
 * `loading` guard: a set that happens before a connect cannot fire one.
 * =========================================================================== */

/* bn_list_label() — a list as the app spells it: the emoji, TWO spaces,
 * then the name.  Used by the rules table's list picker and by the
 * labels it displays, so the same list cannot read two ways in one
 * window.                                                                */
static gchar *
bn_list_label(const TaskList *l)
{
    return *l->emoji != '\0'
        ? g_strdup_printf("%s  %s", l->emoji, l->name)
        : g_strdup(l->name);
}

/* bn_settings_db_path() — the path the mirror's worker should be armed
 * on.  Read from the LIVE connection rather than remembered: a database
 * switch replaces it, and a handler holding the old one would arm a
 * worker on a file that has just been removed.                            */
static const gchar *
bn_settings_db_path(TaskApp *app)
{
    return app->db != NULL ? app->db->path : NULL;
}

/* on_bn_toggled() — the mirror's master switch: persist, then re-arm the
 * timer.  Switching ON runs a pass immediately (that is what populates
 * the All Action Items view); switching OFF stops the timer.             */
static void
on_bn_toggled(GtkWidget *w, gpointer data)
{
    TaskApp *app = data;
    host->config->set(self, "sync",
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)) ? "1" : "0");
    bn_auto_start(app, bn_settings_db_path(app));
    host->notify->notify_changed(app);
}

/* on_bn_interval_changed() — write-through + re-arm the mirror timer.     */
static void
on_bn_interval_changed(GtkWidget *w, gpointer data)
{
    TaskApp *app = data;
    gchar *v = g_strdup_printf("%d",
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w)));
    host->config->set(self, "sync_interval_min", v);
    g_free(v);
    bn_auto_start(app, bn_settings_db_path(app));
}

/* on_bn_toolbar_toggled() — show/hide the toolbar's Sync Notes button.
 * The menu item is deliberately unaffected: this setting is about the
 * TOOLBAR, not about whether syncing by hand is possible.               */
static void
on_bn_toolbar_toggled(GtkWidget *w, gpointer data)
{
    TaskApp *app = data;
    host->config->set(self, "toolbar_button",
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)) ? "1" : "0");
    host->notify->notify_changed(app);
}

/* on_bn_meta_toggled() — show/hide the sidebar's All Action Items view.  */
static void
on_bn_meta_toggled(GtkWidget *w, gpointer data)
{
    TaskApp *app = data;
    host->config->set(self, "meta_row",
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)) ? "1" : "0");
    host->notify->notify_changed(app);
}

/* on_bn_cli_changed() — the CLI path entry: persist ONLY.  The mirror
 * pass happens on commit (focus-out/Enter) — running it per keystroke
 * would spawn the half-typed command over and over.                       */
static void
on_bn_cli_changed(GtkWidget *w, gpointer data)
{
    (void)data;
    const gchar *cli = gtk_entry_get_text(GTK_ENTRY(w));
    host->config->set(self, "cli", *cli != '\0' ? cli : NULL);
}

/* on_bn_cli_commit() — Enter in the CLI path entry: run a pass against
 * the newly named binary so a wrong path reports itself now rather than
 * at the next tick.                                                       */
static void
on_bn_cli_commit(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskApp *app = data;
    if (host->config->get_bool(self, "sync", FALSE))
        bn_start(app, bn_settings_db_path(app));
    host->notify->notify_changed(app);
}

/* on_bn_cli_focus_out() — leaving the CLI path entry: commit now.         */
static gboolean
on_bn_cli_focus_out(GtkWidget *w, GdkEventFocus *event, gpointer data)
{
    (void)event;
    on_bn_cli_commit(w, data);
    return FALSE;                    /* propagate                           */
}

/* The entry the Browse button fills in, hung off the button: the handler
 * needs both it and the app, and the window they live in is rebuilt on
 * every opening (the same reason BN_RULE_UI lives on its tree view).      */
#define BN_CLI_ENTRY "bn-cli-entry"

/* ---------------------------------------------------------------------------
 * on_bn_cli_browse() — pick the Notes program in a file chooser.
 *
 * It STARTS where the answer probably is: the path already typed, else
 * whatever `notes` resolves to on PATH — which is the first line
 * `which -a notes` would print, and the reason typing or pasting a path
 * has to keep working alongside this button rather than being replaced
 * by it.
 *
 * NO file filter.  There is no reliable "is executable" pattern to
 * filter on, and a filter that hides the very file you came for is worse
 * than none.
 *
 * Filling the entry goes through its own "changed" and then the SAME
 * commit Enter uses, so choosing a binary persists it and runs a pass
 * against it — a wrong pick reports itself now rather than at the next
 * tick, and there is one spelling of "the path changed".
 * ------------------------------------------------------------------------- */
static void
on_bn_cli_browse(GtkWidget *button, gpointer data)
{
    TaskApp   *app   = data;
    GtkWidget *entry = g_object_get_data(G_OBJECT(button), BN_CLI_ENTRY);
    if (entry == NULL)
        return;

    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Choose Notes Binary",
        GTK_WINDOW(gtk_widget_get_toplevel(button)),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Select", GTK_RESPONSE_ACCEPT,
        NULL);

    const gchar *cur = gtk_entry_get_text(GTK_ENTRY(entry));
    if (g_path_is_absolute(cur)) {
        /* set_filename answers FALSE for a path that is not there — the
         * binary MOVED, which is the usual reason to be in this dialog —
         * so fall back to the folder it used to be in rather than
         * opening wherever GTK last was.                               */
        if (!gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser), cur)) {
            gchar *dir = g_path_get_dirname(cur);
            gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser),
                                                dir);
            g_free(dir);
        }
    } else {
        gchar *found = g_find_program_in_path(*cur != '\0' ? cur : "notes");
        if (found != NULL)
            gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser), found);
        g_free(found);
    }

    gchar *path = NULL;
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
        path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    gtk_widget_destroy(chooser);
    if (path == NULL)
        return;                      /* cancelled — leave the entry alone  */

    gtk_entry_set_text(GTK_ENTRY(entry), path);
    g_free(path);
    on_bn_cli_commit(entry, app);
}

/* ===========================================================================
 * The filing-rules table.
 *
 * A two-column table of "when the text contains X, file it in Y" with an
 * Add and a Remove button under it.  Both cells are edited IN PLACE:
 * fixing a typo in a pattern by deleting the rule and typing it again is
 * the kind of thing a table is supposed to spare you.
 *
 * THE LAST ROW IS THE DEFAULT RULE — "Anything else" — and it is the
 * reason this table is the whole answer rather than half of one.  Its
 * pattern cell is NOT editable (there is exactly one default rule, and
 * an ordinary rule must not be able to become a second) while its list
 * cell is, because choosing that list is the entire point of the row.
 * It cannot be removed either: something has to catch what no rule
 * claims.
 *
 * A rule that has not been SEEDED yet — the default row before the
 * mirror has ever run — is shown with RC_ID 0 and written on first
 * touch.  That is what keeps opening Settings from creating an "Action
 * Items" list for someone who only came to look.
 *
 * The rules live in the DATABASE (see notes_db_open), so every handler
 * here writes a row and then calls bn_refile() — the rule has to reach
 * the items already mirrored, or it reads as doing nothing until the next
 * new action item turns up.
 * =========================================================================== */

/* The rules store's columns.  Column 0 is a gint64 id, which is exactly
 * the trap gtk_tree_view_set_enable_search guards against: the
 * type-ahead search column would be that id and would match nothing a
 * user could type (gotcha 5).                                            */
enum { RC_ID, RC_PATTERN, RC_LIST_LABEL, RC_LIST_ID, RC_DEFAULT, RC_N };

/* The help behind the "Rules" heading's INFORMATION glyph.  It was a
 * wrapped paragraph in the section until 2026-09-08; a heading plus a
 * tooltip says the same thing without spending six lines of the Settings
 * column on it every time the window opens.
 *
 * It hangs off the GLYPH ALONE — not the heading, not the table.  A
 * tooltip on the table would fire wherever the pointer rested while
 * someone was reading a rule or reaching for a cell, which is the
 * opposite of help; a marker you can choose to hover is one you can also
 * ignore.
 *
 * The literal BREAKS after each \x9c on purpose: a C hex escape is
 * GREEDY, so \x9c followed by a hex digit ("a", "A") is read as one
 * out-of-range escape and the file will not compile (gotcha 25).       */
#define BN_RULES_HELP \
    "Filing rules decide which list each action item lands in. An " \
    "item whose text contains a rule's text is filed in that rule's " \
    "list; case is ignored and the text can sit anywhere in the line, " \
    "so \xe2\x80\x9c" "apple\xe2\x80\x9d claims \xe2\x80\x9c" "Applesauce " \
    "\xe2\x80\x94 Ian to pick a bucketful of apples \xe2\x80\x9d. When two rules " \
    "match, the one HIGHER UP wins \xe2\x80\x94 use Move Up and Move " \
    "Down to say which. Anything no rule claims goes where the last row " \
    "says. Changing a rule moves the items already mirrored too."

/* What the first column shows for the default rule, and what the second
 * shows for a default rule not yet seeded (its list is created when the
 * mirror first runs, so naming it here would be a promise, not a fact). */
#define BN_RULE_ANY      "Anything else"
#define BN_RULE_PENDING  "Action Items (created on the first sync)"

/* The list-picker model behind the second column's combo.                */
enum { LC_LABEL, LC_ID, LC_N };

/* What the second column shows for a rule whose list has gone.  Such a
 * rule files NOTHING (bn_rules_load drops it), and saying so is the only
 * way the user can tell it apart from one that works.                    */
#define BN_RULE_NO_LIST "(list deleted \xe2\x80\x94 pick another)"

/* Everything the rule handlers need, hung off the tree view so it dies
 * WITH it: the Settings window is destroyed and rebuilt on every
 * opening, so a file-scope copy would have to be kept in step by hand —
 * the same reason the row context menus self-destroy on selection-done. */
#define BN_RULE_UI "bn-rule-ui"

typedef struct {
    TaskApp      *app;
    GtkTreeView  *view;
    GtkListStore *store;             /* the rules, RC_*                    */
    GtkListStore *lists;             /* the live lists, LC_*               */
    GtkWidget    *remove;            /* all three grey with the selection  */
    GtkWidget    *up;
    GtkWidget    *down;
} BnRuleUi;

/* bn_rule_ui_free() — GDestroyNotify for the above.                       */
static void
bn_rule_ui_free(gpointer data)
{
    BnRuleUi *ui = data;
    g_clear_object(&ui->store);
    g_clear_object(&ui->lists);
    g_free(ui);
}

/* bn_rule_label_for() — how to show the list a rule names.  Looked up in
 * the picker's own model, which holds only LIVE lists, so anything else
 * is a list that has gone.                                               */
static gchar *
bn_rule_label_for(BnRuleUi *ui, gint64 list_id)
{
    GtkTreeIter iter;
    gboolean    have = gtk_tree_model_get_iter_first(
                           GTK_TREE_MODEL(ui->lists), &iter);
    while (have) {
        gint64 id = 0;
        gtk_tree_model_get(GTK_TREE_MODEL(ui->lists), &iter, LC_ID, &id, -1);
        if (id == list_id) {
            gchar *label = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(ui->lists), &iter,
                               LC_LABEL, &label, -1);
            return label;
        }
        have = gtk_tree_model_iter_next(GTK_TREE_MODEL(ui->lists), &iter);
    }
    return g_strdup(BN_RULE_NO_LIST);
}

/* bn_rule_ui_add_row() — one row into the store, default or ordinary.
 * `pattern` empty means the DEFAULT rule, which shows its own wording in
 * the first column instead.                                             */
static void
bn_rule_ui_add_row(BnRuleUi *ui, gint64 id, const gchar *pattern,
                   gint64 list_id)
{
    gboolean is_default = pattern == NULL || *pattern == '\0';
    gchar   *label      = list_id > 0 ? bn_rule_label_for(ui, list_id)
                                      : g_strdup(BN_RULE_PENDING);
    gtk_list_store_insert_with_values(ui->store, NULL, -1,
        RC_ID,         id,
        RC_PATTERN,    is_default ? BN_RULE_ANY : pattern,
        RC_LIST_LABEL, label,
        RC_LIST_ID,    list_id,
        RC_DEFAULT,    is_default,
        -1);
    g_free(label);
}

/* bn_rule_ui_row() — exec_query callback: one stored rule.               */
static gint
bn_rule_ui_row(gpointer data, gint n_cols, gchar **values, gchar **names)
{
    (void)names;
    BnRuleUi *ui = data;
    if (n_cols < 3 || values[0] == NULL)
        return 0;
    bn_rule_ui_add_row(ui, g_ascii_strtoll(values[0], NULL, 10), values[1],
                       values[2] != NULL
                           ? g_ascii_strtoll(values[2], NULL, 10) : 0);
    return 0;
}

/* bn_rule_ui_reload() — rebuild the table.
 *
 * Ordinary rules first IN THE USER'S OWN ORDER — which is the
 * precedence, first match winning (see the FILING RULES block above) —
 * then the default rule.  It is LAST because that is how the table reads
 * as a sentence: these texts go here, and anything else goes there.  The
 * ORDER BY must match bn_rules_load's, or the table would show an order
 * the mirror does not use.
 *
 * The default row is SYNTHESISED when nothing has seeded it yet, naming
 * the managed list only if it already exists.  Seeding it here instead
 * would create an "Action Items" list for anyone who merely opened
 * Settings.                                                             */
static void
bn_rule_ui_reload(BnRuleUi *ui)
{
    TaskDatabase *db = host->db->main_db(ui->app);
    gtk_list_store_clear(ui->store);
    host->db->exec_query(db,
        "SELECT id, pattern, list_id FROM notes_rule"
        " ORDER BY (pattern = ''), position, id",
        bn_rule_ui_row, ui);

    if (host->db->scalar(db,
            "SELECT count(*) FROM notes_rule WHERE pattern = ''") <= 0)
        bn_rule_ui_add_row(ui, 0, "", bn_managed_find(db));
}

/* ---------------------------------------------------------------------------
 * bn_rule_positions_save() — write `position` from the table's CURRENT
 * order, 1..N over the ordinary rules.
 *
 * Renumbering all of them rather than swapping two values is what
 * NORMALISES the column: rows that predate it all sit on 0, where a swap
 * of 0 and 0 would move nothing.  N is a handful, so the loop is cheaper
 * to read than a CASE would be.  The default rule is skipped — it is not
 * in the ordering at all — and so is a row not yet written.
 * ------------------------------------------------------------------------- */
static void
bn_rule_positions_save(BnRuleUi *ui)
{
    TaskDatabase *db   = host->db->main_db(ui->app);
    GtkTreeIter   iter;
    gboolean      have = gtk_tree_model_get_iter_first(
                             GTK_TREE_MODEL(ui->store), &iter);
    gint          n    = 0;
    while (have) {
        gint64   id  = 0;
        gboolean def = FALSE;
        gtk_tree_model_get(GTK_TREE_MODEL(ui->store), &iter,
                           RC_ID, &id, RC_DEFAULT, &def, -1);
        if (id > 0 && !def) {
            gchar *sql = g_strdup_printf(
                "UPDATE notes_rule SET position = %d WHERE id = %"
                G_GINT64_FORMAT, ++n, id);
            if (!host->db->exec(db, sql))
                bn_status(ui->app, "Could not save the rule order");
            g_free(sql);
        }
        have = gtk_tree_model_iter_next(GTK_TREE_MODEL(ui->store), &iter);
    }
}

/* bn_rule_id_at() — the notes_rule id the store row `iter` stands for.    */
static gint64
bn_rule_id_at(BnRuleUi *ui, GtkTreeIter *iter)
{
    gint64 id = 0;
    gtk_tree_model_get(GTK_TREE_MODEL(ui->store), iter, RC_ID, &id, -1);
    return id;
}

/* on_rule_pattern_edited() — the text a rule looks for.
 *
 * Persisted VERBATIM: not stripped, because leading or trailing space is
 * a legitimate (if broad) thing to match on.  An EMPTY one is the one
 * text an ordinary rule cannot have — that spelling belongs to the
 * default rule — so emptying a rule DELETES it, and a row added but
 * never typed into was never written at all (RC_ID 0).  Both keep the
 * invariant that exactly one empty-pattern row exists.
 *
 * THE DEFAULT ROW IS EDITABLE and this is where that lands: typing a
 * pattern into "Anything else" turns THAT row into an ordinary rule
 * (last in the order, where it already sat) and seeds a fresh default
 * INHERITING ITS LIST — so the destination the user chose is carried
 * over rather than silently reset to the managed list.  Emptying it
 * changes nothing: it is still the row that catches the rest.          */
static void
on_rule_pattern_edited(GtkCellRendererText *cell, gchar *path_str,
                       gchar *new_text, gpointer data)
{
    (void)cell;
    BnRuleUi     *ui = data;
    TaskDatabase *db = host->db->main_db(ui->app);
    GtkTreeIter   iter;
    if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(ui->store),
                                             &iter, path_str))
        return;

    gint64   id    = bn_rule_id_at(ui, &iter);
    gboolean blank = new_text == NULL || *new_text == '\0';
    gboolean def   = FALSE;
    gint64   list_id = 0;
    gtk_tree_model_get(GTK_TREE_MODEL(ui->store), &iter,
                       RC_DEFAULT, &def, RC_LIST_ID, &list_id, -1);

    if (blank && def)
        return;                      /* still the row that catches the rest */
    if (blank && id == 0) {          /* added, then left empty              */
        gtk_list_store_remove(ui->store, &iter);
        return;                      /* nothing was ever written            */
    }

    /* A row whose list is still unresolved — the default before anything
     * seeded it — needs one now that it is becoming a real rule.  This is
     * the ONE place Settings creates the managed list, and only because
     * the user has just written a rule that has to file somewhere.       */
    if (list_id <= 0)
        list_id = bn_managed_list(db);

    gchar *q   = host->db->quote(blank ? "" : new_text);
    gchar *sql;
    if (blank) {
        sql = g_strdup_printf(
            "DELETE FROM notes_rule WHERE id = %" G_GINT64_FORMAT, id);
    } else if (id == 0) {            /* the row is real as of now           */
        sql = g_strdup_printf(
            "INSERT INTO notes_rule (pattern, list_id, position)"
            " VALUES (%s, %" G_GINT64_FORMAT ","
            "  (SELECT COALESCE(MAX(position), 0) + 1 FROM notes_rule))",
            q, list_id);
    } else {
        /* Converting the default carries it to the END of the ordinary
         * rules, which is where it already appeared to be.               */
        sql = def
            ? g_strdup_printf(
                  "UPDATE notes_rule SET pattern = %s, position ="
                  "  (SELECT COALESCE(MAX(position), 0) + 1 FROM notes_rule)"
                  " WHERE id = %" G_GINT64_FORMAT, q, id)
            : g_strdup_printf(
                  "UPDATE notes_rule SET pattern = %s WHERE id = %"
                  G_GINT64_FORMAT, q, id);
    }
    if (!host->db->exec(db, sql))
        bn_status(ui->app, "Could not save the rule");
    g_free(sql);
    g_free(q);

    if (def)                         /* something must still catch the rest */
        bn_default_set(db, list_id);

    bn_rule_ui_reload(ui);           /* ids and order both move             */
    bn_refile(ui->app);
}

/* ---------------------------------------------------------------------------
 * on_rule_editing_started() — the default row's editor opens EMPTY.
 *
 * "Anything else" is a description of the row, not text anyone typed, so
 * offering it for editing would have the user clear it before writing a
 * pattern.  A placeholder says what typing here would mean, which is the
 * only hint that the row can become an ordinary rule at all.
 * ------------------------------------------------------------------------- */
static void
on_rule_editing_started(GtkCellRenderer *cell, GtkCellEditable *editable,
                        const gchar *path_str, gpointer data)
{
    (void)cell;
    BnRuleUi   *ui = data;
    GtkTreeIter iter;
    if (!GTK_IS_ENTRY(editable) ||
        !gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(ui->store),
                                             &iter, path_str))
        return;
    gboolean def = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(ui->store), &iter, RC_DEFAULT, &def, -1);
    if (!def)
        return;
    gtk_entry_set_text(GTK_ENTRY(editable), "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(editable),
        "text to match \xe2\x80\x94 leave empty to keep catching the rest");
}

/* on_rule_list_changed() — which list a rule files its matches into.
 *
 * The combo hands back an ITER into the picker's model, so the id comes
 * off the row the user actually clicked rather than being looked up by
 * its label — two lists may carry the same name, and a lookup by label
 * would file into whichever of them came first.                          */
static void
on_rule_list_changed(GtkCellRendererCombo *cell, gchar *path_str,
                     GtkTreeIter *new_iter, gpointer data)
{
    (void)cell;
    BnRuleUi   *ui = data;
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(ui->store),
                                             &iter, path_str))
        return;

    gchar  *label   = NULL;
    gint64  list_id = 0;
    gtk_tree_model_get(GTK_TREE_MODEL(ui->lists), new_iter,
                       LC_LABEL, &label, LC_ID, &list_id, -1);

    gint64   id  = bn_rule_id_at(ui, &iter);
    gboolean def = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(ui->store), &iter, RC_DEFAULT, &def, -1);

    if (def) {
        /* Writes the default rule whether or not it existed — this is
         * the one place a user can seed it by hand, and the same call
         * the mirror makes on its first pass.                          */
        bn_default_set(host->db->main_db(ui->app), list_id);
        bn_rule_ui_reload(ui);       /* it has an id now                 */
    } else if (id == 0) {
        /* A row added but not yet typed into has no database row to
         * update; the pattern handler carries this choice into the
         * INSERT it makes.                                             */
        gtk_list_store_set(ui->store, &iter, RC_LIST_LABEL, label,
                           RC_LIST_ID, list_id, -1);
    } else {
        gchar *sql = g_strdup_printf(
            "UPDATE notes_rule SET list_id = %" G_GINT64_FORMAT
            " WHERE id = %" G_GINT64_FORMAT, list_id, id);
        if (!host->db->exec(host->db->main_db(ui->app), sql))
            bn_status(ui->app, "Could not save the rule");
        g_free(sql);
        gtk_list_store_set(ui->store, &iter, RC_LIST_LABEL, label,
                           RC_LIST_ID, list_id, -1);
    }
    g_free(label);
    bn_refile(ui->app);
}

/* ---------------------------------------------------------------------------
 * on_rule_add() — a new rule.
 *
 * The row goes into the STORE ONLY, above the default row, and is written
 * when its pattern is typed (on_rule_pattern_edited).  An empty pattern
 * is the default rule's own spelling, so a placeholder row in the
 * database would be a SECOND one — and a fabricated pattern like "text to
 * match" would be data nobody asked for.  A rule you did not finish
 * writing was simply never created.
 *
 * The cursor goes straight into its text cell: a blank row appearing with
 * nothing to type in reads as the button having failed.
 * ------------------------------------------------------------------------- */
static void
on_rule_add(GtkWidget *w, gpointer data)
{
    (void)w;
    BnRuleUi   *ui = data;
    GtkTreeIter first;
    if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ui->lists), &first)) {
        bn_status(ui->app, "There are no lists yet \xe2\x80\x94 a rule has "
                           "to name one");
        return;
    }
    gchar  *label   = NULL;
    gint64  list_id = 0;
    gtk_tree_model_get(GTK_TREE_MODEL(ui->lists), &first,
                       LC_LABEL, &label, LC_ID, &list_id, -1);

    /* Before the default row, which is always last.                     */
    GtkTreeIter iter;
    gint        n    = gtk_tree_model_iter_n_children(
                           GTK_TREE_MODEL(ui->store), NULL);
    gtk_list_store_insert(ui->store, &iter, n > 0 ? n - 1 : 0);
    gtk_list_store_set(ui->store, &iter,
        RC_ID, (gint64)0, RC_PATTERN, "", RC_LIST_LABEL, label,
        RC_LIST_ID, list_id, RC_DEFAULT, FALSE, -1);
    g_free(label);

    GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(ui->store),
                                                &iter);
    gtk_widget_grab_focus(GTK_WIDGET(ui->view));
    gtk_tree_view_set_cursor(ui->view, path,
                             gtk_tree_view_get_column(ui->view, 0), TRUE);
    gtk_tree_path_free(path);
}

/* on_rule_remove() — drop the selected rule; its items go back to
 * whatever now claims them, which is usually the default target.         */
static void
on_rule_remove(GtkWidget *w, gpointer data)
{
    (void)w;
    BnRuleUi         *ui  = data;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(ui->view);
    GtkTreeIter       iter;
    if (!gtk_tree_selection_get_selected(sel, NULL, &iter))
        return;                      /* the button is insensitive anyway   */

    gboolean def = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(ui->store), &iter, RC_DEFAULT, &def, -1);
    if (def)
        return;                      /* ditto — something must catch the
                                      * items no rule claims              */

    gint64 id = bn_rule_id_at(ui, &iter);
    if (id == 0) {                   /* added, never typed into           */
        gtk_list_store_remove(ui->store, &iter);
        return;
    }
    gchar *sql = g_strdup_printf(
        "DELETE FROM notes_rule WHERE id = %" G_GINT64_FORMAT, id);
    host->db->exec(host->db->main_db(ui->app), sql);
    g_free(sql);

    bn_rule_ui_reload(ui);
    bn_refile(ui->app);
}

/* The direction a move button carries, hung off the button so the two
 * share one handler rather than one body written twice.                  */
#define BN_MOVE_DIR "bn-move-dir"

/* ---------------------------------------------------------------------------
 * on_rule_sel_changed() — the three buttons act on the SELECTION, so
 * they say so by greying out when there is nothing they can do to it.
 *
 * A button that silently does nothing is worse than one that is plainly
 * unavailable — and with first-match-wins, "why can I not move this?"
 * has to be answerable by looking.  None of them apply to the DEFAULT
 * row (it cannot go, and it is not in the ordering), nor to a row that
 * has been added but not yet typed into: it has no database row to
 * delete or position.
 * ------------------------------------------------------------------------- */
static void
on_rule_sel_changed(GtkTreeSelection *sel, gpointer data)
{
    BnRuleUi     *ui = data;
    GtkTreeModel *model;
    GtkTreeIter   iter;
    gboolean      have = gtk_tree_selection_get_selected(sel, &model, &iter);
    gboolean      def  = FALSE;
    gint64        id   = 0;
    gboolean      up   = FALSE;
    gboolean      down = FALSE;

    if (have) {
        gtk_tree_model_get(model, &iter, RC_DEFAULT, &def, RC_ID, &id, -1);
        if (!def && id > 0) {
            GtkTreeIter probe = iter;
            up = gtk_tree_model_iter_previous(model, &probe);
            probe = iter;
            if (gtk_tree_model_iter_next(model, &probe)) {
                gboolean next_def = FALSE;
                gtk_tree_model_get(model, &probe, RC_DEFAULT, &next_def, -1);
                down = !next_def;    /* never past the default row        */
            }
        }
    }
    gtk_widget_set_sensitive(ui->remove, have && !def && id > 0);
    gtk_widget_set_sensitive(ui->up,   up);
    gtk_widget_set_sensitive(ui->down, down);
}

/* ---------------------------------------------------------------------------
 * on_rule_move() — swap the selected rule with its neighbour.
 *
 * The ORDER IS THE PRECEDENCE, so this is how a user says which of two
 * matching rules wins.  The store is swapped first and the positions are
 * then written FROM it, so the table and the database cannot disagree
 * about what the user just saw happen.  No reload: the selection follows
 * the row through gtk_list_store_swap, and reloading would lose it.
 * ------------------------------------------------------------------------- */
static void
on_rule_move(GtkWidget *button, gpointer data)
{
    BnRuleUi         *ui  = data;
    gint              dir = GPOINTER_TO_INT(
                                g_object_get_data(G_OBJECT(button),
                                                  BN_MOVE_DIR));
    GtkTreeSelection *sel = gtk_tree_view_get_selection(ui->view);
    GtkTreeIter       iter, other;
    if (!gtk_tree_selection_get_selected(sel, NULL, &iter))
        return;                      /* the buttons are insensitive anyway */

    gboolean def = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(ui->store), &iter, RC_DEFAULT, &def, -1);
    if (def)
        return;

    other = iter;
    gboolean ok = dir < 0
        ? gtk_tree_model_iter_previous(GTK_TREE_MODEL(ui->store), &other)
        : gtk_tree_model_iter_next(GTK_TREE_MODEL(ui->store), &other);
    if (!ok)
        return;
    gtk_tree_model_get(GTK_TREE_MODEL(ui->store), &other, RC_DEFAULT, &def, -1);
    if (def)
        return;                      /* the default rule stays last        */

    gtk_list_store_swap(ui->store, &iter, &other);
    bn_rule_positions_save(ui);
    /* A swap moves the row under a selection that never "changed", so
     * the buttons are re-evaluated by hand — the top row's Move Up must
     * grey the moment it gets there.                                     */
    on_rule_sel_changed(sel, ui);
    bn_refile(ui->app);
}

/* rule_pattern_func() — the first column's per-row look.
 *
 * EVERY row is editable, the default included: typing a pattern into
 * "Anything else" turns it into an ordinary rule (see
 * on_rule_pattern_edited).  What marks it out is ITALICS — the words are
 * a description of the row rather than text anyone typed — and the
 * editor it opens is empty (on_rule_editing_started).
 *
 * A data func rather than another store column: "is this the default?"
 * is already one, and deriving the look from it here keeps one source of
 * truth.  It runs per DRAW, which is why it does nothing but set two
 * properties on a table of a handful of rows.                           */
static void
rule_pattern_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                  GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    (void)col;
    (void)data;
    gboolean def = FALSE;
    gtk_tree_model_get(model, iter, RC_DEFAULT, &def, -1);
    g_object_set(cell,
                 "editable",  TRUE,
                 "style",     def ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL,
                 "style-set", TRUE,
                 NULL);
}

/* ---------------------------------------------------------------------------
 * bn_rules_build() — the rules table and its two buttons, into `box`.
 * ------------------------------------------------------------------------- */
static void
bn_rules_build(TaskApp *app, GtkWidget *box)
{
    BnRuleUi *ui = g_new0(BnRuleUi, 1);
    ui->app   = app;
    ui->store = gtk_list_store_new(RC_N, G_TYPE_INT64, G_TYPE_STRING,
                                   G_TYPE_STRING, G_TYPE_INT64,
                                   G_TYPE_BOOLEAN);
    ui->lists = gtk_list_store_new(LC_N, G_TYPE_STRING, G_TYPE_INT64);

    GPtrArray *lists = host->db->lists(host->db->main_db(app), FALSE);
    for (guint i = 0; i < lists->len; i++) {
        TaskList *l     = g_ptr_array_index(lists, i);
        gchar    *label = bn_list_label(l);
        gtk_list_store_insert_with_values(ui->lists, NULL, -1,
            LC_LABEL, label, LC_ID, l->id, -1);
        g_free(label);
    }
    host->db->lists_free(lists);

    ui->view = GTK_TREE_VIEW(
        gtk_tree_view_new_with_model(GTK_TREE_MODEL(ui->store)));
    gtk_tree_view_set_enable_search(ui->view, FALSE);   /* gotcha 5        */

    GtkCellRenderer *pat = gtk_cell_renderer_text_new();
    g_signal_connect(pat, "edited", G_CALLBACK(on_rule_pattern_edited), ui);
    g_signal_connect(pat, "editing-started",
                     G_CALLBACK(on_rule_editing_started), ui);
    GtkTreeViewColumn *c_pat = gtk_tree_view_column_new_with_attributes(
        "When the text contains", pat, "text", RC_PATTERN, NULL);
    gtk_tree_view_column_set_cell_data_func(c_pat, pat, rule_pattern_func,
                                            NULL, NULL);
    gtk_tree_view_column_set_expand(c_pat, TRUE);
    gtk_tree_view_append_column(ui->view, c_pat);

    GtkCellRenderer *lst = gtk_cell_renderer_combo_new();
    /* has-entry FALSE: a rule names a list that EXISTS, so typing a name
     * into it could only produce one that does not.                      */
    g_object_set(lst, "model", ui->lists, "text-column", LC_LABEL,
                      "has-entry", FALSE, "editable", TRUE, NULL);
    g_signal_connect(lst, "changed", G_CALLBACK(on_rule_list_changed), ui);
    gtk_tree_view_append_column(ui->view,
        gtk_tree_view_column_new_with_attributes(
            "File it in", lst, "text", RC_LIST_LABEL, NULL));

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw),
                                        GTK_SHADOW_IN);
    /* A fixed four rows and a header.  The window's natural height is
     * what the Settings column asks for, so a table growing with its
     * contents would push everything below it off the bottom.            */
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), 108);
    gtk_container_add(GTK_CONTAINER(sw), GTK_WIDGET(ui->view));
    gtk_box_pack_start(GTK_BOX(box), sw, FALSE, FALSE, 0);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *add  = gtk_button_new_with_label("Add Rule");
    ui->remove      = gtk_button_new_with_label("Remove Rule");
    ui->up          = gtk_button_new_with_label("Move Up");
    ui->down        = gtk_button_new_with_label("Move Down");
    gtk_widget_set_tooltip_text(ui->up,
        "The first rule that matches wins â this is how you say "
        "which");
    gtk_widget_set_tooltip_text(ui->down, "Let the rules above claim more");
    gtk_widget_set_sensitive(ui->remove, FALSE);
    gtk_widget_set_sensitive(ui->up, FALSE);
    gtk_widget_set_sensitive(ui->down, FALSE);
    g_object_set_data(G_OBJECT(ui->up),   BN_MOVE_DIR, GINT_TO_POINTER(-1));
    g_object_set_data(G_OBJECT(ui->down), BN_MOVE_DIR, GINT_TO_POINTER(1));
    g_signal_connect(add,        "clicked", G_CALLBACK(on_rule_add), ui);
    g_signal_connect(ui->remove, "clicked", G_CALLBACK(on_rule_remove), ui);
    g_signal_connect(ui->up,     "clicked", G_CALLBACK(on_rule_move), ui);
    g_signal_connect(ui->down,   "clicked", G_CALLBACK(on_rule_move), ui);
    g_signal_connect(gtk_tree_view_get_selection(ui->view), "changed",
                     G_CALLBACK(on_rule_sel_changed), ui);
    gtk_box_pack_start(GTK_BOX(btns), add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), ui->remove, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), ui->up, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), ui->down, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), btns, FALSE, FALSE, 0);

    /* Hung on the view, so it outlives every handler above and no
     * longer: the window is rebuilt on each opening.                     */
    g_object_set_data_full(G_OBJECT(ui->view), BN_RULE_UI, ui,
                           bn_rule_ui_free);
    bn_rule_ui_reload(ui);
}

/* bn_settings_section() — build the Notes section into the window's one
 * scrolling column (see settings_window.h).                               */
static void
bn_settings_section(TaskApp *app, GtkWidget *column, GtkWindow *window,
                    gpointer user_data)
{
    (void)window;
    (void)user_data;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(column), box, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box),
                       host->settings->heading("Notes"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), host->settings->note(
        "Mirror the action items from Notes as ordinary tasks, with "
        "their own notes, subtasks and attachments. Ticking one off or "
        "changing its due date is sent back to Notes on the interval "
        "below; the item's text belongs to the note it lives in, so "
        "edit that in Notes."), FALSE, FALSE, 0);

    GtkWidget *check = gtk_check_button_new_with_label(
        "Mirror Notes action items");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check),
        host->config->get_bool(self, "sync", FALSE));
    g_signal_connect(check, "toggled", G_CALLBACK(on_bn_toggled), app);
    gtk_box_pack_start(GTK_BOX(box), check, FALSE, FALSE, 0);

    /* Path, entry, Browse — and the ENTRY stays the field of record.  A
     * bare command name still searches PATH, so pasting a line of
     * `which -a notes` must go on working; the button is the other way
     * to the same setting, not a replacement for typing one.            */
    GtkWidget *cli_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(cli_row), gtk_label_new("Notes binary path:"),
                       FALSE, FALSE, 0);
    GtkWidget *cli = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(cli), "notes (searched on PATH)");
    gtk_widget_set_hexpand(cli, TRUE);
    gchar *cli_v = host->config->get(self, "cli");
    if (cli_v != NULL)
        gtk_entry_set_text(GTK_ENTRY(cli), cli_v);
    g_free(cli_v);
    g_signal_connect(cli, "changed", G_CALLBACK(on_bn_cli_changed), app);
    g_signal_connect(cli, "activate", G_CALLBACK(on_bn_cli_commit), app);
    g_signal_connect(cli, "focus-out-event",
                     G_CALLBACK(on_bn_cli_focus_out), app);
    gtk_box_pack_start(GTK_BOX(cli_row), cli, TRUE, TRUE, 0);

    GtkWidget *browse = gtk_button_new_with_label("Browse\xe2\x80\xa6");
    gtk_widget_set_tooltip_text(browse,
        "Find the Notes program on disk \xe2\x80\x94 or type a path here, "
        "or a bare name to search your PATH");
    g_object_set_data(G_OBJECT(browse), BN_CLI_ENTRY, cli);
    g_signal_connect(browse, "clicked", G_CALLBACK(on_bn_cli_browse), app);
    gtk_box_pack_start(GTK_BOX(cli_row), browse, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), cli_row, FALSE, FALSE, 0);

    /* The rules table is the WHOLE answer to "where does an item go?",
     * ending in the default rule — which is why there is no destination
     * combo above it any more.  It gets a HEADING rather than a
     * paragraph; what the paragraph said is the tooltip on both.        */
    GtkWidget *rules_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(rules_row), host->settings->heading("Rules"),
                       FALSE, FALSE, 0);
    /* U+2139 INFORMATION SOURCE plus U+FE0E VARIATION SELECTOR-15.  The
     * selector ASKS for text presentation and quartz IGNORES it —
     * measured 2026-09-06, it renders as the blue colour emoji here
     * regardless.  Accepted deliberately, so do not "fix" it by hunting
     * for a font that honours the selector: nothing depends on which
     * presentation appears, and a Linux/Adwaita build drawing the text
     * glyph instead is equally correct.
     *
     * It lives in an EVENT BOX so the hover region is exactly the glyph —
     * the box owns a real window, rather than leaning on hit-testing a
     * no-window label.  visible_window FALSE makes that window
     * INPUT-ONLY, so it still receives the tooltip query but paints
     * nothing over the section (and sidesteps gotcha 18, since nothing
     * here needs padding).                                             */
    GtkWidget *info = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(info), FALSE);
    gtk_container_add(GTK_CONTAINER(info),
                      gtk_label_new("\xe2\x84\xb9\xef\xb8\x8e"));
    gtk_widget_set_tooltip_text(info, BN_RULES_HELP);
    gtk_box_pack_start(GTK_BOX(rules_row), info, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), rules_row, FALSE, FALSE, 0);
    bn_rules_build(app, box);

    /* How often the mirror runs — the same shape as the Google sync's
     * own (0 = only when Sync is pressed).                              */
    GtkWidget *iv_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(iv_row),
                       gtk_label_new("Sync action items every"),
                       FALSE, FALSE, 0);
    GtkWidget *spin = gtk_spin_button_new_with_range(0, 720, 1);
    gtk_widget_set_tooltip_text(spin, "0 = only when you press Sync");
    gchar *iv_v = host->config->get(self, "sync_interval_min");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin),
                              iv_v != NULL ? g_ascii_strtod(iv_v, NULL) : 5);
    g_free(iv_v);
    g_signal_connect(spin, "value-changed",
                     G_CALLBACK(on_bn_interval_changed), app);
    gtk_box_pack_start(GTK_BOX(iv_row), spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(iv_row), gtk_label_new("minutes"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), iv_row, FALSE, FALSE, 0);

    GtkWidget *tb = gtk_check_button_new_with_label(
        "Show Sync button in toolbar");
    gtk_widget_set_tooltip_text(tb,
        "Notes \xe2\x86\x92 Sync Now does the same thing, and stays "
        "either way");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tb),
        host->config->get_bool(self, "toolbar_button", TRUE));
    g_signal_connect(tb, "toggled", G_CALLBACK(on_bn_toolbar_toggled), app);
    gtk_box_pack_start(GTK_BOX(box), tb, FALSE, FALSE, 0);

    GtkWidget *meta = gtk_check_button_new_with_label(
        "Show the All Action Items view in the sidebar");
    gtk_widget_set_tooltip_text(meta,
        "Lists every mirrored action item in one place, whichever list "
        "each one lives in");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(meta),
        host->config->get_bool(self, "meta_row", TRUE));
    g_signal_connect(meta, "toggled", G_CALLBACK(on_bn_meta_toggled), app);
    gtk_box_pack_start(GTK_BOX(box), meta, FALSE, FALSE, 0);
}

/* ---------------------------------------------------------------------------
 * notes_init() — the plugin's init hook: register the worker, the sidebar
 * view, the row decoration, the delete hook and the settings section.
 * Cheap by design; it runs before the window is shown (see plugin.h).
 * ------------------------------------------------------------------------- */
static gboolean
notes_init(TaskApp *app, const TaskPlugin *me)
{
    (void)app;
    (void)me;
    bn_worker_live         = bn_worker;
    bn_worker_live.running = &bn_running;
    bn_worker_live.timer   = &bn_timer;
    host->worker->register_worker(&bn_worker_live);

    host->views->register_view(&bn_view);
    host->rows->add_decoration(&bn_decor);
    host->ui->add_tool(&bn_sync_tool);
    host->ui->add_menu_item(&bn_sync_menu_item);
    host->settings->add_section(bn_settings_section, NULL);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * notes_db_open() — create this plugin's own tables.
 *
 * Called for the main connection at startup and again whenever the
 * database changes identity.  IF NOT EXISTS because a database migrated
 * from schema v9 already has them, and the worker opens the same file on
 * its own connection.
 *
 * notes_task is the mirror's whole bookkeeping: the uid that IS the
 * item's identity, plus `text`/`done`/`due` as the BASELINE — what Notes
 * was last known to hold.  Diffing a row against that baseline is what makes
 * the pending-write set DERIVABLE rather than queued, so there is no
 * queue table to corrupt and the set survives a crash.
 *
 * notes_deleted is DROPPED here, not merely unused.  It held uids whose
 * task had been deleted in Tasks, so the next pass would not re-create
 * them — and that turned out to be the wrong shape for what this mirror
 * is: Notes is authoritative, so the answer to "why is this action item
 * not in Tasks?" must never be a hidden list of uids the app is
 * declining to mirror.  A table nothing reads is worse than no table,
 * because the next person to open the database has to work out which.
 *
 * The remaining statements are duplicated by the v9 migration in db.c,
 * and that
 * is correct rather than redundant: the migration has to MOVE existing
 * data whether or not this plugin is installed, and this hook has to
 * work on a database that never had the old columns at all.
 * ------------------------------------------------------------------------- */
static void
notes_db_open(TaskApp *app, TaskDatabase *db, const TaskPlugin *me)
{
    (void)app;
    (void)me;
    host->db->exec(db,
        "CREATE TABLE IF NOT EXISTS notes_task ("
        "  task_id INTEGER PRIMARY KEY REFERENCES tasks(id)"
        "          ON DELETE CASCADE,"
        "  uid     INTEGER NOT NULL,"
        "  done    INTEGER NOT NULL DEFAULT 0,"
        "  due     INTEGER NOT NULL DEFAULT 0,"
        "  " BN_TEXT_COLUMN ")");
    host->db->exec(db,
        "CREATE INDEX IF NOT EXISTS idx_notes_task_uid ON notes_task(uid)");
    host->db->exec(db, "DROP TABLE IF EXISTS notes_deleted");
    /* The filing rules: "when an item's text contains `pattern`, file it
     * in `list_id`".  In the DATABASE rather than the ini because a rule
     * NAMES A LIST BY ID, and a list id belongs to the file the lists
     * live in — the ini travels with the binary and survives a database
     * switch, which would leave every rule pointing at whatever list
     * happened to hold that id in the new file, filing items into a
     * stranger's list and saying nothing.  That is also why the
     * destination this replaced (the ini key `notes_embed_list` and its
     * combo) is GONE rather than kept alongside: one table, in the file
     * the lists live in, is the whole answer.
     *
     * No foreign key on list_id: lists are TOMBSTONED rather than
     * deleted, so no cascade could see one go — bn_rules_load drops a
     * rule whose list is not live instead, which covers both.          */
    host->db->exec(db,
        "CREATE TABLE IF NOT EXISTS notes_rule ("
        "  id      INTEGER PRIMARY KEY,"
        "  pattern TEXT NOT NULL,"
        "  list_id INTEGER NOT NULL,"
        "  position INTEGER NOT NULL " BN_POSITION_DEFAULT ")");
    /* `position` came after the table did, so it needs BOTH halves and
     * they are not the same statement: the declaration above is all a
     * FRESH database ever runs, this ALTER is all an EXISTING one ever
     * runs (gotcha 24).  Guarded on the COLUMN rather than a version
     * stamp, which makes it idempotent — a re-run on a healthy file
     * would otherwise log "duplicate column name" on every launch, and a
     * warning that fires on the ordinary path is a warning nobody reads.
     * The DEFAULT is built from one macro so it is not spelled twice.
     *
     * Existing rows all land on position 0 and the ORDER BY breaks that
     * tie with `id`, so nothing moves until someone presses Move Up. */
    if (!bn_table_has_column(db, "notes_rule", "position"))
        host->db->exec(db,
            "ALTER TABLE notes_rule ADD COLUMN position INTEGER NOT NULL "
            BN_POSITION_DEFAULT);

    /* notes_task.text — the TEXT baseline that makes a title edited HERE
     * a pending write instead of something Notes overwrites next pass.
     * Same shape as the ALTER above, and deliberately NOT added to the v9
     * migration in db.c: that migration's job is to move what existed
     * when it ran, and teaching it about a column decided on later is how
     * the one path nobody can easily test acquires a bug.  Existing rows
     * land on '', which bn_baseline reports as "no baseline yet" and
     * sync_item resolves in NOTES' favour — so the first pass after the
     * upgrade adopts each item's current text and fills the baseline in,
     * rather than pushing a title that may already be stale.            */
    if (!bn_table_has_column(db, "notes_task", "text"))
        host->db->exec(db,
            "ALTER TABLE notes_task ADD COLUMN " BN_TEXT_COLUMN);
}

static const TaskPlugin notes_plugin = {
    .abi_version     = TASK_PLUGIN_ABI_VERSION,
    .abi_revision    = TASK_PLUGIN_ABI_REVISION,
    .id              = "notes",
    .name            = "Notes Action Items Sync",
    .description     = "Mirror the companion Notes app's action items as "
                       "ordinary tasks.",
    .version         = "1.0.0",
    .enabled_default = TRUE,
    .init            = notes_init,
    .db_open         = notes_db_open,
};

TASK_PLUGIN_EXPORT const TaskPlugin *
task_plugin_entry(const TaskHostApi *api)
{
    host = api;
    self = &notes_plugin;
    return &notes_plugin;
}
