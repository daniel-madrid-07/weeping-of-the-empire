/* text_zh.inc: the launcher's (launcher.c ES_EN) and the settings dialog's (preplay.c PP, its rows) texts in
 * Simplified Chinese, TR(english, translation) each, looked up by the English (text_tr.h). Checked by
 * tools/engine/check_texts.py (same printf specifiers and line breaks, no entry the sources lack). lang-ptbr 11-03. */
TR(L"\n\nOpen %ls now?",
   L"\n\n现在打开 %ls 吗？")
TR(L"Folder with the save (for example, the JKSV backup on the SD card)",
   L"存档所在的文件夹（例如 SD 卡上的 JKSV 备份）")
TR(L"Couldn't back up the current save; nothing was changed.",
   L"无法备份当前存档；未做任何更改。")
TR(L"Couldn't copy the save (step %d). Your previous save is unchanged.\n\n%ls",
   L"无法复制存档（第 %d 步）。你之前的存档保持不变。\n\n%ls")
TR(L"\n\nYour previous save is backed up in:\n",
   L"\n\n你之前的存档已备份到：\n")
TR(L"folder",
   L"文件夹")
TR(L"slot",
   L"个存档位")
TR(L"slots",
   L"个存档位")
TR(L"Import the selected save\nYour current save is backed up first.",
   L"导入所选存档\n会先备份你当前的存档。")
TR(L"Look in another folder…\nFor example, a JKSV backup from the console or a portable emulator.",
   L"在其他文件夹中查找…\n例如来自主机的 JKSV 备份或便携版模拟器。")
TR(L"Choose the save to import",
   L"选择要导入的存档")
TR(L"No TotK saves found",
   L"未找到 TotK 存档")
TR(L"There are no save backups yet.",
   L"还没有存档备份。")
TR(L"automatic",
   L"自动")
TR(L"before import",
   L"导入前")
TR(L"before restore",
   L"恢复前")
TR(L"before update",
   L"更新前")
TR(L"Restore the selected backup\nYour current save is backed up first.",
   L"恢复所选备份\n会先备份你当前的存档。")
TR(L"Choose the backup to restore",
   L"选择要恢复的备份")
TR(L"save on this PC",
   L"此电脑上的存档")
TR(L"Current save: %d %ls, saved on %ls.\n%ls",
   L"当前存档：%d %ls，保存于 %ls。\n%ls")
TR(L"No save yet.\n%ls",
   L"还没有存档。\n%ls")
TR(L"Play",
   L"开始游戏")
TR(L"Restore a backup…",
   L"恢复备份…")
TR(L"Make a backup now",
   L"立即备份")
TR(L"Saves",
   L"存档")
TR(L"Backup made:\n%ls",
   L"已备份：\n%ls")
TR(L"Couldn't make the backup.",
   L"无法备份。")
TR(L"There's no save to back up.",
   L"没有可备份的存档。")
TR(L"There are TotK saves you can import",
   L"有可以导入的 TotK 存档")
TR(L"Do you want to import one before you start? You can also do it later by holding Shift while opening the launcher.",
   L"要在开始前导入一个吗？之后也可以在打开启动器时按住 Shift 来导入。")
TR(L"Open the new version\n%ls",
   L"打开新版本\n%ls")
TR(L"There's a newer version of the game next to this folder:\n%ls\n\nYour save, your settings and the shader cache carry over on their own (your save is backed up before its first start).",
   L"此文件夹旁有更新版本的游戏：\n%ls\n\n你的存档、设置和着色器缓存会自动沿用（首次启动前会先备份存档）。")
TR(L"Keep this one\nI won't ask about that folder again.",
   L"保留当前版本\n不再询问该文件夹。")
TR(L"A newer version is available",
   L"有新版本可用")
TR(L"Couldn't open the new version (error %lu):\n%ls\n\nStaying on this one.",
   L"无法打开新版本（错误 %lu）：\n%ls\n\n继续使用当前版本。")
TR(L"A new version is out: %hs",
   L"新版本已发布：%hs")
TR(L"This update compiles the game again on your PC: it takes 15 to 45 minutes, and you can't play meanwhile. Your save and your settings are kept.",
   L"此更新会在你的电脑上重新编译游戏：需要 15 到 45 分钟，期间无法游玩。你的存档和设置会保留。")
