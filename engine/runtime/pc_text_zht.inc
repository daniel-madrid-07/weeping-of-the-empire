// pc_text_zht.inc: the Traditional Chinese (Taiwan) of the PC port's own texts (pc_text.cpp, pc_ui_lang
// "zh-Hant"), one PC_TR(english, translation) each: pc_text_zh.inc's in Taiwan's words (視窗, 螢幕, 解析度,
// 影格率, 滑鼠, 選單, 預設) and the game's TWzh terms. A text missing here shows in English; checked by
// tools/engine/check_texts.py (the same printf specifiers, no entry the sources lack). lang-ptbr 11-04.
PC_TR("Audio",
      "音訊")
PC_TR("Output",
      "輸出")
PC_TR("Speakers",
      "揚聲器")
PC_TR("TOTK_AUDIO_CHANNELS. 5.1 and 7.1 tell the game it has surround output (7.1 puts the rears on the back and side pairs); Windows remaps to the device. Experimental: with 6 channels the game's effect on one sub mix diverges and is muted.",
      "TOTK_AUDIO_CHANNELS。5.1 和 7.1 會告知遊戲當前為環繞聲輸出（7.1 將後置聲道分為後方和側方兩對）；由 Windows 重新對應到裝置。實驗性：6 聲道時，遊戲中某個子混音的效果會失真，因此被靜音。")
PC_TR("Stereo (default)",
      "立體聲（預設）")
PC_TR("Match the Windows device (experimental)",
      "匹配 Windows 裝置（實驗性）")
PC_TR("5.1 (experimental)",
      "5.1（實驗性）")
PC_TR("7.1 (experimental)",
      "7.1（實驗性）")
PC_TR("Latency",
      "延遲")
PC_TR("TOTK_AUDIO_LATENCY_MS: audio queued ahead of the device. Lower is more responsive; raise it if the sound crackles.",
      "TOTK_AUDIO_LATENCY_MS：提前排入裝置的音訊量。越低反應越快；聲音有爆音時請調高。")
PC_TR("20 ms (default)",
      "20 ms（預設）")
PC_TR("80 ms (wireless headsets)",
      "80 ms（無線耳機）")
PC_TR("Follow the Windows device",
      "跟隨 Windows 裝置")
PC_TR("TOTK_AUDIO_FOLLOW: switch to the new default device (headphones plugged in or out) and recover a lost one without a restart.",
      "TOTK_AUDIO_FOLLOW：切換到新的預設裝置（插拔耳機時），並在無需重啟的情況下恢復丟失的裝置。")
PC_TR("On (default)",
      "開啟（預設）")
PC_TR("Off",
      "關閉")
PC_TR("Mix",
      "混音")
PC_TR("Music",
      "音樂")
PC_TR("TOTK_AUDIO_MUSIC.",
      "TOTK_AUDIO_MUSIC。")
PC_TR("Effects",
      "音效")
PC_TR("TOTK_AUDIO_SFX.",
      "TOTK_AUDIO_SFX。")
PC_TR("Voice",
      "語音")
PC_TR("TOTK_AUDIO_VOICE. No sub mix carries voice alone yet, so by default this does nothing.",
      "TOTK_AUDIO_VOICE。目前沒有單獨承載語音的子混音，因此預設不起作用。")
PC_TR("the stick now",
      "搖桿當前")
PC_TR("Controls",
      "操控")
PC_TR("Keyboard layout",
      "鍵盤配置")
PC_TR("For the console scheme, and with the PC scheme for the world while Gameplay keys is Console (menus use the PC keys). Classic: Space jump, Shift sprint, E interact, F attack, WASD, arrows camera. PC: E interact, Shift sprint, Space jump, left mouse attack, right mouse bow, Q target, F abilities. Custom: the PC keys changed by the bindings below.",
      "用於主機方案；遊戲按鍵為主機佈局時，世界中也可配合 PC 方案使用（選單使用 PC 按鍵）。經典：空格跳躍、Shift 衝刺、E 互動、F 攻擊、WASD 移動、方向鍵控制視角。PC：E 互動、Shift 衝刺、空格跳躍、滑鼠左鍵攻擊、滑鼠右鍵拉弓、Q 注視、F 能力。自訂：經下方按鍵繫結修改的 PC 按鍵。")
PC_TR("Classic (the engine's keys)",
      "經典（引擎按鍵）")
PC_TR("PC (WASD, mouse)",
      "PC（WASD、滑鼠）")
PC_TR("Custom (the bindings below)",
      "自訂（下方按鍵繫結）")
PC_TR("Minimum movement key press",
      "移動鍵最短按下時間")
PC_TR("Milliseconds a quick tap of a movement key (WASD) keeps Link moving, so he takes a step instead of sliding with his legs still. 0 = off (a tap as short as the key press).",
      "輕點移動鍵（WASD）後，林克持續移動的毫秒數，使其邁出一步而不是腿不動地滑行。0 = 關閉（與按鍵時長一致）。")
PC_TR("Sprint toggle",
      "衝刺切換")
PC_TR("A press of the sprint key starts sprinting and the next one stops it; it also stops when Link stops moving or a menu opens. Off: hold it, as on the Switch.",
      "按一下衝刺鍵開始衝刺，再按一下停止；林克停止移動或開啟選單時也會停止。關閉：按住衝刺，與 Switch 相同。")
PC_TR("On",
      "開啟")
PC_TR("Shield and target toggle",
      "盾牌與注視切換")
PC_TR("A press of the shield / target key raises the shield and targets, the next press lowers it; a menu lets it go too. Off: hold it, as on the Switch.",
      "按一下盾牌 / 注視鍵舉盾並注視目標，再按一下放下；開啟選單時也會鬆開。關閉：按住，與 Switch 相同。")
PC_TR("Mouse camera",
      "滑鼠視角")
PC_TR("The mouse turns the camera (click the window to capture the pointer; Esc, F10 or Alt+Tab frees it). 0 = off; above 1 turns faster.",
      "滑鼠控制視角（點選視窗以捕獲指標；Esc、F10 或 Alt+Tab 可釋放）。0 = 關閉；大於 1 轉得更快。")
PC_TR("Pad",
      "控制器")
PC_TR("Button prompts",
      "按鍵提示")
PC_TR("The button pictures in every prompt and menu, for the keys and buttons bound. Automatic follows the device used last: keyboard and mouse, an Xbox (XInput) pad, a DualSense / DualShock 4, or the game's own with a Nintendo pad. A change shows at once, in every prompt and label; with a pad in use, menus hide the mouse pointer until the mouse moves.",
      "所有提示和選單中的按鍵圖示，對應已繫結的按鍵和按鈕。自動：跟隨最近使用的裝置，即鍵盤和滑鼠、Xbox（XInput）控制器、DualSense / DualShock 4，或使用 Nintendo 控制器時的遊戲自帶圖示。更改立即在所有提示和標籤中生效；使用控制器時，選單會隱藏滑鼠指標，直到滑鼠移動。")
PC_TR("Automatic (the device used last)",
      "自動（最近使用的裝置）")
PC_TR("Menu buttons",
      "選單按鍵")
PC_TR("Same as the game",
      "與遊戲相同")
PC_TR("Printed A accepts, B goes back",
      "印有 A 的鍵確認，B 返回")
PC_TR("Switch layout (right accepts, bottom goes back)",
      "Switch 配置（右鍵確認，下鍵返回）")
PC_TR("The pad's four face buttons in menus, dialogues and the title screen. Same as the game: as Face buttons and the Pad buttons rows give them. Printed A accepts, B goes back: the button printed A (Cross) confirms and B (Circle) goes back, whatever jump and interact use in the world. Switch layout: the right button confirms, the bottom one goes back. Nothing changes in the world. A change applies at once.",
      "選單、對話和標題畫面中控制器的四個面鍵。與遊戲相同：依「面鍵」和「控制器按鈕」各列的設定。印有 A 的鍵確認，B 返回：印有 A（叉）的鍵確認，印有 B（圈）的鍵返回，不論世界中跳躍和互動用哪個鍵。Switch 配置：右鍵確認，下鍵返回。世界中的操作不變。變更立即生效。")
PC_TR("Switch (the game's)",
      "Switch（遊戲自帶）")
PC_TR("Keyboard and mouse",
      "鍵盤和滑鼠")
PC_TR("Left mouse shoots while aiming",
      "瞄準時滑鼠左鍵射擊")
PC_TR("With the bow drawn (a ZR key or button held), the left button shoots instead of attacking.",
      "拉弓時（按住 ZR 鍵或按鈕），滑鼠左鍵用於射擊而非攻擊。")
PC_TR("Mouse look version",
      "滑鼠視角模式")
PC_TR("Stick",
      "搖桿")
PC_TR("Direct",
      "直接")
PC_TR("Mouse sensitivity",
      "滑鼠靈敏度")
PC_TR("Scales the camera and the motion aim.",
      "縮放視角和體感瞄準的靈敏度。")
PC_TR("Mouse speed for a full camera turn",
      "滿速轉動視角的滑鼠速度")
PC_TR("Mouse counts per second that turn the camera at full stick speed.",
      "每秒多少滑鼠計數可使視角以搖桿滿速轉動。")
PC_TR("Invert mouse vertical",
      "反轉滑鼠垂直方向")
PC_TR("Mouse as motion aim",
      "滑鼠作體感瞄準")
PC_TR("While aiming, the mouse turns a virtual controller, so the game's motion aiming gets the mouse's precision (when no pad with motion is connected).",
      "瞄準時，滑鼠轉動一個虛擬控制器，使遊戲的體感瞄準獲得滑鼠的精度（未連線帶體感的控制器時）。")
PC_TR("While aiming (bow, throw)",
      "瞄準時（弓、投擲）")
PC_TR("Always",
      "始終")
PC_TR("Motion aim speed",
      "體感瞄準速度")
PC_TR("Aiming sensitivity (bow)",
      "瞄準靈敏度（弓）")
PC_TR("With the bow drawn: the mouse's turn times this (1 = as when not aiming), with or without the mouse as motion aim. A pad's motion sensitivity is separate.",
      "拉弓時：滑鼠轉動量乘以此值（1 = 與未瞄準時相同），無論是否將滑鼠用作體感瞄準。控制器的體感靈敏度另行設定。")
PC_TR("Mouse wheel scrolls the quick menus",
      "滑鼠滾輪滾動快捷選單")
PC_TR("While a D-pad quick menu is held open (weapons, shields, items, bows), each notch moves one entry.",
      "按住十字鍵開啟快捷選單時（武器、盾牌、道具、弓），滾輪每滾一格移動一項。")
PC_TR("Map with the mouse",
      "滑鼠操作地圖")
PC_TR("On the map the mouse moves the map under the reticle (it stays at the centre) as far as the pointer would move, the wheel zooms, a left click places a pin or stamp and a right click goes back.",
      "在地圖上，滑鼠在準星（保持在中央）下方移動地圖，移動距離與滑鼠指標相同，滾輪縮放，左鍵放置圖釘或印章，右鍵返回。")
PC_TR("Map cursor sensitivity",
      "地圖游標靈敏度")
PC_TR("How far the map moves for the mouse's motion: 1 = as far as the desktop pointer would move on the screen, 2 = twice as far.",
      "滑鼠移動時地圖移動的距離：1 = 與桌面指標在螢幕上移動的距離相同，2 = 兩倍。")
PC_TR("How far the map moves under the reticle for the mouse's motion: 1 = like the desktop pointer, 2 = twice as far.",
      "滑鼠移動時地圖在準星下方移動的距離：1 = 與桌面指標相同，2 = 兩倍。")
PC_TR("Menu keys",
      "選單按鍵")
PC_TR("In the pause menu and the Purah Pad: Esc goes back, PageUp / PageDown (and the wheel in the pause menu) switch tabs, and in the pause menu clicks select (left) and go back (right).",
      "在暫停選單和普爾亞平板中：Esc 返回，PageUp / PageDown（暫停選單中也可用滾輪）切換標籤頁，暫停選單中按一下左鍵選擇、右鍵返回。")
PC_TR("Rumble curve",
      "震動曲線")
PC_TR("How light rumbles feel next to strong ones: below 1 light rumbles come through stronger, above 1 only strong ones; 1 = the game's own.",
      "輕微震動相對強烈震動的感受：低於 1 時輕微震動更明顯，高於 1 時只保留強烈震動；1 = 遊戲原本的效果。")
PC_TR("Trigger rumble (Xbox)",
      "扳機震動（Xbox）")
PC_TR("An Xbox One or Series pad's trigger motors buzz with the game's rumble, each trigger with its side's. Off by default: the Switch pad has no trigger motors.",
      "Xbox One 或 Series 控制器的扳機馬達隨遊戲震動而震動，每個扳機對應其所在一側。預設關閉：Switch 控制器的扳機沒有馬達。")
PC_TR("Light",
      "輕")
PC_TR("Full strength",
      "最強")
PC_TR("Camera speed (right stick)",
      "鏡頭速度（右搖桿）")
PC_TR("The camera's turn with the right stick (and the camera keys) times this, at each of the game's own camera speeds, top speed included; the mouse keeps its own sensitivity. At the next start.",
      "用右搖桿（以及鏡頭按鍵）轉動鏡頭的速度乘以此值，適用於遊戲自帶的每一檔鏡頭速度，包括最高速度；滑鼠保持其自身的靈敏度。下次啟動時生效。")
PC_TR("Rumble strength",
      "震動強度")
PC_TR("The pad's motors (Xbox, PlayStation and Switch pads): 0 = off, 1 = the game's own.",
      "控制器的馬達（Xbox、PlayStation 和 Switch 控制器）：0 = 關閉，1 = 遊戲自帶強度。")
PC_TR("Motion (gyro)",
      "體感（陀螺儀）")
PC_TR("The motion sensors of a DualSense, DualShock 4, Pro Controller or Joy-Con, for the game's motion aiming (the game's own Motion Controls option turns it on there). Off: the pad lies still for the game.",
      "DualSense、DualShock 4、Pro 控制器或 Joy-Con 的體感感測器，用於遊戲的體感瞄準（遊戲自帶的「體感操作」選項會在其中開啟）。關閉：對遊戲而言控制器靜止不動。")
PC_TR("Motion sensitivity",
      "體感靈敏度")
PC_TR("How far the aim turns for a turn of the pad: 1 = the pad's own, 2 = twice as far.",
      "瞄準隨控制器轉動而轉動的幅度：1 = 控制器自帶，2 = 兩倍。")
PC_TR("Left stick dead zone",
      "左搖桿死區")
PC_TR("How far the left stick must move before it counts, in percent of its travel. Raise it if Link moves with the stick at rest (a worn stick); lower it for finer small steps. 0 = the pad's own (Xbox 24, PlayStation 15, Switch 12).",
      "左搖桿需要移動多遠才會生效，以行程的百分比計。若搖桿靜止時林克仍在移動（搖桿磨損），請調高；想要更精細的小步移動，請調低。0 = 控制器自帶（Xbox 24，PlayStation 15，Switch 12）。")
PC_TR("Right stick dead zone",
      "右搖桿死區")
PC_TR("How far the right stick must move before it counts, in percent of its travel. Raise it if the camera turns with the stick at rest; lower it for finer aiming. 0 = the pad's own (Xbox 27, PlayStation 15, Switch 12).",
      "右搖桿需要移動多遠才會生效，以行程的百分比計。若搖桿靜止時視角仍在轉動，請調高；想要更精細的瞄準，請調低。0 = 控制器自帶（Xbox 27，PlayStation 15，Switch 12）。")
PC_TR("Walk speed",
      "步行速度")
PC_TR("With the walk key held (Alt in the PC layout): how far the movement keys push the stick, in its travel (1 = a full push, as without the key).",
      "按住步行鍵時（PC 佈局中為 Alt）：移動鍵推動搖桿的幅度（1 = 推到底，與不按該鍵相同）。")
PC_TR("None",
      "無")
PC_TR("Left stick click",
      "左搖桿按下")
PC_TR("Right stick click",
      "右搖桿按下")
PC_TR("D-pad up",
      "十字鍵上")
PC_TR("D-pad down",
      "十字鍵下")
PC_TR("D-pad left",
      "十字鍵左")
PC_TR("D-pad right",
      "十字鍵右")
PC_TR("Back paddle P1 (right, top)",
      "背鍵 P1（右上）")
PC_TR("The Switch button this back paddle presses: an Xbox Elite's paddles (unless the Xbox Accessories app maps them to buttons), a DualSense Edge's back buttons (the right one).",
      "此背鍵按下的 Switch 按鈕：Xbox Elite 的背鍵（除非 Xbox 配件應用將其對應為按鈕），DualSense Edge 的背鍵（右側那個）。")
