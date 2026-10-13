# ONEXPLAYER Elite local input setup

## Status

- State: local rear-right F8 bridge and read-only vendor access work. The user
  confirmed menu open/close and desktop clicks; controller navigation follows
  in the XeSS investigation. This setup is for this installation only.
- Environment: ONEXPLAYER 3, CachyOS, Steam/Proton Experimental, flat EDVR.
- Open hypotheses: desktop clicks intermittently fail for mouse and controller;
  QAM restores them. The component holding desktop input is not identified.
- Ruled-out pointer: the dated capture sequence below records why keyboard,
  evdev and generic controller mappings did not identify the rear button.
- Next check: repeat rear-right and menu navigation on the reviewed build;
  record input state before another desktop click failure is cleared.
- Mesa/MinEdLauncher setup and rollback remain recorded in
  planetary-startup-intel-b390-2026-10-11.md. Temporary paths below identify
  historical evidence; they are not portable Windows handoff instructions.

## October 11: local rear-right menu binding

The user requested the menu on the ONEXPLAYER rear-right button for this
install. The local hid-oxp driver identifies M1 as report 0x22 and M2 as 0x23;
the ONEXPLAYER 3 setup project's measured left paddle is 0x22, so rear-right is
M2. Live sysfs button_m2 is KEY_F16. EDVR's existing hotkey parser supports
F16; no C++ change or repository default change is needed.

The initial focused keyboard test received no rear-button event. InputPlumber
owned the physical keyboard, but its active output was only deck-uhid, with no
keyboard target, no keyboard output capabilities and no F16 translation in the
current Default profile. Ruled out: changing the INI alone could deliver
rear-right in that state, because the input manager discarded keyboard output
before desktop/Proton delivery. KDE has no F15/F16 shortcut.

The existing device YAML already requests deck-uhid, keyboard and mouse. The
current outputs were set to deck-uhid plus keyboard, preserving the controller
and restoring only the keyboard route needed here; no system file was edited.
The Default profile has no target override. The earlier remover is unknown, and
persistence across reboot has not been verified. Do not add a launch wrapper
for an unobserved future reset.

The live edvr-flat.ini and its existing LocalAppData mirror now have
hotkey.menu=F16. They match exactly, SHA256
614057eddf2587a8869e56b02b5d09bfe8077045feb601420212334734969058. The reviewed
byte diff changes only menu=F8 to menu=F16 and its comment; CRLF,
temporal_aa=fsr, UI 125, sharpness 0.75 and all other settings remain. The
original is staged at /tmp/edvr-rear-right-original.ini, SHA256
c2ecc7487c6633000b93600a97fbbbffd4928c6b76be61ad9e6c5c2fe3d1c6e5. User
confirmation initially failed. Correct-build 131338.log loads the F16 hotkey
but records no menu opening; the user reports rear-right does not work.
InputPlumber's keyboard target remains active after that launch.

Ruled out: a stale F8 config or removal of the restored keyboard target caused
this failure, because 131338.log announces F16 and active targets still include
keyboard. Forwarding the declared M2 key is insufficient; capture the actual
press before choosing another binding or changing device mappings.

The keyboard target alone did not make the button work. The user also reported
desktop clicks stopped registering after using the focused tester. All test
processes were already closed. Restoring the on-disk target list
deck-uhid/keyboard/mouse did not recover clicks; reverting to the exact
original active target list, deck-uhid only, also did not immediately recover
them. No system files or mappings were edited for these target changes.

Read-only XWayland checks reported core button1 down and no held Ctrl/Alt/Meta
modifiers. XInput2 reported no buttons held on individual pointer devices and
identified touch as the last core-pointer source. One conditioned XTest
button-release event, with no motion or press, left the core mask unchanged at
0x110. Ruled out: that release cleared the reported core state, because the
before/after masks were identical. The user then recovered physical
mouse/trackpad clicks by pressing the right controller's back button. The core
mask still read 0x110 after practical recovery, so it is not a reliable measure
of this user's physical click problem. The cause remains unknown; do not
disable the touchscreen or restart the compositor on that stale reading.

