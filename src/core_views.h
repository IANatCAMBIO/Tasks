/* ===========================================================================
 * core_views.h — register the app's own virtual views.
 * =========================================================================== */

#ifndef TASK_CORE_VIEWS_H
#define TASK_CORE_VIEWS_H

/* ---------------------------------------------------------------------------
 * task_core_views_init() — register Favorites, All Tasks and Due Today
 * with the view registry (see task_view.h).  Call ONCE from main(),
 * before the library window is built.
 *
 * Call it BEFORE any plugin registers its own: views are ordered by
 * their `sort` field, but ties fall back to registration order, and the
 * app's own views reading first is the expected shape.
 * ------------------------------------------------------------------------- */
void task_core_views_init(void);

#endif /* TASK_CORE_VIEWS_H */
