# Visual polish

The checklist of the visible parts of the system, for the review of the
owner. Each item has a code for reference. A ticked item is completed, with
its date and commit. Items marked **Known** are differences found in the
code during the X12 work of 2026-10-30.

## A. Window frame (X12, `user/compositor/wm/decor.c`)

- [ ] **A1** Title bar: height, background colour, the title font (13 px), centred or left-aligned title. **Known:** the title bar is 28 px, the minios proportions specify 30 px.

  Keep

- [ ] **A2** Buttons for close, maximize and minimize: shape, size, glyphs, red close button. **Known:** the buttons are 14 px, the proportions specify 20 px.

  Keep

- [ ] **A3** Corners. **Known:** the radius is 8 px, the proportions specify 6 px. 

  Make it even larger, preferably 10 px

- [ ] **A4** Shadow, and its strength on active and inactive windows. **Known:** the shadow reaches 14 px, the proportions specify 16 px.

  Keep

- [ ] **A5** Border and separator line, and the difference between active and inactive windows.

- [ ] **A6** Client side headers of libgui (`csd.c`) compared with the server frame. Both kinds should match.

- [ ] **A7** Dimming of a window that does not respond.

- [x] **A8** Resizing at the window edges: the device cursor of virtio-gpu appears at the pointer instead of shifted by its hotspot in the cocoa window of QEMU (2026-10-30, `80c66091`). The resize zones have their original sizes again.

- [x] **A9** No remnants after a resize from the top or the left edge (2026-10-30, `f172d69d`).

- [ ] **A10** Fewer rows in windows: the menu bar of a window lies in the header bar of the client side decorations, at the left of the title. Header bar slots for tabs and tool buttons (the GNOME idea) can follow per application.

## B. Desktop and panel

- [ ] **B1** Wallpapers `default.png` and `dusk.png`, and the solid desktop colour.
- [ ] **B2** Desktop icons: size, label, selection, drag image.
- [ ] **B3** Panel: height, background, position at the bottom or the top.
- [ ] **B4** Panel launcher menu.
- [ ] **B5** Clock and calendar popup.
- [ ] **B6** Volume mixer popup.
- [ ] **B7** Power menu.
- [ ] **B8** Input method indicator and its menu.
- [ ] **B9** Task buttons of open windows.
- [ ] **B10** Notification popups and the notification history.

## C. Session screens

- [ ] **C1** Login window of the greeter.
- [ ] **C2** Lock screen of `lock`.
- [ ] **C3** Black screen with the message after a crash of the locker (drawn by X12).
- [ ] **C4** Authentication dialog `askpass`.
- [ ] **C5** "Not responding" dialog of `sessiond`. **Known:** the dialog is new and plain, a layer window without a title and without a frame.
- [ ] **C6** Screenshot interface, area selection and thumbnail.

## D. Widgets (libgui)

- [ ] **D1** Buttons, including default and destructive buttons.
- [ ] **D2** Text fields, including the masked field and the selection.
- [ ] **D3** Check boxes, radio buttons, switches.
- [ ] **D4** Combo boxes and their dropdowns.
- [ ] **D5** Sliders, spinners, progress bars.
- [ ] **D6** Scroll bars and scroll areas.
- [ ] **D7** List views, tree views, tables: rows, headers, selection.
- [ ] **D8** Tabs, split panes, toolbars, status bars.
- [x] **D9** Menu bars, menus, context menus: rounded cards concentric with the row highlight, also the power, input method and launcher menus of the panel (2026-10-30, `e194b7e9`).
- [ ] **D10** Dialogs: message, prompt, file chooser, application chooser, colour chooser.
- [ ] **D11** Cards and account entries of the greeter style.
- [x] **D12** Focus indication for keyboard navigation: no ring; the hover grey on small controls and a darker hairline on large areas, only after keyboard input (2026-10-30, `b2784307`).
- [ ] **D13** Tooltips.

## E. Text and icons

- [ ] **E1** Interface font, sizes and weights.
- [ ] **E2** Monospace font in the terminal and the editors.
- [ ] **E3** Icon set (`user/share/icons`): style, sizes, consistency.
- [x] **E4** Mouse cursor: SVG cursors of 20 px with the names of the CSS cursor property, chosen by clients and by X12 (2026-10-30, `07e8cffd` and `53bf7b5f`, `docs/design/cursors.md`).
- [ ] **E5** HiDPI rendering at scale 2 (the default `2560x1600@2`).

## F. Applications

- [ ] **F1** Terminal `term`.
- [ ] **F2** Files.
- [ ] **F3** Settings, with the pages Look, Region, System and Users.
- [ ] **F4** Editors `edit`, `gedit` and `code`.
- [ ] **F5** Calculator `calc`.
- [ ] **F6** Image viewer `view` and `paint`.
- [ ] **F7** Media: `player`, `sequencer`, `synth`.
- [ ] **F8** Tools: `sysmon`, `sysinfo`, `logview`, `profiler`, `hexview`, `unicode`, `clock`.
- [ ] **F9** Diagnostics: `x12settings`, `wireview`, `compstat`, `evtest`.
- [ ] **F10** Games: `pong`, `mandel`.
- [ ] **F11** Installer `installer-gui`.

## G. System-wide

- [ ] **G1** Colour palette: accent colour, backgrounds, borders, text contrast.
- [ ] **G2** Spacing and control height (26 px controls).
- [ ] **G3** Dark mode or a second theme. No second theme exists.
- [ ] **G4** Animations and transitions. X12 has none.