Input experiments are back to the original controller-only active output. The
next button diagnosis must measure the actual normalized source event, with a
known controller button as a positive capture check, and must retain the
existing controller instance. No more focused-window tests are needed.

The headless debug capture at 13:28:41 retained the exact gamepad0 instance,
advertised F16 and standard controller button capabilities, and confirmed
DPadUp down/up at 13:29:55. The user then pressed rear-right, but reports
remained at two with no additional event. Ruled out: the declared sysfs
M2=KEY_F16 is confirmed input on the current composite path, because the
positive control arrived and the rear press did not. Inspect the separate input
nodes/firmware route before choosing a new binding.

The follow-up driver-source read corrects the initial M2 assumption:
map_button_show reads the cached mapping index; gamepad_mode_show reads the
cached mode. Probe seeds F15/F16 and XInput before delayed MCU setup attempts
the actual firmware writes. Failed transport leaves those sysfs defaults
visible. Ruled out: the displayed F16 alone proves what the button emits,
because it is not firmware readback. No kernel-journal mapping error was found,
which does not prove the write succeeded.

Every physical ONEX input and HIDraw channel is root-only. Event6 is the
separate consumer/mouse node outside the current composite's event18/event16
sources. Only Steam's virtual controller is readable as deck; another virtual
capture cannot establish the missing physical route. The next discriminating
test needs a read-only, no-grab administrator observer on the ONEX physical
nodes, filtering function-key and mouse/controller button identities only.

F8 and its original comment were restored in both live and mirrored flat INIs
while Elite was closed. Both match the original byte-for-byte, SHA256
c2ecc7487c6633000b93600a97fbbbffd4928c6b76be61ad9e6c5c2fe3d1c6e5. FSR3, UI 125
and sharpness 0.75 remain. User confirms only desktop clicks recovered, not the
EDVR menu. The rear-right request remains open; no new binding is justified
yet.

The authenticated physical observer opened event18 (keyboard) and event6
(consumer/mouse), both identified by the exact 1a86:fe00 USB ancestor. After
READY the user pressed and released rear-right; no function-key or actual BTN_*
identity was reported, while heartbeats continued. It made no grabs, input
injections, mappings or configuration changes. Event18 is grabbed by
InputPlumber and the filter excludes other keyboard codes, so this does not
prove the hardware emits no report. Ruled out: rear-right emits an observed
function-key or mouse/controller-button event on the ungrabbed event6 path in
this test. The next test needs HID reports and aggregate event counts to
distinguish another keyboard code, a grabbed interface and firmware-side
silence.

The combined authenticated capture opened event18, event6 and hidraw1/2/6 with
zero report/event baselines. The user pressed front Console then rear-right.
hidraw6 first recorded six boot-keyboard reports, then hidraw2 recorded two
vendor reports; all evdev counters and hidraw1 stayed zero. No function-key or
mouse-button identity appeared. This confirms that the raw reader bypasses the
grabbed keyboard and can see hardware reports. Ruled out: event18's grab
explains an unseen F16 keyboard emission during this test, because hidraw6 was
observed directly and reported no F16. The rear sequence is associated with
vendor-channel activity, but the counts-only observer discarded its payload;
decode the exact vendor fields before claiming a button identity or changing
firmware mappings.

The vendor-only decoder confirmed the actual rear-right packet: CID 0xB2,
matching 0x3f markers/footer, byte6=0x23 (M2/RightPaddle1), byte12=1 on press
and 2 on release, packet type 0x01, flag 0x80, function 0x02. The source's
`pressed = report.press_state == 1` agrees; its function byte is opaque and
only logged. The first decoder assumed release=0, so it correctly preserved the
actual state2 as rejected protocol metadata rather than falsely labeling it.
Forwarding must use the observed 1/2 pair. No more identity test is needed.

