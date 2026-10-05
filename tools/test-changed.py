#!/usr/bin/env python3
"""Select the tests of the changed files (docs/design/build.md, Tests).

    tools/test-changed.py [--since REF] [--run]
    tools/test-changed.py --check

The changed files are the differences of the working tree from HEAD, the
untracked files that git does not ignore, and with --since also the commits
since the common ancestor with REF. The script prints the selected host
checks and the boot cases of each architecture, and a warning for each
changed file without a rule. With --run it runs the
host checks and then one make test per architecture. --check lists every
tracked file that no rule covers and fails when the list is not empty.

A changed file selects tests in three ways:

1. The files of a case select the case: the directory tests/cases/CASE,
   the kernel test file with KTEST_DEFINE("CASE"), the program of
   user/tests/ and the script of user/etc/tests/ that its cmdline runs. A
   header in kernel/tests/ selects the cases of every test file that
   includes the header.
2. The rules of tests/map. A rule is a path pattern and its targets.
3. The target @program of a rule selects the cases whose files run the
   program that the changed file builds: user/coreutils/nm.c builds
   /bin/nm, user/apps/clock.c builds /bin/clock.
4. A fixture in user/etc/tests/ selects the cases whose files contain the
   file name of the fixture.

In a path pattern "**" matches any number of directories and "*" any
characters except "/". A target is a pattern of case names, a host check
"check-NAME" (a target of the top-level Makefile), "aarch64:PATTERN" for
cases that run on aarch64, "@program", "@run" for the cases that the
kernel test "run" starts (test=run), or "none" for a file without tests.
Every rule that matches a file contributes its targets. A case whose
arches file excludes x86_64 runs on its first architecture.
"""
import argparse
import fnmatch
import os
import re
import subprocess
import sys

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CASES = os.path.join(TOP, "tests", "cases")
MAP = os.path.join(TOP, "tests", "map")
PROGRAM_DIRS = ("user/coreutils/", "user/apps/", "user/binutils/", "user/tests/")


def git(*args):
    command = ["git", "-C", TOP, "-c", "core.quotepath=off"] + list(args)
    out = subprocess.run(command, check=True, capture_output=True, text=True).stdout
    return [line for line in out.splitlines() if line]


def pattern_regex(pattern):
    """Translate a path pattern with ** and * into a regular expression."""
    out, i = "", 0
    while i < len(pattern):
        if pattern.startswith("**/", i):
            out += "(?:.*/)?"
            i += 3
        elif pattern.startswith("**", i):
            out += ".*"
            i += 2
        elif pattern[i] == "*":
            out += "[^/]*"
            i += 1
        elif pattern[i] == "?":
            out += "[^/]"
            i += 1
        else:
            out += re.escape(pattern[i])
            i += 1
    return re.compile(out + r"\Z")


def read_map():
    rules = []
    with open(MAP) as f:
        for number, line in enumerate(f, 1):
            line = line.split("#", 1)[0].split()
            if not line:
                continue
            if len(line) < 2:
                sys.exit(f"tests/map:{number}: a rule needs a pattern and at least one target")
            rules.append((pattern_regex(line[0]), line[1:], number))
    return rules


class Cases:
    """The cases with their files, programs and architectures."""

    def __init__(self):
        self.names = sorted(d for d in os.listdir(CASES) if os.path.isdir(os.path.join(CASES, d)))
        self.files = {}         # path -> set of cases
        self.programs = {}      # case -> set of program names its files run
        self.texts = {}         # case -> the text of its files
        self.run_cases = set()  # the cases with test=run
        self.arches = {}
        self.header_users = {}  # kernel/tests header -> test files that include it
        ktest_files = {}        # case -> kernel test files
        for name in sorted(os.listdir(os.path.join(TOP, "kernel", "tests"))):
            path = "kernel/tests/" + name
            if path.endswith(".c"):
                with open(os.path.join(TOP, path), errors="replace") as f:
                    text = f.read()
                for name in re.findall(r'KTEST_DEFINE(?:_STAGE)?\(\s*"([^"]+)"', text):
                    ktest_files.setdefault(name, []).append(path)
                for header in re.findall(r'#include\s+"([^"]+\.h)"', text):
                    self.header_users.setdefault("kernel/tests/" + header, set()).add(path)
        self.ktest_files = ktest_files
        for case in self.names:
            own = set()
            directory = os.path.join(CASES, case)
            for root, _, names in os.walk(directory):
                for name in names:
                    own.add(os.path.relpath(os.path.join(root, name), TOP))
            text = ""
            for name in os.listdir(directory):
                path = os.path.join(directory, name)
                if os.path.isfile(path):
                    with open(path, errors="replace") as f:
                        text += f.read() + "\n"
            cmdline = ""
            if os.path.exists(os.path.join(directory, "cmdline")):
                with open(os.path.join(directory, "cmdline")) as f:
                    cmdline = f.read()
            test = re.search(r"\btest=(\S+)", cmdline)
            if test and test.group(1) == "run":
                self.run_cases.add(case)
            if test and test.group(1) != "run":
                own.update(ktest_files.get(test.group(1), []))
            elif not test:
                # A case without test= in a cmdline, such as a boot of the
                # installation medium, runs the kernel test of its own name.
                own.update(ktest_files.get(case, []))
            for prog in re.findall(r"\bprog=/bin/(\S+)", cmdline):
                if os.path.exists(os.path.join(TOP, "user/tests", prog + ".c")):
                    own.add("user/tests/" + prog + ".c")
            for script in re.findall(r"/etc/tests/(\S+)", cmdline):
                own.add("user/etc/tests/" + script)
            for path in own:
                if path.endswith((".c", ".sh")) and os.path.exists(os.path.join(TOP, path)):
                    with open(os.path.join(TOP, path), errors="replace") as f:
                        text += f.read()
            self.texts[case] = text
            self.programs[case] = set(re.findall(r"/bin/([A-Za-z0-9_.+-]+)", text)) | \
                set(re.findall(r"^\s*([A-Za-z0-9_.+-]+)\b", text, re.M))
            for path in own:
                self.files.setdefault(path, set()).add(case)
            arches = os.path.join(directory, "arches")
            if os.path.exists(arches):
                with open(arches) as f:
                    self.arches[case] = f.read().split()
            else:
                self.arches[case] = ["x86_64", "aarch64"]

    def own_cases(self, path):
        cases = set(self.files.get(path, set()))
        for test_file in self.header_users.get(path, set()):
            cases |= self.files.get(test_file, set())
        if path.startswith("user/etc/tests/"):
            name = os.path.basename(path)
            cases |= {case for case, text in self.texts.items() if name in text}
        return cases

    def program_cases(self, path):
        if not path.startswith(PROGRAM_DIRS) or not path.endswith(".c"):
            return set()
        program = os.path.basename(path)[:-2]
        return {case for case, programs in self.programs.items() if program in programs}

    def expand(self, pattern):
        return {name for name in self.names if fnmatch.fnmatchcase(name, pattern)}


