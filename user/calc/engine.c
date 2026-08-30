/* Calculator engine: RPN stack operations and algebraic parsing. */
#include "calc.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct parser {
    const char *cur;
    char error[64];
};

static void set_status(struct calc_state *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->status, sizeof c->status, fmt, ap);
    va_end(ap);
}

static void set_error(struct calc_state *c, const char *message)
{
    c->error = 1;
    set_status(c, "Error: %s", message);
}

static void clear_error(struct calc_state *c)
{
    c->error = 0;
}

void calc_format_value(char *buffer, size_t size, double value)
{
    if (isnan(value))
        strlcpy(buffer, "nan", size);
    else if (isinf(value))
        strlcpy(buffer, signbit(value) ? "-inf" : "inf", size);
    else
        snprintf(buffer, size, "%.15g", value);
}

/* ---- algebraic expression parser ---- */

static void parser_skip_space(struct parser *p)
{
    while (isspace((unsigned char)*p->cur))
        p->cur++;
}

static void parser_fail(struct parser *p, const char *message)
{
    if (!p->error[0])
        strlcpy(p->error, message, sizeof p->error);
}

static int parser_take(struct parser *p, char ch)
{
    parser_skip_space(p);
    if (*p->cur != ch)
        return 0;
    p->cur++;
    return 1;
}

static double parse_expression(struct parser *p);
static double parse_unary(struct parser *p);

static double checked_result(struct parser *p, double result)
{
    if (errno == EDOM)
        parser_fail(p, "domain error");
    else if (errno == ERANGE)
        parser_fail(p, "range error");
    return result;
}

static double apply_function(struct parser *p, const char *name,
                             double first, double second, int nargs)
{
    errno = 0;
    double result = 0.0;
    if (nargs == 1) {
        if (strcmp(name, "sin") == 0) result = sin(first);
        else if (strcmp(name, "cos") == 0) result = cos(first);
        else if (strcmp(name, "tan") == 0) result = tan(first);
        else if (strcmp(name, "asin") == 0) result = asin(first);
        else if (strcmp(name, "acos") == 0) result = acos(first);
        else if (strcmp(name, "atan") == 0) result = atan(first);
        else if (strcmp(name, "sinh") == 0) result = sinh(first);
        else if (strcmp(name, "cosh") == 0) result = cosh(first);
        else if (strcmp(name, "tanh") == 0) result = tanh(first);
        else if (strcmp(name, "asinh") == 0) result = asinh(first);
        else if (strcmp(name, "acosh") == 0) result = acosh(first);
        else if (strcmp(name, "atanh") == 0) result = atanh(first);
        else if (strcmp(name, "sqrt") == 0) result = sqrt(first);
        else if (strcmp(name, "cbrt") == 0) result = cbrt(first);
        else if (strcmp(name, "exp") == 0) result = exp(first);
        else if (strcmp(name, "exp2") == 0) result = exp2(first);
        else if (strcmp(name, "ln") == 0) result = log(first);
        else if (strcmp(name, "log") == 0 || strcmp(name, "log10") == 0) result = log10(first);
        else if (strcmp(name, "log2") == 0) result = log2(first);
        else if (strcmp(name, "abs") == 0) result = fabs(first);
        else if (strcmp(name, "floor") == 0) result = floor(first);
        else if (strcmp(name, "ceil") == 0) result = ceil(first);
        else if (strcmp(name, "round") == 0) result = round(first);
        else {
            parser_fail(p, "unknown function");
            return 0.0;
        }
    } else if (nargs == 2) {
        if (strcmp(name, "pow") == 0) result = pow(first, second);
        else if (strcmp(name, "atan2") == 0) result = atan2(first, second);
        else if (strcmp(name, "hypot") == 0) result = hypot(first, second);
        else if (strcmp(name, "min") == 0) result = fmin(first, second);
        else if (strcmp(name, "max") == 0) result = fmax(first, second);
        else if (strcmp(name, "fmod") == 0) result = fmod(first, second);
        else if (strcmp(name, "remainder") == 0) result = remainder(first, second);
        else {
            parser_fail(p, "function needs one argument");
            return 0.0;
        }
    } else {
        parser_fail(p, "wrong number of arguments");
        return 0.0;
    }
    return checked_result(p, result);
}

