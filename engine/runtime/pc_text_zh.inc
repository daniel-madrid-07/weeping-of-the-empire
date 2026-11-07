// pc_text_zh.inc: the Simplified Chinese of the PC port's own texts (pc_text.cpp, pc_ui_lang "zh-Hans"), one
// PC_TR(english, translation) each. A text missing here shows in English; checked by tools/engine/check_texts.py
// (the same printf specifiers, no entry the sources lack). pc-ui 11-02.
PC_TR("Audio",
      "音频")
PC_TR("Output",
      "输出")
PC_TR("Speakers",
      "扬声器")
PC_TR("TOTK_AUDIO_CHANNELS. 5.1 and 7.1 tell the game it has surround output (7.1 puts the rears on the back and side pairs); Windows remaps to the device. Experimental: with 6 channels the game's effect on one sub mix diverges and is muted.",
      "TOTK_AUDIO_CHANNELS。5.1 和 7.1 会告知游戏当前为环绕声输出（7.1 将后置声道分为后方和侧方两对）；由 Windows 重新映射到设备。实验性：6 声道时，游戏中某个子混音的效果会失真，因此被静音。")
PC_TR("Stereo (default)",
      "立体声（默认）")
PC_TR("Match the Windows device (experimental)",
      "匹配 Windows 设备（实验性）")
PC_TR("5.1 (experimental)",
      "5.1（实验性）")
PC_TR("7.1 (experimental)",
      "7.1（实验性）")
PC_TR("Latency",
      "延迟")
PC_TR("TOTK_AUDIO_LATENCY_MS: audio queued ahead of the device. Lower is more responsive; raise it if the sound crackles.",
      "TOTK_AUDIO_LATENCY_MS：提前排入设备的音频量。越低响应越快；声音有爆音时请调高。")
PC_TR("20 ms (default)",
      "20 ms（默认）")
PC_TR("80 ms (wireless headsets)",
      "80 ms（无线耳机）")
PC_TR("Follow the Windows device",
      "跟随 Windows 设备")
PC_TR("TOTK_AUDIO_FOLLOW: switch to the new default device (headphones plugged in or out) and recover a lost one without a restart.",
      "TOTK_AUDIO_FOLLOW：切换到新的默认设备（插拔耳机时），并在无需重启的情况下恢复丢失的设备。")
PC_TR("On (default)",
      "开启（默认）")
PC_TR("Off",
      "关闭")
PC_TR("Mix",
      "混音")
PC_TR("Music",
      "音乐")
PC_TR("TOTK_AUDIO_MUSIC.",
      "TOTK_AUDIO_MUSIC。")
PC_TR("Effects",
      "音效")
PC_TR("TOTK_AUDIO_SFX.",
      "TOTK_AUDIO_SFX。")
PC_TR("Voice",
      "语音")
PC_TR("TOTK_AUDIO_VOICE. No sub mix carries voice alone yet, so by default this does nothing.",
      "TOTK_AUDIO_VOICE。目前没有单独承载语音的子混音，因此默认不起作用。")
PC_TR("the stick now",
      "摇杆当前")
PC_TR("Controls",
      "操控")
PC_TR("Keyboard layout",
      "键盘布局")
PC_TR("For the console scheme, and with the PC scheme for the world while Gameplay keys is Console (menus use the PC keys). Classic: Space jump, Shift sprint, E interact, F attack, WASD, arrows camera. PC: E interact, Shift sprint, Space jump, left mouse attack, right mouse bow, Q target, F abilities. Custom: the PC keys changed by the bindings below.",
      "用于主机方案；游戏按键为主机布局时，世界中也可配合 PC 方案使用（菜单使用 PC 按键）。经典：空格跳跃、Shift 冲刺、E 互动、F 攻击、WASD 移动、方向键控制视角。PC：E 互动、Shift 冲刺、空格跳跃、鼠标左键攻击、鼠标右键拉弓、Q 注视、F 能力。自定义：经下方按键绑定修改的 PC 按键。")
PC_TR("Classic (the engine's keys)",
      "经典（引擎按键）")
PC_TR("PC (WASD, mouse)",
      "PC（WASD、鼠标）")
PC_TR("Custom (the bindings below)",
      "自定义（下方按键绑定）")
PC_TR("Minimum movement key press",
      "移动键最短按下时间")
PC_TR("Milliseconds a quick tap of a movement key (WASD) keeps Link moving, so he takes a step instead of sliding with his legs still. 0 = off (a tap as short as the key press).",
      "轻点移动键（WASD）后，林克持续移动的毫秒数，使其迈出一步而不是腿不动地滑行。0 = 关闭（与按键时长一致）。")
PC_TR("Sprint toggle",
      "冲刺切换")
PC_TR("A press of the sprint key starts sprinting and the next one stops it; it also stops when Link stops moving or a menu opens. Off: hold it, as on the Switch.",
      "按一下冲刺键开始冲刺，再按一下停止；林克停止移动或打开菜单时也会停止。关闭：按住冲刺，与 Switch 相同。")
PC_TR("On",
      "开启")
PC_TR("Shield and target toggle",
      "盾牌与注视切换")
PC_TR("A press of the shield / target key raises the shield and targets, the next press lowers it; a menu lets it go too. Off: hold it, as on the Switch.",
      "按一下盾牌 / 注视键举盾并注视目标，再按一下放下；打开菜单时也会松开。关闭：按住，与 Switch 相同。")
PC_TR("Mouse camera",
      "鼠标视角")
PC_TR("The mouse turns the camera (click the window to capture the pointer; Esc, F10 or Alt+Tab frees it). 0 = off; above 1 turns faster.",
      "鼠标控制视角（点击窗口以捕获指针；Esc、F10 或 Alt+Tab 可释放）。0 = 关闭；大于 1 转得更快。")
PC_TR("Pad",
      "手柄")
PC_TR("Button prompts",
      "按键提示")
PC_TR("The button pictures in every prompt and menu, for the keys and buttons bound. Automatic follows the device used last: keyboard and mouse, an Xbox (XInput) pad, a DualSense / DualShock 4, or the game's own with a Nintendo pad. A change shows at once, in every prompt and label; with a pad in use, menus hide the mouse pointer until the mouse moves.",
      "所有提示和菜单中的按键图标，对应已绑定的按键和按钮。自动：跟随最近使用的设备，即键盘和鼠标、Xbox（XInput）手柄、DualSense / DualShock 4，或使用 Nintendo 手柄时的游戏自带图标。更改立即在所有提示和标签中生效；使用手柄时，菜单会隐藏鼠标指针，直到鼠标移动。")
PC_TR("Automatic (the device used last)",
      "自动（最近使用的设备）")
PC_TR("Menu buttons",
      "菜单按键")
PC_TR("Same as the game",
      "与游戏相同")
PC_TR("Printed A accepts, B goes back",
      "印有 A 的键确认，B 返回")
PC_TR("Switch layout (right accepts, bottom goes back)",
      "Switch 布局（右键确认，下键返回）")
PC_TR("The pad's four face buttons in menus, dialogues and the title screen. Same as the game: as Face buttons and the Pad buttons rows give them. Printed A accepts, B goes back: the button printed A (Cross) confirms and B (Circle) goes back, whatever jump and interact use in the world. Switch layout: the right button confirms, the bottom one goes back. Nothing changes in the world. A change applies at once.",
      "菜单、对话和标题画面中手柄的四个面键。与游戏相同：按“面键”和“手柄按钮”各行的设置。印有 A 的键确认，B 返回：印有 A（叉）的键确认，印有 B（圈）的键返回，不论世界中跳跃和互动用哪个键。Switch 布局：右键确认，下键返回。世界中的操作不变。更改立即生效。")
PC_TR("Switch (the game's)",
      "Switch（游戏自带）")
PC_TR("Keyboard and mouse",
      "键盘和鼠标")
PC_TR("Left mouse shoots while aiming",
      "瞄准时鼠标左键射击")
PC_TR("With the bow drawn (a ZR key or button held), the left button shoots instead of attacking.",
      "拉弓时（按住 ZR 键或按钮），鼠标左键用于射击而非攻击。")
PC_TR("Mouse look version",
      "鼠标视角模式")
PC_TR("Stick",
      "摇杆")
PC_TR("Direct",
      "直接")
PC_TR("Mouse sensitivity",
      "鼠标灵敏度")
PC_TR("Scales the camera and the motion aim.",
      "缩放视角和体感瞄准的灵敏度。")
PC_TR("Mouse speed for a full camera turn",
      "满速转动视角的鼠标速度")
PC_TR("Mouse counts per second that turn the camera at full stick speed.",
      "每秒多少鼠标计数可使视角以摇杆满速转动。")
PC_TR("Invert mouse vertical",
      "反转鼠标垂直方向")
PC_TR("Mouse as motion aim",
      "鼠标作体感瞄准")
PC_TR("While aiming, the mouse turns a virtual controller, so the game's motion aiming gets the mouse's precision (when no pad with motion is connected).",
      "瞄准时，鼠标转动一个虚拟手柄，使游戏的体感瞄准获得鼠标的精度（未连接带体感的手柄时）。")
PC_TR("While aiming (bow, throw)",
      "瞄准时（弓、投掷）")
PC_TR("Always",
      "始终")
PC_TR("Motion aim speed",
      "体感瞄准速度")
PC_TR("Aiming sensitivity (bow)",
      "瞄准灵敏度（弓）")
PC_TR("With the bow drawn: the mouse's turn times this (1 = as when not aiming), with or without the mouse as motion aim. A pad's motion sensitivity is separate.",
      "拉弓时：鼠标转动量乘以此值（1 = 与未瞄准时相同），无论是否将鼠标用作体感瞄准。手柄的体感灵敏度另行设置。")
PC_TR("Mouse wheel scrolls the quick menus",
      "鼠标滚轮滚动快捷菜单")
PC_TR("While a D-pad quick menu is held open (weapons, shields, items, bows), each notch moves one entry.",
      "按住十字键打开快捷菜单时（武器、盾牌、道具、弓），滚轮每滚一格移动一项。")
PC_TR("Map with the mouse",
      "鼠标操作地图")
PC_TR("On the map the mouse moves the map under the reticle (it stays at the centre) as far as the pointer would move, the wheel zooms, a left click places a pin or stamp and a right click goes back.",
      "在地图上，鼠标在准星（保持在中央）下方移动地图，移动距离与鼠标指针相同，滚轮缩放，左键放置图钉或印章，右键返回。")
PC_TR("Map cursor sensitivity",
      "地图光标灵敏度")
PC_TR("How far the map moves for the mouse's motion: 1 = as far as the desktop pointer would move on the screen, 2 = twice as far.",
      "鼠标移动时地图移动的距离：1 = 与桌面指针在屏幕上移动的距离相同，2 = 两倍。")
PC_TR("How far the map moves under the reticle for the mouse's motion: 1 = like the desktop pointer, 2 = twice as far.",
      "鼠标移动时地图在准星下方移动的距离：1 = 与桌面指针相同，2 = 两倍。")
PC_TR("Menu keys",
      "菜单按键")
PC_TR("In the pause menu and the Purah Pad: Esc goes back, PageUp / PageDown (and the wheel in the pause menu) switch tabs, and in the pause menu clicks select (left) and go back (right).",
      "在暂停菜单和普尔亚平板中：Esc 返回，PageUp / PageDown（暂停菜单中也可用滚轮）切换标签页，暂停菜单中单击左键选择、右键返回。")
PC_TR("Rumble curve",
      "震动曲线")
PC_TR("How light rumbles feel next to strong ones: below 1 light rumbles come through stronger, above 1 only strong ones; 1 = the game's own.",
      "轻微震动相对强烈震动的感受：低于 1 时轻微震动更明显，高于 1 时只保留强烈震动；1 = 游戏原本的效果。")
PC_TR("Trigger rumble (Xbox)",
      "扳机震动（Xbox）")
PC_TR("An Xbox One or Series pad's trigger motors buzz with the game's rumble, each trigger with its side's. Off by default: the Switch pad has no trigger motors.",
      "Xbox One 或 Series 手柄的扳机马达随游戏震动而震动，每个扳机对应其所在一侧。默认关闭：Switch 手柄的扳机没有马达。")
PC_TR("Light",
      "轻")
PC_TR("Full strength",
      "最强")
PC_TR("Camera speed (right stick)",
      "镜头速度（右摇杆）")
PC_TR("The camera's turn with the right stick (and the camera keys) times this, at each of the game's own camera speeds, top speed included; the mouse keeps its own sensitivity. At the next start.",
      "用右摇杆（以及镜头按键）转动镜头的速度乘以此值，适用于游戏自带的每一档镜头速度，包括最高速度；鼠标保持其自身的灵敏度。下次启动时生效。")
PC_TR("Rumble strength",
      "震动强度")
PC_TR("The pad's motors (Xbox, PlayStation and Switch pads): 0 = off, 1 = the game's own.",
      "手柄的马达（Xbox、PlayStation 和 Switch 手柄）：0 = 关闭，1 = 游戏自带强度。")
PC_TR("Motion (gyro)",
      "体感（陀螺仪）")
PC_TR("The motion sensors of a DualSense, DualShock 4, Pro Controller or Joy-Con, for the game's motion aiming (the game's own Motion Controls option turns it on there). Off: the pad lies still for the game.",
      "DualSense、DualShock 4、Pro 手柄或 Joy-Con 的体感传感器，用于游戏的体感瞄准（游戏自带的“体感操作”选项会在其中开启）。关闭：对游戏而言手柄静止不动。")
PC_TR("Motion sensitivity",
      "体感灵敏度")
PC_TR("How far the aim turns for a turn of the pad: 1 = the pad's own, 2 = twice as far.",
      "瞄准随手柄转动而转动的幅度：1 = 手柄自带，2 = 两倍。")
PC_TR("Left stick dead zone",
      "左摇杆死区")
PC_TR("How far the left stick must move before it counts, in percent of its travel. Raise it if Link moves with the stick at rest (a worn stick); lower it for finer small steps. 0 = the pad's own (Xbox 24, PlayStation 15, Switch 12).",
      "左摇杆需要移动多远才会生效，以行程的百分比计。若摇杆静止时林克仍在移动（摇杆磨损），请调高；想要更精细的小步移动，请调低。0 = 手柄自带（Xbox 24，PlayStation 15，Switch 12）。")
PC_TR("Right stick dead zone",
      "右摇杆死区")
PC_TR("How far the right stick must move before it counts, in percent of its travel. Raise it if the camera turns with the stick at rest; lower it for finer aiming. 0 = the pad's own (Xbox 27, PlayStation 15, Switch 12).",
      "右摇杆需要移动多远才会生效，以行程的百分比计。若摇杆静止时视角仍在转动，请调高；想要更精细的瞄准，请调低。0 = 手柄自带（Xbox 27，PlayStation 15，Switch 12）。")
PC_TR("Walk speed",
      "步行速度")
PC_TR("With the walk key held (Alt in the PC layout): how far the movement keys push the stick, in its travel (1 = a full push, as without the key).",
      "按住步行键时（PC 布局中为 Alt）：移动键推动摇杆的幅度（1 = 推到底，与不按该键相同）。")
PC_TR("None",
      "无")
PC_TR("Left stick click",
      "左摇杆按下")
PC_TR("Right stick click",
      "右摇杆按下")
PC_TR("D-pad up",
      "十字键上")
PC_TR("D-pad down",
      "十字键下")
PC_TR("D-pad left",
      "十字键左")
PC_TR("D-pad right",
      "十字键右")
PC_TR("Back paddle P1 (right, top)",
      "背键 P1（右上）")
PC_TR("The Switch button this back paddle presses: an Xbox Elite's paddles (unless the Xbox Accessories app maps them to buttons), a DualSense Edge's back buttons (the right one).",
      "此背键按下的 Switch 按钮：Xbox Elite 的背键（除非 Xbox 配件应用将其映射为按钮），DualSense Edge 的背键（右侧那个）。")
PC_TR("Back paddle P2 (right, bottom)",
      "背键 P2（右下）")
