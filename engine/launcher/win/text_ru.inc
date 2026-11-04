/* text_ru.inc: the launcher's (launcher.c ES_EN) and the settings dialog's (preplay.c PP, its rows) texts in
 * Russian, TR(english, translation) each, looked up by the English (text_tr.h). Checked by
 * tools/engine/check_texts.py (same printf specifiers and line breaks, no entry the sources lack). lang-ptbr 11-03. */
TR(L"\n\nOpen %ls now?",
   L"\n\nОткрыть %ls сейчас?")
TR(L"Folder with the save (for example, the JKSV backup on the SD card)",
   L"Папка с сохранением (например, резервная копия JKSV на SD-карте)")
TR(L"Couldn't back up the current save; nothing was changed.",
   L"Не удалось сделать резервную копию текущего сохранения; ничего не изменено.")
TR(L"Couldn't copy the save (step %d). Your previous save is unchanged.\n\n%ls",
   L"Не удалось скопировать сохранение (шаг %d). Прежнее сохранение не изменилось.\n\n%ls")
TR(L"\n\nYour previous save is backed up in:\n",
   L"\n\nРезервная копия прежнего сохранения:\n")
TR(L"folder",
   L"папка")
TR(L"slot",
   L"ячейка")
TR(L"slots",
   L"яч.")
TR(L"Import the selected save\nYour current save is backed up first.",
   L"Импортировать выбранное сохранение\nСначала создаётся копия текущего.")
TR(L"Look in another folder…\nFor example, a JKSV backup from the console or a portable emulator.",
   L"Искать в другой папке…\nНапример, копия JKSV с консоли или портативный эмулятор.")
TR(L"Choose the save to import",
   L"Выберите сохранение для импорта")
TR(L"No TotK saves found",
   L"Сохранения TotK не найдены")
TR(L"There are no save backups yet.",
   L"Резервных копий сохранений пока нет.")
TR(L"automatic",
   L"автоматическая")
TR(L"before import",
   L"перед импортом")
TR(L"before restore",
   L"перед восстановлением")
TR(L"before update",
   L"перед обновлением")
TR(L"Restore the selected backup\nYour current save is backed up first.",
   L"Восстановить выбранную копию\nСначала создаётся копия текущего.")
TR(L"Choose the backup to restore",
   L"Выберите копию для восстановления")
TR(L"save on this PC",
   L"сохранение на этом ПК")
TR(L"Current save: %d %ls, saved on %ls.\n%ls",
   L"Текущее сохранение: %d %ls, сохранено %ls.\n%ls")
TR(L"No save yet.\n%ls",
   L"Сохранения пока нет.\n%ls")
TR(L"Play",
   L"Играть")
TR(L"Restore a backup…",
   L"Восстановить копию…")
TR(L"Make a backup now",
   L"Сделать копию сейчас")
TR(L"Saves",
   L"Сохранения")
TR(L"Backup made:\n%ls",
   L"Копия создана:\n%ls")
TR(L"Couldn't make the backup.",
   L"Не удалось сделать копию.")
TR(L"There's no save to back up.",
   L"Нет сохранения для копирования.")
TR(L"There are TotK saves you can import",
   L"Есть сохранения TotK, которые можно импортировать")
TR(L"Do you want to import one before you start? You can also do it later by holding Shift while opening the launcher.",
   L"Импортировать одно перед началом? Это можно сделать и позже, удерживая Shift при открытии лаунчера.")
TR(L"Open the new version\n%ls",
   L"Открыть новую версию\n%ls")
TR(L"There's a newer version of the game next to this folder:\n%ls\n\nYour save, your settings and the shader cache carry over on their own (your save is backed up before its first start).",
   L"Рядом с этой папкой есть более новая версия игры:\n%ls\n\nСохранение, настройки и кэш шейдеров переносятся сами (перед её первым запуском создаётся копия сохранения).")
TR(L"Keep this one\nI won't ask about that folder again.",
   L"Оставить эту\nБольше не спрашивать об этой папке.")
TR(L"A newer version is available",
   L"Доступна новая версия")
TR(L"Couldn't open the new version (error %lu):\n%ls\n\nStaying on this one.",
   L"Не удалось открыть новую версию (ошибка %lu):\n%ls\n\nОстаётся эта.")
TR(L"A new version is out: %hs",
   L"Вышла новая версия: %hs")
