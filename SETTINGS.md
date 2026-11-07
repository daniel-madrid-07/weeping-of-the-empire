<!-- Generated from the game's code: change the code, not this file. -->
# Settings

Press **F10** in the game to open the settings window. Most rows apply at once; a row that needs a restart
says so, and **Restart the game now** at the bottom does it. The most used rows are also in the game's own Options
screen. The text under each row below is the help line F10 shows for it at the bottom of the page.

Your settings are kept in `%APPDATA%\nxrecomp\TotK-engine\settings.ini` and survive updates.

## Graphics > Display

- **Graphics preset**: Sets every quality row below at once. Ultra is the game at its best on an 8 GB card; Low and Medium draw less than the Switch does. Changing a row by hand makes it Custom.
- **Window mode**: Windowed, borderless fullscreen (the default: Alt+Tab is instant) or exclusive fullscreen (the monitor switches to the resolution and refresh rate below). F11 or Alt+Enter also switch it.
- **Window size**: The window's size in windowed mode. Fullscreen uses the monitor's resolution.
- **Graphics card**: The graphics card the game runs on, for a laptop with two. Automatic picks the dedicated one, the faster; the integrated one uses less power but is much slower.
- **Monitor**: The monitor fullscreen uses and a windowed start opens on. Remember: the one the game was on last.
- **Exclusive resolution**: The resolution the monitor switches to in exclusive fullscreen. Borderless and windowed keep the desktop's.
- **Exclusive refresh rate**: The refresh rate in exclusive fullscreen. Higher is smoother when the frame rate keeps up.
- **Frame rate**: The most frames per second the game shows. Higher is smoother and costs more GPU and CPU; 30 is the Switch's. Unlocked follows the monitor's refresh rate (240 at most); steady holds a rate the scene can keep. For an even picture pick a cap that divides the refresh rate (30 or 60 on 60 Hz, 40 on 120 Hz), or turn VRR on. Steam Deck: 60 (OLED: 90) at first; if it is not held, lower it to 40 with the screen at 40 Hz (Quick Access > Performance).
- **Steady's lowest rate**: Frame rate "Unlocked, steady" only: the lowest rate it holds. 30 (default) suits most PCs. On one that cannot hold 30, 20 or 15 show every frame for the same time (3 or 4 refreshes on 60 Hz) instead of an uneven 15-20.
- **Vsync**: On (default): no tearing. Off: slightly less input latency, with tearing lines.
- **Variable refresh (VRR)**: Variable refresh rate (G-SYNC, FreeSync): the monitor waits for each frame, smooth below its refresh rate and without tearing. Needs a VRR monitor with VRR on in Windows or the driver.
- **Perf overlay (F9)**: Shows fps, the 1% low and a frame-time graph over the game (F9), to check performance.
- **Shader compile indicator**: Shows when shaders are being compiled (bottom right): a short stutter while it is up is the compile, not your PC. Off hides it in play (a first start still shows its progress); the compiling still happens.
- **Fps readout (F3)**: A small frame-rate counter in a corner (F3 shows or hides it). Minimal: fps; Detailed adds the frame time, the 1% low and the worst frame. Off costs nothing.
- **Fps readout corner**: The counter's corner. Top right keeps clear of the hearts and the map.
- **Fps readout size**: The counter's size. It also grows with the window's height.

## Graphics > Colour

- **Colour adjustment**: Colour adjustment of the game's picture (not these menus). Off shows the game's own colours and costs nothing; Vivid is livelier; Brighter dark tones lightens caves, the Depths and nights; Custom uses the four rows below.
- **Brightness**: Custom only: makes every colour brighter or darker; 0 is the game's.
- **Contrast**: Custom only: more or less difference between light and dark; 1 is the game's.
- **Saturation**: Custom only: how colourful the picture is; 1 is the game's, 0.5 greyer, 2 twice as colourful.
- **Gamma**: Custom only: below 1 brightens the dark tones, above 1 darkens them; 1 is the game's.