PC_TR("The Switch button this back paddle presses: an Xbox Elite's paddles (unless the Xbox Accessories app maps them to buttons), a DualSense Edge's back buttons (none: P1 and P3 only).",
      "此背键按下的 Switch 按钮：Xbox Elite 的背键（除非 Xbox 配件应用将其映射为按钮），DualSense Edge 的背键（无：仅 P1 和 P3）。")
PC_TR("Back paddle P3 (left, top)",
      "背键 P3（左上）")
PC_TR("The Switch button this back paddle presses: an Xbox Elite's paddles (unless the Xbox Accessories app maps them to buttons), a DualSense Edge's back buttons (the left one).",
      "此背键按下的 Switch 按钮：Xbox Elite 的背键（除非 Xbox 配件应用将其映射为按钮），DualSense Edge 的背键（左侧那个）。")
PC_TR("Back paddle P4 (left, bottom)",
      "背键 P4（左下）")
PC_TR("Game",
      "游戏")
PC_TR("Window",
      "窗口")
PC_TR("Pause when the window loses focus",
      "失去焦点时暂停")
PC_TR("TOTK_PAUSE_UNFOCUSED: while the game window is not in front the game stops, picture, sound and its clock, and goes on where it was (hle_oe.cpp oe_host_paused).",
      "TOTK_PAUSE_UNFOCUSED：游戏窗口不在前台时，游戏会停止（画面、声音和计时），并从原处继续（hle_oe.cpp oe_host_paused）。")
PC_TR("Key bindings",
      "按键绑定")
PC_TR("Up to 4 keys or mouse buttons. Editing one switches the layout to Custom; empty = the PC layout's.",
      "最多 4 个按键或鼠标按钮。编辑任一项会将布局切换为自定义；空 = 使用 PC 布局的按键。")
PC_TR("A (interact)",
      "A（互动）")
PC_TR("B (sprint / back)",
      "B（冲刺 / 返回）")
PC_TR("X (jump)",
      "X（跳跃）")
PC_TR("Y (attack)",
      "Y（攻击）")
PC_TR("Left stick click (crouch)",
      "左摇杆按下（蹲下）")
PC_TR("Right stick click (look)",
      "右摇杆按下（视角）")
PC_TR("L (abilities)",
      "L（能力）")
PC_TR("R (throw)",
      "R（投掷）")
PC_TR("ZL (target / shield)",
      "ZL（注视 / 盾牌）")
PC_TR("ZR (bow)",
      "ZR（弓）")
PC_TR("+ (menu)",
      "＋键（菜单）")
PC_TR("- (map)",
      "－键（地图）")
PC_TR("D-pad left (shields)",
      "十字键左（盾牌）")
PC_TR("D-pad up (items)",
      "十字键上（道具）")
PC_TR("D-pad right (weapons)",
      "十字键右（武器）")
PC_TR("Move left",
      "向左移动")
PC_TR("Move forward",
      "向前移动")
PC_TR("Move right",
      "向右移动")
PC_TR("Move back",
      "向后移动")
PC_TR("Camera left",
      "视角向左")
PC_TR("Camera up",
      "视角向上")
PC_TR("Camera right",
      "视角向右")
PC_TR("Camera down",
      "视角向下")
PC_TR("Walk (hold)",
      "步行（按住）")
PC_TR("Pad buttons",
      "手柄按钮")
PC_TR("Up to 4 pad buttons, by position: A = the right face button (Xbox B, PlayStation Circle), B = bottom (Xbox A, Cross), X = top (Xbox Y, Triangle), Y = left (Xbox X, Square). Empty = its own button; None = unbound.",
      "最多 4 个手柄按钮，按位置计：A = 右侧面键（Xbox B，PlayStation 圆圈），B = 下方（Xbox A，叉），X = 上方（Xbox Y，三角），Y = 左侧（Xbox X，方块）。空 = 其自身按钮；无 = 未绑定。")
PC_TR("Skipping to Great Sky Island in %.1f s... (release Minus to cancel)",
      "%.1f s 后跳转到初始空岛……（松开－键取消）")
PC_TR("Skip to Great Sky Island: F2 or hold Minus (restarts into a new game)",
      "跳转到初始空岛：F2 或按住－键（重新开始新游戏）")
PC_TR("Skip opening: ON   (F2 / Minus to change)",
      "跳过开场：开启   （F2 / －键切换）")
PC_TR("Skip opening: OFF   (F2 / Minus to change)",
      "跳过开场：关闭   （F2 / －键切换）")
PC_TR("Graphics settings: F10  (pad: L3 + R3)",
      "图像设置：F10  （手柄：L3 + R3）")
PC_TR("Shader warm-up: %d / %d (%d %%) - press A to play now, it keeps compiling in the background",
      "着色器预热：%d / %d（%d %%），按 A 立即开始游戏，后台继续编译")
PC_TR("Preparing the shaders for this PC (first start): %d %%",
      "正在为此电脑准备着色器（首次启动）：%d %%")
PC_TR("Compiling shaders %d %%",
      "正在编译着色器 %d %%")
PC_TR("Loading shaders %d %%",
      "正在加载着色器 %d %%")
PC_TR("(paused: low memory)",
      "（已暂停：内存不足）")
PC_TR("Windows runs your monitor at %lu Hz: set it to %lu Hz in Settings > Display > Advanced display",
      "Windows 当前以 %lu Hz 运行显示器：请在“设置 > 显示 > 高级显示”中改为 %lu Hz")
PC_TR("It writes the NSO header.",
      "它会写入 NSO 头。")
PC_TR("It writes past main's image (bss).",
      "它写入了 main 镜像之外的区域（bss）。")
PC_TR("It changes code the port already patches (%s).",
      "它修改了本移植版已打补丁的代码（%s）。")
PC_TR("It overlaps %s, higher in the list.",
      "它与列表中靠前的 %s 重叠。")
PC_TR("It is cut short at word %s.",
      "它在字 %s 处被截断。")
PC_TR("It has an opcode the cheat engine does not know (%s).",
      "它含有金手指引擎不认识的操作码（%s）。")
PC_TR("It uses the alias or ASLR memory region, which the port does not have.",
      "它使用了本移植版没有的别名或 ASLR 内存区域。")
PC_TR("It changes the game's code only under a condition (a button, a value), which the port cannot do yet.",
      "它仅在特定条件（按钮、数值）下修改游戏代码，本移植版暂时无法做到。")
PC_TR("It cannot be read (%s).",
      "它无法读取（%s）。")
PC_TR("%d ExeFS patches in use",
      "已使用 %d 个 ExeFS 补丁")
PC_TR("%d code patches need the game built on this PC (Builder install), not used",
      "%d 个代码补丁需要在本机构建的游戏（Builder 安装），未使用")
PC_TR("the build with its code patches failed (see exefs/%s/build.log next to totk.exe), not used",
      "带代码补丁的构建失败（见 totk.exe 旁的 exefs/%s/build.log），未使用")
PC_TR("%d code patches: the game is rebuilt with them at this start",
      "%d 个代码补丁：本次启动时会带着它们重新构建游戏")
PC_TR("%d ExeFS patches for another game version, not used",
      "%d 个适用于其他游戏版本的 ExeFS 补丁，未使用")
PC_TR("left out: %s",
      "已排除：%s")
PC_TR("%d cheats (F10 Cheats tab)",
      "%d 个金手指（F10 金手指标签页）")
PC_TR("no cheat file for this game version (%s)",
      "此游戏版本没有金手指文件（%s）")
PC_TR("It changes the game's code: the game is rebuilt with it at the next start.",
      "它会修改游戏代码：下次启动时游戏将带着它重新构建。")
PC_TR("Not an amiibo dump: an amiibo image is 540 bytes.",
      "不是 amiibo 转储文件：amiibo 镜像为 540 字节。")
PC_TR("Not an amiibo dump: the amiibo marker is missing.",
      "不是 amiibo 转储文件：缺少 amiibo 标记。")
PC_TR("Not an amiibo dump: unknown figure data.",
      "不是 amiibo 转储文件：人偶数据未知。")
PC_TR("The file cannot be read.",
      "无法读取该文件。")
PC_TR("amiibo: pick one in F10 (or L3 + R3) > Amiibo",
      "amiibo：在 F10（或 L3 + R3）> Amiibo 中选择")
PC_TR("Your own amiibo dumps (.bin, 540 bytes, or Flipper .nfc) in the amiibo folder. In the game, open its amiibo screen; when it waits for a figure, press Scan on one here.",
      "将你自己的 amiibo 转储文件（.bin，540 字节，或 Flipper .nfc）放入 amiibo 文件夹。在游戏中打开 amiibo 界面；等待人偶时，在此处对其中一个点击扫描。")
PC_TR("Open amiibo folder",
      "打开 amiibo 文件夹")
PC_TR("The game is waiting for an amiibo.",
      "游戏正在等待 amiibo。")
PC_TR("On the reader: %s",
      "读取器上：%s")
PC_TR("Open the game's amiibo screen first: Scan works while it waits for a figure.",
      "请先打开游戏的 amiibo 界面：游戏等待人偶时才能扫描。")
PC_TR("No dumps yet: put .bin or .nfc files in the amiibo folder.",
      "还没有转储文件：请将 .bin 或 .nfc 文件放入 amiibo 文件夹。")
PC_TR("Scan",
      "扫描")
PC_TR("Movement",
      "移动")
PC_TR("Jump",
      "跳跃")
PC_TR("Sprint",
      "冲刺")
PC_TR("Crouch",
      "蹲下")
PC_TR("Let go (a wall, a ride)",
      "松手（墙壁、骑乘）")
PC_TR("Dive",
      "俯冲")
PC_TR("Close the paraglider",
      "收起滑翔伞")
PC_TR("Swim dash",
      "游泳冲刺")
PC_TR("Combat",
      "战斗")
PC_TR("Attack / draw the bow",
      "攻击 / 拉弓")
PC_TR("Lower the bow (cancel)",
      "放下弓（取消）")
PC_TR("Next arrow material",
      "下一种箭矢素材")
PC_TR("Previous arrow material",
      "上一种箭矢素材")
PC_TR("Shield and lock on",
      "盾牌与注视")
PC_TR("Put away / take out the weapon",
      "收起 / 拿出武器")
PC_TR("Take out / put away the bow",
      "拿出 / 收起弓")
PC_TR("Throw",
      "投掷")
PC_TR("Next weapon",
      "下一把武器")
PC_TR("Previous weapon",
      "上一把武器")
PC_TR("Construct: right arm",
      "构造体：右臂")
PC_TR("Construct: left arm",
      "构造体：左臂")
PC_TR("Construct: back part",
      "构造体：后部")
PC_TR("Construct: guard",
      "构造体：防护")
PC_TR("Construct: attach a device",
      "构造体：安装装置")
PC_TR("Construct: get off",
      "构造体：离开")
PC_TR("Items and tools",
      "道具与工具")
PC_TR("Interact",
      "互动")
PC_TR("Melee weapons",
      "近战武器")
PC_TR("Shields",
      "盾牌")
PC_TR("Bows",
      "弓")
PC_TR("Materials",
      "素材")
PC_TR("Scope",
      "望远镜")
PC_TR("Whistle",
      "口哨")
PC_TR("Inventory",
      "物品栏")
PC_TR("Map",
      "地图")
PC_TR("Adventure log",
      "冒险笔记")
PC_TR("Pause menu",
      "暂停菜单")
PC_TR("Abilities",
      "能力")
PC_TR("Ultrahand",
      "究极手")
PC_TR("Fuse",
      "余料建造")
PC_TR("Ascend",
      "通天术")
PC_TR("Recall",
      "倒转乾坤")
PC_TR("Autobuild",
      "蓝图")
PC_TR("Purah Pad camera",
      "普尔亚平板相机")
PC_TR("Ability wheel",
      "能力轮盘")
PC_TR("Use the ability",
      "使用能力")
PC_TR("Secondary action",
      "次要操作")
PC_TR("Pull closer",
      "拉近")
PC_TR("Push away",
      "推远")
PC_TR("Rotate",
      "旋转")
PC_TR("Reset rotation",
      "重置旋转")
PC_TR("Raise",
      "升高")
PC_TR("Lower",
      "降低")
PC_TR("Unstick",
      "解除卡住")
PC_TR("Leave the ability",
      "退出能力")
PC_TR("Leave (camera, Scope, quick menu)",
      "退出（相机、望远镜、快捷菜单）")
PC_TR("Take a photo",
      "拍照")
PC_TR("Self-portrait",
      "自拍")
PC_TR("Camera: zoom in",
      "相机：放大")
PC_TR("Camera: zoom out",
      "相机：缩小")
PC_TR("Place a pin (Scope)",
      "放置图钉（望远镜）")
PC_TR("Horse",
      "马")
PC_TR("Spur",
      "驱马")
PC_TR("Dismount",
      "下马")
PC_TR("Soothe",
      "安抚")
PC_TR("Reset the camera",
      "重置视角")
PC_TR("Menus",
      "菜单")
PC_TR("Confirm",
      "确认")
PC_TR("Back",
      "返回")
PC_TR("Previous tab",
      "上一标签页")
PC_TR("Next tab",
      "下一标签页")
PC_TR("Rotate left",
      "向左旋转")
PC_TR("Rotate right",
      "向右旋转")
PC_TR("Selection up",
      "向上选择")
PC_TR("Selection down",
      "向下选择")
PC_TR("Selection left",
      "向左选择")
PC_TR("Selection right",
      "向右选择")
PC_TR("D-pad up (selection)",
      "十字键上（选择）")
PC_TR("D-pad down (selection)",
      "十字键下（选择）")
PC_TR("D-pad left (selection, value)",
      "十字键左（选择、数值）")
PC_TR("D-pad right (selection, value)",
      "十字键右（选择、数值）")
PC_TR("Scroll up",
      "向上滚动")
PC_TR("Scroll down",
      "向下滚动")
PC_TR("Item action (X prompt)",
      "道具操作（X 提示）")
PC_TR("Menu: ZL (previous page)",
      "菜单：ZL（上一页）")
PC_TR("Menu: ZR (next page)",
      "菜单：ZR（下一页）")
PC_TR("Menu: left stick click",
      "菜单：左摇杆按下")
PC_TR("Menu: right stick click",
      "菜单：右摇杆按下")
PC_TR("Item action (Y prompt)",
      "道具操作（Y 提示）")
PC_TR("Recipe book: open (Food tab)",
      "食谱：打开（料理标签页）")
PC_TR("Recipe book: close",
      "食谱：关闭")
PC_TR("Recipe book: cooking history",
      "食谱：烹饪记录")
PC_TR("Quick menu: choose",
      "快捷菜单：选择")
PC_TR("Quick menu: sort",
      "快捷菜单：排序")
PC_TR("Quick menu: D-pad up",
      "快捷菜单：十字键上")
PC_TR("Quick menu: D-pad down",
      "快捷菜单：十字键下")
PC_TR("Quick menu: D-pad left",
      "快捷菜单：十字键左")
PC_TR("Quick menu: D-pad right",
      "快捷菜单：十字键右")
PC_TR("Quick menu: drop the item",
      "快捷菜单：丢弃道具")
PC_TR("Advance dialogue",
      "推进对话")
PC_TR("Conversation log",
      "对话记录")
PC_TR("Pick the answer",
      "选择回答")
PC_TR("Answer 1",
      "回答 1")
PC_TR("Answer 2",
      "回答 2")
PC_TR("Answer 3",
      "回答 3")
PC_TR("Answer 4",
      "回答 4")
PC_TR("Advance (events)",
      "推进（事件）")
PC_TR("Skip",
      "跳过")
PC_TR("Skip (short scenes)",
      "跳过（短场景）")
PC_TR("Rebinding is not available yet",
      "暂时无法重新绑定")
PC_TR("No such action here",
      "此处没有该操作")
PC_TR("A fixed control has that key",
      "已有固定控制使用该按键")
PC_TR("Reserved key (F10, F12, Windows, Print Screen)",
      "保留按键（F10、F12、Windows、Print Screen）")
PC_TR("Confirm, back and pause must keep a key",
      "确认、返回和暂停必须保留一个按键")
PC_TR("That swap would clash again: choose another key",
      "该交换会再次冲突：请选择其他按键")
PC_TR("The wheel only works for single presses",
      "滚轮仅适用于单次按下")