static double parse_primary(struct parser *p)
{
    parser_skip_space(p);
    if (parser_take(p, '(')) {
        double value = parse_expression(p);
        if (!parser_take(p, ')'))
            parser_fail(p, "missing ')'");
        return value;
    }

    char *end;
    errno = 0;
    double number = strtod(p->cur, &end);
    if (end != p->cur) {
        p->cur = end;
        return checked_result(p, number);
    }

    if (isalpha((unsigned char)*p->cur)) {
        char name[20];
        int length = 0;
        while (isalnum((unsigned char)*p->cur) || *p->cur == '_') {
            if (length + 1 < (int)sizeof name)
                name[length++] = (char)tolower((unsigned char)*p->cur);
            p->cur++;
        }
        name[length] = '\0';
        if (strcmp(name, "pi") == 0)
            return M_PI;
        if (strcmp(name, "e") == 0)
            return M_E;
        if (!parser_take(p, '(')) {
            parser_fail(p, "unknown name");
            return 0.0;
        }
        double first = parse_expression(p), second = 0.0;
        int nargs = 1;
        if (parser_take(p, ',')) {
            second = parse_expression(p);
            nargs = 2;
        }
        if (!parser_take(p, ')'))
            parser_fail(p, "missing ')' after function");
        if (p->error[0])
            return 0.0;
        return apply_function(p, name, first, second, nargs);
    }

    parser_fail(p, "expected a number");
    return 0.0;
}

static double parse_power(struct parser *p)
{
    double left = parse_primary(p);
    if (!p->error[0] && parser_take(p, '^')) {
        double right = parse_unary(p);
        errno = 0;
        left = checked_result(p, pow(left, right));
    }
    return left;
}

static double parse_unary(struct parser *p)
{
    if (parser_take(p, '+'))
        return parse_unary(p);
    if (parser_take(p, '-'))
        return -parse_unary(p);
    return parse_power(p);
}

static double parse_term(struct parser *p)
{
    double value = parse_unary(p);
    while (!p->error[0]) {
        parser_skip_space(p);
        char op = *p->cur;
        if (op != '*' && op != '/' && op != '%')
            break;
        p->cur++;
        double right = parse_unary(p);
        if (p->error[0])
            break;
        if ((op == '/' || op == '%') && right == 0.0) {
            parser_fail(p, "division by zero");
            break;
        }
        errno = 0;
        value = op == '*' ? value * right : op == '/' ? value / right : fmod(value, right);
        checked_result(p, value);
    }
    return value;
}

static double parse_expression(struct parser *p)
{
    double value = parse_term(p);
    while (!p->error[0]) {
        parser_skip_space(p);
        char op = *p->cur;
        if (op != '+' && op != '-')
            break;
        p->cur++;
        double right = parse_term(p);
        value = op == '+' ? value + right : value - right;
    }
    return value;
}

static int algebraic_value(const char *expression, double *result,
                           char *error, size_t error_size)
{
    struct parser p = { .cur = expression, .error = "" };
    double value = parse_expression(&p);
    parser_skip_space(&p);
    if (!p.error[0] && *p.cur)
        parser_fail(&p, "unexpected input");
    if (p.error[0]) {
        strlcpy(error, p.error, error_size);
        return 0;
    }
    *result = value;
    return 1;
}

/* ---- calculator state and operations ---- */

void calc_reset(struct calc_state *c)
{
    c->depth = 0;
    c->input[0] = '\0';
    c->just_evaluated = 0;
    clear_error(c);
    set_status(c, c->mode == MODE_RPN ? "RPN stack cleared" : "Expression cleared");
}

static int append_text(struct calc_state *c, const char *text)
{
    size_t have = strlen(c->input), add = strlen(text);
    if (have + add >= sizeof c->input) {
        set_error(c, "input is too long");
        return 0;
    }
    memcpy(c->input + have, text, add + 1);
    clear_error(c);
    return 1;
}

static int stack_push(struct calc_state *c, double value)
{
    if (c->depth == CALC_STACK_MAX) {
        set_error(c, "stack overflow");
        return 0;
    }
    c->stack[c->depth++] = value;
    return 1;
}

static void set_result_status(struct calc_state *c, double value)
{
    char text[48];
    calc_format_value(text, sizeof text, value);
    clear_error(c);
    set_status(c, "Result: %s", text);
}

static int rpn_commit(struct calc_state *c)
{
    if (!c->input[0])
        return 1;
    char *end;
    errno = 0;
    double value = strtod(c->input, &end);
    if (end == c->input || *end) {
        set_error(c, "invalid number");
        return 0;
    }
    if (errno == ERANGE) {
        set_error(c, "number out of range");
        return 0;
    }
    if (!stack_push(c, value))
        return 0;
    c->input[0] = '\0';
    set_result_status(c, value);
    return 1;
}

