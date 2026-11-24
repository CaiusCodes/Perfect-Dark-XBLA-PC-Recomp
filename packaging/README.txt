PERFECT DARK XBLA PC RECOMP
===========================
Version @VERSION@

A native Windows port of the Xbox 360 (Xbox LIVE Arcade) release of Perfect
Dark, built with the ReXGlue static recompiler. This download contains no
original game program, artwork, music or other game data. You supply your own
legally obtained package; the launcher imports the game data from it.

IMPORTANT
---------

You need your own copy of Perfect Dark XBLA: the Xbox 360 package for

  Perfect Dark (Xbox LIVE Arcade)
  Xbox 360 Title ID: 584109C2
  Package file: 4287CC74C591DB37B577DAEA43BB6E51692D7A8E58 (the 2010
  release) or 8292DB976888C5DCD68C695F11B3DFED5F4512E858 (the re-release).
  Both carry the same game; other builds are refused.

The first time you start the game it reads the package you provide, checks
that it is the supported version and imports the game data from it into this
folder. After that the package is not needed any more.

Requirements:
  Windows 10 or 11 (64-bit) and a DirectX 12 capable graphics card.
  Nothing else needs installing: the Microsoft Visual C++ runtime the game
  uses is included.

FIRST-TIME SETUP
----------------

1. Extract the release ZIP to a writable folder (not Program Files, and not
   inside the ZIP). You get one folder, "Perfect Dark".
   Tip: put your package file beside "Perfect Dark.exe", or in a folder
   called "Game Package" next to it, and it is found automatically.
2. Open "Perfect Dark.exe". The first time, the setup window opens.
3. If your package was found it is checked and imported straight away.
   Otherwise drag the package file or the folder holding it onto the window,
   or click "Browse Package". The launcher checks it first and tells you if
   it is the wrong file or an unsupported version; the import starts as soon
   as it is accepted. You can cancel it at any time.
4. Tick any extras you want (all optional, all off unless ticked):
     Start in fullscreen, Create desktop shortcut, All cheats unlocked,
     100% story completion, Unlock all multiplayer content, Auto-Aim on.
   All cheats unlocked makes every cheat available in the Cheats menu; you
   still switch on the ones you want. 100% story completion marks every
   mission complete on every difficulty (missions you never finished show a
   best time of 59:59; real times are kept). It is written into your save,
   so it stays after you untick it. Auto-Aim on starts new players with
   Auto-Aim on, as on the Xbox (handy on a Steam Deck); players you already
   have keep theirs, set in Player Setup > Control.
   Game language picks the language of the game's menus, briefings and
   subtitles: English, French, German, Italian or Spanish (click it to
   change). You can also change it in the game, in Help & Options >
   Settings > Display Interface > Language. The PC port's own menu labels
   stay in English.
5. Click "Play" once it says READY TO PLAY. The extras you ticked are applied
   at that moment.

From then on, opening "Perfect Dark.exe" starts the game straight away.
Hold Shift while opening it to get the setup window back.
The launcher only reads your package; it never changes, moves or deletes it,
and it is not needed again. Nothing is installed outside this folder and no
administrator rights are needed. It does not download anything and does
not contact Xbox LIVE.

WHERE THINGS LIVE
-----------------

Everything stays inside the extracted "Perfect Dark" folder (saves and
settings too, unless you choose another folder for them, see below):

  Perfect Dark.exe            Open this to play (and to set up the first time)
  README.txt, licenses\       This file and the third-party notices
  resources\
    runtime\                  The game program and its DLLs
      logs\                   Game and setup logs
    assets\                   Game data imported from your package
    userdata\                 Saves, leaderboards, achievements, caches and
                              perfect_dark.toml (settings and key bindings)

Paths are relative, so the folder can be moved or copied anywhere (a USB
drive works too). To back up your progress, copy resources\userdata.