PC_TR("Refused",
      "已拒绝")
PC_TR("Swapped with ",
      "已互换：")
PC_TR("Stored now; acts once the PC gameplay keys are on (Controls tab).",
      "已保存；启用 PC 游戏按键后生效（操控标签页）。")
PC_TR("Press a key, mouse button or wheel (Esc: cancel)",
      "请按下按键、鼠标按钮或滚轮（Esc：取消）")
PC_TR("Change",
      "更改")
PC_TR("Add",
      "添加")
PC_TR("Default",
      "默认")
PC_TR("Key moved by",
      "按键被移走：")
PC_TR("changed",
      "已更改")
PC_TR("in use by",
      "被占用：")
PC_TR("settings.ini gives this key to both; it works for the other one. Change one of them.",
      "settings.ini 把该按键同时给了两者；它对另一项有效。请更改其中一项。")
PC_TR("Key rebinding is not available in this build yet.",
      "此版本暂不支持按键重新绑定。")
PC_TR("Restore defaults",
      "恢复默认")
PC_TR("Cheats from your mods' cheats folders (Atmosphere format). They switch at once; one that changes the game's code once the game is built with it (at the start after its mod is turned on or changed). Cheats can break a save: keep a copy.",
      "来自各模组 cheats 文件夹的金手指（Atmosphere 格式）。立即生效；修改游戏代码的金手指，需在游戏带着它构建后才生效（在其模组启用或更改后的启动时）。金手指可能损坏存档：请保留副本。")
PC_TR("%s, %d x %d at %d Hz",
      "%s，%d x %d，%d Hz")
PC_TR(" (it can do %d Hz: Windows display settings)",
      "（可达 %d Hz：Windows 显示设置）")
PC_TR(" (native %d x %d)",
      "（原生 %d x %d）")
PC_TR(". HDR on in Windows",
      "。Windows 中已开启 HDR")
PC_TR(". HDR off in Windows (the monitor can)",
      "。Windows 中未开启 HDR（显示器支持）")
PC_TR(". No HDR",
      "。无 HDR")
PC_TR(", %d nits",
      "，%d nits")
PC_TR(". VRR on (%s",
      "。VRR 已开启（%s")
PC_TR(". VRR off in the driver (%s monitor",
      "。VRR 在驱动中已关闭（%s 显示器")
PC_TR(". VRR: a %s monitor, not confirmed by the driver (Options: VRR on if it is on there",
      "。VRR：%s 显示器，驱动未确认（选项：若那里已开启，则 VRR 开启")
PC_TR(". No VRR",
      "。无 VRR")
PC_TR("Graphics",
      "图像")
PC_TR("Rendering",
      "渲染")
PC_TR("Upscaler",
      "超分辨率")
PC_TR("Frame generation",
      "帧生成")
PC_TR("Ultrahand rotation speed",
      "究极手旋转速度")
PC_TR("TOTK_PC_ROTATE_SPEED: with R held in Ultrahand or Autobuild, the mouse moved sideways turns the object in 45-degree steps (1x: the move that turns the camera 45 degrees) and the wheel turns it vertically, a step a notch.",
      "TOTK_PC_ROTATE_SPEED：在究极手或蓝图中按住 R 时，左右移动鼠标以 45 度步进旋转物体（1x：使镜头转动 45 度的移动量），滚轮使其纵向旋转，每格一步。")
PC_TR("0.5x (slow)",
      "0.5x（慢）")
PC_TR("2x (fast)",
      "2x（快）")
PC_TR("Automatic (a dedicated GPU first)",
      "自动（优先独立 GPU）")
PC_TR("Display",
      "显示")
PC_TR("Graphics card",
      "显卡")
PC_TR("The graphics card the game runs on, for a laptop with two. Automatic picks the dedicated one, the faster; the integrated one uses less power but is much slower.",
      "游戏运行所用的显卡，适用于有两块显卡的笔记本。自动选择性能更强的独立显卡；集成显卡更省电但慢得多。")
PC_TR("Sets every quality row below at once. Ultra is the game at its best on an 8 GB card; Low and Medium draw less than the Switch does. Changing a row by hand makes it Custom.",
      "一次设置下方所有画质项。超高是 8 GB 显卡上游戏的最佳效果；低和中的绘制量比 Switch 更少。手动更改某一项会变为自定义。")
PC_TR("Windowed, borderless fullscreen (the default: Alt+Tab is instant) or exclusive fullscreen (the monitor switches to the resolution and refresh rate below). F11 or Alt+Enter also switch it.",
      "窗口化、无边框全屏（默认：Alt+Tab 即时切换）或独占全屏（显示器切换到下方的分辨率和刷新率）。F11 或 Alt+Enter 也可切换。")
PC_TR("The window's size in windowed mode. Fullscreen uses the monitor's resolution.",
      "窗口模式下的窗口大小。全屏使用显示器的分辨率。")
PC_TR("The monitor fullscreen uses and a windowed start opens on. Remember: the one the game was on last.",
      "全屏所用的显示器，窗口化启动也在其上打开。“记住”：上次游戏所在的显示器。")
PC_TR("The resolution the monitor switches to in exclusive fullscreen. Borderless and windowed keep the desktop's.",
      "独占全屏时显示器切换到的分辨率。无边框和窗口化保持桌面分辨率。")
PC_TR("The refresh rate in exclusive fullscreen. Higher is smoother when the frame rate keeps up.",
      "独占全屏时的刷新率。帧率跟得上时，越高越流畅。")
PC_TR("The most frames per second the game shows. Higher is smoother and costs more GPU and CPU; 30 is the Switch's. Unlocked follows the monitor's refresh rate (240 at most); steady holds a rate the scene can keep. For an even picture pick a cap that divides the refresh rate (30 or 60 on 60 Hz, 40 on 120 Hz), or turn VRR on. Steam Deck: 60 (OLED: 90) at first; if it is not held, lower it to 40 with the screen at 40 Hz (Quick Access > Performance).",
      "游戏显示的每秒最大帧数。越高越流畅，但占用更多 GPU 和 CPU；30 为 Switch 的帧率。不限帧率跟随显示器刷新率（最高 240）；稳定则保持场景能维持的帧率。为使画面均匀，请选择能整除刷新率的帧率上限（60 Hz 选 30 或 60，120 Hz 选 40），或开启 VRR。Steam Deck：初始为 60（OLED：90）；若无法维持，请降至 40，并将屏幕设为 40 Hz（快速访问 > 性能）。")
PC_TR("Frame rate \"Unlocked, steady\" only: the lowest rate it holds. 30 (default) suits most PCs. On one that cannot hold 30, 20 or 15 show every frame for the same time (3 or 4 refreshes on 60 Hz) instead of an uneven 15-20.",
      "仅用于帧率“不限帧率，稳定”：它保持的最低帧率。30（默认）适合大多数电脑。在无法稳定 30 的电脑上，20 或 15 让每帧显示相同时长（60 Hz 下 3 或 4 次刷新），而不是不均匀的 15-20。")
PC_TR("%d fps on this %d Hz monitor shows frames unevenly (one refresh, then two). Even: %s, steady, VRR on, or the monitor at %d Hz or a multiple (Steam Deck: Quick Access > Performance).",
      "%d fps 在这台 %d Hz 显示器上会使画面不均匀（一次刷新，然后两次）。均匀的选择：%s、稳定、开启 VRR，或将显示器设为 %d Hz 或其倍数（Steam Deck：快速访问 > 性能）。")
PC_TR("uneven",
      "不均匀")
PC_TR("On (default): no tearing. Off: slightly less input latency, with tearing lines.",
      "开启（默认）：无撕裂。关闭：输入延迟略低，但有撕裂线。")
PC_TR("Variable refresh rate (G-SYNC, FreeSync): the monitor waits for each frame, smooth below its refresh rate and without tearing. Needs a VRR monitor with VRR on in Windows or the driver.",
      "可变刷新率（G-SYNC、FreeSync）：显示器等待每一帧，在其刷新率以下流畅且无撕裂。需要 VRR 显示器，并在 Windows 或驱动中开启 VRR。")
PC_TR("Shows fps, the 1% low and a frame-time graph over the game (F9), to check performance.",
      "在游戏上方显示 fps、1% 低帧和帧时间曲线（F9），用于检查性能。")
PC_TR("A small frame-rate counter in a corner (F3 shows or hides it). Minimal: fps; Detailed adds the frame time, the 1% low and the worst frame. Off costs nothing.",
      "角落里的小型帧率计数器（F3 显示或隐藏）。简略：fps；详细：另加帧时间、1% 低帧和最差帧。关闭不占用资源。")
PC_TR("The counter's corner. Top right keeps clear of the hearts and the map.",
      "计数器所在的角落。右上角不会遮挡爱心和地图。")
PC_TR("The counter's size. It also grows with the window's height.",
      "计数器的大小。也会随窗口高度增大。")
PC_TR("Colour adjustment of the game's picture (not these menus). Off shows the game's own colours and costs nothing; Vivid is livelier; Brighter dark tones lightens caves, the Depths and nights; Custom uses the four rows below.",
      "对游戏画面进行色彩调整（不影响这些菜单）。关闭：显示游戏原色，无开销；鲜艳更生动；提亮暗部会照亮洞穴、地底和夜晚；自定义使用下方四项。")
PC_TR("Custom only: makes every colour brighter or darker; 0 is the game's.",
      "仅自定义：使所有颜色更亮或更暗；0 为游戏原值。")
PC_TR("Custom only: more or less difference between light and dark; 1 is the game's.",
      "仅自定义：调整明暗差异；1 为游戏原值。")
PC_TR("Custom only: how colourful the picture is; 1 is the game's, 0.5 greyer, 2 twice as colourful.",
      "仅自定义：画面的色彩浓度；1 为游戏原值，0.5 更灰，2 为两倍浓度。")
PC_TR("Custom only: below 1 brightens the dark tones, above 1 darkens them; 1 is the game's.",
      "仅自定义：低于 1 提亮暗部，高于 1 压暗；1 为游戏原值。")
PC_TR("The resolution the 3D scene is drawn at, against the game's own. Higher is sharper and costs much more GPU; lower is faster and blurrier. The default: the window's resolution.",
      "3D 场景的绘制分辨率，相对于游戏自身的分辨率。越高越清晰，GPU 开销大得多；越低越快但更模糊。默认：窗口分辨率。")
PC_TR("Lowers the 3D resolution only when the GPU falls behind, never below the floor chosen, to hold the frame rate. Off keeps the picture sharp all the time; On is smoother on a slower GPU.",
      "仅当 GPU 跟不上时降低 3D 分辨率，且不低于所选下限，以保持帧率。关闭：画面始终清晰；开启：在较慢的 GPU 上更流畅。")
PC_TR("Above 1080p, menus, text and the HUD are drawn at the window's resolution instead of the game's 1080p stretched, and the 3D scene (render scale, DLSS / DLAA) reaches the window's resolution too. Sharper picture for a little GPU.",
      "高于 1080p 时，菜单、文字和 HUD 按窗口分辨率绘制，而不是拉伸游戏的 1080p；3D 场景（渲染分辨率、DLSS / DLAA）也达到窗口分辨率。画面更清晰，GPU 开销很小。")
PC_TR("Generated frames between the real ones make motion look smoother, but the game itself does not run faster and input latency rises a little. DLSS x2: RTX 40 and newer, x3 and x4: RTX 50; FSR x2: any GPU. Best when the game already runs well.",
      "在真实帧之间生成的帧使动作更流畅，但游戏本身不会更快，输入延迟会略有增加。DLSS x2：RTX 40 及更新，x3 和 x4：RTX 50；FSR x2：任意 GPU。游戏本身已运行良好时效果最佳。")
PC_TR("How the 3D scene is scaled up to the output: Bicubic (the port's) or FSR 1 (the game's own). Matters only below 100 % render resolution; either costs little.",
      "3D 场景放大到输出分辨率的方式：双三次（重编译）或 FSR 1（游戏自带）。仅在渲染分辨率低于 100 % 时有意义；两者开销都很小。")
PC_TR("The filter that scales the 3D scene when the render resolution is not 100 %: Catmull-Rom, or FSR 1 EASU (sharper edges). Either costs little.",
      "渲染分辨率不是 100 % 时放大 3D 场景的滤镜：Catmull-Rom，或 FSR 1 EASU（边缘更锐利）。两者开销都很小。")
PC_TR("How the finished picture is scaled to the window: Bilinear, or FSR 1 for a window above 1080p (sharper). Costs little.",
      "成品画面缩放到窗口的方式：双线性，或用于高于 1080p 窗口的 FSR 1（更锐利）。开销很小。")
PC_TR("Smooths jagged edges on the final picture. SMAA 1x: smoother edges for a little GPU; Off is the game's own look.",
      "平滑最终画面中的锯齿边缘。SMAA 1x：边缘更平滑，GPU 开销很小；关闭为游戏原样。")
PC_TR("Sharpens the final picture (AMD RCAS). More is crisper but can look grainy; Off is the game's look. Costs very little.",
      "锐化最终画面（AMD RCAS）。越高越清晰，但可能显得有颗粒感；关闭为游戏原样。开销极小。")
PC_TR("Keeps ground textures sharp when seen at a shallow angle. Higher is sharper for a little GPU; 16x suits most GPUs. The game's: its own setting.",
      "在斜视角度下保持地面纹理清晰。越高越清晰，GPU 开销很小；16x 适合大多数 GPU。游戏自带：使用其自身设置。")
PC_TR("Which texture detail level is used; 0 is the game's. Below 0 is sharper but can shimmer; above 0 is blurrier and can break textures (red grass). Not a quality slider: leave it at 0.",
      "使用的纹理细节级别；0 为游戏原值。低于 0 更清晰但可能闪烁；高于 0 更模糊，且可能破坏纹理（红色草地）。不是画质滑块：请保持为 0。")
PC_TR("The resolution of the renderer's shadow maps: 1x is the Switch's, 2x (default) sharper, 3x and 4x sharper still for more GPU time and memory. With little video memory they are lowered to what fits (4 GB cards: 2x).",
      "渲染器阴影贴图的分辨率：1x 为 Switch 的水平，2x（默认）更清晰，3x 和 4x 更加清晰，但占用更多 GPU 时间和显存。显存较小时会降低到可容纳的倍数（4 GB 显卡：2x）。")
PC_TR("Sun shadows grow softer the farther they fall from what casts them, as in real light. Off is the game's look; on costs some GPU. A change rebuilds the shadow shaders in the background: the old shadows show until then.",
      "太阳阴影离投射物越远越柔和，如同真实光照。关闭为游戏原样；开启会占用一些 GPU。更改会在后台重建阴影着色器：在此之前仍显示旧阴影。")
PC_TR("Adds the small sun shadows the shadow maps miss: at feet, under stones, in grass. Off is the game's look; Subtle to Strong cost some GPU.",
      "补充阴影贴图遗漏的细小太阳阴影：脚下、石头下、草丛中。关闭为游戏原样；细微到强烈会占用一些 GPU。")
PC_TR("Prepares the shaders shipped with this version before you meet them, so new places stutter less the first time. Off: only the shaders this PC has built itself. On is better.",
      "在你遇到之前预先准备此版本附带的着色器，使新区域首次进入时卡顿更少。关闭：只使用本机自行构建的着色器。开启更好。")
PC_TR("A shader still being built: Limit to 0.1 s (default) shows the frame after 0.1 s and the missing objects appear a moment later; Always wait never shows anything incomplete but can freeze the picture for a second or more.",
      "着色器仍在构建时：限制为 0.1 s（默认）会在 0.1 s 后显示该帧，缺少的物体稍后出现；始终等待从不显示不完整的画面，但可能使画面卡住一秒或更久。")
PC_TR("Where the frame cap waits: at the present (default, steadier frame times) or before the frame (slightly less input latency).",
      "帧率上限在哪里等待：在呈现处（默认，帧时间更稳定）或在帧开始前（输入延迟略低）。")
PC_TR("What the game's 60 Hz tick follows: a 60 Hz clock (default, right for any monitor) or the monitor's vblank (a fixed-refresh monitor only). Leave the default.",
      "游戏的 60 Hz 节拍跟随什么：60 Hz 时钟（默认，适合任何显示器）或显示器的垂直消隐（仅限固定刷新率显示器）。请保持默认。")
