/* Native GUI front end for the RPN and algebraic calculator. */
#include "calc.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui/app.h>

static struct app *application;
static struct widget *display;
static struct widget *status_label;
static struct widget *enter_button, *equals_button;
static struct widget *swap_button, *drop_button;
static struct widget *left_paren_button, *right_paren_button;
static struct calc_state calculator;

/* ---- user interface ---- */

static void refresh_ui(void)
{
    if (status_label)
        widget_set_text(status_label, calculator.status);
    if (display)
        widget_invalidate(display);
}

static void update_mode_controls(void)
{
    int rpn = calculator.mode == MODE_RPN;
    widget_set_text(enter_button, rpn ? "ENTER" : "Eval");
    widget_set_enabled(equals_button, !rpn);
    widget_set_enabled(swap_button, rpn);
    widget_set_enabled(drop_button, rpn);
    widget_set_enabled(left_paren_button, !rpn);
    widget_set_enabled(right_paren_button, !rpn);
}

static int on_action(struct widget *w, void *args, void *arg)
{
    calc_action(&calculator, arg);
    refresh_ui();
    widget_focus(display);
    return 1;
}

static int on_mode(struct widget *w, void *args, void *arg)
{
    int mode = ((struct sig_select *)args)->index;
    calc_change_mode(&calculator, mode == 0 ? MODE_RPN : MODE_ALGEBRAIC);
    update_mode_controls();
    refresh_ui();
    widget_focus(display);
    printf("calc: mode %s\n", calculator.mode == MODE_RPN ? "rpn" : "algebraic");
    fflush(stdout);
    return 1;
}

static void draw_right(struct painter *p, int width, int y, const char *text, uint32_t color)
{
    int x = width - 8 - painter_text_width(p, text, -1);
    if (x < 28)
        x = 28;
    painter_text(p, x, y, text, color);
}

static int on_display_paint(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    const struct theme *theme = p->theme;
    painter_fill(p, 0, 0, w->w, w->h, theme->color[TC_WINDOW]);
    painter_rounded(p, 0, 0, w->w, w->h, theme->color[TC_FIELD],
                    theme->color[w->focused ? TC_ACCENT : TC_BORDER]);
    painter_push(p, 2, 2, w->w - 4, w->h - 4);
    int th = painter_text_height(p), color = theme->color[calculator.error ? TC_ACCENT : TC_TEXT];
    if (calculator.mode == MODE_RPN) {
        static const char names[] = { 'T', 'Z', 'Y', 'X' };
        for (int row = 0; row < 4; row++) {
            int index = calculator.depth - 4 + row;
            char label[4] = { names[row], ':', '\0' };
            char value[64] = "--";
            if (row == 3 && calculator.input[0])
                snprintf(value, sizeof value, "[%s]", calculator.input);
            else if (index >= 0)
                calc_format_value(value, sizeof value, calculator.stack[index]);
            int y = 6 + row * (th + 5);
            painter_text(p, 8, y, label, theme->color[TC_TEXT_DISABLED]);
            draw_right(p, w->w - 4, y, value, (uint32_t)color);
        }
    } else {
        painter_text(p, 8, 8, "Expression", theme->color[TC_TEXT_DISABLED]);
        draw_right(p, w->w - 4, 8 + th + 12,
                   calculator.input[0] ? calculator.input : "0", (uint32_t)color);
        painter_text(p, 8, w->h - th - 10, "Enter or Eval computes the expression",
                     theme->color[TC_TEXT_DISABLED]);
    }
    painter_pop(p);
    return 1;
}