static void rpn_enter(struct calc_state *c)
{
    if (c->input[0]) {
        rpn_commit(c);
        return;
    }
    if (!c->depth) {
        set_error(c, "stack is empty");
        return;
    }
    if (stack_push(c, c->stack[c->depth - 1]))
        set_result_status(c, c->stack[c->depth - 1]);
}

static void rpn_binary(struct calc_state *c, char op)
{
    if (!rpn_commit(c))
        return;
    if (c->depth < 2) {
        set_error(c, "two stack values required");
        return;
    }
    double left = c->stack[c->depth - 2], right = c->stack[c->depth - 1];
    if ((op == '/' || op == '%') && right == 0.0) {
        set_error(c, "division by zero");
        return;
    }
    errno = 0;
    double result = op == '+' ? left + right : op == '-' ? left - right
                  : op == '*' ? left * right : op == '/' ? left / right
                  : op == '%' ? fmod(left, right) : pow(left, right);
    if (errno == EDOM) {
        set_error(c, "domain error");
        return;
    }
    if (errno == ERANGE) {
        set_error(c, "range error");
        return;
    }
    c->depth--;
    c->stack[c->depth - 1] = result;
    set_result_status(c, result);
}

static void rpn_unary(struct calc_state *c, const char *name)
{
    if (!rpn_commit(c))
        return;
    if (!c->depth) {
        set_error(c, "one stack value required");
        return;
    }
    struct parser p = { .cur = "", .error = "" };
    double result;
    if (strcmp(name, "recip") == 0) {
        if (c->stack[c->depth - 1] == 0.0) {
            set_error(c, "division by zero");
            return;
        }
        result = 1.0 / c->stack[c->depth - 1];
    } else {
        result = apply_function(&p, name, c->stack[c->depth - 1], 0.0, 1);
        if (p.error[0]) {
            set_error(c, p.error);
            return;
        }
    }
    c->stack[c->depth - 1] = result;
    set_result_status(c, result);
}

static int input_ends_operator(const char *text)
{
    size_t n = strlen(text);
    while (n && isspace((unsigned char)text[n - 1]))
        n--;
    if (!n)
        return 1;
    char ch = text[n - 1];
    return ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '%'
        || ch == '^' || ch == '(' || ch == ',';
}

static void algebraic_append(struct calc_state *c, const char *text, int starts_value)
{
    if (c->just_evaluated && starts_value)
        c->input[0] = '\0';
    if (append_text(c, text)) {
        c->just_evaluated = 0;
        set_status(c, "Editing algebraic expression");
    }
}

static void algebraic_function(struct calc_state *c, const char *name)
{
    char text[CALC_INPUT_MAX];
    if (c->input[0] && !input_ends_operator(c->input)) {
        int n = snprintf(text, sizeof text, "%s(%s)", name, c->input);
        if (n < 0 || n >= (int)sizeof text) {
            set_error(c, "input is too long");
            return;
        }
        strlcpy(c->input, text, sizeof c->input);
        c->just_evaluated = 0;
        clear_error(c);
        set_status(c, "Applied %s to expression", name);
    } else {
        char prefix[24];
        snprintf(prefix, sizeof prefix, "%s(", name);
        algebraic_append(c, prefix, 1);
    }
}

static int algebraic_evaluate(struct calc_state *c)
{
    if (!c->input[0]) {
        set_error(c, "expression is empty");
        return 0;
    }
    char error[64], text[48];
    double result;
    if (!algebraic_value(c->input, &result, error, sizeof error)) {
        set_error(c, error);
        return 0;
    }
    calc_format_value(text, sizeof text, result);
    strlcpy(c->input, text, sizeof c->input);
    c->just_evaluated = 1;
    set_result_status(c, result);
    return 1;
}

void calc_change_mode(struct calc_state *c, enum calc_mode mode)
{
    if (mode == c->mode)
        return;
    if (mode == MODE_ALGEBRAIC) {
        if (!c->input[0] && c->depth)
            calc_format_value(c->input, sizeof c->input, c->stack[c->depth - 1]);
        c->mode = mode;
        c->just_evaluated = c->input[0] != '\0';
        clear_error(c);
        set_status(c, "Algebraic mode");
        return;
    }

    if (c->input[0]) {
        char error[64];
        double result;
        if (algebraic_value(c->input, &result, error, sizeof error)) {
            if (c->depth)
                c->stack[c->depth - 1] = result;
            else
                stack_push(c, result);
        } else {
            set_error(c, error);
        }
    }
    c->mode = mode;
    c->input[0] = '\0';
    c->just_evaluated = 0;
    if (!c->error)
        set_status(c, "RPN mode");
}