PC_TR("The last part of the frame cap's wait busy-waits for exact timing: steadier frame times for some CPU. Off is the default.",
      "帧率上限等待的最后一段采用忙等待以获得精确计时：帧时间更稳定，占用部分 CPU。默认为关闭。")
PC_TR("How many finished frames may wait to be shown. 1 lowers input latency, 3 evens out uneven frames; 2 is the default.",
      "最多可有多少已完成的帧等待显示。1 降低输入延迟，3 使不均匀的帧更平稳；默认为 2。")
PC_TR("How many frames the CPU may prepare ahead of the GPU. 2 (default) is the balance; 1 lowers latency but is slower when both are busy; 3 adds latency.",
      "CPU 可比 GPU 提前准备多少帧。2（默认）较均衡；1 降低延迟，但两者都忙时更慢；3 增加延迟。")
PC_TR("Holds each frame back just before it reads your input, so it waits less afterwards: lower input latency. NVIDIA uses Reflex; Boost also keeps the GPU clocks up (more power).",
      "在帧读取输入之前才放行，使其之后等待更少：输入延迟更低。NVIDIA 使用 Reflex；Boost 还会保持 GPU 时钟较高（更耗电）。")
PC_TR("HDR output for an HDR monitor (HDR10 or scRGB). Needs HDR on for that monitor in Windows; otherwise the picture stays SDR.",
      "面向 HDR 显示器的 HDR 输出（HDR10 或 scRGB）。需要在 Windows 中为该显示器开启 HDR；否则画面仍为 SDR。")
PC_TR("Your monitor's peak brightness in nits, so highlights are neither clipped nor dim. Use the value the monitor states.",
      "显示器的峰值亮度，单位 nits，使高光既不被削平也不暗淡。请使用显示器标明的数值。")
PC_TR("How bright ordinary white (menus, the sky) is in HDR. Raise it if the picture looks dim, lower it if it is too bright.",
      "HDR 下普通白色（菜单、天空）的亮度。画面偏暗时调高，过亮时调低。")
PC_TR("How much of their real colour bright lights keep in HDR: a torch flame or lava stays orange instead of turning yellow-white. 0 (the default) shows the picture as in SDR, only brighter; the HUD is left as it is.",
      "HDR 下明亮光源保留多少真实颜色：火把的火焰或岩浆保持橙色，而不是变成黄白色。设为 0（默认）时画面与 SDR 相同，只是更亮；HUD 保持原样。")
PC_TR("The window's (the default): on a 3:2, 16:10, 21:9, 32:9 or 48:9 window or monitor the picture takes that shape, 21:9, 32:9 and 48:9 with a wider view; other shapes, 16:9 among them, keep the game's own. 16:9 / 16:10 / 21:9 / 32:9 / 48:9 / 3:2: that shape whatever the window. A new shape's layouts are made at its first start (about 30 s, once). A wider picture costs more GPU.",
      "跟随窗口（默认）：在 3:2、16:10、21:9、32:9 或 48:9 的窗口或显示器上，画面采用该形状，21:9、32:9 和 48:9 视野更宽；其他形状（包括 16:9）保持游戏原画面。16:9 / 16:10 / 21:9 / 32:9 / 48:9 / 3:2：无论窗口如何都使用该形状。新形状的界面布局会在其首次启动时生成（约 30 秒，仅一次）。画面越宽，GPU 负担越大。")
PC_TR("On 21:9, 32:9 or 48:9: the HUD at the screen's edges, or in the middle 16:9 (easier to see on a very wide screen). Nothing changes at 16:9. Automatic (the default): the middle on 48:9, the edges otherwise.",
      "21:9、32:9 或 48:9 时：HUD 位于屏幕边缘，或位于中间的 16:9 区域（超宽屏上更易看清）。16:9 时无变化。 自动（默认）：48:9 时在中间，其他时在边缘。")
PC_TR("On 21:9, 32:9 or 48:9: real-time cutscenes wide like the rest of the game, or the original 16:9 with bars at the sides.",
      "21:9、32:9 或 48:9 时：实时过场动画与游戏其余部分一样宽，或保持原始 16:9 并在两侧加黑边。")
PC_TR("Widens or narrows the camera's view (1 = the game's). Wider shows more of the world and costs some GPU and CPU.",
      "拉宽或收窄视野（1 = 游戏原值）。更宽能看到更多世界，占用一些 GPU 和 CPU。")
PC_TR("More memory for textures: fewer blurry textures when you turn around, for about 490 MiB of video memory per step.",
      "更多纹理内存：转身时模糊纹理更少，每级约占 490 MiB 显存。")
PC_TR("More grass, flowers and pebbles. Higher looks fuller and costs GPU and CPU.",
      "更多草、花和卵石。越高越茂密，占用 GPU 和 CPU。")
PC_TR("Sharper shadows: 2 doubles the shadow map's width and height, for 4x its memory and more GPU.",
      "更清晰的阴影：2 使阴影贴图的宽和高翻倍，内存为 4 倍，GPU 开销更大。")
PC_TR("Moves the lines where the sun's sharp near shadows hand over to coarser ones further away (1 = the game's, 2 = twice as far). Near shadows get a little softer: raise Shadow resolution with it. Costs a little GPU and CPU. With Shadow distance above 1 the two multiplied stay at 4 at most, so at Shadow distance 4 this does nothing.",
      "把太阳近处清晰阴影过渡到较粗糙阴影的分界线推得更远（1 = 游戏原设，2 = 两倍距离）。近处阴影会稍微变柔和：请同时提高阴影分辨率。会稍微增加 GPU 和 CPU 开销。 当阴影距离大于 1 时，两者相乘最多为 4，因此阴影距离为 4 时此项不起作用。")
PC_TR("Fixed keeps the sun's shadow map at its largest size: steadier, sharper leaf shadows for some GPU. The game's changes its size with the view.",
      "固定使太阳阴影贴图保持最大尺寸：叶片阴影更稳定、更锐利，占用一些 GPU。游戏自带会随视角改变其大小。")
PC_TR("How far the sharp shadows reach. Higher looks better in the distance and costs GPU.",
      "清晰阴影的延伸距离。越高远处越好看，占用 GPU。")
PC_TR("Pushes the distance fog back: clearer far mountains. Higher shows more and costs some GPU.",
      "把远处雾气推远：远山更清晰。越高显示越多，占用一些 GPU。")
PC_TR("How far models keep their full detail. Higher looks better in the distance and costs GPU and CPU.",
      "模型保持完整细节的距离。越高远处越好看，占用 GPU 和 CPU。")
PC_TR("How far characters, enemies and objects keep full animation. Higher is smoother in the distance and costs CPU.",
      "角色、敌人和物体保持完整动画的距离。越高远处越流畅，占用 CPU。")
PC_TR("How far the ground and water keep their full detail. Higher looks better and costs CPU and GPU.",
      "地面和水面保持完整细节的距离。越高越好看，占用 CPU 和 GPU。")
PC_TR("How far from the camera characters, enemies and physics keep being simulated (1 = the game's). Higher keeps distant things alive and costs CPU.",
      "角色、敌人和物理效果距离镜头多远仍保持模拟（1 = 游戏原值）。越高远处事物保持活跃，占用 CPU。")
PC_TR("Fast: Link's departure when travelling is about 2.5 s shorter. Normal is the game's own.",
      "快速：旅行时林克出发的等待约缩短 2.5 s。常规为游戏原样。")
PC_TR("The game's overall volume, on top of the Windows volume.",
      "游戏的总音量，叠加在 Windows 音量之上。")
PC_TR("The music's volume.",
      "音乐音量。")
PC_TR("The sound effects' volume.",
      "音效音量。")
PC_TR("The voices' volume. No part of the game's mix carries voices alone yet, so this does nothing for now.",
      "语音音量。游戏的混音中目前没有单独承载语音的部分，因此暂时不起作用。")
PC_TR("The speakers the game mixes for: stereo, or 5.1 / 7.1 surround (experimental). Pick what your speakers or headphones use.",
      "游戏混音所用的扬声器：立体声，或 5.1 / 7.1 环绕声（实验性）。请选择你的扬声器或耳机所用的。")
PC_TR("How much sound is queued ahead. Lower responds sooner; raise it if the sound crackles.",
      "提前排入多少音频。越低响应越快；声音有爆音时请调高。")
PC_TR("The sound moves to headphones or speakers as you plug them in or out, without a restart.",
      "插拔耳机或扬声器时，声音自动切换，无需重启。")
PC_TR("No sound while the game window is not in front; the game keeps running.",
      "游戏窗口不在前台时无声音；游戏继续运行。")
PC_TR("PC: keyboard and mouse controls in the menus (Q / E tabs, Esc back, the mouse pointer) and PC keys in the prompts. Console: the Switch layout on the keyboard everywhere.",
      "PC：菜单中使用键盘和鼠标操作（Q / E 切换标签页，Esc 返回，鼠标指针），提示中显示 PC 按键。主机：所有界面都使用键盘上的 Switch 布局。")
PC_TR("PC (default): one key per action (Space jumps, Shift sprints, F attacks...). Console layout: the old keys in the world. Menus use the PC keys either way.",
      "PC（默认）：每个动作一个按键（空格跳跃，Shift 冲刺，F 攻击……）。主机布局：世界中使用旧按键。菜单两种方式下都使用 PC 按键。")
PC_TR("The keys of the Console scheme: Classic, PC (WASD), or Custom (the key bindings below).",
      "主机方案的按键：经典、PC（WASD）或自定义（下方的按键绑定）。")
PC_TR("The button pictures in prompts and menus. Automatic follows the device you used last: keyboard and mouse, Xbox, PlayStation or Nintendo.",
      "提示和菜单中的按键图标。自动跟随你最近使用的设备：键盘和鼠标、Xbox、PlayStation 或 Nintendo。")
PC_TR("The pad's face buttons in menus and dialogues: as the game, printed A accepts and B goes back, or the Switch layout (right accepts). The world keeps its layout.",
      "菜单和对话中手柄的面键：与游戏相同、印有 A 的键确认且 B 返回，或 Switch 布局（右键确认）。世界中的布局不变。")
PC_TR("In menus a mouse pointer shows and selects what it points at; left click is A, Esc goes back.",
      "菜单中会显示鼠标指针并选中其指向的项；左键为 A，Esc 返回。")
PC_TR("In the pause menu and the Purah Pad: Esc goes back, PageUp / PageDown or the wheel switch tabs, left click selects and right click goes back.",
      "在暂停菜单和普尔亚平板中：Esc 返回，PageUp / PageDown 或滚轮切换标签页，左键选择，右键返回。")
PC_TR("How fast the mouse turns the camera (click the window to capture the pointer; Esc, F10 or Alt+Tab frees it). 0 = off; above 1 turns faster.",
      "鼠标转动视角的速度（点击窗口以捕获指针；Esc、F10 或 Alt+Tab 可释放）。0 = 关闭；大于 1 转得更快。")
PC_TR("How fast the mouse turns the camera and aims. Higher turns faster.",
      "鼠标转动视角和瞄准的速度。越高转得越快。")
PC_TR("The mouse's speed while the bow is drawn, times the usual one (1 = the same). Lower aims more finely.",
      "拉弓时的鼠标速度，乘以常规速度（1 = 相同）。越低瞄准越精细。")
PC_TR("How much mouse movement a second counts as the stick pushed all the way. Lower turns faster.",
      "鼠标每秒移动多少算作摇杆推到底。越低转得越快。")
PC_TR("Moving the mouse up looks down, as in flight games.",
      "鼠标上移则向下看，如同飞行游戏。")
PC_TR("The mouse buttons aim and shoot.",
      "鼠标按钮用于瞄准和射击。")
PC_TR("The mouse stands in for a pad's motion aim: while aiming, always, or off.",
      "鼠标代替手柄的体感瞄准：瞄准时、始终或关闭。")
PC_TR("How far motion aim turns for a mouse movement. Higher turns further.",
      "鼠标移动一次，体感瞄准转动多远。越高转得越远。")
PC_TR("While a D-pad quick menu is open (weapons, shields, items, bows), each wheel notch moves one entry.",
      "十字键快捷菜单打开时（武器、盾牌、道具、弓），滚轮每滚一格移动一项。")
PC_TR("On the map the mouse pans, the wheel zooms, a left click places a pin and a right click goes back.",
      "在地图上，鼠标平移，滚轮缩放，左键放置图钉，右键返回。")
PC_TR("With the walk key held (Alt in the PC layout), how far the movement keys push the stick: lower walks slower; 1 is as without the key.",
      "按住步行键时（PC 布局中为 Alt），移动键推动摇杆的幅度：越小走得越慢；1 与不按该键相同。")
PC_TR("How long a quick tap of W, A, S or D keeps Link moving, so he takes a step instead of sliding. 0 = off.",
      "轻点 W、A、S 或 D 后，林克持续移动多久，使其迈出一步而不是滑行。0 = 关闭。")
PC_TR("One press of the sprint key starts sprinting and the next stops it. Off: hold it, as on the Switch.",
      "按一下冲刺键开始冲刺，再按一下停止。关闭：按住，与 Switch 相同。")
PC_TR("One press of the shield / target key raises the shield and targets, the next lowers it. Off: hold it, as on the Switch.",
      "按一下盾牌 / 注视键举盾并注视，再按一下放下。关闭：按住，与 Switch 相同。")
PC_TR("How fast moving the mouse sideways turns an object while R is held in Ultrahand or Autobuild (1x: as far as the camera turns). The wheel turns it vertically; moving the mouse up or down does not turn it.",
      "在究极手或蓝图中按住 R 时，左右移动鼠标旋转物体的快慢（1x：与镜头转动相同）。滚轮使其纵向旋转；上下移动鼠标不会旋转物体。")
PC_TR("A non-Nintendo pad's face buttons: by position (the Switch layout: the right button confirms) or by label (Xbox A confirms).",
      "非 Nintendo 手柄的面键：按位置（Switch 布局：右侧按钮确认）或按标签（Xbox A 确认）。")
PC_TR("Controller vibration: off, normal, or stronger (x2 to x4).",
      "手柄震动：关闭、常规或更强（x2 到 x4）。")
PC_TR("A pad's motion controls (aiming, some puzzles). Off ignores the gyro.",
      "手柄的体感操作（瞄准、部分谜题）。关闭则忽略陀螺仪。")
PC_TR("How far motion aim turns for a turn of the pad: 1 = the pad's own, 2 = twice as far.",
      "瞄准随手柄转动而转动的幅度：1 = 手柄自带，2 = 两倍。")
PC_TR("How light rumbles feel next to strong ones: below 1 light ones come through stronger, above 1 only strong ones do. 1 = the game's own.",
      "轻微震动相对强烈震动的感受：低于 1 时轻微震动更明显，高于 1 时只保留强烈震动。1 = 游戏原本的。")
PC_TR("Xbox One / Series pads: the trigger motors buzz with the game's rumble. Off by default (the Switch pad has none).",
      "Xbox One / Series 手柄：扳机马达随游戏的震动而震动。默认关闭（Switch 手柄没有扳机马达）。")
PC_TR("How fast the right stick turns the camera: 1 = the game's own, 2 = twice as fast. The mouse keeps its own sensitivity.",
      "右摇杆转动镜头的速度：1 = 游戏原本的，2 = 两倍快。鼠标保留自己的灵敏度。")
PC_TR("How far the left stick must move before it counts. Raise it if Link moves with the stick at rest; lower it for finer steps. 0 = the pad's own.",
      "左摇杆需要移动多远才会生效。若摇杆静止时林克仍在移动，请调高；想要更精细的步伐，请调低。0 = 手柄自带。")
PC_TR("How far the right stick must move before it counts. Raise it if the camera turns with the stick at rest; lower it for finer aiming. 0 = the pad's own.",
      "右摇杆需要移动多远才会生效。若摇杆静止时视角仍在转动，请调高；想要更精细的瞄准，请调低。0 = 手柄自带。")
