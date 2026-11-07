// pc_text_ru.inc: the Russian of the PC port's own texts (pc_text.cpp, pc_ui_lang "ru"), one
// PC_TR(english, translation) each. A text missing here shows in English; checked by tools/engine/check_texts.py
// (the same printf specifiers, no entry the sources lack). pc-ui 11-02.
PC_TR("Audio",
      "Звук")
PC_TR("Output",
      "Вывод")
PC_TR("Speakers",
      "Колонки")
PC_TR("TOTK_AUDIO_CHANNELS. 5.1 and 7.1 tell the game it has surround output (7.1 puts the rears on the back and side pairs); Windows remaps to the device. Experimental: with 6 channels the game's effect on one sub mix diverges and is muted.",
      "TOTK_AUDIO_CHANNELS. 5.1 и 7.1 сообщают игре, что есть объёмный звук (7.1 выводит тыл на задние и боковые пары); Windows подгоняет под устройство. Экспериментально: при 6 каналах эффект игры на одном подмиксе расходится и заглушается.")
PC_TR("Stereo (default)",
      "Стерео (по умолч.)")
PC_TR("Match the Windows device (experimental)",
      "По устройству Windows (экспер.)")
PC_TR("5.1 (experimental)",
      "5.1 (экспер.)")
PC_TR("7.1 (experimental)",
      "7.1 (экспер.)")
PC_TR("Latency",
      "Задержка")
PC_TR("TOTK_AUDIO_LATENCY_MS: audio queued ahead of the device. Lower is more responsive; raise it if the sound crackles.",
      "TOTK_AUDIO_LATENCY_MS: звук, поставленный в очередь перед устройством. Меньше — отзывчивее; увеличьте, если звук трещит.")
PC_TR("20 ms (default)",
      "20 ms (по умолч.)")
PC_TR("80 ms (wireless headsets)",
      "80 ms (беспр. наушники)")
PC_TR("Follow the Windows device",
      "Следовать за Windows")
PC_TR("TOTK_AUDIO_FOLLOW: switch to the new default device (headphones plugged in or out) and recover a lost one without a restart.",
      "TOTK_AUDIO_FOLLOW: переключение на новое устройство по умолчанию (наушники вставлены или вынуты) и возврат потерянного без перезапуска.")
PC_TR("On (default)",
      "Вкл. (по умолч.)")
PC_TR("Off",
      "Выкл.")
PC_TR("Mix",
      "Микшер")
PC_TR("Music",
      "Музыка")
PC_TR("Effects",
      "Эффекты")
PC_TR("Voice",
      "Голоса")
PC_TR("TOTK_AUDIO_VOICE. No sub mix carries voice alone yet, so by default this does nothing.",
      "TOTK_AUDIO_VOICE. Пока ни один подмикс не несёт один голос, поэтому по умолчанию это ничего не меняет.")
PC_TR("the stick now",
      "стик сейчас")
PC_TR("Controls",
      "Управление")
PC_TR("Keyboard layout",
      "Раскладка клавиатуры")
PC_TR("For the console scheme, and with the PC scheme for the world while Gameplay keys is Console (menus use the PC keys). Classic: Space jump, Shift sprint, E interact, F attack, WASD, arrows camera. PC: E interact, Shift sprint, Space jump, left mouse attack, right mouse bow, Q target, F abilities. Custom: the PC keys changed by the bindings below.",
      "Для консольной схемы, а при схеме ПК — для игрового мира, пока «Клавиши в игре» — «Консольная» (меню используют клавиши ПК). Классика: Пробел — прыжок, Shift — бег, E — действие, F — атака, WASD, стрелки — камера. ПК: E — действие, Shift — бег, Пробел — прыжок, левая кнопка мыши — атака, правая — лук, Q — цель, F — способности. Свой: клавиши ПК, изменённые привязками ниже.")
PC_TR("Classic (the engine's keys)",
      "Классика (клавиши движка)")
PC_TR("PC (WASD, mouse)",
      "ПК (WASD, мышь)")
PC_TR("Custom (the bindings below)",
      "Свой (привязки ниже)")
PC_TR("Minimum movement key press",
      "Мин. нажатие клавиши движения")
PC_TR("Milliseconds a quick tap of a movement key (WASD) keeps Link moving, so he takes a step instead of sliding with his legs still. 0 = off (a tap as short as the key press).",
      "Миллисекунды, в течение которых быстрое касание клавиши движения (WASD) продолжает двигать Линка, чтобы он делал шаг, а не скользил, не двигая ногами. 0 = выкл. (касание столь же короткое, как нажатие).")
PC_TR("Sprint toggle",
      "Переключение бега")
PC_TR("A press of the sprint key starts sprinting and the next one stops it; it also stops when Link stops moving or a menu opens. Off: hold it, as on the Switch.",
      "Нажатие клавиши бега включает бег, следующее выключает; бег также прекращается, если Линк остановился или открылось меню. Выкл.: удерживать, как на Switch.")
PC_TR("On",
      "Вкл.")
PC_TR("Shield and target toggle",
      "Переключение щита и цели")
PC_TR("A press of the shield / target key raises the shield and targets, the next press lowers it; a menu lets it go too. Off: hold it, as on the Switch.",
      "Нажатие клавиши щита / цели поднимает щит и берёт цель, следующее опускает; меню тоже отпускает её. Выкл.: удерживать, как на Switch.")
PC_TR("Mouse camera",
      "Камера мышью")
PC_TR("The mouse turns the camera (click the window to capture the pointer; Esc, F10 or Alt+Tab frees it). 0 = off; above 1 turns faster.",
      "Мышь поворачивает камеру (щёлкните окно, чтобы захватить курсор; Esc, F10 или Alt+Tab освобождают его). 0 = выкл.; больше 1 — быстрее.")
PC_TR("Pad",
      "Геймпад")
PC_TR("Button prompts",
      "Подсказки кнопок")
PC_TR("The button pictures in every prompt and menu, for the keys and buttons bound. Automatic follows the device used last: keyboard and mouse, an Xbox (XInput) pad, a DualSense / DualShock 4, or the game's own with a Nintendo pad. A change shows at once, in every prompt and label; with a pad in use, menus hide the mouse pointer until the mouse moves.",
      "Картинки кнопок во всех подсказках и меню, для назначенных клавиш и кнопок. «Автоматически» следует за последним использованным устройством: клавиатура и мышь, геймпад Xbox (XInput), DualSense / DualShock 4 или значки игры при геймпаде Nintendo. Изменение видно сразу, во всех подсказках и надписях; при использовании геймпада меню скрывают курсор мыши, пока мышь не сдвинется.")
PC_TR("Automatic (the device used last)",
      "Автоматически (последнее устройство)")
PC_TR("Menu buttons",
      "Кнопки в меню")
PC_TR("Same as the game",
      "Как в игре")
PC_TR("Printed A accepts, B goes back",
      "A — принять, B — назад (по надписям)")
PC_TR("Switch layout (right accepts, bottom goes back)",
      "Как на Switch (правая — принять, нижняя — назад)")
PC_TR("The pad's four face buttons in menus, dialogues and the title screen. Same as the game: as Face buttons and the Pad buttons rows give them. Printed A accepts, B goes back: the button printed A (Cross) confirms and B (Circle) goes back, whatever jump and interact use in the world. Switch layout: the right button confirms, the bottom one goes back. Nothing changes in the world. A change applies at once.",
      "Четыре лицевые кнопки геймпада в меню, диалогах и на титульном экране. Как в игре: как их задают «Лицевые кнопки» и строки «Кнопки геймпада». A — принять, B — назад: кнопка с буквой A (Крест) подтверждает, а с буквой B (Круг) возвращает назад, какие бы кнопки ни были у прыжка и действия в мире. Как на Switch: правая кнопка подтверждает, нижняя возвращает назад. В мире ничего не меняется. Изменение действует сразу.")
PC_TR("Switch (the game's)",
      "Switch (как в игре)")
PC_TR("Keyboard and mouse",
      "Клавиатура и мышь")
PC_TR("Left mouse shoots while aiming",
      "Левая кнопка стреляет при прицеле")
PC_TR("With the bow drawn (a ZR key or button held), the left button shoots instead of attacking.",
      "С натянутым луком (удерживается клавиша или кнопка ZR) левая кнопка стреляет, а не атакует.")
PC_TR("Mouse look version",
      "Режим обзора мышью")
PC_TR("Stick",
      "Стик")
PC_TR("Direct",
      "Прямой")
PC_TR("Mouse sensitivity",
      "Чувствительность мыши")
PC_TR("Scales the camera and the motion aim.",
      "Масштабирует камеру и гироприцел.")
PC_TR("Mouse speed for a full camera turn",
      "Мышь: скорость полного поворота")
PC_TR("Mouse counts per second that turn the camera at full stick speed.",
      "Отсчётов мыши в секунду, которые поворачивают камеру на полной скорости стика.")
PC_TR("Invert mouse vertical",
      "Инверсия мыши по вертикали")
PC_TR("Mouse as motion aim",
      "Мышь как гироприцел")
PC_TR("While aiming, the mouse turns a virtual controller, so the game's motion aiming gets the mouse's precision (when no pad with motion is connected).",
      "При прицеливании мышь вращает виртуальный контроллер, и игровой гироприцел получает точность мыши (если не подключён геймпад с датчиками движения).")
PC_TR("While aiming (bow, throw)",
      "При прицеливании (лук, бросок)")
PC_TR("Always",
      "Всегда")
PC_TR("Motion aim speed",
      "Скорость гироприцела")
PC_TR("Aiming sensitivity (bow)",
      "Чувствительность прицела (лук)")
PC_TR("With the bow drawn: the mouse's turn times this (1 = as when not aiming), with or without the mouse as motion aim. A pad's motion sensitivity is separate.",
      "С натянутым луком: поворот мыши умножается на это значение (1 = как без прицеливания), с мышью как гироприцелом или без. Чувствительность движения геймпада настраивается отдельно.")
PC_TR("Mouse wheel scrolls the quick menus",
      "Колесо мыши листает быстрые меню")
PC_TR("While a D-pad quick menu is held open (weapons, shields, items, bows), each notch moves one entry.",
      "Пока открыто быстрое меню D-pad (оружие, щиты, предметы, луки), щелчок колеса сдвигает выбор на одну позицию.")
PC_TR("Map with the mouse",
      "Карта мышью")
PC_TR("On the map the mouse moves the map under the reticle (it stays at the centre) as far as the pointer would move, the wheel zooms, a left click places a pin or stamp and a right click goes back.",
      "На карте мышь двигает карту под прицелом (он остаётся по центру) настолько, насколько сдвинулся бы указатель, колесо масштабирует, левый щелчок ставит метку или штамп, правый — назад.")
PC_TR("Map cursor sensitivity",
      "Чувствительность курсора карты")
PC_TR("How far the map moves for the mouse's motion: 1 = as far as the desktop pointer would move on the screen, 2 = twice as far.",
      "Насколько карта сдвигается от движения мыши: 1 = как указатель на рабочем столе, 2 = вдвое дальше.")
PC_TR("How far the map moves under the reticle for the mouse's motion: 1 = like the desktop pointer, 2 = twice as far.",
      "Насколько карта сдвигается под прицелом от движения мыши: 1 = как указатель на рабочем столе, 2 = вдвое дальше.")
PC_TR("Menu keys",
      "Клавиши меню")
PC_TR("In the pause menu and the Purah Pad: Esc goes back, PageUp / PageDown (and the wheel in the pause menu) switch tabs, and in the pause menu clicks select (left) and go back (right).",
      "В меню паузы и в Пураншете: Esc — назад, PageUp / PageDown (и колесо в меню паузы) переключают вкладки, а в меню паузы щелчки выбирают (левый) и возвращают назад (правый).")
PC_TR("Rumble curve",
      "Кривая вибрации")
PC_TR("How light rumbles feel next to strong ones: below 1 light rumbles come through stronger, above 1 only strong ones; 1 = the game's own.",
      "Как ощущается слабая вибрация рядом с сильной: ниже 1 слабая вибрация становится сильнее, выше 1 остаётся только сильная; 1 = как в игре.")
PC_TR("Trigger rumble (Xbox)",
      "Вибрация курков (Xbox)")
PC_TR("An Xbox One or Series pad's trigger motors buzz with the game's rumble, each trigger with its side's. Off by default: the Switch pad has no trigger motors.",
      "Моторы курков геймпада Xbox One или Series вибрируют вместе с вибрацией игры, каждый курок со своей стороной. По умолчанию выключено: у геймпада Switch нет моторов в курках.")
PC_TR("Light",
      "Слабая")
PC_TR("Full strength",
      "Полная сила")
PC_TR("Camera speed (right stick)",
      "Скорость камеры (правый стик)")
PC_TR("The camera's turn with the right stick (and the camera keys) times this, at each of the game's own camera speeds, top speed included; the mouse keeps its own sensitivity. At the next start.",
      "Поворот камеры правым стиком (и клавишами камеры), умноженный на это значение, на каждой из скоростей камеры самой игры, включая максимальную; мышь сохраняет свою чувствительность. При следующем запуске.")
PC_TR("Rumble strength",
      "Сила вибрации")
PC_TR("The pad's motors (Xbox, PlayStation and Switch pads): 0 = off, 1 = the game's own.",
      "Моторы геймпада (Xbox, PlayStation и Switch): 0 = выкл., 1 = как в игре.")
PC_TR("Motion (gyro)",
      "Гироскоп")
PC_TR("The motion sensors of a DualSense, DualShock 4, Pro Controller or Joy-Con, for the game's motion aiming (the game's own Motion Controls option turns it on there). Off: the pad lies still for the game.",
      "Датчики движения DualSense, DualShock 4, Pro Controller или Joy-Con для игрового гироприцела (включается и настройкой управления движением в самой игре). Выкл.: для игры геймпад неподвижен.")
PC_TR("Motion sensitivity",
      "Чувствит. движения")
PC_TR("How far the aim turns for a turn of the pad: 1 = the pad's own, 2 = twice as far.",
      "Насколько поворачивается гироприцел при повороте геймпада: 1 = как у геймпада, 2 = вдвое сильнее.")
PC_TR("Left stick dead zone",
      "Мёртвая зона левого стика")
PC_TR("How far the left stick must move before it counts, in percent of its travel. Raise it if Link moves with the stick at rest (a worn stick); lower it for finer small steps. 0 = the pad's own (Xbox 24, PlayStation 15, Switch 12).",
      "Насколько нужно отклонить левый стик, чтобы это засчитывалось, в процентах хода. Увеличьте, если Линк движется при неотклонённом стике (износ); уменьшите для точных малых шагов. 0 = как у геймпада (Xbox 24, PlayStation 15, Switch 12).")
PC_TR("Right stick dead zone",
      "Мёртвая зона правого стика")
PC_TR("How far the right stick must move before it counts, in percent of its travel. Raise it if the camera turns with the stick at rest; lower it for finer aiming. 0 = the pad's own (Xbox 27, PlayStation 15, Switch 12).",
      "Насколько нужно отклонить правый стик, чтобы это засчитывалось, в процентах хода. Увеличьте, если камера поворачивается при неотклонённом стике; уменьшите для точного прицеливания. 0 = как у геймпада (Xbox 27, PlayStation 15, Switch 12).")
PC_TR("Walk speed",
      "Скорость шага")
PC_TR("With the walk key held (Alt in the PC layout): how far the movement keys push the stick, in its travel (1 = a full push, as without the key).",
      "С зажатой клавишей шага (Alt в раскладке ПК): насколько клавиши движения отклоняют стик (1 = до упора, как без клавиши).")
PC_TR("None",
      "Нет")
PC_TR("Left stick click",
      "Нажатие левого стика")
PC_TR("Right stick click",
      "Нажатие правого стика")
PC_TR("D-pad up",
      "D-pad вверх")
PC_TR("D-pad down",
      "D-pad вниз")
PC_TR("D-pad left",
      "D-pad влево")
PC_TR("D-pad right",
      "D-pad вправо")
PC_TR("Back paddle P1 (right, top)",
      "Задний лепесток P1 (справа, верх)")
PC_TR("The Switch button this back paddle presses: an Xbox Elite's paddles (unless the Xbox Accessories app maps them to buttons), a DualSense Edge's back buttons (the right one).",
      "Кнопка Switch, которую нажимает этот задний лепесток: лепестки Xbox Elite (если приложение «Аксессуары Xbox» не назначило их на кнопки), задние кнопки DualSense Edge (правая).")
PC_TR("Back paddle P2 (right, bottom)",
      "Задний лепесток P2 (справа, низ)")
PC_TR("The Switch button this back paddle presses: an Xbox Elite's paddles (unless the Xbox Accessories app maps them to buttons), a DualSense Edge's back buttons (none: P1 and P3 only).",
      "Кнопка Switch, которую нажимает этот задний лепесток: лепестки Xbox Elite (если приложение «Аксессуары Xbox» не назначило их на кнопки), задние кнопки DualSense Edge (нет: только P1 и P3).")