PC_TR("Back paddle P2 (right, bottom)",
      "背鍵 P2（右下）")
PC_TR("The Switch button this back paddle presses: an Xbox Elite's paddles (unless the Xbox Accessories app maps them to buttons), a DualSense Edge's back buttons (none: P1 and P3 only).",
      "此背鍵按下的 Switch 按鈕：Xbox Elite 的背鍵（除非 Xbox 配件應用將其對應為按鈕），DualSense Edge 的背鍵（無：僅 P1 和 P3）。")
PC_TR("Back paddle P3 (left, top)",
      "背鍵 P3（左上）")
PC_TR("The Switch button this back paddle presses: an Xbox Elite's paddles (unless the Xbox Accessories app maps them to buttons), a DualSense Edge's back buttons (the left one).",
      "此背鍵按下的 Switch 按鈕：Xbox Elite 的背鍵（除非 Xbox 配件應用將其對應為按鈕），DualSense Edge 的背鍵（左側那個）。")
PC_TR("Back paddle P4 (left, bottom)",
      "背鍵 P4（左下）")
PC_TR("Game",
      "遊戲")
PC_TR("Window",
      "視窗")
PC_TR("Pause when the window loses focus",
      "失去焦點時暫停")
PC_TR("TOTK_PAUSE_UNFOCUSED: while the game window is not in front the game stops, picture, sound and its clock, and goes on where it was (hle_oe.cpp oe_host_paused).",
      "TOTK_PAUSE_UNFOCUSED：遊戲視窗不在前景時，遊戲會停止（畫面、聲音和計時），並從原處繼續（hle_oe.cpp oe_host_paused）。")
PC_TR("Key bindings",
      "按鍵繫結")
PC_TR("Up to 4 keys or mouse buttons. Editing one switches the layout to Custom; empty = the PC layout's.",
      "最多 4 個按鍵或滑鼠按鈕。編輯任一項會將佈局切換為自訂；空 = 使用 PC 佈局的按鍵。")
PC_TR("A (interact)",
      "A（互動）")
PC_TR("B (sprint / back)",
      "B（衝刺 / 返回）")
PC_TR("X (jump)",
      "X（跳躍）")
PC_TR("Y (attack)",
      "Y（攻擊）")
PC_TR("Left stick click (crouch)",
      "左搖桿按下（蹲下）")
PC_TR("Right stick click (look)",
      "右搖桿按下（視角）")
PC_TR("L (abilities)",
      "L（能力）")
PC_TR("R (throw)",
      "R（投擲）")
PC_TR("ZL (target / shield)",
      "ZL（注視 / 盾牌）")
PC_TR("ZR (bow)",
      "ZR（弓）")
PC_TR("+ (menu)",
      "＋鍵（選單）")
PC_TR("- (map)",
      "－鍵（地圖）")
PC_TR("D-pad left (shields)",
      "十字鍵左（盾牌）")
PC_TR("D-pad up (items)",
      "十字鍵上（道具）")
PC_TR("D-pad right (weapons)",
      "十字鍵右（武器）")
PC_TR("Move left",
      "向左移動")
PC_TR("Move forward",
      "向前移動")
PC_TR("Move right",
      "向右移動")
PC_TR("Move back",
      "向後移動")
PC_TR("Camera left",
      "視角向左")
PC_TR("Camera up",
      "視角向上")
PC_TR("Camera right",
      "視角向右")
PC_TR("Camera down",
      "視角向下")
PC_TR("Walk (hold)",
      "步行（按住）")
PC_TR("Pad buttons",
      "控制器按鈕")
PC_TR("Up to 4 pad buttons, by position: A = the right face button (Xbox B, PlayStation Circle), B = bottom (Xbox A, Cross), X = top (Xbox Y, Triangle), Y = left (Xbox X, Square). Empty = its own button; None = unbound.",
      "最多 4 個控制器按鈕，按位置計：A = 右側面鍵（Xbox B，PlayStation 圓圈），B = 下方（Xbox A，叉），X = 上方（Xbox Y，三角），Y = 左側（Xbox X，方塊）。空 = 其自身按鈕；無 = 未繫結。")
PC_TR("Skipping to Great Sky Island in %.1f s... (release Minus to cancel)",
      "%.1f s 後跳轉到初始空島……（鬆開－鍵取消）")
PC_TR("Skip to Great Sky Island: F2 or hold Minus (restarts into a new game)",
      "跳轉到初始空島：F2 或按住－鍵（重新開始新遊戲）")
PC_TR("Skip opening: ON   (F2 / Minus to change)",
      "跳過開場：開啟   （F2 / －鍵切換）")
PC_TR("Skip opening: OFF   (F2 / Minus to change)",
      "跳過開場：關閉   （F2 / －鍵切換）")
PC_TR("Graphics settings: F10  (pad: L3 + R3)",
      "圖形設定：F10  （控制器：L3 + R3）")
PC_TR("Shader warm-up: %d / %d (%d %%) - press A to play now, it keeps compiling in the background",
      "著色器預熱：%d / %d（%d %%），按 A 立即開始遊戲，背景繼續編譯")
PC_TR("Preparing the shaders for this PC (first start): %d %%",
      "正在為此電腦準備著色器（首次啟動）：%d %%")
PC_TR("Compiling shaders %d %%",
      "正在編譯著色器 %d %%")
PC_TR("Loading shaders %d %%",
      "正在載入著色器 %d %%")
PC_TR("(paused: low memory)",
      "（已暫停：記憶體不足）")
PC_TR("Windows runs your monitor at %lu Hz: set it to %lu Hz in Settings > Display > Advanced display",
      "Windows 當前以 %lu Hz 執行顯示器：請在「設定 > 顯示 > 高階顯示」中改為 %lu Hz")
PC_TR("It writes the NSO header.",
      "它會寫入 NSO 頭。")
PC_TR("It writes past main's image (bss).",
      "它寫入了 main 映像之外的區域（bss）。")
PC_TR("It changes code the port already patches (%s).",
      "它修改了本移植版已打補丁的程式碼（%s）。")
PC_TR("It overlaps %s, higher in the list.",
      "它與清單中靠前的 %s 重疊。")
PC_TR("It is cut short at word %s.",
      "它在字 %s 處被截斷。")
PC_TR("It has an opcode the cheat engine does not know (%s).",
      "它含有金手指引擎不認識的操作碼（%s）。")
PC_TR("It uses the alias or ASLR memory region, which the port does not have.",
      "它使用了本移植版沒有的別名或 ASLR 記憶體區域。")
PC_TR("It changes the game's code only under a condition (a button, a value), which the port cannot do yet.",
      "它僅在特定條件（按鈕、數值）下修改遊戲程式碼，本移植版暫時無法做到。")
PC_TR("It cannot be read (%s).",
      "它無法讀取（%s）。")
PC_TR("%d ExeFS patches in use",
      "已使用 %d 個 ExeFS 補丁")
PC_TR("%d code patches need the game built on this PC (Builder install), not used",
      "%d 個程式碼補丁需要在本機建置的遊戲（Builder 安裝），未使用")
PC_TR("the build with its code patches failed (see exefs/%s/build.log next to totk.exe), not used",
      "帶程式碼補丁的建置失敗（見 totk.exe 旁的 exefs/%s/build.log），未使用")
PC_TR("%d code patches: the game is rebuilt with them at this start",
      "%d 個程式碼補丁：本次啟動時會帶著它們重新建置遊戲")
PC_TR("%d ExeFS patches for another game version, not used",
      "%d 個適用於其他遊戲版本的 ExeFS 補丁，未使用")
PC_TR("left out: %s",
      "已排除：%s")
PC_TR("%d cheats (F10 Cheats tab)",
      "%d 個金手指（F10 金手指標籤頁）")
PC_TR("no cheat file for this game version (%s)",
      "此遊戲版本沒有金手指檔案（%s）")
PC_TR("It changes the game's code: the game is rebuilt with it at the next start.",
      "它會修改遊戲程式碼：下次啟動時遊戲將帶著它重新建置。")
PC_TR("Not an amiibo dump: an amiibo image is 540 bytes.",
      "不是 amiibo 轉儲檔案：amiibo 映像為 540 位元組。")
PC_TR("Not an amiibo dump: the amiibo marker is missing.",
      "不是 amiibo 轉儲檔案：缺少 amiibo 標記。")
PC_TR("Not an amiibo dump: unknown figure data.",
      "不是 amiibo 轉儲檔案：人偶資料未知。")
PC_TR("The file cannot be read.",
      "無法讀取該檔案。")
PC_TR("amiibo: pick one in F10 (or L3 + R3) > Amiibo",
      "amiibo：在 F10（或 L3 + R3）> Amiibo 中選擇")
PC_TR("Your own amiibo dumps (.bin, 540 bytes, or Flipper .nfc) in the amiibo folder. In the game, open its amiibo screen; when it waits for a figure, press Scan on one here.",
      "將你自己的 amiibo 轉儲檔案（.bin，540 位元組，或 Flipper .nfc）放入 amiibo 資料夾。在遊戲中開啟 amiibo 介面；等待人偶時，在此處對其中一個點選掃描。")
PC_TR("Open amiibo folder",
      "開啟 amiibo 資料夾")
PC_TR("The game is waiting for an amiibo.",
      "遊戲正在等待 amiibo。")
PC_TR("On the reader: %s",
      "讀取器上：%s")
PC_TR("Open the game's amiibo screen first: Scan works while it waits for a figure.",
      "請先開啟遊戲的 amiibo 介面：遊戲等待人偶時才能掃描。")
PC_TR("No dumps yet: put .bin or .nfc files in the amiibo folder.",
      "還沒有轉儲檔案：請將 .bin 或 .nfc 檔案放入 amiibo 資料夾。")
PC_TR("Scan",
      "掃描")
PC_TR("Movement",
      "移動")
PC_TR("Jump",
      "跳躍")
PC_TR("Sprint",
      "衝刺")
PC_TR("Crouch",
      "蹲下")
PC_TR("Let go (a wall, a ride)",
      "鬆手（牆壁、騎乘）")
PC_TR("Dive",
      "俯衝")
PC_TR("Close the paraglider",
      "收起滑翔傘")
PC_TR("Swim dash",
      "游泳衝刺")
PC_TR("Combat",
      "戰鬥")
PC_TR("Attack / draw the bow",
      "攻擊 / 拉弓")
PC_TR("Lower the bow (cancel)",
      "放下弓（取消）")
PC_TR("Next arrow material",
      "下一種箭矢素材")
PC_TR("Previous arrow material",
      "上一種箭矢素材")
PC_TR("Shield and lock on",
      "盾牌與注視")
PC_TR("Put away / take out the weapon",
      "收起 / 拿出武器")
PC_TR("Take out / put away the bow",
      "拿出 / 收起弓")
PC_TR("Throw",
      "投擲")
PC_TR("Next weapon",
      "下一把武器")
PC_TR("Previous weapon",
      "上一把武器")
PC_TR("Construct: right arm",
      "構造體：右臂")
PC_TR("Construct: left arm",
      "構造體：左臂")
PC_TR("Construct: back part",
      "構造體：後部")
PC_TR("Construct: guard",
      "構造體：防護")
PC_TR("Construct: attach a device",
      "構造體：安裝裝置")
PC_TR("Construct: get off",
      "構造體：離開")
PC_TR("Items and tools",
      "道具與工具")
PC_TR("Interact",
      "互動")
PC_TR("Melee weapons",
      "近戰武器")
PC_TR("Shields",
      "盾牌")
PC_TR("Bows",
      "弓")
PC_TR("Materials",
      "素材")
PC_TR("Scope",
      "望遠鏡")
PC_TR("Whistle",
      "口哨")
PC_TR("Inventory",
      "物品欄")
PC_TR("Map",
      "地圖")
PC_TR("Adventure log",
      "冒險筆記")
PC_TR("Pause menu",
      "暫停選單")
PC_TR("Abilities",
      "能力")
PC_TR("Ultrahand",
      "究極手")
PC_TR("Fuse",
      "餘料建造")
PC_TR("Ascend",
      "通天術")
PC_TR("Recall",
      "倒轉乾坤")
PC_TR("Autobuild",
      "藍圖")
PC_TR("Purah Pad camera",
      "普爾亞平板相機")
PC_TR("Ability wheel",
      "能力輪盤")
PC_TR("Use the ability",
      "使用能力")
PC_TR("Secondary action",
      "次要操作")
PC_TR("Pull closer",
      "拉近")
PC_TR("Push away",
      "推遠")
PC_TR("Rotate",
      "旋轉")
PC_TR("Reset rotation",
      "重設旋轉")
PC_TR("Raise",
      "升高")
PC_TR("Lower",
      "降低")
PC_TR("Unstick",
      "解除卡住")
PC_TR("Leave the ability",
      "退出能力")
PC_TR("Leave (camera, Scope, quick menu)",
      "退出（相機、望遠鏡、快捷選單）")
PC_TR("Take a photo",
      "拍照")
PC_TR("Self-portrait",
      "自拍")
PC_TR("Camera: zoom in",
      "相機：放大")
PC_TR("Camera: zoom out",
      "相機：縮小")
PC_TR("Place a pin (Scope)",
      "放置圖釘（望遠鏡）")
PC_TR("Horse",
      "馬")
PC_TR("Spur",
      "驅馬")
PC_TR("Dismount",
      "下馬")
PC_TR("Soothe",
      "安撫")
PC_TR("Reset the camera",
      "重設視角")
PC_TR("Menus",
      "選單")
PC_TR("Confirm",
      "確認")
PC_TR("Back",
      "返回")
PC_TR("Previous tab",
      "上一標籤頁")
PC_TR("Next tab",
      "下一標籤頁")
PC_TR("Rotate left",
      "向左旋轉")
PC_TR("Rotate right",
      "向右旋轉")
PC_TR("Selection up",
      "向上選擇")
PC_TR("Selection down",
      "向下選擇")
PC_TR("Selection left",
      "向左選擇")
PC_TR("Selection right",
      "向右選擇")
PC_TR("D-pad up (selection)",
      "十字鍵上（選擇）")
PC_TR("D-pad down (selection)",
      "十字鍵下（選擇）")
PC_TR("D-pad left (selection, value)",
      "十字鍵左（選擇、數值）")
PC_TR("D-pad right (selection, value)",
      "十字鍵右（選擇、數值）")
PC_TR("Scroll up",
      "向上滾動")
PC_TR("Scroll down",
      "向下滾動")
PC_TR("Item action (X prompt)",
      "道具操作（X 提示）")
PC_TR("Menu: ZL (previous page)",
      "選單：ZL（上一頁）")
PC_TR("Menu: ZR (next page)",
      "選單：ZR（下一頁）")
PC_TR("Menu: left stick click",
      "選單：左搖桿按下")
PC_TR("Menu: right stick click",
      "選單：右搖桿按下")
PC_TR("Item action (Y prompt)",
      "道具操作（Y 提示）")
PC_TR("Recipe book: open (Food tab)",
      "食譜：開啟（料理標籤頁）")
PC_TR("Recipe book: close",
      "食譜：關閉")
PC_TR("Recipe book: cooking history",
      "食譜：烹飪記錄")
PC_TR("Quick menu: choose",
      "快捷選單：選擇")
PC_TR("Quick menu: sort",
      "快捷選單：排序")
PC_TR("Quick menu: D-pad up",
      "快捷選單：十字鍵上")
PC_TR("Quick menu: D-pad down",
      "快捷選單：十字鍵下")
PC_TR("Quick menu: D-pad left",
      "快捷選單：十字鍵左")
PC_TR("Quick menu: D-pad right",
      "快捷選單：十字鍵右")
PC_TR("Quick menu: drop the item",
      "快捷選單：丟棄道具")
PC_TR("Advance dialogue",
      "推進對話")
PC_TR("Conversation log",
      "對話記錄")
PC_TR("Pick the answer",
      "選擇回答")
PC_TR("Answer 1",
      "回答 1")
PC_TR("Answer 2",
      "回答 2")
PC_TR("Answer 3",
      "回答 3")
PC_TR("Answer 4",
      "回答 4")
PC_TR("Advance (events)",
      "推進（事件）")
PC_TR("Skip",
      "跳過")
PC_TR("Skip (short scenes)",
      "跳過（短場景）")
PC_TR("Rebinding is not available yet",
      "暫時無法重新繫結")
PC_TR("No such action here",
      "此處沒有該操作")
PC_TR("A fixed control has that key",
      "已有固定控制使用該按鍵")