PC_TR("The game's texts and subtitles, and these settings' own (English, Spanish, Russian, Brazilian Portuguese, Simplified Chinese, Traditional Chinese). Português (Brasil): 1.4.3's texts; with 1.0.0 they need the 1.4.3 update given to Setup (English otherwise), voices stay English. Automatic: Windows' language if the game has it, else English.",
      "游戏的文本和字幕，以及这些设置自身的文字（英语、西班牙语、俄语、巴西葡萄牙语、简体中文、繁体中文）。Português (Brasil)：1.4.3 的文本；1.0.0 需要交给 Setup 的 1.4.3 更新（否则为英语），语音仍为英语。自动：游戏支持时使用 Windows 的语言，否则为英语。")
PC_TR("A new game skips the opening and starts on the Great Sky Island. F2 or Minus on the title screen also switch it. Game version 1.0.0 only (on 1.4.x the opening plays and the row is hidden).",
      "新游戏会跳过开场，直接从初始空岛开始。标题画面上按 F2 或－键也可切换。仅限游戏版本 1.0.0（在 1.4.x 上会播放开场，此行隐藏）。")
PC_TR("Docked (default) or handheld: handheld renders at the Switch's lower handheld resolution (faster, blurrier).",
      "底座模式（默认）或掌机模式：掌机模式以 Switch 较低的掌机分辨率渲染（更快，更模糊）。")
PC_TR("Pauses the game, picture and sound, while its window is not in front.",
      "窗口不在前台时暂停游戏、画面和声音。")
PC_TR("The temperature on the Purah Pad's map: °C or °F. Automatic follows the game's language.",
      "普尔亚平板地图上的温度单位：°C 或 °F。自动：跟随游戏语言。")
PC_TR("How many older copies of your saves are kept, in case a save goes wrong.",
      "保留多少份旧存档备份，以防存档损坏。")
PC_TR("F12 saves a screenshot as a PNG in the Screenshots folder.",
      "F12 将截图以 PNG 格式保存到 Screenshots 文件夹。")
PC_TR("After a crash, a report and a minidump are written to attach to a bug report. Nothing is sent anywhere.",
      "崩溃后会生成报告和 minidump，可附在错误报告中。不会向任何地方发送数据。")
PC_TR("Checks GitHub for a newer version when the game starts (at most once a day); asks first if the update rebuilds the game.",
      "游戏启动时检查 GitHub 上是否有新版本（每天最多一次）；若更新需要重新构建游戏，会先询问。")
PC_TR("%s: get off",
      "%s：下来")
PC_TR("Control scheme",
      "操控方案")
PC_TR("PC: the keyboard and mouse controls of the menus (Q / E tabs, Esc back, the mouse pointer), Esc / I / M / J in the world and the prompts with PC keys. Console: the keyboard layout below in every screen.",
      "PC：菜单使用键盘鼠标操作（Q / E 切换标签页，Esc 返回，鼠标指针），游戏中使用 Esc / I / M / J，提示显示 PC 按键。主机：所有界面都使用下方的键位布局。")
PC_TR("Console",
      "主机")
PC_TR("Gameplay keys",
      "游戏按键")
PC_TR("TOTK_GAMEPLAY. With the PC scheme: PC (default) = one key per action: Space jumps, Shift sprints, F attacks, Q guards, 4-9 pick an ability, Tab opens the wheel...; Console layout = the keys as before in the world. Menus use the PC keys either way.",
      "TOTK_GAMEPLAY。使用 PC 方案时：PC（默认）= 一个按键对应一个动作：空格跳跃，Shift 冲刺，F 攻击，Q 防御，4-9 选择能力，Tab 打开轮盘……；主机布局 = 游戏中沿用原来的按键。两种方式下菜单都使用 PC 按键。")
PC_TR("Console layout",
      "主机布局")
PC_TR("Language",
      "语言")
PC_TR("TOTK_LANG: the game's texts and voices' subtitles, and the PC settings' own texts (English, Spanish, Russian, Brazilian Portuguese, Simplified Chinese, Traditional Chinese). Português (Brasil): 1.4.3's texts; with 1.0.0 they need the 1.4.3 update given to Setup (English otherwise), voices stay English. Automatic: Windows' display language if the game has it, else English.",
      "TOTK_LANG：游戏文本、语音字幕以及 PC 设置自身的文字（英语、西班牙语、俄语、巴西葡萄牙语、简体中文、繁体中文）。Português (Brasil)：1.4.3 的文本；1.0.0 需要交给 Setup 的 1.4.3 更新（否则为英语），语音仍为英语。自动：游戏支持时使用 Windows 的显示语言，否则为英语。")
PC_TR("Automatic (Windows' language)",
      "自动（Windows 语言）")
PC_TR("Russian",
      "俄语")
PC_TR("Japanese",
      "日语")
PC_TR("Korean",
      "韩语")
PC_TR("Chinese (Simplified)",
      "简体中文")
PC_TR("Chinese (Traditional)",
      "繁体中文")
PC_TR("This folder has no romfs, exefs or cheats files.",
      "此文件夹中没有 romfs、exefs 或 cheats 文件。")
PC_TR("The folder name contains |, which the mod list uses between names. Rename the folder.",
      "文件夹名称含有 |，而 Mod 列表用它分隔名称。请重命名该文件夹。")
PC_TR("It is made for another game (title %s).",
      "它适用于另一款游戏（title %s）。")
PC_TR("An earlier mods folder has a mod with this name; this one is left out.",
      "之前的 mods 文件夹中已有同名 Mod，此项被忽略。")
PC_TR("It has code modules (subsdk or main), which the port cannot load, so the whole mod stays off.",
      "它包含代码模块（subsdk 或 main），本移植版无法加载，因此整个 Mod 保持关闭。")
PC_TR("It only has code patches (exefs) or cheats, which this version cannot use yet.",
      "它只有代码补丁（exefs）或金手指，当前版本尚无法使用。")
PC_TR("A mod's folder",
      "某个 Mod 的文件夹")
PC_TR("A mod (.zip, .7z or .rar)",
      "Mod（.zip、.7z 或 .rar）")
PC_TR("code patches",
      "代码补丁")
PC_TR("cheats",
      "金手指")
PC_TR("Installed %s (%s, for %s) in %s. It loads at the next start.",
      "已安装 %s（%s，适用于 %s），位于 %s。下次启动时加载。")
PC_TR("Its files don't name a game version: it was put with the one you play.",
      "其文件未注明游戏版本：已放到你正在玩的版本下。")
PC_TR("It is made for game version %s, which the port does not build: it was put with the 1.4.3 mods and may not work.",
      "此 Mod 适用于游戏版本 %s，本移植版不构建该版本：已放到 1.4.3 的 Mod 中，可能无法使用。")
PC_TR("Not installed: it could not be opened. Unpack it yourself and choose its folder with Install a mod folder.",
      "未安装：无法打开。请自行解压，然后用“安装 Mod 文件夹”选择其文件夹。")
PC_TR("Not installed: it has no romfs, exefs or cheats folder. If the mod is packed inside it, unpack it and choose that folder instead.",
      "未安装：其中没有 romfs、exefs 或 cheats 文件夹。如果 Mod 打包在里面，请解压后改选该文件夹。")
PC_TR("Not installed: it is a mod for another game.",
      "未安装：这是其他游戏的 Mod。")
PC_TR("Not installed: it loads code of its own (subsdk / main), which a recompiled game cannot load.",
      "未安装：它会加载自身的代码（subsdk / main），重编译的游戏无法加载。")
PC_TR("Not installed: it could not be copied. Check that the mods folder can be written (Open mods folder).",
      "未安装：无法复制。请检查 mods 文件夹是否可写入（打开 mods 文件夹）。")
PC_TR("Mods load at the next start. The top mod wins a file two mods change; the port's own files (PC button prompts, fixes) stay over a mod's.",
      "Mod 在下次启动时加载。两个 Mod 修改同一文件时，列表靠前的优先；移植版自带的文件（PC 按键提示、修复）优先于 Mod。")
PC_TR("The launcher chose these mods.",
      "这些 Mod 由启动器选择。")
PC_TR("Open mods folder",
      "打开 mods 文件夹")
PC_TR("Install a mod...",
      "安装 Mod……")
PC_TR("Install a mod folder...",
      "安装 Mod 文件夹……")
PC_TR("Installing...",
      "正在安装……")
PC_TR("In exclusive fullscreen the game goes to the desktop while the dialog is open.",
      "独占全屏时，对话框打开期间游戏会切到桌面。")
PC_TR("No mods yet. Put each mod in a folder of its own in the mods folder (its romfs, exefs and cheats folders as emulators take them), then start the game again.",
      "还没有 Mod。请将每个 Mod 放进 mods 文件夹中各自的文件夹里（其 romfs、exefs 和 cheats 文件夹与模拟器的用法相同），然后重新启动游戏。")
PC_TR("Ctrl+Up / Ctrl+Down (pad: Y / X) move the mod in focus.",
      "Ctrl+上 / Ctrl+下（手柄：Y / X）移动当前选中的 Mod。")
PC_TR("%d files replaced, %d added",
      "替换 %d 个文件，新增 %d 个")
PC_TR("; %d left to the port's own files",
      "；%d 个留给移植版自带文件")
PC_TR("; %d left to %s, higher in the list",
      "；%d 个留给列表中更靠前的 %s")
PC_TR("; %d for another game version, not used",
      "；%d 个属于其他游戏版本，未使用")
PC_TR("Processor: %s, %d cores, %lu threads. Memory: %.0f GB.",
      "处理器：%s，%d 核，%lu 线程。内存：%.0f GB。")
PC_TR("unknown",
      "未知")
PC_TR("Graphics card: %s, %.0f GB.",
      "显卡：%s，%.0f GB。")
PC_TR("Display: %s",
      "显示：%s")
PC_TR("1%% low %.1f fps (%.1f ms)",
      "1%% 低帧 %.1f fps（%.1f ms）")
PC_TR("1%% low: collecting",
      "1%% 低帧：采集中")
PC_TR("frame time 0-50 ms  (F9)",
      "帧时间 0-50 ms  (F9)")
PC_TR("frame",
      "帧")
PC_TR("1% low",
      "1% 低帧")
PC_TR("worst",
      "最差")
PC_TR("real",
      "真实")
PC_TR("Press a key or mouse button (Esc: cancel)",
      "按下按键或鼠标按钮（Esc：取消）")
PC_TR("(the layout's keys)",
      "（布局自带的按键）")
PC_TR("Add key",
      "添加按键")
PC_TR("Clear",
      "清除")
PC_TR("A (right)",
      "A（右）")
PC_TR("B (bottom)",
      "B（下）")
PC_TR("X (top)",
      "X（上）")
PC_TR("Y (left)",
      "Y（左）")
PC_TR("L stick",
      "左摇杆")
PC_TR("R stick",
      "右摇杆")
PC_TR("none",
      "无")
PC_TR("Press a pad button (Esc: cancel)",
      "按下手柄按钮（Esc：取消）")
PC_TR("(its own button)",
      "（自带的按键）")
PC_TR("Add button",
      "添加手柄按钮")
PC_TR("this run",
      "本次运行")
PC_TR("Set by NX_SET_ for this run only; not saved.",
      "仅本次运行由 NX_SET_ 设定，不保存。")
PC_TR("at next start",
      "下次启动时")
PC_TR("restart",
      "重启")
PC_TR("Point at a setting, or move to it, to see what it does.",
      "指向或移动到某项设置，即可查看其作用。")
PC_TR("Applies when the game starts again.",
      "重新启动游戏后生效。")
PC_TR("No settings on this page yet.",
      "此页暂无设置。")
PC_TR("Reset this page to defaults",
      "将此页恢复默认")
PC_TR("Settings  (F10)",
      "设置  (F10)")
PC_TR("Mods",
      "Mod")
PC_TR("Cheats",
      "金手指")
PC_TR("Keys",
      "按键")
PC_TR("Some changes apply when the game starts again.",
      "部分更改需要重新启动游戏后生效。")
PC_TR("Restart the game now",
      "立即重启游戏")
PC_TR("The game closes and starts again with the new settings. Progress since your last save is lost.",
      "游戏将关闭并以新设置重新启动。自上次存档以来的进度会丢失。")
PC_TR("Restart",
      "重启")
PC_TR("Cancel",
      "取消")
PC_TR("Saved to %s",
      "已保存到 %s")
PC_TR("Not saved (TOTK_SETTINGS=0)",
      "未保存（TOTK_SETTINGS=0）")
PC_TR("Copy system info",
      "复制系统信息")
PC_TR("Copied: paste it into your bug report.",
      "已复制：请粘贴到你的错误报告中。")
PC_TR("Could not copy.",
      "无法复制。")
PC_TR("F10, Esc, B or L3 + R3 closes. A: select. L / R: pages. F9: perf overlay.",
      "F10、Esc、B 或 L3 + R3 关闭。A：选择。L / R：切换页面。F9：性能叠加显示。")
PC_TR("Low",
      "低")
PC_TR("Medium",
      "中")
PC_TR("High",
      "高")
PC_TR("Welcome",
      "欢迎")
PC_TR("Pick graphics settings for this PC. Everything can be changed later with F10 (or L3 + R3).",
      "为这台电脑选择图像设置。之后都可以通过 F10（或 L3 + R3）更改。")
PC_TR("Set for this display: frame rate %s%s.%s",
      "已按此显示器设置：帧率 %s%s。%s")
PC_TR("Unlocked (up to the refresh)",
      "不限帧率（最高到刷新率）")
PC_TR(", VRR on",
      "，VRR 已开启")
PC_TR(" HDR output (off) is in F10 > Graphics > HDR.",
      " HDR 输出（已关闭）位于 F10 > 图像 > HDR。")
PC_TR("%s (recommended)",
      "%s（推荐）")
PC_TR("Keep the current settings",
      "保持当前设置")
PC_TR("Some choices apply when the game starts again. Esc keeps the current settings.",
      "部分选项需要重新启动游戏后生效。按 Esc 保持当前设置。")
PC_TR("Applies when the game starts again: F10 > Restart the game now",
      "重新启动游戏后生效：F10 > 立即重启游戏")
PC_TR("Preset",
      "预设")
PC_TR("Graphics preset",
      "图像预设")
PC_TR("Sets the rows below it; any change by hand makes it Custom.",
      "设定下方各项；手动更改任何一项都会变为自定义。")
PC_TR("Ultra",
      "超高")
PC_TR("Ultra+",
      "超高+")
PC_TR("Custom",
      "自定义")
PC_TR("First-run screen done",
      "首次运行界面已完成")
PC_TR("F10 hint shown",
      "已显示 F10 提示")
PC_TR("Window mode",
      "窗口模式")
PC_TR("F11 or Alt+Enter also switch it.",
      "F11 或 Alt+Enter 也可切换。")
PC_TR("Windowed",
      "窗口化")
PC_TR("Borderless fullscreen (default)",
      "无边框全屏（默认）")
PC_TR("Exclusive fullscreen",
      "独占全屏")
PC_TR("Window size",
      "窗口大小")
PC_TR("Windowed mode only (LARECOMP_WINDOW at start).",
      "仅窗口化模式（启动时为 LARECOMP_WINDOW）。")
PC_TR("1280 x 720 (default)",
      "1280 x 720（默认）")
PC_TR("Frame rate",
      "帧率")
PC_TR("TOTK_FPS / TOTK_UNLOCK_FPS. Unlocked: up to the monitor's refresh rate (60 to 240), loading screens at the game's own pace. Steady: unlocked, but held at the highest rate the scene keeps, in whole refreshes of the monitor (240, 120, 80, 60 on a 240 Hz one), instead of swinging. 40 and 45: for a PC that cannot hold 60; smooth on a monitor whose refresh they divide (40 on 120 Hz, 45 on 90 or 180 Hz) or with VRR, uneven on a fixed 60 Hz.",
      "TOTK_FPS / TOTK_UNLOCK_FPS。不限帧率：最高到显示器刷新率（60 至 240），加载画面按游戏自身节奏。稳定：同样不限帧率，但固定在场景能保持的最高帧率上，且为显示器刷新次数的整数分之一（240 Hz 显示器上为 240、120、80、60），不再来回波动。40 和 45：适合无法稳定 60 的电脑；在刷新率能被其整除的显示器上流畅（120 Hz 上 40，90 或 180 Hz 上 45）或配合 VRR 使用，固定 60 Hz 下则不均匀。")
