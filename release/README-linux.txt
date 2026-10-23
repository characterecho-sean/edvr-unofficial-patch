EDVR Linux wrapper for Steam + Proton

Requirements: Python 3 and an existing Steam install of Elite Dangerous with
its Proton prefix. The wrapper uses Steam's installed Proton and runtime
directly; it does not need Protontricks or system Wine. It can discover native
and Flatpak Steam installations when their files are available to your user.

Run Elite once from Steam, then close it. If you change Elite's Proton version
in Steam, launch Elite once again before using this wrapper. On Steam Deck, use
Desktop Mode. Run as your normal user, without sudo:

    ./edvr_installer.sh

This opens the Windows installer GUI with the Odyssey product folder selected.
For a flat install with a current installer build:

    ./edvr_installer.sh --install --scope flat

To see the Windows install plan without changing game files:

    ./edvr_installer.sh --preview --scope flat

Preview starts Steam's Proton and runtime. It may update prefix bookkeeping,
but the Windows installer's --dry-run makes no game file changes. The wrapper's
own --dry-run instead resolves and prints the exact paths and command without
starting a process or writing files. The shell launcher works from any
directory and forwards every option to the Python wrapper. You can also run
python3 edvr-installer-linux.py directly.

The wrapper also accepts --repair, --uninstall, and --collect-logs. Use
--game-dir '/path/to/product' for a different product folder inside the
installed game tree, or --installer '/path/to/edvr-installer.exe' for a
separate EXE. --steam-dir '/path/to/Steam' or STEAM_DIR selects a nonstandard
Steam root. --proton '/path/to/Proton' accepts a Proton directory or its proton
script if the prefix record is missing or stale. STEAM_COMPAT_DATA_PATH can
select an existing custom compatdata directory (the parent of pfx); its
pfx/drive_c must already exist.

The wrapper only launches the installer. Keep any existing required Elite
launch options; headset/OpenXR setup and Proton gameplay remain unverified
here.