PC_TR("Back paddle P3 (left, top)",
      "Задний лепесток P3 (слева, верх)")
PC_TR("The Switch button this back paddle presses: an Xbox Elite's paddles (unless the Xbox Accessories app maps them to buttons), a DualSense Edge's back buttons (the left one).",
      "Кнопка Switch, которую нажимает этот задний лепесток: лепестки Xbox Elite (если приложение «Аксессуары Xbox» не назначило их на кнопки), задние кнопки DualSense Edge (левая).")
PC_TR("Back paddle P4 (left, bottom)",
      "Задний лепесток P4 (слева, низ)")
PC_TR("Game",
      "Игра")
PC_TR("Window",
      "Окно")
PC_TR("Pause when the window loses focus",
      "Пауза при потере фокуса окном")
PC_TR("TOTK_PAUSE_UNFOCUSED: while the game window is not in front the game stops, picture, sound and its clock, and goes on where it was (hle_oe.cpp oe_host_paused).",
      "TOTK_PAUSE_UNFOCUSED: пока окно игры не на переднем плане, игра останавливается (картинка, звук и часы) и продолжается с того же места (hle_oe.cpp oe_host_paused).")
PC_TR("Key bindings",
      "Привязки клавиш")
PC_TR("Up to 4 keys or mouse buttons. Editing one switches the layout to Custom; empty = the PC layout's.",
      "До 4 клавиш или кнопок мыши. Изменение любой переключает раскладку на «Свой»; пусто = как в раскладке ПК.")
PC_TR("A (interact)",
      "A (действие)")
PC_TR("B (sprint / back)",
      "B (бег / назад)")
PC_TR("X (jump)",
      "X (прыжок)")
PC_TR("Y (attack)",
      "Y (атака)")
PC_TR("Left stick click (crouch)",
      "Нажатие левого стика (присесть)")
PC_TR("Right stick click (look)",
      "Нажатие правого стика (обзор)")
PC_TR("L (abilities)",
      "L (способности)")
PC_TR("R (throw)",
      "R (бросок)")
PC_TR("ZL (target / shield)",
      "ZL (цель / щит)")
PC_TR("ZR (bow)",
      "ZR (лук)")
PC_TR("+ (menu)",
      "+ (меню)")
PC_TR("- (map)",
      "- (карта)")
PC_TR("D-pad left (shields)",
      "D-pad влево (щиты)")
PC_TR("D-pad up (items)",
      "D-pad вверх (предм.)")
PC_TR("D-pad right (weapons)",
      "D-pad вправо (оружие)")
PC_TR("Move left",
      "Идти влево")
PC_TR("Move forward",
      "Идти вперёд")
PC_TR("Move right",
      "Идти вправо")
PC_TR("Move back",
      "Идти назад")
PC_TR("Camera left",
      "Камера влево")
PC_TR("Camera up",
      "Камера вверх")
PC_TR("Camera right",
      "Камера вправо")
PC_TR("Camera down",
      "Камера вниз")
PC_TR("Walk (hold)",
      "Ходьба (удерж.)")
PC_TR("Pad buttons",
      "Кнопки геймпада")
PC_TR("Up to 4 pad buttons, by position: A = the right face button (Xbox B, PlayStation Circle), B = bottom (Xbox A, Cross), X = top (Xbox Y, Triangle), Y = left (Xbox X, Square). Empty = its own button; None = unbound.",
      "До 4 кнопок геймпада, по положению: A = правая кнопка (Xbox B, PlayStation «Круг»), B = нижняя (Xbox A, «Крестик»), X = верхняя (Xbox Y, «Треугольник»), Y = левая (Xbox X, «Квадрат»). Пусто = своя кнопка; «Нет» = не назначено.")
PC_TR("Skipping to Great Sky Island in %.1f s... (release Minus to cancel)",
      "Переход на Great Sky Island через %.1f с... (отпустите Минус, чтобы отменить)")
PC_TR("Skip to Great Sky Island: F2 or hold Minus (restarts into a new game)",
      "Пропуск к Great Sky Island: F2 или удерживайте Минус (перезапуск в новую игру)")
PC_TR("Skip opening: ON   (F2 / Minus to change)",
      "Пропуск вступления: ВКЛ   (F2 / Минус — изменить)")
PC_TR("Skip opening: OFF   (F2 / Minus to change)",
      "Пропуск вступления: ВЫКЛ   (F2 / Минус — изменить)")
PC_TR("Graphics settings: F10  (pad: L3 + R3)",
      "Настройки графики: F10  (геймпад: L3 + R3)")
PC_TR("Shader warm-up: %d / %d (%d %%) - press A to play now, it keeps compiling in the background",
      "Прогрев шейдеров: %d / %d (%d %%) - нажмите A, чтобы играть сейчас, компиляция продолжится в фоне")
PC_TR("Preparing the shaders for this PC (first start): %d %%",
      "Подготовка шейдеров для этого ПК (первый запуск): %d %%")
PC_TR("Compiling shaders %d %%",
      "Компиляция шейдеров %d %%")
PC_TR("Loading shaders %d %%",
      "Загрузка шейдеров %d %%")
PC_TR("(paused: low memory)",
      "(пауза: мало памяти)")
PC_TR("Windows runs your monitor at %lu Hz: set it to %lu Hz in Settings > Display > Advanced display",
      "Windows запускает монитор на %lu Hz: установите %lu Hz в Параметры > Дисплей > Дополнительные параметры дисплея")
PC_TR("It writes the NSO header.",
      "Записывает заголовок NSO.")
PC_TR("It writes past main's image (bss).",
      "Записывает за пределы образа main (bss).")
PC_TR("It changes code the port already patches (%s).",
      "Изменяет код, который порт уже патчит (%s).")
PC_TR("It overlaps %s, higher in the list.",
      "Пересекается с %s выше в списке.")
PC_TR("It is cut short at word %s.",
      "Обрывается на слове %s.")
PC_TR("It has an opcode the cheat engine does not know (%s).",
      "Содержит код операции, неизвестный движку читов (%s).")
PC_TR("It uses the alias or ASLR memory region, which the port does not have.",
      "Использует область памяти alias или ASLR, которой в порте нет.")
PC_TR("It changes the game's code only under a condition (a button, a value), which the port cannot do yet.",
      "Меняет код игры только при условии (кнопка, значение), чего порт пока не умеет.")
PC_TR("It cannot be read (%s).",
      "Невозможно прочитать (%s).")
PC_TR("%d ExeFS patches in use",
      "Используется патчей ExeFS: %d")
PC_TR("%d code patches need the game built on this PC (Builder install), not used",
      "Патчи кода (%d) требуют игры, собранной на этом ПК (установка Builder), не используются")
PC_TR("the build with its code patches failed (see exefs/%s/build.log next to totk.exe), not used",
      "сборка с патчами кода не удалась (см. exefs/%s/build.log рядом с totk.exe), патчи не используются")
PC_TR("%d code patches: the game is rebuilt with them at this start",
      "Патчей кода: %d, игра пересобирается с ними при этом запуске")
PC_TR("%d ExeFS patches for another game version, not used",
      "Патчей ExeFS для другой версии игры: %d, не используются")
PC_TR("left out: %s",
      "исключено: %s")
PC_TR("%d cheats (F10 Cheats tab)",
      "Читов: %d (вкладка «Читы» в F10)")
PC_TR("no cheat file for this game version (%s)",
      "нет файла читов для этой версии игры (%s)")
PC_TR("It changes the game's code: the game is rebuilt with it at the next start.",
      "Меняет код игры: игра пересобирается с ним при следующем запуске.")
PC_TR("Not an amiibo dump: an amiibo image is 540 bytes.",
      "Это не дамп amiibo: образ amiibo занимает 540 байт.")
PC_TR("Not an amiibo dump: the amiibo marker is missing.",
      "Это не дамп amiibo: отсутствует маркер amiibo.")
PC_TR("Not an amiibo dump: unknown figure data.",
      "Это не дамп amiibo: неизвестные данные фигурки.")
PC_TR("The file cannot be read.",
      "Файл не удаётся прочитать.")
PC_TR("amiibo: pick one in F10 (or L3 + R3) > Amiibo",
      "amiibo: выберите в F10 (или L3 + R3) > Amiibo")
PC_TR("Your own amiibo dumps (.bin, 540 bytes, or Flipper .nfc) in the amiibo folder. In the game, open its amiibo screen; when it waits for a figure, press Scan on one here.",
      "Ваши дампы amiibo (.bin, 540 байт, или Flipper .nfc) в папке amiibo. В игре откройте экран amiibo; когда он ждёт фигурку, нажмите здесь «Сканировать» на одном из них.")
PC_TR("Open amiibo folder",
      "Открыть папку amiibo")
PC_TR("The game is waiting for an amiibo.",
      "Игра ждёт amiibo.")
PC_TR("On the reader: %s",
      "На считывателе: %s")
PC_TR("Open the game's amiibo screen first: Scan works while it waits for a figure.",
      "Сначала откройте экран amiibo в игре: «Сканировать» работает, пока она ждёт фигурку.")
PC_TR("No dumps yet: put .bin or .nfc files in the amiibo folder.",
      "Дампов пока нет: положите файлы .bin или .nfc в папку amiibo.")
PC_TR("Scan",
      "Сканировать")
PC_TR("Movement",
      "Движение")
PC_TR("Jump",
      "Прыжок")
PC_TR("Sprint",
      "Бег")
PC_TR("Crouch",
      "Присесть")
PC_TR("Let go (a wall, a ride)",
      "Отпустить (стена, езда)")
PC_TR("Dive",
      "Нырок")
PC_TR("Close the paraglider",
      "Закрыть параплан")
PC_TR("Swim dash",
      "Рывок в воде")
PC_TR("Combat",
      "Бой")
PC_TR("Attack / draw the bow",
      "Атака / натянуть лук")
PC_TR("Lower the bow (cancel)",
      "Опустить лук (отмена)")
PC_TR("Next arrow material",
      "Следующий материал стрел")
PC_TR("Previous arrow material",
      "Предыдущий материал стрел")
PC_TR("Shield and lock on",
      "Щит и захват цели")
PC_TR("Put away / take out the weapon",
      "Убрать / достать оружие")
PC_TR("Take out / put away the bow",
      "Достать / убрать лук")
PC_TR("Throw",
      "Бросок")
PC_TR("Next weapon",
      "След. оружие")
PC_TR("Previous weapon",
      "Пред. оружие")
PC_TR("Construct: right arm",
      "Конструкция: правая рука")
PC_TR("Construct: left arm",
      "Конструкция: левая рука")
PC_TR("Construct: back part",
      "Конструкция: задняя часть")
PC_TR("Construct: guard",
      "Конструкция: защита")
PC_TR("Construct: attach a device",
      "Конструкция: подключить устройство")
PC_TR("Construct: get off",
      "Конструкция: слезть")
PC_TR("Items and tools",
      "Предметы и инструменты")
PC_TR("Interact",
      "Действие")
PC_TR("Melee weapons",
      "Ближнее оружие")
PC_TR("Shields",
      "Щиты")
PC_TR("Bows",
      "Луки")
PC_TR("Materials",
      "Материалы")
PC_TR("Whistle",
      "Свист")
PC_TR("Inventory",
      "Инвентарь")
PC_TR("Map",
      "Карта")
PC_TR("Adventure log",
      "Журнал приключений")
PC_TR("Pause menu",
      "Меню паузы")
PC_TR("Abilities",
      "Способности")
PC_TR("Purah Pad camera",
      "Камера Пураншета")
PC_TR("Ability wheel",
      "Колесо способностей")
PC_TR("Use the ability",
      "Применить способность")
PC_TR("Secondary action",
      "Доп. действие")
PC_TR("Pull closer",
      "Притянуть")
PC_TR("Push away",
      "Оттолкнуть")
PC_TR("Rotate",
      "Вращать")
PC_TR("Reset rotation",
      "Сбросить вращение")
PC_TR("Raise",
      "Поднять")
PC_TR("Lower",
      "Опустить")
PC_TR("Unstick",
      "Отлепить")
PC_TR("Leave the ability",
      "Выйти из способности")
PC_TR("Leave (camera, Scope, quick menu)",
      "Выйти (камера, Scope, быстрое меню)")
PC_TR("Take a photo",
      "Сделать снимок")
PC_TR("Self-portrait",
      "Селфи")
PC_TR("Camera: zoom in",
      "Камера: приблизить")
PC_TR("Camera: zoom out",
      "Камера: отдалить")
PC_TR("Place a pin (Scope)",
      "Поставить метку (Scope)")
PC_TR("Horse",
      "Лошадь")
PC_TR("Spur",
      "Шпоры")
PC_TR("Dismount",
      "Спешиться")
PC_TR("Soothe",
      "Успокоить")
PC_TR("Reset the camera",
      "Сбросить камеру")
PC_TR("Menus",
      "Меню")
PC_TR("Confirm",
      "Подтвердить")
PC_TR("Back",
      "Назад")
PC_TR("Previous tab",
      "Пред. вкладка")
PC_TR("Next tab",
      "След. вкладка")
PC_TR("Rotate left",
      "Повернуть влево")
PC_TR("Rotate right",
      "Повернуть вправо")
PC_TR("Selection up",
      "Выбор вверх")
PC_TR("Selection down",
      "Выбор вниз")
PC_TR("Selection left",
      "Выбор влево")
PC_TR("Selection right",
      "Выбор вправо")
PC_TR("D-pad up (selection)",
      "D-pad вверх (выбор)")
PC_TR("D-pad down (selection)",
      "D-pad вниз (выбор)")
PC_TR("D-pad left (selection, value)",
      "D-pad влево (выбор, значение)")
PC_TR("D-pad right (selection, value)",
      "D-pad вправо (выбор, значение)")
PC_TR("Scroll up",
      "Прокрутка вверх")
PC_TR("Scroll down",
      "Прокрутка вниз")
PC_TR("Item action (X prompt)",
      "Действие с предметом (X)")
PC_TR("Menu: ZL (previous page)",
      "Меню: ZL (пред. страница)")
PC_TR("Menu: ZR (next page)",
      "Меню: ZR (след. страница)")
PC_TR("Menu: left stick click",
      "Меню: нажатие левого стика")
PC_TR("Menu: right stick click",
      "Меню: нажатие правого стика")
PC_TR("Item action (Y prompt)",
      "Действие с предметом (Y)")
PC_TR("Recipe book: open (Food tab)",
      "Книга рецептов: открыть (Еда)")
PC_TR("Recipe book: close",
      "Книга рецептов: закрыть")
PC_TR("Recipe book: cooking history",
      "Книга рецептов: история готовки")
PC_TR("Quick menu: choose",
      "Быстрое меню: выбрать")
PC_TR("Quick menu: sort",
      "Быстрое меню: сортировка")
PC_TR("Quick menu: D-pad up",
      "Быстрое меню: D-pad вверх")
PC_TR("Quick menu: D-pad down",
      "Быстрое меню: D-pad вниз")
PC_TR("Quick menu: D-pad left",
      "Быстрое меню: D-pad влево")
PC_TR("Quick menu: D-pad right",
      "Быстрое меню: D-pad вправо")
PC_TR("Quick menu: drop the item",
      "Быстрое меню: выбросить предмет")
PC_TR("Advance dialogue",
      "Продолжить диалог")
PC_TR("Conversation log",
      "Журнал разговоров")
PC_TR("Pick the answer",
      "Выбрать ответ")
PC_TR("Answer 1",
      "Ответ 1")
PC_TR("Answer 2",
      "Ответ 2")
PC_TR("Answer 3",
      "Ответ 3")
PC_TR("Answer 4",
      "Ответ 4")
PC_TR("Advance (events)",
      "Далее (события)")
PC_TR("Skip",
      "Пропуск")
PC_TR("Skip (short scenes)",
      "Пропуск (короткие сцены)")
PC_TR("Rebinding is not available yet",
      "Переназначение пока недоступно")
PC_TR("No such action here",
      "Здесь нет такого действия")
PC_TR("A fixed control has that key",
      "Эту клавишу занимает фиксированное управление")
PC_TR("Reserved key (F10, F12, Windows, Print Screen)",
      "Зарезервированная клавиша (F10, F12, Windows, Print Screen)")
PC_TR("Confirm, back and pause must keep a key",
      "У подтверждения, возврата и паузы должна остаться клавиша")
PC_TR("That swap would clash again: choose another key",
      "Такой обмен снова вызовет конфликт: выберите другую клавишу")
PC_TR("The wheel only works for single presses",
      "Колесо работает только для одиночных нажатий")
PC_TR("Refused",
      "Отказано")
PC_TR("Swapped with ",
      "Обмен с ")
