/* Included by the profiler so the test exercises its connected widgets
 * and model with deterministic weights after the real capture has ended. */
static void check_ui(void)
{
    int failed = 0;
#define UI_CHECK(c) do { if (!(c)) { printf("FAIL: profiler UI line %d\n", __LINE__); failed++; } } while (0)
    UI_CHECK(!running && session->events > 0);
    FILE *f = fopen("/tmp/profiler-gui.json", "r");
    char text[160] = {0};
    UI_CHECK(f != NULL);
    if (f) {
        fread(text, 1, sizeof text - 1, f);
        fclose(f);
        UI_CHECK(strstr(text, "\"format\":\"minios-profile\"") != NULL);
    }
    on_reset(NULL, NULL, NULL);
    UI_CHECK(!running && !session->events && !zoom_node);
    struct prof_tree *t = tree();
    int outer = prof_names_intern(session->names, "render_frame");
    int work = prof_names_intern(session->names, "draw_widgets");
    int wait = prof_names_intern(session->names, "wait_for_buffer");
    struct prof_stack stack = { .names = {outer}, .count = 1 };
    int parent = prof_tree_add(t, &stack, 10000000, 0);
    stack.count = 2;
    stack.names[1] = work;
    int child = prof_tree_add(t, &stack, 60000000, 0);
    stack.names[1] = wait;
    stack.kernel[1] = 1;
    prof_tree_add(t, &stack, 30000000, 0);
    refresh_views();
    UI_CHECK(flame_canvas->w > 100 && flame_canvas->h > 40 && break_table->h > 30);
    UI_CHECK(box_at(flame_canvas->w / 2, flame_canvas->h - 2 * ROW_HEIGHT + 3,
                    flame_canvas->w, flame_canvas->h) >= 0);
    struct sig_click click = { .button = 1, .x = flame_canvas->w / 2,
                              .y = flame_canvas->h - 2 * ROW_HEIGHT + 3 };
    widget_emit(flame_canvas, "press", &click);
    UI_CHECK(zoom_node == parent && nbreak == 3);
    UI_CHECK(strstr(widget_text(break_info), "render_frame") != NULL);
    UI_CHECK(break_rows[0].node == child && break_rows[0].total == 60000000);
    struct sig_select select = { .index = 0 };
    widget_emit(break_table, "activate", &select);
    UI_CHECK(zoom_node == child && nbreak == 1);
    struct widget *win = app_first_window(app);
    widget_emit(widget_find(win, "up"), "clicked", NULL);
    UI_CHECK(zoom_node == parent);
    widget_emit(widget_find(win, "all"), "clicked", NULL);
    UI_CHECK(!zoom_node);
    widget_set_text(search_box, "draw");
    widget_emit(search_box, "changed", NULL);
    UI_CHECK(strstr(widget_text(search_info), "60.0%") != NULL);
    /* A stopped timer must preserve the user's current interaction. */
    hover_box = 1;
    tick(NULL);
    UI_CHECK(hover_box == 1);
    UI_CHECK(save_capture("/tmp/profiler-gui.folded", PROF_EXPORT_FOLDED) == 0);
    f = fopen("/tmp/profiler-gui.folded", "r");
    UI_CHECK(f != NULL);
    if (f) {
        memset(text, 0, sizeof text);
        fread(text, 1, sizeof text - 1, f);
        fclose(f);
        UI_CHECK(strstr(text, "render_frame;draw_widgets 60000000") != NULL);
    }
    /* Leave a selected breakdown and search highlight visible for screenshots. */
    widget_emit(flame_canvas, "press", &click);
    UI_CHECK(zoom_node == parent);
    printf("profiler: UI checks %s\n", failed ? "FAILED" : "passed");
    fflush(stdout);
#undef UI_CHECK
}