PC_TR("Reserved key (F10, F12, Windows, Print Screen)",
      "保留按鍵（F10、F12、Windows、Print Screen）")
PC_TR("Confirm, back and pause must keep a key",
      "確認、返回和暫停必須保留一個按鍵")
PC_TR("That swap would clash again: choose another key",
      "該交換會再次衝突：請選擇其他按鍵")
PC_TR("The wheel only works for single presses",
      "滾輪僅適用於單次按下")
PC_TR("Refused",
      "已拒絕")
PC_TR("Swapped with ",
      "已互換：")
PC_TR("Stored now; acts once the PC gameplay keys are on (Controls tab).",
      "已儲存；啟用 PC 遊戲按鍵後生效（操控標籤頁）。")
PC_TR("Press a key, mouse button or wheel (Esc: cancel)",
      "請按下按鍵、滑鼠按鈕或滾輪（Esc：取消）")
PC_TR("Change",
      "更改")
PC_TR("Add",
      "新增")
PC_TR("Default",
      "預設")
PC_TR("Key moved by",
      "按鍵被移走：")
PC_TR("changed",
      "已更改")
PC_TR("in use by",
      "被佔用：")
PC_TR("settings.ini gives this key to both; it works for the other one. Change one of them.",
      "settings.ini 把該按鍵同時給了兩者；它對另一項有效。請更改其中一項。")
PC_TR("Key rebinding is not available in this build yet.",
      "此版本暫不支援按鍵重新繫結。")
PC_TR("Restore defaults",
      "恢復預設")
PC_TR("Cheats from your mods' cheats folders (Atmosphere format). They switch at once; one that changes the game's code once the game is built with it (at the start after its mod is turned on or changed). Cheats can break a save: keep a copy.",
      "來自各模組 cheats 資料夾的金手指（Atmosphere 格式）。立即生效；修改遊戲程式碼的金手指，需在遊戲帶著它建置後才生效（在其模組啟用或更改後的啟動時）。金手指可能損壞存檔：請保留副本。")
PC_TR("%s, %d x %d at %d Hz",
      "%s，%d x %d，%d Hz")
PC_TR(" (it can do %d Hz: Windows display settings)",
      "（可達 %d Hz：Windows 顯示設定）")
PC_TR(" (native %d x %d)",
      "（原生 %d x %d）")
PC_TR(". HDR on in Windows",
      "。Windows 中已開啟 HDR")
PC_TR(". HDR off in Windows (the monitor can)",
      "。Windows 中未開啟 HDR（顯示器支援）")
PC_TR(". No HDR",
      "。無 HDR")
PC_TR(", %d nits",
      "，%d nits")
PC_TR(". VRR on (%s",
      "。VRR 已開啟（%s")
PC_TR(". VRR off in the driver (%s monitor",
      "。VRR 在驅動程式中已關閉（%s 顯示器")
PC_TR(". VRR: a %s monitor, not confirmed by the driver (Options: VRR on if it is on there",
      "。VRR：%s 顯示器，驅動程式未確認（選項：若那裡已開啟，則 VRR 開啟")
PC_TR(". No VRR",
      "。無 VRR")
PC_TR("Graphics",
      "圖形")
PC_TR("Rendering",
      "渲染")
PC_TR("Upscaler",
      "超解析度")
PC_TR("Frame generation",
      "影格生成")
PC_TR("Ultrahand rotation speed",
      "究極手旋轉速度")
PC_TR("TOTK_PC_ROTATE_SPEED: with R held in Ultrahand or Autobuild, the mouse moved sideways turns the object in 45-degree steps (1x: the move that turns the camera 45 degrees) and the wheel turns it vertically, a step a notch.",
      "TOTK_PC_ROTATE_SPEED：在究極手或藍圖中按住 R 時，左右移動滑鼠以 45 度步進旋轉物體（1x：使鏡頭轉動 45 度的移動量），滾輪使其縱向旋轉，每格一步。")
PC_TR("0.5x (slow)",
      "0.5x（慢）")
PC_TR("2x (fast)",
      "2x（快）")
PC_TR("Automatic (a dedicated GPU first)",
      "自動（優先獨立 GPU）")
PC_TR("Display",
      "顯示")
PC_TR("Graphics card",
      "顯示卡")
PC_TR("The graphics card the game runs on, for a laptop with two. Automatic picks the dedicated one, the faster; the integrated one uses less power but is much slower.",
      "遊戲執行所用的顯示卡，適用於有兩塊顯示卡的筆電。自動選擇效能更強的獨立顯示卡；整合顯示卡更省電但慢得多。")
PC_TR("Sets every quality row below at once. Ultra is the game at its best on an 8 GB card; Low and Medium draw less than the Switch does. Changing a row by hand makes it Custom.",
      "一次設定下方所有畫質項。超高是 8 GB 顯示卡上遊戲的最佳效果；低和中的繪製量比 Switch 更少。手動更改某一項會變為自訂。")
PC_TR("Windowed, borderless fullscreen (the default: Alt+Tab is instant) or exclusive fullscreen (the monitor switches to the resolution and refresh rate below). F11 or Alt+Enter also switch it.",
      "視窗化、無邊框全螢幕（預設：Alt+Tab 即時切換）或獨佔全螢幕（顯示器切換到下方的解析度和更新率）。F11 或 Alt+Enter 也可切換。")
PC_TR("The window's size in windowed mode. Fullscreen uses the monitor's resolution.",
      "視窗模式下的視窗大小。全螢幕使用顯示器的解析度。")
PC_TR("The monitor fullscreen uses and a windowed start opens on. Remember: the one the game was on last.",
      "全螢幕所用的顯示器，視窗化啟動也在其上開啟。「記住」：上次遊戲所在的顯示器。")
PC_TR("The resolution the monitor switches to in exclusive fullscreen. Borderless and windowed keep the desktop's.",
      "獨佔全螢幕時顯示器切換到的解析度。無邊框和視窗化保持桌面解析度。")
PC_TR("The refresh rate in exclusive fullscreen. Higher is smoother when the frame rate keeps up.",
      "獨佔全螢幕時的更新率。影格率跟得上時，越高越流暢。")
PC_TR("The most frames per second the game shows. Higher is smoother and costs more GPU and CPU; 30 is the Switch's. Unlocked follows the monitor's refresh rate (240 at most); steady holds a rate the scene can keep. For an even picture pick a cap that divides the refresh rate (30 or 60 on 60 Hz, 40 on 120 Hz), or turn VRR on. Steam Deck: 60 (OLED: 90) at first; if it is not held, lower it to 40 with the screen at 40 Hz (Quick Access > Performance).",
      "遊戲顯示的每秒最大影格數。越高越流暢，但佔用更多 GPU 和 CPU；30 為 Switch 的影格率。不限影格率跟隨顯示器更新率（最高 240）；穩定則保持場景能維持的影格率。為使畫面均勻，請選擇能整除更新率的影格率上限（60 Hz 選 30 或 60，120 Hz 選 40），或開啟 VRR。Steam Deck：初始為 60（OLED：90）；若無法維持，請降至 40，並將螢幕設為 40 Hz（快速訪問 > 效能）。")
PC_TR("Frame rate \"Unlocked, steady\" only: the lowest rate it holds. 30 (default) suits most PCs. On one that cannot hold 30, 20 or 15 show every frame for the same time (3 or 4 refreshes on 60 Hz) instead of an uneven 15-20.",
      "僅用於影格率「不限影格率，穩定」：它保持的最低影格率。30（預設）適合大多數電腦。在無法穩定 30 的電腦上，20 或 15 讓每影格顯示相同時長（60 Hz 下 3 或 4 次更新），而不是不均勻的 15-20。")
PC_TR("%d fps on this %d Hz monitor shows frames unevenly (one refresh, then two). Even: %s, steady, VRR on, or the monitor at %d Hz or a multiple (Steam Deck: Quick Access > Performance).",
      "%d fps 在這台 %d Hz 顯示器上會使畫面不均勻（一次更新，然後兩次）。均勻的選擇：%s、穩定、開啟 VRR，或將顯示器設為 %d Hz 或其倍數（Steam Deck：快速訪問 > 效能）。")
PC_TR("uneven",
      "不均勻")
PC_TR("On (default): no tearing. Off: slightly less input latency, with tearing lines.",
      "開啟（預設）：無撕裂。關閉：輸入延遲略低，但有撕裂線。")
PC_TR("Variable refresh rate (G-SYNC, FreeSync): the monitor waits for each frame, smooth below its refresh rate and without tearing. Needs a VRR monitor with VRR on in Windows or the driver.",
      "可變更新率（G-SYNC、FreeSync）：顯示器等待每一影格，在其更新率以下流暢且無撕裂。需要 VRR 顯示器，並在 Windows 或驅動程式中開啟 VRR。")
PC_TR("Shows fps, the 1% low and a frame-time graph over the game (F9), to check performance.",
      "在遊戲上方顯示 fps、1% 低影格和影格時間曲線（F9），用於檢查效能。")
PC_TR("A small frame-rate counter in a corner (F3 shows or hides it). Minimal: fps; Detailed adds the frame time, the 1% low and the worst frame. Off costs nothing.",
      "角落裡的小型影格率計數器（F3 顯示或隱藏）。簡略：fps；詳細：另加影格時間、1% 低影格和最差影格。關閉不佔用資源。")
PC_TR("The counter's corner. Top right keeps clear of the hearts and the map.",
      "計數器所在的角落。右上角不會遮擋愛心和地圖。")
PC_TR("The counter's size. It also grows with the window's height.",
      "計數器的大小。也會隨視窗高度增大。")
PC_TR("Colour adjustment of the game's picture (not these menus). Off shows the game's own colours and costs nothing; Vivid is livelier; Brighter dark tones lightens caves, the Depths and nights; Custom uses the four rows below.",
      "對遊戲畫面進行色彩調整（不影響這些選單）。關閉：顯示遊戲原色，無開銷；鮮豔更生動；提亮暗部會照亮洞穴、地底和夜晚；自訂使用下方四項。")
PC_TR("Custom only: makes every colour brighter or darker; 0 is the game's.",
      "僅自訂：使所有顏色更亮或更暗；0 為遊戲原值。")
PC_TR("Custom only: more or less difference between light and dark; 1 is the game's.",
      "僅自訂：調整明暗差異；1 為遊戲原值。")
PC_TR("Custom only: how colourful the picture is; 1 is the game's, 0.5 greyer, 2 twice as colourful.",
      "僅自訂：畫面的色彩濃度；1 為遊戲原值，0.5 更灰，2 為兩倍濃度。")
PC_TR("Custom only: below 1 brightens the dark tones, above 1 darkens them; 1 is the game's.",
      "僅自訂：低於 1 提亮暗部，高於 1 壓暗；1 為遊戲原值。")
PC_TR("The resolution the 3D scene is drawn at, against the game's own. Higher is sharper and costs much more GPU; lower is faster and blurrier. The default: the window's resolution.",
      "3D 場景的繪製解析度，相對於遊戲自身的解析度。越高越清晰，GPU 開銷大得多；越低越快但更模糊。預設：視窗解析度。")
PC_TR("Lowers the 3D resolution only when the GPU falls behind, never below the floor chosen, to hold the frame rate. Off keeps the picture sharp all the time; On is smoother on a slower GPU.",
      "僅當 GPU 跟不上時降低 3D 解析度，且不低於所選下限，以保持影格率。關閉：畫面始終清晰；開啟：在較慢的 GPU 上更流暢。")
PC_TR("Above 1080p, menus, text and the HUD are drawn at the window's resolution instead of the game's 1080p stretched, and the 3D scene (render scale, DLSS / DLAA) reaches the window's resolution too. Sharper picture for a little GPU.",
      "高於 1080p 時，選單、文字和 HUD 按視窗解析度繪製，而不是拉伸遊戲的 1080p；3D 場景（渲染解析度、DLSS / DLAA）也達到視窗解析度。畫面更清晰，GPU 開銷很小。")
PC_TR("Generated frames between the real ones make motion look smoother, but the game itself does not run faster and input latency rises a little. DLSS x2: RTX 40 and newer, x3 and x4: RTX 50; FSR x2: any GPU. Best when the game already runs well.",
      "在真實影格之間生成的影格使動作更流暢，但遊戲本身不會更快，輸入延遲會略有增加。DLSS x2：RTX 40 及更新，x3 和 x4：RTX 50；FSR x2：任意 GPU。遊戲本身已執行良好時效果最佳。")
PC_TR("How the 3D scene is scaled up to the output: Bicubic (the port's) or FSR 1 (the game's own). Matters only below 100 % render resolution; either costs little.",
      "3D 場景放大到輸出解析度的方式：雙三次（重編譯）或 FSR 1（遊戲自帶）。僅在渲染解析度低於 100 % 時有意義；兩者開銷都很小。")
PC_TR("The filter that scales the 3D scene when the render resolution is not 100 %: Catmull-Rom, or FSR 1 EASU (sharper edges). Either costs little.",
      "渲染解析度不是 100 % 時放大 3D 場景的濾鏡：Catmull-Rom，或 FSR 1 EASU（邊緣更銳利）。兩者開銷都很小。")
PC_TR("How the finished picture is scaled to the window: Bilinear, or FSR 1 for a window above 1080p (sharper). Costs little.",
      "成品畫面縮放到視窗的方式：雙線性，或用於高於 1080p 視窗的 FSR 1（更銳利）。開銷很小。")
PC_TR("Smooths jagged edges on the final picture. SMAA 1x: smoother edges for a little GPU; Off is the game's own look.",
      "平滑最終畫面中的鋸齒邊緣。SMAA 1x：邊緣更平滑，GPU 開銷很小；關閉為遊戲原樣。")
PC_TR("Sharpens the final picture (AMD RCAS). More is crisper but can look grainy; Off is the game's look. Costs very little.",
      "銳化最終畫面（AMD RCAS）。越高越清晰，但可能顯得有顆粒感；關閉為遊戲原樣。開銷極小。")
PC_TR("Keeps ground textures sharp when seen at a shallow angle. Higher is sharper for a little GPU; 16x suits most GPUs. The game's: its own setting.",
      "在斜視角度下保持地面紋理清晰。越高越清晰，GPU 開銷很小；16x 適合大多數 GPU。遊戲自帶：使用其自身設定。")
PC_TR("Which texture detail level is used; 0 is the game's. Below 0 is sharper but can shimmer; above 0 is blurrier and can break textures (red grass). Not a quality slider: leave it at 0.",
      "使用的紋理細節級別；0 為遊戲原值。低於 0 更清晰但可能閃爍；高於 0 更模糊，且可能破壞紋理（紅色草地）。不是畫質滑塊：請保持為 0。")
PC_TR("The resolution of the renderer's shadow maps: 1x is the Switch's, 2x (default) sharper, 3x and 4x sharper still for more GPU time and memory. With little video memory they are lowered to what fits (4 GB cards: 2x).",
      "渲染器陰影貼圖的解析度：1x 為 Switch 的水平，2x（預設）更清晰，3x 和 4x 更加清晰，但佔用更多 GPU 時間和視訊記憶體。視訊記憶體較小時會降低到可容納的倍數（4 GB 顯示卡：2x）。")
PC_TR("Sun shadows grow softer the farther they fall from what casts them, as in real light. Off is the game's look; on costs some GPU. A change rebuilds the shadow shaders in the background: the old shadows show until then.",
      "太陽陰影離投射物越遠越柔和，如同真實光照。關閉為遊戲原樣；開啟會佔用一些 GPU。更改會在背景重建陰影著色器：在此之前仍顯示舊陰影。")
PC_TR("Adds the small sun shadows the shadow maps miss: at feet, under stones, in grass. Off is the game's look; Subtle to Strong cost some GPU.",
      "補充陰影貼圖遺漏的細小太陽陰影：腳下、石頭下、草叢中。關閉為遊戲原樣；細微到強烈會佔用一些 GPU。")
PC_TR("Prepares the shaders shipped with this version before you meet them, so new places stutter less the first time. Off: only the shaders this PC has built itself. On is better.",
      "在你遇到之前預先準備此版本附帶的著色器，使新區域首次進入時卡頓更少。關閉：只使用本機自行建置的著色器。開啟更好。")
PC_TR("A shader still being built: Limit to 0.1 s (default) shows the frame after 0.1 s and the missing objects appear a moment later; Always wait never shows anything incomplete but can freeze the picture for a second or more.",
      "著色器仍在建置時：限制為 0.1 s（預設）會在 0.1 s 後顯示該影格，缺少的物體稍後出現；始終等待從不顯示不完整的畫面，但可能使畫面卡住一秒或更久。")