PC_TR("Stored now; acts once the PC gameplay keys are on (Controls tab).",
      "Сохранено; действует, когда «Клавиши в игре» — «ПК» (вкладка «Управление»).")
PC_TR("Press a key, mouse button or wheel (Esc: cancel)",
      "Нажмите клавишу, кнопку мыши или колесо (Esc: отмена)")
PC_TR("Change",
      "Изменить")
PC_TR("Add",
      "Добавить")
PC_TR("Default",
      "По умолч.")
PC_TR("Key moved by",
      "Клавиша перенесена:")
PC_TR("changed",
      "изменено")
PC_TR("in use by",
      "занята:")
PC_TR("settings.ini gives this key to both; it works for the other one. Change one of them.",
      "settings.ini назначает эту клавишу обоим; работает для второго. Измените одно из них.")
PC_TR("Key rebinding is not available in this build yet.",
      "Переназначение клавиш в этой сборке пока недоступно.")
PC_TR("Restore defaults",
      "Восстановить по умолчанию")
PC_TR("Cheats from your mods' cheats folders (Atmosphere format). They switch at once; one that changes the game's code once the game is built with it (at the start after its mod is turned on or changed). Cheats can break a save: keep a copy.",
      "Читы из папок читов ваших модов (формат Atmosphere). Включаются сразу; чит, меняющий код игры, действует после сборки игры с ним (при запуске после включения или изменения его мода). Читы могут испортить сохранение: сделайте копию.")
PC_TR("%s, %d x %d at %d Hz",
      "%s, %d x %d при %d Hz")
PC_TR(" (it can do %d Hz: Windows display settings)",
      " (поддерживает %d Hz: параметры дисплея Windows)")
PC_TR(" (native %d x %d)",
      " (нативное %d x %d)")
PC_TR(". HDR on in Windows",
      ". HDR включён в Windows")
PC_TR(". HDR off in Windows (the monitor can)",
      ". HDR выключен в Windows (монитор поддерживает)")
PC_TR(". No HDR",
      ". Без HDR")
PC_TR(". VRR on (%s",
      ". VRR включён (%s")
PC_TR(". VRR off in the driver (%s monitor",
      ". VRR выключен в драйвере (монитор %s")
PC_TR(". VRR: a %s monitor, not confirmed by the driver (Options: VRR on if it is on there",
      ". VRR: монитор %s, драйвер не подтверждает (Настройки: VRR вкл., если он там включён")
PC_TR(". No VRR",
      ". Без VRR")
PC_TR("Graphics",
      "Графика")
PC_TR("Rendering",
      "Рендеринг")
PC_TR("Ultrahand rotation speed",
      "Скорость вращения Комбикинеза")
PC_TR("TOTK_PC_ROTATE_SPEED: with R held in Ultrahand or Autobuild, the mouse moved sideways turns the object in 45-degree steps (1x: the move that turns the camera 45 degrees) and the wheel turns it vertically, a step a notch.",
      "TOTK_PC_ROTATE_SPEED: при удержании R в Комбикинезе или Реконструкторе движение мыши вбок поворачивает предмет шагами по 45° (1x: движение, которое поворачивает камеру на 45°), а колесо — по вертикали, шаг за щелчок.")
PC_TR("0.5x (slow)",
      "0.5x (медленно)")
PC_TR("2x (fast)",
      "2x (быстро)")
PC_TR("Automatic (a dedicated GPU first)",
      "Автоматически (дискретная первой)")
PC_TR("Display",
      "Экран")
PC_TR("Graphics card",
      "Видеокарта")
PC_TR("The graphics card the game runs on, for a laptop with two. Automatic picks the dedicated one, the faster; the integrated one uses less power but is much slower.",
      "Видеокарта, на которой работает игра, для ноутбука с двумя. «Автоматически» выбирает дискретную, более быструю; встроенная потребляет меньше, но намного медленнее.")
PC_TR("Sets every quality row below at once. Ultra is the game at its best on an 8 GB card; Low and Medium draw less than the Switch does. Changing a row by hand makes it Custom.",
      "Задаёт сразу все строки качества ниже. «Ультра» — игра в лучшем виде на карте с 8 GB; «Низкий» и «Средний» рисуют меньше, чем Switch. Изменение строки вручную переключает на «Свой».")
PC_TR("Windowed, borderless fullscreen (the default: Alt+Tab is instant) or exclusive fullscreen (the monitor switches to the resolution and refresh rate below). F11 or Alt+Enter also switch it.",
      "Окно, полноэкранный режим без рамки (по умолчанию: Alt+Tab мгновенный) или эксклюзивный полноэкранный (монитор переключается на разрешение и частоту обновления ниже). F11 или Alt+Enter тоже переключают.")
PC_TR("The window's size in windowed mode. Fullscreen uses the monitor's resolution.",
      "Размер окна в оконном режиме. Полный экран использует разрешение монитора.")
PC_TR("The monitor fullscreen uses and a windowed start opens on. Remember: the one the game was on last.",
      "Монитор, который использует полный экран и на котором открывается окно при запуске. «Запомнить»: тот, где игра была в прошлый раз.")
PC_TR("The resolution the monitor switches to in exclusive fullscreen. Borderless and windowed keep the desktop's.",
      "Разрешение, на которое переключается монитор в эксклюзивном полноэкранном режиме. Режимы без рамки и оконный сохраняют разрешение рабочего стола.")
PC_TR("The refresh rate in exclusive fullscreen. Higher is smoother when the frame rate keeps up.",
      "Частота обновления в эксклюзивном полноэкранном режиме. Выше — плавнее, если частота кадров успевает.")
PC_TR("The most frames per second the game shows. Higher is smoother and costs more GPU and CPU; 30 is the Switch's. Unlocked follows the monitor's refresh rate (240 at most); steady holds a rate the scene can keep. For an even picture pick a cap that divides the refresh rate (30 or 60 on 60 Hz, 40 on 120 Hz), or turn VRR on. Steam Deck: 60 (OLED: 90) at first; if it is not held, lower it to 40 with the screen at 40 Hz (Quick Access > Performance).",
      "Максимум кадров в секунду, которые показывает игра. Выше — плавнее, но больше нагрузка на GPU и CPU; 30 — как на Switch. «Без лимита» следует частоте обновления монитора (максимум 240); «Без лимита, стабильно» держит частоту, которую сцена может выдержать. Для ровной картинки выберите лимит, на который делится частота обновления (30 или 60 при 60 Гц, 40 при 120 Гц), или включите VRR. Steam Deck: сначала 60 (OLED: 90); если не держится, снизьте до 40 и поставьте экран на 40 Гц (Быстрый доступ > Производительность).")
PC_TR("Frame rate \"Unlocked, steady\" only: the lowest rate it holds. 30 (default) suits most PCs. On one that cannot hold 30, 20 or 15 show every frame for the same time (3 or 4 refreshes on 60 Hz) instead of an uneven 15-20.",
      "Только для частоты кадров «Без лимита, стабильно»: наименьшая частота, которую он держит. 30 (по умолчанию) подходит большинству ПК. На ПК, не тянущем 30, при 20 или 15 каждый кадр показывается одинаковое время (3 или 4 обновления при 60 Hz) вместо неровных 15-20.")
PC_TR("%d fps on this %d Hz monitor shows frames unevenly (one refresh, then two). Even: %s, steady, VRR on, or the monitor at %d Hz or a multiple (Steam Deck: Quick Access > Performance).",
      "%d кадр/с на этом мониторе %d Гц показываются неравномерно (одно обновление, затем два). Равномерно: %s, «стабильно», VRR вкл. или монитор на %d Гц или кратной частоте (Steam Deck: Быстрый доступ > Производительность).")
PC_TR("uneven",
      "неровно")
PC_TR("On (default): no tearing. Off: slightly less input latency, with tearing lines.",
      "Вкл. (по умолчанию): без разрывов. Выкл.: чуть меньше задержка ввода, но с линиями разрывов.")
PC_TR("Variable refresh rate (G-SYNC, FreeSync): the monitor waits for each frame, smooth below its refresh rate and without tearing. Needs a VRR monitor with VRR on in Windows or the driver.",
      "Переменная частота обновления (G-SYNC, FreeSync): монитор ждёт каждый кадр, плавно ниже своей частоты и без разрывов. Нужен VRR-монитор с включённым VRR в Windows или драйвере.")
PC_TR("Shows fps, the 1% low and a frame-time graph over the game (F9), to check performance.",
      "Показывает fps, 1% low и график времени кадра поверх игры (F9) для проверки производительности.")
PC_TR("A small frame-rate counter in a corner (F3 shows or hides it). Minimal: fps; Detailed adds the frame time, the 1% low and the worst frame. Off costs nothing.",
      "Небольшой счётчик fps в углу (F3 показывает или скрывает). «Минимум»: fps; «Подробно» добавляет время кадра, 1% low и худший кадр. «Выкл.» ничего не стоит.")
PC_TR("The counter's corner. Top right keeps clear of the hearts and the map.",
      "Угол счётчика. Справа вверху не закрывает сердца и карту.")
PC_TR("The counter's size. It also grows with the window's height.",
      "Размер счётчика. Он также растёт вместе с высотой окна.")
PC_TR("Colour adjustment of the game's picture (not these menus). Off shows the game's own colours and costs nothing; Vivid is livelier; Brighter dark tones lightens caves, the Depths and nights; Custom uses the four rows below.",
      "Цветокоррекция картинки игры (не этих меню). «Выкл.» показывает цвета игры и ничего не стоит; «Сочные» — живее; «Светлее тёмные тона» осветляет пещеры, Depths и ночи; «Свой» использует четыре строки ниже.")
PC_TR("Custom only: makes every colour brighter or darker; 0 is the game's.",
      "Только для «Свой»: делает все цвета ярче или темнее; 0 — как в игре.")
PC_TR("Custom only: more or less difference between light and dark; 1 is the game's.",
      "Только для «Свой»: больше или меньше разница между светлым и тёмным; 1 — как в игре.")
PC_TR("Custom only: how colourful the picture is; 1 is the game's, 0.5 greyer, 2 twice as colourful.",
      "Только для «Свой»: насколько цветная картинка; 1 — как в игре, 0.5 — серее, 2 — вдвое цветнее.")
PC_TR("Custom only: below 1 brightens the dark tones, above 1 darkens them; 1 is the game's.",
      "Только для «Свой»: меньше 1 осветляет тёмные тона, больше 1 затемняет; 1 — как в игре.")
PC_TR("The resolution the 3D scene is drawn at, against the game's own. Higher is sharper and costs much more GPU; lower is faster and blurrier. The default: the window's resolution.",
      "Разрешение, в котором рисуется 3D-сцена, относительно разрешения игры. Выше — чётче, но намного больше нагрузка на GPU; ниже — быстрее и размытее. По умолчанию: разрешение окна.")
PC_TR("Lowers the 3D resolution only when the GPU falls behind, never below the floor chosen, to hold the frame rate. Off keeps the picture sharp all the time; On is smoother on a slower GPU.",
      "Снижает 3D-разрешение, только когда GPU не успевает, но не ниже выбранного минимума, чтобы удержать частоту кадров. «Выкл.» сохраняет чёткую картинку всегда; «Вкл.» плавнее на слабом GPU.")
PC_TR("Above 1080p, menus, text and the HUD are drawn at the window's resolution instead of the game's 1080p stretched, and the 3D scene (render scale, DLSS / DLAA) reaches the window's resolution too. Sharper picture for a little GPU.",
      "Выше 1080p меню, текст и HUD рисуются в разрешении окна, а не растягиваются из 1080p игры, и 3D-сцена (разрешение рендера, DLSS / DLAA) тоже доходит до разрешения окна. Чётче картинка за небольшую нагрузку на GPU.")
PC_TR("Generated frames between the real ones make motion look smoother, but the game itself does not run faster and input latency rises a little. DLSS x2: RTX 40 and newer, x3 and x4: RTX 50; FSR x2: any GPU. Best when the game already runs well.",
      "Сгенерированные кадры между реальными делают движение плавнее, но сама игра быстрее не работает, а задержка ввода немного растёт. DLSS x2: RTX 40 и новее, x3 и x4: RTX 50; FSR x2: любой GPU. Лучше всего, когда игра и так идёт хорошо.")
PC_TR("How the 3D scene is scaled up to the output: Bicubic (the port's) or FSR 1 (the game's own). Matters only below 100 % render resolution; either costs little.",
      "Как 3D-сцена масштабируется до вывода: «Бикубический» (порта) или «FSR 1 (как в игре)». Важно только при разрешении рендера ниже 100 %; оба стоят недорого.")
PC_TR("The filter that scales the 3D scene when the render resolution is not 100 %: Catmull-Rom, or FSR 1 EASU (sharper edges). Either costs little.",
      "Фильтр, масштабирующий 3D-сцену, когда разрешение рендера не 100 %: Catmull-Rom или FSR 1 EASU (чётче края). Оба стоят недорого.")
PC_TR("How the finished picture is scaled to the window: Bilinear, or FSR 1 for a window above 1080p (sharper). Costs little.",
      "Как готовая картинка масштабируется до окна: «Билинейный» или «FSR 1» для окна выше 1080p (чётче). Стоит недорого.")
PC_TR("Smooths jagged edges on the final picture. SMAA 1x: smoother edges for a little GPU; Off is the game's own look.",
      "Сглаживает неровные края на итоговой картинке. SMAA 1x: более гладкие края за небольшую нагрузку на GPU; Выкл. — вид игры.")
PC_TR("Sharpens the final picture (AMD RCAS). More is crisper but can look grainy; Off is the game's look. Costs very little.",
      "Повышает резкость итоговой картинки (AMD RCAS). Больше — чётче, но может выглядеть зернисто; Выкл. — вид игры. Стоит очень мало.")
PC_TR("Keeps ground textures sharp when seen at a shallow angle. Higher is sharper for a little GPU; 16x suits most GPUs. The game's: its own setting.",
      "Сохраняет чёткость текстур земли под острым углом. Выше — чётче за небольшую нагрузку на GPU; 16x подходит большинству GPU. «Как в игре»: собственная настройка игры.")
PC_TR("Which texture detail level is used; 0 is the game's. Below 0 is sharper but can shimmer; above 0 is blurrier and can break textures (red grass). Not a quality slider: leave it at 0.",
      "Какой уровень детализации текстур используется; 0 — как в игре. Ниже 0 — чётче, но может мерцать; выше 0 — размытее и может ломать текстуры (красная трава). Это не ползунок качества: оставьте 0.")
PC_TR("The resolution of the renderer's shadow maps: 1x is the Switch's, 2x (default) sharper, 3x and 4x sharper still for more GPU time and memory. With little video memory they are lowered to what fits (4 GB cards: 2x).",
      "Разрешение карт теней рендерера: 1x — как на Switch, 2x (по умолчанию) чётче, 3x и 4x ещё чётче за больше времени GPU и памяти. При малом объёме видеопамяти они снижаются до того, что помещается (карты на 4 ГБ: 2x).")
PC_TR("Sun shadows grow softer the farther they fall from what casts them, as in real light. Off is the game's look; on costs some GPU. A change rebuilds the shadow shaders in the background: the old shadows show until then.",
      "Тени от солнца становятся мягче по мере удаления от отбрасывающего их предмета, как при реальном свете. Выкл. — вид игры; вкл. нагружает GPU. Изменение пересобирает шейдеры теней в фоне: до этого видны старые тени.")
PC_TR("Adds the small sun shadows the shadow maps miss: at feet, under stones, in grass. Off is the game's look; Subtle to Strong cost some GPU.",
      "Добавляет мелкие тени от солнца, которые карты теней пропускают: у ног, под камнями, в траве. «Выкл.» — вид игры; «Слабые» и «Сильные» нагружают GPU.")
PC_TR("Prepares the shaders shipped with this version before you meet them, so new places stutter less the first time. Off: only the shaders this PC has built itself. On is better.",
      "Заранее готовит шейдеры, поставляемые с этой версией, чтобы в новых местах при первой встрече было меньше рывков. Выкл.: только шейдеры, собранные самим этим ПК. Вкл. лучше.")
PC_TR("A shader still being built: Limit to 0.1 s (default) shows the frame after 0.1 s and the missing objects appear a moment later; Always wait never shows anything incomplete but can freeze the picture for a second or more.",
      "Шейдер ещё собирается: «Лимит 0.1 с» (по умолчанию) показывает кадр через 0.1 с, а недостающие объекты появляются чуть позже; «Всегда ждать» ничего неполного не показывает, но может замораживать картинку на секунду и более.")
PC_TR("Where the frame cap waits: at the present (default, steadier frame times) or before the frame (slightly less input latency).",
      "Где ждёт лимит кадров: при выводе (по умолчанию, стабильнее время кадра) или перед кадром (чуть меньше задержка ввода).")
