# make test, run as: sh /etc/tests/make.sh. Every failing check prints a
# line starting with FAIL and the value received. pdpmake treats a target
# whose time equals the time of a prerequisite as out of date, so the
# script waits between steps whose order in time matters.
check() {
    test "$2" = "$3" || echo "FAIL $1: [$2]"
}
mkdir /mk
mkdir /mk/sub
cd /mk
printf 'x\n' > a.c
printf 'y\n' > b.c
cat > Makefile <<'M'
OBJS = a.o b.o
NAME = prog
.SUFFIXES: .c .o
.c.o:
	cp $< $@
$(NAME): $(OBJS)
	cat $(OBJS) > $@
clean:
	rm $(NAME) $(OBJS)
fail:
	@false
	@echo not reached
keep:
	-@false
	@echo kept
M
make > out.txt 2>&1 || echo "FAIL make-build"
check make-result "$(cat prog | tr '\n' ' ')" "x y "
grep 'cp a.c a.o' out.txt > /dev/null || echo "FAIL make-echo-commands"
make -q; check make-q-fresh "$?" "0"
check make-up-to-date "$(make)" "make: 'prog' is up to date"
sleep 1
touch a.c
make -q; check make-q-after-touch "$?" "1"
check make-n "$(make -n | tr '\n' ' ')" "cp a.c a.o "
check make-s "$(make -s)" ""
make -q; check make-q-rebuilt "$?" "0"
make fail > /dev/null 2>&1; test $? != 0 || echo "FAIL make-error-status"
check make-ignore-prefix "$(make keep 2>/dev/null)" "kept"
check make-i "$(make -i fail 2>/dev/null)" "not reached"
make clean > /dev/null || echo "FAIL make-clean"
ls prog > /dev/null 2>&1 && echo "FAIL make-clean-result"
make NAME=other > /dev/null || echo "FAIL make-macro-arg"
ls other > /dev/null 2>&1 || echo "FAIL make-macro-arg-result"
make -t > /dev/null; ls prog > /dev/null 2>&1 || echo "FAIL make-t"
cat > macros.mk <<'M'
A = one
B = $(A) two
C := $(B)
A = changed
D ?= default
D ?= ignored
E = start
E += more
SRCS = x.c y.c z.c
all:
	@echo "$(B)|$(C)|$(D)|$(E)|$(SRCS:.c=.o)|$${TEST}"
M
check make-macros "$(make -f macros.mk)" "changed two|one two|default|start more|x.o y.o z.o|1"
check make-macro-override "$(make -f macros.mk D=given)" "changed two|one two|given|start more|x.o y.o z.o|1"
check make-e "$(E=env make -e -f macros.mk | cut -d'|' -f4)" "env"
check make-env-macro "$(E=env make -f macros.mk | cut -d'|' -f4)" "start more"
cat > cond.mk <<'M'
X = 1
ifeq ($(X),1)
R = yes
else
R = no
endif
ifdef UNDEFINED
R := $(R)-bad
endif
ifndef UNDEFINED
R := $(R)-ok
endif
all:
	@echo $(R)
M
check make-conditionals "$(make -f cond.mk)" "yes-ok"
cat > suf.mk <<'M'
.SUFFIXES: .in .txt
.in.txt:
	@cp $< $@
all: p.txt q.txt
	@echo $? $@
M
printf 'p\n' > p.in
printf 'q\n' > q.in
sleep 1
check make-suffix-rules "$(make -f suf.mk)" "p.txt q.txt all"
check make-suffix-result "$(cat p.txt q.txt | tr '\n' ' ')" "p q "
cat > inc.mk <<'M'
include macros.mk
M
check make-include "$(make -f inc.mk)" "changed two|one two|default|start more|x.o y.o z.o|1"
cat > sub/Makefile <<'M'
here:
	@pwd
M
check make-C "$(make -C sub)" "/mk/sub"
cat > dc.mk <<'M'
target::
	@echo first
target::
	@echo second
M
check make-double-colon "$(make -f dc.mk | tr '\n' ' ')" "first second "
cat > wild.mk <<'M'
all: *.in
	@echo $?
M
check make-wildcard "$(make -f wild.mk)" "p.in q.in"
cat > phony.mk <<'M'
.PHONY: all
all:
	@echo phony
M
touch all
check make-phony "$(make -f phony.mk)" "phony"
rm all
cat > silent.mk <<'M'
.SILENT:
quiet:
	echo quiet
M
check make-silent-target "$(make -f silent.mk)" "quiet"
cat > k.mk <<'M'
both: one two
one:
	@false
two:
	@echo two
M
check make-k "$(make -k -f k.mk 2>/dev/null)" "two"
make -f k.mk > /dev/null 2>&1; test $? != 0 || echo "FAIL make-S-stops"
make -f nonexistent.mk > /dev/null 2>&1; test $? != 0 || echo "FAIL make-missing-file"
make -p -f phony.mk 2>/dev/null | grep 'MAKE' > /dev/null || echo "FAIL make-p"
check make-two-files "$(make -f dc.mk -f silent.mk quiet)" "quiet"
check make-posix "$(make --posix -f silent.mk 2>&1)" "quiet"
cd /
for f in a.c b.c prog other a.o b.o Makefile macros.mk cond.mk suf.mk p.in q.in p.txt q.txt inc.mk dc.mk wild.mk phony.mk silent.mk k.mk out.txt sub/Makefile; do rm /mk/$f > /dev/null 2>&1; done
rmdir /mk/sub /mk
echo "make: done"
