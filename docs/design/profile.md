# System profiler

## Capture and attribution

`kernel/debug/profile.c` collects CPU samples, scheduler transitions,
kernel heap allocations and frees, and completed transfers. Each CPU owns
a 128 KiB byte ring containing variable-length records. Producers publish
whole records without taking the reader's lock. Ring overflow is reported
through `prof_stats.dropped`. Timer readiness notifications wake the reader.

`/dev/profile` controls the global recording session. `PROF_CONFIGURE`
selects event classes, stack depth (up to 32 frames), a CPU sampling divider
(1–1000 timer ticks), minimum allocation size and minimum transfer duration.
`PROF_START` resets the capture and selects a PID, or every process when
PID is zero. System-wide recording excludes the recording process.
`PROF_STOP` quiesces producers, after which the remaining records can be
read. Closing the device stops capture and frees ring storage.
`PROF_GET_STATS` reports configuration, recorded events, lost events and
pending bytes. Readers receive whole events merged by timestamp.

`kernel/debug/unwind.c` walks frame pointers and stitches kernel stacks
onto the user frames that caused them when available. The client resolver
loads kernel symbols from `/dev/ksyms`, process executables from their ELF
symbol tables, and shared-library mappings from `/dev/maps`. Unresolved
addresses remain numeric. Lock-release samples are attributed past the
lock primitives to the code that held the lock.

`libc/src/profanalyze.c` maintains four call trees, flat histograms, thread
statistics, outstanding allocation addresses and pending scheduler stacks.
Each tree node stores inclusive weight, exclusive self weight, an inclusive
secondary magnitude, and the number of events ending at that node. The
weights are CPU nanoseconds estimated from samples, off-CPU nanoseconds,
allocated kernel heap bytes, and I/O latency nanoseconds. Heap secondary
weights are bytes still held. I/O secondary weights are transferred bytes.
Thread scheduling measurements are separate from sampled CPU estimates.

## Graphical analysis

The launcher entry Profiler starts `/bin/profiler`. `profiler -p PID`
preselects a target, and the Profile command of sysmon starts it with the
selected process.
Select the event classes, stack depth and CPU sample interval, then press
Start. These settings apply to the next capture. A larger sample interval
reduces CPU sampling overhead. Stop retains the capture for inspection.
Reset stops capture, drains it and clears the analysis before refreshing
the process selector. It does not restart recording.

The Flame graph tab supports:

- Clicking a frame to select its call site and zoom into its subtree.
  Right-clicking, clicking the bottom frame, or pressing Up selects its
  caller. All frames restores the complete tree.
- A resizable breakdown pane showing the selected function, caller,
  inclusive and self costs, and percentage of the entire capture. The
  table contains self work and direct callees sorted by inclusive cost.
  Its percentages use the selected frame as the denominator. These rows
  are disjoint and sum to that frame's inclusive cost, even for recursion.
  Double-click a callee to inspect its breakdown.
- A case-sensitive function substring search. Matching frames are purple.
  The matched amount is the weight of stacks containing a match within
  the current subtree. Nested and recursive matches count only once.
- Hover details with inclusive cost, self cost, self event count, mode,
  and held or transferred bytes where applicable. Warm colors identify
  user code and cool colors identify kernel code.

The Frames tab lists exclusive hotspots for the selected view. Threads
shows CPU, off-CPU and ready time together with scheduling counts. Heap
lists allocation call sites still holding memory. Transfers lists latency
and bytes by leaf function. The status bar reports capture duration and
dropped events. Updates stop when recording stops, preserving hover and
search context. Reads are bounded per UI callback during capture so a
continuous stream cannot prevent input and repaint processing.

Breakdown is specific to the selected call path. A function appearing
under different callers has a separate breakdown in each location. Event
counts are observations, not function invocation counts. CPU widths are
sample-based estimates. Outstanding kernel allocations are not proof of
leaks: they can be legitimate live objects. Missing events and truncated
stacks limit attribution, particularly allocation/free pairing and blocked
threads that have not resumed when the capture ends.

## Export formats

Export JSON saves all four call trees, thread statistics, event counts,
recording timestamps, sample period and heap totals. Export folded saves
the entire currently selected view, independent of zoom and search. The
GUI prompts for a guest filesystem path and confirms replacement of an
existing file. Export stops and drains capture before serialization.
Successful export paths and errors remain visible below the tabs.

The shared `prof_session_export` API writes a sibling temporary file with
exclusive creation, checks write and close errors, and renames it over the
destination only after completion. Failures leave an existing destination
untouched and remove temporary files created by the export. A temporary
name collision reports an error without removing the conflicting file.
This provides atomic replacement, not a guarantee of persistence through
power failure. Paths refer to the guest filesystem, not the host.

JSON uses `format: "minios-profile"` and `version: 1`. `views` contains
`cpu`, `offcpu`, `heap`, and `io`. Each includes explicit units and nodes
identified by `id` and `parent` (`-1` for root). Node fields are `name`,
`kernel`, `total`, `self`, `extra`, and `self_events`. Integer weights
preserve nanosecond and byte precision. Use an integer-preserving parser
for values above JavaScript's exact integer range. `dropped` is null when
device statistics are unavailable. Symbol bytes outside printable ASCII
are escaped as JSON Unicode escapes. This is an aggregate report, not a
raw event timeline or a format the GUI can reopen.

Folded stacks contain one `outer;inner;leaf weight` record per exclusive
cost. Summing their weights reproduces the root total. CPU, off-CPU and I/O
weights are nanoseconds. Heap weights are allocated bytes, including bytes
later freed. Kernel frames carry `_[k]`. Semicolons, whitespace and control
bytes inside names become underscores because this format has no standard
escaping convention. Unattributed root weight is emitted as
`[unattributed]`. Use JSON when exact symbol spelling matters.

## Command-line workflow

`prof command args...` starts a command after sampling has been enabled.
`prof -p PID -d SECONDS` attaches to a process. `prof -a -d SECONDS` records
every process. `-e cpu,sched,heap,io` selects event classes, `-D` sets the
CPU sampling divider and `-s` sets maximum stack depth. `-c` reports call
chains, `-g` draws an ASCII flame graph, `-t` adds thread statistics, `-m`
adds kernel heap analysis and `-i` adds transfer analysis.

Examples:

```sh
prof -e all -o /home/capture.json -p 42 -d 10
prof -F -o /home/cpu.folded -a -d 5
profiler -r 10 -o /home/capture.json
```

`prof -o` exports full JSON while retaining the usual terminal report.
`-F -o` exports folded CPU stacks to a file. Without `-o`, `-F` writes
folded stacks to standard output. An export failure returns a nonzero
status. `profiler -r` starts all event classes immediately and stops after
the requested number of seconds. Its optional `-o` saves JSON at that point.

## Validation

`tests/cases/profile` runs the device, resolver, scheduler, allocation,
transfer, call-tree and flame-layout tests in `/bin/proftest`.
`tests/cases/profreport` runs deterministic analysis and export checks in
`/bin/profreporttest`: recursive search accounting, subtree boundaries,
breakdown conservation, sorting, JSON escaping, folded conservation,
selected-view weights, empty captures, file replacement, failure cleanup,
and a real command-line export. `profiler_gui` exercises the graphical
profiler during a workload. `prof_gui` checks system-wide command-line
profiling during GUI activity. Run affected QEMU cases serially with
`JOBS=1 CPUS=4`.