## Graphics > Rendering

- **Render resolution**: The resolution the 3D scene is drawn at, against the game's own. Higher is sharper and costs much more GPU; lower is faster and blurrier. The default: the window's resolution.
- **Dynamic resolution**: Lowers the 3D resolution only when the GPU falls behind, never below the floor chosen, to hold the frame rate. Off keeps the picture sharp all the time; On is smoother on a slower GPU.
- **Native-resolution interface**: Above 1080p, menus, text and the HUD are drawn at the window's resolution instead of the game's 1080p stretched, and the 3D scene (render scale, DLSS / DLAA) reaches the window's resolution too. Sharper picture for a little GPU.
- **Upscaler**: Upscales the 3D scene with a temporal upscaler that builds detail over several frames. DLSS: NVIDIA RTX only; DLAA: DLSS at full resolution, the best image for more GPU. FSR and XeSS: any GPU (XeSS runs fastest on Intel Arc). All cost some GPU; Off is the game's own.
- **Frame generation**: Generated frames between the real ones make motion look smoother, but the game itself does not run faster and input latency rises a little. DLSS x2: RTX 40 and newer, x3 and x4: RTX 50; FSR x2: any GPU. Best when the game already runs well.
- **Frame generation mode**: Smooth keeps the frames shown in step with the monitor's refresh. Maximum fps adds the generated frames to every real frame the PC makes, even past the refresh rate: more frames than the screen shows (the fps readout counts them all), for VRR panels or low latency; Smooth looks steadier on a fixed refresh. With V-sync off the picture can tear.
- **DLSS model**: Which AI model DLSS and DLAA use. NVIDIA's default picks one for each quality. Preset K is the sharpest; preset J ghosts a little less behind moving things but flickers a little more. Changing it rebuilds DLSS once (a short pause).
- **DLSS character trails**: Where something moved (Link, animals, a swung weapon), DLSS and DLAA can keep blending in older frames, which leaves faint trails, or trust the new frame, which removes them but can make those edges flicker a little. Fewest trails is the default.
- **3D upscaler**: How the 3D scene is scaled up to the output: Bicubic (the port's) or FSR 1 (the game's own). Matters only below 100 % render resolution; either costs little.
- **Scene upscaler**: The filter that scales the 3D scene when the render resolution is not 100 %: Catmull-Rom, or FSR 1 EASU (sharper edges). Either costs little.
- **Window upscaler**: How the finished picture is scaled to the window: Bilinear, or FSR 1 for a window above 1080p (sharper). Costs little.
- **Anti-aliasing**: Smooths jagged edges on the final picture. SMAA 1x: smoother edges for a little GPU; Off is the game's own look.
- **Sharpening**: Sharpens the final picture (AMD RCAS). More is crisper but can look grainy; Off is the game's look. Costs very little.
- **Anisotropic filtering**: Keeps ground textures sharp when seen at a shallow angle. Higher is sharper for a little GPU; 16x suits most GPUs. The game's: its own setting.
- **Texture LOD bias**: Which texture detail level is used; 0 is the game's. Below 0 is sharper but can shimmer; above 0 is blurrier and can break textures (red grass). Not a quality slider: leave it at 0.
- **Compact textures (low VRAM)**: For graphics cards with 4 GB of video memory or less: the textures take a quarter of the memory, so the game no longer runs out of it (stutters, blurry or late textures). Fine colour detail gets a little softer. Automatic turns it on only on such cards.
- **Shadow map scale (renderer)**: The resolution of the renderer's shadow maps: 1x is the Switch's, 2x (default) sharper, 3x and 4x sharper still for more GPU time and memory. With little video memory they are lowered to what fits (4 GB cards: 2x).
- **Soft shadows**: Sun shadows grow softer the farther they fall from what casts them, as in real light. Off is the game's look; on costs some GPU. A change rebuilds the shadow shaders in the background: the old shadows show until then.
- **Contact shadows**: Adds the small sun shadows the shadow maps miss: at feet, under stones, in grass. Off is the game's look; Subtle to Strong cost some GPU.
- **Shipped shader list**: Prepares the shaders shipped with this version before you meet them, so new places stutter less the first time. Off: only the shaders this PC has built itself. On is better.
- **Shader wait**: A shader still being built: Limit to 0.1 s (default) shows the frame after 0.1 s and the missing objects appear a moment later; Always wait never shows anything incomplete but can freeze the picture for a second or more.