TR(L"This update compiles the game again on your PC: it takes 15 to 45 minutes, and you can't play meanwhile. Your save and your settings are kept.",
   L"Это обновление заново компилирует игру на вашем ПК: это занимает от 15 до 45 минут, и играть в это время нельзя. Сохранение и настройки остаются.")
TR(L"Update now\nThe game opens by itself when it's done.",
   L"Обновить сейчас\nПо завершении игра откроется сама.")
TR(L"Next time\nPlay the installed version.",
   L"В следующий раз\nИграть в установленную версию.")
TR(L"Skip this version\nI won't ask about it again.",
   L"Пропустить эту версию\nБольше не спрашивать о ней.")
TR(L"Downloading the installer: %lld of %lld MB",
   L"Загрузка установщика: %lld из %lld МБ")
TR(L"Installing: step %d of %d (%ls)",
   L"Установка: шаг %d из %d (%ls)")
TR(L"Installing...",
   L"Установка...")
TR(L"Cancelling...",
   L"Отмена...")
TR(L"Getting ready...",
   L"Подготовка...")
TR(L"Updating to %hs",
   L"Обновление до %hs")
TR(L"Couldn't update to %hs; the installed version opens.\n\n%ls",
   L"Не удалось обновить до %hs; открывается установленная версия.\n\n%ls")
TR(L"AVX (enabled by Windows)",
   L"AVX (включено Windows)")
TR(L"This Windows doesn't have the Microsoft Store's \"VP9 Video Extensions\"%ls.\n\nCutscenes look the same: the game brings its own video decoder (libvpx) and decodes them on the processor instead of the graphics card. If you want the graphics card to do it, install \"VP9 Video Extensions\" from the Microsoft Store.\n\nThis notice only shows once.",
   L"В этой Windows нет «Расширений для видео VP9» из Microsoft Store%ls.\n\nРолики выглядят так же: у игры свой видеодекодер (libvpx), и она декодирует их на процессоре, а не на видеокарте. Если хотите, чтобы это делала видеокарта, установите «Расширения для видео VP9» из Microsoft Store.\n\nЭто уведомление показывается один раз.")
TR(L" (nor Media Foundation: an N edition without the Media Feature Pack)",
   L" (и нет Media Foundation: выпуск N без Media Feature Pack)")
TR(L"it could not be opened. Unpack it yourself and drop its folder on the launcher",
   L"его не удалось открыть. Распакуйте его сами и перетащите папку на лаунчер")
TR(L"it has no romfs, exefs or cheats folder. If the mod is packed inside it, unpack it and drop that folder instead",
   L"в нём нет папки romfs, exefs или cheats. Если мод упакован внутри, распакуйте его и перетащите эту папку")
TR(L"it is a mod for another game",
   L"это мод для другой игры")
TR(L"it loads code of its own (subsdk / main), which a recompiled game cannot load",
   L"он загружает собственный код (subsdk / main), который перекомпилированная игра загрузить не может")
TR(L"it could not be copied. Check that the mods folder beside the game can be written",
   L"его не удалось скопировать. Проверьте, что в папку mods рядом с игрой можно записывать")
TR(L"The mod was not installed:\n%ls\n\nWhy: %ls.",
   L"Мод не установлен:\n%ls\n\nПричина: %ls.")
TR(L" + code patches",
   L" + патчи кода")
TR(L"code patches",
   L"патчи кода")
TR(L" + cheats",
   L" + читы")
TR(L"cheats",
   L"читы")
TR(L"\nIts files don't name a game version: it was put with the one you play (%ls).",
   L"\nЕго файлы не называют версию игры: он помещён к той, в которую вы играете (%ls).")
TR(L"\nIt is made for game version %ls, which the port does not build: it was put with the 1.4.3 mods and may not work.",
   L"\nОн сделан для версии игры %ls, которую порт не собирает: он помещён к модам 1.4.3 и может не работать.")
TR(L"Mod installed: %ls (%ls, for %ls)\nin %ls%ls\n\nOrder and switches: F10 > Mods.",
   L"Мод установлен: %ls (%ls, для %ls)\nв %ls%ls\n\nПорядок и переключатели: F10 > Моды.")
TR(L"The game reads its data from your game dump, which is no longer at:\n%ls\n\nIf you moved it, pick it again. If you'll delete it, install again with Setup and tick \"Copy the game files\".",
   L"Игра читает данные из вашего дампа, которого больше нет здесь:\n%ls\n\nЕсли вы его переместили, выберите его снова. Если собираетесь его удалить, установите игру заново через Setup и отметьте «Копировать файлы игры».")