InputPlumber v0.81's existing OXP HID driver recognizes this B2 format, but the
current composite has only evdev18 and evdev16 sources. Kernel hid-oxp
raw_event handles B8 RGB replies and does not convert B2 buttons into keyboard
events. Ruled out: choosing another INI key alone fixes the current vendor
route, because the actual M2 packet never reaches a keyboard target. A narrow
per-install bridge can read only the vendor interface and send F8 through a
standalone InputPlumber keyboard target while Elite has focus, then
release/stop that target on exit. This avoids changing the main controller's
target list or global controller mappings. Implementation and local menu
testing are still pending.

The staged bridge passes measured M2 1/2 protocol and focus/debounce/release
tests. Live host PID 244679 is deck-owned, has the exact Odyssey game cwd and
matches the XWayland window's _NET_WM_PID; its class is steam_app_359320.
Dry-run identifies Elite foreground and leaves gamepad0 unchanged, with no
device open, target creation, key sends or file writes. Live DBus creation,
SendKey signature introspection and removal of an unattached keyboard0 target
also passed without sending keys or altering the main target. Subsequent local
installation and user-authorized testing confirmed menu forwarding; details
follow.

The user explicitly approved a read-only udev rule and the per-install bridge.
`pkexec /usr/bin/python3 -I -B /tmp/edvr_install_rear_read_access.py --install`
succeeded. The installed `/etc/udev/rules.d/70-edvr-onex-rear-button.rules`
matches hidraw only by USB bus, VID 1a86, PID fe00 and interface 02, assigning
group deck and mode 0640. The rule changed access only for hidraw2: owner root,
group ID 1000, mode 0640; deck UID 1000 can open it read-only and cannot write.
hidraw1 and hidraw6 remain root:root 0600 and unreadable to deck. The original
permissions snapshot is
`/etc/udev/rules.d/.70-edvr-onex-rear-button.state.json`; rollback is available
through `/home/deck/.local/share/elite-mesa-37bf953a58/rear_menu_access.py
--rollback` with local root authorization.

The bridge is installed at
`/home/deck/.local/share/elite-mesa-37bf953a58/rear_menu.py` (SHA256
bea553d04f0e88436bead487237186892b025db2971295bab046383de61762db). Its launcher
is `launch.py` (SHA256
0438254d21a8e6fce4228ef5ba4993505032d33b23b5bf1e8c4966b9621ad4f7), with the
original saved as `launch.py.before-rear-menu` (SHA256
781f898a252b7a5350313407bdae90b8933f5b884a1e51f9dcc94c4ca5b86c4e). The current
Steam launch options are unchanged; original Mesa setup and `execvpe` behavior
are preserved. Each launch creates its own keyboard target and forwards only M2
to F8 while the exact Elite PID and cwd are foreground; it closes on exit. The
test log records two F8 down/up pairs and `CLOSED main_targets_unchanged=True`.
User confirmed rear-right opens and closes the EDVR menu and desktop clicks
work.

At this earlier checkpoint controller navigation was still open. Source
inspection showed `menu.cpp` keyboard-only at lines 2763 and 3450; flat
navigation at line 4140 used `handleKeys`, XInput served hotkey capture at line
3311, and `input_gate.h` excluded pads at line 49. No existing INI key enabled
pad navigation. The implementation and validation status in the main XeSS
investigation supersedes that checkpoint. Its Windows build-gate caveats still
apply.

## October 13: desktop cursor and click recovery

The user reported the right stick moving the cursor only on the desktop, then
both physical mouse/trackpad and controller clicks failing. Pressing the front
QAM button restored clicks before a capture was made. No input settings were
changed during this investigation.

The live composite retained Default, deck-uhid as its only controller target,
and intercept mode none; its profile has no joystick-to-mouse mapping. Elite
and the per-launch rear-button bridge were absent. This suggests desktop input
state associated with Steam or InputPlumber, but does not identify the owner of
a held button or grab. Ruled out: the rear-button bridge actively holding
desktop input in this occurrence, because it was not running. No focused test
window, device restart, target replacement or firmware write was used. QAM
recovery is a measured workaround, not a permanent repair.