## Graphics > Presentation, HDR

- **Frame pacing**: Where the frame cap waits: at the present (default, steadier frame times) or before the frame (slightly less input latency).
- **Game tick source**: What the game's 60 Hz tick follows: a 60 Hz clock (default, right for any monitor) or the monitor's vblank (a fixed-refresh monitor only). Leave the default.
- **Precise pacing**: The last part of the frame cap's wait busy-waits for exact timing: steadier frame times for some CPU. Off is the default.
- **Queued frames**: How many finished frames may wait to be shown. 1 lowers input latency, 3 evens out uneven frames; 2 is the default.
- **GPU frames in flight**: How many frames the CPU may prepare ahead of the GPU. 2 (default) is the balance; 1 lowers latency but is slower when both are busy; 3 adds latency.
- **Low latency**: Holds each frame back just before it reads your input, so it waits less afterwards: lower input latency. NVIDIA uses Reflex; Boost also keeps the GPU clocks up (more power).
- **HDR output**: HDR output for an HDR monitor (HDR10 or scRGB). Needs HDR on for that monitor in Windows; otherwise the picture stays SDR.
- **Peak brightness (nits)**: Your monitor's peak brightness in nits, so highlights are neither clipped nor dim. Use the value the monitor states.
- **Paper white (nits)**: How bright ordinary white (menus, the sky) is in HDR. Raise it if the picture looks dim, lower it if it is too bright.
- **Highlight colour**: How much of their real colour bright lights keep in HDR: a torch flame or lava stays orange instead of turning yellow-white. 0 (the default) shows the picture as in SDR, only brighter; the HUD is left as it is.

## Graphics > Game patches

