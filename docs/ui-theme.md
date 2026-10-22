# MyOS desktop theme: "Golden Hour"

A desktop look for MyOS. It keeps what people liked about the early-2000s desktops
(friendly, glossy, a big green Start button, a bright taskbar) but changes enough
details to be its own look. MyOS has no graphical desktop yet. This is the design
to build when it does.

![Golden Hour mockup](../live/assets/themes/golden-hour/mockup.png)

Everything here is drawn by one script from one palette:
`live/assets/themes/golden-hour/make-theme.py`. Change a color or size there and run
it again (it needs `rsvg-convert`, from Homebrew's `librsvg`). Don't edit the
generated files by hand.

| file | what it is |
|---|---|
| `make-theme.py` | the palette, every icon, the Start button, the mockup |
| `icons/svg/NAME.svg` | each icon, on a 48×48 design grid |
| `icons/png/{16,24,32,48,96}/NAME.png` | each icon, rendered at that size |
| `start-button.*`, `start-button-hover.*`, `start-button-pressed.*` | the Start button in its three states (the PNGs are 2×: 500×128) |
| `mockup.svg`, `mockup.png`, `mockup@2x.png` | a 1024×768 desktop built from the pieces |
| `icon-sheet.svg`, `icon-sheet.png` | every icon with its name |
| `ui-states.svg`, `ui-states.png` | every element in every state |
| `login.svg`, `login.png`, `login@2x.png` | the login screen |
| `settings.svg`, `settings.png` | Settings open on Users (create account) |
| `settings-wifi.svg`, `settings-wifi.png` | Settings open on Wi-Fi |
| `settings-general.svg`, `settings-general.png` | Settings open on General |
| `battery-popup.svg`, `battery-popup.png` | the tray battery pop-up, above the bottom tray |

The color tables under "Elements and states" are written by the script too, from the
same table it draws with. Change a color in the script, not in this file.
| `wallpaper.jpg` | the wallpaper (1152×864, generated) |

The first ideas (`live/assets/themes/myos-xp-*.jpg`, `start-button-options.*`,
`start-button-in-menu.*`) are kept for the record. Shape C was the one picked.

## What makes it different from the old XP look

| | XP | Golden Hour |
|---|---|---|
| title bars, start menu header | royal blue | teal (`#2f9aa1` → `#0f5560`) |
| taskbar | blue | navy on the left to teal on the right |
| window buttons | square, red close | round, light teal, amber close |
| Start button | green, round right end, flag logo, "start" | green, **flat left, full taskbar height, top slopes down to a rounded nose on the right** (shape C, "ramp"), circle-and-dot logo, "Start" |
| start menu header | user picture | the MyOS logo |
| right column of the start menu, side panels | light blue | warm sand (`#f9e6c1` → `#efd3a0`) |
| wallpaper | green hill, blue noon sky | rolling hills and a lake at golden hour |

## Layout (at 1024×768)

| part | size and place |
|---|---|
| taskbar | full width, **40 px** high, at the bottom of the screen |
| Start button | **40 px high, the same as the taskbar**: its top is the bar's top edge and its bottom is the bottom of the screen. Flat on the left (the screen edge). 156 px wide (the 250×64 design scaled by 40/64). Logo 16 px, "Start" 19 px |
| task buttons | 160×30 px, 4 px corners, 5 px from the bar's top, 10 px apart, starting at x = 176 |
| tray | the last 150 px of the taskbar: network and volume icons (18 px), clock |
| window | 8 px corners, 1.5 px dark-teal border, soft shadow (0, 3 px, blur 5, 35% black) |
| title bar | 30 px; icon 18 px at the left; title in bold 13 px; three round buttons, 21 px, 3 px apart, at the right |
| menu bar | 22 px; toolbar 34 px with 24 px buttons; address box 22 px |
| side panel ("File tasks") | 170 px wide, sand; boxes with 6 px corners, a 16 px round chevron and a teal heading |
| file view | 48 px icons on a 120×100 px grid |
| status bar | 23 px, at the bottom of the window |
| start menu | 390×430 px, right above the taskbar, flush with the left edge; header 56 px (logo 40 px, user name bold 18 px); left column 210 px white (40 px icons, name and one line under it); right column sand (32 px icons, teal bold names); footer 46 px (30 px round Log off and Turn off) |
| hot corner | the top-right corner of the desktop: a 24 px page curl, as if the wallpaper's corner is folded over. Moving the mouse into the corner shows all windows side by side, like Mission Control on the Mac; the curl grows to 40 px as it starts. Always drawn above the wallpaper, below windows |
| desktop icons | 48 px, a column at the left, 82 px apart; white 12 px labels with a dark shadow |

## Base palette

The base colors. Gradients run from top to bottom unless the table says otherwise.
The next section uses them for every element and state.

| name in `make-theme.py` | hex | used for |
|---|---|---|
| `bar_left` → `bar_right` | `#0e2c52` → `#1b5d74` | taskbar, left to right |
| `bar_edge` | `#7fb6e6` | 1.5 px line along the top of the taskbar |
| `title_top` / `title_mid` / `title_bottom` | `#2f9aa1` / `#1f7f86` / `#0f5560` | window title bars, start menu header and footer |
| `title_edge` | `#0a4048` | window and menu borders |
| `green_1` … `green_4` | `#7fe36a` `#3fb447` `#2c9431` `#1c6b21` | Start button, "All Programs" and "Back" (gloss: light top, crease at 50–55%, dark bottom) |
| `green_rim` | `#145a19` | their outline |
| `teal_btn_top` → `teal_btn_bottom` | `#d6f6f7` → `#5cc3cb` | minimize, maximize, Log off |
| `teal_btn_rim` | `#2b8a92` | their outline |
| `amber_top` / `amber_mid` / `amber_bottom` | `#ffc56b` / `#f08a12` / `#c96500` | close, Turn off, the browser's compass |
| `amber_rim` | `#a24f00` | their outline |
| `steel_1` → `steel_2` | `#e2edf6` → `#7d9bb8` | Forward, Up (buttons that are not the main action) |
| `panel_white` | `#fbfbf9` | window background, start menu left column |
| `panel_sand` → `panel_sand_deep` | `#f9e6c1` → `#efd3a0` | start menu right column, side panels |
| `menu_bar` | `#efece4` | menu bar, toolbar, status bar |
| `border` | `#c9c2b2` | thin lines between parts |
| `text` | `#1d2430` | text |
| `text_soft` | `#5b6573` | second lines, status bar |
| `label_teal` | `#1f6d72` | right column of the start menu, panel headings |
| taskbar text, title text | `#ffffff` | with a 1 px dark shadow |

Gloss, the same everywhere: a white highlight over the top half, from 75% opacity
down to 0, often drawn at 30–80% strength. Round buttons get an oval highlight.

Icon colors: folders `#fff0a8` → `#ffd84d` → `#f0b22a` (back `#e9b53a` → `#c98c17`,
outline `#a87412`); globes `#9fd8ff` → `#1f6fd1` → `#0f3f8a` (radial); screens
`#0d1a12` with `#5cf27a` green; disks `#4a6c8f` → `#1d3554`; paper `#ffffff` →
`#e3e8ee` with `#7d8a96` outlines and `#6a9fd4` lines; the key `#e0a52c`.

## Login screen

![Login screen](../live/assets/themes/golden-hour/login.png)

The same wallpaper as the desktop, blurred and tinted dark teal, so the desktop
seems to come into focus when you log on.

| part | size and place |
|---|---|
| top band | 64 px; "Welcome" in bold italic Trebuchet MS 22 px on the right |
| bottom band | 80 px; Turn off (36 px icon, 15 px text) on the left; a two-line hint in 12 px light teal on the right |
| golden lines | 2 px where each band meets the middle, fading out at both ends |
| left side | the logo (108 px) and "MyOS" (bold italic 54 px), both right-aligned to x = 470; "To begin, click your user name" in 15 px light teal under them |
| divider | a 1.5 px white line at the center (x = 511), fading out at the top and bottom |
| user tiles | start at x = 548, 400 px wide. The selected tile is 120 px high, with a 72 px picture, the name (bold 22 px), "Type your password", a 220x26 password box (a focused text box) and a 30 px green go button. Other users: 56 px picture, name 20 px, one line under it, at 85% |
| user pictures | 8 px corners, 2 px white border; a piece of the wallpaper, or a white silhouette on teal for users without one |

The colors are in the tables below under "Login background", "Login bands" and "User tile".

## Settings

![Settings: Users](../live/assets/themes/golden-hour/settings.png)

The Settings window follows the same category order as macOS System Settings,
without Apple-only items (iCloud, Wallet, Family, and the like). **Wi-Fi is in
the same place as on the Mac**: just under Software Update, above Bluetooth.
The page is there even though the kernel cannot join a network yet.

| part | size and place |
|---|---|
| window | 988×706 px, 8 px from the left and top; title "Settings" |
| sidebar | 220 px, sand; Search box; the signed-in user; then the category list |
| selected row | the menu-highlight teal bar |
| group breaks | a thin `border` line, matching the Mac's spacing |
| Users | each account is a 64 px row (picture, name, Administrator or Standard). **Create New User...** asks for the administrator password first, the same way the Mac does, then Continue opens the new-account fields (name, password, account type) |
| Wi-Fi | an Off toggle and a note that the kernel does not support it yet; the list of networks will appear here when it does |
| General | a list of rows (About, Software Update, Storage, Date & Time, Language, Sharing, Startup Disk, Transfer or Reset), each with a chevron, like the Mac |

### Sidebar, top to bottom

1. Search
2. the signed-in user (rodger, Administrator)
3. Software Update
4. **Wi-Fi**, Bluetooth, Network, VPN, Battery
5. General, Accessibility, Appearance, Desktop & Taskbar, Displays, Wallpaper, Notifications, Sound, Lock Screen, Privacy & Security, Password, **Users**
6. Keyboard, Mouse, Printers

Creating a user: only an administrator can do it. Clicking Create New User...
opens a dialog that asks for that administrator's password. Cancel or a wrong
password leaves the list unchanged. Continue then asks for the new name,
password, and whether the account is Administrator or Standard.

## Elements and states

What each part of the screen looks like in each state: active and inactive window,
hover, pressed, focused, disabled. The colors in the base palette above are the
starting points; these tables give the exact fill of every element.

![Every element in every state](../live/assets/themes/golden-hour/ui-states.png)

The rules behind the states:

- **Focus is teal and bright; no focus is the same teal washed out.** An inactive
  title bar keeps its hue but loses most of its color, its text shadow and some gloss.
  Its buttons fade to match. So you can always tell which window gets your typing.
- **Hover is lighter, pressed is darker** and loses most of its gloss, as if pushed in.
  This holds for every glossy part: Start, title-bar buttons, task buttons, scroll bars.
- **Amber means "look here"**: close, Turn off, a task button whose window wants you,
  a push button under the mouse.
- **Selection is teal**: the highlight in menus, a selected file's name, an open menu.

<!-- colors:begin -->
<!-- Made by make-theme.py from its UI table; edit there, not here. -->

### Taskbar

The bar along the bottom of the screen, 40 px high.

| state | fill | border | text / glyph | notes |
|---|---|---|---|---|
| normal | left to right: `#0e2c52` → `#1b5d74` | none | - | shade over it, top to bottom: `#ffffff` (30% opaque) at 0% → `#ffffff` (6% opaque) at 15% → `#000000` (18% opaque) at 100%; the shade is drawn over the base; a 1.5 px line along the top in `bar_edge` at 80% |

### Tray

The clock and status icons at the right end of the taskbar.

| state | fill | border | text / glyph | notes |
|---|---|---|---|---|
| normal | top to bottom: `#2d7c8f` → `#16546a` | none | `#ffffff` | shade over it, top to bottom: `#ffffff` (30% opaque) at 0% → `#ffffff` (6% opaque) at 15% → `#000000` (18% opaque) at 100%; the same shade and top line as the taskbar; a 1.5 px `#0b2346` divider on its left |

### Task button

One per open window, on the taskbar; 160x30 px, 4 px corners.

| state | fill | border | text / glyph | notes |
|---|---|---|---|---|
| normal | top to bottom: `#3a6fa8` → `#1d4a7c` | `#0b2346` 1 px | `#ffffff` | gloss 25% |
| hover | top to bottom: `#4d86c2` → `#275a90` | `#0b2346` 1 px | `#ffffff` | gloss 35% |
| active | top to bottom: `#132e55` → `#1f4f80` | `#06162e` 1 px | `#ffffff` | its window has the focus: looks pressed in, a 3 px black inner shadow at 30% along the top; bold text |
| attention | top to bottom: `#ffc56b` at 0% → `#f08a12` at 55% → `#c96500` at 100% | `#a24f00` 1 px | `#ffffff` | gloss 35%; the window wants you: flashes 3 times (0.5 s on, 0.5 s off), then stays amber |

### Start button

Shape C, the full taskbar height (see Layout).

| state | fill | border | text / glyph | notes |
|---|---|---|---|---|
| normal | top to bottom: `#7fe36a` at 0% → `#3fb447` at 50% → `#2c9431` at 55% → `#1c6b21` at 100% | `#145a19` 1.5 px | `#ffffff` | gloss 70%; the hard step at 50-55% is the gloss crease; text shadow `#0b3d0f` at 75% |
| hover | top to bottom: `#9df08a` at 0% → `#52c95a` at 50% → `#3aa940` at 55% → `#22802a` at 100% | `#145a19` 1.5 px | `#ffffff` | gloss 80% |
| pressed | top to bottom: `#5cc954` at 0% → `#2c9431` at 50% → `#22802a` at 55% → `#145a19` at 100% | `#0e4512` 1.5 px | `#ffffff` | gloss 35%; also while the start menu is open; the logo and text move 1 px down |

### Window title bar

30 px; also the start menu's header and footer.

| state | fill | border | text / glyph | notes |
|---|---|---|---|---|
| active | top to bottom: `#2f9aa1` at 0% → `#1f7f86` at 50% → `#0f5560` at 100% | `#0a4048` 1.5 px; window frame `#0a4048` 1.5 px | `#ffffff` | gloss 35%; title text has a 1 px black shadow at 80%; window shadow 0/3 px, blur 5, black 35% |
| inactive | top to bottom: `#a9cacd` at 0% → `#8fb5b9` at 50% → `#769ea3` at 100% | `#628a8f` 1.5 px; window frame `#8aa9ac` 1.5 px | `#ffffff` | gloss 25%; no text shadow; the icon at 75%; window shadow 0/2 px, blur 3, black 20% |

### Minimize and maximize buttons

Round, 21 px, 3 px apart, in the title bar.

| state | fill | border | text / glyph | notes |
|---|---|---|---|---|
| normal | top to bottom: `#d6f6f7` → `#5cc3cb` | `#2b8a92` rim, 1/30 of the size | `#ffffff` |  |
| hover | top to bottom: `#effdfd` → `#7fd8de` | `#2b8a92` rim, 1/30 of the size | `#ffffff` |  |
| pressed | top to bottom: `#5cc3cb` → `#3c9fa7` | `#1f6d72` rim, 1/30 of the size | `#e8fbfb` |  |
| inactive | top to bottom: `#e6eff0` → `#a9c6c9` | `#8aa9ac` rim, 1/30 of the size | `#f7fafa` | the window doesn't have the focus |

### Close button

Round, 21 px, the last button in the title bar.

| state | fill | border | text / glyph | notes |
|---|---|---|---|---|
| normal | top to bottom: `#ffc56b` at 0% → `#f08a12` at 55% → `#c96500` at 100% | `#a24f00` rim, 1/30 of the size | `#ffffff` |  |
| hover | top to bottom: `#ffd994` at 0% → `#ff9e30` at 55% → `#db7600` at 100% | `#a24f00` rim, 1/30 of the size | `#ffffff` |  |
| pressed | top to bottom: `#f08a12` at 0% → `#c96500` at 55% → `#934800` at 100% | `#7a3a00` rim, 1/30 of the size | `#ffe9cc` |  |
| inactive | top to bottom: `#f6e0c6` at 0% → `#e6b98a` at 55% → `#cf9c6a` at 100% | `#b8916a` rim, 1/30 of the size | `#fffaf3` |  |

### Menu highlight

The row under the mouse in the start menu, menus and lists.

| state | fill | border | text / glyph | notes |
|---|---|---|---|---|
| hover | top to bottom: `#3aa7ae` → `#1f7f86` | `#17666c` 1 px | `#ffffff` | the same on the white and the sand column; its icon stays as it is |

### Menu bar item

File, Edit, View... at the top of a window.

| state | fill | border | text / glyph | notes |
|---|---|---|---|---|
| normal | `#efece4` flat | none | `#1d2430` |  |
| hover | `#d7ecee` flat | `#8fc7cb` 1 px | `#1d2430` |  |
| open | top to bottom: `#3aa7ae` → `#1f7f86` | `#17666c` 1 px | `#ffffff` | while its menu is open; the menu is drawn like the start menu's white column |

### Selected file

A selected item in a file window or on the desktop.

| state | fill | border | text / glyph | notes |
|---|---|---|---|---|
| focused | `#cdeef0` flat | `#5cc3cb` 1 px | `#1d2430` | the name under the icon gets a `#1f7f86` background and white text; on the desktop the name background is `#1f7f86` at 85% |
| unfocused | `#e4e6e2` flat | `#c9c2b2` 1 px | `#1d2430` | selection in a window that doesn't have the focus |

### Push button

OK, Cancel and the like in dialogs; 75x23 px, 4 px corners.

| state | fill | border | text / glyph | notes |
|---|---|---|---|---|
| normal | top to bottom: `#ffffff` → `#e6e2d8` | `#a59d8c` 1 px | `#1d2430` |  |
| hover | top to bottom: `#ffffff` → `#e6e2d8` | `#f08a12` 2 px | `#1d2430` | an amber 2 px border |
| pressed | top to bottom: `#e2ddd2` → `#f6f4ef` | `#a59d8c` 1 px | `#1d2430` |  |
| default | top to bottom: `#ffffff` → `#e6e2d8` | `#2b8a92` 2 px | `#1d2430` | the button Enter presses |
| disabled | `#efece4` flat | `#d4cfc4` 1 px | `#9aa3ad` |  |

### Text box

Address bar, file names, dialogs; 22 px high.

| state | fill | border | text / glyph | notes |
|---|---|---|---|---|
| normal | `#ffffff` flat | `#c9c2b2` 1 px | `#1d2430` |  |
| focused | `#ffffff` flat | `#2b8a92` 1.5 px | `#1d2430` | plus a 2 px `#5cc3cb` ring at 40% just outside; caret `#1d2430` |
| disabled | `#efece4` flat | `#d4cfc4` 1 px | `#9aa3ad` |  |

### Scroll bar

17 px wide; track `menu_bar` with a `border` line on its inner side.

| state | fill | border | text / glyph | notes |
|---|---|---|---|---|
| thumb | left to right: `#d6f6f7` → `#5cc3cb` | `#2b8a92` 1 px | - | the gradient runs across the thumb |
| thumb hover | left to right: `#effdfd` → `#7fd8de` | `#2b8a92` 1 px | - |  |

### Tooltip

The little note that appears under the mouse.

| state | fill | border | text / glyph | notes |
|---|---|---|---|---|
| normal | `#fff7dd` flat | `#c9a24a` 1 px | `#1d2430` | 11 px Verdana; shadow 0/2 px, blur 2, black 25% |

### Hot corner

The top-right corner of the desktop: the wallpaper curls over; move the mouse into the corner to see all windows (like Mission Control on the Mac).

| state | fill | border | text / glyph | notes |
|---|---|---|---|---|
| normal | top to bottom: `#0f5560` → `#0e2c52` | `#b9a37a` 1 px | - | fill is what shows under the curl; the flap is the back of the wallpaper, lit along the fold, `#b9a37a` edge, shadow -1/1.5 px, blur 1.5, black 35% |
| hover | top to bottom: `#1f7f86` → `#0e2c52` | `#b9a37a` 1 px | - | the mouse is in the corner: the curl grows to 40 px in 120 ms, then the windows spread out |

### Login background

The middle of the login screen.

| state | fill | border | text / glyph | notes |
|---|---|---|---|---|
| normal | top to bottom: `#0e2c52` (55% opaque) at 0% → `#0f5560` (35% opaque) at 50% → `#0e2c52` (70% opaque) at 100% | none | - | drawn over the wallpaper, which is blurred (Gaussian, 6 px) |

### Login bands

The bands at the top (64 px) and bottom (80 px) of the login screen.

| state | fill | border | text / glyph | notes |
|---|---|---|---|---|
| normal | top to bottom: `#0e2c52` → `#133f63` | none | - | the bottom band runs the other way; a 2 px golden line where each band meets the middle, left to right: clear → `#ffc56b` → clear |

### User tile

One per user on the login screen; 380x84 px, 10 px corners.

| state | fill | border | text / glyph | notes |
|---|---|---|---|---|
| normal | none (clear) | none | `#ffffff` | the picture at 85% |
| hover | left to right: `#ffffff` (16% opaque) at 0% → `#ffffff` (3% opaque) at 100% | `#7fb6e6` 1 px | `#ffffff` |  |
| selected | left to right: `#ffffff` (28% opaque) at 0% → `#ffffff` (6% opaque) at 100% | `#ffc56b` 1.5 px | `#ffffff` | the golden border is the theme's name; shows the password box (a focused text box) and the green go button |

<!-- colors:end -->

## Fonts

| where | font |
|---|---|
| "Start", window titles, the user name | Trebuchet MS bold (the Start label is bold italic) |
| everything else | Verdana, 11–13 px |

For MyOS's own graphics these will become bitmap fonts at these sizes. That is
cheaper than drawing outlines on the old PCs.

## Icons

![All icons](../live/assets/themes/golden-hour/icon-sheet.png)

| icon | where it is used |
|---|---|
| `logo` | start menu header (white; also in the Start button) |
| `terminal` | Terminal (desktop, start menu, side panel, taskbar) |
| `run` | Run... |
| `my-files` | My Files (a folder with a green leaf) |
| `folder` | folders, title bar and address box of a file window |
| `text-file`, `binary-file` | files |
| `text-editor` | Text Editor |
| `web-browser` | Web Browser (a globe with an amber compass) |
| `network` | Network |
| `disks` | Disks |
| `settings` | Settings |
| `help` | Help |
| `all-programs` | All Programs (green, forward) |
| `back` | Back (green: the main action) |
| `forward`, `up` | Forward, Up; also the chevron on side-panel headings |
| `log-off` | Log off |
| `turn-off` | Turn off |
| `minimize`, `maximize`, `close` | title-bar buttons |
| `recycle` | Recycle, "Delete this file" |
| `tray-network`, `tray-volume` | the tray (white, for the dark taskbar) |
| `wifi`, `bluetooth`, `vpn`, `battery` | Settings: the network group (Wi-Fi sits where it does on the Mac) |
| `users`, `password`, `lock`, `privacy` | Settings: Users, Password, Lock Screen, Privacy & Security |
| `display`, `wallpaper`, `desktop`, `appearance` | Settings: Displays, Wallpaper, Desktop & Taskbar, Appearance |
| `keyboard`, `mouse`, `printer`, `sound` | Settings: Keyboard, Mouse, Printers, Sound |
| `accessibility`, `notifications`, `updates`, `about`, `datetime` | Settings: Accessibility, Notifications, Software Update, About, Date & Time |

Icons are drawn on a 48×48 grid with a 1.2–1.6 px darker outline, so they stay
readable on both the white and sand panels. They are rendered at the sizes the
layout uses (16, 18–24 → 24, 32, 40–48 → 48) plus 96 for 2× screens. On the
old PCs the desktop should blit these ready-made PNGs, never draw SVG at run time.
