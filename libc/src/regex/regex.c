#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

enum node_type {
    NODE_EMPTY,
    NODE_CHAR,
    NODE_DOT,
    NODE_CLASS,
    NODE_BOL,
    NODE_EOL,
    NODE_SEQUENCE,
    NODE_ALTERNATE,
    NODE_REPEAT,
    NODE_GROUP,
    NODE_BACKREF,
};

struct node {
    enum node_type type;
    int left;
    int right;
    int minimum;
    int maximum;
    unsigned group;
    unsigned char character;
    unsigned char negated;
    unsigned char bits[32];
};

struct parser {
    const char *pattern;
    size_t length;
    size_t position;
    int flags;
    int error;
    size_t groups;
    struct node *nodes;
    size_t count;
    size_t capacity;
};

static int add_node(struct parser *parser, enum node_type type)
{
    if (parser->count == parser->capacity) {
        size_t capacity = parser->capacity ? parser->capacity * 2 : 32;
        if (capacity < parser->capacity || capacity > 65536) {
            parser->error = REG_ESIZE;
            return -1;
        }
        struct node *nodes = realloc(parser->nodes, capacity * sizeof *nodes);
        if (!nodes) {
            parser->error = REG_ESPACE;
            return -1;
        }
        parser->nodes = nodes;
        parser->capacity = capacity;
    }
    int index = (int)parser->count++;
    memset(&parser->nodes[index], 0, sizeof parser->nodes[index]);
    parser->nodes[index].type = type;
    parser->nodes[index].left = -1;
    parser->nodes[index].right = -1;
    return index;
}

static int extended(const struct parser *parser)
{
    return (parser->flags & REG_EXTENDED) != 0;
}

static int at_pair(const struct parser *parser, char second)
{
    return parser->position + 1 < parser->length &&
           parser->pattern[parser->position] == '\\' &&
           parser->pattern[parser->position + 1] == second;
}

static int at_alternate(const struct parser *parser)
{
    return extended(parser) ?
        parser->position < parser->length && parser->pattern[parser->position] == '|' :
        at_pair(parser, '|');
}

static int at_close(const struct parser *parser)
{
    return extended(parser) ?
        parser->position < parser->length && parser->pattern[parser->position] == ')' :
        at_pair(parser, ')');
}

static void skip_operator(struct parser *parser)
{
    parser->position += extended(parser) ? 1 : 2;
}