static void append_constant(struct calc_state *c, const char *name, double value)
{
    if (c->mode == MODE_RPN) {
        if (!rpn_commit(c))
            return;
        if (stack_push(c, value))
            set_result_status(c, value);
        return;
    }
    if (c->just_evaluated)
        c->input[0] = '\0';
    size_t n = strlen(c->input);
    if (n && !input_ends_operator(c->input) && c->input[n - 1] != '(')
        append_text(c, "*");
    algebraic_append(c, name, 0);
}

void calc_action(struct calc_state *c, const char *action)
{
    clear_error(c);
    if (strlen(action) == 1 && (isdigit((unsigned char)action[0]) || action[0] == '.')) {
        if (c->mode == MODE_ALGEBRAIC)
            algebraic_append(c, action, 1);
        else if (append_text(c, action))
            set_status(c, "Entering RPN value");
        return;
    }
    if (strcmp(action, "AC") == 0) {
        calc_reset(c);
    } else if (strcmp(action, "CE") == 0) {
        c->input[0] = '\0';
        c->just_evaluated = 0;
        set_status(c, "Entry cleared");
    } else if (strcmp(action, "BS") == 0) {
        size_t n = strlen(c->input);
        if (n)
            c->input[n - 1] = '\0';
        set_status(c, "Backspace");
    } else if (strcmp(action, "SIGN") == 0) {
        if (c->mode == MODE_RPN) {
            if (c->input[0]) {
                char *exponent = strrchr(c->input, 'e');
                if (!exponent)
                    exponent = strrchr(c->input, 'E');
                char *sign = exponent ? exponent + 1 : c->input;
                if (*sign == '-')
                    memmove(sign, sign + 1, strlen(sign));
                else if (*sign == '+')
                    *sign = '-';
                else {
                    size_t n = strlen(c->input);
                    if (n + 1 < sizeof c->input) {
                        memmove(sign + 1, sign, strlen(sign) + 1);
                        *sign = '-';
                    }
                }
                set_status(c, "Changed entry sign");
            } else if (c->depth) {
                c->stack[c->depth - 1] = -c->stack[c->depth - 1];
                set_result_status(c, c->stack[c->depth - 1]);
            } else {
                set_error(c, "stack is empty");
            }
        } else if (!c->input[0]) {
            algebraic_append(c, "-", 1);
        } else {
            char text[CALC_INPUT_MAX];
            int n = snprintf(text, sizeof text, "-(%s)", c->input);
            if (n < 0 || n >= (int)sizeof text)
                set_error(c, "input is too long");
            else {
                strlcpy(c->input, text, sizeof c->input);
                c->just_evaluated = 0;
                set_status(c, "Changed expression sign");
            }
        }
    } else if (strcmp(action, "ENTER") == 0 || strcmp(action, "=") == 0) {
        if (c->mode == MODE_RPN)
            rpn_enter(c);
        else
            algebraic_evaluate(c);
    } else if (strcmp(action, "SWAP") == 0) {
        if (c->depth < 2)
            set_error(c, "two stack values required");
        else {
            double tmp = c->stack[c->depth - 1];
            c->stack[c->depth - 1] = c->stack[c->depth - 2];
            c->stack[c->depth - 2] = tmp;
            set_result_status(c, c->stack[c->depth - 1]);
        }
    } else if (strcmp(action, "DROP") == 0) {
        if (!c->depth)
            set_error(c, "stack is empty");
        else {
            c->depth--;
            set_status(c, "Dropped X; stack depth %d", c->depth);
        }
    } else if (strcmp(action, "pi") == 0) {
        append_constant(c, "pi", M_PI);
    } else if (strcmp(action, "const_e") == 0) {
        append_constant(c, "e", M_E);
    } else if (strcmp(action, "(") == 0) {
        if (c->mode == MODE_ALGEBRAIC) {
            size_t n = strlen(c->input);
            if (n && !input_ends_operator(c->input))
                append_text(c, "*");
            algebraic_append(c, "(", 1);
        }
    } else if (strcmp(action, ")") == 0) {
        if (c->mode == MODE_ALGEBRAIC)
            algebraic_append(c, ")", 0);
    } else if (strcmp(action, "recip") == 0) {
        if (c->mode == MODE_RPN)
            rpn_unary(c, "recip");
        else if (!c->input[0] || input_ends_operator(c->input))
            algebraic_append(c, "1/(", 1);
        else {
            char text[CALC_INPUT_MAX];
            int n = snprintf(text, sizeof text, "1/(%s)", c->input);
            if (n < 0 || n >= (int)sizeof text)
                set_error(c, "input is too long");
            else {
                strlcpy(c->input, text, sizeof c->input);
                c->just_evaluated = 0;
                set_status(c, "Applied reciprocal to expression");
            }
        }
    } else if (strcmp(action, "+") == 0 || strcmp(action, "-") == 0 ||
               strcmp(action, "*") == 0 || strcmp(action, "/") == 0 ||
               strcmp(action, "%") == 0 || strcmp(action, "^") == 0) {
        if (c->mode == MODE_RPN)
            rpn_binary(c, action[0]);
        else
            algebraic_append(c, action, 0);
    } else {
        if (c->mode == MODE_RPN)
            rpn_unary(c, action);
        else
            algebraic_function(c, action);
    }
}