PC_TR("What the game's 60 Hz tick follows: a 60 Hz clock (default, right for any monitor) or the monitor's vblank (a fixed-refresh monitor only). Leave the default.",
      "За чем следует такт игры 60 Hz: за часами 60 Hz (по умолчанию, подходит любому монитору) или за vblank монитора (только монитор с фиксированной частотой). Оставьте по умолчанию.")
PC_TR("The last part of the frame cap's wait busy-waits for exact timing: steadier frame times for some CPU. Off is the default.",
      "Последняя часть ожидания лимита кадров крутится в активном ожидании ради точного тайминга: стабильнее время кадра за часть CPU. По умолчанию выкл.")
PC_TR("How many finished frames may wait to be shown. 1 lowers input latency, 3 evens out uneven frames; 2 is the default.",
      "Сколько готовых кадров может ждать показа. 1 снижает задержку ввода, 3 сглаживает неровные кадры; по умолчанию 2.")
PC_TR("How many frames the CPU may prepare ahead of the GPU. 2 (default) is the balance; 1 lowers latency but is slower when both are busy; 3 adds latency.",
      "Сколько кадров CPU может подготовить вперёд GPU. 2 (по умолчанию) — баланс; 1 снижает задержку, но медленнее, когда заняты оба; 3 добавляет задержку.")
PC_TR("Holds each frame back just before it reads your input, so it waits less afterwards: lower input latency. NVIDIA uses Reflex; Boost also keeps the GPU clocks up (more power).",
      "Задерживает каждый кадр прямо перед чтением ввода, чтобы потом он ждал меньше: ниже задержка ввода. NVIDIA использует Reflex; Boost также удерживает частоты GPU (больше энергии).")
PC_TR("HDR output for an HDR monitor (HDR10 or scRGB). Needs HDR on for that monitor in Windows; otherwise the picture stays SDR.",
      "HDR-вывод для HDR-монитора (HDR10 или scRGB). Нужен включённый HDR для этого монитора в Windows; иначе картинка остаётся SDR.")
PC_TR("Your monitor's peak brightness in nits, so highlights are neither clipped nor dim. Use the value the monitor states.",
      "Пиковая яркость монитора в nits, чтобы света не обрезались и не тускнели. Берите значение, указанное для монитора.")
PC_TR("How bright ordinary white (menus, the sky) is in HDR. Raise it if the picture looks dim, lower it if it is too bright.",
      "Яркость обычного белого (меню, небо) в HDR. Повысьте, если картинка тусклая, понизьте, если слишком яркая.")
PC_TR("How much of their real colour bright lights keep in HDR: a torch flame or lava stays orange instead of turning yellow-white. 0 (the default) shows the picture as in SDR, only brighter; the HUD is left as it is.",
      "Насколько яркие источники света сохраняют свой настоящий цвет в HDR: пламя факела или лава остаются оранжевыми, а не становятся желтовато-белыми. При 0 (по умолчанию) картинка как в SDR, только ярче; HUD остаётся как есть.")
PC_TR("The window's (the default): on a 3:2, 16:10, 21:9, 32:9 or 48:9 window or monitor the picture takes that shape, 21:9, 32:9 and 48:9 with a wider view; other shapes, 16:9 among them, keep the game's own. 16:9 / 16:10 / 21:9 / 32:9 / 48:9 / 3:2: that shape whatever the window. A new shape's layouts are made at its first start (about 30 s, once). A wider picture costs more GPU.",
      "«Как у окна» (по умолчанию): в окне или на мониторе 3:2, 16:10, 21:9, 32:9 или 48:9 картинка принимает эту форму, 21:9, 32:9 и 48:9 — с более широким обзором; другие формы, в том числе 16:9, оставляют картинку игры. 16:9 / 16:10 / 21:9 / 32:9 / 48:9 / 3:2: эта форма при любом окне. Интерфейс для новой формы готовится при её первом запуске (около 30 с, один раз). Более широкая картинка сильнее нагружает GPU.")
PC_TR("On 21:9, 32:9 or 48:9: the HUD at the screen's edges, or in the middle 16:9 (easier to see on a very wide screen). Nothing changes at 16:9. Automatic (the default): the middle on 48:9, the edges otherwise.",
      "На 21:9, 32:9 или 48:9: HUD по краям экрана или посередине в 16:9 (удобнее на очень широком экране). На 16:9 ничего не меняется. Автоматически (по умолчанию): по центру на 48:9, по краям на остальных.")
PC_TR("On 21:9, 32:9 or 48:9: real-time cutscenes wide like the rest of the game, or the original 16:9 with bars at the sides.",
      "На 21:9, 32:9 или 48:9: заставки в реальном времени широкие, как остальная игра, или оригинальные 16:9 с полосами по бокам.")
PC_TR("Widens or narrows the camera's view (1 = the game's). Wider shows more of the world and costs some GPU and CPU.",
      "Расширяет или сужает обзор камеры (1 = как в игре). Шире — видно больше мира, но нагрузка на GPU и CPU растёт.")
PC_TR("More memory for textures: fewer blurry textures when you turn around, for about 490 MiB of video memory per step.",
      "Больше памяти для текстур: меньше размытых текстур при повороте, около 490 MiB видеопамяти за шаг.")
PC_TR("More grass, flowers and pebbles. Higher looks fuller and costs GPU and CPU.",
      "Больше травы, цветов и камешков. Выше — гуще, но нагружает GPU и CPU.")
PC_TR("Sharper shadows: 2 doubles the shadow map's width and height, for 4x its memory and more GPU.",
      "Чётче тени: 2 удваивает ширину и высоту карты теней, то есть в 4x больше памяти и больше нагрузка на GPU.")
PC_TR("Moves the lines where the sun's sharp near shadows hand over to coarser ones further away (1 = the game's, 2 = twice as far). Near shadows get a little softer: raise Shadow resolution with it. Costs a little GPU and CPU. With Shadow distance above 1 the two multiplied stay at 4 at most, so at Shadow distance 4 this does nothing.",
      "Отодвигает линии, где чёткие ближние тени от солнца сменяются более грубыми (1 = как в игре, 2 = вдвое дальше). Ближние тени становятся немного мягче: повышайте вместе с этим Разрешение теней. Немного нагружает GPU и CPU. Если «Дальность теней» больше 1, их произведение не превышает 4, так что при «Дальности теней» 4 этот параметр ничего не меняет.")
PC_TR("Fixed keeps the sun's shadow map at its largest size: steadier, sharper leaf shadows for some GPU. The game's changes its size with the view.",
      "«Фиксированный» держит карту теней солнца максимального размера: стабильнее и чётче тени листвы за часть GPU. «Как в игре» меняет её размер вместе с обзором.")
PC_TR("How far the sharp shadows reach. Higher looks better in the distance and costs GPU.",
      "Как далеко достигают чёткие тени. Выше — лучше вдали, но нагружает GPU.")
PC_TR("Pushes the distance fog back: clearer far mountains. Higher shows more and costs some GPU.",
      "Отодвигает туман вдали: дальние горы чище. Выше — видно больше, но нагружает GPU.")
PC_TR("How far models keep their full detail. Higher looks better in the distance and costs GPU and CPU.",
      "Как далеко модели сохраняют полную детализацию. Выше — лучше вдали, но нагружает GPU и CPU.")
PC_TR("How far characters, enemies and objects keep full animation. Higher is smoother in the distance and costs CPU.",
      "Как далеко персонажи, враги и предметы сохраняют полную анимацию. Выше — плавнее вдали, но нагружает CPU.")
PC_TR("How far the ground and water keep their full detail. Higher looks better and costs CPU and GPU.",
      "Как далеко земля и вода сохраняют полную детализацию. Выше — лучше, но нагружает CPU и GPU.")
PC_TR("How far from the camera characters, enemies and physics keep being simulated (1 = the game's). Higher keeps distant things alive and costs CPU.",
      "На каком расстоянии от камеры персонажи, враги и физика продолжают симулироваться (1 = как в игре). Выше — дальние объекты остаются живыми, но растёт нагрузка на CPU.")
PC_TR("Fast: Link's departure when travelling is about 2.5 s shorter. Normal is the game's own.",
      "«Быстрая»: отправление Линка при быстром перемещении короче примерно на 2.5 с. «Обычная» — как в игре.")
PC_TR("The game's overall volume, on top of the Windows volume.",
      "Общая громкость игры поверх громкости Windows.")
PC_TR("The music's volume.",
      "Громкость музыки.")
PC_TR("The sound effects' volume.",
      "Громкость звуковых эффектов.")
PC_TR("The voices' volume. No part of the game's mix carries voices alone yet, so this does nothing for now.",
      "Громкость голосов. Ни одна часть микса игры пока не несёт голоса отдельно, поэтому сейчас это ничего не меняет.")
PC_TR("The speakers the game mixes for: stereo, or 5.1 / 7.1 surround (experimental). Pick what your speakers or headphones use.",
      "Колонки, под которые игра микширует звук: стерео или объёмный 5.1 / 7.1 (экспериментально). Выберите то, что используют ваши колонки или наушники.")
PC_TR("How much sound is queued ahead. Lower responds sooner; raise it if the sound crackles.",
      "Сколько звука ставится в очередь заранее. Меньше — быстрее отклик; увеличьте, если звук трещит.")
PC_TR("The sound moves to headphones or speakers as you plug them in or out, without a restart.",
      "Звук переходит на наушники или колонки при их подключении и отключении, без перезапуска.")
PC_TR("No sound while the game window is not in front; the game keeps running.",
      "Нет звука, пока окно игры не на переднем плане; игра продолжает работать.")
PC_TR("PC: keyboard and mouse controls in the menus (Q / E tabs, Esc back, the mouse pointer) and PC keys in the prompts. Console: the Switch layout on the keyboard everywhere.",
      "ПК: управление клавиатурой и мышью в меню (вкладки Q / E, Esc назад, курсор мыши) и клавиши ПК в подсказках. Консоль: раскладка Switch на клавиатуре везде.")
PC_TR("PC (default): one key per action (Space jumps, Shift sprints, F attacks...). Console layout: the old keys in the world. Menus use the PC keys either way.",
      "ПК (по умолчанию): одна клавиша на действие (Пробел — прыжок, Shift — бег, F — атака...). Консольная: старые клавиши в мире. Меню в любом случае используют клавиши ПК.")
PC_TR("The keys of the Console scheme: Classic, PC (WASD), or Custom (the key bindings below).",
      "Клавиши консольной схемы: Классика, ПК (WASD) или Свой (привязки клавиш ниже).")
PC_TR("The button pictures in prompts and menus. Automatic follows the device you used last: keyboard and mouse, Xbox, PlayStation or Nintendo.",
      "Картинки кнопок в подсказках и меню. «Автоматически» следует за последним использованным устройством: клавиатура и мышь, Xbox, PlayStation или Nintendo.")
PC_TR("The pad's face buttons in menus and dialogues: as the game, printed A accepts and B goes back, or the Switch layout (right accepts). The world keeps its layout.",
      "Лицевые кнопки геймпада в меню и диалогах: как в игре, A — принять и B — назад по надписям, или как на Switch (правая — принять). В мире раскладка не меняется.")
PC_TR("In menus a mouse pointer shows and selects what it points at; left click is A, Esc goes back.",
      "В меню показывается курсор мыши и выбирает то, на что указывает; левый щелчок — A, Esc — назад.")
PC_TR("In the pause menu and the Purah Pad: Esc goes back, PageUp / PageDown or the wheel switch tabs, left click selects and right click goes back.",
      "В меню паузы и в Пураншете: Esc — назад, PageUp / PageDown или колесо переключают вкладки, левый щелчок выбирает, правый — назад.")
PC_TR("How fast the mouse turns the camera (click the window to capture the pointer; Esc, F10 or Alt+Tab frees it). 0 = off; above 1 turns faster.",
      "Как быстро мышь поворачивает камеру (щёлкните окно, чтобы захватить курсор; Esc, F10 или Alt+Tab освобождают его). 0 = выкл.; больше 1 — быстрее.")
PC_TR("How fast the mouse turns the camera and aims. Higher turns faster.",
      "Как быстро мышь поворачивает камеру и целится. Выше — быстрее.")
PC_TR("The mouse's speed while the bow is drawn, times the usual one (1 = the same). Lower aims more finely.",
      "Скорость мыши при натянутом луке относительно обычной (1 = та же). Ниже — точнее прицел.")
PC_TR("How much mouse movement a second counts as the stick pushed all the way. Lower turns faster.",
      "Сколько движения мыши в секунду считается полным отклонением стика. Ниже — быстрее поворот.")
PC_TR("Moving the mouse up looks down, as in flight games.",
      "Движение мыши вверх — взгляд вниз, как в авиасимуляторах.")
PC_TR("The mouse buttons aim and shoot.",
      "Кнопки мыши целятся и стреляют.")
PC_TR("The mouse stands in for a pad's motion aim: while aiming, always, or off.",
      "Мышь заменяет гироприцел геймпада: при прицеливании, всегда или выкл.")
PC_TR("How far motion aim turns for a mouse movement. Higher turns further.",
      "На сколько поворачивается гироприцел при движении мыши. Выше — поворот сильнее.")
PC_TR("While a D-pad quick menu is open (weapons, shields, items, bows), each wheel notch moves one entry.",
      "Пока открыто быстрое меню на D-pad (оружие, щиты, предметы, луки), каждый щелчок колеса сдвигает выбор на одну позицию.")
PC_TR("On the map the mouse pans, the wheel zooms, a left click places a pin and a right click goes back.",
      "На карте мышь двигает её, колесо масштабирует, левый щелчок ставит метку, правый возвращает назад.")
PC_TR("With the walk key held (Alt in the PC layout), how far the movement keys push the stick: lower walks slower; 1 is as without the key.",
      "С зажатой клавишей шага (Alt в раскладке ПК) — насколько клавиши движения отклоняют стик: меньше — шаг медленнее; 1 — как без клавиши.")
PC_TR("How long a quick tap of W, A, S or D keeps Link moving, so he takes a step instead of sliding. 0 = off.",
      "Сколько времени быстрое касание W, A, S или D продолжает двигать Линка, чтобы он делал шаг, а не скользил. 0 = выкл.")
PC_TR("One press of the sprint key starts sprinting and the next stops it. Off: hold it, as on the Switch.",
      "Одно нажатие клавиши бега включает бег, следующее выключает. Выкл.: удерживать, как на Switch.")
PC_TR("One press of the shield / target key raises the shield and targets, the next lowers it. Off: hold it, as on the Switch.",
      "Одно нажатие клавиши щита / цели поднимает щит и берёт цель, следующее опускает. Выкл.: удерживать, как на Switch.")
PC_TR("How fast moving the mouse sideways turns an object while R is held in Ultrahand or Autobuild (1x: as far as the camera turns). The wheel turns it vertically; moving the mouse up or down does not turn it.",
      "Как быстро движение мыши вбок поворачивает предмет при удержании R в Комбикинезе или Реконструкторе (1x: как поворачивается камера). Колесо поворачивает по вертикали; движение мыши вверх или вниз предмет не поворачивает.")
PC_TR("A non-Nintendo pad's face buttons: by position (the Switch layout: the right button confirms) or by label (Xbox A confirms).",
      "Лицевые кнопки не-Nintendo геймпада: «По положению» (раскладка Switch: правая кнопка подтверждает) или «По подписи» (A на Xbox подтверждает).")
PC_TR("Controller vibration: off, normal, or stronger (x2 to x4).",
      "Вибрация геймпада: выкл., обычная или сильнее (от x2 до x4).")
PC_TR("A pad's motion controls (aiming, some puzzles). Off ignores the gyro.",
      "Управление геймпада движением (прицеливание, некоторые головоломки). Выкл. игнорирует гироскоп.")
PC_TR("How far motion aim turns for a turn of the pad: 1 = the pad's own, 2 = twice as far.",
      "Насколько поворачивается гироприцел при повороте геймпада: 1 = как у геймпада, 2 = вдвое сильнее.")
PC_TR("How light rumbles feel next to strong ones: below 1 light ones come through stronger, above 1 only strong ones do. 1 = the game's own.",
      "Как слабая вибрация ощущается рядом с сильной: ниже 1 слабая становится заметнее, выше 1 остаётся только сильная. 1 = как в игре.")
PC_TR("Xbox One / Series pads: the trigger motors buzz with the game's rumble. Off by default (the Switch pad has none).",
      "Геймпады Xbox One / Series: моторы курков вибрируют вместе с вибрацией игры. По умолчанию выкл. (у геймпада Switch их нет).")
PC_TR("How fast the right stick turns the camera: 1 = the game's own, 2 = twice as fast. The mouse keeps its own sensitivity.",
      "Скорость поворота камеры правым стиком: 1 = как в игре, 2 = вдвое быстрее. У мыши своя чувствительность.")