static unsigned char fold(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

static void class_add(struct parser *parser, struct node *node, unsigned char c)
{
    node->bits[c >> 3] |= (unsigned char)(1u << (c & 7));
    if (parser->flags & REG_ICASE) {
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        else if (c >= 'a' && c <= 'z')
            c = (unsigned char)(c - 'a' + 'A');
        node->bits[c >> 3] |= (unsigned char)(1u << (c & 7));
    }
}

static void class_range(struct parser *parser, struct node *node,
                        unsigned char first, unsigned char last)
{
    for (unsigned c = first; c <= last; c++)
        class_add(parser, node, (unsigned char)c);
}

static int class_named(struct parser *parser, struct node *node,
                       const char *name, size_t length)
{
    for (unsigned c = 0; c < 256; c++) {
        int member = 0;
#define CLASS(name_text, expression) \
        if (length == sizeof(name_text) - 1 && \
            memcmp(name, name_text, sizeof(name_text) - 1) == 0) member = (expression)
        CLASS("alnum", isalnum((int)c));
        else CLASS("alpha", isalpha((int)c));
        else CLASS("blank", c == ' ' || c == '\t');
        else CLASS("cntrl", iscntrl((int)c));
        else CLASS("digit", isdigit((int)c));
        else CLASS("graph", isprint((int)c) && c != ' ');
        else CLASS("lower", islower((int)c));
        else CLASS("print", isprint((int)c));
        else CLASS("punct", ispunct((int)c));
        else CLASS("space", isspace((int)c));
        else CLASS("upper", isupper((int)c));
        else CLASS("xdigit", isxdigit((int)c));
        else {
            parser->error = REG_ECTYPE;
            return 0;
        }
#undef CLASS
        if (member)
            class_add(parser, node, (unsigned char)c);
    }
    return 1;
}

static int parse_class(struct parser *parser)
{
    int index = add_node(parser, NODE_CLASS);
    if (index < 0)
        return -1;
    struct node *node = &parser->nodes[index];
    parser->position++;
    if (parser->position < parser->length && parser->pattern[parser->position] == '^') {
        node->negated = 1;
        parser->position++;
    }

    int first_item = 1;
    while (parser->position < parser->length) {
        if (parser->pattern[parser->position] == ']' && !first_item) {
            parser->position++;
            return index;
        }
        first_item = 0;

        if (parser->position + 1 < parser->length &&
            parser->pattern[parser->position] == '[' &&
            parser->pattern[parser->position + 1] == ':') {
            size_t name = parser->position + 2;
            size_t end = name;
            while (end + 1 < parser->length &&
                   !(parser->pattern[end] == ':' && parser->pattern[end + 1] == ']'))
                end++;
            if (end + 1 >= parser->length) {
                parser->error = REG_EBRACK;
                return -1;
            }
            if (!class_named(parser, node, parser->pattern + name, end - name))
                return -1;
            parser->position = end + 2;
            continue;
        }
        if (parser->position + 1 < parser->length &&
            parser->pattern[parser->position] == '[' &&
            (parser->pattern[parser->position + 1] == '.' ||
             parser->pattern[parser->position + 1] == '=')) {
            parser->error = REG_ECOLLATE;
            return -1;
        }

        unsigned char begin;
        if (parser->pattern[parser->position] == '\\' &&
            parser->position + 1 < parser->length) {
            parser->position++;
            begin = (unsigned char)parser->pattern[parser->position++];
        } else {
            begin = (unsigned char)parser->pattern[parser->position++];
        }

        if (parser->position < parser->length &&
            parser->pattern[parser->position] == '-' &&
            parser->position + 1 < parser->length &&
            parser->pattern[parser->position + 1] != ']') {
            parser->position++;
            unsigned char end;
            if (parser->pattern[parser->position] == '\\' &&
                parser->position + 1 < parser->length)
                parser->position++;
            end = (unsigned char)parser->pattern[parser->position++];
            if (end < begin) {
                parser->error = REG_ERANGE;
                return -1;
            }
            class_range(parser, node, begin, end);
        } else {
            class_add(parser, node, begin);
        }
    }
    parser->error = REG_EBRACK;
    return -1;
}

static int parse_alternate(struct parser *parser);

static int parse_atom(struct parser *parser)
{
    if (parser->position >= parser->length) {
        parser->error = REG_BADPAT;
        return -1;
    }

    int group_open = extended(parser) ? parser->pattern[parser->position] == '(' :
                                       at_pair(parser, '(');
    if (group_open) {
        skip_operator(parser);
        unsigned group = (unsigned)++parser->groups;
        int child = parse_alternate(parser);
        if (child < 0)
            return -1;
        if (!at_close(parser)) {
            parser->error = REG_EPAREN;
            return -1;
        }
        skip_operator(parser);
        int index = add_node(parser, NODE_GROUP);
        if (index >= 0) {
            parser->nodes[index].left = child;
            parser->nodes[index].group = group;
        }
        return index;
    }

    unsigned char c = (unsigned char)parser->pattern[parser->position++];
    if (c == '[') {
        parser->position--;
        return parse_class(parser);
    }
    if (c == '.')
        return add_node(parser, NODE_DOT);
    if (c == '^')
        return add_node(parser, NODE_BOL);
    if (c == '$')
        return add_node(parser, NODE_EOL);

    if (c == '\\') {
        if (parser->position >= parser->length) {
            parser->error = REG_EESCAPE;
            return -1;
        }
        c = (unsigned char)parser->pattern[parser->position++];
        if (!extended(parser) && c >= '1' && c <= '9') {
            unsigned group = c - '0';
            if (group > parser->groups) {
                parser->error = REG_ESUBREG;
                return -1;
            }
            int index = add_node(parser, NODE_BACKREF);
            if (index >= 0)
                parser->nodes[index].group = group;
            return index;
        }
    }

    int index = add_node(parser, NODE_CHAR);
    if (index >= 0)
        parser->nodes[index].character = c;
    return index;
}

static int quantifier_at(const struct parser *parser)
{
    if (parser->position >= parser->length)
        return 0;
    char c = parser->pattern[parser->position];
    if (c == '*')
        return 1;
    if (extended(parser))
        return c == '+' || c == '?' || c == '{';
    return at_pair(parser, '+') || at_pair(parser, '?') || at_pair(parser, '{');
}

static int parse_decimal(struct parser *parser, int *value)
{
    if (parser->position >= parser->length ||
        !isdigit((unsigned char)parser->pattern[parser->position]))
        return 0;
    unsigned number = 0;
    while (parser->position < parser->length &&
           isdigit((unsigned char)parser->pattern[parser->position])) {
        number = number * 10 + (unsigned)(parser->pattern[parser->position++] - '0');
        if (number > RE_DUP_MAX) {
            parser->error = REG_BADBR;
            return 0;
        }
    }
    *value = (int)number;
    return 1;
}

static int bound_close(const struct parser *parser)
{
    return extended(parser) ?
        parser->position < parser->length && parser->pattern[parser->position] == '}' :
        at_pair(parser, '}');
}

static void skip_bound_close(struct parser *parser)
{
    parser->position += extended(parser) ? 1 : 2;
}

static int parse_piece(struct parser *parser)
{
    if (quantifier_at(parser)) {
        parser->error = REG_BADRPT;
        return -1;
    }
    int child = parse_atom(parser);
    if (child < 0 || !quantifier_at(parser))
        return child;

    int minimum, maximum;
    char c = parser->pattern[parser->position];
    if (c == '*') {
        parser->position++;
        minimum = 0;
        maximum = -1;
    } else {
        if (!extended(parser))
            parser->position++;
        c = parser->pattern[parser->position++];
        if (c == '+') {
            minimum = 1;
            maximum = -1;
        } else if (c == '?') {
            minimum = 0;
            maximum = 1;
        } else {
            if (!parse_decimal(parser, &minimum)) {
                if (!parser->error)
                    parser->error = REG_BADBR;
                return -1;
            }
            if (parser->position < parser->length && parser->pattern[parser->position] == ',') {
                parser->position++;
                if (bound_close(parser))
                    maximum = -1;
                else if (!parse_decimal(parser, &maximum)) {
                    if (!parser->error)
                        parser->error = REG_BADBR;
                    return -1;
                }
            } else {
                maximum = minimum;
            }
            if (!bound_close(parser)) {
                parser->error = REG_EBRACE;
                return -1;
            }
            skip_bound_close(parser);
            if (maximum >= 0 && maximum < minimum) {
                parser->error = REG_BADBR;
                return -1;
            }
        }
    }
    if (quantifier_at(parser)) {
        parser->error = REG_BADRPT;
        return -1;
    }
    int index = add_node(parser, NODE_REPEAT);
    if (index >= 0) {
        parser->nodes[index].left = child;
        parser->nodes[index].minimum = minimum;
        parser->nodes[index].maximum = maximum;
    }
    return index;
}

static int parse_sequence(struct parser *parser)
{
    int sequence = -1;
    while (parser->position < parser->length &&
           !at_alternate(parser) && !at_close(parser)) {
        int piece = parse_piece(parser);
        if (piece < 0)
            return -1;
        if (sequence < 0) {
            sequence = piece;
        } else {
            int joined = add_node(parser, NODE_SEQUENCE);
            if (joined < 0)
                return -1;
            parser->nodes[joined].left = sequence;
            parser->nodes[joined].right = piece;
            sequence = joined;
        }
    }
    return sequence >= 0 ? sequence : add_node(parser, NODE_EMPTY);
}

static int parse_alternate(struct parser *parser)
{
    int left = parse_sequence(parser);
    if (left < 0)
        return -1;
    while (at_alternate(parser)) {
        skip_operator(parser);
        int right = parse_sequence(parser);
        if (right < 0)
            return -1;
        int alternate = add_node(parser, NODE_ALTERNATE);
        if (alternate < 0)
            return -1;
        parser->nodes[alternate].left = left;
        parser->nodes[alternate].right = right;
        left = alternate;
    }
    return left;
}

enum opcode {
    OP_CHAR,
    OP_DOT,
    OP_CLASS,
    OP_BOL,
    OP_EOL,
    OP_SAVE,
    OP_BACKREF,
    OP_SPLIT,
    OP_REPEAT,
    OP_MATCH,
};

struct instruction {
    enum opcode opcode;
    int next;
    int alternate;
    unsigned argument;
    unsigned char character;
    unsigned char negated;
    unsigned char bits[32];
};

struct compiled_regex {
    struct instruction *code;
    size_t count;
    size_t capacity;
    size_t slots;
    int start;
    int flags;
};

static int emit(struct compiled_regex *compiled, enum opcode opcode)
{
    if (compiled->count == compiled->capacity) {
        size_t capacity = compiled->capacity ? compiled->capacity * 2 : 64;
        if (capacity < compiled->capacity || capacity > 262144)
            return -1;
        struct instruction *code = realloc(compiled->code, capacity * sizeof *code);
        if (!code)
            return -1;
        compiled->code = code;
        compiled->capacity = capacity;
    }
    int index = (int)compiled->count++;
    memset(&compiled->code[index], 0, sizeof compiled->code[index]);
    compiled->code[index].opcode = opcode;
    compiled->code[index].next = -1;
    compiled->code[index].alternate = -1;
    return index;
}

static int compile_node(const struct parser *parser, int node_index,
                        struct compiled_regex *compiled, int next)
{
    const struct node *node = &parser->nodes[node_index];
    int index, right, left;
    switch (node->type) {
    case NODE_EMPTY:
        return next;
    case NODE_CHAR:
        index = emit(compiled, OP_CHAR);
        if (index >= 0) {
            compiled->code[index].character = node->character;
            compiled->code[index].next = next;
        }
        return index;
    case NODE_DOT:
        index = emit(compiled, OP_DOT);
        if (index >= 0)
            compiled->code[index].next = next;
        return index;
    case NODE_CLASS:
        index = emit(compiled, OP_CLASS);
        if (index >= 0) {
            compiled->code[index].negated = node->negated;
            memcpy(compiled->code[index].bits, node->bits, sizeof node->bits);
            compiled->code[index].next = next;
        }
        return index;
    case NODE_BOL:
        index = emit(compiled, OP_BOL);
        if (index >= 0)
            compiled->code[index].next = next;
        return index;
    case NODE_EOL:
        index = emit(compiled, OP_EOL);
        if (index >= 0)
            compiled->code[index].next = next;
        return index;
    case NODE_SEQUENCE:
        right = compile_node(parser, node->right, compiled, next);
        if (right < 0)
            return -1;
        return compile_node(parser, node->left, compiled, right);
    case NODE_ALTERNATE:
        left = compile_node(parser, node->left, compiled, next);
        if (left < 0)
            return -1;
        right = compile_node(parser, node->right, compiled, next);
        if (right < 0)
            return -1;
        index = emit(compiled, OP_SPLIT);
        if (index >= 0) {
            compiled->code[index].next = left;
            compiled->code[index].alternate = right;
        }
        return index;
    case NODE_REPEAT: {
        int current = next;
        if (node->maximum < 0) {
            int split = emit(compiled, OP_REPEAT);
            if (split < 0)
                return -1;
            int body = compile_node(parser, node->left, compiled, split);
            if (body < 0)
                return -1;
            compiled->code[split].next = body;
            compiled->code[split].alternate = current;
            current = split;
        } else {
            for (int n = node->maximum; n > node->minimum; n--) {
                int body = compile_node(parser, node->left, compiled, current);
                if (body < 0)
                    return -1;
                int split = emit(compiled, OP_SPLIT);
                if (split < 0)
                    return -1;
                compiled->code[split].next = body;
                compiled->code[split].alternate = current;
                current = split;
            }
        }
        for (int n = 0; n < node->minimum; n++) {
            current = compile_node(parser, node->left, compiled, current);
            if (current < 0)
                return -1;
        }
        return current;
    }
    case NODE_GROUP:
        index = emit(compiled, OP_SAVE);
        if (index < 0)
            return -1;
        compiled->code[index].argument = node->group * 2 + 1;
        compiled->code[index].next = next;
        left = compile_node(parser, node->left, compiled, index);
        if (left < 0)
            return -1;
        index = emit(compiled, OP_SAVE);
        if (index >= 0) {
            compiled->code[index].argument = node->group * 2;
            compiled->code[index].next = left;
        }
        return index;
    case NODE_BACKREF:
        index = emit(compiled, OP_BACKREF);
        if (index >= 0) {
            compiled->code[index].argument = node->group;
            compiled->code[index].next = next;
        }
        return index;
    }
    return -1;
}

int regcomp(regex_t *regex, const char *pattern, int flags)
{
    if (!regex || !pattern)
        return REG_BADPAT;
    regex->re_nsub = 0;
    regex->__compiled = 0;
    regex->__cflags = flags;

    struct parser parser = {
        .pattern = pattern,
        .length = strlen(pattern),
        .flags = flags,
    };
    int root = parse_alternate(&parser);
    if (root >= 0 && parser.position != parser.length)
        parser.error = REG_EPAREN;
    if (root < 0 || parser.error) {
        int error = parser.error ? parser.error : REG_BADPAT;
        free(parser.nodes);
        return error;
    }

    struct compiled_regex *compiled = calloc(1, sizeof *compiled);
    if (!compiled) {
        free(parser.nodes);
        return REG_ESPACE;
    }
    compiled->flags = flags;
    compiled->slots = (parser.groups + 1) * 2;
    int match = emit(compiled, OP_MATCH);
    int end = match < 0 ? -1 : emit(compiled, OP_SAVE);
    if (end >= 0) {
        compiled->code[end].argument = 1;
        compiled->code[end].next = match;
    }
    int body = end < 0 ? -1 : compile_node(&parser, root, compiled, end);
    int start = body < 0 ? -1 : emit(compiled, OP_SAVE);
    if (start >= 0) {
        compiled->code[start].argument = 0;
        compiled->code[start].next = body;
        compiled->start = start;
    }
    free(parser.nodes);
    if (start < 0) {
        free(compiled->code);
        free(compiled);
        return REG_ESPACE;
    }
    regex->re_nsub = parser.groups;
    regex->__compiled = compiled;
    return 0;
}

struct choice {
    int instruction;
    size_t position;
    regoff_t *captures;
};

static int push_choice(struct choice **choices, size_t *count, size_t *capacity,
                       int instruction, size_t position, const regoff_t *captures,
                       size_t slots)
{
    if (*count == *capacity) {
        size_t grown_capacity = *capacity ? *capacity * 2 : 32;
        struct choice *grown = realloc(*choices, grown_capacity * sizeof *grown);
        if (!grown)
            return 0;
        *choices = grown;
        *capacity = grown_capacity;
    }
    regoff_t *copy = malloc(slots * sizeof *copy);
    if (!copy)
        return 0;
    memcpy(copy, captures, slots * sizeof *copy);
    (*choices)[*count] = (struct choice) { instruction, position, copy };
    (*count)++;
    return 1;
}

static void free_choices(struct choice *choices, size_t count)
{
    while (count)
        free(choices[--count].captures);
    free(choices);
}

static int class_contains(const struct instruction *instruction, unsigned char c)
{
    int member = (instruction->bits[c >> 3] & (1u << (c & 7))) != 0;
    return instruction->negated ? !member : member;
}

static int bytes_equal(const char *a, const char *b, size_t n, int insensitive)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i];
        unsigned char y = (unsigned char)b[i];
        if (insensitive) {
            x = fold(x);
            y = fold(y);
        }
        if (x != y)
            return 0;
    }
    return 1;
}