PC_TR("30 (the game's own)",
      "30（游戏自带）")
PC_TR("Unlocked (monitor rate)",
      "不限帧率（显示器刷新率）")
PC_TR("Unlocked, steady (a rate it can hold)",
      "不限帧率，稳定（可保持的帧率）")
PC_TR("Steady's lowest rate",
      "稳定模式最低帧率")
PC_TR("TOTK_STEADY_FLOOR. Frame rate Steady only: the lowest rate it holds. 20 or 15 keep a PC that cannot hold 30 even (3 or 4 refreshes a frame on 60 Hz).",
      "TOTK_STEADY_FLOOR。仅用于“稳定”帧率：它保持的最低帧率。20 或 15 让无法稳定 30 的电脑帧间隔均匀（60 Hz 下每帧 3 或 4 次刷新）。")
PC_TR("30 (default)",
      "30（默认）")
PC_TR("Perf overlay (F9)",
      "性能叠加显示（F9）")
PC_TR("Fps, 1% low and the frame-time graph.",
      "显示 fps、1% 低帧和帧时间曲线。")
PC_TR("Fps readout (F3)",
      "帧率显示（F3）")
PC_TR("TOTK_FPS_OVERLAY. F3 shows or hides it in game.",
      "TOTK_FPS_OVERLAY。游戏中按 F3 显示或隐藏。")
PC_TR("Off (default; F3 shows it)",
      "关闭（默认；F3 显示）")
PC_TR("Minimal: fps",
      "简略：fps")
PC_TR("Detailed: fps, frame time, 1% low, worst",
      "详细：fps、帧时间、1% 低帧、最差")
PC_TR("Fps readout corner",
      "帧率显示位置")
PC_TR("Top right keeps clear of the hearts and the map.",
      "右上角不会遮挡爱心和地图。")
PC_TR("Top right (default)",
      "右上角（默认）")
PC_TR("Top left",
      "左上角")
PC_TR("Bottom right",
      "右下角")
PC_TR("Bottom left",
      "左下角")
PC_TR("Fps readout size",
      "帧率显示大小")
PC_TR("Follows the window height (1080p to 4K).",
      "随窗口高度缩放（1080p 到 4K）。")
PC_TR("Small (default)",
      "小（默认）")
PC_TR("Medium size",
      "中")
PC_TR("Large size",
      "大")
PC_TR("Render resolution",
      "渲染分辨率")
PC_TR("TOTK_RES_SCALE, of the game's 1600 x 900. Window resolution: the 3D scene as large as the picture in the window (x0.83 of it on an integrated GPU or one under 8 GB; two thirds of it with DLSS, FSR or XeSS, their Quality). Applies at once.",
      "TOTK_RES_SCALE，相对于游戏的 1600 x 900。窗口分辨率：3D 场景与窗口中的画面一样大（集成显卡或显存低于 8 GB 时为其 x0.83；使用 DLSS、FSR 或 XeSS 时为其三分之二，即质量模式）。立即生效。")
PC_TR("100 % (native)",
      "100 %（原生）")
PC_TR("Window resolution (default)",
      "窗口分辨率（默认）")
PC_TR("Window resolution x0.83 (default: integrated GPU or under 8 GB)",
      "窗口分辨率 x0.83（默认：集成显卡或显存低于 8 GB）")
PC_TR("Native-resolution interface",
      "原生分辨率界面")
PC_TR("TOTK_OUT_SCALE: above 1080p, menus, text and the HUD drawn at the window's resolution instead of the game's 1920 x 1080 stretched; the 3D scene reaches it too. On by default. Applies at once.",
      "TOTK_OUT_SCALE：高于 1080p 时，菜单、文字和 HUD 按窗口分辨率绘制，而不是拉伸游戏的 1920 x 1080；3D 场景也达到该分辨率。默认开启。立即生效。")
PC_TR("Dynamic resolution",
      "动态分辨率")
PC_TR("Off: the scene always at the render resolution. On: the game lowers it only when the GPU is short of time, never below the chosen floor, aiming at the frame rate. Applies at once.",
      "关闭：场景始终使用渲染分辨率。开启：仅在 GPU 时间不足时由游戏降低分辨率，不会低于所选下限，以达到目标帧率为准。立即生效。")
PC_TR("Off (fixed size)",
      "关闭（固定大小）")
PC_TR("On, never below 90 %",
      "开启，不低于 90 %")
PC_TR("On, never below 75 %",
      "开启，不低于 75 %")
PC_TR("On, never below 50 %",
      "开启，不低于 50 %")
PC_TR("On, the game's own (no minimum)",
      "开启，游戏自带（无下限）")
PC_TR("Dynamic resolution (clock)",
      "动态分辨率（时钟）")
PC_TR("TOTK_DYNRES: the game's own, aimed at the frame rate.",
      "TOTK_DYNRES：游戏自带方式，以帧率为目标。")
PC_TR("3D upscaler",
      "3D 放大器")
PC_TR("TOTK_SSAA. Applies at once.",
      "TOTK_SSAA。立即生效。")
PC_TR("Bicubic (recomp)",
      "双三次（重编译）")
PC_TR("FSR 1 (the game's)",
      "FSR 1（游戏自带）")
PC_TR("Shipped shader list",
      "附带着色器列表")
PC_TR("TOTK_SHADER_SEED: on, the shader list that comes with the build lets the game prepare those shaders before you meet them (fewer stutters the first time). Off: only the shaders this PC has met itself. Takes effect at the next start.",
      "TOTK_SHADER_SEED：开启后，随版本附带的着色器列表会让游戏在你遇到这些着色器之前先行准备（首次遇到时卡顿更少）。关闭：只使用这台电脑自己遇到过的着色器。下次启动时生效。")
PC_TR("Window upscaler",
      "窗口放大器")
PC_TR("TOTK_UPSCALE.",
      "TOTK_UPSCALE。")
PC_TR("Bilinear",
      "双线性")
PC_TR("FSR 1 (window above 1080p)",
      "FSR 1（窗口高于 1080p）")
PC_TR("Anti-aliasing",
      "抗锯齿")
PC_TR("TOTK_AA, on the presented image.",
      "TOTK_AA，作用于呈现的画面。")
PC_TR("Sharpening",
      "锐化")
PC_TR("TOTK_SHARPEN.",
      "TOTK_SHARPEN。")
PC_TR("RCAS low",
      "RCAS 低")
PC_TR("RCAS medium",
      "RCAS 中")
PC_TR("RCAS high",
      "RCAS 高")
PC_TR("Colour",
      "色彩")
PC_TR("Colour adjustment",
      "色彩调整")
PC_TR("TOTK_COLOR: the game's picture, its HUD included (not this menu). Off shows the game's own colours unchanged and costs nothing. Vivid: livelier colours. Brighter dark tones: caves, the Depths and nights lighter. Custom: the four rows below. Applies at once.",
      "TOTK_COLOR：作用于游戏画面，包括其 HUD（不含本菜单）。关闭显示游戏原色，不增加开销。鲜艳：色彩更生动。提亮暗部：洞穴、地底和夜晚更亮。自定义：下方四项。立即生效。")
PC_TR("Off (the game's own colours; default)",
      "关闭（游戏原色；默认）")
PC_TR("Vivid",
      "鲜艳")
PC_TR("Brighter dark tones",
      "提亮暗部")
PC_TR("Custom (the four rows below)",
      "自定义（下方四项）")
PC_TR("Brightness",
      "亮度")
PC_TR("TOTK_COLOR_BRIGHTNESS (Custom only): added to every colour; 0 is the game's.",
      "TOTK_COLOR_BRIGHTNESS（仅自定义）：加到每种颜色上；0 为游戏原样。")
PC_TR("Contrast",
      "对比度")
PC_TR("TOTK_COLOR_CONTRAST (Custom only): around mid grey; 1 is the game's.",
      "TOTK_COLOR_CONTRAST（仅自定义）：以中灰为基准；1 为游戏原样。")
PC_TR("Saturation",
      "饱和度")
PC_TR("TOTK_COLOR_SATURATION (Custom only): 1 is the game's, 0.5 greyer, 2 twice as colourful.",
      "TOTK_COLOR_SATURATION（仅自定义）：1 为游戏原样，0.5 更灰，2 色彩加倍。")
PC_TR("Gamma",
      "伽马")
PC_TR("TOTK_COLOR_GAMMA (Custom only): below 1 brightens dark tones, above 1 darkens them; 1 is the game's.",
      "TOTK_COLOR_GAMMA（仅自定义）：小于 1 提亮暗部，大于 1 压暗；1 为游戏原样。")
PC_TR("Shadow map scale (renderer)",
      "阴影贴图比例（渲染器）")
PC_TR("TOTK_SHADOW_SCALE: the renderer's shadow targets. The game patches' Shadow resolution is separate. Applies at once.",
      "TOTK_SHADOW_SCALE：渲染器的阴影目标。与游戏补丁中的阴影分辨率相互独立。立即生效。")
PC_TR("1x (Switch)",
      "1x（Switch）")
PC_TR("2x (default)",
      "2x（默认）")
PC_TR("Soft shadows",
      "柔和阴影")
PC_TR("TOTK_SOFT_SHADOWS: the sun's shadow edges widen with the distance to what casts them (contact-hardening, PCSS) instead of keeping one width. Off is the game's look. A change rebuilds the shadowed shaders in the background: the old shadows show until they are ready.",
      "TOTK_SOFT_SHADOWS：太阳阴影的边缘会随与投影物距离变宽（接触硬化，PCSS），而不是保持同一宽度。关闭为游戏原样。更改会在后台重建带阴影的着色器：准备好之前仍显示旧阴影。")
PC_TR("Off (the game's)",
      "关闭（游戏原样）")
PC_TR("Soft",
      "柔和")
PC_TR("Softer",
      "更柔和")
PC_TR("Softest",
      "最柔和")
PC_TR("Contact shadows",
      "接触阴影")
PC_TR("TOTK_CONTACT_SHADOWS: small sun shadows at feet, under stones and in grass, which the shadow maps are too coarse for (a short march toward the sun through the scene's depth, up to 80 m). Off is the game's look; costs some GPU. Applies at once.",
      "TOTK_CONTACT_SHADOWS：脚下、石头下和草丛中的细小太阳阴影，阴影贴图精度不足以表现这些（沿场景深度朝太阳方向短距离步进，最远 80 m）。关闭为游戏原样；会占用一些 GPU。立即生效。")
PC_TR("Subtle",
      "细微")
PC_TR("Strong",
      "强烈")
PC_TR("Volume",
      "音量")
PC_TR("TOTK_VOLUME, master gain.",
      "TOTK_VOLUME，主音量。")
PC_TR("Mute in the background",
      "后台静音")
PC_TR("TOTK_MUTE_UNFOCUSED: no sound while the game window is not in front; the game runs on (Pause when the window loses focus stops it too).",
      "TOTK_MUTE_UNFOCUSED：游戏窗口不在前台时没有声音；游戏继续运行（“失去焦点时暂停”则会让游戏也停止）。")
PC_TR("Face buttons",
      "面键")
PC_TR("TOTK_PAD_LABELS.",
      "TOTK_PAD_LABELS。")
PC_TR("By position (Switch layout)",
      "按位置（Switch 布局）")
PC_TR("By label (Xbox A = A)",
      "按标签（Xbox A = A）")
PC_TR("Rumble",
      "震动")
PC_TR("TOTK_RUMBLE.",
      "TOTK_RUMBLE。")
PC_TR("Normal",
      "常规")
PC_TR("Motion controls",
      "体感操作")
PC_TR("TOTK_GYRO.",
      "TOTK_GYRO。")
PC_TR("Start",
      "开始")
PC_TR("Skip the opening",
      "跳过开场")
PC_TR("TOTK_SKIP_OPENING. F2 or Minus on the title screen also toggles it.",
      "TOTK_SKIP_OPENING。在标题画面按 F2 或－键也可切换。")
PC_TR("On (start at the Great Sky Island)",
      "开启（从初始空岛开始）")
PC_TR("Console mode",
      "主机模式")
PC_TR("TOTK_HANDHELD: handheld renders at the lower resolution.",
      "TOTK_HANDHELD：掌机模式以较低分辨率渲染。")
PC_TR("Docked",
      "底座模式")
PC_TR("Handheld",
      "掌机模式")
PC_TR("TOTK_KEYBOARD_LAYOUT.",
      "TOTK_KEYBOARD_LAYOUT。")
PC_TR("Classic",
      "经典")
PC_TR("PC (WASD)",
      "PC（WASD）")
PC_TR("Custom (TOTK_KBM_<ACTION>)",
      "自定义（TOTK_KBM_<ACTION>）")
PC_TR("Mouse buttons aim and shoot",
      "鼠标按键瞄准和射击")
PC_TR("TOTK_MOUSE_AIM.",
      "TOTK_MOUSE_AIM。")
PC_TR("TOTK_MOUSE_SENS.",
      "TOTK_MOUSE_SENS。")
PC_TR("Mouse counts for a full stick",
      "满摇杆所需鼠标计数")
PC_TR("TOTK_MOUSE_STICK_SPEED (per second).",
      "TOTK_MOUSE_STICK_SPEED（每秒）。")
PC_TR("TOTK_MOUSE_INVERT.",
      "TOTK_MOUSE_INVERT。")
PC_TR("TOTK_MOUSE_GYRO.",
      "TOTK_MOUSE_GYRO。")
PC_TR("While aiming",
      "瞄准时")
PC_TR("TOTK_MOUSE_GYRO_SPEED.",
      "TOTK_MOUSE_GYRO_SPEED。")
PC_TR("Anisotropic filtering",
      "各向异性过滤")
PC_TR("TOTK_ANISO: a floor under the game's. Applies at once.",
      "TOTK_ANISO：不低于游戏自带的值。立即生效。")
PC_TR("The game's",
      "游戏自带")
PC_TR("Texture LOD bias",
      "纹理 LOD 偏移")
PC_TR("TOTK_LOD_BIAS: below 0 is sharper. Applies at once.",
      "TOTK_LOD_BIAS：小于 0 更锐利。立即生效。")
PC_TR("Compact textures (low VRAM)",
      "紧凑纹理（显存不足）")
PC_TR("LARECOMP_ASTC_BC3: the game's textures kept in a quarter of the video memory (BC3), with slightly softer "
      "colour detail. Automatic: on only for graphics cards with 4 GB or less. Takes effect at the next start.",
      "LARECOMP_ASTC_BC3：游戏纹理只占四分之一的显存（BC3），细微的色彩细节略微变柔和。自动：仅在 4 GB 或更少显存的显卡上开启。下次启动时生效。")
PC_TR("Automatic (on with 4 GB of VRAM or less)",
      "自动（显存 4 GB 或更少时开启）")
PC_TR("For graphics cards with 4 GB of video memory or less: the textures take a quarter of the memory, so the game "
      "no longer runs out of it (stutters, blurry or late textures). Fine colour detail gets a little softer. "
      "Automatic turns it on only on such cards.",
      "适用于显存 4 GB 或更少的显卡：纹理只占四分之一的显存，游戏不再因显存不足而卡顿、纹理模糊或延迟出现。细微的色彩细节会略微变柔和。自动仅在这类显卡上开启。")
PC_TR("Scene upscaler",
      "场景放大器")
PC_TR("TOTK_SCENE_UPSCALE. Applies at once.",
      "TOTK_SCENE_UPSCALE。立即生效。")
PC_TR("TOTK_PAUSE_UNFOCUSED.",
      "TOTK_PAUSE_UNFOCUSED。")
PC_TR("Saves",
      "存档")
PC_TR("Save backups kept",
      "存档备份数量")
PC_TR("TOTK_SAVE_BACKUPS.",
      "TOTK_SAVE_BACKUPS。")
PC_TR("10 (default)",
      "10（默认）")
PC_TR("F12 screenshots",
      "F12 截图")
PC_TR("TOTK_PC_SHOT: PNGs in the Screenshots folder.",
      "TOTK_PC_SHOT：PNG 保存在 Screenshots 文件夹。")
PC_TR("Temperature unit",
      "温度单位")
