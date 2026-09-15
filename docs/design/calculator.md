# Calculator

The calculator is a native `libgui` application installed as `/home/.local/bin/calc` and
listed in `/etc/launcher`.  Its default input model is reverse Polish
notation (RPN); the input-mode combo box changes the same window to an
algebraic expression editor.  The calculation state does not depend on the
windowing code, which lets the boot suite exercise it through
`calc --self-test` without starting the compositor.

## RPN mode

The RPN model has a 32-element `double` stack.  The display shows its top four
levels as T, Z, Y and X, with X at the bottom of the display.  Digits and the
decimal point form a pending value.  `ENTER` pushes that value, or duplicates
X when no value is pending.  A binary operation first pushes a pending value,
then consumes Y and X and leaves its result in X.  A unary operation replaces
X in place.

The visible binary operations are addition, subtraction, multiplication,
division, remainder and power.  The scientific keys provide trigonometric,
inverse-trigonometric, hyperbolic, logarithmic, exponential, square-root and
reciprocal operations.  The pi and e keys push constants.  `SWAP` exchanges X
and Y, `DROP` removes X, `CE` clears only pending input, and `AC` clears the
complete stack.  Division by zero, an insufficient stack, domain and range
errors leave the operands available for correction.

## Algebraic mode

Algebraic mode edits one expression and evaluates it with `ENTER`, `Eval` or
`=`.  Its recursive-descent parser implements the precedence levels below,
from lowest to highest:

1. addition and subtraction;
2. multiplication, division and remainder;
3. unary plus and minus;
4. right-associative exponentiation;
5. numbers, constants, function calls and parenthesized expressions.

Exponentiation binds more strongly than a leading sign, so `-2^2` evaluates
as `-(2^2)`, while the exponent may itself have a sign.  Parentheses override
precedence.  Decimal and hexadecimal floating constants accepted by `strtod`
are available, along with `pi` and `e`.

The parser accepts the one-argument functions `sin`, `cos`, `tan`, `asin`,
`acos`, `atan`, `sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh`, `sqrt`,
`cbrt`, `exp`, `exp2`, `ln`, `log`, `log10`, `log2`, `abs`, `floor`, `ceil`
and `round`.  It also accepts `pow`, `atan2`, `hypot`, `min`, `max`, `fmod`
and `remainder` with two comma-separated arguments.  Function keys either
insert a call at the cursor position represented by the end of the expression
or wrap the existing complete expression.

Changing from RPN to algebraic mode copies X into the expression field.
Changing back evaluates a valid expression and places the result in X.  An
invalid expression reports the parser error and preserves the earlier RPN
stack value.

## Input and error handling

The canvas retains keyboard focus after button and mode selections.  In RPN
mode the keyboard accepts digits, decimal input, arithmetic operators,
signed decimal exponents, backspace and Enter.  Algebraic mode additionally
accepts identifiers, parentheses, commas and spaces.  Escape clears the
current calculator model.
All display formatting uses 15 significant decimal digits, while computation
uses the libc `double` math interface and therefore remains entirely in user
space.  No floating-point operation enters the kernel.

`tests/cases/calculator` runs the headless self-test.  It covers RPN stack
entry, binary and unary operations, stack manipulation, algebraic precedence,
right-associative powers, signed powers, constants, one- and two-argument
functions, and syntax-error reporting.  `tests/cases/gui_calc` starts the
compositor and the real application, computes an RPN expression through the
keyboard, changes the combo box to algebraic mode, evaluates a precedence
expression and closes the window through the window manager.