- **Aspect ratio**: The window's (the default): on a 3:2, 16:10, 21:9, 32:9 or 48:9 window or monitor the picture takes that shape, 21:9, 32:9 and 48:9 with a wider view; other shapes, 16:9 among them, keep the game's own. 16:9 / 16:10 / 21:9 / 32:9 / 48:9 / 3:2: that shape whatever the window. A new shape's layouts are made at its first start (about 30 s, once). A wider picture costs more GPU. The window's is the default from v0.1.9; a saved 16:9 switches to it once (pick 16:9 again to keep it). If you move or resize the window to another shape, the game tells you to restart for it (F10 > Restart the game now).
- **HUD position (21:9, 32:9, 48:9)**: On 21:9, 32:9 or 48:9: the HUD at the screen's edges, or in the middle 16:9 (easier to see on a very wide screen). Nothing changes at 16:9. Automatic (the default): the middle on 48:9, the edges otherwise.
- **Cutscenes (21:9, 32:9, 48:9)**: On 21:9, 32:9 or 48:9: real-time cutscenes wide like the rest of the game, or the original 16:9 with bars at the sides.
- **Field of view**: Widens or narrows the camera's view (1 = the game's). Wider shows more of the world and costs some GPU and CPU.
- **Texture cache**: More memory for textures: fewer blurry textures when you turn around, for about 490 MiB of video memory per step.
- **Grass density**: More grass, flowers and pebbles. Higher looks fuller and costs GPU and CPU.
- **Shadow resolution**: Sharper shadows: 2 doubles the shadow map's width and height, for 4x its memory and more GPU.
- **Sun shadow map size**: Fixed keeps the sun's shadow map at its largest size: steadier, sharper leaf shadows for some GPU. The game's changes its size with the view.
- **Near shadow reach**: Moves the lines where the sun's sharp near shadows hand over to coarser ones further away (1 = the game's, 2 = twice as far). Near shadows get a little softer: raise Shadow resolution with it. Costs a little GPU and CPU. With Shadow distance above 1 the two multiplied stay at 4 at most, so at Shadow distance 4 this does nothing.
- **Shadow distance**: How far the sharp shadows reach. Higher looks better in the distance and costs GPU.
- **Fog distance**: Pushes the distance fog back: clearer far mountains. Higher shows more and costs some GPU.
- **Detail distance**: How far models keep their full detail. Higher looks better in the distance and costs GPU and CPU.
- **Actor detail distance**: How far characters, enemies and objects keep full animation. Higher is smoother in the distance and costs CPU.
- **Terrain detail distance**: How far the ground and water keep their full detail. Higher looks better and costs CPU and GPU.
- **Simulation distance**: How far from the camera characters, enemies and physics keep being simulated (1 = the game's). Higher keeps distant things alive and costs CPU.
- **Fast-travel speed**: Fast: Link's departure when travelling is about 2.5 s shorter. Normal is the game's own.

## Audio

- **Volume**: The game's overall volume, on top of the Windows volume.
- **Music**: The music's volume.
- **Effects**: The sound effects' volume.
- **Voice**: The voices' volume. No part of the game's mix carries voices alone yet, so this does nothing for now.
- **Speakers**: The speakers the game mixes for: stereo, or 5.1 / 7.1 surround (experimental). Pick what your speakers or headphones use.
- **Latency**: How much sound is queued ahead. Lower responds sooner; raise it if the sound crackles.
- **Follow the Windows device**: The sound moves to headphones or speakers as you plug them in or out, without a restart.
- **Mute in the background**: No sound while the game window is not in front; the game keeps running.

## Controls

- **Control scheme**: PC: keyboard and mouse controls in the menus (Q / E tabs, Esc back, the mouse pointer) and PC keys in the prompts. Console: the Switch layout on the keyboard everywhere.
- **Gameplay keys**: PC (default): one key per action (Space jumps, Shift sprints, F attacks...). Console layout: the old keys in the world. Menus use the PC keys either way.
- **Keyboard layout**: The keys of the Console scheme: Classic, PC (WASD), or Custom (the key bindings below).
- **Button prompts**: The button pictures in prompts and menus. Automatic follows the device you used last: keyboard and mouse, Xbox, PlayStation or Nintendo.
- **Menu buttons**: The pad's face buttons in menus and dialogues: as the game, printed A accepts and B goes back, or the Switch layout (right accepts). The world keeps its layout.
- **Mouse cursor in menus**: In menus a mouse pointer shows and selects what it points at; left click is A, Esc goes back.
- **Menu keys**: In the pause menu and the Purah Pad: Esc goes back, PageUp / PageDown or the wheel switch tabs, left click selects and right click goes back.
- **Mouse camera**: How fast the mouse turns the camera (click the window to capture the pointer; Esc, F10 or Alt+Tab frees it). 0 = off; above 1 turns faster.
- **Mouse sensitivity**: How fast the mouse turns the camera and aims. Higher turns faster.
- **Aiming sensitivity (bow)**: The mouse's speed while the bow is drawn, times the usual one (1 = the same). Lower aims more finely.
- **Mouse speed for a full camera turn**: How much mouse movement a second counts as the stick pushed all the way. Lower turns faster.
- **Invert mouse vertical**: Moving the mouse up looks down, as in flight games.
- **Left mouse shoots while aiming**: The mouse buttons aim and shoot.
- **Mouse as motion aim**: The mouse stands in for a pad's motion aim: while aiming, always, or off.
- **Motion aim speed**: How far motion aim turns for a mouse movement. Higher turns further.
- **Mouse wheel scrolls the quick menus**: While a D-pad quick menu is open (weapons, shields, items, bows), each wheel notch moves one entry.
- **Map with the mouse**: On the map the mouse pans, the wheel zooms, a left click places a pin and a right click goes back.
- **Map cursor sensitivity**: How far the map moves under the reticle for the mouse's motion: 1 = like the desktop pointer, 2 = twice as far.
- **Walk speed**: With the walk key held (Alt in the PC layout), how far the movement keys push the stick: lower walks slower; 1 is as without the key.
- **Minimum movement key press**: How long a quick tap of W, A, S or D keeps Link moving, so he takes a step instead of sliding. 0 = off.
- **Sprint toggle**: One press of the sprint key starts sprinting and the next stops it. Off: hold it, as on the Switch.
- **Shield and target toggle**: One press of the shield / target key raises the shield and targets, the next lowers it. Off: hold it, as on the Switch.
- **Ultrahand rotation speed**: How fast moving the mouse sideways turns an object while R is held in Ultrahand or Autobuild (1x: as far as the camera turns). The wheel turns it vertically; moving the mouse up or down does not turn it.
- **Face buttons**: A non-Nintendo pad's face buttons: by position (the Switch layout: the right button confirms) or by label (Xbox A confirms).
- **Rumble strength**: Controller vibration: off, normal, or stronger (x2 to x4).
- **Rumble curve**: How light rumbles feel next to strong ones: below 1 light ones come through stronger, above 1 only strong ones do. 1 = the game's own.
- **Trigger rumble (Xbox)**: Xbox One / Series pads: the trigger motors buzz with the game's rumble. Off by default (the Switch pad has none).
- **Camera speed (right stick)**: How fast the right stick turns the camera: 1 = the game's own, 2 = twice as fast. The mouse keeps its own sensitivity.
- **Motion (gyro)**: A pad's motion controls (aiming, some puzzles). Off ignores the gyro.
- **Motion sensitivity**: How far motion aim turns for a turn of the pad: 1 = the pad's own, 2 = twice as far.
- **Left stick dead zone**: How far the left stick must move before it counts. Raise it if Link moves with the stick at rest; lower it for finer steps. 0 = the pad's own.
- **Right stick dead zone**: How far the right stick must move before it counts. Raise it if the camera turns with the stick at rest; lower it for finer aiming. 0 = the pad's own.

## Game

- **Language**: The game's texts and subtitles, and these settings' own (English, Spanish, Russian, Brazilian Portuguese, Simplified Chinese, Traditional Chinese). Português (Brasil): 1.4.3's texts; with 1.0.0 they need the 1.4.3 update given to Setup (English otherwise), voices stay English. Automatic: Windows' language if the game has it, else English.
- **Skip the opening**: A new game skips the opening and starts on the Great Sky Island. F2 or Minus on the title screen also switch it. Game version 1.0.0 only (on 1.4.x the opening plays and the row is hidden).
- **Console mode**: Docked (default) or handheld: handheld renders at the Switch's lower handheld resolution (faster, blurrier).
- **Pause when the window loses focus**: Pauses the game, picture and sound, while its window is not in front.
- **Temperature unit**: The temperature on the Purah Pad's map: °C or °F. Automatic follows the game's language.
- **Save backups kept**: How many older copies of your saves are kept, in case a save goes wrong.
- **F12 screenshots**: F12 saves a screenshot as a PNG in the Screenshots folder.
- **Crash reports**: After a crash, a report and a minidump are written to attach to a bug report. Nothing is sent anywhere.
- **Automatic updates**: Checks GitHub for a newer version when the game starts (at most once a day); asks first if the update rebuilds the game.
- **Safe mode (if the game crashes)**: For a game that crashes or hangs: the renderer without its faster paths, DLSS, FSR and frame generation, and without the driver paths some GPUs crash in. Slower. At the next start.
- **Discord status**: Shows "Playing", the time played and a "Join the community" button (the game's subreddit) on your Discord profile while Discord runs. Nothing else is sent.

Rows not listed here (a few newer controller rows, the advanced ones) show their own help in F10.