TR(L"Pick the dump",
   L"Выбрать дамп")
TR(L"Close",
   L"Закрыть")
TR(L"Can't find your game dump",
   L"Не удаётся найти дамп игры")
TR(L"Switch game (NSP, XCI)",
   L"Игра Switch (NSP, XCI)")
TR(L"Your game dump (the one Setup read)",
   L"Ваш дамп игры (тот, что прочитал Setup)")
TR(L"The file that says where the game data is, is damaged:\n%ls\n\nInstall again with Setup.",
   L"Файл, указывающий, где данные игры, повреждён:\n%ls\n\nУстановите игру заново через Setup.")
TR(L"That file is not the game dump Setup read:\n%ls",
   L"Этот файл — не тот дамп, который прочитал Setup:\n%ls")
TR(L"Couldn't save the new location in:\n%ls",
   L"Не удалось сохранить новое расположение в:\n%ls")
TR(L"Settings",
   L"Настройки")
TR(L"%ls: incomplete (totk.exe, player.txt or img\\main.bin is missing)",
   L"%ls: неполная (нет totk.exe, player.txt или img\\main.bin)")
TR(L"%ls: this build of the game was made for processors with AVX2, and this one lacks %ls. Make it again with the Builder on this PC: it picks the build for processors without AVX2 by itself.",
   L"%ls: эта сборка игры сделана для процессоров с AVX2, а у этого нет %ls. Соберите её заново через Builder на этом ПК: он сам выберет сборку для процессоров без AVX2.")
TR(L" Your 1.0.0 saves were copied to this version once: what you play later in 1.0.0 will not show up here, and %ls saves do not go back to 1.0.0.",
   L" Ваши сохранения 1.0.0 были однажды скопированы в эту версию: то, во что вы потом играете в 1.0.0, здесь не появится, а сохранения %ls не переносятся обратно в 1.0.0.")
TR(L"TotK %ls (beta). This version runs at full frame rate, but it is less tested than 1.0.0. The keyboard and mouse controls work as on 1.0.0 in play, except the ability keys 4 to 9 (pick abilities from the ability wheel instead). In menus, use the keyboard or a controller: the mouse pointer does not work in 1.4.3's menus yet.%ls",
   L"TotK %ls (бета). Эта версия идёт с полной частотой кадров, но проверена меньше, чем 1.0.0. Управление с клавиатуры и мыши в игре работает как в 1.0.0, кроме клавиш способностей 4–9 (выбирайте способности в Колесе способностей). В меню используйте клавиатуру или геймпад: указатель мыши в меню 1.4.3 пока не работает.%ls")
TR(L"%ls: couldn't start (error %lu)",
   L"%ls: не удалось запустить (ошибка %lu)")
TR(L"%ls: the game used an instruction this processor doesn't have (code 0xC000001D)\nLog: %ls",
   L"%ls: игра использовала инструкцию, которой нет у этого процессора (код 0xC000001D)\nЖурнал: %ls")
TR(L"%ls: the game closed while starting (code 0x%08lx)\nCrash report: %ls",
   L"%ls: игра закрылась при запуске (код 0x%08lx)\nОтчёт о сбое: %ls")
TR(L"%ls: the game closed while starting (code 0x%08lx)\nLog: %ls",
   L"%ls: игра закрылась при запуске (код 0x%08lx)\nЖурнал: %ls")
TR(L"The game closed because of an error.\n\nCrash report:\n%ls",
   L"Игра закрылась из-за ошибки.\n\nОтчёт о сбое:\n%ls")
TR(L"The game closed with an error (code 0x%08lx).\n\nLogs: %ls",
   L"Игра закрылась с ошибкой (код 0x%08lx).\n\nЖурналы: %ls")
TR(L"The game restarted %d times in under a minute; it is not opened again.\n\nStart it again from the launcher.",
   L"Игра перезапустилась %d раз меньше чем за минуту; больше она не открывается.\n\nЗапустите её снова из лаунчера.")
TR(L"The game is already running.",
   L"Игра уже запущена.")
TR(L"This processor can't run the game: it lacks instructions the game needs (%ls).\n\nIt needs at least SSE4.2 and POPCNT: a 1st-generation Intel Core (2008) or later, or an AMD FX or Ryzen.",
   L"Этот процессор не может запустить игру: у него нет нужных игре инструкций (%ls).\n\nНужны как минимум SSE4.2 и POPCNT: Intel Core 1-го поколения (2008) или новее, либо AMD FX или Ryzen.")