PC_TR("TOTK_TEMP_UNIT: the temperature on the Purah Pad's map. Automatic: the game's (°F in English (US), °C in the other languages). The number changes at once, the °C / °F sign beside it the next time the map is opened.",
      "TOTK_TEMP_UNIT：普尔亚平板地图上的温度。自动：随游戏（English (US) 为 °F，其他语言为 °C）。数字立即改变，旁边的 °C / °F 符号在下次打开地图时改变。")
PC_TR("Automatic (by the game's language)",
      "自动（随游戏语言）")
PC_TR("°C (Celsius)",
      "°C（摄氏）")
PC_TR("°F (Fahrenheit)",
      "°F（华氏）")
PC_TR("Crash reports",
      "崩溃报告")
PC_TR("TOTK_PC_CRASH: a report and a minidump per crash.",
      "TOTK_PC_CRASH：每次崩溃生成一份报告和 minidump。")
PC_TR("Automatic updates",
      "自动更新")
PC_TR("Discord status",
      "Discord 状态")
PC_TR("Shows \"Playing\", the time played and a \"Join the community\" button (the game's subreddit) on your Discord profile while Discord runs. Nothing else is sent.",
      "Discord 运行时，在你的 Discord 个人资料上显示“正在玩”、本次游戏时长和“Join the community”按钮（本游戏的 Reddit 社区）。不会发送其他任何内容。")
PC_TR("Key bindings (Custom layout)",
      "按键绑定（自定义布局）")
