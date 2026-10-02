/* The context menu of the text widgets: Undo, Redo, Cut, Copy, Paste,
 * Delete and Select all. */
#include <stdlib.h>
#include <string.h>
#include "editmenu.h"

static const struct {
    const char *text, *icon;
} items[EDIT_NACTIONS] = {
    [EDIT_UNDO] = { "Undo", "undo" },
    [EDIT_REDO] = { "Redo", "redo" },
    [EDIT_CUT] = { "Cut", "cut" },
    [EDIT_COPY] = { "Copy", "copy" },
    [EDIT_PASTE] = { "Paste", "paste" },
    [EDIT_DELETE] = { "Delete", NULL },
    [EDIT_SELECT_ALL] = { "Select all", NULL },
};

/* The handler argument of an item is its action.  The menu stores the
 * callback in user, and the parent of the menu is the owner. */
static int on_item(struct widget *item, void *args, void *arg)
{
    struct widget *menu = item->parent;
    edit_action_fn fn = (edit_action_fn)menu->user;
    if (fn)
        fn(menu->parent, (enum edit_action)(long)arg);
    return 1;
}

static void add_item(struct widget *menu, enum edit_action a, unsigned shown)
{
    if (!(shown & EDIT_BIT(a)))
        return;
    struct widget *it = menu_add(menu, items[a].text, items[a].icon);
    widget_set_id(it, items[a].text);
    widget_connect(it, "clicked", on_item, (void *)(long)a);
}

static struct widget *build(struct widget *owner, unsigned shown)
{
    struct widget *menu = widget_new(&menu_class, owner);
    if (!menu)
        return NULL;
    menu->visible = 0;
    add_item(menu, EDIT_UNDO, shown);
    add_item(menu, EDIT_REDO, shown);
    if (shown & (EDIT_BIT(EDIT_UNDO) | EDIT_BIT(EDIT_REDO)))
        menu_add_separator(menu);
    add_item(menu, EDIT_CUT, shown);
    add_item(menu, EDIT_COPY, shown);
    add_item(menu, EDIT_PASTE, shown);
    add_item(menu, EDIT_DELETE, shown);
    menu_add_separator(menu);
    add_item(menu, EDIT_SELECT_ALL, shown);
    return menu;
}

void edit_menu_popup(struct widget *owner, struct widget **menu, int x, int y, unsigned shown, unsigned enabled,
                     edit_action_fn fn)
{
    if (!*menu)
        *menu = build(owner, shown);
    if (!*menu)
        return;
    (*menu)->user = (void *)fn;
    for (struct widget *it = (*menu)->first; it; it = it->next)
        for (int a = 0; a < EDIT_NACTIONS; a++)
            if (it->id && items[a].text && strcmp(it->id, items[a].text) == 0)
                widget_set_enabled(it, (enabled & EDIT_BIT(a)) != 0);
    int ax, ay;
    widget_abs(owner, &ax, &ay);
    menu_popup(*menu, ax + x, ay + y);
}