PC_TR("How far the left stick must move before it counts. Raise it if Link moves with the stick at rest; lower it for finer steps. 0 = the pad's own.",
      "Насколько нужно отклонить левый стик, чтобы это засчитывалось. Увеличьте, если Линк движется при неотклонённом стике; уменьшите для точных шагов. 0 = как у геймпада.")
PC_TR("How far the right stick must move before it counts. Raise it if the camera turns with the stick at rest; lower it for finer aiming. 0 = the pad's own.",
      "Насколько нужно отклонить правый стик, чтобы это засчитывалось. Увеличьте, если камера поворачивается при неотклонённом стике; уменьшите для точного прицеливания. 0 = как у геймпада.")
PC_TR("The game's texts and subtitles, and these settings' own (English, Spanish, Russian, Brazilian Portuguese, Simplified Chinese, Traditional Chinese). Português (Brasil): 1.4.3's texts; with 1.0.0 they need the 1.4.3 update given to Setup (English otherwise), voices stay English. Automatic: Windows' language if the game has it, else English.",
      "Тексты и субтитры игры, а также тексты этих настроек (английский, испанский, русский, бразильский португальский, китайский упрощённый, китайский традиционный). Português (Brasil): тексты версии 1.4.3; для 1.0.0 нужно обновление 1.4.3, переданное в Setup (иначе английский); голоса остаются английскими. Автоматически: язык Windows, если он есть в игре, иначе английский.")
PC_TR("A new game skips the opening and starts on the Great Sky Island. F2 or Minus on the title screen also switch it. Game version 1.0.0 only (on 1.4.x the opening plays and the row is hidden).",
      "Новая игра пропускает вступление и начинается на Great Sky Island. F2 или Минус на титульном экране тоже переключают. Только для версии игры 1.0.0 (в 1.4.x вступление идёт как обычно, а строка скрыта).")
PC_TR("Docked (default) or handheld: handheld renders at the Switch's lower handheld resolution (faster, blurrier).",
      "«Док-станция» (по умолчанию) или «Портативный»: в портативном режиме рендер идёт в более низком портативном разрешении Switch (быстрее, размытее).")
PC_TR("Pauses the game, picture and sound, while its window is not in front.",
      "Приостанавливает игру, картинку и звук, пока её окно не на переднем плане.")
PC_TR("The temperature on the Purah Pad's map: °C or °F. Automatic follows the game's language.",
      "Температура на карте Пураншета: °C или °F. «Автоматически» следует языку игры.")
PC_TR("How many older copies of your saves are kept, in case a save goes wrong.",
      "Сколько прежних копий ваших сохранений хранится на случай, если сохранение испортится.")
PC_TR("F12 saves a screenshot as a PNG in the Screenshots folder.",
      "F12 сохраняет скриншот в PNG в папку Screenshots.")
PC_TR("After a crash, a report and a minidump are written to attach to a bug report. Nothing is sent anywhere.",
      "После сбоя записываются отчёт и minidump, чтобы приложить их к сообщению об ошибке. Ничего никуда не отправляется.")
PC_TR("Checks GitHub for a newer version when the game starts (at most once a day); asks first if the update rebuilds the game.",
      "Проверяет GitHub на новую версию при запуске игры (не чаще раза в день); если обновление пересобирает игру, сначала спрашивает.")
PC_TR("%s: get off",
      "%s: слезть")
PC_TR("Control scheme",
      "Схема управления")
PC_TR("PC: the keyboard and mouse controls of the menus (Q / E tabs, Esc back, the mouse pointer), Esc / I / M / J in the world and the prompts with PC keys. Console: the keyboard layout below in every screen.",
      "ПК: управление меню с клавиатуры и мыши (вкладки Q / E, Esc назад, указатель мыши), Esc / I / M / J в мире и подсказки с клавишами ПК. Консоль: раскладка ниже на всех экранах.")
PC_TR("PC",
      "ПК")
PC_TR("Console",
      "Консоль")
PC_TR("Gameplay keys",
      "Клавиши в игре")
PC_TR("TOTK_GAMEPLAY. With the PC scheme: PC (default) = one key per action: Space jumps, Shift sprints, F attacks, Q guards, 4-9 pick an ability, Tab opens the wheel...; Console layout = the keys as before in the world. Menus use the PC keys either way.",
      "TOTK_GAMEPLAY. Со схемой ПК: ПК (по умолчанию) = одна клавиша на действие: Пробел прыгает, Shift бежит, F атакует, Q защищает, 4-9 выбирают способность, Tab открывает колесо...; Консольная = клавиши, как раньше, в мире. Меню в любом случае используют клавиши ПК.")
PC_TR("Console layout",
      "Консольная")
PC_TR("Language",
      "Язык")
PC_TR("TOTK_LANG: the game's texts and voices' subtitles, and the PC settings' own texts (English, Spanish, Russian, Brazilian Portuguese, Simplified Chinese, Traditional Chinese). Português (Brasil): 1.4.3's texts; with 1.0.0 they need the 1.4.3 update given to Setup (English otherwise), voices stay English. Automatic: Windows' display language if the game has it, else English.",
      "TOTK_LANG: тексты игры и субтитры голосов, а также тексты настроек ПК (английский, испанский, русский, бразильский португальский, китайский упрощённый, китайский традиционный). Português (Brasil): тексты версии 1.4.3; для 1.0.0 нужно обновление 1.4.3, переданное в Setup (иначе английский); голоса остаются английскими. Автоматически: язык интерфейса Windows, если он есть в игре, иначе английский.")
PC_TR("Automatic (Windows' language)",
      "Автоматически (язык Windows)")
PC_TR("Russian",
      "Русский")
PC_TR("Japanese",
      "Японский")
PC_TR("Korean",
      "Корейский")
PC_TR("Chinese (Simplified)",
      "Китайский (упрощённый)")
PC_TR("Chinese (Traditional)",
      "Китайский (традиционный)")
PC_TR("This folder has no romfs, exefs or cheats files.",
      "В этой папке нет файлов romfs, exefs или cheats.")
PC_TR("The folder name contains |, which the mod list uses between names. Rename the folder.",
      "Имя папки содержит |, которым список модов разделяет имена. Переименуйте папку.")
PC_TR("It is made for another game (title %s).",
      "Мод сделан для другой игры (title %s).")
PC_TR("An earlier mods folder has a mod with this name; this one is left out.",
      "В более ранней папке модов есть мод с таким именем; этот пропущен.")
PC_TR("It has code modules (subsdk or main), which the port cannot load, so the whole mod stays off.",
      "В нём есть модули кода (subsdk или main), которые порт не может загрузить, поэтому мод целиком остаётся выключенным.")
PC_TR("It only has code patches (exefs) or cheats, which this version cannot use yet.",
      "В нём только патчи кода (exefs) или читы, которые эта версия пока не умеет использовать.")
PC_TR("A mod's folder",
      "Папка мода")
PC_TR("A mod (.zip, .7z or .rar)",
      "Мод (.zip, .7z или .rar)")
PC_TR("code patches",
      "патчи кода")
PC_TR("cheats",
      "читы")
PC_TR("Installed %s (%s, for %s) in %s. It loads at the next start.",
      "Установлен %s (%s, для %s) в %s. Загрузится при следующем запуске.")
PC_TR("Its files don't name a game version: it was put with the one you play.",
      "Файлы мода не указывают версию игры: он установлен для той, в которую вы играете.")
PC_TR("It is made for game version %s, which the port does not build: it was put with the 1.4.3 mods and may not work.",
      "Мод сделан для версии игры %s, которую порт не собирает: он установлен к модам 1.4.3 и может не работать.")
PC_TR("Not installed: it could not be opened. Unpack it yourself and choose its folder with Install a mod folder.",
      "Не установлен: не удалось открыть. Распакуйте его сами и выберите папку через «Установить папку мода».")
PC_TR("Not installed: it has no romfs, exefs or cheats folder. If the mod is packed inside it, unpack it and choose that folder instead.",
      "Не установлен: нет папки romfs, exefs или cheats. Если мод упакован внутри, распакуйте его и выберите эту папку.")
PC_TR("Not installed: it is a mod for another game.",
      "Не установлен: это мод для другой игры.")
PC_TR("Not installed: it loads code of its own (subsdk / main), which a recompiled game cannot load.",
      "Не установлен: он загружает собственный код (subsdk / main), который перекомпилированная игра загрузить не может.")
PC_TR("Not installed: it could not be copied. Check that the mods folder can be written (Open mods folder).",
      "Не установлен: не удалось скопировать. Проверьте, что в папку модов можно записывать («Открыть папку модов»).")
PC_TR("Mods load at the next start. The top mod wins a file two mods change; the port's own files (PC button prompts, fixes) stay over a mod's.",
      "Моды загружаются при следующем запуске. Если два мода меняют один файл, побеждает верхний; собственные файлы порта (подсказки кнопок ПК, исправления) остаются поверх файлов мода.")
PC_TR("The launcher chose these mods.",
      "Эти моды выбрал лаунчер.")
PC_TR("Open mods folder",
      "Открыть папку модов")
PC_TR("Install a mod...",
      "Установить мод...")
PC_TR("Install a mod folder...",
      "Установить папку мода...")
PC_TR("Installing...",
      "Установка...")
PC_TR("In exclusive fullscreen the game goes to the desktop while the dialog is open.",
      "В эксклюзивном полноэкранном режиме игра сворачивается, пока открыто окно выбора.")
PC_TR("No mods yet. Put each mod in a folder of its own in the mods folder (its romfs, exefs and cheats folders as emulators take them), then start the game again.",
      "Модов пока нет. Положите каждый мод в отдельную папку внутри папки модов (его папки romfs, exefs и cheats — как их берут эмуляторы), затем снова запустите игру.")
PC_TR("Ctrl+Up / Ctrl+Down (pad: Y / X) move the mod in focus.",
      "Ctrl+Вверх / Ctrl+Вниз (геймпад: Y / X) двигают выбранный мод.")
PC_TR("%d files replaced, %d added",
      "заменено файлов: %d, добавлено: %d")
PC_TR("; %d left to the port's own files",
      "; %d оставлено собственным файлам порта")
PC_TR("; %d left to %s, higher in the list",
      "; %d оставлено для %s выше в списке")
PC_TR("; %d for another game version, not used",
      "; %d для другой версии игры, не используются")
PC_TR("Processor: %s, %d cores, %lu threads. Memory: %.0f GB.",
      "Процессор: %s, ядер: %d, потоков: %lu. Память: %.0f GB.")
PC_TR("unknown",
      "неизвестно")
PC_TR("Graphics card: %s, %.0f GB.",
      "Видеокарта: %s, %.0f GB.")
PC_TR("Display: %s",
      "Экран: %s")
PC_TR("1%% low: collecting",
      "1%% low: сбор данных")
PC_TR("frame time 0-50 ms  (F9)",
      "время кадра 0-50 ms  (F9)")
PC_TR("frame",
      "кадр")
PC_TR("worst",
      "худший")
PC_TR("real",
      "реальных")
PC_TR("Press a key or mouse button (Esc: cancel)",
      "Нажмите клавишу или кнопку мыши (Esc: отмена)")
PC_TR("(the layout's keys)",
      "(клавиши раскладки)")
PC_TR("Add key",
      "Добавить клавишу")
PC_TR("Clear",
      "Очистить")
PC_TR("A (right)",
      "A (справа)")
PC_TR("B (bottom)",
      "B (снизу)")
PC_TR("X (top)",
      "X (сверху)")
PC_TR("Y (left)",
      "Y (слева)")
PC_TR("L stick",
      "Левый стик")
PC_TR("R stick",
      "Правый стик")
PC_TR("none",
      "нет")
PC_TR("Press a pad button (Esc: cancel)",
      "Нажмите кнопку геймпада (Esc: отмена)")
PC_TR("(its own button)",
      "(своя кнопка)")
PC_TR("Add button",
      "Добавить кнопку")
PC_TR("this run",
      "на этот запуск")
PC_TR("Set by NX_SET_ for this run only; not saved.",
      "Задано через NX_SET_ только на этот запуск; не сохраняется.")
PC_TR("at next start",
      "при след. запуске")
PC_TR("restart",
      "перезапуск")
PC_TR("Point at a setting, or move to it, to see what it does.",
      "Наведите курсор на настройку или перейдите к ней, чтобы узнать, что она делает.")
PC_TR("Applies when the game starts again.",
      "Применяется при следующем запуске игры.")
PC_TR("No settings on this page yet.",
      "На этой странице пока нет настроек.")
PC_TR("Reset this page to defaults",
      "Сбросить настройки страницы")
PC_TR("Settings  (F10)",
      "Настройки  (F10)")
PC_TR("Mods",
      "Моды")
PC_TR("Cheats",
      "Читы")
PC_TR("Keys",
      "Клавиши")
PC_TR("Some changes apply when the game starts again.",
      "Некоторые изменения применяются при следующем запуске игры.")
PC_TR("Restart the game now",
      "Перезапустить игру сейчас")
PC_TR("The game closes and starts again with the new settings. Progress since your last save is lost.",
      "Игра закроется и запустится заново с новыми настройками. Прогресс с последнего сохранения будет потерян.")
PC_TR("Restart",
      "Перезапуск")
PC_TR("Cancel",
      "Отмена")
PC_TR("Saved to %s",
      "Сохранено в %s")
PC_TR("Not saved (TOTK_SETTINGS=0)",
      "Не сохранено (TOTK_SETTINGS=0)")
PC_TR("Copy system info",
      "Копировать сведения")
PC_TR("Copied: paste it into your bug report.",
      "Скопировано: вставьте в отчёт об ошибке.")
PC_TR("Could not copy.",
      "Не удалось скопировать.")
PC_TR("F10, Esc, B or L3 + R3 closes. A: select. L / R: pages. F9: perf overlay.",
      "F10, Esc, B или L3 + R3 — закрыть. A: выбор. L / R: страницы. F9: perf-оверлей.")
PC_TR("Low",
      "Низкий")
PC_TR("Medium",
      "Средний")
PC_TR("High",
      "Высокий")
PC_TR("Welcome",
      "Добро пожаловать")
PC_TR("Pick graphics settings for this PC. Everything can be changed later with F10 (or L3 + R3).",
      "Выберите настройки графики для этого ПК. Всё можно изменить позже через F10 (или L3 + R3).")
PC_TR("Set for this display: frame rate %s%s.%s",
      "Настроено под этот дисплей: частота кадров %s%s.%s")
PC_TR("Unlocked (up to the refresh)",
      "Без лимита (до частоты монитора)")
PC_TR(", VRR on",
      ", VRR вкл.")
PC_TR(" HDR output (off) is in F10 > Graphics > HDR.",
      " Вывод HDR (выкл.) — в F10 > Графика > HDR.")
PC_TR("%s (recommended)",
      "%s (рекомендуется)")
PC_TR("Keep the current settings",
      "Оставить текущие настройки")
PC_TR("Some choices apply when the game starts again. Esc keeps the current settings.",
      "Некоторые варианты применяются при следующем запуске игры. Esc оставляет текущие настройки.")
PC_TR("Applies when the game starts again: F10 > Restart the game now",
      "Применяется при следующем запуске игры: F10 > Перезапустить игру сейчас")
PC_TR("Preset",
      "Пресет")
PC_TR("Graphics preset",
      "Пресет графики")
PC_TR("Sets the rows below it; any change by hand makes it Custom.",
      "Задаёт строки ниже; любое ручное изменение переключает на «Свой».")
PC_TR("Ultra",
      "Ультра")
PC_TR("Ultra+",
      "Ультра+")
PC_TR("Custom",
      "Свой")
PC_TR("First-run screen done",
      "Первый запуск пройден")
PC_TR("F10 hint shown",
      "Подсказка F10 показана")
PC_TR("Window mode",
      "Режим окна")
PC_TR("F11 or Alt+Enter also switch it.",
      "F11 или Alt+Enter тоже переключают.")
PC_TR("Windowed",
      "Оконный")
PC_TR("Borderless fullscreen (default)",
      "Полный экран без рамки (по умолч.)")
PC_TR("Exclusive fullscreen",
      "Эксклюзивный полный экран")
PC_TR("Window size",
      "Размер окна")
PC_TR("Windowed mode only (LARECOMP_WINDOW at start).",
      "Только оконный режим (LARECOMP_WINDOW при запуске).")
PC_TR("1280 x 720 (default)",
      "1280 x 720 (по умолч.)")
PC_TR("Frame rate",
      "Частота кадров")