To keep saves and settings somewhere else (another drive, or a folder you
back up or sync), tick "Keep saves and settings in another folder" in the
setup window and choose the folder. Press Play to apply it: what is in
resources\userdata is copied there, the old copy is left as it was, and from
then on the game uses only the chosen folder (it holds everything that
resources\userdata would). resources\userdata-location.txt records the
choice; updates keep it. Untick the box to go back to resources\userdata,
which takes back any newer files. If the chosen folder is missing (a drive
not connected), the game does not start with empty saves; it says so instead.

SETUP AGAIN, UPDATING AND OLDER RELEASES
----------------------------------------

To change your extras or import your package again, hold Shift while
opening "Perfect Dark.exe" (keep holding until the window appears), or start
it with --setup (for example from a shortcut whose target ends in
"Perfect Dark.exe" --setup). Importing again replaces resources\assets only;
saves and settings in resources\userdata are kept. If a needed file goes
missing, opening Perfect Dark.exe shows the setup window again by itself.

To update, open Help & Options > Settings > Check for Updates in the game. It asks
GitHub for the latest release (only when you choose it; nothing else is sent).
If there is a newer one, Update now closes the game; the launcher downloads
the release, checks it against the checksum GitHub publishes, replaces the
program files (Perfect Dark.exe, README.txt, licenses and resources\runtime)
and offers Play. Your game data, saves and settings in resources\assets and
resources\userdata are never touched. You can still update by hand: extract a
new release over this folder (your resources\assets and resources\userdata
are kept), or into a new folder: setup finds the old copy next to it and
imports from it.

Coming from an older release (Setup Perfect Dark.exe and a Game folder):
extract this release into the old release folder or next to it. The launcher
finds the old Game folder and offers it alongside any package, so you do not
need the package again, and your saves and settings come with it. The old
folder is never changed or deleted; remove it yourself once you are happy.

KEYBOARD AND MOUSE
------------------

  Mouse and keyboard controls are always on. The default layout is:

  Move                  W A S D
  Look                  Mouse
  Fire                  Left mouse button
  Aim                   Right mouse button
  Zoom                  Middle mouse button
  Reload                R
  Use / activate        E
  Next weapon           Mouse wheel up
  Previous weapon       Mouse wheel down
  Weapon menu           Q (hold, move the mouse towards an item, release)
  Secondary fire        F
  Crouch                Ctrl
  Objectives            Tab
  Pause                 Esc
  Switch graphics       G (Texture Pack 1 / Texture Pack 2, Settings > Graphics >
                        Texture Packs; nothing until Texture Pack 2 is set)
  Mouse sensitivity     1.5, invert look off
  In menus              Point with the mouse to highlight, left button selects,
                        right button goes back, wheel moves a row at a time.
                        W / S and E still work.
  Title screen          Any key or a left click starts.