static int on_key(struct widget *w, void *args, void *arg)
{
    struct sig_key *key = args;
    int report_result = key->ch == '\n';
    if (key->code == 0x01) {
        calc_reset(&calculator);
    } else if (key->ch == '\b') {
        calc_action(&calculator, "BS");
    } else if (key->ch == '\n') {
        calc_action(&calculator, "ENTER");
    } else if (calculator.mode == MODE_RPN) {
        char text[2] = { (char)key->ch, '\0' };
        if (isdigit(key->ch) || key->ch == '.' || key->ch == 'e' || key->ch == 'E') {
            calc_input_text(&calculator, text);
        } else if (strchr("+-*/%^", key->ch)) {
            size_t n = strlen(calculator.input);
            if ((key->ch == '+' || key->ch == '-') && n &&
                (calculator.input[n - 1] == 'e' || calculator.input[n - 1] == 'E'))
                calc_input_text(&calculator, text);
            else {
                calc_action(&calculator, text);
                report_result = 1;
            }
        } else {
            return 0;
        }
    } else {
        char text[2] = { (char)key->ch, '\0' };
        if ((key->ch >= 32 && key->ch < 127) &&
            (isalnum(key->ch) || strchr(".+-*/%^(), ", key->ch)))
            calc_input_text(&calculator, text);
        else
            return 0;
    }
    refresh_ui();
    if (report_result && !calculator.error) {
        char value[48];
        if (calculator.mode == MODE_RPN && calculator.depth)
            calc_format_value(value, sizeof value, calculator.stack[calculator.depth - 1]);
        else
            strlcpy(value, calculator.input, sizeof value);
        printf("calc: result %s\n", value);
        fflush(stdout);
    }
    return 1;
}

struct button_spec {
    const char *label;
    const char *action;
};

static const struct button_spec buttons[7][6] = {
    { { "sin", "sin" }, { "cos", "cos" }, { "tan", "tan" }, { "ln", "ln" }, { "sqrt", "sqrt" }, { "x^y", "^" } },
    { { "asin", "asin" }, { "acos", "acos" }, { "atan", "atan" }, { "log10", "log10" }, { "exp", "exp" }, { "1/x", "recip" } },
    { { "7", "7" }, { "8", "8" }, { "9", "9" }, { "/", "/" }, { "(", "(" }, { ")", ")" } },
    { { "4", "4" }, { "5", "5" }, { "6", "6" }, { "*", "*" }, { "pi", "pi" }, { "e", "const_e" } },
    { { "1", "1" }, { "2", "2" }, { "3", "3" }, { "-", "-" }, { "SWAP", "SWAP" }, { "DROP", "DROP" } },
    { { "0", "0" }, { ".", "." }, { "+/-", "SIGN" }, { "+", "+" }, { "ENTER", "ENTER" }, { "=", "=" } },
    { { "AC", "AC" }, { "CE", "CE" }, { "Back", "BS" }, { "sinh", "sinh" }, { "cosh", "cosh" }, { "tanh", "tanh" } },
};

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
        return calc_self_test();

    calculator.mode = MODE_RPN;
    calc_reset(&calculator);
    application = app_create();
    if (!application) {
        fprintf(stderr, "calc: cannot connect to X12\n");
        return 1;
    }
    struct widget *window = app_window(application, 520, 500, "calculator");
    if (!window)
        return 1;

    struct widget *mode_row = box_new(window, 0);
    label_new(mode_row, "Input mode:");
    struct widget *mode = combobox_new(mode_row);
    combobox_add(mode, "RPN");
    combobox_add(mode, "Algebraic");
    widget_connect(mode, "changed", on_mode, NULL);

    display = canvas_new(window);
    widget_set_hint(display, 0, 126);
    widget_set_min(display, 200, 110);
    widget_connect(display, "paint", on_display_paint, NULL);
    widget_connect(display, "key", on_key, NULL);

    struct widget *grid = grid_new(window);
    widget_set_stretch(grid, 1, 1);
    for (int row = 0; row < 7; row++) {
        grid_set_stretch(grid, row, -1, 1);
        for (int col = 0; col < 6; col++) {
            if (row == 0)
                grid_set_stretch(grid, -1, col, 1);
            struct widget *button = button_new(grid, buttons[row][col].label);
            widget_set_grid(button, row, col, 1, 1);
            widget_set_stretch(button, 1, 1);
            widget_connect(button, "clicked", on_action, (void *)buttons[row][col].action);
            const char *action = buttons[row][col].action;
            if (strcmp(action, "ENTER") == 0) enter_button = button;
            else if (strcmp(action, "=") == 0) equals_button = button;
            else if (strcmp(action, "SWAP") == 0) swap_button = button;
            else if (strcmp(action, "DROP") == 0) drop_button = button;
            else if (strcmp(action, "(") == 0) left_paren_button = button;
            else if (strcmp(action, ")") == 0) right_paren_button = button;
        }
    }

    struct widget *statusbar = statusbar_new(window);
    status_label = statusbar_add(statusbar, 1);
    update_mode_controls();
    refresh_ui();
    widget_focus(display);
    printf("calc: mode rpn\n");
    fflush(stdout);
    int code = app_run(application);
    app_destroy(application);
    return code;
}
