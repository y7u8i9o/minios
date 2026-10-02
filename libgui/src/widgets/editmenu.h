#pragma once
/* The context menu of the text widgets (editor.c, textfield.c). */
#include <gui/app.h>

enum edit_action {
    EDIT_UNDO, EDIT_REDO, EDIT_CUT, EDIT_COPY, EDIT_PASTE, EDIT_DELETE, EDIT_SELECT_ALL, EDIT_NACTIONS,
};

#define EDIT_BIT(a) (1u << (a))

typedef void (*edit_action_fn)(struct widget *owner, enum edit_action action);

/* edit_menu_popup opens the context menu of owner at the widget
 * coordinates x, y.  The menu is created on the first call as an
 * invisible child of owner and stored in *menu, so it is destroyed with
 * owner.  The actions in the mask shown are items of the menu, and the
 * actions in the mask enabled can be chosen.  A chosen item calls fn with
 * owner and the action. */
void edit_menu_popup(struct widget *owner, struct widget **menu, int x, int y, unsigned shown, unsigned enabled,
                     edit_action_fn fn);