PC_TR("Where the frame cap waits: at the present (default, steadier frame times) or before the frame (slightly less input latency).",
      "影格率上限在哪裡等待：在呈現處（預設，影格時間更穩定）或在影格開始前（輸入延遲略低）。")
PC_TR("What the game's 60 Hz tick follows: a 60 Hz clock (default, right for any monitor) or the monitor's vblank (a fixed-refresh monitor only). Leave the default.",
      "遊戲的 60 Hz 節拍跟隨什麼：60 Hz 時鐘（預設，適合任何顯示器）或顯示器的垂直消隱（僅限固定更新率顯示器）。請保持預設。")
PC_TR("The last part of the frame cap's wait busy-waits for exact timing: steadier frame times for some CPU. Off is the default.",
      "影格率上限等待的最後一段採用忙等待以獲得精確計時：影格時間更穩定，佔用部分 CPU。預設為關閉。")
PC_TR("How many finished frames may wait to be shown. 1 lowers input latency, 3 evens out uneven frames; 2 is the default.",
      "最多可有多少已完成的影格等待顯示。1 降低輸入延遲，3 使不均勻的影格更平穩；預設為 2。")
PC_TR("How many frames the CPU may prepare ahead of the GPU. 2 (default) is the balance; 1 lowers latency but is slower when both are busy; 3 adds latency.",
      "CPU 可比 GPU 提前準備多少影格。2（預設）較均衡；1 降低延遲，但兩者都忙時更慢；3 增加延遲。")
PC_TR("Holds each frame back just before it reads your input, so it waits less afterwards: lower input latency. NVIDIA uses Reflex; Boost also keeps the GPU clocks up (more power).",
      "在影格讀取輸入之前才放行，使其之後等待更少：輸入延遲更低。NVIDIA 使用 Reflex；Boost 還會保持 GPU 時鐘較高（更耗電）。")
PC_TR("HDR output for an HDR monitor (HDR10 or scRGB). Needs HDR on for that monitor in Windows; otherwise the picture stays SDR.",
      "面向 HDR 顯示器的 HDR 輸出（HDR10 或 scRGB）。需要在 Windows 中為該顯示器開啟 HDR；否則畫面仍為 SDR。")
PC_TR("Your monitor's peak brightness in nits, so highlights are neither clipped nor dim. Use the value the monitor states.",
      "顯示器的峰值亮度，單位 nits，使高光既不被削平也不暗淡。請使用顯示器標明的數值。")
PC_TR("How bright ordinary white (menus, the sky) is in HDR. Raise it if the picture looks dim, lower it if it is too bright.",
      "HDR 下普通白色（選單、天空）的亮度。畫面偏暗時調高，過亮時調低。")
PC_TR("How much of their real colour bright lights keep in HDR: a torch flame or lava stays orange instead of turning yellow-white. 0 (the default) shows the picture as in SDR, only brighter; the HUD is left as it is.",
      "HDR 下明亮光源保留多少真實顏色：火把的火焰或岩漿保持橙色，而不是變成黃白色。設為 0（預設）時畫面與 SDR 相同，只是更亮；HUD 保持原樣。")
PC_TR("The window's (the default): on a 3:2, 16:10, 21:9, 32:9 or 48:9 window or monitor the picture takes that shape, 21:9, 32:9 and 48:9 with a wider view; other shapes, 16:9 among them, keep the game's own. 16:9 / 16:10 / 21:9 / 32:9 / 48:9 / 3:2: that shape whatever the window. A new shape's layouts are made at its first start (about 30 s, once). A wider picture costs more GPU.",
      "跟隨視窗（預設）：在 3:2、16:10、21:9、32:9 或 48:9 的視窗或顯示器上，畫面採用該形狀，21:9、32:9 和 48:9 視野更寬；其他形狀（包括 16:9）保持遊戲原畫面。16:9 / 16:10 / 21:9 / 32:9 / 48:9 / 3:2：無論視窗如何都使用該形狀。新形狀的介面佈局會在其首次啟動時生成（約 30 秒，僅一次）。畫面越寬，GPU 負擔越大。")
PC_TR("On 21:9, 32:9 or 48:9: the HUD at the screen's edges, or in the middle 16:9 (easier to see on a very wide screen). Nothing changes at 16:9. Automatic (the default): the middle on 48:9, the edges otherwise.",
      "21:9、32:9 或 48:9 時：HUD 位於螢幕邊緣，或位於中間的 16:9 區域（超寬螢幕上更易看清）。16:9 時無變化。 自動（預設）：48:9 時在中間，其他時在邊緣。")
PC_TR("On 21:9, 32:9 or 48:9: real-time cutscenes wide like the rest of the game, or the original 16:9 with bars at the sides.",
      "21:9、32:9 或 48:9 時：即時過場動畫與遊戲其餘部分一樣寬，或保持原始 16:9 並在兩側加黑邊。")
PC_TR("Widens or narrows the camera's view (1 = the game's). Wider shows more of the world and costs some GPU and CPU.",
      "拉寬或收窄視野（1 = 遊戲原值）。更寬能看到更多世界，佔用一些 GPU 和 CPU。")
PC_TR("More memory for textures: fewer blurry textures when you turn around, for about 490 MiB of video memory per step.",
      "更多紋理記憶體：轉身時模糊紋理更少，每級約佔 490 MiB 視訊記憶體。")
PC_TR("More grass, flowers and pebbles. Higher looks fuller and costs GPU and CPU.",
      "更多草、花和卵石。越高越茂密，佔用 GPU 和 CPU。")
PC_TR("Sharper shadows: 2 doubles the shadow map's width and height, for 4x its memory and more GPU.",
      "更清晰的陰影：2 使陰影貼圖的寬和高翻倍，記憶體為 4 倍，GPU 開銷更大。")
PC_TR("Moves the lines where the sun's sharp near shadows hand over to coarser ones further away (1 = the game's, 2 = twice as far). Near shadows get a little softer: raise Shadow resolution with it. Costs a little GPU and CPU. With Shadow distance above 1 the two multiplied stay at 4 at most, so at Shadow distance 4 this does nothing.",
      "把太陽近處清晰陰影過渡到較粗糙陰影的分界線推得更遠（1 = 遊戲原設，2 = 兩倍距離）。近處陰影會稍微變柔和：請同時提高陰影解析度。會稍微增加 GPU 和 CPU 開銷。 當陰影距離大於 1 時，兩者相乘最多為 4，因此陰影距離為 4 時此項不起作用。")
PC_TR("Fixed keeps the sun's shadow map at its largest size: steadier, sharper leaf shadows for some GPU. The game's changes its size with the view.",
      "固定使太陽陰影貼圖保持最大尺寸：葉片陰影更穩定、更銳利，佔用一些 GPU。遊戲自帶會隨視角改變其大小。")
PC_TR("How far the sharp shadows reach. Higher looks better in the distance and costs GPU.",
      "清晰陰影的延伸距離。越高遠處越好看，佔用 GPU。")
PC_TR("Pushes the distance fog back: clearer far mountains. Higher shows more and costs some GPU.",
      "把遠處霧氣推遠：遠山更清晰。越高顯示越多，佔用一些 GPU。")
PC_TR("How far models keep their full detail. Higher looks better in the distance and costs GPU and CPU.",
      "模型保持完整細節的距離。越高遠處越好看，佔用 GPU 和 CPU。")
PC_TR("How far characters, enemies and objects keep full animation. Higher is smoother in the distance and costs CPU.",
      "角色、敵人和物體保持完整動畫的距離。越高遠處越流暢，佔用 CPU。")
PC_TR("How far the ground and water keep their full detail. Higher looks better and costs CPU and GPU.",
      "地面和水面保持完整細節的距離。越高越好看，佔用 CPU 和 GPU。")
PC_TR("How far from the camera characters, enemies and physics keep being simulated (1 = the game's). Higher keeps distant things alive and costs CPU.",
      "角色、敵人和物理效果距離鏡頭多遠仍保持模擬（1 = 遊戲原值）。越高遠處事物保持活躍，佔用 CPU。")
PC_TR("Fast: Link's departure when travelling is about 2.5 s shorter. Normal is the game's own.",
      "快速：旅行時林克出發的等待約縮短 2.5 s。常規為遊戲原樣。")
PC_TR("The game's overall volume, on top of the Windows volume.",
      "遊戲的總音量，疊加在 Windows 音量之上。")
PC_TR("The music's volume.",
      "音樂音量。")
PC_TR("The sound effects' volume.",
      "音效音量。")
PC_TR("The voices' volume. No part of the game's mix carries voices alone yet, so this does nothing for now.",
      "語音音量。遊戲的混音中目前沒有單獨承載語音的部分，因此暫時不起作用。")
PC_TR("The speakers the game mixes for: stereo, or 5.1 / 7.1 surround (experimental). Pick what your speakers or headphones use.",
      "遊戲混音所用的揚聲器：立體聲，或 5.1 / 7.1 環繞聲（實驗性）。請選擇你的揚聲器或耳機所用的。")
PC_TR("How much sound is queued ahead. Lower responds sooner; raise it if the sound crackles.",
      "提前排入多少音訊。越低反應越快；聲音有爆音時請調高。")
PC_TR("The sound moves to headphones or speakers as you plug them in or out, without a restart.",
      "插拔耳機或揚聲器時，聲音自動切換，無需重啟。")
PC_TR("No sound while the game window is not in front; the game keeps running.",
      "遊戲視窗不在前景時無聲音；遊戲繼續執行。")
PC_TR("PC: keyboard and mouse controls in the menus (Q / E tabs, Esc back, the mouse pointer) and PC keys in the prompts. Console: the Switch layout on the keyboard everywhere.",
      "PC：選單中使用鍵盤和滑鼠操作（Q / E 切換標籤頁，Esc 返回，滑鼠指標），提示中顯示 PC 按鍵。主機：所有介面都使用鍵盤上的 Switch 佈局。")
PC_TR("PC (default): one key per action (Space jumps, Shift sprints, F attacks...). Console layout: the old keys in the world. Menus use the PC keys either way.",
      "PC（預設）：每個動作一個按鍵（空格跳躍，Shift 衝刺，F 攻擊……）。主機佈局：世界中使用舊按鍵。選單兩種方式下都使用 PC 按鍵。")
PC_TR("The keys of the Console scheme: Classic, PC (WASD), or Custom (the key bindings below).",
      "主機方案的按鍵：經典、PC（WASD）或自訂（下方的按鍵繫結）。")
PC_TR("The button pictures in prompts and menus. Automatic follows the device you used last: keyboard and mouse, Xbox, PlayStation or Nintendo.",
      "提示和選單中的按鍵圖示。自動跟隨你最近使用的裝置：鍵盤和滑鼠、Xbox、PlayStation 或 Nintendo。")
PC_TR("The pad's face buttons in menus and dialogues: as the game, printed A accepts and B goes back, or the Switch layout (right accepts). The world keeps its layout.",
      "選單和對話中控制器的面鍵：與遊戲相同、印有 A 的鍵確認且 B 返回，或 Switch 配置（右鍵確認）。世界中的配置不變。")
PC_TR("In menus a mouse pointer shows and selects what it points at; left click is A, Esc goes back.",
      "選單中會顯示滑鼠指標並選取其指向的項；左鍵為 A，Esc 返回。")
PC_TR("In the pause menu and the Purah Pad: Esc goes back, PageUp / PageDown or the wheel switch tabs, left click selects and right click goes back.",
      "在暫停選單和普爾亞平板中：Esc 返回，PageUp / PageDown 或滾輪切換標籤頁，左鍵選擇，右鍵返回。")
PC_TR("How fast the mouse turns the camera (click the window to capture the pointer; Esc, F10 or Alt+Tab frees it). 0 = off; above 1 turns faster.",
      "滑鼠轉動視角的速度（點選視窗以捕獲指標；Esc、F10 或 Alt+Tab 可釋放）。0 = 關閉；大於 1 轉得更快。")
PC_TR("How fast the mouse turns the camera and aims. Higher turns faster.",
      "滑鼠轉動視角和瞄準的速度。越高轉得越快。")
PC_TR("The mouse's speed while the bow is drawn, times the usual one (1 = the same). Lower aims more finely.",
      "拉弓時的滑鼠速度，乘以常規速度（1 = 相同）。越低瞄準越精細。")
PC_TR("How much mouse movement a second counts as the stick pushed all the way. Lower turns faster.",
      "滑鼠每秒移動多少算作搖桿推到底。越低轉得越快。")
PC_TR("Moving the mouse up looks down, as in flight games.",
      "滑鼠上移則向下看，如同飛行遊戲。")
PC_TR("The mouse buttons aim and shoot.",
      "滑鼠按鈕用於瞄準和射擊。")
PC_TR("The mouse stands in for a pad's motion aim: while aiming, always, or off.",
      "滑鼠代替控制器的體感瞄準：瞄準時、始終或關閉。")
PC_TR("How far motion aim turns for a mouse movement. Higher turns further.",
      "滑鼠移動一次，體感瞄準轉動多遠。越高轉得越遠。")
PC_TR("While a D-pad quick menu is open (weapons, shields, items, bows), each wheel notch moves one entry.",
      "十字鍵快捷選單開啟時（武器、盾牌、道具、弓），滾輪每滾一格移動一項。")
PC_TR("On the map the mouse pans, the wheel zooms, a left click places a pin and a right click goes back.",
      "在地圖上，滑鼠平移，滾輪縮放，左鍵放置圖釘，右鍵返回。")
PC_TR("With the walk key held (Alt in the PC layout), how far the movement keys push the stick: lower walks slower; 1 is as without the key.",
      "按住步行鍵時（PC 佈局中為 Alt），移動鍵推動搖桿的幅度：越小走得越慢；1 與不按該鍵相同。")
PC_TR("How long a quick tap of W, A, S or D keeps Link moving, so he takes a step instead of sliding. 0 = off.",
      "輕點 W、A、S 或 D 後，林克持續移動多久，使其邁出一步而不是滑行。0 = 關閉。")
PC_TR("One press of the sprint key starts sprinting and the next stops it. Off: hold it, as on the Switch.",
      "按一下衝刺鍵開始衝刺，再按一下停止。關閉：按住，與 Switch 相同。")
PC_TR("One press of the shield / target key raises the shield and targets, the next lowers it. Off: hold it, as on the Switch.",
      "按一下盾牌 / 注視鍵舉盾並注視，再按一下放下。關閉：按住，與 Switch 相同。")
PC_TR("How fast moving the mouse sideways turns an object while R is held in Ultrahand or Autobuild (1x: as far as the camera turns). The wheel turns it vertically; moving the mouse up or down does not turn it.",
      "在究極手或藍圖中按住 R 時，左右移動滑鼠旋轉物體的快慢（1x：與鏡頭轉動相同）。滾輪使其縱向旋轉；上下移動滑鼠不會旋轉物體。")
PC_TR("A non-Nintendo pad's face buttons: by position (the Switch layout: the right button confirms) or by label (Xbox A confirms).",
      "非 Nintendo 控制器的面鍵：按位置（Switch 佈局：右側按鈕確認）或按標籤（Xbox A 確認）。")
PC_TR("Controller vibration: off, normal, or stronger (x2 to x4).",
      "控制器震動：關閉、常規或更強（x2 到 x4）。")
PC_TR("A pad's motion controls (aiming, some puzzles). Off ignores the gyro.",
      "控制器的體感操作（瞄準、部分謎題）。關閉則忽略陀螺儀。")
PC_TR("How far motion aim turns for a turn of the pad: 1 = the pad's own, 2 = twice as far.",
      "瞄準隨控制器轉動而轉動的幅度：1 = 控制器自帶，2 = 兩倍。")
PC_TR("How light rumbles feel next to strong ones: below 1 light ones come through stronger, above 1 only strong ones do. 1 = the game's own.",
      "輕微震動相對強烈震動的感受：低於 1 時輕微震動更明顯，高於 1 時只保留強烈震動。1 = 遊戲原本的。")
PC_TR("Xbox One / Series pads: the trigger motors buzz with the game's rumble. Off by default (the Switch pad has none).",
      "Xbox One / Series 控制器：扳機馬達隨遊戲的震動而震動。預設關閉（Switch 控制器沒有扳機馬達）。")
PC_TR("How fast the right stick turns the camera: 1 = the game's own, 2 = twice as fast. The mouse keeps its own sensitivity.",
      "右搖桿轉動鏡頭的速度：1 = 遊戲原本的，2 = 兩倍快。滑鼠保留自己的靈敏度。")
