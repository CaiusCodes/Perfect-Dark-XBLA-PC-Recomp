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
     100% story completion, Unlock all multiplayer content.
   All cheats unlocked makes every cheat available in the Cheats menu; you
   still switch on the ones you want. 100% story completion marks every
   mission complete on every difficulty (missions you never finished show a
   best time of 59:59; real times are kept). It is written into your save,
   so it stays after you untick it.
5. Click "Play" once it says READY TO PLAY. The extras you ticked are applied
   at that moment.

From then on, opening "Perfect Dark.exe" starts the game straight away.
The launcher only reads your package; it never changes, moves or deletes it,
and it is not needed again. Nothing is installed outside this folder and no
administrator rights are needed. It does not download anything and does
not contact Xbox LIVE.

WHERE THINGS LIVE
-----------------

Everything stays inside the extracted "Perfect Dark" folder:

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

SETUP AGAIN, UPDATING AND OLDER RELEASES
----------------------------------------

To change your extras or import your package again, start the game with
--setup (for example from a shortcut whose target ends in
"Perfect Dark.exe" --setup). Importing again replaces resources\assets only;
saves and settings in resources\userdata are kept. If a needed file goes
missing, opening Perfect Dark.exe shows the setup window again by itself.

To update, extract a new release over this folder (your resources\assets
and resources\userdata are kept), or into a new folder: setup finds the old
copy next to it and imports from it.

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
  Mouse sensitivity     1.5, invert look off
  In menus              Point with the mouse to highlight, left button selects,
                        right button goes back, wheel moves a row at a time.
                        W / S and E still work.
  Title screen          Any key or a left click starts.

An Xbox controller works too.

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

  Render Scale (Settings > Graphics) sets the internal rendering sharpness:
  1x, 2x or 3x the Xbox's 1280 x 720. It starts at 1x; higher looks sharper
  and needs a stronger graphics card. The scales other than the current one
  say "(requires restart)": choosing one restarts the game to apply it, or,
  during a mission or a match, applies it the next time the game starts.

  Upscaling (Settings > Graphics, next to Render Scale) sets how the picture is
  scaled to your window or screen: Off (smooth, the original look), Sharpen
  (AMD CAS sharpening) or FSR (AMD FSR 1 upscaling). FSR helps most with
  Render Scale 1x on a large screen, the fast setting for a slower graphics
  card or integrated graphics. Changes apply at once.

  The graphics API is chosen automatically: Direct3D 12 on NVIDIA cards,
  Vulkan on others. To pick one yourself, set gpu = "d3d12" or
  gpu = "vulkan" in resources\userdata\perfect_dark.toml.

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
    itself, not a title update, save or theme. The reason is shown in the
    window and in resources\runtime\logs\setup.log.
  - The launcher says a file in resources\runtime is missing: the download is
    incomplete or something removed a file. Extract the whole ZIP again
    (your resources\assets and resources\userdata are kept).

REPORTING A PROBLEM
  Every time the game runs it writes a log to resources\runtime\logs
  (perfect_dark_001.log and so on; the 20 newest are kept). When reporting a
  problem, send the newest one. It starts with your Windows version,
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
    4. Send the newest log from resources\runtime\logs.
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