def select(paths, rules, cases):
    """Return the host checks, the cases per architecture and the files without a rule."""
    checks, selected, unmatched = set(), {"x86_64": set(), "aarch64": set()}, []
    for path in paths:
        found = cases.own_cases(path)
        covered = bool(found)
        for case in found:
            selected["x86_64"].add(case)
        for regex, targets, _ in rules:
            if not regex.match(path):
                continue
            covered = True
            for target in targets:
                if target == "none":
                    continue
                if target.startswith("check-"):
                    checks.add(target)
                elif target == "@run":
                    selected["x86_64"].update(cases.run_cases)
                elif target == "@program":
                    selected["x86_64"].update(cases.program_cases(path))
                elif target.startswith("aarch64:"):
                    selected["aarch64"].update(cases.expand(target[len("aarch64:"):]))
                else:
                    expanded = cases.expand(target)
                    if not expanded:
                        sys.exit(f"tests/map: the target {target} matches no case")
                    selected["x86_64"].update(expanded)
        if not covered:
            unmatched.append(path)
    # A case that does not run on x86_64 moves to its architecture.
    for case in list(selected["x86_64"]):
        if "x86_64" not in cases.arches[case]:
            selected["x86_64"].discard(case)
            selected[cases.arches[case][0]].add(case)
    return checks, selected, unmatched


def changed_files(since):
    paths = set(git("diff", "--name-only", "HEAD"))
    paths |= set(git("ls-files", "--others", "--exclude-standard"))
    if since:
        paths |= set(git("diff", "--name-only", since + "...HEAD"))
    return sorted(paths)


def main():
    parser = argparse.ArgumentParser(description="Select the tests of the changed files.")
    parser.add_argument("--since", help="also include the commits since the common ancestor with this ref")
    parser.add_argument("--run", action="store_true", help="run the selected checks and cases")
    parser.add_argument("--check", action="store_true", help="list the tracked files that no rule covers")
    args = parser.parse_args()
    rules = read_map()
    cases = Cases()
    if args.check:
        _, _, unmatched = select(git("ls-files"), rules, cases)
        for path in unmatched:
            print(f"no rule: {path}")
        print(f"test-changed: {len(unmatched)} tracked files without a rule")
        return 1 if unmatched else 0
    paths = changed_files(args.since)
    checks, selected, unmatched = select(paths, rules, cases)
    print(f"test-changed: {len(paths)} changed files")
    for path in unmatched:
        print(f"test-changed: warning: no rule in tests/map for {path}")
    print("host checks: " + (" ".join(sorted(checks)) or "none"))
    for arch in ("x86_64", "aarch64"):
        print(f"cases {arch}: " + (" ".join(sorted(selected[arch])) or "none"))
    if not args.run:
        return 0
    for check in sorted(checks):
        if subprocess.run(["make", "-C", TOP, check]).returncode != 0:
            return 1
    status = 0
    for arch in ("x86_64", "aarch64"):
        if selected[arch]:
            command = ["make", "-C", TOP, "ARCH=" + arch, "test", "CASES=" + " ".join(sorted(selected[arch]))]
            status |= subprocess.run(command).returncode
    return 1 if status else 0


if __name__ == "__main__":
    sys.exit(main())