PC_TR("How far the left stick must move before it counts. Raise it if Link moves with the stick at rest; lower it for finer steps. 0 = the pad's own.",
      "左搖桿需要移動多遠才會生效。若搖桿靜止時林克仍在移動，請調高；想要更精細的步伐，請調低。0 = 控制器自帶。")
PC_TR("How far the right stick must move before it counts. Raise it if the camera turns with the stick at rest; lower it for finer aiming. 0 = the pad's own.",
      "右搖桿需要移動多遠才會生效。若搖桿靜止時視角仍在轉動，請調高；想要更精細的瞄準，請調低。0 = 控制器自帶。")
PC_TR("The game's texts and subtitles, and these settings' own (English, Spanish, Russian, Brazilian Portuguese, Simplified Chinese, Traditional Chinese). Português (Brasil): 1.4.3's texts; with 1.0.0 they need the 1.4.3 update given to Setup (English otherwise), voices stay English. Automatic: Windows' language if the game has it, else English.",
      "遊戲的文字和字幕，以及這些設定自身的文字（英語、西班牙語、俄語、巴西葡萄牙語、簡體中文、繁體中文）。Português (Brasil)：1.4.3 的文字；1.0.0 需要交給 Setup 的 1.4.3 更新（否則為英語），語音仍為英語。自動：遊戲支援時使用 Windows 的語言，否則為英語。")
PC_TR("A new game skips the opening and starts on the Great Sky Island. F2 or Minus on the title screen also switch it. Game version 1.0.0 only (on 1.4.x the opening plays and the row is hidden).",
      "新遊戲會跳過開場，直接從初始空島開始。標題畫面上按 F2 或－鍵也可切換。僅限遊戲版本 1.0.0（在 1.4.x 上會播放開場，此行隱藏）。")
PC_TR("Docked (default) or handheld: handheld renders at the Switch's lower handheld resolution (faster, blurrier).",
      "TV 模式（預設）或手提模式：手提模式以 Switch 較低的手提模式解析度渲染（更快，更模糊）。")
PC_TR("Pauses the game, picture and sound, while its window is not in front.",
      "視窗不在前景時暫停遊戲、畫面和聲音。")
PC_TR("The temperature on the Purah Pad's map: °C or °F. Automatic follows the game's language.",
      "普爾亞平板地圖上的溫度單位：°C 或 °F。自動：跟隨遊戲語言。")
PC_TR("How many older copies of your saves are kept, in case a save goes wrong.",
      "保留多少份舊存檔備份，以防存檔損壞。")
PC_TR("F12 saves a screenshot as a PNG in the Screenshots folder.",
      "F12 將截圖以 PNG 格式儲存到 Screenshots 資料夾。")
PC_TR("After a crash, a report and a minidump are written to attach to a bug report. Nothing is sent anywhere.",
      "崩潰後會生成報告和 minidump，可附在錯誤報告中。不會向任何地方傳送資料。")
PC_TR("Checks GitHub for a newer version when the game starts (at most once a day); asks first if the update rebuilds the game.",
      "遊戲啟動時檢查 GitHub 上是否有新版本（每天最多一次）；若更新需要重新建置遊戲，會先詢問。")
PC_TR("%s: get off",
      "%s：下來")
PC_TR("Control scheme",
      "操控方案")
PC_TR("PC: the keyboard and mouse controls of the menus (Q / E tabs, Esc back, the mouse pointer), Esc / I / M / J in the world and the prompts with PC keys. Console: the keyboard layout below in every screen.",
      "PC：選單使用鍵盤滑鼠操作（Q / E 切換標籤頁，Esc 返回，滑鼠指標），遊戲中使用 Esc / I / M / J，提示顯示 PC 按鍵。主機：所有介面都使用下方的鍵位佈局。")
PC_TR("Console",
      "主機")
PC_TR("Gameplay keys",
      "遊戲按鍵")
PC_TR("TOTK_GAMEPLAY. With the PC scheme: PC (default) = one key per action: Space jumps, Shift sprints, F attacks, Q guards, 4-9 pick an ability, Tab opens the wheel...; Console layout = the keys as before in the world. Menus use the PC keys either way.",
      "TOTK_GAMEPLAY。使用 PC 方案時：PC（預設）= 一個按鍵對應一個動作：空格跳躍，Shift 衝刺，F 攻擊，Q 防禦，4-9 選擇能力，Tab 開啟輪盤……；主機佈局 = 遊戲中沿用原來的按鍵。兩種方式下選單都使用 PC 按鍵。")
PC_TR("Console layout",
      "主機佈局")
PC_TR("Language",
      "語言")
PC_TR("TOTK_LANG: the game's texts and voices' subtitles, and the PC settings' own texts (English, Spanish, Russian, Brazilian Portuguese, Simplified Chinese, Traditional Chinese). Português (Brasil): 1.4.3's texts; with 1.0.0 they need the 1.4.3 update given to Setup (English otherwise), voices stay English. Automatic: Windows' display language if the game has it, else English.",
      "TOTK_LANG：遊戲文字、語音字幕以及 PC 設定自身的文字（英語、西班牙語、俄語、巴西葡萄牙語、簡體中文、繁體中文）。Português (Brasil)：1.4.3 的文字；1.0.0 需要交給 Setup 的 1.4.3 更新（否則為英語），語音仍為英語。自動：遊戲支援時使用 Windows 的顯示語言，否則為英語。")
PC_TR("Automatic (Windows' language)",
      "自動（Windows 語言）")
PC_TR("Russian",
      "俄語")
PC_TR("Japanese",
      "日語")
PC_TR("Korean",
      "韓語")
PC_TR("Chinese (Simplified)",
      "簡體中文")
PC_TR("Chinese (Traditional)",
      "繁體中文")
PC_TR("This folder has no romfs, exefs or cheats files.",
      "此資料夾中沒有 romfs、exefs 或 cheats 檔案。")
PC_TR("The folder name contains |, which the mod list uses between names. Rename the folder.",
      "資料夾名稱含有 |，而 Mod 清單用它分隔名稱。請重新命名該資料夾。")
PC_TR("It is made for another game (title %s).",
      "它適用於另一款遊戲（title %s）。")
PC_TR("An earlier mods folder has a mod with this name; this one is left out.",
      "之前的 mods 資料夾中已有同名 Mod，此項被忽略。")
PC_TR("It has code modules (subsdk or main), which the port cannot load, so the whole mod stays off.",
      "它包含程式碼模組（subsdk 或 main），本移植版無法載入，因此整個 Mod 保持關閉。")
PC_TR("It only has code patches (exefs) or cheats, which this version cannot use yet.",
      "它只有程式碼補丁（exefs）或金手指，當前版本尚無法使用。")
PC_TR("A mod's folder",
      "某個 Mod 的資料夾")
PC_TR("A mod (.zip, .7z or .rar)",
      "Mod（.zip、.7z 或 .rar）")
PC_TR("code patches",
      "程式碼補丁")
PC_TR("cheats",
      "金手指")
PC_TR("Installed %s (%s, for %s) in %s. It loads at the next start.",
      "已安裝 %s（%s，適用於 %s），位於 %s。下次啟動時載入。")
PC_TR("Its files don't name a game version: it was put with the one you play.",
      "其檔案未註明遊戲版本：已放到你正在玩的版本下。")
PC_TR("It is made for game version %s, which the port does not build: it was put with the 1.4.3 mods and may not work.",
      "此 Mod 適用於遊戲版本 %s，本移植版不建置該版本：已放到 1.4.3 的 Mod 中，可能無法使用。")
PC_TR("Not installed: it could not be opened. Unpack it yourself and choose its folder with Install a mod folder.",
      "未安裝：無法開啟。請自行解壓縮，然後用「安裝 Mod 資料夾」選擇其資料夾。")
PC_TR("Not installed: it has no romfs, exefs or cheats folder. If the mod is packed inside it, unpack it and choose that folder instead.",
      "未安裝：其中沒有 romfs、exefs 或 cheats 資料夾。如果 Mod 打包在裡面，請解壓縮後改選該資料夾。")
PC_TR("Not installed: it is a mod for another game.",
      "未安裝：這是其他遊戲的 Mod。")
PC_TR("Not installed: it loads code of its own (subsdk / main), which a recompiled game cannot load.",
      "未安裝：它會載入自身的程式碼（subsdk / main），重編譯的遊戲無法載入。")
PC_TR("Not installed: it could not be copied. Check that the mods folder can be written (Open mods folder).",
      "未安裝：無法複製。請檢查 mods 資料夾是否可寫入（開啟 mods 資料夾）。")
PC_TR("Mods load at the next start. The top mod wins a file two mods change; the port's own files (PC button prompts, fixes) stay over a mod's.",
      "Mod 在下次啟動時載入。兩個 Mod 修改同一檔案時，清單靠前的優先；移植版自帶的檔案（PC 按鍵提示、修復）優先於 Mod。")
PC_TR("The launcher chose these mods.",
      "這些 Mod 由啟動器選擇。")
PC_TR("Open mods folder",
      "開啟 mods 資料夾")
PC_TR("Install a mod...",
      "安裝 Mod……")
PC_TR("Install a mod folder...",
      "安裝 Mod 資料夾……")
PC_TR("Installing...",
      "正在安裝……")
PC_TR("In exclusive fullscreen the game goes to the desktop while the dialog is open.",
      "獨佔全螢幕時，對話方塊開啟期間遊戲會切到桌面。")
PC_TR("No mods yet. Put each mod in a folder of its own in the mods folder (its romfs, exefs and cheats folders as emulators take them), then start the game again.",
      "還沒有 Mod。請將每個 Mod 放進 mods 資料夾中各自的資料夾裡（其 romfs、exefs 和 cheats 資料夾與模擬器的用法相同），然後重新啟動遊戲。")
PC_TR("Ctrl+Up / Ctrl+Down (pad: Y / X) move the mod in focus.",
      "Ctrl+上 / Ctrl+下（控制器：Y / X）移動當前選取的 Mod。")
PC_TR("%d files replaced, %d added",
      "替換 %d 個檔案，新增 %d 個")
PC_TR("; %d left to the port's own files",
      "；%d 個留給移植版自帶檔案")
PC_TR("; %d left to %s, higher in the list",
      "；%d 個留給清單中更靠前的 %s")
PC_TR("; %d for another game version, not used",
      "；%d 個屬於其他遊戲版本，未使用")
PC_TR("Processor: %s, %d cores, %lu threads. Memory: %.0f GB.",
      "處理器：%s，%d 核，%lu 執行緒。記憶體：%.0f GB。")
PC_TR("unknown",
      "未知")
PC_TR("Graphics card: %s, %.0f GB.",
      "顯示卡：%s，%.0f GB。")
PC_TR("Display: %s",
      "顯示：%s")
PC_TR("1%% low %.1f fps (%.1f ms)",
      "1%% 低影格 %.1f fps（%.1f ms）")
PC_TR("1%% low: collecting",
      "1%% 低影格：採集中")
PC_TR("frame time 0-50 ms  (F9)",
      "影格時間 0-50 ms  (F9)")
PC_TR("frame",
      "影格")
PC_TR("1% low",
      "1% 低影格")
PC_TR("worst",
      "最差")
PC_TR("real",
      "真實")
PC_TR("Press a key or mouse button (Esc: cancel)",
      "按下按鍵或滑鼠按鈕（Esc：取消）")
PC_TR("(the layout's keys)",
      "（佈局自帶的按鍵）")
PC_TR("Add key",
      "新增按鍵")
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
      "左搖桿")
PC_TR("R stick",
      "右搖桿")
PC_TR("none",
      "無")
PC_TR("Press a pad button (Esc: cancel)",
      "按下控制器按鈕（Esc：取消）")
PC_TR("(its own button)",
      "（自帶的按鍵）")
PC_TR("Add button",
      "新增控制器按鈕")
PC_TR("this run",
      "本次執行")
PC_TR("Set by NX_SET_ for this run only; not saved.",
      "僅本次執行由 NX_SET_ 設定，不儲存。")
PC_TR("at next start",
      "下次啟動時")
PC_TR("restart",
      "重啟")
PC_TR("Point at a setting, or move to it, to see what it does.",
      "指向或移動到某項設定，即可檢視其作用。")
PC_TR("Applies when the game starts again.",
      "重新啟動遊戲後生效。")
PC_TR("No settings on this page yet.",
      "此頁暫無設定。")
PC_TR("Reset this page to defaults",
      "將此頁恢復預設")
PC_TR("Settings  (F10)",
      "設定  (F10)")
PC_TR("Mods",
      "Mod")
PC_TR("Cheats",
      "金手指")
PC_TR("Keys",
      "按鍵")
PC_TR("Some changes apply when the game starts again.",
      "部分更改需要重新啟動遊戲後生效。")
PC_TR("Restart the game now",
      "立即重啟遊戲")
PC_TR("The game closes and starts again with the new settings. Progress since your last save is lost.",
      "遊戲將關閉並以新設定重新啟動。自上次存檔以來的進度會丟失。")
PC_TR("Restart",
      "重啟")
PC_TR("Cancel",
      "取消")
PC_TR("Saved to %s",
      "已儲存到 %s")
PC_TR("Not saved (TOTK_SETTINGS=0)",
      "未儲存（TOTK_SETTINGS=0）")
PC_TR("Copy system info",
      "複製系統資訊")
PC_TR("Copied: paste it into your bug report.",
      "已複製：請貼上到你的錯誤報告中。")
PC_TR("Could not copy.",
      "無法複製。")
PC_TR("F10, Esc, B or L3 + R3 closes. A: select. L / R: pages. F9: perf overlay.",
      "F10、Esc、B 或 L3 + R3 關閉。A：選擇。L / R：切換頁面。F9：效能疊加顯示。")
PC_TR("Low",
      "低")
PC_TR("Medium",
      "中")
PC_TR("High",
      "高")
PC_TR("Welcome",
      "歡迎")
PC_TR("Pick graphics settings for this PC. Everything can be changed later with F10 (or L3 + R3).",
      "為這台電腦選擇圖形設定。之後都可以透過 F10（或 L3 + R3）更改。")
PC_TR("Set for this display: frame rate %s%s.%s",
      "已按此顯示器設定：影格率 %s%s。%s")
PC_TR("Unlocked (up to the refresh)",
      "不限影格率（最高到更新率）")
PC_TR(", VRR on",
      "，VRR 已開啟")
PC_TR(" HDR output (off) is in F10 > Graphics > HDR.",
      " HDR 輸出（已關閉）位於 F10 > 圖形 > HDR。")
PC_TR("%s (recommended)",
      "%s（推薦）")
PC_TR("Keep the current settings",
      "保持當前設定")
PC_TR("Some choices apply when the game starts again. Esc keeps the current settings.",
      "部分選項需要重新啟動遊戲後生效。按 Esc 保持當前設定。")
PC_TR("Applies when the game starts again: F10 > Restart the game now",
      "重新啟動遊戲後生效：F10 > 立即重啟遊戲")
PC_TR("Preset",
      "預設")
PC_TR("Graphics preset",
      "圖形預設")
PC_TR("Sets the rows below it; any change by hand makes it Custom.",
      "設定下方各項；手動更改任何一項都會變為自訂。")
PC_TR("Ultra",
      "超高")
PC_TR("Ultra+",
      "超高+")
PC_TR("Custom",
      "自訂")
PC_TR("First-run screen done",
      "首次執行介面已完成")
PC_TR("F10 hint shown",
      "已顯示 F10 提示")
PC_TR("Window mode",
      "視窗模式")
PC_TR("F11 or Alt+Enter also switch it.",
      "F11 或 Alt+Enter 也可切換。")
PC_TR("Windowed",
      "視窗化")
PC_TR("Borderless fullscreen (default)",
      "無邊框全螢幕（預設）")
PC_TR("Exclusive fullscreen",
      "獨佔全螢幕")
PC_TR("Window size",
      "視窗大小")
PC_TR("Windowed mode only (LARECOMP_WINDOW at start).",
      "僅視窗化模式（啟動時為 LARECOMP_WINDOW）。")
PC_TR("1280 x 720 (default)",
      "1280 x 720（預設）")
PC_TR("Frame rate",
      "影格率")