void calc_input_text(struct calc_state *c, const char *text)
{
    clear_error(c);
    if (c->mode == MODE_ALGEBRAIC) {
        unsigned char ch = (unsigned char)text[0];
        algebraic_append(c, text, isalnum(ch) || ch == '.' || ch == '(');
    } else if (append_text(c, text)) {
        set_status(c, "Entering RPN value");
    }
}

int calc_self_test(void)
{
    int failures = 0;
#define CHECK(condition, name) do { if (!(condition)) { printf("calc: FAIL %s\n", name); failures++; } } while (0)
#define CHECK_NEAR(value, expected, name) \
    CHECK(fabs((value) - (expected)) < 1e-12, name)
    struct calc_state c = { .mode = MODE_RPN };
    calc_reset(&c);
    calc_action(&c, "3"); calc_action(&c, "ENTER");
    calc_action(&c, "4"); calc_action(&c, "+");
    CHECK(c.depth == 1 && c.stack[0] == 7.0, "RPN addition");
    calc_action(&c, "2"); calc_action(&c, "ENTER");
    calc_action(&c, "^");
    CHECK(c.depth == 1, "RPN binary stack reduction");
    CHECK_NEAR(c.stack[0], 49.0, "RPN power");
    calc_action(&c, "sqrt");
    CHECK_NEAR(c.stack[0], 7.0, "RPN unary function");
    calc_action(&c, "ENTER"); calc_action(&c, "2"); calc_action(&c, "ENTER");
    calc_action(&c, "SWAP"); calc_action(&c, "DROP");
    CHECK(c.depth == 2 && c.stack[1] == 2.0, "RPN stack controls");

    calc_reset(&c);
    calc_input_text(&c, "1e"); calc_action(&c, "SIGN"); calc_input_text(&c, "3");
    calc_action(&c, "ENTER");
    CHECK_NEAR(c.stack[0], 0.001, "RPN signed decimal exponent");

    calc_reset(&c);
    calc_change_mode(&c, MODE_ALGEBRAIC);
    strlcpy(c.input, "2+3*4", sizeof c.input);
    CHECK(algebraic_evaluate(&c) && strcmp(c.input, "14") == 0, "operator precedence");
    strlcpy(c.input, "(2+3)^2", sizeof c.input);
    CHECK(algebraic_evaluate(&c) && strcmp(c.input, "25") == 0, "parentheses and power");
    strlcpy(c.input, "2^3^2", sizeof c.input);
    CHECK(algebraic_evaluate(&c) && strcmp(c.input, "512") == 0, "right associative power");
    strlcpy(c.input, "-2^2", sizeof c.input);
    CHECK(algebraic_evaluate(&c) && strcmp(c.input, "-4") == 0, "unary precedence");
    strlcpy(c.input, "sqrt(81)+sin(pi/2)", sizeof c.input);
    CHECK(algebraic_evaluate(&c) && fabs(strtod(c.input, NULL) - 10.0) < 1e-12,
          "functions and constants");
    strlcpy(c.input, "pow(2,8)+hypot(3,4)", sizeof c.input);
    CHECK(algebraic_evaluate(&c) && fabs(strtod(c.input, NULL) - 261.0) < 1e-12,
          "two argument functions");
    strlcpy(c.input, "4", sizeof c.input);
    calc_action(&c, "recip");
    CHECK(algebraic_evaluate(&c) && strcmp(c.input, "0.25") == 0,
          "algebraic reciprocal");
    strlcpy(c.input, "2+", sizeof c.input);
    CHECK(!algebraic_evaluate(&c) && c.error, "syntax error");
    printf("calc: self-test %d failures\n", failures);
    return failures != 0;
#undef CHECK_NEAR
#undef CHECK
}