static int run_at(const struct compiled_regex *compiled, const char *string,
                  size_t begin, size_t end, size_t start, int flags,
                  regoff_t *result)
{
    size_t span = end - begin + 1;
    if (compiled->count > ((size_t)-1 - 7) / span)
        return REG_ESPACE;
    size_t repeat_bytes = (compiled->count * span + 7) / 8;
    regoff_t *captures = malloc(compiled->slots * sizeof *captures);
    regoff_t *best = malloc(compiled->slots * sizeof *best);
    unsigned char *repeat_seen = calloc(repeat_bytes, 1);
    if (!captures || !best || !repeat_seen) {
        free(captures);
        free(best);
        free(repeat_seen);
        return REG_ESPACE;
    }
    for (size_t i = 0; i < compiled->slots; i++)
        captures[i] = best[i] = -1;

    struct choice *choices = 0;
    size_t choice_count = 0, choice_capacity = 0;
    int instruction = compiled->start;
    size_t position = start;
    regoff_t best_end = -1;
    size_t limit = compiled->count > 10000000 / (span ? span : 1) ?
                   10000000 : compiled->count * span * 64 + 10000;
    if (limit > 10000000)
        limit = 10000000;
    size_t steps = 0;
    int exhausted = 0;

    for (;;) {
        if (++steps > limit) {
            exhausted = 1;
            break;
        }
        const struct instruction *op = &compiled->code[instruction];
        switch (op->opcode) {
        case OP_CHAR:
            if (position >= end ||
                ((compiled->flags & REG_ICASE) ?
                 fold((unsigned char)string[position]) != fold(op->character) :
                 (unsigned char)string[position] != op->character))
                goto backtrack;
            position++;
            instruction = op->next;
            continue;
        case OP_DOT:
            if (position >= end ||
                ((compiled->flags & REG_NEWLINE) && string[position] == '\n'))
                goto backtrack;
            position++;
            instruction = op->next;
            continue;
        case OP_CLASS:
            if (position >= end || !class_contains(op, (unsigned char)string[position]) ||
                ((compiled->flags & REG_NEWLINE) && op->negated &&
                 string[position] == '\n'))
                goto backtrack;
            position++;
            instruction = op->next;
            continue;
        case OP_BOL:
            if (!((position == begin && !(flags & REG_NOTBOL)) ||
                  ((compiled->flags & REG_NEWLINE) && position > begin &&
                   string[position - 1] == '\n')))
                goto backtrack;
            instruction = op->next;
            continue;
        case OP_EOL:
            if (!((position == end && !(flags & REG_NOTEOL)) ||
                  ((compiled->flags & REG_NEWLINE) && position < end &&
                   string[position] == '\n')))
                goto backtrack;
            instruction = op->next;
            continue;
        case OP_SAVE:
            if (op->argument < compiled->slots)
                captures[op->argument] = (regoff_t)position;
            instruction = op->next;
            continue;
        case OP_BACKREF: {
            size_t slot = op->argument * 2;
            if (slot + 1 >= compiled->slots || captures[slot] < 0 ||
                captures[slot + 1] < captures[slot])
                goto backtrack;
            size_t length = (size_t)(captures[slot + 1] - captures[slot]);
            if (length > end - position ||
                !bytes_equal(string + captures[slot], string + position, length,
                             (compiled->flags & REG_ICASE) != 0))
                goto backtrack;
            position += length;
            instruction = op->next;
            continue;
        }
        case OP_SPLIT:
            if (!push_choice(&choices, &choice_count, &choice_capacity,
                             op->alternate, position, captures, compiled->slots)) {
                exhausted = 1;
                goto finished;
            }
            instruction = op->next;
            continue;
        case OP_REPEAT: {
            /* A nullable repeated expression can return to its split
             * without consuming input.  Taking the exit on that revisit
             * preserves the language while preventing an epsilon cycle. */
            size_t repeat_bit = (size_t)instruction * span + position - begin;
            unsigned char repeat_mask = (unsigned char)(1u << (repeat_bit & 7));
            if (repeat_seen[repeat_bit >> 3] & repeat_mask) {
                instruction = op->alternate;
                continue;
            }
            repeat_seen[repeat_bit >> 3] |= repeat_mask;
            if (!push_choice(&choices, &choice_count, &choice_capacity,
                             op->alternate, position, captures, compiled->slots)) {
                exhausted = 1;
                goto finished;
            }
            instruction = op->next;
            continue;
        }
        case OP_MATCH:
            if ((regoff_t)position > best_end) {
                best_end = (regoff_t)position;
                memcpy(best, captures, compiled->slots * sizeof *best);
            }
            goto backtrack;
        }

backtrack:
        if (!choice_count)
            break;
        choice_count--;
        instruction = choices[choice_count].instruction;
        position = choices[choice_count].position;
        memcpy(captures, choices[choice_count].captures,
               compiled->slots * sizeof *captures);
        free(choices[choice_count].captures);
    }

finished:
    free_choices(choices, choice_count);
    free(captures);
    free(repeat_seen);
    if (exhausted) {
        free(best);
        return REG_ESPACE;
    }
    if (best_end < 0) {
        free(best);
        return REG_NOMATCH;
    }
    memcpy(result, best, compiled->slots * sizeof *result);
    free(best);
    return 0;
}