PC_TR("TOTK_FPS / TOTK_UNLOCK_FPS. Unlocked: up to the monitor's refresh rate (60 to 240), loading screens at the game's own pace. Steady: unlocked, but held at the highest rate the scene keeps, in whole refreshes of the monitor (240, 120, 80, 60 on a 240 Hz one), instead of swinging. 40 and 45: for a PC that cannot hold 60; smooth on a monitor whose refresh they divide (40 on 120 Hz, 45 on 90 or 180 Hz) or with VRR, uneven on a fixed 60 Hz.",
      "TOTK_FPS / TOTK_UNLOCK_FPS. «Без лимита»: до частоты обновления монитора (от 60 до 240), экраны загрузки идут в темпе самой игры. «Без лимита, стабильно»: то же, но с удержанием наибольшей частоты, которую держит сцена, в целых долях обновления монитора (240, 120, 80, 60 на 240 Hz), без скачков. 40 и 45: для ПК, не тянущего 60; плавно на мониторе, частота которого на них делится (40 на 120 Hz, 45 на 90 или 180 Hz), или с VRR, неровно на фиксированных 60 Hz.")
PC_TR("30 (the game's own)",
      "30 (как в игре)")
PC_TR("Unlocked (monitor rate)",
      "Без лимита (частота монитора)")
PC_TR("Unlocked, steady (a rate it can hold)",
      "Без лимита, стабильно")
PC_TR("Steady's lowest rate",
      "Нижний предел стабильного режима")
PC_TR("TOTK_STEADY_FLOOR. Frame rate Steady only: the lowest rate it holds. 20 or 15 keep a PC that cannot hold 30 even (3 or 4 refreshes a frame on 60 Hz).",
      "TOTK_STEADY_FLOOR. Только для режима «Без лимита, стабильно»: наименьшая частота, которую он держит. 20 или 15 делают картинку ровной на ПК, не тянущем 30 (3 или 4 обновления на кадр при 60 Hz).")
PC_TR("30 (default)",
      "30 (по умолчанию)")
PC_TR("Perf overlay (F9)",
      "Perf-оверлей (F9)")
PC_TR("Fps, 1% low and the frame-time graph.",
      "Fps, 1% low и график времени кадра.")
PC_TR("Fps readout (F3)",
      "Счётчик fps (F3)")
PC_TR("TOTK_FPS_OVERLAY. F3 shows or hides it in game.",
      "TOTK_FPS_OVERLAY. F3 показывает или скрывает его в игре.")
PC_TR("Off (default; F3 shows it)",
      "Выкл. (по умолч.; F3 показывает)")
PC_TR("Minimal: fps",
      "Минимум: fps")
PC_TR("Detailed: fps, frame time, 1% low, worst",
      "Подробно: fps, кадр, 1% low, худший")
PC_TR("Fps readout corner",
      "Угол счётчика fps")
PC_TR("Top right keeps clear of the hearts and the map.",
      "Справа вверху не закрывает сердца и карту.")
PC_TR("Top right (default)",
      "Справа вверху (по умолч.)")
PC_TR("Top left",
      "Слева вверху")
PC_TR("Bottom right",
      "Справа внизу")
PC_TR("Bottom left",
      "Слева внизу")
PC_TR("Fps readout size",
      "Размер счётчика fps")
PC_TR("Follows the window height (1080p to 4K).",
      "Зависит от высоты окна (от 1080p до 4K).")
PC_TR("Small (default)",
      "Малый (по умолч.)")
PC_TR("Medium size",
      "Средний")
PC_TR("Large size",
      "Большой")
PC_TR("Render resolution",
      "Разрешение рендера")
PC_TR("TOTK_RES_SCALE, of the game's 1600 x 900. Window resolution: the 3D scene as large as the picture in the window (x0.83 of it on an integrated GPU or one under 8 GB; two thirds of it with DLSS, FSR or XeSS, their Quality). Applies at once.",
      "TOTK_RES_SCALE, от 1600 x 900 игры. Разрешение окна: 3D-сцена размером с изображение в окне (x0,83 от него на встроенной GPU или с памятью меньше 8 GB; две трети с DLSS, FSR или XeSS, их режим «Качество»). Применяется сразу.")
PC_TR("100 % (native)",
      "100 % (нативное)")
PC_TR("Window resolution (default)",
      "Разрешение окна (по умолчанию)")
PC_TR("Window resolution x0.83 (default: integrated GPU or under 8 GB)",
      "Разрешение окна x0,83 (по умолчанию: встроенная GPU или меньше 8 GB)")
PC_TR("Native-resolution interface",
      "Интерфейс в нативном разрешении")
PC_TR("TOTK_OUT_SCALE: above 1080p, menus, text and the HUD drawn at the window's resolution instead of the game's 1920 x 1080 stretched; the 3D scene reaches it too. On by default. Applies at once.",
      "TOTK_OUT_SCALE: выше 1080p меню, текст и HUD рисуются в разрешении окна, а не растягиваются из 1920 x 1080 игры; 3D-сцена тоже доходит до него. Включено по умолчанию. Применяется сразу.")
PC_TR("Dynamic resolution",
      "Динамическое разрешение")
PC_TR("Off: the scene always at the render resolution. On: the game lowers it only when the GPU is short of time, never below the chosen floor, aiming at the frame rate. Applies at once.",
      "Выкл.: сцена всегда в разрешении рендера. Вкл.: игра снижает его, только когда GPU не хватает времени, но не ниже выбранного порога, стремясь к целевой частоте кадров. Действует сразу.")
PC_TR("Off (fixed size)",
      "Выкл. (фиксированное)")
PC_TR("On, never below 90 %",
      "Вкл., не ниже 90 %")
PC_TR("On, never below 75 %",
      "Вкл., не ниже 75 %")
PC_TR("On, never below 50 %",
      "Вкл., не ниже 50 %")
PC_TR("On, the game's own (no minimum)",
      "Вкл., как в игре (без минимума)")
PC_TR("Dynamic resolution (clock)",
      "Динам. разрешение (часы)")
PC_TR("TOTK_DYNRES: the game's own, aimed at the frame rate.",
      "TOTK_DYNRES: собственное в игре, нацеленное на частоту кадров.")
PC_TR("3D upscaler",
      "Апскейлер 3D")
PC_TR("TOTK_SSAA. Applies at once.",
      "TOTK_SSAA. Применяется сразу.")
PC_TR("Bicubic (recomp)",
      "Бикубический (recomp)")
PC_TR("FSR 1 (the game's)",
      "FSR 1 (как в игре)")
PC_TR("Shipped shader list",
      "Список шейдеров из сборки")
PC_TR("TOTK_SHADER_SEED: on, the shader list that comes with the build lets the game prepare those shaders before you meet them (fewer stutters the first time). Off: only the shaders this PC has met itself. Takes effect at the next start.",
      "TOTK_SHADER_SEED: вкл. — список шейдеров из сборки позволяет игре подготовить эти шейдеры до встречи с ними (меньше подвисаний в первый раз). Выкл.: только шейдеры, которые этот ПК уже встречал сам. Действует со следующего запуска.")
PC_TR("Window upscaler",
      "Апскейлер окна")
PC_TR("Bilinear",
      "Билинейный")
PC_TR("FSR 1 (window above 1080p)",
      "FSR 1 (окно выше 1080p)")
PC_TR("Anti-aliasing",
      "Сглаживание")
PC_TR("TOTK_AA, on the presented image.",
      "TOTK_AA, на выводимом изображении.")
PC_TR("Sharpening",
      "Резкость")
PC_TR("RCAS low",
      "RCAS низкий")
PC_TR("RCAS medium",
      "RCAS средний")
PC_TR("RCAS high",
      "RCAS высокий")
PC_TR("Colour",
      "Цвет")
PC_TR("Colour adjustment",
      "Цветокоррекция")
PC_TR("TOTK_COLOR: the game's picture, its HUD included (not this menu). Off shows the game's own colours unchanged and costs nothing. Vivid: livelier colours. Brighter dark tones: caves, the Depths and nights lighter. Custom: the four rows below. Applies at once.",
      "TOTK_COLOR: картинка игры, включая HUD (но не это меню). Выкл. показывает цвета игры без изменений и ничего не стоит. Сочные: более живые цвета. Светлее тёмные тона: пещеры, Depths и ночи светлее. Свой: четыре строки ниже. Применяется сразу.")
PC_TR("Off (the game's own colours; default)",
      "Выкл. (цвета игры; по умолч.)")
PC_TR("Vivid",
      "Сочные")
PC_TR("Brighter dark tones",
      "Светлее тёмные тона")
PC_TR("Custom (the four rows below)",
      "Свой (четыре строки ниже)")
PC_TR("Brightness",
      "Яркость")
PC_TR("TOTK_COLOR_BRIGHTNESS (Custom only): added to every colour; 0 is the game's.",
      "TOTK_COLOR_BRIGHTNESS (только Свой): добавляется к каждому цвету; 0 — как в игре.")
PC_TR("Contrast",
      "Контраст")
PC_TR("TOTK_COLOR_CONTRAST (Custom only): around mid grey; 1 is the game's.",
      "TOTK_COLOR_CONTRAST (только Свой): вокруг среднего серого; 1 — как в игре.")
PC_TR("Saturation",
      "Насыщенность")
PC_TR("TOTK_COLOR_SATURATION (Custom only): 1 is the game's, 0.5 greyer, 2 twice as colourful.",
      "TOTK_COLOR_SATURATION (только Свой): 1 — как в игре, 0.5 серее, 2 вдвое красочнее.")
PC_TR("Gamma",
      "Гамма")
PC_TR("TOTK_COLOR_GAMMA (Custom only): below 1 brightens dark tones, above 1 darkens them; 1 is the game's.",
      "TOTK_COLOR_GAMMA (только Свой): ниже 1 осветляет тёмные тона, выше 1 затемняет; 1 — как в игре.")
PC_TR("Shadow map scale (renderer)",
      "Масштаб карты теней (рендерер)")
PC_TR("TOTK_SHADOW_SCALE: the renderer's shadow targets. The game patches' Shadow resolution is separate. Applies at once.",
      "TOTK_SHADOW_SCALE: цели теней рендерера. «Разрешение теней» из патчей игры — отдельная настройка. Применяется сразу.")
PC_TR("2x (default)",
      "2x (по умолч.)")
PC_TR("Soft shadows",
      "Мягкие тени")
PC_TR("TOTK_SOFT_SHADOWS: the sun's shadow edges widen with the distance to what casts them (contact-hardening, PCSS) instead of keeping one width. Off is the game's look. A change rebuilds the shadowed shaders in the background: the old shadows show until they are ready.",
      "TOTK_SOFT_SHADOWS: края теней от солнца расширяются с расстоянием до отбрасывающего их предмета (contact-hardening, PCSS), а не остаются одной ширины. Выкл. — вид игры. Смена пересобирает затенённые шейдеры в фоне: до их готовности видны старые тени.")
PC_TR("Off (the game's)",
      "Выкл. (как в игре)")
PC_TR("Soft",
      "Мягкие")
PC_TR("Softer",
      "Мягче")
PC_TR("Softest",
      "Мягчайшие")
PC_TR("Contact shadows",
      "Контактные тени")
PC_TR("TOTK_CONTACT_SHADOWS: small sun shadows at feet, under stones and in grass, which the shadow maps are too coarse for (a short march toward the sun through the scene's depth, up to 80 m). Off is the game's look; costs some GPU. Applies at once.",
      "TOTK_CONTACT_SHADOWS: мелкие тени от солнца у ног, под камнями и в траве, для которых карты теней слишком грубы (короткий трассинг к солнцу по глубине сцены, до 80 м). Выкл. — вид игры; нагружает GPU. Применяется сразу.")
PC_TR("Subtle",
      "Слабые")
PC_TR("Strong",
      "Сильные")
PC_TR("Volume",
      "Громкость")
PC_TR("TOTK_VOLUME, master gain.",
      "TOTK_VOLUME, общая громкость.")
PC_TR("Mute in the background",
      "Глушить в фоне")
PC_TR("TOTK_MUTE_UNFOCUSED: no sound while the game window is not in front; the game runs on (Pause when the window loses focus stops it too).",
      "TOTK_MUTE_UNFOCUSED: нет звука, пока окно игры не на переднем плане; игра продолжается («Пауза при потере фокуса окном» останавливает и её).")
PC_TR("Face buttons",
      "Лицевые кнопки")
PC_TR("By position (Switch layout)",
      "По положению (раскладка Switch)")
PC_TR("By label (Xbox A = A)",
      "По подписи (Xbox A = A)")
PC_TR("Rumble",
      "Вибрация")
PC_TR("Normal",
      "Обычная")
PC_TR("Motion controls",
      "Управление движением")
PC_TR("Start",
      "Начало")
PC_TR("Skip the opening",
      "Пропуск вступления")
PC_TR("TOTK_SKIP_OPENING. F2 or Minus on the title screen also toggles it.",
      "TOTK_SKIP_OPENING. F2 или Минус на титульном экране тоже переключает его.")
PC_TR("On (start at the Great Sky Island)",
      "Вкл. (старт на Great Sky Island)")
PC_TR("Console mode",
      "Консольный режим")
PC_TR("TOTK_HANDHELD: handheld renders at the lower resolution.",
      "TOTK_HANDHELD: в портативном режиме рендер идёт в меньшем разрешении.")
PC_TR("Docked",
      "Док-станция")
PC_TR("Handheld",
      "Портативный")
PC_TR("Classic",
      "Классика")
PC_TR("PC (WASD)",
      "ПК (WASD)")
PC_TR("Custom (TOTK_KBM_<ACTION>)",
      "Свой (TOTK_KBM_<ACTION>)")
PC_TR("Mouse buttons aim and shoot",
      "Кнопки мыши: прицел и выстрел")
PC_TR("Mouse counts for a full stick",
      "Отсчёты мыши на полный стик")
PC_TR("TOTK_MOUSE_STICK_SPEED (per second).",
      "TOTK_MOUSE_STICK_SPEED (в секунду).")
PC_TR("While aiming",
      "При прицеливании")
PC_TR("Anisotropic filtering",
      "Анизотропная фильтрация")
PC_TR("TOTK_ANISO: a floor under the game's. Applies at once.",
      "TOTK_ANISO: не ниже, чем в игре. Применяется сразу.")
PC_TR("The game's",
      "Как в игре")
PC_TR("Texture LOD bias",
      "Смещение LOD текстур")
PC_TR("TOTK_LOD_BIAS: below 0 is sharper. Applies at once.",
      "TOTK_LOD_BIAS: ниже 0 — чётче. Применяется сразу.")
PC_TR("Compact textures (low VRAM)",
      "Компактные текстуры (мало VRAM)")
PC_TR("LARECOMP_ASTC_BC3: the game's textures kept in a quarter of the video memory (BC3), with slightly softer "
      "colour detail. Automatic: on only for graphics cards with 4 GB or less. Takes effect at the next start.",
      "LARECOMP_ASTC_BC3: текстуры игры занимают четверть видеопамяти (BC3), мелкие цветовые детали чуть мягче. "
      "Автоматически: включено только на видеокартах с 4 ГБ или меньше. Применяется при следующем запуске.")
PC_TR("Automatic (on with 4 GB of VRAM or less)",
      "Автоматически (вкл. при VRAM до 4 ГБ)")
PC_TR("For graphics cards with 4 GB of video memory or less: the textures take a quarter of the memory, so the game "
      "no longer runs out of it (stutters, blurry or late textures). Fine colour detail gets a little softer. "
      "Automatic turns it on only on such cards.",
      "Для видеокарт с 4 ГБ видеопамяти или меньше: текстуры занимают четверть памяти, и игре её больше хватает (нет "
      "рывков, размытых или запаздывающих текстур). Мелкие цветовые детали становятся чуть мягче. Автоматически "
      "включает это только на таких картах.")
PC_TR("Scene upscaler",
      "Апскейлер сцены")
PC_TR("TOTK_SCENE_UPSCALE. Applies at once.",
      "TOTK_SCENE_UPSCALE. Применяется сразу.")
PC_TR("Saves",
      "Сохранения")
PC_TR("Save backups kept",
      "Копий сохранений")
PC_TR("10 (default)",
      "10 (по умолч.)")
PC_TR("F12 screenshots",
      "Скриншоты F12")
PC_TR("TOTK_PC_SHOT: PNGs in the Screenshots folder.",
      "TOTK_PC_SHOT: PNG в папке Screenshots.")
PC_TR("Temperature unit",
      "Единица температуры")
PC_TR("TOTK_TEMP_UNIT: the temperature on the Purah Pad's map. Automatic: the game's (°F in English (US), °C in the other languages). The number changes at once, the °C / °F sign beside it the next time the map is opened.",
      "TOTK_TEMP_UNIT: температура на карте Пураншета. «Автоматически»: как в игре (°F в English (US), °C в остальных языках). Число меняется сразу, значок °C / °F рядом с ним — при следующем открытии карты.")
PC_TR("Automatic (by the game's language)",
      "Автоматически (по языку игры)")
PC_TR("°C (Celsius)",
      "°C (Цельсий)")
PC_TR("°F (Fahrenheit)",
      "°F (Фаренгейт)")
PC_TR("Crash reports",
      "Отчёты о сбоях")