PC_TR("TOTK_FPS / TOTK_UNLOCK_FPS. Unlocked: up to the monitor's refresh rate (60 to 240), loading screens at the game's own pace. Steady: unlocked, but held at the highest rate the scene keeps, in whole refreshes of the monitor (240, 120, 80, 60 on a 240 Hz one), instead of swinging. 40 and 45: for a PC that cannot hold 60; smooth on a monitor whose refresh they divide (40 on 120 Hz, 45 on 90 or 180 Hz) or with VRR, uneven on a fixed 60 Hz.",
      "TOTK_FPS / TOTK_UNLOCK_FPS。不限影格率：最高到顯示器更新率（60 至 240），載入畫面按遊戲自身節奏。穩定：同樣不限影格率，但固定在場景能保持的最高影格率上，且為顯示器更新次數的整數分之一（240 Hz 顯示器上為 240、120、80、60），不再來回波動。40 和 45：適合無法穩定 60 的電腦；在更新率能被其整除的顯示器上流暢（120 Hz 上 40，90 或 180 Hz 上 45）或配合 VRR 使用，固定 60 Hz 下則不均勻。")
PC_TR("30 (the game's own)",
      "30（遊戲自帶）")
PC_TR("Unlocked (monitor rate)",
      "不限影格率（顯示器更新率）")
PC_TR("Unlocked, steady (a rate it can hold)",
      "不限影格率，穩定（可保持的影格率）")
PC_TR("Steady's lowest rate",
      "穩定模式最低影格率")
PC_TR("TOTK_STEADY_FLOOR. Frame rate Steady only: the lowest rate it holds. 20 or 15 keep a PC that cannot hold 30 even (3 or 4 refreshes a frame on 60 Hz).",
      "TOTK_STEADY_FLOOR。僅用於「穩定」影格率：它保持的最低影格率。20 或 15 讓無法穩定 30 的電腦影格間隔均勻（60 Hz 下每影格 3 或 4 次更新）。")
PC_TR("30 (default)",
      "30（預設）")
PC_TR("Perf overlay (F9)",
      "效能疊加顯示（F9）")
PC_TR("Fps, 1% low and the frame-time graph.",
      "顯示 fps、1% 低影格和影格時間曲線。")
PC_TR("Fps readout (F3)",
      "影格率顯示（F3）")
PC_TR("TOTK_FPS_OVERLAY. F3 shows or hides it in game.",
      "TOTK_FPS_OVERLAY。遊戲中按 F3 顯示或隱藏。")
PC_TR("Off (default; F3 shows it)",
      "關閉（預設；F3 顯示）")
PC_TR("Minimal: fps",
      "簡略：fps")
PC_TR("Detailed: fps, frame time, 1% low, worst",
      "詳細：fps、影格時間、1% 低影格、最差")
PC_TR("Fps readout corner",
      "影格率顯示位置")
PC_TR("Top right keeps clear of the hearts and the map.",
      "右上角不會遮擋愛心和地圖。")
PC_TR("Top right (default)",
      "右上角（預設）")
PC_TR("Top left",
      "左上角")
PC_TR("Bottom right",
      "右下角")
PC_TR("Bottom left",
      "左下角")
PC_TR("Fps readout size",
      "影格率顯示大小")
PC_TR("Follows the window height (1080p to 4K).",
      "隨視窗高度縮放（1080p 到 4K）。")
PC_TR("Small (default)",
      "小（預設）")
PC_TR("Medium size",
      "中")
PC_TR("Large size",
      "大")
PC_TR("Render resolution",
      "渲染解析度")
PC_TR("TOTK_RES_SCALE, of the game's 1600 x 900. Window resolution: the 3D scene as large as the picture in the window (x0.83 of it on an integrated GPU or one under 8 GB; two thirds of it with DLSS, FSR or XeSS, their Quality). Applies at once.",
      "TOTK_RES_SCALE，相對於遊戲的 1600 x 900。視窗解析度：3D 場景與視窗中的畫面一樣大（內顯或顯示記憶體低於 8 GB 時為其 x0.83；使用 DLSS、FSR 或 XeSS 時為其三分之二，即品質模式）。立即生效。")
PC_TR("100 % (native)",
      "100 %（原生）")
PC_TR("Window resolution (default)",
      "視窗解析度（預設）")
PC_TR("Window resolution x0.83 (default: integrated GPU or under 8 GB)",
      "視窗解析度 x0.83（預設：內顯或顯示記憶體低於 8 GB）")
PC_TR("Native-resolution interface",
      "原生解析度介面")
PC_TR("TOTK_OUT_SCALE: above 1080p, menus, text and the HUD drawn at the window's resolution instead of the game's 1920 x 1080 stretched; the 3D scene reaches it too. On by default. Applies at once.",
      "TOTK_OUT_SCALE：高於 1080p 時，選單、文字和 HUD 按視窗解析度繪製，而不是拉伸遊戲的 1920 x 1080；3D 場景也達到該解析度。預設開啟。立即生效。")
PC_TR("Dynamic resolution",
      "動態解析度")
PC_TR("Off: the scene always at the render resolution. On: the game lowers it only when the GPU is short of time, never below the chosen floor, aiming at the frame rate. Applies at once.",
      "關閉：場景始終使用渲染解析度。開啟：僅在 GPU 時間不足時由遊戲降低解析度，不會低於所選下限，以達到目標影格率為準。立即生效。")
PC_TR("Off (fixed size)",
      "關閉（固定大小）")
PC_TR("On, never below 90 %",
      "開啟，不低於 90 %")
PC_TR("On, never below 75 %",
      "開啟，不低於 75 %")
PC_TR("On, never below 50 %",
      "開啟，不低於 50 %")
PC_TR("On, the game's own (no minimum)",
      "開啟，遊戲自帶（無下限）")
PC_TR("Dynamic resolution (clock)",
      "動態解析度（時鐘）")
PC_TR("TOTK_DYNRES: the game's own, aimed at the frame rate.",
      "TOTK_DYNRES：遊戲自帶方式，以影格率為目標。")
PC_TR("3D upscaler",
      "3D 放大器")
PC_TR("TOTK_SSAA. Applies at once.",
      "TOTK_SSAA。立即生效。")
PC_TR("Bicubic (recomp)",
      "雙三次（重編譯）")
PC_TR("FSR 1 (the game's)",
      "FSR 1（遊戲自帶）")
PC_TR("Shipped shader list",
      "附帶著色器清單")
PC_TR("TOTK_SHADER_SEED: on, the shader list that comes with the build lets the game prepare those shaders before you meet them (fewer stutters the first time). Off: only the shaders this PC has met itself. Takes effect at the next start.",
      "TOTK_SHADER_SEED：開啟後，隨版本附帶的著色器清單會讓遊戲在你遇到這些著色器之前先行準備（首次遇到時卡頓更少）。關閉：只使用這台電腦自己遇到過的著色器。下次啟動時生效。")
PC_TR("Window upscaler",
      "視窗放大器")
PC_TR("TOTK_UPSCALE.",
      "TOTK_UPSCALE。")
PC_TR("Bilinear",
      "雙線性")
PC_TR("FSR 1 (window above 1080p)",
      "FSR 1（視窗高於 1080p）")
PC_TR("Anti-aliasing",
      "抗鋸齒")
PC_TR("TOTK_AA, on the presented image.",
      "TOTK_AA，作用於呈現的畫面。")
PC_TR("Sharpening",
      "銳化")
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
      "色彩調整")
PC_TR("TOTK_COLOR: the game's picture, its HUD included (not this menu). Off shows the game's own colours unchanged and costs nothing. Vivid: livelier colours. Brighter dark tones: caves, the Depths and nights lighter. Custom: the four rows below. Applies at once.",
      "TOTK_COLOR：作用於遊戲畫面，包括其 HUD（不含本選單）。關閉顯示遊戲原色，不增加開銷。鮮豔：色彩更生動。提亮暗部：洞穴、地底和夜晚更亮。自訂：下方四項。立即生效。")
PC_TR("Off (the game's own colours; default)",
      "關閉（遊戲原色；預設）")
PC_TR("Vivid",
      "鮮豔")
PC_TR("Brighter dark tones",
      "提亮暗部")
PC_TR("Custom (the four rows below)",
      "自訂（下方四項）")
PC_TR("Brightness",
      "亮度")
PC_TR("TOTK_COLOR_BRIGHTNESS (Custom only): added to every colour; 0 is the game's.",
      "TOTK_COLOR_BRIGHTNESS（僅自訂）：加到每種顏色上；0 為遊戲原樣。")
PC_TR("Contrast",
      "對比度")
PC_TR("TOTK_COLOR_CONTRAST (Custom only): around mid grey; 1 is the game's.",
      "TOTK_COLOR_CONTRAST（僅自訂）：以中灰為基準；1 為遊戲原樣。")
PC_TR("Saturation",
      "飽和度")
PC_TR("TOTK_COLOR_SATURATION (Custom only): 1 is the game's, 0.5 greyer, 2 twice as colourful.",
      "TOTK_COLOR_SATURATION（僅自訂）：1 為遊戲原樣，0.5 更灰，2 色彩加倍。")
PC_TR("Gamma",
      "伽馬")
PC_TR("TOTK_COLOR_GAMMA (Custom only): below 1 brightens dark tones, above 1 darkens them; 1 is the game's.",
      "TOTK_COLOR_GAMMA（僅自訂）：小於 1 提亮暗部，大於 1 壓暗；1 為遊戲原樣。")
PC_TR("Shadow map scale (renderer)",
      "陰影貼圖比例（渲染器）")
PC_TR("TOTK_SHADOW_SCALE: the renderer's shadow targets. The game patches' Shadow resolution is separate. Applies at once.",
      "TOTK_SHADOW_SCALE：渲染器的陰影目標。與遊戲補丁中的陰影解析度相互獨立。立即生效。")
PC_TR("1x (Switch)",
      "1x（Switch）")
PC_TR("2x (default)",
      "2x（預設）")
PC_TR("Soft shadows",
      "柔和陰影")
PC_TR("TOTK_SOFT_SHADOWS: the sun's shadow edges widen with the distance to what casts them (contact-hardening, PCSS) instead of keeping one width. Off is the game's look. A change rebuilds the shadowed shaders in the background: the old shadows show until they are ready.",
      "TOTK_SOFT_SHADOWS：太陽陰影的邊緣會隨與投影物距離變寬（接觸硬化，PCSS），而不是保持同一寬度。關閉為遊戲原樣。更改會在背景重建帶陰影的著色器：準備好之前仍顯示舊陰影。")
PC_TR("Off (the game's)",
      "關閉（遊戲原樣）")
PC_TR("Soft",
      "柔和")
PC_TR("Softer",
      "更柔和")
PC_TR("Softest",
      "最柔和")
PC_TR("Contact shadows",
      "接觸陰影")
PC_TR("TOTK_CONTACT_SHADOWS: small sun shadows at feet, under stones and in grass, which the shadow maps are too coarse for (a short march toward the sun through the scene's depth, up to 80 m). Off is the game's look; costs some GPU. Applies at once.",
      "TOTK_CONTACT_SHADOWS：腳下、石頭下和草叢中的細小太陽陰影，陰影貼圖精度不足以表現這些（沿場景深度朝太陽方向短距離步進，最遠 80 m）。關閉為遊戲原樣；會佔用一些 GPU。立即生效。")
PC_TR("Subtle",
      "細微")
PC_TR("Strong",
      "強烈")
PC_TR("Volume",
      "音量")
PC_TR("TOTK_VOLUME, master gain.",
      "TOTK_VOLUME，主音量。")
PC_TR("Mute in the background",
      "背景靜音")
PC_TR("TOTK_MUTE_UNFOCUSED: no sound while the game window is not in front; the game runs on (Pause when the window loses focus stops it too).",
      "TOTK_MUTE_UNFOCUSED：遊戲視窗不在前景時沒有聲音；遊戲繼續執行（「失去焦點時暫停」則會讓遊戲也停止）。")
PC_TR("Face buttons",
      "面鍵")
PC_TR("TOTK_PAD_LABELS.",
      "TOTK_PAD_LABELS。")
PC_TR("By position (Switch layout)",
      "按位置（Switch 佈局）")
PC_TR("By label (Xbox A = A)",
      "按標籤（Xbox A = A）")
PC_TR("Rumble",
      "震動")
PC_TR("TOTK_RUMBLE.",
      "TOTK_RUMBLE。")
PC_TR("Normal",
      "常規")
PC_TR("Motion controls",
      "體感操作")
PC_TR("TOTK_GYRO.",
      "TOTK_GYRO。")
PC_TR("Start",
      "開始")
PC_TR("Skip the opening",
      "跳過開場")
PC_TR("TOTK_SKIP_OPENING. F2 or Minus on the title screen also toggles it.",
      "TOTK_SKIP_OPENING。在標題畫面按 F2 或－鍵也可切換。")
PC_TR("On (start at the Great Sky Island)",
      "開啟（從初始空島開始）")
PC_TR("Console mode",
      "主機模式")
PC_TR("TOTK_HANDHELD: handheld renders at the lower resolution.",
      "TOTK_HANDHELD：手提模式以較低解析度渲染。")
PC_TR("Docked",
      "TV 模式")
PC_TR("Handheld",
      "手提模式")
PC_TR("TOTK_KEYBOARD_LAYOUT.",
      "TOTK_KEYBOARD_LAYOUT。")
PC_TR("Classic",
      "經典")
PC_TR("PC (WASD)",
      "PC（WASD）")
PC_TR("Custom (TOTK_KBM_<ACTION>)",
      "自訂（TOTK_KBM_<ACTION>）")
PC_TR("Mouse buttons aim and shoot",
      "滑鼠按鍵瞄準和射擊")
PC_TR("TOTK_MOUSE_AIM.",
      "TOTK_MOUSE_AIM。")
PC_TR("TOTK_MOUSE_SENS.",
      "TOTK_MOUSE_SENS。")
PC_TR("Mouse counts for a full stick",
      "滿搖桿所需滑鼠計數")
PC_TR("TOTK_MOUSE_STICK_SPEED (per second).",
      "TOTK_MOUSE_STICK_SPEED（每秒）。")
PC_TR("TOTK_MOUSE_INVERT.",
      "TOTK_MOUSE_INVERT。")
PC_TR("TOTK_MOUSE_GYRO.",
      "TOTK_MOUSE_GYRO。")
PC_TR("While aiming",
      "瞄準時")
PC_TR("TOTK_MOUSE_GYRO_SPEED.",
      "TOTK_MOUSE_GYRO_SPEED。")
PC_TR("Anisotropic filtering",
      "各向異性過濾")
PC_TR("TOTK_ANISO: a floor under the game's. Applies at once.",
      "TOTK_ANISO：不低於遊戲自帶的值。立即生效。")
PC_TR("The game's",
      "遊戲自帶")
PC_TR("Texture LOD bias",
      "紋理 LOD 偏移")
PC_TR("TOTK_LOD_BIAS: below 0 is sharper. Applies at once.",
      "TOTK_LOD_BIAS：小於 0 更銳利。立即生效。")
PC_TR("Compact textures (low VRAM)",
      "緊湊紋理（視訊記憶體不足）")
PC_TR("LARECOMP_ASTC_BC3: the game's textures kept in a quarter of the video memory (BC3), with slightly softer colour detail. Automatic: on only for graphics cards with 4 GB or less. Takes effect at the next start.",
      "LARECOMP_ASTC_BC3：遊戲紋理只佔四分之一的視訊記憶體（BC3），細微的色彩細節略微變柔和。自動：僅在 4 GB 或更少視訊記憶體的顯示卡上開啟。下次啟動時生效。")
PC_TR("Automatic (on with 4 GB of VRAM or less)",
      "自動（視訊記憶體 4 GB 或更少時開啟）")
PC_TR("For graphics cards with 4 GB of video memory or less: the textures take a quarter of the memory, so the game no longer runs out of it (stutters, blurry or late textures). Fine colour detail gets a little softer. Automatic turns it on only on such cards.",
      "適用於視訊記憶體 4 GB 或更少的顯示卡：紋理只佔四分之一的視訊記憶體，遊戲不再因視訊記憶體不足而卡頓、紋理模糊或延遲出現。細微的色彩細節會略微變柔和。自動僅在這類顯示卡上開啟。")
PC_TR("Scene upscaler",
      "場景放大器")
PC_TR("TOTK_SCENE_UPSCALE. Applies at once.",
      "TOTK_SCENE_UPSCALE。立即生效。")
PC_TR("TOTK_PAUSE_UNFOCUSED.",
      "TOTK_PAUSE_UNFOCUSED。")
PC_TR("Saves",
      "存檔")
PC_TR("Save backups kept",
      "存檔備份數量")
PC_TR("TOTK_SAVE_BACKUPS.",
      "TOTK_SAVE_BACKUPS。")
PC_TR("10 (default)",
      "10（預設）")
PC_TR("F12 screenshots",
      "F12 截圖")
PC_TR("TOTK_PC_SHOT: PNGs in the Screenshots folder.",
      "TOTK_PC_SHOT：PNG 儲存在 Screenshots 資料夾。")