TR(L"Update now\nThe game opens by itself when it's done.",
   L"立即更新\n完成后游戏会自动打开。")
TR(L"Next time\nPlay the installed version.",
   L"下次再说\n游玩已安装的版本。")
TR(L"Skip this version\nI won't ask about it again.",
   L"跳过此版本\n不再询问此版本。")
TR(L"Downloading the installer: %lld of %lld MB",
   L"正在下载安装程序：%lld / %lld MB")
TR(L"Installing: step %d of %d (%ls)",
   L"正在安装：第 %d 步，共 %d 步（%ls）")
TR(L"Installing...",
   L"正在安装……")
TR(L"Cancelling...",
   L"正在取消……")
TR(L"Getting ready...",
   L"正在准备……")
TR(L"Updating to %hs",
   L"正在更新到 %hs")
TR(L"Couldn't update to %hs; the installed version opens.\n\n%ls",
   L"无法更新到 %hs；将打开已安装的版本。\n\n%ls")
TR(L"AVX (enabled by Windows)",
   L"AVX（由 Windows 启用）")
TR(L"This Windows doesn't have the Microsoft Store's \"VP9 Video Extensions\"%ls.\n\nCutscenes look the same: the game brings its own video decoder (libvpx) and decodes them on the processor instead of the graphics card. If you want the graphics card to do it, install \"VP9 Video Extensions\" from the Microsoft Store.\n\nThis notice only shows once.",
   L"此 Windows 没有 Microsoft Store 的“VP9 视频扩展”%ls。\n\n过场动画看起来一样：游戏自带视频解码器（libvpx），在处理器而非显卡上解码。如果想让显卡来解码，请从 Microsoft Store 安装“VP9 视频扩展”。\n\n此提示只显示一次。")
TR(L" (nor Media Foundation: an N edition without the Media Feature Pack)",
   L"（也没有 Media Foundation：未安装媒体功能包的 N 版）")
TR(L"it could not be opened. Unpack it yourself and drop its folder on the launcher",
   L"无法打开它。请自行解压，然后把文件夹拖到启动器上")
TR(L"it has no romfs, exefs or cheats folder. If the mod is packed inside it, unpack it and drop that folder instead",
   L"其中没有 romfs、exefs 或 cheats 文件夹。如果模组打包在里面，请解压后拖入那个文件夹")
TR(L"it is a mod for another game",
   L"这是其他游戏的模组")
TR(L"it loads code of its own (subsdk / main), which a recompiled game cannot load",
   L"它会加载自己的代码（subsdk / main），重新编译的游戏无法加载")
TR(L"it could not be copied. Check that the mods folder beside the game can be written",
   L"无法复制它。请检查游戏旁的 mods 文件夹是否可写入")
TR(L"The mod was not installed:\n%ls\n\nWhy: %ls.",
   L"模组未安装：\n%ls\n\n原因：%ls。")
TR(L" + code patches",
   L" + 代码补丁")
TR(L"code patches",
   L"代码补丁")
TR(L" + cheats",
   L" + 金手指")
TR(L"cheats",
   L"金手指")
TR(L"\nIts files don't name a game version: it was put with the one you play (%ls).",
   L"\n其文件未注明游戏版本：已放到你游玩的版本（%ls）中。")
TR(L"\nIt is made for game version %ls, which the port does not build: it was put with the 1.4.3 mods and may not work.",
   L"\n它是为游戏版本 %ls 制作的，移植版不构建该版本：已放到 1.4.3 的模组中，可能无法使用。")
TR(L"Mod installed: %ls (%ls, for %ls)\nin %ls%ls\n\nOrder and switches: F10 > Mods.",
   L"模组已安装：%ls（%ls，适用于 %ls）\n位于 %ls%ls\n\n顺序和开关：F10 > Mod。")
TR(L"The game reads its data from your game dump, which is no longer at:\n%ls\n\nIf you moved it, pick it again. If you'll delete it, install again with Setup and tick \"Copy the game files\".",
   L"游戏从你的游戏转储中读取数据，但它已不在：\n%ls\n\n如果你移动了它，请重新选择。如果要删除它，请用 Setup 重新安装并勾选“复制游戏文件”。")
TR(L"Pick the dump",
   L"选择游戏转储")