PC_TR("TOTK_PC_CRASH: a report and a minidump per crash.",
      "TOTK_PC_CRASH: отчёт и minidump на каждый сбой.")
PC_TR("Automatic updates",
      "Автообновления")
PC_TR("Discord status",
      "Статус в Discord")
PC_TR("Shows \"Playing\", the time played and a \"Join the community\" button (the game's subreddit) on your Discord profile while Discord runs. Nothing else is sent.",
      "Показывает «Играет», время игры и кнопку «Join the community» (сабреддит игры) в профиле Discord, пока Discord запущен. Больше ничего не отправляется.")
PC_TR("Key bindings (Custom layout)",
      "Привязки клавиш (раскладка «Свой»)")
PC_TR("TOTK_KBM_A. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_A. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_B. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_B. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_X. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_X. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_Y. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_Y. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_LSTICK. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_LSTICK. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_RSTICK. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_RSTICK. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_L. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_L. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_R. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_R. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_ZL. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_ZL. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_ZR. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_ZR. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_PLUS. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_PLUS. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_MINUS. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_MINUS. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_DPAD_LEFT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_DPAD_LEFT. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_DPAD_UP. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_DPAD_UP. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_DPAD_RIGHT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_DPAD_RIGHT. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_DPAD_DOWN. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_DPAD_DOWN. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_MOVE_LEFT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_MOVE_LEFT. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_MOVE_UP. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_MOVE_UP. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_MOVE_RIGHT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_MOVE_RIGHT. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_MOVE_DOWN. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_MOVE_DOWN. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_CAMERA_LEFT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_CAMERA_LEFT. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_CAMERA_UP. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_CAMERA_UP. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_CAMERA_RIGHT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_CAMERA_RIGHT. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_CAMERA_DOWN. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_CAMERA_DOWN. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("TOTK_KBM_WALK. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_WALK. Для консольной схемы и для мира при «Клавиши в игре: Консольная». Пусто: клавиши раскладки ПК.")
PC_TR("Screenshot saved: %s",
      "Скриншот сохранён: %s")
PC_TR("Screenshot failed (%s)",
      "Сбой скриншота (%s)")
PC_TR("Screenshot skipped (still saving the previous ones)",
      "Скриншот пропущен (ещё сохраняются предыдущие)")
PC_TR("TOTK_WINDOW_MODE. F11 or Alt+Enter switch between windowed and the fullscreen kind chosen here.",
      "TOTK_WINDOW_MODE. F11 или Alt+Enter переключают между оконным режимом и выбранным здесь видом полного экрана.")
PC_TR("Monitor",
      "Монитор")
PC_TR("TOTK_MONITOR: where fullscreen goes and a windowed start opens. Remember: the monitor the game was on last (drag the window there, or pick it once), at the same window position.",
      "TOTK_MONITOR: куда идёт полный экран и где открывается окно при запуске. Запомнить: монитор, на котором игра была в прошлый раз (перетащите окно туда или выберите один раз), с той же позицией окна.")
PC_TR("Exclusive resolution",
      "Эксклюзивное разрешение")
PC_TR("TOTK_EXCLUSIVE_SIZE: the monitor's mode in exclusive fullscreen; the list is the modes of the monitor the game started on. Borderless and windowed keep the desktop's.",
      "TOTK_EXCLUSIVE_SIZE: режим монитора в эксклюзивном полном экране; список — режимы монитора, на котором запустилась игра. Режим без рамки и оконный сохраняют режим рабочего стола.")
PC_TR("Exclusive refresh rate",
      "Эксклюз. частота обновления")
PC_TR("TOTK_EXCLUSIVE_HZ: the refresh rate in exclusive fullscreen. A rate the resolution lacks gives the fastest it has under it.",
      "TOTK_EXCLUSIVE_HZ: частота обновления в эксклюзивном полном экране. Если нужной частоты для разрешения нет, берётся самая быстрая из меньших.")
PC_TR("Presentation",
      "Подача кадров")
PC_TR("Off: frames go out at once and may tear (LARECOMP_VK_PRESENT=immediate); on: mailbox, or FIFO with VRR.",
      "Выкл.: кадры выходят сразу и могут рваться (LARECOMP_VK_PRESENT=immediate); вкл.: mailbox, а с VRR — FIFO.")
PC_TR("On (default, no tearing)",
      "Вкл. (по умолч., без разрывов)")
PC_TR("Off (tearing, lowest latency)",
      "Выкл. (разрывы, мин. задержка)")
PC_TR("Variable refresh (VRR)",
      "Переменная частота (VRR)")
PC_TR("Only with a G-SYNC / FreeSync monitor (and, windowed, the driver's windowed VRR on): the panel follows each frame (FIFO). Without one it is plain vsync at a fixed refresh, and a cap under it repeats frames. Best with a frame cap a little under the refresh (116 at 120 Hz, 138 at 144 Hz); Unlocked and DLSS frame generation keep under it by themselves.",
      "Только с монитором G-SYNC / FreeSync (и, в окне, при включённом оконном VRR в драйвере): панель следует за каждым кадром (FIFO). Без него это обычный vsync на фиксированной частоте, а лимит ниже неё повторяет кадры. Лучше с лимитом кадров чуть ниже частоты обновления (116 при 120 Hz, 138 при 144 Hz); «Без лимита» и генерация кадров DLSS держатся ниже неё сами.")
PC_TR("Off (default)",
      "Выкл. (по умолч.)")
PC_TR("On (G-Sync / FreeSync monitor)",
      "Вкл. (монитор G-Sync / FreeSync)")
PC_TR("Frame pacing",
      "Темп кадров")
PC_TR("TOTK_PACE_AT: where the frame cap waits.",
      "TOTK_PACE_AT: где ждёт лимит кадров.")
PC_TR("At the present (default, steadier)",
      "При выводе (по умолч., ровнее)")
PC_TR("Before the frame (lower latency)",
      "Перед кадром (меньше задержка)")
PC_TR("Game tick source",
      "Источник такта игры")
PC_TR("TOTK_VSYNC: what the game's 60 Hz tick follows.",
      "TOTK_VSYNC: чему следует такт игры в 60 Hz.")
PC_TR("60 Hz clock (default)",
      "Часы 60 Hz (по умолч.)")
PC_TR("The monitor's vblank (fixed refresh only)",
      "Vblank монитора (только фикс. частота)")
PC_TR("Precise pacing",
      "Точный темп")
PC_TR("TOTK_PACE_SPIN_US: the cap's last part spins (CPU).",
      "TOTK_PACE_SPIN_US: конец ожидания лимита — активный цикл (CPU).")
PC_TR("Queued frames",
      "Кадры в очереди")
PC_TR("TOTK_MAX_QUEUED: frames waiting to be shown (needs present wait).",
      "TOTK_MAX_QUEUED: кадры в ожидании показа (нужен present wait).")
PC_TR("No limit",
      "Без лимита")
PC_TR("2 (default)",
      "2 (по умолч.)")
PC_TR("GPU frames in flight",
      "Кадры GPU в работе")
PC_TR("1 (lockstep: slower when CPU and GPU are both busy)",
      "1 (lockstep: медленнее, когда заняты и CPU, и GPU)")
PC_TR("Low latency",
      "Низкая задержка")
PC_TR("TOTK_REFLEX: each frame is held back before it reads input, by as much as it would otherwise wait for the GPU, the render thread or the frame cap later. NVIDIA: Reflex's sleep (VK_NV_low_latency2); other GPUs, or jit: the engine's own. Boost keeps the GPU clocks up (Reflex).",
      "TOTK_REFLEX: каждый кадр задерживается перед чтением ввода ровно на столько, сколько ему иначе пришлось бы потом ждать GPU, поток рендера или лимит кадров. NVIDIA: ожидание Reflex (VK_NV_low_latency2); другие GPU, или jit: собственный механизм движка. Boost держит частоты GPU высокими (Reflex).")
PC_TR("On (NVIDIA Reflex; other GPUs: the engine's own)",
      "Вкл. (NVIDIA Reflex; другие GPU: свой механизм)")
PC_TR("On + boost",
      "Вкл. + boost")
PC_TR("On (the engine's own, any GPU)",
      "Вкл. (свой механизм движка, любой GPU)")
PC_TR("Shaders",
      "Шейдеры")
PC_TR("Shader wait",
      "Ждать шейдеры")
PC_TR("A frame that needs a shader still being built waits for it. Limit to 0.1 s: past that, the frame is shown and the objects whose shaders are not ready yet appear a moment later (fullscreen passes and compute always wait). Always wait: nothing is ever drawn incomplete, but a shader the first time can freeze the picture for a second or more. TOTK_HOLD_MAX_MS overrides.",
      "Кадр, которому нужен ещё собираемый шейдер, ждёт его. Лимит 0.1 с: после этого кадр показывается, а объекты, чьи шейдеры ещё не готовы, появляются чуть позже (полноэкранные проходы и compute ждут всегда). Всегда ждать: ничего не рисуется неполным, но шейдер в первый раз может заморозить картинку на секунду и дольше. TOTK_HOLD_MAX_MS переопределяет.")
PC_TR("Limit to 0.1 s (default)",
      "Лимит 0.1 с (по умолч.)")
PC_TR("Always wait (nothing missing)",
      "Всегда ждать (всё на месте)")
PC_TR("HDR output",
      "Вывод HDR")
PC_TR("TOTK_HDR: needs Windows HDR on for the monitor.",
      "TOTK_HDR: нужен включённый HDR Windows для монитора.")
PC_TR("Off (SDR, default)",
      "Выкл. (SDR, по умолч.)")
PC_TR("Peak brightness (nits)",
      "Пиковая яркость (nits)")
PC_TR("TOTK_HDR_PEAK: the display's peak.",
      "TOTK_HDR_PEAK: пик яркости дисплея.")
PC_TR("Paper white (nits)",
      "Уровень белого (nits)")
PC_TR("TOTK_HDR_PAPER: the brightness of SDR white.",
      "TOTK_HDR_PAPER: яркость белого цвета SDR.")
PC_TR("Highlight colour",
      "Цвет ярких источников")
PC_TR("TOTK_HDR_HUE: the hue of bright lights from the game's HDR scene, 0 to 100.",
      "TOTK_HDR_HUE: оттенок ярких источников света из HDR-сцены игры, от 0 до 100.")
PC_TR("Mouse cursor in menus",
      "Курсор мыши в меню")
PC_TR("TOTK_MENU_CURSOR: in a menu the pointer shows and selects what it points at; left click is A (back is Esc).",
      "TOTK_MENU_CURSOR: в меню указатель виден и выбирает то, на что наведён; левый клик — A (назад — Esc).")
PC_TR("Minimum render resolution",
      "Мин. разрешение рендера")
PC_TR("100 %: the scene is always rendered at the render resolution (dynamic resolution off). 0 %: the game's own dynamic resolution. In between: dynamic, never below that fraction.",
      "100 %: сцена всегда рендерится в разрешении рендера (динамическое разрешение выкл.). 0 %: собственное динамическое разрешение игры. Между ними: динамическое, но не ниже этой доли.")
PC_TR("Field of view",
      "Поле зрения")
PC_TR("Multiplier of the camera's field of view (1 = the game's). The degrees are the camera's view right now: aiming and some cutscenes narrow or widen it, and the multiplier keeps their proportion. Wider costs some GPU and CPU (more of the world on screen).",
      "Множитель поля зрения камеры (1 = как в игре). Градусы — текущий обзор камеры: прицеливание и некоторые сцены сужают или расширяют его, а множитель сохраняет их пропорцию. Шире — больше нагрузка на GPU и CPU (на экране больше мира).")
PC_TR("Shadow resolution",
      "Разрешение теней")
PC_TR("Multiplier of the shadow map's width and height (2 = sharper, 4x the memory).",
      "Множитель ширины и высоты карты теней (2 = чётче, памяти в 4x больше).")
PC_TR("Near shadow reach",
      "Дальность ближних теней")
PC_TR("Multiplier of how far the sun's sharpest shadow cascades reach (1 = the game's: the first two hand over 32 m and 64 m away, where the change in sharpness shows as a line, mostly at 4K). 2 moves those lines to 64 m and 128 m; the far shadows stay. The same shadow map then covers more ground, so near shadows get softer by that factor: raise Shadow resolution with it. Costs a little GPU and CPU (more objects drawn into the near shadow maps).",
      "Множитель того, как далеко доходят самые чёткие каскады теней от солнца (1 = как в игре: первые два сменяются на 32 м и 64 м, где смена чёткости видна как линия, особенно в 4K). 2 переносит эти линии на 64 м и 128 м; дальние тени не меняются. Та же карта теней тогда покрывает больше земли, поэтому ближние тени становятся мягче во столько же раз: повышайте вместе с этим Разрешение теней. Немного нагружает GPU и CPU (в ближние карты теней рисуется больше объектов).")
PC_TR("Sun shadow map size",
      "Размер карты теней солнца")
PC_TR("The game's: the sun's shadow map shrinks and grows with the area its cascades cover, so near the ground foliage shadows turn coarse, and every size change moves the shadow texels (leaf shadows jump instead of sliding). Fixed: always the largest size the game picks (times the shadow resolution), steady and sharper; costs some GPU.",
      "Как в игре: карта теней солнца уменьшается и растёт вместе с площадью, которую покрывают её каскады, поэтому у земли тени листвы становятся грубыми, а каждая смена размера сдвигает текселы теней (тени листьев прыгают, а не скользят). Фиксированный: всегда наибольший размер, который выбирает игра (умноженный на разрешение теней), стабильно и чётче; нагружает GPU.")
PC_TR("Detail distance",
      "Дальность деталей")
PC_TR("Multiplier of the distance at which models switch to lower detail (2 = full detail twice as far).",
      "Множитель расстояния, на котором модели переключаются на меньшую детализацию (2 = полная детализация вдвое дальше).")
PC_TR("Actor detail distance",
      "Дальность деталей акторов")
PC_TR("Multiplier of the distances at which characters, enemies and objects start skipping update, animation and draw frames (2 = full rate twice as far; costs CPU).",
      "Множитель расстояний, на которых персонажи, враги и объекты начинают пропускать кадры обновления, анимации и отрисовки (2 = полная частота вдвое дальше; нагружает CPU).")
PC_TR("Terrain detail distance",
      "Дальность деталей рельефа")
PC_TR("Multiplier of the distances at which the ground and water switch to coarser levels (2 = full detail twice as far; costs CPU and GPU).",
      "Множитель расстояний, на которых земля и вода переключаются на более грубые уровни (2 = полная детализация вдвое дальше; нагружает CPU и GPU).")
PC_TR("Simulation distance",
      "Дальность симуляции")
PC_TR("Multiplier of the distance from the camera at which characters, enemies, objects and their physics stop being updated (1 = the game's; 1.5-2 = high). Costs CPU: more actors are simulated every frame, and distant ones keep reacting (fights, physics) instead of freezing.",
      "Множитель расстояния от камеры, на котором персонажи, враги, объекты и их физика перестают обновляться (1 = как в игре; 1.5-2 = высоко). Нагружает CPU: каждый кадр симулируется больше акторов, а дальние продолжают реагировать (бои, физика), а не замирают.")
PC_TR("Mouse camera (direct)",
      "Камера мышью (прямая)")
PC_TR("Multiplier of the camera turn the game itself applies per mouse count (on top of the mouse sensitivity). 0 = off: the mouse turns the camera through emulated right-stick deflection instead.",
      "Множитель поворота камеры, который игра сама применяет на каждый отсчёт мыши (сверх чувствительности мыши). 0 = выкл.: мышь поворачивает камеру через эмулированное отклонение правого стика.")
PC_TR("Fixed (steady)",
      "Фиксированный (стабильно)")
PC_TR("The game's (changes)",
      "Как в игре (меняется)")
PC_TR("Game patches",
      "Патчи игры")
PC_TR("Aspect ratio",
      "Формат кадра")
PC_TR("The window's: on a 3:2, 16:10, 21:9, 32:9 or 48:9 window or monitor the game draws a canvas of that shape at start (1920x1280, 1920x1200, 3440x1440, 5120x1440 or 7680x1440), the HUD laid out for it: 21:9, 32:9 and 48:9 a wider view, 3:2 and 16:10 a taller one (the same height of view, a little less at the sides). Other shapes, and 16:9, keep the game's 16:9 picture. 16:10 / 21:9 / 32:9 / 48:9 / 3:2: that canvas whatever the window (on another shape it is shown whole, with bars; 21:9, 32:9 and 48:9 are heavier than the window's own shape: the whole canvas is drawn, then shown smaller). Needs the game patches' RomFS sets (TOTK_PATCH_SETS) and the overlay's layout for that shape. At the next start: the canvas and the game files laid out for it are chosen as the game starts. Setup prepares the layouts of this PC's monitors' shapes; another shape's are made at the start after you pick it (about 30 s, once).",
      "Как у окна: в окне или на мониторе 3:2, 16:10, 21:9, 32:9 или 48:9 игра при запуске рисует холст такой формы (1920x1280, 1920x1200, 3440x1440, 5120x1440 или 7680x1440), HUD расставлен под него: 21:9, 32:9 и 48:9 дают более широкий обзор, 3:2 и 16:10 — более высокий (та же высота обзора, по бокам чуть меньше). Другие форматы и 16:9 сохраняют картинку игры 16:9. 16:10 / 21:9 / 32:9 / 48:9 / 3:2: этот холст при любом окне (в другом формате он показывается целиком, с полосами; 21:9, 32:9 и 48:9 тяжелее формата самого окна: рисуется весь холст, затем показывается меньше). Нужны наборы RomFS из патчей игры (TOTK_PATCH_SETS) и раскладка оверлея для этого формата. При следующем запуске: холст и подогнанные под него файлы игры выбираются при старте игры. Setup готовит раскладки для форматов мониторов этого ПК; раскладки другого формата создаются при запуске после его выбора (около 30 с, один раз).")
PC_TR("HUD position (21:9, 32:9, 48:9)",
      "Позиция HUD (21:9, 32:9, 48:9)")
PC_TR("Where the HUD goes on a 21:9, 32:9 or 48:9 canvas: at the screen's edges, or in the middle 16:9 (closer to the eyes on a very wide screen). Menus are the same either way. Nothing changes at 16:9. At the next start: the game loads its interface layouts once, as it starts. Automatic (the default): in the middle on 48:9, at the edges otherwise.",
      "Где находится HUD на холсте 21:9, 32:9 или 48:9: по краям экрана или в центральной области 16:9 (ближе к глазам на очень широком экране). Меню одинаковы в обоих случаях. При 16:9 ничего не меняется. При следующем запуске: игра загружает раскладки интерфейса один раз при старте. «Автоматически» (по умолчанию): в центре на 48:9, по краям в остальных случаях.")
PC_TR("Texture cache",
      "Кэш текстур")
PC_TR("Multiplier of the game's texture cache (fewer blurry textures when turning back; 490 MiB more per step). Needs the game patches' RomFS sets (TOTK_PATCH_SETS). At the next start: the game sizes its memory once, as it starts.",
      "Множитель кэша текстур игры (меньше размытых текстур при возврате назад; на 490 MiB больше за шаг). Нужны наборы RomFS из патчей игры (TOTK_PATCH_SETS). При следующем запуске: игра выделяет память один раз при старте.")
PC_TR("Grass density",
      "Плотность травы")
PC_TR("Multiplier of grass, flowers and pebbles. Needs the game patches' RomFS sets (TOTK_PATCH_SETS). At the next start: the game reads its grass table once, as it starts.",
      "Множитель травы, цветов и камешков. Нужны наборы RomFS из патчей игры (TOTK_PATCH_SETS). При следующем запуске: игра читает свою таблицу травы один раз при старте.")
PC_TR("Shadow distance",
      "Дальность теней")
PC_TR("Multiplier of the sharp shadow cascades' distances. Needs the game patches' RomFS sets (TOTK_PATCH_SETS). At the next start: the value is in the shader package, which the game reads once, as it starts.",
      "Множитель дальности чётких каскадов теней. Нужны наборы RomFS из патчей игры (TOTK_PATCH_SETS). При следующем запуске: значение хранится в пакете шейдеров, который игра читает один раз при старте.")
PC_TR("Fog distance",
      "Дальность тумана")
PC_TR("Multiplier of the distance fog's start and end (clearer distant mountains). Needs the game patches' RomFS sets (TOTK_PATCH_SETS). At the next start: the game reads its sky and fog palettes once, as it starts.",
      "Множитель начала и конца дымки на расстоянии (дальние горы яснее). Нужны наборы RomFS из патчей игры (TOTK_PATCH_SETS). При следующем запуске: игра читает палитры неба и дымки один раз при старте.")
PC_TR("Fast-travel speed",
      "Скорость перемещения")
PC_TR("Fast: Link's departure when travelling is about 2.5 s shorter (its long waits halved, his dissolve as on the Switch); Normal is the game's own. Needs the game patches' RomFS sets (TOTK_PATCH_SETS). At the next start: the game loads the travel scene the first time you travel and keeps it.",
      "«Быстрая»: отправление Линка при быстром перемещении примерно на 2.5 с короче (долгие паузы вдвое меньше, растворение как на Switch); «Обычная» — как в игре. Нужны наборы RomFS из патчей игры (TOTK_PATCH_SETS). При следующем запуске: игра загружает сцену перемещения при первом перемещении и сохраняет её.")
PC_TR("Cutscenes (21:9, 32:9, 48:9)",
      "Заставки (21:9, 32:9, 48:9)")
PC_TR("On a 21:9, 32:9 or 48:9 canvas, real-time cutscenes and conversations: wide like the rest of the game, or the original 16:9 framing with bars at the sides (for a scene that looks odd at the edges). Pre-rendered movies are 16:9 video: always shown in the middle with bars.",
      "На холсте 21:9, 32:9 или 48:9 заставки и разговоры в реальном времени: широкие, как остальная игра, или исходный кадр 16:9 с полосами по бокам (для сцены, которая странно выглядит по краям). Заранее отрендеренные видеоролики — 16:9: всегда по центру, с полосами.")
PC_TR("Upscales the 3D scene with a temporal upscaler that builds detail over several frames. DLSS: NVIDIA RTX only; DLAA: DLSS at full resolution, the best image for more GPU. FSR and XeSS: any GPU (XeSS runs fastest on Intel Arc). All cost some GPU; Off is the game's own.",
      "Масштабирует 3D-сцену временным апскейлером, который набирает детали за несколько кадров. DLSS: только NVIDIA RTX; DLAA: DLSS в полном разрешении, лучшее изображение ценой большей нагрузки на GPU. FSR и XeSS: любой GPU (XeSS быстрее всего на Intel Arc). Все немного нагружают GPU; «Выкл.» — собственный вариант игры.")
PC_TR("Safe mode (if the game crashes)",
      "Безопасный режим (если игра вылетает)")
PC_TR("For a game that crashes or hangs: the renderer without its faster paths, DLSS, FSR and frame generation, and without the driver paths some GPUs crash in. Slower. At the next start.",
      "Для игры, которая вылетает или зависает: рендерер без быстрых путей, без DLSS, FSR и генерации кадров и без путей драйвера, на которых падают некоторые GPU. Медленнее. При следующем запуске.")
PC_TR("Wide picture: F10 > Restart the game now to use %s",
      "Широкая картинка: F10 > «Перезапустить игру сейчас», чтобы использовать %s")
PC_TR("The window changed shape: F10 > Restart the game now to fit it",
      "Окно изменило форму: F10 > «Перезапустить игру сейчас», чтобы подогнать картинку")
PC_TR("Screen shape: start the game once from TotK Launcher.exe to prepare its layout (16:9 until then)",
      "Форма экрана: один раз запустите игру через TotK Launcher.exe, чтобы подготовить интерфейс (до тех пор 16:9)")
PC_TR("This screen shape's layout was not prepared: 16:9 until Setup runs again",
      "Интерфейс для этой формы экрана не подготовлен: 16:9 до повторного запуска Setup")
PC_TR("Safe mode is on: DLSS, FSR and frame generation stay off",
      "Безопасный режим включён: DLSS, FSR и генерация кадров остаются выключенными")
PC_TR("Frame generation mode",
      "Режим генерации кадров")
PC_TR("Smooth (matched to the refresh)",
      "Плавный (по частоте обновления)")
PC_TR("Maximum fps",
      "Максимум кадров")
PC_TR("TOTK_FG_PACE. Smooth: the frames shown match the monitor (real ones at the cap divided by the multiplier). Maximum fps: the real frames as without frame generation and the multiplier's generated ones after each, past the refresh rate, with VRR too (mailbox, else immediate, which can tear; the panel may leave VRR; TOTK_FG_MAX_VRR_FIFO=1 keeps VRR's FIFO).",
      "TOTK_FG_PACE. Плавный: выводимые кадры идут в такт с монитором (реальные — с ограничением, делённым на множитель). Максимум кадров: реальные кадры как без генерации кадров, а после каждого — сгенерированные кадры множителя, выше частоты обновления, в том числе с VRR (mailbox, иначе immediate, при котором возможны разрывы изображения; монитор может выйти из режима VRR; TOTK_FG_MAX_VRR_FIFO=1 сохраняет FIFO режима VRR).")
PC_TR("Smooth keeps the frames shown in step with the monitor's refresh. Maximum fps adds the generated frames to every real frame the PC makes, even past the refresh rate: more frames than the screen shows (the fps readout counts them all), for VRR panels or low latency; Smooth looks steadier on a fixed refresh. With V-sync off the picture can tear.",
      "Плавный держит выводимые кадры в такт с обновлением монитора. Максимум кадров добавляет сгенерированные кадры к каждому реальному кадру, который делает ПК, даже выше частоты обновления: кадров больше, чем показывает экран (счётчик кадров считает все), для мониторов с VRR или низкой задержки; при фиксированной частоте Плавный выглядит ровнее. При выключенной вертикальной синхронизации возможны разрывы изображения.")
PC_TR("Scope",
      "Визор")
PC_TR("Ultrahand",
      "Комбикинез")
PC_TR("Fuse",
      "Чаромонтаж")
PC_TR("Ascend",
      "Чаропортал")
PC_TR("Recall",
      "Автореверс")
PC_TR("Autobuild",
      "Реконструктор")
PC_TR(", %d nits",
      ", %d нит")
PC_TR("Upscaler",
      "Апскейлер")
PC_TR("TOTK_DLSS_SR / TOTK_FSR_SR / TOTK_XESS_SR. DLSS (NVIDIA RTX), FSR or XeSS (any GPU) upscales the 3D scene to the output in place of the game's upscaler and its FXAA (DLAA / native AA when Render resolution matches the output).",
      "TOTK_DLSS_SR / TOTK_FSR_SR / TOTK_XESS_SR. DLSS (NVIDIA RTX), FSR или XeSS (любой GPU) масштабирует 3D-сцену до выходного разрешения вместо масштабирования игры и её FXAA (DLAA / собственное сглаживание, когда разрешение рендеринга совпадает с выходным).")
PC_TR("FSR (any GPU)",
      "FSR (любой GPU)")
PC_TR("Frame generation",
      "Генерация кадров")
PC_TR("TOTK_DLSS_FG / TOTK_FSR_FG. Generated frames between real ones: DLSS x2 on NVIDIA RTX 40 series and newer, x3 and x4 on RTX 50; FSR x2 on any GPU. The frame cap counts the frames shown (at a 30 cap: x2, 60 shown). Adds some input latency.",
      "TOTK_DLSS_FG / TOTK_FSR_FG. Сгенерированные кадры между реальными: DLSS x2 на NVIDIA RTX 40 и новее, x3 и x4 на RTX 50; FSR x2 на любом GPU. Лимит кадров считает показанные кадры (при лимите 30: x2, показано 60). Добавляет немного задержки ввода.")
PC_TR("1%% low %.1f fps (%.1f ms)",
      "1%% мин. %.1f fps (%.1f мс)")
PC_TR("1% low",
      "1% мин.")
PC_TR("1x (Switch)",
      "1x (Switch)")
PC_TR("Vsync",
      "Вертикальная синхронизация")
PC_TR("HDR",
      "HDR")
PC_TR("0.1% low",
      "0,1% мин.")
PC_TR("game",
      "игра")
PC_TR("DLSS model",
      "Модель DLSS")
PC_TR("NVIDIA's default",
      "По умолчанию NVIDIA")
PC_TR("Preset K",
      "Пресет K")
PC_TR("Preset J",
      "Пресет J")
PC_TR("TOTK_DLSS_PRESET. The AI model DLSS and DLAA use: NVIDIA's default for each quality, or preset K (sharpest) or J (a little less ghosting, a little more flicker) for all.",
      "TOTK_DLSS_PRESET. Модель ИИ, которую используют DLSS и DLAA: по умолчанию NVIDIA для каждого качества или пресет K (самый чёткий) либо J (чуть меньше шлейфов, чуть больше мерцания) для всех.")
PC_TR("DLSS character trails",
      "Шлейфы за персонажами в DLSS")
PC_TR("Fewest trails",
      "Меньше шлейфов")
PC_TR("Between",
      "Среднее")
PC_TR("Smoothest (more trails)",
      "Плавнее (больше шлейфов)")
PC_TR("TOTK_DLSS_TRAILS. Where something moved (Link, animals, a swung weapon), how much DLSS and DLAA trust the new frame over the old ones: fewest trails, or smoother edges with more trails.",
      "TOTK_DLSS_TRAILS. Там, где что-то сдвинулось (Линк, животные, взмах оружия), насколько DLSS и DLAA доверяют новому кадру, а не прежним: меньше шлейфов или более гладкие края с большими шлейфами.")
PC_TR("Which AI model DLSS and DLAA use. NVIDIA's default picks one for each quality. Preset K is the sharpest; preset J ghosts a little less behind moving things but flickers a little more. Changing it rebuilds DLSS once (a short pause).",
      "Какую модель ИИ используют DLSS и DLAA. По умолчанию NVIDIA выбирает модель для каждого качества. Пресет K самый чёткий; пресет J оставляет чуть меньше шлейфов за движущимися объектами, но чуть сильнее мерцает. При смене DLSS пересоздаётся один раз (короткая пауза).")
PC_TR("Where something moved (Link, animals, a swung weapon), DLSS and DLAA can keep blending in older frames, which leaves faint trails, or trust the new frame, which removes them but can make those edges flicker a little. Fewest trails is the default.",
      "Там, где что-то сдвинулось (Линк, животные, взмах оружия), DLSS и DLAA могут продолжать смешивать старые кадры, что оставляет слабые шлейфы, или доверять новому кадру, что убирает их, но края могут слегка мерцать. «Меньше шлейфов» — значение по умолчанию.")
PC_TR("Safe GPU mode: the graphics driver crashed in the last runs, so shaders now build the slow, safe way",
      "Безопасный режим GPU: графический драйвер падал в последних запусках, поэтому шейдеры теперь собираются медленным безопасным способом")
PC_TR("Safe GPU mode: the graphics driver kept crashing, so shaders are also built unoptimised; the game may run slower until it has run cleanly a few times",
      "Безопасный режим GPU: графический драйвер продолжал падать, поэтому шейдеры теперь собираются и без оптимизации; игра может работать медленнее, пока несколько запусков не пройдут без сбоев")
PC_TR("XeSS (any GPU)",
      "XeSS (любой GPU)")
PC_TR("Shader compile indicator",
      "Индикатор компиляции шейдеров")
PC_TR("Bottom right while shaders compile: the percentage on a first start, how many are left in play.",
      "Справа внизу во время компиляции шейдеров: процент при первом запуске и сколько осталось во время игры.")
PC_TR("Shows when shaders are being compiled (bottom right): a short stutter while it is up is the compile, not your PC. Off hides it in play (a first start still shows its progress); the compiling still happens.",
      "Показывает, когда компилируются шейдеры (справа внизу): короткий рывок в это время вызван компиляцией, а не вашим ПК. «Выкл.» скрывает индикатор во время игры (первый запуск всё равно показывает прогресс); компиляция всё равно идёт.")
PC_TR("Shaders left to compile:",
      "Осталось скомпилировать шейдеров:")
PC_TR("Last hitch",
      "Последний рывок")
PC_TR("No hitch yet",
      "Рывков пока нет")
PC_TR("GPU wait",
      "ожидание GPU")
PC_TR("shader builds",
      "сборка шейдеров")
PC_TR("texture uploads",
      "загрузка текстур")
PC_TR("new images",
      "новые изображения")
PC_TR("file reads",
      "чтение файлов")
PC_TR("present (driver / window)",
      "вывод кадра (драйвер / окно)")
PC_TR("game CPU",
      "ЦП игры")
PC_TR("render CPU",
      "ЦП отрисовки")
PC_TR("s ago",
      "с назад")
PC_TR("Upscaler:",
      "Апскейлер:")
PC_TR("off",
      "выкл.")
PC_TR("plain scaling",
      "простое масштабирование")
PC_TR("native",
      "родное")
PC_TR("Quality",
      "Качество")
PC_TR("Balanced",
      "Баланс")
PC_TR("Performance",
      "Производительность")
PC_TR("Ultra Performance",
      "Ультрапроизводительность")
PC_TR("idle",
      "не работает")
PC_TR("3D output is",
      "3D-вывод")
PC_TR("scene",
      "сцена")
PC_TR("it failed to start",
      "не удалось запустить")
PC_TR("starting",
      "запускается")
PC_TR("no 3D view this frame",
      "нет 3D-кадра")
PC_TR("no scene seen",
      "сцены нет")