PC_TR("Temperature unit",
      "溫度單位")
PC_TR("TOTK_TEMP_UNIT: the temperature on the Purah Pad's map. Automatic: the game's (°F in English (US), °C in the other languages). The number changes at once, the °C / °F sign beside it the next time the map is opened.",
      "TOTK_TEMP_UNIT：普爾亞平板地圖上的溫度。自動：隨遊戲（English (US) 為 °F，其他語言為 °C）。數字立即改變，旁邊的 °C / °F 符號在下次開啟地圖時改變。")
PC_TR("Automatic (by the game's language)",
      "自動（隨遊戲語言）")
PC_TR("°C (Celsius)",
      "°C（攝氏）")
PC_TR("°F (Fahrenheit)",
      "°F（華氏）")
PC_TR("Crash reports",
      "崩潰報告")
PC_TR("TOTK_PC_CRASH: a report and a minidump per crash.",
      "TOTK_PC_CRASH：每次崩潰生成一份報告和 minidump。")
PC_TR("Automatic updates",
      "自動更新")
PC_TR("Discord status",
      "Discord 狀態")
PC_TR("Shows \"Playing\", the time played and a \"Join the community\" button (the game's subreddit) on your Discord profile while Discord runs. Nothing else is sent.",
      "Discord 執行時，在你的 Discord 個人資料上顯示「正在玩」、本次遊戲時長和「Join the community」按鈕（本遊戲的 Reddit 社群）。不會傳送其他任何內容。")
PC_TR("Key bindings (Custom layout)",
      "按鍵繫結（自訂佈局）")