TR(L"Close",
   L"关闭")
TR(L"Can't find your game dump",
   L"找不到你的游戏转储")
TR(L"Switch game (NSP, XCI)",
   L"Switch 游戏（NSP、XCI）")
TR(L"Your game dump (the one Setup read)",
   L"你的游戏转储（Setup 读取的那个）")
TR(L"The file that says where the game data is, is damaged:\n%ls\n\nInstall again with Setup.",
   L"记录游戏数据位置的文件已损坏：\n%ls\n\n请用 Setup 重新安装。")
TR(L"That file is not the game dump Setup read:\n%ls",
   L"该文件不是 Setup 读取的游戏转储：\n%ls")
TR(L"Couldn't save the new location in:\n%ls",
   L"无法将新位置保存到：\n%ls")
TR(L"Settings",
   L"设置")
TR(L"%ls: incomplete (totk.exe, player.txt or img\\main.bin is missing)",
   L"%ls：不完整（缺少 totk.exe、player.txt 或 img\\main.bin）")
TR(L"%ls: this build of the game was made for processors with AVX2, and this one lacks %ls. Make it again with the Builder on this PC: it picks the build for processors without AVX2 by itself.",
   L"%ls：此游戏版本是为支持 AVX2 的处理器构建的，而此处理器缺少 %ls。请在这台电脑上用 Builder 重新构建：它会自动选择适用于无 AVX2 处理器的版本。")
TR(L" Your 1.0.0 saves were copied to this version once: what you play later in 1.0.0 will not show up here, and %ls saves do not go back to 1.0.0.",
   L"你的 1.0.0 存档已复制到此版本一次：之后在 1.0.0 中的进度不会出现在这里，%ls 的存档也不会回到 1.0.0。")
TR(L"TotK %ls (beta). This version runs at full frame rate, but it is less tested than 1.0.0. The keyboard and mouse controls work as on 1.0.0 in play, except the ability keys 4 to 9 (pick abilities from the ability wheel instead). In menus, use the keyboard or a controller: the mouse pointer does not work in 1.4.3's menus yet.%ls",
   L"TotK %ls（测试版）。此版本以完整帧率运行，但测试不如 1.0.0 充分。游戏中的键盘和鼠标操作与 1.0.0 相同，但能力键 4 至 9 除外（请改在能力轮盘中选择能力）。在菜单中请使用键盘或手柄：1.4.3 的菜单中暂不支持鼠标指针。%ls")
TR(L"%ls: couldn't start (error %lu)",
   L"%ls：无法启动（错误 %lu）")
TR(L"%ls: the game used an instruction this processor doesn't have (code 0xC000001D)\nLog: %ls",
   L"%ls：游戏使用了此处理器没有的指令（代码 0xC000001D）\n日志：%ls")
TR(L"%ls: the game closed while starting (code 0x%08lx)\nCrash report: %ls",
   L"%ls：游戏在启动时关闭（代码 0x%08lx）\n崩溃报告：%ls")
TR(L"%ls: the game closed while starting (code 0x%08lx)\nLog: %ls",
   L"%ls：游戏在启动时关闭（代码 0x%08lx）\n日志：%ls")
TR(L"The game closed because of an error.\n\nCrash report:\n%ls",
   L"游戏因错误而关闭。\n\n崩溃报告：\n%ls")
TR(L"The game closed with an error (code 0x%08lx).\n\nLogs: %ls",
   L"游戏因错误关闭（代码 0x%08lx）。\n\n日志：%ls")
TR(L"The game restarted %d times in under a minute; it is not opened again.\n\nStart it again from the launcher.",
   L"游戏在一分钟内重启了 %d 次，不再自动打开。\n\n请从启动器重新启动。")
TR(L"The game is already running.",
   L"游戏已在运行。")
TR(L"This processor can't run the game: it lacks instructions the game needs (%ls).\n\nIt needs at least SSE4.2 and POPCNT: a 1st-generation Intel Core (2008) or later, or an AMD FX or Ryzen.",
   L"此处理器无法运行游戏：缺少游戏所需的指令（%ls）。\n\n至少需要 SSE4.2 和 POPCNT：第 1 代 Intel Core（2008 年）或更新，或 AMD FX、Ryzen。")
