#pragma once

#include <stddef.h>

#define CALC_INPUT_MAX 256
#define CALC_STACK_MAX 32

enum calc_mode {
    MODE_RPN,
    MODE_ALGEBRAIC,
};

struct calc_state {
    enum calc_mode mode;
    double stack[CALC_STACK_MAX];
    int depth;
    char input[CALC_INPUT_MAX];
    char status[96];
    int error;
    int just_evaluated;
};

void calc_format_value(char *buffer, size_t size, double value);
void calc_reset(struct calc_state *c);
void calc_change_mode(struct calc_state *c, enum calc_mode mode);
void calc_action(struct calc_state *c, const char *action);
void calc_input_text(struct calc_state *c, const char *text);
int calc_self_test(void);