TR(L"No Vulkan driver is installed (vulkan-1.dll is missing), and the game needs one.\n\nInstall the latest driver for your graphics card from the NVIDIA, AMD or Intel website. A Vulkan 1.3 GPU is required: NVIDIA GTX 900 or later, AMD RX 400 or later, Intel Arc or Iris Xe.",
   L"Драйвер Vulkan не установлен (нет vulkan-1.dll), а он нужен игре.\n\nУстановите последний драйвер видеокарты с сайта NVIDIA, AMD или Intel. Нужен GPU с Vulkan 1.3: NVIDIA GTX 900 или новее, AMD RX 400 или новее, Intel Arc или Iris Xe.")
TR(L"(launcher folder)",
   L"(папка лаунчера)")
TR(L"Can't find the game: there's no packages\\ with current.txt and no totk.exe next to the launcher.\n\n%ls",
   L"Не удаётся найти игру: рядом с лаунчером нет packages\\ с current.txt и нет totk.exe.\n\n%ls")
TR(L"Can't find the game data (extracted RomFS) in:\n%ls",
   L"Не удаётся найти данные игры (распакованный RomFS) в:\n%ls")
TR(L"Can't find the game data in:\n%ls\n\nInstall again with Setup.",
   L"Не удаётся найти данные игры в:\n%ls\n\nУстановите игру заново через Setup.")
TR(L"Change settings and try again\nSafe mode, window, resolution scale, frame rate...",
   L"Изменить настройки и попробовать снова\nБезопасный режим, окно, масштаб разрешения, частота кадров...")
TR(L"Show the log",
   L"Показать журнал")
TR(L"Couldn't start the game",
   L"Не удалось запустить игру")
TR(L"The game closed while starting. Safe mode, a lower resolution scale or a window may help.",
   L"Игра закрылась при запуске. Может помочь безопасный режим, меньший масштаб разрешения или окно.")
TR(L"Couldn't start the game.\n\n%ls\n%ls\n\nLauncher log: %ls",
   L"Не удалось запустить игру.\n\n%ls\n%ls\n\nЖурнал лаунчера: %ls")
TR(L"(current: %hs)",
   L"(сейчас: %hs)")
TR(L"Every setting goes back to how the game was installed (all of them, F10's and the controls too). Your current file is kept as a copy beside it.\n\nContinue?",
   L"Все настройки вернутся к состоянию после установки (все, включая F10 и управление). Текущий файл сохранится как копия рядом.\n\nПродолжить?")
TR(L"settings.ini could not be moved aside (is another program holding it?).",
   L"Не удалось переименовать settings.ini (его занимает другая программа?).")
TR(L"The settings could not be saved to settings.ini.",
   L"Не удалось сохранить настройки в settings.ini.")
TR(L"They apply when the game starts. Every setting is in the game under F10.",
   L"Они применяются при запуске игры. Все настройки есть в игре в F10.")
TR(L"Reset all settings...",
   L"Сбросить все настройки...")
TR(L"Cancel",
   L"Отмена")
TR(L"Windowed",
   L"Оконный")
TR(L"Borderless fullscreen (default)",
   L"Полный экран без рамки (по умолч.)")
TR(L"Exclusive fullscreen",
   L"Эксклюзивный полный экран")
TR(L"1280 x 720 (default)",
   L"1280 x 720 (по умолч.)")
TR(L"30 (the game's own)",
   L"30 (как в игре)")
TR(L"Unlocked (monitor rate)",
   L"Без лимита (частота монитора)")
TR(L"Unlocked, steady (a rate it can hold)",
   L"Без лимита, стабильно")
TR(L"On (default, no tearing)",
   L"Вкл. (по умолч., без разрывов)")
TR(L"Off (tearing, lowest latency)",
   L"Выкл. (разрывы, мин. задержка)")
TR(L"100 % (native)",
   L"100 % (нативное)")
TR(L"Follow the window height",
   L"По высоте окна")
TR(L"Off",
   L"Выкл.")
TR(L"FSR (any GPU)",
   L"FSR (любой GPU)")
TR(L"Limit to 0.1 s (default)",
   L"Лимит 0.1 с (по умолч.)")
TR(L"Always wait (nothing missing)",
   L"Всегда ждать (всё на месте)")
TR(L"On: slower, for a game that crashes or hangs",
   L"Вкл.: медленнее, если игра вылетает или зависает")
TR(L"Window",
   L"Окно")
TR(L"Window size (windowed)",
   L"Размер окна (оконный)")
