// pc_help.cpp: one line of help per settings row, for the player. What the row does, which way is better looking or
// faster, and what it costs; F10 shows it at the bottom for the row in focus (pc_overlay.cpp), in the game's
// language (pc_tr: the Spanish in pc_text_es.inc). "Applies at the next start" is not written here: F10 adds it
// from the row's PC_SET_RESTART flag, so it follows a row made live (live-settings). A row with no line here shows
// its own help (the key bindings' and pad buttons' rows: theirs is written for the player already);
// tools/engine/test_pc_help.py checks every shown row has one or the other.
#include "pc_settings.h"

#include <cstring>

#include "pc_text.h"

namespace {
struct Help {
    const char *key, *text;
};
const Help k_help[] = {
    // Graphics > Display
    {"graphics.preset", PC_T("Sets every quality row below at once. Ultra is the game at its best on an 8 GB card; Low and "
                             "Medium draw less than the Switch does. Changing a row by hand makes it Custom.")},
    {"display.window_mode", PC_T("Windowed, borderless fullscreen (the default: Alt+Tab is instant) or exclusive fullscreen "
                                 "(the monitor switches to the resolution and refresh rate below). F11 or Alt+Enter also "
                                 "switch it.")},
    {"display.window_size", PC_T("The window's size in windowed mode. Fullscreen uses the monitor's resolution.")},
    {"graphics.gpu", PC_T("The graphics card the game runs on, for a laptop with two. Automatic picks the dedicated one, "
                          "the faster; the integrated one uses less power but is much slower.")},
    {"display.monitor", PC_T("The monitor fullscreen uses and a windowed start opens on. Remember: the one the game was on "
                             "last.")},
    {"display.exclusive_size", PC_T("The resolution the monitor switches to in exclusive fullscreen. Borderless and windowed "
                                    "keep the desktop's.")},
    {"display.exclusive_refresh", PC_T("The refresh rate in exclusive fullscreen. Higher is smoother when the frame rate keeps "
                                       "up.")},
    {"display.fps_cap", PC_T("The most frames per second the game shows. Higher is smoother and costs more GPU and CPU; 30 is "
                             "the Switch's. Unlocked follows the monitor's refresh rate (240 at most); steady holds a rate "
                             "the scene can keep. For an even picture pick a cap that divides the refresh rate (30 or 60 "
                             "on 60 Hz, 40 on 120 Hz), or turn VRR on. Steam Deck: 60 (OLED: 90) at first; if it is not "
                             "held, lower it to 40 with the screen at 40 Hz (Quick Access > Performance).")},
    {"display.steady_floor", PC_T("Frame rate \"Unlocked, steady\" only: the lowest rate it holds. 30 (default) suits most "
                                  "PCs. On one that cannot hold 30, 20 or 15 show every frame for the same time (3 or 4 "
                                  "refreshes on 60 Hz) instead of an uneven 15-20.")},
    {"display.vsync", PC_T("On (default): no tearing. Off: slightly less input latency, with tearing lines.")},
    {"display.vrr", PC_T("Variable refresh rate (G-SYNC, FreeSync): the monitor waits for each frame, smooth below its "
                         "refresh rate and without tearing. Needs a VRR monitor with VRR on in Windows or the driver.")},
    {"display.perf_overlay", PC_T("Shows fps, the 1% low and a frame-time graph over the game (F9), to check performance.")},
    {"display.fps_readout", PC_T("A small frame-rate counter in a corner (F3 shows or hides it). Minimal: fps; Detailed adds "
                                 "the frame time, the 1% low and the worst frame. Off costs nothing.")},
    {"display.fps_position", PC_T("The counter's corner. Top right keeps clear of the hearts and the map.")},
    {"display.fps_size", PC_T("The counter's size. It also grows with the window's height.")},
    // Graphics > Colour
    {"display.color", PC_T("Colour adjustment of the game's picture (not these menus). Off shows the game's own colours and "
                           "costs nothing; Vivid is livelier; Brighter dark tones lightens caves, the Depths and nights; "
                           "Custom uses the four rows below.")},
    {"display.color_brightness", PC_T("Custom only: makes every colour brighter or darker; 0 is the game's.")},
    {"display.color_contrast", PC_T("Custom only: more or less difference between light and dark; 1 is the game's.")},
    {"display.color_saturation", PC_T("Custom only: how colourful the picture is; 1 is the game's, 0.5 greyer, 2 twice as "
                                      "colourful.")},
    {"display.color_gamma", PC_T("Custom only: below 1 brightens the dark tones, above 1 darkens them; 1 is the game's.")},
    // Graphics > Rendering
    {"graphics.render_scale", PC_T("The resolution the 3D scene is drawn at, against the game's own. Higher is sharper and "
                                   "costs much more GPU; lower is faster and blurrier. 100 % is native.")},
    {"graphics.dynamic_resolution", PC_T("Lowers the 3D resolution only when the GPU falls behind, never below the floor "
                                         "chosen, to hold the frame rate. Off keeps the picture sharp all the time; On is "
                                         "smoother on a slower GPU.")},
    {"graphics.native_ui", PC_T("Above 1080p, menus, text and the HUD are drawn at the window's resolution instead of the "
                                "game's 1080p stretched. Sharper text for a little GPU. Off by default for now: at some "
                                "resolutions it can show broken text (being fixed).")},
    {"graphics.dlss", PC_T("Upscales the 3D scene with a temporal upscaler that builds detail over several frames. DLSS: "
                           "NVIDIA RTX only; DLAA: DLSS at full resolution, the best image for more GPU. FSR: any GPU. All "
                           "cost some GPU; Off is the game's own.")},
    {"graphics.frame_gen", PC_T("Generated frames between the real ones make motion look smoother, but the game itself does "
                                "not run faster and input latency rises a little. DLSS x2: RTX 40 and newer, x3 and x4: RTX "
                                "50; FSR x2: any GPU. Best when the game already runs well.")},
    {"graphics.frame_gen_pace", PC_T("Smooth keeps the frames shown in step with the monitor's refresh. Maximum fps adds the "
                                     "generated frames to every real frame the PC makes, even past the refresh rate: more "
                                     "frames than the screen shows (the fps readout counts them all), for VRR panels or low "
                                     "latency; Smooth looks steadier on a fixed refresh. With V-sync off the picture can "
                                     "tear.")},
    {"graphics.upscaler_3d", PC_T("How the 3D scene is scaled up to the output: Bicubic (the port's) or FSR 1 (the game's "
                                  "own). Matters only below 100 % render resolution; either costs little.")},
    {"graphics.scene_upscaler", PC_T("The filter that scales the 3D scene when the render resolution is not 100 %: "
                                     "Catmull-Rom, or FSR 1 EASU (sharper edges). Either costs little.")},
    {"graphics.window_upscaler", PC_T("How the finished picture is scaled to the window: Bilinear, or FSR 1 for a window "
                                      "above 1080p (sharper). Costs little.")},
    {"graphics.antialiasing", PC_T("Smooths jagged edges on the final picture. SMAA 1x: smoother edges for a little GPU; Off "
                                   "is the game's own look.")},
    {"graphics.sharpening", PC_T("Sharpens the final picture (AMD RCAS). More is crisper but can look grainy; Off is the "
                                 "game's look. Costs very little.")},
    {"graphics.anisotropy", PC_T("Keeps ground textures sharp when seen at a shallow angle. Higher is sharper for a little "
                                 "GPU; 16x suits most GPUs. The game's: its own setting.")},
    {"graphics.lod_bias", PC_T("Which texture detail level is used; 0 is the game's. Below 0 is sharper but can shimmer; "
                               "above 0 is blurrier and can break textures (red grass). Not a quality slider: leave it at 0.")},
    {"graphics.compact_textures", PC_T("For graphics cards with 4 GB of video memory or less: the textures take a "
                                       "quarter of the memory, so the game no longer runs out of it (stutters, blurry "
                                       "or late textures). Fine colour detail gets a little softer. Automatic turns it "
                                       "on only on such cards.")},
    {"graphics.shadow_scale", PC_T("The resolution of the renderer's shadow maps: 1x is the Switch's, 2x (default) sharper, "
                                   "3x and 4x sharper still for more GPU time and memory.")},
    {"graphics.soft_shadows", PC_T("Sun shadows grow softer the farther they fall from what casts them, as in real light. Off "
                                   "is the game's look; on costs some GPU. A change rebuilds the shadow shaders in the "
                                   "background: the old shadows show until then.")},
    {"graphics.contact_shadows", PC_T("Adds the small sun shadows the shadow maps miss: at feet, under stones, in grass. Off is "
                                      "the game's look; Subtle to Strong cost some GPU.")},
    {"graphics.shader_seed", PC_T("Prepares the shaders shipped with this version before you meet them, so new places "
                                  "stutter less the first time. Off: only the shaders this PC has built itself. On is "
                                  "better.")},
    {"graphics.shader_wait", PC_T("A shader still being built: Limit to 0.1 s (default) shows the frame after 0.1 s and the "
                                  "missing objects appear a moment later; Always wait never shows anything incomplete but "
                                  "can freeze the picture for a second or more.")},
    // Graphics > Presentation, HDR
    {"display.pace_at", PC_T("Where the frame cap waits: at the present (default, steadier frame times) or before the frame "
                             "(slightly less input latency).")},
    {"display.vsync_source", PC_T("What the game's 60 Hz tick follows: a 60 Hz clock (default, right for any monitor) or the "
                                  "monitor's vblank (a fixed-refresh monitor only). Leave the default.")},
    {"display.pace_spin", PC_T("The last part of the frame cap's wait busy-waits for exact timing: steadier frame times for "
                               "some CPU. Off is the default.")},
    {"display.max_queued", PC_T("How many finished frames may wait to be shown. 1 lowers input latency, 3 evens out uneven "
                                "frames; 2 is the default.")},
    {"display.frames_in_flight", PC_T("How many frames the CPU may prepare ahead of the GPU. 2 (default) is the balance; 1 "
                                      "lowers latency but is slower when both are busy; 3 adds latency.")},
    {"display.low_latency", PC_T("Holds each frame back just before it reads your input, so it waits less afterwards: lower "
                                 "input latency. NVIDIA uses Reflex; Boost also keeps the GPU clocks up (more power).")},
    {"display.hdr", PC_T("HDR output for an HDR monitor (HDR10 or scRGB). Needs HDR on for that monitor in Windows; "
                         "otherwise the picture stays SDR.")},
    {"display.hdr_peak", PC_T("Your monitor's peak brightness in nits, so highlights are neither clipped nor dim. Use the "
                              "value the monitor states.")},
    {"display.hdr_paper", PC_T("How bright ordinary white (menus, the sky) is in HDR. Raise it if the picture looks dim, "
                               "lower it if it is too bright.")},
    // Graphics > Game patches
    {"graphics.aspect_ratio", PC_T("The window's (the default): on a 16:10, 21:9, 32:9 or 48:9 window or monitor the "
                                   "picture takes that shape, 21:9, 32:9 and 48:9 with a wider view; other shapes, 16:9 "
                                   "among them, keep the game's own. 16:9 / 16:10 / 21:9 / 32:9 / 48:9: that shape "
                                   "whatever the window. A new shape's layouts are made at its first start (about "
                                   "30 s, once). A wider picture costs more GPU.")},
    {"graphics.hud_layout", PC_T("On 21:9, 32:9 or 48:9: the HUD at the screen's edges, or in the middle 16:9 (easier to see on a "
                                 "very wide screen). Nothing changes at 16:9. Automatic (the default): the middle on 48:9, the edges otherwise.")},
    {"graphics.cutscene_aspect", PC_T("On 21:9, 32:9 or 48:9: real-time cutscenes wide like the rest of the game, or the original "
                                      "16:9 with bars at the sides.")},
    {"graphics.fov", PC_T("Widens or narrows the camera's view (1 = the game's). Wider shows more of the world and costs some "
                          "GPU and CPU.")},
    {"graphics.texture_cache", PC_T("More memory for textures: fewer blurry textures when you turn around, for about 490 MiB "
                                    "of video memory per step.")},
    {"graphics.grass_density", PC_T("More grass, flowers and pebbles. Higher looks fuller and costs GPU and CPU.")},
    {"graphics.shadow_resolution", PC_T("Sharper shadows: 2 doubles the shadow map's width and height, for 4x its memory and "
                                        "more GPU.")},
    {"graphics.shadow_fixed", PC_T("Fixed keeps the sun's shadow map at its largest size: steadier, sharper leaf shadows for "
                                   "some GPU. The game's changes its size with the view.")},
    {"graphics.shadow_reach", PC_T("Moves the lines where the sun's sharp near shadows hand over to coarser ones further away "
                                   "(1 = the game's, 2 = twice as far). Near shadows get a little softer: raise Shadow "
                                   "resolution with it. Costs a little GPU and CPU.")},
    {"graphics.shadow_distance", PC_T("How far the sharp shadows reach. Higher looks better in the distance and costs GPU.")},
    {"graphics.fog_distance", PC_T("Pushes the distance fog back: clearer far mountains. Higher shows more and costs some "
                                   "GPU.")},
    {"graphics.lod_distance", PC_T("How far models keep their full detail. Higher looks better in the distance and costs GPU "
                                   "and CPU.")},
    {"graphics.actor_detail_distance", PC_T("How far characters, enemies and objects keep full animation. Higher is smoother "
                                            "in the distance and costs CPU.")},
    {"graphics.terrain_detail_distance", PC_T("How far the ground and water keep their full detail. Higher looks better and "
                                              "costs CPU and GPU.")},
    {"game.simulation_distance", PC_T("How far from the camera characters, enemies and physics keep being simulated (1 = the "
                                      "game's). Higher keeps distant things alive and costs CPU.")},
    {"game.fast_travel", PC_T("Fast: Link's departure when travelling is about 2.5 s shorter. Normal is the game's own.")},
    // Audio
    {"audio.volume", PC_T("The game's overall volume, on top of the Windows volume.")},
    {"audio.music", PC_T("The music's volume.")},
    {"audio.effects", PC_T("The sound effects' volume.")},
    {"audio.voice", PC_T("The voices' volume. No part of the game's mix carries voices alone yet, so this does nothing for "
                         "now.")},
    {"audio.channels", PC_T("The speakers the game mixes for: stereo, or 5.1 / 7.1 surround (experimental). Pick what your "
                            "speakers or headphones use.")},
    {"audio.latency", PC_T("How much sound is queued ahead. Lower responds sooner; raise it if the sound crackles.")},
    {"audio.follow_device", PC_T("The sound moves to headphones or speakers as you plug them in or out, without a "
                                 "restart.")},
    {"audio.mute_unfocused", PC_T("No sound while the game window is not in front; the game keeps running.")},
    // Controls
    {"controls.scheme", PC_T("PC: keyboard and mouse controls in the menus (Q / E tabs, Esc back, the mouse pointer) and PC "
                             "keys in the prompts. Console: the Switch layout on the keyboard everywhere.")},
    {"controls.gameplay", PC_T("PC (default): one key per action (Space jumps, Shift sprints, F attacks...). Console layout: "
                               "the old keys in the world. Menus use the PC keys either way.")},
    {"controls.keyboard_layout", PC_T("The keys of the Console scheme: Classic, PC (WASD), or Custom (the key bindings "
                                      "below).")},
    {"controls.glyphs", PC_T("The button pictures in prompts and menus. Automatic follows the device you used last: keyboard "
                             "and mouse, Xbox, PlayStation or Nintendo.")},
    {"controls.menu_cursor", PC_T("In menus a mouse pointer shows and selects what it points at; left click is A, Esc goes "
                                  "back.")},
    {"controls.menu_keys", PC_T("In the pause menu and the Purah Pad: Esc goes back, PageUp / PageDown or the wheel switch "
                                "tabs, left click selects and right click goes back.")},
    {"controls.mouse_camera_scale", PC_T("How fast the mouse turns the camera (click the window to capture the pointer; Esc, "
                                         "F10 or Alt+Tab frees it). 0 = off; above 1 turns faster.")},
    {"controls.mouse_sensitivity", PC_T("How fast the mouse turns the camera and aims. Higher turns faster.")},
    {"controls.mouse_aim_scale", PC_T("The mouse's speed while the bow is drawn, times the usual one (1 = the same). Lower "
                                      "aims more finely.")},
    {"controls.mouse_stick_speed", PC_T("How much mouse movement a second counts as the stick pushed all the way. Lower "
                                        "turns faster.")},
    {"controls.mouse_invert_y", PC_T("Moving the mouse up looks down, as in flight games.")},
    {"controls.mouse_aim", PC_T("The mouse buttons aim and shoot.")},
    {"controls.mouse_gyro", PC_T("The mouse stands in for a pad's motion aim: while aiming, always, or off.")},
    {"controls.mouse_gyro_speed", PC_T("How far motion aim turns for a mouse movement. Higher turns further.")},
    {"controls.mouse_wheel", PC_T("While a D-pad quick menu is open (weapons, shields, items, bows), each wheel notch moves "
                                  "one entry.")},
    {"controls.mouse_map", PC_T("On the map the mouse pans, the wheel zooms, a left click places a pin and a right click goes "
                                "back.")},
    {"controls.mouse_map_speed", PC_T("How far the map moves under the reticle for the mouse's motion: 1 = like the desktop "
                                      "pointer, 2 = twice as far.")},
    {"controls.walk_speed", PC_T("With the walk key held (Alt in the PC layout), how far the movement keys push the stick: "
                                 "lower walks slower; 1 is as without the key.")},
    {"controls.key_min_hold_ms", PC_T("How long a quick tap of W, A, S or D keeps Link moving, so he takes a step instead of "
                                      "sliding. 0 = off.")},
    {"controls.sprint_toggle", PC_T("One press of the sprint key starts sprinting and the next stops it. Off: hold it, as on "
                                    "the Switch.")},
    {"controls.lock_toggle", PC_T("One press of the shield / target key raises the shield and targets, the next lowers it. "
                                  "Off: hold it, as on the Switch.")},
    {"controls.ability.rotate_step", PC_T("How much mouse movement turns an object one 45° step while R is held in Ultrahand "
                                          "or Autobuild. Lower turns sooner.")},
    {"controls.pad_labels", PC_T("A non-Nintendo pad's face buttons: by position (the Switch layout: the right button "
                                 "confirms) or by label (Xbox A confirms).")},
    {"controls.rumble", PC_T("Controller vibration: off, normal, or stronger (x2 to x4).")},
    {"controls.rumble_curve", PC_T("How light rumbles feel next to strong ones: below 1 light ones come through stronger, "
                                   "above 1 only strong ones do. 1 = the game's own.")},
    {"controls.rumble_triggers", PC_T("Xbox One / Series pads: the trigger motors buzz with the game's rumble. Off by "
                                      "default (the Switch pad has none).")},
    {"controls.stick_camera_speed", PC_T("How fast the right stick turns the camera: 1 = the game's own, 2 = twice as "
                                         "fast. The mouse keeps its own sensitivity.")},
    {"controls.gyro", PC_T("A pad's motion controls (aiming, some puzzles). Off ignores the gyro.")},
    {"controls.gyro_scale", PC_T("How far motion aim turns for a turn of the pad: 1 = the pad's own, 2 = twice as far.")},
    {"controls.deadzone_left", PC_T("How far the left stick must move before it counts. Raise it if Link moves with the stick "
                                    "at rest; lower it for finer steps. 0 = the pad's own.")},
    {"controls.deadzone_right", PC_T("How far the right stick must move before it counts. Raise it if the camera turns with "
                                     "the stick at rest; lower it for finer aiming. 0 = the pad's own.")},
    // Game
    {"game.language", PC_T("The game's texts and subtitles, and these settings' own (English, Spanish, Russian, Brazilian "
                           "Portuguese, Simplified Chinese). Português (Brasil): 1.4.3's texts; with 1.0.0 they need the "
                           "1.4.3 update given to Setup (English otherwise), voices stay English. Automatic: Windows' "
                           "language if the game has it, else English.")},
    {"game.skip_opening", PC_T("A new game skips the opening and starts on the Great Sky Island. F2 or Minus on the title "
                               "screen also switch it.")},
    {"game.console_mode", PC_T("Docked (default) or handheld: handheld renders at the Switch's lower handheld resolution "
                               "(faster, blurrier).")},
    {"game.pause_unfocused", PC_T("Pauses the game, picture and sound, while its window is not in front.")},
    {"game.temperature_unit", PC_T("The temperature on the Purah Pad's map: °C or °F. Automatic follows the game's "
                                   "language.")},
    {"game.save_backups", PC_T("How many older copies of your saves are kept, in case a save goes wrong.")},
    {"game.screenshots", PC_T("F12 saves a screenshot as a PNG in the Screenshots folder.")},
    {"game.crash_reports", PC_T("After a crash, a report and a minidump are written to attach to a bug report. Nothing is "
                                "sent anywhere.")},
    {"launcher.auto_update", PC_T("Checks GitHub for a newer version when the game starts (at most once a day); asks first if "
                                  "the update rebuilds the game.")},
    {"graphics.safe_mode", PC_T("For a game that crashes or hangs: the renderer without its faster paths, DLSS, FSR and "
                                "frame generation, and without the driver paths some GPUs crash in. Slower. At the next "
                                "start.")},
    {"game.discord", PC_T("Shows \"Playing\" and the time played on your Discord profile while Discord runs. Nothing else "
                          "is sent.")},
};
}  // namespace

const char *pc_setting_help(const char *key) {
    for (const Help &h : k_help)
        if (!strcmp(h.key, key)) return h.text;
    return nullptr;
}