int regexec(const regex_t *regex, const char *string, size_t nmatch,
            regmatch_t matches[], int flags)
{
    if (!regex || !regex->__compiled || !string)
        return REG_BADPAT;
    const struct compiled_regex *compiled = regex->__compiled;
    size_t begin = 0;
    size_t end;
    if (flags & REG_STARTEND) {
        if (!matches || !nmatch || matches[0].rm_so < 0 ||
            matches[0].rm_eo < matches[0].rm_so)
            return REG_BADPAT;
        begin = (size_t)matches[0].rm_so;
        end = (size_t)matches[0].rm_eo;
    } else {
        end = strlen(string);
    }

    regoff_t *captures = malloc(compiled->slots * sizeof *captures);
    if (!captures)
        return REG_ESPACE;
    int status = REG_NOMATCH;
    for (size_t start = begin; start <= end; start++) {
        status = run_at(compiled, string, begin, end, start, flags, captures);
        if (status == 0)
            break;
        if (status != REG_NOMATCH)
            break;
    }
    if (status == 0 && !(compiled->flags & REG_NOSUB) && matches) {
        size_t groups = regex->re_nsub + 1;
        size_t count = nmatch < groups ? nmatch : groups;
        for (size_t i = 0; i < count; i++) {
            matches[i].rm_so = captures[i * 2];
            matches[i].rm_eo = captures[i * 2 + 1];
        }
        for (size_t i = count; i < nmatch; i++) {
            matches[i].rm_so = -1;
            matches[i].rm_eo = -1;
        }
    }
    free(captures);
    return status;
}

size_t regerror(int error, const regex_t *regex, char *buffer, size_t size)
{
    static const char *const messages[] = {
        "success",
        "no match",
        "invalid regular expression",
        "invalid collating element",
        "invalid character class",
        "trailing backslash",
        "invalid back reference",
        "unmatched bracket",
        "unmatched parenthesis",
        "unmatched brace",
        "invalid repetition count",
        "invalid character range",
        "out of memory",
        "repetition operator has no operand",
        "empty expression",
        "regular expression is too large",
    };
    const char *message = error >= 0 &&
        (size_t)error < sizeof messages / sizeof messages[0] ?
        messages[error] : "unknown regular expression error";
    size_t required = strlen(message) + 1;
    if (buffer && size) {
        size_t copy = required < size ? required : size;
        memcpy(buffer, message, copy - 1);
        buffer[copy - 1] = 0;
    }
    return required;
}

void regfree(regex_t *regex)
{
    if (!regex || !regex->__compiled)
        return;
    struct compiled_regex *compiled = regex->__compiled;
    free(compiled->code);
    free(compiled);
    regex->__compiled = 0;
    regex->re_nsub = 0;
}