PC_TR("TOTK_KBM_A. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_A。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_B. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_B。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_X. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_X。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_Y. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_Y。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_LSTICK. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_LSTICK。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_RSTICK. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_RSTICK。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_L. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_L。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_R. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_R。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_ZL. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_ZL。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_ZR. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_ZR。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_PLUS. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_PLUS。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_MINUS. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_MINUS。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_DPAD_LEFT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_DPAD_LEFT。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_DPAD_UP. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_DPAD_UP。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_DPAD_RIGHT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_DPAD_RIGHT。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_DPAD_DOWN. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_DPAD_DOWN。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_MOVE_LEFT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_MOVE_LEFT。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_MOVE_UP. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_MOVE_UP。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_MOVE_RIGHT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_MOVE_RIGHT。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_MOVE_DOWN. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_MOVE_DOWN。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_CAMERA_LEFT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_CAMERA_LEFT。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_CAMERA_UP. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_CAMERA_UP。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_CAMERA_RIGHT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_CAMERA_RIGHT。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_CAMERA_DOWN. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_CAMERA_DOWN。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("TOTK_KBM_WALK. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_WALK。用于主机方案，以及游戏中“游戏按键：主机布局”时。留空：使用 PC 布局的按键。")
PC_TR("Screenshot saved: %s",
      "截图已保存：%s")
PC_TR("Screenshot failed (%s)",
      "截图失败（%s）")
PC_TR("Screenshot skipped (still saving the previous ones)",
      "已跳过截图（仍在保存之前的截图）")
PC_TR("TOTK_WINDOW_MODE. F11 or Alt+Enter switch between windowed and the fullscreen kind chosen here.",
      "TOTK_WINDOW_MODE。F11 或 Alt+Enter 在窗口化与此处选择的全屏类型之间切换。")
PC_TR("Monitor",
      "显示器")
PC_TR("TOTK_MONITOR: where fullscreen goes and a windowed start opens. Remember: the monitor the game was on last (drag the window there, or pick it once), at the same window position.",
      "TOTK_MONITOR：全屏以及窗口化启动时所用的显示器。记住：游戏上次所在的显示器（把窗口拖过去，或选择一次），窗口位置相同。")
PC_TR("Exclusive resolution",
      "独占分辨率")
PC_TR("TOTK_EXCLUSIVE_SIZE: the monitor's mode in exclusive fullscreen; the list is the modes of the monitor the game started on. Borderless and windowed keep the desktop's.",
      "TOTK_EXCLUSIVE_SIZE：独占全屏时显示器的模式；列表为游戏启动时所在显示器的模式。无边框和窗口化沿用桌面的设置。")
PC_TR("Exclusive refresh rate",
      "独占刷新率")
PC_TR("TOTK_EXCLUSIVE_HZ: the refresh rate in exclusive fullscreen. A rate the resolution lacks gives the fastest it has under it.",
      "TOTK_EXCLUSIVE_HZ：独占全屏时的刷新率。分辨率不支持的刷新率，会采用其下可用的最高刷新率。")
PC_TR("Presentation",
      "呈现")
PC_TR("Vsync",
      "垂直同步")
PC_TR("Off: frames go out at once and may tear (LARECOMP_VK_PRESENT=immediate); on: mailbox, or FIFO with VRR.",
      "关闭：帧立即送出，可能出现撕裂（LARECOMP_VK_PRESENT=immediate）；开启：mailbox，配合 VRR 时为 FIFO。")
PC_TR("On (default, no tearing)",
      "开启（默认，无撕裂）")
PC_TR("Off (tearing, lowest latency)",
      "关闭（有撕裂，延迟最低）")
PC_TR("Variable refresh (VRR)",
      "可变刷新率（VRR）")
PC_TR("Only with a G-SYNC / FreeSync monitor (and, windowed, the driver's windowed VRR on): the panel follows each frame (FIFO). Without one it is plain vsync at a fixed refresh, and a cap under it repeats frames. Best with a frame cap a little under the refresh (116 at 120 Hz, 138 at 144 Hz); Unlocked and DLSS frame generation keep under it by themselves.",
      "仅适用于 G-SYNC / FreeSync 显示器（窗口化时还需开启驱动的窗口化 VRR）：屏幕随每一帧刷新（FIFO）。没有时就是固定刷新率下的普通垂直同步，低于刷新率的帧率上限会重复帧。最好将帧率上限设为略低于刷新率（120 Hz 时 116，144 Hz 时 138）；不限帧率和 DLSS 帧生成会自行保持在刷新率之下。")
PC_TR("Off (default)",
      "关闭（默认）")
PC_TR("On (G-Sync / FreeSync monitor)",
      "开启（G-Sync / FreeSync 显示器）")
PC_TR("Frame pacing",
      "帧节奏")
PC_TR("TOTK_PACE_AT: where the frame cap waits.",
      "TOTK_PACE_AT：帧率上限在何处等待。")
PC_TR("At the present (default, steadier)",
      "在呈现处（默认，更平稳）")
PC_TR("Before the frame (lower latency)",
      "在帧开始前（延迟更低）")
PC_TR("Game tick source",
      "游戏节拍来源")
PC_TR("TOTK_VSYNC: what the game's 60 Hz tick follows.",
      "TOTK_VSYNC：游戏的 60 Hz 时钟跟随什么。")
PC_TR("60 Hz clock (default)",
      "60 Hz 时钟（默认）")
PC_TR("The monitor's vblank (fixed refresh only)",
      "显示器垂直消隐（仅固定刷新率）")
PC_TR("Precise pacing",
      "精确节奏")
PC_TR("TOTK_PACE_SPIN_US: the cap's last part spins (CPU).",
      "TOTK_PACE_SPIN_US：帧率上限的最后一段采用忙等待（占用 CPU）。")
PC_TR("Queued frames",
      "排队帧数")
PC_TR("TOTK_MAX_QUEUED: frames waiting to be shown (needs present wait).",
      "TOTK_MAX_QUEUED：等待显示的帧数（需要 present wait）。")
PC_TR("No limit",
      "无限制")
PC_TR("2 (default)",
      "2（默认）")
PC_TR("GPU frames in flight",
      "GPU 在途帧数")
PC_TR("TOTK_FRAMES_IN_FLIGHT.",
      "TOTK_FRAMES_IN_FLIGHT。")
PC_TR("1 (lockstep: slower when CPU and GPU are both busy)",
      "1（锁步：CPU 和 GPU 都忙时更慢）")
PC_TR("Low latency",
      "低延迟")
PC_TR("TOTK_REFLEX: each frame is held back before it reads input, by as much as it would otherwise wait for the GPU, the render thread or the frame cap later. NVIDIA: Reflex's sleep (VK_NV_low_latency2); other GPUs, or jit: the engine's own. Boost keeps the GPU clocks up (Reflex).",
      "TOTK_REFLEX：每一帧在读取输入之前先被延后，延后的时长等于它之后因 GPU、渲染线程或帧率上限而要等待的时间。NVIDIA：使用 Reflex 的休眠（VK_NV_low_latency2）；其他 GPU 或 jit：使用引擎自带的。Boost 让 GPU 保持高频（Reflex）。")
PC_TR("On (NVIDIA Reflex; other GPUs: the engine's own)",
      "开启（NVIDIA Reflex；其他 GPU：引擎自带）")
PC_TR("On + boost",
      "开启 + Boost")
PC_TR("On (the engine's own, any GPU)",
      "开启（引擎自带，任意 GPU）")
PC_TR("Shaders",
      "着色器")
PC_TR("Shader wait",
      "着色器等待")
PC_TR("A frame that needs a shader still being built waits for it. Limit to 0.1 s: past that, the frame is shown and the objects whose shaders are not ready yet appear a moment later (fullscreen passes and compute always wait). Always wait: nothing is ever drawn incomplete, but a shader the first time can freeze the picture for a second or more. TOTK_HOLD_MAX_MS overrides.",
      "需要着色器而它仍在构建的帧会等待其完成。限制为 0.1 s：超过后先显示该帧，着色器未就绪的物体稍后才会出现（全屏通道和计算始终等待）。始终等待：绝不绘制不完整的画面，但着色器首次出现时可能让画面停顿一秒或更久。TOTK_HOLD_MAX_MS 可覆盖此项。")
PC_TR("Limit to 0.1 s (default)",
      "限制为 0.1 s（默认）")
PC_TR("Always wait (nothing missing)",
      "始终等待（无缺失）")
PC_TR("HDR output",
      "HDR 输出")
PC_TR("TOTK_HDR: needs Windows HDR on for the monitor.",
      "TOTK_HDR：需要为该显示器开启 Windows HDR。")
PC_TR("Off (SDR, default)",
      "关闭（SDR，默认）")
PC_TR("Peak brightness (nits)",
      "峰值亮度（nits）")
PC_TR("TOTK_HDR_PEAK: the display's peak.",
      "TOTK_HDR_PEAK：显示器的峰值亮度。")
PC_TR("Paper white (nits)",
      "纸白亮度（nits）")
PC_TR("TOTK_HDR_PAPER: the brightness of SDR white.",
      "TOTK_HDR_PAPER：SDR 白色的亮度。")
PC_TR("Highlight colour",
      "高光颜色")
PC_TR("TOTK_HDR_HUE: the hue of bright lights from the game's HDR scene, 0 to 100.",
      "TOTK_HDR_HUE：从游戏的 HDR 场景取回明亮光源的色调，0 到 100。")
PC_TR("Mouse cursor in menus",
      "菜单中的鼠标指针")
PC_TR("TOTK_MENU_CURSOR: in a menu the pointer shows and selects what it points at; left click is A (back is Esc).",
      "TOTK_MENU_CURSOR：在菜单中显示指针，并选中指针所指之处；左键为 A（返回为 Esc）。")
PC_TR("Minimum render resolution",
      "最低渲染分辨率")
PC_TR("100 %: the scene is always rendered at the render resolution (dynamic resolution off). 0 %: the game's own dynamic resolution. In between: dynamic, never below that fraction.",
      "100 %：场景始终以渲染分辨率渲染（动态分辨率关闭）。0 %：使用游戏自带的动态分辨率。介于两者之间：动态，但不低于该比例。")
PC_TR("Field of view",
      "视野")
PC_TR("Multiplier of the camera's field of view (1 = the game's). The degrees are the camera's view right now: aiming and some cutscenes narrow or widen it, and the multiplier keeps their proportion. Wider costs some GPU and CPU (more of the world on screen).",
      "镜头视野的倍数（1 = 游戏原值）。显示的角度是镜头当前的视野：瞄准和部分过场动画会收窄或拓宽视野，倍数会保持其比例。越宽会占用更多 GPU 和 CPU（屏幕上显示更多世界）。")
PC_TR("Shadow resolution",
      "阴影分辨率")
PC_TR("Multiplier of the shadow map's width and height (2 = sharper, 4x the memory).",
      "阴影贴图宽高的倍数（2 = 更清晰，内存为 4 倍）。")
PC_TR("Near shadow reach",
      "近处阴影范围")
PC_TR("Multiplier of how far the sun's sharpest shadow cascades reach (1 = the game's: the first two hand over 32 m and 64 m away, where the change in sharpness shows as a line, mostly at 4K). 2 moves those lines to 64 m and 128 m; the far shadows stay. The same shadow map then covers more ground, so near shadows get softer by that factor: raise Shadow resolution with it. Costs a little GPU and CPU (more objects drawn into the near shadow maps).",
      "太阳最清晰的几级阴影级联所覆盖距离的倍数（1 = 游戏原设：前两级在 32 米和 64 米处交接，清晰度的变化会显示为一条线，在 4K 下尤其明显）。2 会把这些线移到 64 米和 128 米；远处阴影不变。同一张阴影贴图因此覆盖更大的地面，近处阴影会按相同倍数变柔和：请同时提高阴影分辨率。会稍微增加 GPU 和 CPU 开销（近处阴影贴图中绘制的物体更多）。")
PC_TR("Sun shadow map size",
      "太阳阴影贴图大小")
PC_TR("The game's: the sun's shadow map shrinks and grows with the area its cascades cover, so near the ground foliage shadows turn coarse, and every size change moves the shadow texels (leaf shadows jump instead of sliding). Fixed: always the largest size the game picks (times the shadow resolution), steady and sharper; costs some GPU.",
      "游戏自带：太阳阴影贴图会随级联覆盖的范围缩放，所以近地面的植被阴影会变粗糙，且每次大小变化都会移动阴影纹素（叶影会跳动而不是平滑移动）。固定：始终使用游戏会选用的最大尺寸（乘以阴影分辨率），稳定且更清晰；会占用一些 GPU。")
PC_TR("Detail distance",
      "细节距离")
PC_TR("Multiplier of the distance at which models switch to lower detail (2 = full detail twice as far).",
      "模型切换到较低细节的距离的倍数（2 = 完整细节延伸到两倍远）。")
PC_TR("Actor detail distance",
      "角色细节距离")
PC_TR("Multiplier of the distances at which characters, enemies and objects start skipping update, animation and draw frames (2 = full rate twice as far; costs CPU).",
      "角色、敌人和物体开始跳过更新、动画和绘制帧的距离的倍数（2 = 完整帧率延伸到两倍远；占用 CPU）。")
PC_TR("Terrain detail distance",
      "地形细节距离")
PC_TR("Multiplier of the distances at which the ground and water switch to coarser levels (2 = full detail twice as far; costs CPU and GPU).",
      "地面和水面切换到较粗略层级的距离的倍数（2 = 完整细节延伸到两倍远；占用 CPU 和 GPU）。")
PC_TR("Simulation distance",
      "模拟距离")
PC_TR("Multiplier of the distance from the camera at which characters, enemies, objects and their physics stop being updated (1 = the game's; 1.5-2 = high). Costs CPU: more actors are simulated every frame, and distant ones keep reacting (fights, physics) instead of freezing.",
      "角色、敌人、物体及其物理停止更新时与镜头距离的倍数（1 = 游戏原值；1.5-2 = 高）。占用 CPU：每帧要模拟更多角色，远处的也会继续反应（战斗、物理），而不是静止。")
PC_TR("Mouse camera (direct)",
      "鼠标视角（直接）")
PC_TR("Multiplier of the camera turn the game itself applies per mouse count (on top of the mouse sensitivity). 0 = off: the mouse turns the camera through emulated right-stick deflection instead.",
      "游戏对每个鼠标计数自身施加的视角转动倍数（叠加在鼠标灵敏度之上）。0 = 关闭：鼠标改为通过模拟的右摇杆偏转来转动视角。")
PC_TR("Fixed (steady)",
      "固定（稳定）")
PC_TR("The game's (changes)",
      "游戏原样（会变化）")
PC_TR("Game patches",
      "游戏补丁")
PC_TR("Aspect ratio",
      "画面比例")
PC_TR("The window's: on a 3:2, 16:10, 21:9, 32:9 or 48:9 window or monitor the game draws a canvas of that shape at start (1920x1280, 1920x1200, 3440x1440, 5120x1440 or 7680x1440), the HUD laid out for it: 21:9, 32:9 and 48:9 a wider view, 3:2 and 16:10 a taller one (the same height of view, a little less at the sides). Other shapes, and 16:9, keep the game's 16:9 picture. 16:10 / 21:9 / 32:9 / 48:9 / 3:2: that canvas whatever the window (on another shape it is shown whole, with bars; 21:9, 32:9 and 48:9 are heavier than the window's own shape: the whole canvas is drawn, then shown smaller). Needs the game patches' RomFS sets (TOTK_PATCH_SETS) and the overlay's layout for that shape. At the next start: the canvas and the game files laid out for it are chosen as the game starts. Setup prepares the layouts of this PC's monitors' shapes; another shape's are made at the start after you pick it (about 30 s, once).",
      "跟随窗口：窗口或显示器为 3:2、16:10、21:9、32:9 或 48:9 时，游戏启动时会绘制相应形状的画布（1920x1280、1920x1200、3440x1440、5120x1440 或 7680x1440），HUD 也按其布局：21:9、32:9 和 48:9 视野更宽，3:2 和 16:10 更高（视野高度相同，两侧略少）。其他形状和 16:9 保持游戏的 16:9 画面。16:10 / 21:9 / 32:9 / 48:9 / 3:2：无论窗口如何都使用该画布（形状不同时完整显示，带黑边；21:9、32:9 和 48:9 比窗口自身形状更吃性能：先绘制整个画布，再缩小显示）。需要游戏补丁的 RomFS 集合（TOTK_PATCH_SETS）以及覆盖层对应形状的布局。下次启动时：游戏启动时选定画布及为其排布的游戏文件。Setup 会为这台电脑的显示器形状准备布局；其他形状的布局在你选择后的下次启动时生成（约 30 秒，仅一次）。")
PC_TR("HUD position (21:9, 32:9, 48:9)",
      "HUD 位置（21:9、32:9、48:9）")
PC_TR("Where the HUD goes on a 21:9, 32:9 or 48:9 canvas: at the screen's edges, or in the middle 16:9 (closer to the eyes on a very wide screen). Menus are the same either way. Nothing changes at 16:9. At the next start: the game loads its interface layouts once, as it starts. Automatic (the default): in the middle on 48:9, at the edges otherwise.",
      "21:9、32:9 或 48:9 画布上 HUD 的位置：位于屏幕边缘，或位于中间的 16:9 区域（超宽屏上离视线更近）。菜单在两种方式下相同。16:9 时没有变化。下次启动时：游戏在启动时加载一次界面布局。自动（默认）：48:9 时位于中间，其他情况位于边缘。")
PC_TR("Texture cache",
      "纹理缓存")
PC_TR("Multiplier of the game's texture cache (fewer blurry textures when turning back; 490 MiB more per step). Needs the game patches' RomFS sets (TOTK_PATCH_SETS). At the next start: the game sizes its memory once, as it starts.",
      "游戏纹理缓存的倍数（回头转身时模糊纹理更少；每级多占 490 MiB）。需要游戏补丁的 RomFS 集合（TOTK_PATCH_SETS）。下次启动时：游戏在启动时一次性分配内存。")
PC_TR("Grass density",
      "草地密度")
PC_TR("Multiplier of grass, flowers and pebbles. Needs the game patches' RomFS sets (TOTK_PATCH_SETS). At the next start: the game reads its grass table once, as it starts.",
      "草、花和卵石数量的倍数。需要游戏补丁的 RomFS 集合（TOTK_PATCH_SETS）。下次启动时：游戏在启动时读取一次草的数据表。")
PC_TR("Shadow distance",
      "阴影距离")
PC_TR("Multiplier of the sharp shadow cascades' distances. Needs the game patches' RomFS sets (TOTK_PATCH_SETS). At the next start: the value is in the shader package, which the game reads once, as it starts.",
      "清晰阴影级联距离的倍数。需要游戏补丁的 RomFS 集合（TOTK_PATCH_SETS）。下次启动时：该值位于着色器包中，游戏在启动时读取一次。")
PC_TR("Fog distance",
      "雾距离")
PC_TR("Multiplier of the distance fog's start and end (clearer distant mountains). Needs the game patches' RomFS sets (TOTK_PATCH_SETS). At the next start: the game reads its sky and fog palettes once, as it starts.",
      "距离雾起点和终点的倍数（远处的山更清晰）。需要游戏补丁的 RomFS 集合（TOTK_PATCH_SETS）。下次启动时：游戏在启动时读取一次天空和雾的色板。")
PC_TR("Fast-travel speed",
      "快速旅行速度")
PC_TR("Fast: Link's departure when travelling is about 2.5 s shorter (its long waits halved, his dissolve as on the Switch); Normal is the game's own. Needs the game patches' RomFS sets (TOTK_PATCH_SETS). At the next start: the game loads the travel scene the first time you travel and keeps it.",
      "快速：旅行时林克的出发过程约缩短 2.5 s（长时间等待减半，其消散效果与 Switch 上相同）；常规为游戏原样。需要游戏补丁的 RomFS 集合（TOTK_PATCH_SETS）。下次启动时：游戏在你第一次旅行时加载旅行场景并保留。")
PC_TR("Cutscenes (21:9, 32:9, 48:9)",
      "过场动画（21:9、32:9、48:9）")
PC_TR("On a 21:9, 32:9 or 48:9 canvas, real-time cutscenes and conversations: wide like the rest of the game, or the original 16:9 framing with bars at the sides (for a scene that looks odd at the edges). Pre-rendered movies are 16:9 video: always shown in the middle with bars.",
      "在 21:9、32:9 或 48:9 画布上，实时过场动画和对话：与游戏其余部分一样宽，或保持原始 16:9 画面、两侧加黑边（用于边缘显得奇怪的场景）。预渲染影片是 16:9 视频：始终居中显示，带黑边。")
PC_TR("Upscales the 3D scene with a temporal upscaler that builds detail over several frames. DLSS: NVIDIA RTX only; DLAA: DLSS at full resolution, the best image for more GPU. FSR and XeSS: any GPU (XeSS runs fastest on Intel Arc). All cost some GPU; Off is the game's own.",
      "使用时间性放大技术放大 3D 场景，在多帧之间累积细节。DLSS：仅限 NVIDIA RTX；DLAA：全分辨率的 DLSS，画质最佳但更耗 GPU。FSR 和 XeSS：任意 GPU（XeSS 在 Intel Arc 上最快）。都会占用一些 GPU；关闭则为游戏自带的方式。")
PC_TR("Safe mode (if the game crashes)",
      "安全模式（游戏崩溃时）")
PC_TR("For a game that crashes or hangs: the renderer without its faster paths, DLSS, FSR and frame generation, and without the driver paths some GPUs crash in. Slower. At the next start.",
      "适用于崩溃或卡死的游戏：渲染器关闭较快的路径、DLSS、FSR 和帧生成，并避开部分 GPU 会崩溃的驱动路径。速度更慢。下次启动时生效。")
PC_TR("Wide picture: F10 > Restart the game now to use %s",
      "宽画面：F10 > 立即重启游戏 以使用 %s")
PC_TR("The window changed shape: F10 > Restart the game now to fit it",
      "窗口形状已改变：F10 > 立即重启游戏 以适配窗口")
PC_TR("Screen shape: start the game once from TotK Launcher.exe to prepare its layout (16:9 until then)",
      "屏幕形状：请从 TotK Launcher.exe 启动一次游戏以准备其界面布局（在此之前保持 16:9）")
PC_TR("This screen shape's layout was not prepared: 16:9 until Setup runs again",
      "此屏幕形状的界面布局未能准备好：在重新运行 Setup 之前保持 16:9")
PC_TR("Safe mode is on: DLSS, FSR and frame generation stay off",
      "安全模式已开启：DLSS、FSR 和帧生成保持关闭")
PC_TR("Frame generation mode",
      "帧生成模式")
PC_TR("Smooth (matched to the refresh)",
      "平滑（匹配刷新率）")
PC_TR("Maximum fps",
      "最高帧率")
PC_TR("TOTK_FG_PACE. Smooth: the frames shown match the monitor (real ones at the cap divided by the multiplier). Maximum fps: the real frames as without frame generation and the multiplier's generated ones after each, past the refresh rate, with VRR too (mailbox, else immediate, which can tear; the panel may leave VRR; TOTK_FG_MAX_VRR_FIFO=1 keeps VRR's FIFO).",
      "TOTK_FG_PACE。平滑：显示的帧与显示器同步（真实帧为帧率上限除以倍数）。最高帧率：真实帧与不开帧生成时相同，每个真实帧后加上倍数对应的生成帧，可超过刷新率，开启 VRR 时也是如此（使用 mailbox，没有时用 immediate，可能出现画面撕裂；显示器可能退出 VRR；TOTK_FG_MAX_VRR_FIFO=1 保留 VRR 的 FIFO）。")
PC_TR("Smooth keeps the frames shown in step with the monitor's refresh. Maximum fps adds the generated frames to every real frame the PC makes, even past the refresh rate: more frames than the screen shows (the fps readout counts them all), for VRR panels or low latency; Smooth looks steadier on a fixed refresh. With V-sync off the picture can tear.",
      "平滑让显示的帧与显示器刷新同步。最高帧率把生成帧加到电脑生成的每个真实帧上，即使超过刷新率：帧数多于屏幕能显示的（帧率显示会全部计入），适合 VRR 显示器或低延迟；固定刷新率下平滑看起来更稳。关闭垂直同步时画面可能撕裂。")
PC_TR("TOTK_DLSS_SR / TOTK_FSR_SR / TOTK_XESS_SR. DLSS (NVIDIA RTX), FSR or XeSS (any GPU) upscales the 3D scene to the output in place of the game's upscaler and its FXAA (DLAA / native AA when Render resolution matches the output).",
      "TOTK_DLSS_SR / TOTK_FSR_SR / TOTK_XESS_SR。DLSS（NVIDIA RTX）、FSR 或 XeSS（任意 GPU）代替游戏自带的放大和 FXAA，将 3D 场景放大到输出分辨率（渲染分辨率与输出相同时为 DLAA / 原生抗锯齿）。")
PC_TR("FSR (any GPU)",
      "FSR（任意 GPU）")
PC_TR("TOTK_DLSS_FG / TOTK_FSR_FG. Generated frames between real ones: DLSS x2 on NVIDIA RTX 40 series and newer, x3 and x4 on RTX 50; FSR x2 on any GPU. The frame cap counts the frames shown (at a 30 cap: x2, 60 shown). Adds some input latency.",
      "TOTK_DLSS_FG / TOTK_FSR_FG。在真实帧之间生成帧：DLSS x2 需 NVIDIA RTX 40 系列及更新，x3 和 x4 需 RTX 50；FSR x2 适用任意 GPU。帧率上限按显示的帧数计算（上限 30 时：x2，显示 60）。会增加一些输入延迟。")
PC_TR("HDR",
      "HDR")
PC_TR("0.1% low",
      "0.1% 低帧")
PC_TR("game",
      "游戏")
PC_TR("DLSS model",
      "DLSS 模型")
PC_TR("NVIDIA's default",
      "NVIDIA 默认")
PC_TR("Preset K",
      "预设 K")
PC_TR("Preset J",
      "预设 J")
PC_TR("TOTK_DLSS_PRESET. The AI model DLSS and DLAA use: NVIDIA's default for each quality, or preset K (sharpest) or J (a little less ghosting, a little more flicker) for all.",
      "TOTK_DLSS_PRESET。DLSS 和 DLAA 使用的 AI 模型：每种质量用 NVIDIA 默认，或全部使用预设 K（最清晰）或 J（拖影稍少，闪烁稍多）。")
PC_TR("DLSS character trails",
      "DLSS 角色拖影")
PC_TR("Fewest trails",
      "拖影最少")
PC_TR("Between",
      "适中")
PC_TR("Smoothest (more trails)",
      "最平滑（拖影较多）")
PC_TR("TOTK_DLSS_TRAILS. Where something moved (Link, animals, a swung weapon), how much DLSS and DLAA trust the new frame over the old ones: fewest trails, or smoother edges with more trails.",
      "TOTK_DLSS_TRAILS。在有物体移动的地方（林克、动物、挥动的武器），DLSS 和 DLAA 对新帧相对旧帧的信任程度：拖影最少，或边缘更平滑但拖影更多。")
PC_TR("Which AI model DLSS and DLAA use. NVIDIA's default picks one for each quality. Preset K is the sharpest; preset J ghosts a little less behind moving things but flickers a little more. Changing it rebuilds DLSS once (a short pause).",
      "DLSS 和 DLAA 使用哪个 AI 模型。NVIDIA 默认会为每种质量选择一个。预设 K 最清晰；预设 J 在移动物体后的拖影稍少，但闪烁稍多。更改后 DLSS 会重建一次（短暂停顿）。")
PC_TR("Where something moved (Link, animals, a swung weapon), DLSS and DLAA can keep blending in older frames, which leaves faint trails, or trust the new frame, which removes them but can make those edges flicker a little. Fewest trails is the default.",
      "在有物体移动的地方（林克、动物、挥动的武器），DLSS 和 DLAA 可以继续混合旧帧，这会留下淡淡的拖影；也可以信任新帧，这能去除拖影，但这些边缘可能会轻微闪烁。默认为“拖影最少”。")
PC_TR("Safe GPU mode: the graphics driver crashed in the last runs, so shaders now build the slow, safe way",
      "GPU 安全模式：图形驱动在最近几次运行中崩溃，因此着色器现在以较慢的安全方式编译")
PC_TR("Safe GPU mode: the graphics driver kept crashing, so shaders are also built unoptimised; the game may run slower until it has run cleanly a few times",
      "GPU 安全模式：图形驱动持续崩溃，因此着色器也将以未优化方式编译；在游戏正常运行几次之前，可能会变慢")
PC_TR("XeSS (any GPU)",
      "XeSS（任意 GPU）")
PC_TR("Shader compile indicator",
      "着色器编译提示")
PC_TR("Bottom right while shaders compile: the percentage on a first start, how many are left in play.",
      "着色器编译时显示在右下角：首次启动时显示百分比，游戏中显示剩余数量。")
PC_TR("Shows when shaders are being compiled (bottom right): a short stutter while it is up is the compile, not your PC. Off hides it in play (a first start still shows its progress); the compiling still happens.",
      "在着色器编译时显示（右下角）：此时出现的短暂卡顿来自编译，而不是你的电脑。关闭后在游戏中隐藏提示（首次启动仍会显示进度），编译仍会进行。")
PC_TR("Shaders left to compile:",
      "待编译着色器：")
PC_TR("Last hitch",
      "上次卡顿")
PC_TR("No hitch yet",
      "暂无卡顿")
PC_TR("GPU wait",
      "等待 GPU")
PC_TR("shader builds",
      "着色器编译")
PC_TR("texture uploads",
      "纹理上传")
PC_TR("new images",
      "新建图像")
PC_TR("file reads",
      "文件读取")
PC_TR("present (driver / window)",
      "呈现（驱动 / 窗口）")
PC_TR("game CPU",
      "游戏 CPU")
PC_TR("render CPU",
      "渲染 CPU")
PC_TR("s ago",
      "秒前")
PC_TR("Upscaler:",
      "超分辨率：")
PC_TR("off",
      "关闭")
PC_TR("plain scaling",
      "普通缩放")
PC_TR("native",
      "原生")
PC_TR("Quality",
      "质量")
PC_TR("Balanced",
      "平衡")
PC_TR("Performance",
      "性能")
PC_TR("Ultra Performance",
      "超级性能")
PC_TR("idle",
      "未运行")
PC_TR("3D output is",
      "3D 输出为")
PC_TR("scene",
      "场景")
PC_TR("it failed to start",
      "启动失败")
PC_TR("starting",
      "正在启动")
PC_TR("no 3D view this frame",
      "本帧无 3D 画面")
PC_TR("no scene seen",
      "未见场景")
