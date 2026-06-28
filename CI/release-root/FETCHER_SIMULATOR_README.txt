Fetcher Simulator public test install
====================================

This package does not include Bardcraft or its Nexus dependencies. Each player
must download those files through their own Nexus account.

Required downloads
------------------

1. Morrowind with Tribunal and Bloodmoon.
2. Tamriel Data. Use the "Tamriel Data (Vanilla)" file unless you already know
   you want the larger HD package.
3. Skill Framework.
4. Stats Window Extender.
5. Bardcraft (OpenMW): https://www.nexusmods.com/morrowind/mods/56814

Bardcraft's Nexus permissions do not allow reuploading the mod archive to other
sites. Do not ask another player to send you Bardcraft. Download it from Nexus.

First-time setup
----------------

1. Run openmw-wizard.exe from this folder.
2. Point the wizard at your Morrowind installation and finish its setup.
3. Return to this folder and run Install-Fetcher-Bardcraft-With-UMO.bat.

The Bardcraft installer stops before downloading mods if Morrowind.esm is not
registered in this portable install's openmw.cfg.

UMO install path
----------------

UMO can help download and extract Nexus mods for OpenMW.

UMO page: https://modding-openmw.com/mods/umo/
UMO source: https://gitlab.com/modding-openmw/umo

Basic UMO steps:

Fast path:

1. Double-click:

   Install-Fetcher-Bardcraft-With-UMO.bat

   Or run this one-liner from this folder:

   powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install-Fetcher-Bardcraft-With-UMO.ps1

The helper downloads umo.exe and tes3cmd.exe if they are not already present.
It also downloads a portable copy of the official 7-Zip command-line tools when
7-Zip is unavailable. Nothing is installed system-wide. The helper then uses the
included fetcher-bardcraft-umo.json modlist. If that file is missing, it tries to
download it from the Fetcher Simulator GitHub prerelease.

After UMO installs the Nexus version of Bardcraft, the helper downloads the
small Fetcher Bardcraft multiplayer compatibility patch from its own GitHub
prerelease. It verifies the patch checksum and the installed Bardcraft scripts
before applying anything. Unsupported or locally modified Bardcraft versions
are left unchanged and reported as an error.

The helper tells UMO to install the mods inside this package under:

   Data Files\fetcher-bardcraft

On first run, UMO may open a Nexus login page in your browser. Finish that login
and return to the console. The helper also registers its portable umo.exe as the
current user's nxm:// handler so Nexus "Slow Download" buttons can return files
to the waiting installer. After UMO finishes, the helper rewrites openmw.cfg with
the needed data= and content= lines.

Large Nexus downloads can take several minutes and the console may look quiet
while UMO is still downloading. Leave the window open until it reports that the
Fetcher public test load order was applied or prints an error.

Manual UMO steps:

1. Download UMO for Windows.
2. Open Windows Terminal in the folder that contains umo.exe.
3. Run:

   .\umo.exe setup
   .\umo.exe check

4. Use the Nexus "Mod Manager Download" button for Bardcraft and each
   dependency. UMO should catch the nxm:// links after setup.
5. Let UMO download and extract the mods.

For manual UMO usage, set UMO's mod install path to this package's Data Files
folder if you want the install to stay self-contained. Then run
Apply-Fetcher-Public-Test-Config.bat after the mods are installed.

Manual install path
-------------------

If you do not use UMO:

1. Create a folder such as C:\OpenMWMods\Fetcher.
2. Download Tamriel Data, Skill Framework, Stats Window Extender, and Bardcraft
   from Nexus.
3. Extract each mod into its own folder.
4. Add data= lines in this package's openmw.cfg for:

   - your Morrowind Data Files folder
   - Tamriel Data
   - Skill Framework
   - Stats Window Extender
   - Bardcraft

OpenMW must be pointed at the folder that directly contains the mod files, not
at a parent folder. For Bardcraft, the correct folder contains:

   Bardcraft.ESP
   Bardcraft.omwscripts
   scripts\Bardcraft
   meshes\Bardcraft
   sound\Bardcraft
   midi\Bardcraft

Apply the public test load order
--------------------------------

After the mods are installed and their data folders are listed in openmw.cfg,
double-click:

   Apply-Fetcher-Public-Test-Config.bat

The BAT file rewrites the content= lines in this package's openmw.cfg to match
the public test server load order. It also creates a backup of the previous
openmw.cfg next to the original file.

If the BAT reports missing files, install that mod or add the correct data=
folder to openmw.cfg, then run the BAT again.

OpenMW animation settings
-------------------------

This package enables the required Bardcraft settings automatically in both
settings.cfg files:

   shield sheathing = true
   smooth animation transitions = true
   use additional anim sources = true
   weapon sheathing = true

Community songs
---------------

The public server does not distribute custom MIDI files through Bardcraft by
default. Each player must install the same local song pack. Bardcraft matches
the local song content hash when synchronizing multiplayer playback, so a file
with the same title but different notes will not be substituted.

If Bardcraft reports a missing local song, obtain the pack from the server
community and install its loose .mid/.midi files under:

   midi\Bardcraft\custom

OpenMW cannot discover songs that remain inside a ZIP/7Z archive. Do not rename
another MIDI file to match; Bardcraft validates the actual song content hash.

Troubleshooting
---------------

If Bardcraft's menu does not open with B:

   - Bardcraft.omwscripts is not enabled, or
   - the Bardcraft data folder is not listed in openmw.cfg.

If instruments or outfits are missing:

   - Bardcraft.ESP is not enabled, or
   - Tamriel_Data.esm is not enabled, or
   - the wrong data folder was added.

If animations do not play:

   - Use Additional Animation Sources is not enabled, or
   - Bardcraft.omwscripts is not loaded.