PC_TR("TOTK_KBM_A. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_A。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_B. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_B。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_X. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_X。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_Y. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_Y。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_LSTICK. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_LSTICK。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_RSTICK. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_RSTICK。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_L. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_L。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_R. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_R。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_ZL. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_ZL。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_ZR. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_ZR。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_PLUS. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_PLUS。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_MINUS. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_MINUS。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_DPAD_LEFT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_DPAD_LEFT。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_DPAD_UP. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_DPAD_UP。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_DPAD_RIGHT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_DPAD_RIGHT。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_DPAD_DOWN. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_DPAD_DOWN。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_MOVE_LEFT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_MOVE_LEFT。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_MOVE_UP. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_MOVE_UP。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_MOVE_RIGHT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_MOVE_RIGHT。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_MOVE_DOWN. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_MOVE_DOWN。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_CAMERA_LEFT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_CAMERA_LEFT。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_CAMERA_UP. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_CAMERA_UP。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_CAMERA_RIGHT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_CAMERA_RIGHT。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_CAMERA_DOWN. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_CAMERA_DOWN。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("TOTK_KBM_WALK. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_WALK。用於主機方案，以及遊戲中「遊戲按鍵：主機佈局」時。留空：使用 PC 佈局的按鍵。")
PC_TR("Screenshot saved: %s",
      "截圖已儲存：%s")
PC_TR("Screenshot failed (%s)",
      "截圖失敗（%s）")
PC_TR("Screenshot skipped (still saving the previous ones)",
      "已跳過截圖（仍在儲存之前的截圖）")
PC_TR("TOTK_WINDOW_MODE. F11 or Alt+Enter switch between windowed and the fullscreen kind chosen here.",
      "TOTK_WINDOW_MODE。F11 或 Alt+Enter 在視窗化與此處選擇的全螢幕類型之間切換。")
PC_TR("Monitor",
      "顯示器")
PC_TR("TOTK_MONITOR: where fullscreen goes and a windowed start opens. Remember: the monitor the game was on last (drag the window there, or pick it once), at the same window position.",
      "TOTK_MONITOR：全螢幕以及視窗化啟動時所用的顯示器。記住：遊戲上次所在的顯示器（把視窗拖過去，或選擇一次），視窗位置相同。")
PC_TR("Exclusive resolution",
      "獨佔解析度")
PC_TR("TOTK_EXCLUSIVE_SIZE: the monitor's mode in exclusive fullscreen; the list is the modes of the monitor the game started on. Borderless and windowed keep the desktop's.",
      "TOTK_EXCLUSIVE_SIZE：獨佔全螢幕時顯示器的模式；清單為遊戲啟動時所在顯示器的模式。無邊框和視窗化沿用桌面的設定。")
PC_TR("Exclusive refresh rate",
      "獨佔更新率")
PC_TR("TOTK_EXCLUSIVE_HZ: the refresh rate in exclusive fullscreen. A rate the resolution lacks gives the fastest it has under it.",
      "TOTK_EXCLUSIVE_HZ：獨佔全螢幕時的更新率。解析度不支援的更新率，會採用其下可用的最高更新率。")
PC_TR("Presentation",
      "呈現")
PC_TR("Vsync",
      "垂直同步")
PC_TR("Off: frames go out at once and may tear (LARECOMP_VK_PRESENT=immediate); on: mailbox, or FIFO with VRR.",
      "關閉：影格立即送出，可能出現撕裂（LARECOMP_VK_PRESENT=immediate）；開啟：mailbox，配合 VRR 時為 FIFO。")
PC_TR("On (default, no tearing)",
      "開啟（預設，無撕裂）")
PC_TR("Off (tearing, lowest latency)",
      "關閉（有撕裂，延遲最低）")
PC_TR("Variable refresh (VRR)",
      "可變更新率（VRR）")
PC_TR("Only with a G-SYNC / FreeSync monitor (and, windowed, the driver's windowed VRR on): the panel follows each frame (FIFO). Without one it is plain vsync at a fixed refresh, and a cap under it repeats frames. Best with a frame cap a little under the refresh (116 at 120 Hz, 138 at 144 Hz); Unlocked and DLSS frame generation keep under it by themselves.",
      "僅適用於 G-SYNC / FreeSync 顯示器（視窗化時還需開啟驅動程式的視窗化 VRR）：螢幕隨每一影格更新（FIFO）。沒有時就是固定更新率下的普通垂直同步，低於更新率的影格率上限會重複影格。最好將影格率上限設為略低於更新率（120 Hz 時 116，144 Hz 時 138）；不限影格率和 DLSS 影格生成會自行保持在更新率之下。")
PC_TR("Off (default)",
      "關閉（預設）")
PC_TR("On (G-Sync / FreeSync monitor)",
      "開啟（G-Sync / FreeSync 顯示器）")
PC_TR("Frame pacing",
      "影格節奏")
PC_TR("TOTK_PACE_AT: where the frame cap waits.",
      "TOTK_PACE_AT：影格率上限在何處等待。")
PC_TR("At the present (default, steadier)",
      "在呈現處（預設，更平穩）")
PC_TR("Before the frame (lower latency)",
      "在影格開始前（延遲更低）")
PC_TR("Game tick source",
      "遊戲節拍來源")
PC_TR("TOTK_VSYNC: what the game's 60 Hz tick follows.",
      "TOTK_VSYNC：遊戲的 60 Hz 時鐘跟隨什麼。")
PC_TR("60 Hz clock (default)",
      "60 Hz 時鐘（預設）")
PC_TR("The monitor's vblank (fixed refresh only)",
      "顯示器垂直消隱（僅固定更新率）")
PC_TR("Precise pacing",
      "精確節奏")
PC_TR("TOTK_PACE_SPIN_US: the cap's last part spins (CPU).",
      "TOTK_PACE_SPIN_US：影格率上限的最後一段採用忙等待（佔用 CPU）。")
PC_TR("Queued frames",
      "排隊影格數")
PC_TR("TOTK_MAX_QUEUED: frames waiting to be shown (needs present wait).",
      "TOTK_MAX_QUEUED：等待顯示的影格數（需要 present wait）。")
PC_TR("No limit",
      "無限制")
PC_TR("2 (default)",
      "2（預設）")
PC_TR("GPU frames in flight",
      "GPU 在途影格數")
PC_TR("TOTK_FRAMES_IN_FLIGHT.",
      "TOTK_FRAMES_IN_FLIGHT。")
PC_TR("1 (lockstep: slower when CPU and GPU are both busy)",
      "1（鎖步：CPU 和 GPU 都忙時更慢）")
PC_TR("Low latency",
      "低延遲")
PC_TR("TOTK_REFLEX: each frame is held back before it reads input, by as much as it would otherwise wait for the GPU, the render thread or the frame cap later. NVIDIA: Reflex's sleep (VK_NV_low_latency2); other GPUs, or jit: the engine's own. Boost keeps the GPU clocks up (Reflex).",
      "TOTK_REFLEX：每一影格在讀取輸入之前先被延後，延後的時長等於它之後因 GPU、渲染執行緒或影格率上限而要等待的時間。NVIDIA：使用 Reflex 的休眠（VK_NV_low_latency2）；其他 GPU 或 jit：使用引擎自帶的。Boost 讓 GPU 保持高頻（Reflex）。")
PC_TR("On (NVIDIA Reflex; other GPUs: the engine's own)",
      "開啟（NVIDIA Reflex；其他 GPU：引擎自帶）")
PC_TR("On + boost",
      "開啟 + Boost")
PC_TR("On (the engine's own, any GPU)",
      "開啟（引擎自帶，任意 GPU）")
PC_TR("Shaders",
      "著色器")
PC_TR("Shader wait",
      "著色器等待")
PC_TR("A frame that needs a shader still being built waits for it. Limit to 0.1 s: past that, the frame is shown and the objects whose shaders are not ready yet appear a moment later (fullscreen passes and compute always wait). Always wait: nothing is ever drawn incomplete, but a shader the first time can freeze the picture for a second or more. TOTK_HOLD_MAX_MS overrides.",
      "需要著色器而它仍在建置的影格會等待其完成。限制為 0.1 s：超過後先顯示該影格，著色器未就緒的物體稍後才會出現（全螢幕通道和計算始終等待）。始終等待：絕不繪製不完整的畫面，但著色器首次出現時可能讓畫面停頓一秒或更久。TOTK_HOLD_MAX_MS 可覆蓋此項。")
PC_TR("Limit to 0.1 s (default)",
      "限制為 0.1 s（預設）")
PC_TR("Always wait (nothing missing)",
      "始終等待（無缺失）")
PC_TR("HDR output",
      "HDR 輸出")
PC_TR("TOTK_HDR: needs Windows HDR on for the monitor.",
      "TOTK_HDR：需要為該顯示器開啟 Windows HDR。")
PC_TR("Off (SDR, default)",
      "關閉（SDR，預設）")
PC_TR("Peak brightness (nits)",
      "峰值亮度（nits）")
PC_TR("TOTK_HDR_PEAK: the display's peak.",
      "TOTK_HDR_PEAK：顯示器的峰值亮度。")
PC_TR("Paper white (nits)",
      "紙白亮度（nits）")
PC_TR("TOTK_HDR_PAPER: the brightness of SDR white.",
      "TOTK_HDR_PAPER：SDR 白色的亮度。")
PC_TR("Highlight colour",
      "高光顏色")
PC_TR("TOTK_HDR_HUE: the hue of bright lights from the game's HDR scene, 0 to 100.",
      "TOTK_HDR_HUE：從遊戲的 HDR 場景取回明亮光源的色調，0 到 100。")
PC_TR("Mouse cursor in menus",
      "選單中的滑鼠指標")
PC_TR("TOTK_MENU_CURSOR: in a menu the pointer shows and selects what it points at; left click is A (back is Esc).",
      "TOTK_MENU_CURSOR：在選單中顯示指標，並選取指標所指之處；左鍵為 A（返回為 Esc）。")
PC_TR("Minimum render resolution",
      "最低渲染解析度")
PC_TR("100 %: the scene is always rendered at the render resolution (dynamic resolution off). 0 %: the game's own dynamic resolution. In between: dynamic, never below that fraction.",
      "100 %：場景始終以渲染解析度渲染（動態解析度關閉）。0 %：使用遊戲自帶的動態解析度。介於兩者之間：動態，但不低於該比例。")
PC_TR("Field of view",
      "視野")
PC_TR("Multiplier of the camera's field of view (1 = the game's). The degrees are the camera's view right now: aiming and some cutscenes narrow or widen it, and the multiplier keeps their proportion. Wider costs some GPU and CPU (more of the world on screen).",
      "鏡頭視野的倍數（1 = 遊戲原值）。顯示的角度是鏡頭當前的視野：瞄準和部分過場動畫會收窄或拓寬視野，倍數會保持其比例。越寬會佔用更多 GPU 和 CPU（螢幕上顯示更多世界）。")
PC_TR("Shadow resolution",
      "陰影解析度")
PC_TR("Multiplier of the shadow map's width and height (2 = sharper, 4x the memory).",
      "陰影貼圖寬高的倍數（2 = 更清晰，記憶體為 4 倍）。")
PC_TR("Near shadow reach",
      "近處陰影範圍")
PC_TR("Multiplier of how far the sun's sharpest shadow cascades reach (1 = the game's: the first two hand over 32 m and 64 m away, where the change in sharpness shows as a line, mostly at 4K). 2 moves those lines to 64 m and 128 m; the far shadows stay. The same shadow map then covers more ground, so near shadows get softer by that factor: raise Shadow resolution with it. Costs a little GPU and CPU (more objects drawn into the near shadow maps).",
      "太陽最清晰的幾級陰影級聯所覆蓋距離的倍數（1 = 遊戲原設：前兩級在 32 米和 64 米處交接，清晰度的變化會顯示為一條線，在 4K 下尤其明顯）。2 會把這些線移到 64 米和 128 米；遠處陰影不變。同一張陰影貼圖因此覆蓋更大的地面，近處陰影會按相同倍數變柔和：請同時提高陰影解析度。會稍微增加 GPU 和 CPU 開銷（近處陰影貼圖中繪製的物體更多）。")
PC_TR("Sun shadow map size",
      "太陽陰影貼圖大小")
PC_TR("The game's: the sun's shadow map shrinks and grows with the area its cascades cover, so near the ground foliage shadows turn coarse, and every size change moves the shadow texels (leaf shadows jump instead of sliding). Fixed: always the largest size the game picks (times the shadow resolution), steady and sharper; costs some GPU.",
      "遊戲自帶：太陽陰影貼圖會隨級聯覆蓋的範圍縮放，所以近地面的植被陰影會變粗糙，且每次大小變化都會移動陰影紋素（葉影會跳動而不是平滑移動）。固定：始終使用遊戲會選用的最大尺寸（乘以陰影解析度），穩定且更清晰；會佔用一些 GPU。")
PC_TR("Detail distance",
      "細節距離")
PC_TR("Multiplier of the distance at which models switch to lower detail (2 = full detail twice as far).",
      "模型切換到較低細節的距離的倍數（2 = 完整細節延伸到兩倍遠）。")
PC_TR("Actor detail distance",
      "角色細節距離")
PC_TR("Multiplier of the distances at which characters, enemies and objects start skipping update, animation and draw frames (2 = full rate twice as far; costs CPU).",
      "角色、敵人和物體開始跳過更新、動畫和繪製影格的距離的倍數（2 = 完整影格率延伸到兩倍遠；佔用 CPU）。")
PC_TR("Terrain detail distance",
      "地形細節距離")
PC_TR("Multiplier of the distances at which the ground and water switch to coarser levels (2 = full detail twice as far; costs CPU and GPU).",
      "地面和水面切換到較粗略層級的距離的倍數（2 = 完整細節延伸到兩倍遠；佔用 CPU 和 GPU）。")
PC_TR("Simulation distance",
      "模擬距離")
PC_TR("Multiplier of the distance from the camera at which characters, enemies, objects and their physics stop being updated (1 = the game's; 1.5-2 = high). Costs CPU: more actors are simulated every frame, and distant ones keep reacting (fights, physics) instead of freezing.",
      "角色、敵人、物體及其物理停止更新時與鏡頭距離的倍數（1 = 遊戲原值；1.5-2 = 高）。佔用 CPU：每影格要模擬更多角色，遠處的也會繼續反應（戰鬥、物理），而不是靜止。")
PC_TR("Mouse camera (direct)",
      "滑鼠視角（直接）")
PC_TR("Multiplier of the camera turn the game itself applies per mouse count (on top of the mouse sensitivity). 0 = off: the mouse turns the camera through emulated right-stick deflection instead.",
      "遊戲對每個滑鼠計數自身施加的視角轉動倍數（疊加在滑鼠靈敏度之上）。0 = 關閉：滑鼠改為透過模擬的右搖桿偏轉來轉動視角。")
PC_TR("Fixed (steady)",
      "固定（穩定）")
PC_TR("The game's (changes)",
      "遊戲原樣（會變化）")
PC_TR("Game patches",
      "遊戲補丁")
PC_TR("Aspect ratio",
      "畫面比例")
PC_TR("The window's: on a 3:2, 16:10, 21:9, 32:9 or 48:9 window or monitor the game draws a canvas of that shape at start (1920x1280, 1920x1200, 3440x1440, 5120x1440 or 7680x1440), the HUD laid out for it: 21:9, 32:9 and 48:9 a wider view, 3:2 and 16:10 a taller one (the same height of view, a little less at the sides). Other shapes, and 16:9, keep the game's 16:9 picture. 16:10 / 21:9 / 32:9 / 48:9 / 3:2: that canvas whatever the window (on another shape it is shown whole, with bars; 21:9, 32:9 and 48:9 are heavier than the window's own shape: the whole canvas is drawn, then shown smaller). Needs the game patches' RomFS sets (TOTK_PATCH_SETS) and the overlay's layout for that shape. At the next start: the canvas and the game files laid out for it are chosen as the game starts. Setup prepares the layouts of this PC's monitors' shapes; another shape's are made at the start after you pick it (about 30 s, once).",
      "跟隨視窗：視窗或顯示器為 3:2、16:10、21:9、32:9 或 48:9 時，遊戲啟動時會繪製相應形狀的畫布（1920x1280、1920x1200、3440x1440、5120x1440 或 7680x1440），HUD 也按其佈局：21:9、32:9 和 48:9 視野更寬，3:2 和 16:10 更高（視野高度相同，兩側略少）。其他形狀和 16:9 保持遊戲的 16:9 畫面。16:10 / 21:9 / 32:9 / 48:9 / 3:2：無論視窗如何都使用該畫布（形狀不同時完整顯示，帶黑邊；21:9、32:9 和 48:9 比視窗自身形狀更吃效能：先繪製整個畫布，再縮小顯示）。需要遊戲補丁的 RomFS 集合（TOTK_PATCH_SETS）以及覆蓋層對應形狀的佈局。下次啟動時：遊戲啟動時選定畫布及為其排布的遊戲檔案。Setup 會為這台電腦的顯示器形狀準備佈局；其他形狀的佈局在你選擇後的下次啟動時生成（約 30 秒，僅一次）。")
PC_TR("HUD position (21:9, 32:9, 48:9)",
      "HUD 位置（21:9、32:9、48:9）")
PC_TR("Where the HUD goes on a 21:9, 32:9 or 48:9 canvas: at the screen's edges, or in the middle 16:9 (closer to the eyes on a very wide screen). Menus are the same either way. Nothing changes at 16:9. At the next start: the game loads its interface layouts once, as it starts. Automatic (the default): in the middle on 48:9, at the edges otherwise.",
      "21:9、32:9 或 48:9 畫布上 HUD 的位置：位於螢幕邊緣，或位於中間的 16:9 區域（超寬螢幕上離視線更近）。選單在兩種方式下相同。16:9 時沒有變化。下次啟動時：遊戲在啟動時載入一次介面佈局。自動（預設）：48:9 時位於中間，其他情況位於邊緣。")
PC_TR("Texture cache",
      "紋理快取")
PC_TR("Multiplier of the game's texture cache (fewer blurry textures when turning back; 490 MiB more per step). Needs the game patches' RomFS sets (TOTK_PATCH_SETS). At the next start: the game sizes its memory once, as it starts.",
      "遊戲紋理快取的倍數（回頭轉身時模糊紋理更少；每級多佔 490 MiB）。需要遊戲補丁的 RomFS 集合（TOTK_PATCH_SETS）。下次啟動時：遊戲在啟動時一次性分配記憶體。")
PC_TR("Grass density",
      "草地密度")
PC_TR("Multiplier of grass, flowers and pebbles. Needs the game patches' RomFS sets (TOTK_PATCH_SETS). At the next start: the game reads its grass table once, as it starts.",
      "草、花和卵石數量的倍數。需要遊戲補丁的 RomFS 集合（TOTK_PATCH_SETS）。下次啟動時：遊戲在啟動時讀取一次草的資料表。")
PC_TR("Shadow distance",
      "陰影距離")
PC_TR("Multiplier of the sharp shadow cascades' distances. Needs the game patches' RomFS sets (TOTK_PATCH_SETS). At the next start: the value is in the shader package, which the game reads once, as it starts.",
      "清晰陰影級聯距離的倍數。需要遊戲補丁的 RomFS 集合（TOTK_PATCH_SETS）。下次啟動時：該值位於著色器包中，遊戲在啟動時讀取一次。")
PC_TR("Fog distance",
      "霧距離")
PC_TR("Multiplier of the distance fog's start and end (clearer distant mountains). Needs the game patches' RomFS sets (TOTK_PATCH_SETS). At the next start: the game reads its sky and fog palettes once, as it starts.",
      "距離霧起點和終點的倍數（遠處的山更清晰）。需要遊戲補丁的 RomFS 集合（TOTK_PATCH_SETS）。下次啟動時：遊戲在啟動時讀取一次天空和霧的色板。")
PC_TR("Fast-travel speed",
      "快速旅行速度")
PC_TR("Fast: Link's departure when travelling is about 2.5 s shorter (its long waits halved, his dissolve as on the Switch); Normal is the game's own. Needs the game patches' RomFS sets (TOTK_PATCH_SETS). At the next start: the game loads the travel scene the first time you travel and keeps it.",
      "快速：旅行時林克的出發過程約縮短 2.5 s（長時間等待減半，其消散效果與 Switch 上相同）；常規為遊戲原樣。需要遊戲補丁的 RomFS 集合（TOTK_PATCH_SETS）。下次啟動時：遊戲在你第一次旅行時載入旅行場景並保留。")
PC_TR("Cutscenes (21:9, 32:9, 48:9)",
      "過場動畫（21:9、32:9、48:9）")
PC_TR("On a 21:9, 32:9 or 48:9 canvas, real-time cutscenes and conversations: wide like the rest of the game, or the original 16:9 framing with bars at the sides (for a scene that looks odd at the edges). Pre-rendered movies are 16:9 video: always shown in the middle with bars.",
      "在 21:9、32:9 或 48:9 畫布上，即時過場動畫和對話：與遊戲其餘部分一樣寬，或保持原始 16:9 畫面、兩側加黑邊（用於邊緣顯得奇怪的場景）。預渲染影片是 16:9 影片：始終居中顯示，帶黑邊。")
PC_TR("Upscales the 3D scene with a temporal upscaler that builds detail over several frames. DLSS: NVIDIA RTX only; DLAA: DLSS at full resolution, the best image for more GPU. FSR and XeSS: any GPU (XeSS runs fastest on Intel Arc). All cost some GPU; Off is the game's own.",
      "使用時間性放大技術放大 3D 場景，在多影格之間累積細節。DLSS：僅限 NVIDIA RTX；DLAA：全解析度的 DLSS，畫質最佳但更耗 GPU。FSR 和 XeSS：任意 GPU（XeSS 在 Intel Arc 上最快）。都會佔用一些 GPU；關閉則為遊戲自帶的方式。")
PC_TR("Safe mode (if the game crashes)",
      "安全模式（遊戲崩潰時）")
PC_TR("For a game that crashes or hangs: the renderer without its faster paths, DLSS, FSR and frame generation, and without the driver paths some GPUs crash in. Slower. At the next start.",
      "適用於崩潰或卡死的遊戲：渲染器關閉較快的路徑、DLSS、FSR 和影格生成，並避開部分 GPU 會崩潰的驅動程式路徑。速度更慢。下次啟動時生效。")
PC_TR("Wide picture: F10 > Restart the game now to use %s",
      "寬畫面：F10 > 立即重啟遊戲 以使用 %s")
PC_TR("The window changed shape: F10 > Restart the game now to fit it",
      "視窗形狀已改變：F10 > 立即重啟遊戲 以符合視窗")
PC_TR("Screen shape: start the game once from TotK Launcher.exe to prepare its layout (16:9 until then)",
      "螢幕形狀：請從 TotK Launcher.exe 啟動一次遊戲以準備其介面佈局（在此之前保持 16:9）")
PC_TR("This screen shape's layout was not prepared: 16:9 until Setup runs again",
      "此螢幕形狀的介面佈局未能準備好：在重新執行 Setup 之前保持 16:9")
PC_TR("Safe mode is on: DLSS, FSR and frame generation stay off",
      "安全模式已開啟：DLSS、FSR 和影格生成保持關閉")
PC_TR("Frame generation mode",
      "影格生成模式")
PC_TR("Smooth (matched to the refresh)",
      "平滑（匹配更新率）")
PC_TR("Maximum fps",
      "最高影格率")
PC_TR("TOTK_FG_PACE. Smooth: the frames shown match the monitor (real ones at the cap divided by the multiplier). Maximum fps: the real frames as without frame generation and the multiplier's generated ones after each, past the refresh rate, with VRR too (mailbox, else immediate, which can tear; the panel may leave VRR; TOTK_FG_MAX_VRR_FIFO=1 keeps VRR's FIFO).",
      "TOTK_FG_PACE。平滑：顯示的影格與顯示器同步（真實影格為影格率上限除以倍數）。最高影格率：真實影格與不開影格生成時相同，每個真實影格後加上倍數對應的生成影格，可超過更新率，開啟 VRR 時也是如此（使用 mailbox，沒有時用 immediate，可能出現畫面撕裂；顯示器可能退出 VRR；TOTK_FG_MAX_VRR_FIFO=1 保留 VRR 的 FIFO）。")
PC_TR("Smooth keeps the frames shown in step with the monitor's refresh. Maximum fps adds the generated frames to every real frame the PC makes, even past the refresh rate: more frames than the screen shows (the fps readout counts them all), for VRR panels or low latency; Smooth looks steadier on a fixed refresh. With V-sync off the picture can tear.",
      "平滑讓顯示的影格與顯示器更新同步。最高影格率把生成影格加到電腦生成的每個真實影格上，即使超過更新率：影格數多於螢幕能顯示的（影格率顯示會全部計入），適合 VRR 顯示器或低延遲；固定更新率下平滑看起來更穩。關閉垂直同步時畫面可能撕裂。")
PC_TR("TOTK_DLSS_SR / TOTK_FSR_SR / TOTK_XESS_SR. DLSS (NVIDIA RTX), FSR or XeSS (any GPU) upscales the 3D scene to the output in place of the game's upscaler and its FXAA (DLAA / native AA when Render resolution matches the output).",
      "TOTK_DLSS_SR / TOTK_FSR_SR / TOTK_XESS_SR。DLSS（NVIDIA RTX）、FSR 或 XeSS（任意 GPU）代替遊戲自帶的放大和 FXAA，將 3D 場景放大到輸出解析度（渲染解析度與輸出相同時為 DLAA / 原生抗鋸齒）。")
PC_TR("FSR (any GPU)",
      "FSR（任意 GPU）")
PC_TR("TOTK_DLSS_FG / TOTK_FSR_FG. Generated frames between real ones: DLSS x2 on NVIDIA RTX 40 series and newer, x3 and x4 on RTX 50; FSR x2 on any GPU. The frame cap counts the frames shown (at a 30 cap: x2, 60 shown). Adds some input latency.",
      "TOTK_DLSS_FG / TOTK_FSR_FG。在真實影格之間生成影格：DLSS x2 需 NVIDIA RTX 40 系列及更新，x3 和 x4 需 RTX 50；FSR x2 適用任意 GPU。影格率上限按顯示的影格數計算（上限 30 時：x2，顯示 60）。會增加一些輸入延遲。")
PC_TR("HDR",
      "HDR")
PC_TR("0.1% low",
      "0.1% 低影格")
PC_TR("game",
      "遊戲")
PC_TR("DLSS model",
      "DLSS 模型")
PC_TR("NVIDIA's default",
      "NVIDIA 預設")
PC_TR("Preset K",
      "預設 K")
PC_TR("Preset J",
      "預設 J")
PC_TR("TOTK_DLSS_PRESET. The AI model DLSS and DLAA use: NVIDIA's default for each quality, or preset K (sharpest) or J (a little less ghosting, a little more flicker) for all.",
      "TOTK_DLSS_PRESET。DLSS 和 DLAA 使用的 AI 模型：每種品質用 NVIDIA 預設，或全部使用預設 K（最清晰）或 J（拖影稍少，閃爍稍多）。")
PC_TR("DLSS character trails",
      "DLSS 角色拖影")
PC_TR("Fewest trails",
      "拖影最少")
PC_TR("Between",
      "適中")
PC_TR("Smoothest (more trails)",
      "最平滑（拖影較多）")
PC_TR("TOTK_DLSS_TRAILS. Where something moved (Link, animals, a swung weapon), how much DLSS and DLAA trust the new frame over the old ones: fewest trails, or smoother edges with more trails.",
      "TOTK_DLSS_TRAILS。在有物體移動的地方（林克、動物、揮動的武器），DLSS 和 DLAA 對新影格相對舊影格的信任程度：拖影最少，或邊緣更平滑但拖影更多。")
PC_TR("Which AI model DLSS and DLAA use. NVIDIA's default picks one for each quality. Preset K is the sharpest; preset J ghosts a little less behind moving things but flickers a little more. Changing it rebuilds DLSS once (a short pause).",
      "DLSS 和 DLAA 使用哪個 AI 模型。NVIDIA 預設會為每種品質選擇一個。預設 K 最清晰；預設 J 在移動物體後的拖影稍少，但閃爍稍多。更改後 DLSS 會重建一次（短暫停頓）。")
PC_TR("Where something moved (Link, animals, a swung weapon), DLSS and DLAA can keep blending in older frames, which leaves faint trails, or trust the new frame, which removes them but can make those edges flicker a little. Fewest trails is the default.",
      "在有物體移動的地方（林克、動物、揮動的武器），DLSS 和 DLAA 可以繼續混合舊影格，這會留下淡淡的拖影；也可以信任新影格，這能去除拖影，但這些邊緣可能會輕微閃爍。預設為「拖影最少」。")
PC_TR("Safe GPU mode: the graphics driver crashed in the last runs, so shaders now build the slow, safe way",
      "GPU 安全模式：圖形驅動程式在最近幾次執行中崩潰，因此著色器現在以較慢的安全方式編譯")
PC_TR("Safe GPU mode: the graphics driver kept crashing, so shaders are also built unoptimised; the game may run slower until it has run cleanly a few times",
      "GPU 安全模式：圖形驅動程式持續崩潰，因此著色器也將以未最佳化方式編譯；在遊戲正常執行幾次之前，可能會變慢")
PC_TR("XeSS (any GPU)",
      "XeSS（任意 GPU）")
PC_TR("Shader compile indicator",
      "著色器編譯提示")
PC_TR("Bottom right while shaders compile: the percentage on a first start, how many are left in play.",
      "著色器編譯時顯示在右下角：首次啟動時顯示百分比，遊戲中顯示剩餘數量。")
PC_TR("Shows when shaders are being compiled (bottom right): a short stutter while it is up is the compile, not your PC. Off hides it in play (a first start still shows its progress); the compiling still happens.",
      "在著色器編譯時顯示（右下角）：此時出現的短暫卡頓來自編譯，而不是你的電腦。關閉後在遊戲中隱藏提示（首次啟動仍會顯示進度），編譯仍會進行。")
PC_TR("Shaders left to compile:",
      "待編譯著色器：")
PC_TR("Last hitch",
      "上次卡頓")
PC_TR("No hitch yet",
      "暫無卡頓")
PC_TR("GPU wait",
      "等待 GPU")
PC_TR("shader builds",
      "著色器編譯")
PC_TR("texture uploads",
      "紋理上傳")
PC_TR("new images",
      "新建圖形")
PC_TR("file reads",
      "檔案讀取")
PC_TR("present (driver / window)",
      "呈現（驅動程式 / 視窗）")
PC_TR("game CPU",
      "遊戲 CPU")
PC_TR("render CPU",
      "渲染 CPU")
PC_TR("s ago",
      "秒前")
PC_TR("Upscaler:",
      "超解析度：")
PC_TR("off",
      "關閉")
PC_TR("plain scaling",
      "普通縮放")
PC_TR("native",
      "原生")
PC_TR("Quality",
      "品質")
PC_TR("Balanced",
      "平衡")
PC_TR("Performance",
      "效能")
PC_TR("Ultra Performance",
      "超級效能")
PC_TR("idle",
      "未執行")
PC_TR("3D output is",
      "3D 輸出為")
PC_TR("scene",
      "場景")
PC_TR("it failed to start",
      "啟動失敗")
PC_TR("starting",
      "正在啟動")
PC_TR("no 3D view this frame",
      "本影格無 3D 畫面")
PC_TR("no scene seen",
      "未見場景")