TR(L"No Vulkan driver is installed (vulkan-1.dll is missing), and the game needs one.\n\nInstall the latest driver for your graphics card from the NVIDIA, AMD or Intel website. A Vulkan 1.3 GPU is required: NVIDIA GTX 900 or later, AMD RX 400 or later, Intel Arc or Iris Xe.",
   L"未安装 Vulkan 驱动（缺少 vulkan-1.dll），而游戏需要它。\n\n请从 NVIDIA、AMD 或 Intel 官网安装显卡的最新驱动。需要支持 Vulkan 1.3 的 GPU：NVIDIA GTX 900 或更新、AMD RX 400 或更新、Intel Arc 或 Iris Xe。")
TR(L"(launcher folder)",
   L"（启动器文件夹）")
TR(L"Can't find the game: there's no packages\\ with current.txt and no totk.exe next to the launcher.\n\n%ls",
   L"找不到游戏：启动器旁没有含 current.txt 的 packages\\，也没有 totk.exe。\n\n%ls")
TR(L"Can't find the game data (extracted RomFS) in:\n%ls",
   L"在以下位置找不到游戏数据（解包的 RomFS）：\n%ls")
TR(L"Can't find the game data in:\n%ls\n\nInstall again with Setup.",
   L"在以下位置找不到游戏数据：\n%ls\n\n请用 Setup 重新安装。")
TR(L"Change settings and try again\nSafe mode, window, resolution scale, frame rate...",
   L"更改设置后重试\n安全模式、窗口、分辨率缩放、帧率……")
TR(L"Show the log",
   L"显示日志")
TR(L"Couldn't start the game",
   L"无法启动游戏")
TR(L"The game closed while starting. Safe mode, a lower resolution scale or a window may help.",
   L"游戏在启动时关闭。安全模式、较低的分辨率缩放或窗口模式可能有帮助。")
TR(L"Couldn't start the game.\n\n%ls\n%ls\n\nLauncher log: %ls",
   L"无法启动游戏。\n\n%ls\n%ls\n\n启动器日志：%ls")
TR(L"(current: %hs)",
   L"（当前：%hs）")
TR(L"Every setting goes back to how the game was installed (all of them, F10's and the controls too). Your current file is kept as a copy beside it.\n\nContinue?",
   L"所有设置都会恢复到安装时的状态（全部，包括 F10 和操作设置）。当前文件会作为副本保留在旁边。\n\n继续吗？")
TR(L"settings.ini could not be moved aside (is another program holding it?).",
   L"无法将 settings.ini 移到一旁（是否有其他程序占用？）。")
TR(L"The settings could not be saved to settings.ini.",
   L"无法将设置保存到 settings.ini。")
TR(L"They apply when the game starts. Every setting is in the game under F10.",
   L"这些设置在游戏启动时生效。所有设置都在游戏中的 F10 里。")
TR(L"Reset all settings...",
   L"重置所有设置……")
TR(L"Cancel",
   L"取消")
TR(L"Windowed",
   L"窗口化")
TR(L"Borderless fullscreen (default)",
   L"无边框全屏（默认）")
TR(L"Exclusive fullscreen",
   L"独占全屏")
TR(L"1280 x 720 (default)",
   L"1280 x 720（默认）")
TR(L"30 (the game's own)",
   L"30（游戏自带）")
TR(L"Unlocked (monitor rate)",
   L"不限帧率（显示器刷新率）")
TR(L"Unlocked, steady (a rate it can hold)",
   L"不限帧率，稳定（可保持的帧率）")
TR(L"On (default, no tearing)",
   L"开启（默认，无撕裂）")
TR(L"Off (tearing, lowest latency)",
   L"关闭（有撕裂，延迟最低）")
TR(L"100 % (native)",
   L"100 %（原生）")
TR(L"Follow the window height",
   L"跟随窗口高度")
TR(L"Off",
   L"关闭")
TR(L"FSR (any GPU)",
   L"FSR（任意 GPU）")
TR(L"Limit to 0.1 s (default)",
   L"限制为 0.1 s（默认）")
TR(L"Always wait (nothing missing)",
   L"始终等待（无缺失）")
TR(L"On: slower, for a game that crashes or hangs",
   L"开启：较慢，用于会崩溃或卡死的游戏")