An Xbox controller works too. Settings > Gamepad Controls > Control Options has
two pages of PC options for it:
  Stick Settings        Right / Left Stick Dead Zone (how far a stick moves before
                        it counts) and Right / Left Stick Response (higher is
                        slower near the centre for precise aiming; full tilt is
                        unchanged). "Original" plays as the game always did.
  Advanced Control      from Rare's 2010 Xbox 360 update: Legacy (left stick moves
                        and turns, right stick strafes and looks up and down;
                        holding Aim, the left stick aims),
                        Southpaw (sticks swapped), Swap Triggers, Secondary Fire
                        Reset, Lock Sight (the crosshair stays centred while you
                        turn), and Aim Sensitivity (how fast you aim while
                        holding Aim; "Original" is this version's own speed).
                        One setting for every controller.

Diagonals: holding two movement keys (W+A, W+D) moves as fast as the fastest an
Xbox controller's thumbstick can go on a diagonal, so keyboard and controller
players are on equal terms in multiplayer. Both are about 40% faster than walking
straight ahead (strafe-running), as in the original game. Settings in
resources\userdata\perfect_dark.toml, if you want to change that:
  mnk_diagonal_reach = 117    how far the keyboard may push the stick on a diagonal,
                              in percent of a round stick's travel (100 to 141)
  mnk_strafe_running = true   no limit: the N64-style diagonal (about 48% faster than
                              straight ahead, more than any controller can do)

CHANGING KEYS
  Open Help & Options > Settings > Keyboard & Mouse. Select an action (E), then
  choose "Change key", "Add a second key" or "Reset to default". When the row
  says "Press a key...", press the key, mouse button (the side buttons too:
  Mouse 4 and Mouse 5) or mouse wheel you want.
  Esc cancels. If the key already belongs to another action, the two swap.
  Changes apply at once and are saved. Mouse Sensitivity and Invert Look are
  on the same page. Esc, F4, F7 and F9 are reserved and cannot be bound.

DISPLAY
  Settings > Graphics adds Resolution (1280 x 720 up to 3840 x 2160),
  Game Display (Windowed / Fullscreen), Vsync and Show FPS. Changes apply
  immediately.

  The game is drawn by Plume, a renderer that draws it natively with
  DirectX 12, or with Vulkan: Settings > Graphics > Graphics API (the game
  restarts to switch). DirectX 12 is the default; try Vulkan if DirectX 12
  gives you trouble on your graphics driver. If your PC cannot run Vulkan,
  the game stays on DirectX 12.

  If something looks wrong or the game will not start drawing, the Classic
  renderer of earlier versions (an emulated Xbox 360 graphics chip) is still
  there: close the game, open resources\userdata\perfect_dark.toml in Notepad,
  add the line  gpu_native = false  and start the game again. Remove the line
  to go back to Plume.

  Render Scale (Settings > Graphics) sets the internal rendering sharpness:
  1x, 2x or 3x the Xbox's 1280 x 720. It starts at 1x; higher looks sharper
  and needs a stronger graphics card. The scales other than the current one
  say "(requires restart)": choosing one restarts the game to apply it, or,
  during a mission or a match, applies it the next time the game starts.

  Upscaling (Settings > Graphics, next to Render Scale) sets how the picture is
  scaled to your window or screen: Off (smooth, the original look) or FSR
  (AMD FSR 1 upscaling). FSR helps most with Render Scale 1x on a large
  screen, the fast setting for a slower graphics card or integrated graphics.
  Sharpness, below it, sharpens the picture: Off, Low, Medium or High (AMD
  CAS sharpening, or FSR's own sharpening when Upscaling is FSR). It works at
  any Render Scale and helps most at 1x. Changes apply at once. The old
  "Sharpen" choice carries over as Sharpness Low.

  Vivid Colours and Black Level (Settings > Graphics) adjust the finished
  picture, which was made for old TVs and can look flat on a modern screen.
  Vivid Colours makes colours richer; Black Level makes black truly black
  without losing the shadows above it. Each is Off, Light, Normal or Heavy,
  both start Off, and changes apply at once.

  F12 saves a screenshot to resources\userdata\screenshots.

  Settings > Graphics > Graphics API picks DirectX 12 or Vulkan. In
  resources\userdata\perfect_dark.toml the same setting is
  gpu_native_api = "d3d12" or "vulkan" (gpu = "vulkan" also picks Vulkan
  when gpu_native_api is not set). If Vulkan cannot run on your PC, the game
  says so and uses DirectX 12. With the Classic renderer (gpu_native = false),
  gpu = "d3d12" or "vulkan" picks its API; otherwise it uses DirectX 12 on
  NVIDIA cards and Vulkan on others.

  Vsync On shows each frame at the screen's next refresh, with no tearing
  and no added input lag; Off shows frames at once (lowest latency, may
  tear). The game itself always runs at its own 60 frames a second.

  High-refresh screens: at 60, 120 or 240 Hz every frame is shown for the
  same time. At 144 or 165 Hz, 60 frames a second cannot be spread evenly
  (some frames stay up one refresh longer), which can look like a slight
  judder. Turn on G-Sync or FreeSync if your screen has it, or set the
  screen to 120 Hz in Windows display settings while you play.

  On a variable-refresh screen (G-Sync, FreeSync) you can try
    present_pace_to_guest_vblank = true
    present_pacing_offset_us = 9000
  in resources\userdata\perfect_dark.toml: frames are then shown exactly
  evenly spaced, at the cost of about half a frame more lag. It is off by
  default.

TEXTURE PACKS AND N64 TEXTURES
  Settings > Graphics > Texture Packs. Work in progress; Plume only. The page
  has four rows: Texture Pack 1, Texture Pack 2, Import Texture Pack... and
  Texture Tools.

  Texture Pack 1 and Texture Pack 2 are the two looks the G key switches
  between, anywhere, on the fly (change the key under Settings > Keyboard &
  Mouse > Switch Graphics; D-pad Down on a controller during play does the
  same). Each can be Remastered, N64 (with your N64 ROM imported) or any
  texture pack you have; Texture Pack 2 can also be None, and then G does
  nothing. Out of the box Texture Pack 1 is Remastered and Texture Pack 2 is
  None. Choosing a value shows it at once. The game always starts on Texture
  Pack 1 (G's choice lasts until you quit). A pack is drawn over the look it was
  made for (the game works this out from the pack's pictures: PD Forever over
  the N64 look, Ultimate and XBLA Plus over the remastered one; without an N64
  ROM, over the remastered one). A pack you import shows up in both lists
  straight away; nothing switches on by itself.

  A pack is switched in one go: the first time it is used the game checks its
  pictures (usually under a second, a few seconds from a slow disk), then loads
  the ones on screen while the old look stays, then switches. Meanwhile its
  row says "Texture Pack 1 (applying)". A pack chosen last time is ready
  before the title screen. Once both Texture Packs are set, the one not shown
  is kept ready in the background, so G switches in the next frame with
  nothing loading. That uses some extra video memory while Texture Pack 2 is
  set (about what the other look's pictures on screen take: with Ultimate and
  Forever in the Carrington Institute, under 200 MB); with Texture Pack 2
  None nothing extra is kept. When a level starts (and at a cutscene's cuts)
  with a pack shown, the picture waits a moment, at most a couple of seconds,
  until the pack's pictures for it are loaded, so they appear at once instead
  of popping in.

  Import Texture Pack... opens one Windows file picker for everything:
    - your own N64 Perfect Dark ROM (.z64, .n64 or .v64, any byte order:
      NTSC-U 1.0 and 1.1, PAL and NTSC-J, the last not tested yet; checked
      by its content, not its name, so a modified or bad dump is refused with
      the reason),
    - a texture pack's .zip,
    - pictures named by texture number (several at once): they become a new
      pack named after their folder; pictures without such a name are left
      out, as the game can't tell which texture they replace.
  For a pack that is a folder, press "Import a folder instead..." in the
  picker and choose the folder. The ROM is copied to resources\n64 and packs
  to resources\texture-packs (Texture Tools > Open Texture Packs Folder shows
  it; a folder copied there by hand works too). An installed pack is never
  replaced: importing the same pack again (even under another name) says it is
  already installed, and a different pack under a name that is taken is
  installed as "Name (2)". A pack's name comes from its .zip or folder, made
  readable (PD.Forever.Plus.HD.v0.10.zip becomes "PD Forever Plus HD v0.10").
  A notice says when an import starts and how it ended. A different N64 ROM
  imported while the game runs is used from the next start.
  Only pictures (real PNG and JPEG files) and .txt / .md notes are copied
  from what you import; anything else in a pack is ignored, and a .zip that
  would write outside its own folder is refused. Nothing in a pack is ever
  run.

  N64 textures: once the ROM is imported, choose N64 for Texture Pack 1 or 2
  (importing it doesn't change them). N64 mode draws the N64's textures,
  characters, guns, props, level rooms, skies (with the N64's star field where
  it had one) and water from your ROM; the HUD, crosshair, fonts and menus
  keep their remastered look. With the Japanese (NTSC-J) ROM, only the N64
  textures are drawn (its characters, guns, props and rooms stay remastered);
  it is not tested yet. It only changes what you see: in a network match each
  player can use either look. This download contains no N64 data: only your
  ROM is read.

  To compare a pack with the game's own textures, set Texture Pack 2 to
  Remastered (or N64) and press G.

  Packs made for the N64 PC port and Dabs Mod (ext_tex packs, such as the
  Perfect Dark Plus HD packs from github.com/retro-foundry/Perfect-Dark-Plus-
  HD-Textures: download them there, then import the .zip) work as they are:
  pictures named by texture number in hexadecimal ("00C6.png", "000f.jpg"),
  PNG or JPEG. Pictures in the pack's own folder are drawn over the N64
  textures and, where they fit, over the remastered ones; pictures in its
  "xbla" folder are drawn over the remastered textures first, including HD
  versions of the game's fonts. Folders named after N64 model files (such as
  PcaseZ) are matched to those models' textures through your N64 ROM. The PC
  port's own font folders (pictures of the N64 fonts, which this game doesn't
  use) are left out. A picture made for a texture of another shape is
  skipped, so a pack made for the N64 textures doesn't smear across the
  remaster's reworked surfaces; those keep their remastered picture. The
  packs are their makers' work and are not part of this download.

  N64 emulator HD packs (made for N64 emulators, pictures named like
  "Perfect Dark#1A2B3C4D#2#0#..._ciByRGBA.png", the Rice / GLideN64 hi-res
  naming) work too: import the pack's .zip or folder. They are matched to the
  game's textures through your own N64 ROM, so they need it imported, and
  they are drawn over the N64 look.

  Making a pack: Texture Tools > Dump Game Textures... opens a list of the
  texture sources: Remastered (always first), N64 (with your ROM imported)
  and every pack you have. Choose one and its textures are saved in the same
  layout as those packs, each source in a folder of its own under
  resources\userdata\texture-dump:
    - Remastered: every remastered texture as ext_tex\xbla\<number>.png,
    - N64: every N64 texture as ext_tex\<number>.png,
    - a pack: what the game takes from that pack, as packs\<pack>\<number>.png
      (and its xbla folder), with dump-summary.txt saying what was left out.
  Numbers are in hexadecimal, pictures the right way up; files already there
  are left alone. A notice says when it starts and when it's done (the
  remastered dump takes about a minute). Texture Tools > Open Texture Dump
  Folder opens it. Edit them at any size up to 4096 x 4096 (PNG, or JPEG for
  pictures without transparency) and put them in a folder named after your
  pack, the same way: N64 textures at its top, remastered ones in its "xbla"
  folder. Pictures a pack leaves out keep the game's own. A picture the game
  can't use (damaged, too large, or the wrong shape) is skipped and named in
  the log. To see which numbers a scene uses, set pd_texture_dump = true in
  resources\userdata\perfect_dark.toml: then only the textures the game draws
  are saved, as it draws them.

  A pack's settings file: a text file named pd-pack.txt in the pack's folder
  (beside its pictures; in a .zip, in the pack's folder or above it) can
  switch off the remaster's reflections (the shine it adds to costumes,
  guns and floors from its environment maps) on chosen textures, for a pack
  that repaints them as something that shouldn't shine:
      # Joanna's combat suit
      no_reflection = 12A6
  Numbers as the pack names its pictures (hexadecimal in a pack named that
  way, decimal in a decimal one; "0x12A6" is always hexadecimal); several on
  one line separated by commas or spaces. Lines starting with # are notes.
  The log says how many textures it read ("drawn without reflections").
  It can also keep the game's own picture for chosen textures, in both looks,
  where the pack's picture doesn't suit the game:
      # this texture keeps the game's own picture
      keep_original = 0123
  The log says how many it read ("keep the game's own picture").

  Reflections by part: no_reflection switches the shine off on every part of
  a model drawn with that texture. To keep it on some parts (say a costume's
  metal guards) and off others (its cloth), add a reflection mask instead: a
  picture named like the texture's with "-reflect" after the number, beside
  it (for Joanna's suit, xbla\12A6-reflect.png), white where the game may
  shine and black where it shouldn't, laid out like the texture's own picture
  (any size: a small one is fine). Each part of the model is drawn without
  reflections when most of it lies on black. The log names each part it
  decides ("a part of 518 vertices (518 on black) drawn without
  reflections").

  Pictures for this game only: a picture named with "-xbla" after the number
  (0046-xbla.png, xbla\12A6-xbla.png) is read by this game alone; in the
  remastered look it replaces the plain picture for that number (the N64 look
  keeps the plain one). So a pack can carry fixes for this game's remastered
  look and still work unchanged in the N64 PC port and Dabs Mod, which don't
  read it. The log counts them ("pictures with -xbla used").

  Detail textures: a few N64 surfaces are drawn from two textures, the
  surface's own and a fine "detail" texture blended in close up: the dataDyne
  carpet (0046, with the carpet pile 0074) and three of Grid's floors and
  walls (0DA6, 0DA8, 0DA9, also with 0074). The remaster drew them from one
  texture with the detail built in, so in the remastered look the game draws
  a pack's 0074 into its picture for the surface, as the N64 does. Leave
  0074 out (or give 0046 the detail itself in the pack's "xbla" folder) to
  draw the surface's picture alone.

  Packs made from version 0.9.2's dump (decimal numbers like "0123.png" or
  "0123 wall.png", in its remastered and n64 folders) still work as before. A
  pack is read the new way when it has an "xbla" folder (an empty one will
  do) or a picture whose number has a letter A-F in it.

  Without the menus: Perfect Dark.exe --setup --unattended --n64-rom <file>
  and --texture-pack <.zip or folder> do the same imports.

LEADERBOARDS
  Leaderboards on the main menu shows your own times and scores: an overall
  board, mission totals, and one board per mission and challenge. They are
  kept on this PC in resources\userdata and are never sent anywhere, so the
  only player ranked is you. Change Filter switches between the three views,
  and View Player Info says whose scores these are.

ACHIEVEMENTS
  Achievements on the main menu lists the game's 20 achievements, what each
  one needs, and which you have earned. They unlock as you play and are kept
  in resources\userdata. Nothing is sent anywhere.

MULTIPLAYER
  Play Game > Combat Simulator > Match Type:
    Host Match          opens your lobby for others to join.
    Join Match          opens the Multiplayer screen:
      LAN               lists the games hosted on your network. Select one
                        to see its mode and arena, then "Join this match".
      Direct Connect    type the host's address (an IP address or a
                        hostname) and connect straight to their game.

  How many can play, as in the original game:
    Local split-screen on one PC     up to 4 players
    LAN / Direct Connect             up to 8 players
    Combat Simulator                 up to 12 in a match, players and bots
    Co-Operative / Counter-Operative 2 players

  Nothing touches the network until you host or choose LAN or Direct Connect;
  that is when Windows Firewall asks. Allow it on private and public
  networks - Windows often treats a virtual LAN as a public network.

  LAN works on a home network, and on any virtual LAN the players set up
  themselves (Hamachi, ZeroTier, Radmin VPN and the like): everyone joins the
  same virtual network, one player hosts, the others choose LAN. If a virtual
  network doesn't show the game, use Direct Connect with the host's address
  on it. Direct Connect over the internet needs the host's router to forward
  UDP ports 1000, 1001 and 3076 to the host's PC.

  While you host, the lobby shows Your Address: this PC's addresses, each
  named by its network (LAN, Wi-Fi, Hamachi, ZeroTier...). Open it and pick
  one to copy it to the clipboard, then send it to friends for Direct
  Connect. The same list is in Help & Options > Settings > Online, and
  Invite Friends (X on a controller, R on the keyboard) shows it too.
  Everyone needs the same version of this port.

  Two PCs never keep exactly the same time - one can run the game a fraction
  of a percent faster than the other, which used to add up over a match until
  other players zipped about and got harder to aim at the longer you played.
  Joining PCs now keep their game in step with the host's, so a match plays
  the same at minute ten as at minute one. This relies on everyone running the
  same version: an older one cannot read the timing the newer one sends.

  YOUR NAME AND THE ONLINE PAGE
  Help & Options > Settings > Online shows the name other players see and
  whether online play is on. Select
  Player Name and choose "Change name..." to type a new one: Enter saves it,
  Esc cancels. A new name shows on your own screen at once, but in a network
  lobby the other players keep seeing the old one until they or you leave
  the lobby and join again (the game sends names only when a player joins).
  Switching online play on or off there takes effect the next time you start
  the game.

  Optional settings in resources\userdata\perfect_dark.toml:
    net_player_name    the name other players see (default: your Windows name)
    net_address        the IP address this PC uses (default: automatic)


THIS IS A DEVELOPMENT PREVIEW
Boot, menus, mission select, briefings and gameplay (movement, mouse look,
weapons, lens flares) have been tested on early missions. The whole campaign
is unlocked, but most missions and multiplayer have not been played through.

Known issues:
  - The IR scanner has not been retested since the dark-room lighting fix.
  - A crash has been seen on one mission briefing screen.
  - Xbox LIVE itself is not available: leaderboards and achievements are
    kept on this PC only. The menus say "online" where the original said
    Xbox LIVE.
  - Network play is new. It has been tested with two PCs and with four
    copies on one PC; matches with five to eight players, Co-Operative and
    Counter-Operative over the network, and a host leaving mid-match have
    not been tested yet.

TROUBLESHOOTING
  - Extract the ZIP first. Do not run Perfect Dark.exe from inside the ZIP.
  - Perfect Dark.exe is not digitally signed, so the first time you open it
    Windows may show "Windows protected your PC". Choose More info, then Run
    anyway.
  - The launcher rejects the package: choose the Perfect Dark game package
    itself, not a title update, save or theme. Only the two package files
    named at the top of this file work. The reason is shown in the window
    and in resources\runtime\logs\setup.log.
  - The launcher says a file in resources\runtime is missing: the download is
    incomplete or something removed a file. Extract the whole ZIP again
    (your resources\assets and resources\userdata are kept).
  - Only part of the picture shows after changing Render Scale, or the game
    cannot be used to change it back: close the game, open
    resources\userdata\perfect_dark.toml in Notepad and set
        pd_render_scale = "1"
    (or delete that line), then start the game again. Render Scale is the
    only scale setting the game uses: resolution_scale and
    draw_resolution_scale lines in the file are ignored. Please report it
    with your newest log (see below).

REPORTING A PROBLEM
  Every time the game runs it writes a log to resources\runtime\logs
  (perfect_dark_001.log and so on; the 20 newest are kept). To report a
  problem, open an Issue at
  https://github.com/CaiusCodes/Perfect-Dark-XBLA-PC-Recomp/issues
  and attach the newest one. It starts with your Windows version,
  processor, graphics card, driver, screen and main settings; it holds no
  saves or passwords. If the game crashed, the log ends with a [crash] line,
  and a crash-<date>-<time>.dmp file is written beside it: send that too.

  For stutter, slowdowns, lag in multiplayer, or anything that comes and goes,
  a detailed log helps most:
    1. Close the game and open resources\userdata\perfect_dark.toml in
       Notepad.
    2. Change "diagnostics = false" to "diagnostics = true" (add the line if
       it is not there) and save.
    3. Play until the problem has happened a few times, then close the game.
    4. Attach the newest log from resources\runtime\logs to your Issue.
    5. Set it back to false afterwards.
  The detailed log records every slow frame and what the game was waiting
  on, how the game, the graphics card and the screen kept time, and in
  multiplayer the packet timing to each player.

LEGAL
Perfect Dark, Xbox, Xbox 360 and related names and marks belong to their
respective owners. This project is not affiliated with or endorsed by them.
License notices for the open-source components are in the licenses folder.

After setup, resources\assets contains data from your own package. Do not
share the folder once you have played from it; share the release ZIP instead.

Perfect Dark.exe in the main folder (the launcher and setup) includes a
package reader derived from Velocity and is licensed under the GNU GPL
version 3 (licenses\LICENSE-Extract-STFS.txt); its source is in the project
repository. The game in resources\runtime is a separate program and is not
covered by the GPL.
