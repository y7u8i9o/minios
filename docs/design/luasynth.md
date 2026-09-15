# Lua Synthesizer

`luasynth` is a separate package and launcher entry named **Lua Synthesizer**.
The existing `synth` package, `user/apps/synth.c` and `synthvoice.h` remain
unchanged. Both packages can be installed together or removed independently.

    pkg install /usr/share/packages/luasynth-0.1.1.mpk
    luasynth

Version 0.1.1 upgrades an installed 0.1.0 package through `pkg install`.
Close the instrument before upgrading. This version requires the rebuilt
Lua interpreter with the native thread binding.

The desktop supplies `audiod`, the GUI framework and Lua. The package's
small C launcher only invokes `/bin/lua` with its installed `main.lua` and
forwards an optional patch filename. All instrument behavior, synthesis,
controls, presets and patch serialization are implemented in Lua under
`user/packages/luasynth/files/share/apps/luasynth/`.

## Instrument

Eight voices have independent amplitude envelopes, phases and velocity.
Keyboard repeat does not create extra voices. At the voice limit, stealing
prefers released voices, then the lowest envelope level and oldest voice.
Sustain defers release until the pedal is released. Release duration is
measured from the envelope's current level.

Each voice mixes two oscillators. Sine uses a 2048-entry wavetable. Saw and variable-width pulse apply
PolyBLEP discontinuity correction, and triangle is piecewise linear. Corrected waveforms are cached in 2048-entry tables,
with correction widths rounded upward to quarter-octave frequency bands.
This keeps oscillator calls out of the sample loop. The cache shared
by voices within the engine is bounded to 128 tables. Oscillator B has a two-octave range in either direction
and detune of up to 50 cents. A shared sinusoidal LFO supplies vibrato,
and note-dependent panning spreads a chord across the stereo field.

The voices sum into a shared stereo biquad low-pass. Its cutoff follows
the strongest voice envelope and its resonance sets the Q. Filter and
vibrato coefficients update per render block. A cross-feedback stereo
delay follows, with independent time, wet mix and feedback controls.
Bounded saturation prevents PCM overflow when voices and resonant peaks
sum. The output is interleaved S16_LE at 48000 Hz.

`engine.lua` has no audio-server or GUI dependency. `render(frames)`
accepts blocks up to 4096 frames, returns a PCM string, and updates a
scope trace and output peak. The application renders the server's quantum
only when buffers are ready and primes the pool before playback starts.

The application has two native threads. The main thread owns the GUI and
`controller.lua`, which caches controls and the latest display state.
`worker.lua` owns a separate Lua state, the sound engine, audio connection
and playback stream. It polls the audio descriptor and command queue,
processes at most 32 commands per iteration, then fills the ready buffers.
GUI layout, painting, patch-file access and the 100 ms display/demo timer
execute independently of rendering.

Commands and meter snapshots cross bounded copied-string queues. Audio
meters are best effort and never wait for the GUI. A failed control send
requests audio shutdown so that an unqueued note-off cannot leave a stuck
voice. Patch changes and note events are applied in command order. A command
barrier exists for tests, while ordinary controls remain asynchronous.
Closing the window removes its descriptor watch and timer, requests worker
shutdown, joins it after its audio resources close, and destroys the GUI.
See [Lua threading and system APIs](lua-threads.md) for binding details.

Live status uses a fixed canvas so meter changes do not relayout the window.
The engine reuses work arrays and packs PCM in groups of 64 frames to reduce
allocation. See [playback measurements and remaining limits](luasynth-performance.md).

## Interface and patches

The window groups oscillator, envelope, filter, modulation, stereo and
delay controls above a scope and a two-octave keyboard. The lower typing
row is `Z S X D C V G B H N J M`, and the upper row is
`Q 2 W 3 E R 5 T 6 Y 7 U I`. Space is sustain, brackets shift octave,
Escape is panic, and Ctrl+Q closes. Mouse holds and keyboard holds use
separate input identifiers, so releasing one does not cut off the other.
Captured mouse motion supports dragging outside and back into the keyboard.
Window focus loss releases notes. Moving focus to a control releases
keyboard holds. Panic also clears the delay and stops the demo.

Six factory presets cover a pad, keys, bass, glass-like tone, pulse lead
and pure sine. Save/load uses a bounded plain-text key/value format,
validated against the same ranges as the controls. It never evaluates
patch contents as Lua. A bad patch leaves the current sound unchanged.
The package registers `.lsynth` with its own MIME handler and supplies
`luasynth(1)`. User patches live outside the package's owned files, so
removal does not delete them.

`lgui.c` exposes `button`, `x` and `y` for canvas `press`, `motion`,
`release` and `wheel` signals, matching the existing native canvas events.
Audio disconnects remove the descriptor watch and appear in the status
line. Worker errors are returned with a traceback after the worker releases
its audio objects.

## Validation

`make check-lua` runs pure engine checks, native-widget host tests and
the native audio worker lifecycle test.
They check sample format and A4 frequency, polyphony, retrigger suppression,
velocity, pedal and release, stereo output, delay tails, bounded output
for every preset, patch validation and round trips, mouse drag/release,
simultaneous keyboard notes, shared mouse/keyboard holds, layout bounds,
and audio-resource cleanup. The UI test can write a native-widget preview
with `LUASYNTH_PREVIEW=/tmp/luasynth.ppm`.

`tests/cases/luasynth` installs the Lua and C packages together, runs the
same engine checks inside MiniOS, then removes the Lua package and verifies
that the C package is intact. `gui_luasynth` opens the installed Lua app
against X12 and audiod and plays three- and eight-note chords with injected keyboard
input. Its host post-check inspects the resulting WAV for nonzero stereo
PCM. The existing `gui_mixer` test continues to exercise the C synthesizer.

`lua_threads` tests the general binding inside MiniOS. `luasynth_worker`
checks thread identity, control ordering, audio progress while the GUI
thread sleeps, and join/cleanup. `luasynth_profile` compares inline and
worker rendering, including a deliberate 500 ms GUI pause. The measured
results and remaining eight-voice deadline misses are recorded in
[playback measurements](luasynth-performance.md).