TR(L"Window",
   L"窗口")
TR(L"Window size (windowed)",
   L"窗口大小（窗口化）")
TR(L"Frame rate",
   L"帧率")
TR(L"Vertical sync",
   L"垂直同步")
TR(L"Resolution scale",
   L"分辨率缩放")
TR(L"Upscaler",
   L"超分辨率")
TR(L"Frame generation",
   L"帧生成")
TR(L"Waiting for new shaders",
   L"等待新着色器")
TR(L"Safe mode",
   L"安全模式")
TR(L"%ls comes from a game version the converter doesn't know (revision %u). It converts saves of 1.1.x, 1.2.x and 1.4.x.",
   L"%ls 来自转换器不认识的游戏版本（修订号 %u）。它可以转换 1.1.x、1.2.x 和 1.4.x 的存档。")
TR(L"%ls has data that version 1.0.0 doesn't have and that the converter doesn't know (%u; a modified save, or one from a version after 1.4.x). Converted, the save would be broken.",
   L"%ls 含有 1.0.0 版本没有、转换器也不认识的数据（%u；被修改过的存档，或来自 1.4.x 之后的版本）。转换后存档会损坏。")
TR(L"%ls holds more than a 1.0.0 save has room for.",
   L"%ls 的内容超出了 1.0.0 存档的容量。")
TR(L"The picture in %ls couldn't be converted.",
   L"无法转换 %ls 中的图片。")
TR(L"%ls couldn't be read or written.",
   L"无法读取或写入 %ls。")
TR(L"%ls is damaged or incomplete.",
   L"%ls 已损坏或不完整。")
TR(L"\n\nThe save was from version %ls and is now a 1.0.0 save:",
   L"\n\n该存档来自 %ls 版本，现已转换为 1.0.0 存档：")
TR(L"\n· %d entries from later updates that 1.0.0 doesn't have were left out.",
   L"\n· 已去除 %d 条 1.0.0 没有的后续更新数据。")
TR(L"\n· %d values that exist only in later versions were set back to their default.",
   L"\n· 已将 %d 个仅存在于后续版本的数值恢复为默认值。")
TR(L"\n· %d slot pictures were made smaller (%d couldn't be read and are now black).",
   L"\n· 已缩小 %d 张存档位图片（%d 张无法读取，现为黑色）。")
TR(L"\n· The game settings (option.sav) couldn't be converted: the game starts with its defaults.",
   L"\n· 无法转换游戏设置（option.sav）：游戏将以默认设置启动。")
TR(L"Couldn't keep a copy of the save you wanted to import; nothing was changed.",
   L"无法保留你要导入的存档的副本；未做任何更改。")
TR(L"The save wasn't imported; your previous save is unchanged.",
   L"存档未导入；你之前的存档保持不变。")
TR(L"Done.",
   L"完成。")
TR(L"\n\nThe original save, not converted, is kept in:\n",
   L"\n\n未转换的原始存档保存在：\n")
TR(L"unknown version",
   L"未知版本")
TR(L"The game is version 1.0.0. A save from 1.1.x, 1.2.x or 1.4.x is converted to 1.0.0 when you import it; the original is kept, untouched, in the backups.",
   L"游戏版本为 1.0.0。导入 1.1.x、1.2.x 或 1.4.x 的存档时会转换为 1.0.0；原始存档原封不动地保留在备份中。")
TR(L"imported original, not converted",
   L"导入的原始存档，未转换")
TR(L"Import a save…\nFrom yuzu, Eden, Ryujinx, the previous version of nxrecomp or a folder (JKSV). Saves of later game versions (1.1.x to 1.4.x) are converted to 1.0.0.",
   L"导入存档…\n来自 yuzu、Eden、Ryujinx、nxrecomp 的旧版本或文件夹（JKSV）。后续游戏版本（1.1.x 至 1.4.x）的存档会转换为 1.0.0。")
TR(L"Community\nr/WeepingOfTheEmpire on Reddit: news, help, screenshots",
   L"社区\nReddit 上的 r/WeepingOfTheEmpire：新闻、帮助、截图")
TR(L"Community\nAsk for help on r/WeepingOfTheEmpire (Reddit)",
   L"社区\n在 r/WeepingOfTheEmpire（Reddit）寻求帮助")