TR(L"Frame rate",
   L"Частота кадров")
TR(L"Vertical sync",
   L"Вертикальная синхронизация")
TR(L"Resolution scale",
   L"Масштаб разрешения")
TR(L"Upscaler",
   L"Апскейлер")
TR(L"Frame generation",
   L"Генерация кадров")
TR(L"Waiting for new shaders",
   L"Ожидание новых шейдеров")
TR(L"Safe mode",
   L"Безопасный режим")
TR(L"%ls comes from a game version the converter doesn't know (revision %u). It converts saves of 1.1.x, 1.2.x and 1.4.x.",
   L"%ls — из версии игры, которую конвертер не знает (ревизия %u). Он конвертирует сохранения версий 1.1.x, 1.2.x и 1.4.x.")
TR(L"%ls has data that version 1.0.0 doesn't have and that the converter doesn't know (%u; a modified save, or one from a version after 1.4.x). Converted, the save would be broken.",
   L"В %ls есть данные, которых нет в версии 1.0.0 и которые конвертер не знает (%u; изменённое сохранение или из версии новее 1.4.x). После конвертации сохранение было бы повреждено.")
TR(L"%ls holds more than a 1.0.0 save has room for.",
   L"%ls содержит больше, чем помещается в сохранение 1.0.0.")
TR(L"The picture in %ls couldn't be converted.",
   L"Не удалось конвертировать картинку в %ls.")
TR(L"%ls couldn't be read or written.",
   L"Не удалось прочитать или записать %ls.")
TR(L"%ls is damaged or incomplete.",
   L"%ls повреждён или неполон.")
TR(L"\n\nThe save was from version %ls and is now a 1.0.0 save:",
   L"\n\nСохранение было из версии %ls и теперь стало сохранением 1.0.0:")
TR(L"\n· %d entries from later updates that 1.0.0 doesn't have were left out.",
   L"\n· Пропущено записей из поздних обновлений, которых нет в 1.0.0: %d.")
TR(L"\n· %d values that exist only in later versions were set back to their default.",
   L"\n· Значений, существующих только в поздних версиях, сброшено к умолчанию: %d.")
TR(L"\n· %d slot pictures were made smaller (%d couldn't be read and are now black).",
   L"\n· Картинок ячеек уменьшено: %d (%d не удалось прочитать, теперь они чёрные).")
TR(L"\n· The game settings (option.sav) couldn't be converted: the game starts with its defaults.",
   L"\n· Не удалось конвертировать настройки игры (option.sav): игра запустится с настройками по умолчанию.")
TR(L"Couldn't keep a copy of the save you wanted to import; nothing was changed.",
   L"Не удалось сохранить копию сохранения, которое вы хотели импортировать; ничего не изменено.")
TR(L"The save wasn't imported; your previous save is unchanged.",
   L"Сохранение не импортировано; прежнее сохранение не изменилось.")
TR(L"Done.",
   L"Готово.")
TR(L"\n\nThe original save, not converted, is kept in:\n",
   L"\n\nИсходное сохранение, без конвертации, хранится в:\n")
TR(L"unknown version",
   L"неизвестная версия")
TR(L"The game is version 1.0.0. A save from 1.1.x, 1.2.x or 1.4.x is converted to 1.0.0 when you import it; the original is kept, untouched, in the backups.",
   L"Игра версии 1.0.0. Сохранение из 1.1.x, 1.2.x или 1.4.x при импорте конвертируется в 1.0.0; оригинал остаётся нетронутым в резервных копиях.")
TR(L"imported original, not converted",
   L"импортированный оригинал, без конвертации")
TR(L"Import a save…\nFrom yuzu, Eden, Ryujinx, the previous version of nxrecomp or a folder (JKSV). Saves of later game versions (1.1.x to 1.4.x) are converted to 1.0.0.",
   L"Импортировать сохранение…\nИз yuzu, Eden, Ryujinx, прошлой версии nxrecomp или папки (JKSV). Сохранения поздних версий игры (с 1.1.x по 1.4.x) конвертируются в 1.0.0.")
TR(L"Community\nr/WeepingOfTheEmpire on Reddit: news, help, screenshots",
   L"Сообщество\nr/WeepingOfTheEmpire на Reddit: новости, помощь, скриншоты")
TR(L"Community\nAsk for help on r/WeepingOfTheEmpire (Reddit)",
   L"Сообщество\nПопросите помощи в r/WeepingOfTheEmpire (Reddit)")
