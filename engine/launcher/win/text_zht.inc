/* text_zht.inc: the launcher's (launcher.c ES_EN) and the settings dialog's (preplay.c PP, its rows) texts in
 * Traditional Chinese (Taiwan's words), TR(english, translation) each, looked up by the English (text_tr.h).
 * Checked by tools/engine/check_texts.py (same printf specifiers and line breaks, no entry the sources lack).
 * lang-ptbr 11-04. */
TR(L"\n\nOpen %ls now?",
   L"\n\n現在開啟 %ls 嗎？")
TR(L"Folder with the save (for example, the JKSV backup on the SD card)",
   L"存檔所在的資料夾（例如 SD 卡上的 JKSV 備份）")
TR(L"Couldn't back up the current save; nothing was changed.",
   L"無法備份當前存檔；未做任何更改。")
TR(L"Couldn't copy the save (step %d). Your previous save is unchanged.\n\n%ls",
   L"無法複製存檔（第 %d 步）。你之前的存檔保持不變。\n\n%ls")
TR(L"\n\nYour previous save is backed up in:\n",
   L"\n\n你之前的存檔已備份到：\n")
TR(L"folder",
   L"資料夾")
TR(L"slot",
   L"個存檔位")
TR(L"slots",
   L"個存檔位")
TR(L"Import the selected save\nYour current save is backed up first.",
   L"匯入所選存檔\n會先備份你當前的存檔。")
TR(L"Look in another folder…\nFor example, a JKSV backup from the console or a portable emulator.",
   L"在其他資料夾中尋找…\n例如來自主機的 JKSV 備份或便攜版模擬器。")
TR(L"Choose the save to import",
   L"選擇要匯入的存檔")
TR(L"No TotK saves found",
   L"未找到 TotK 存檔")
TR(L"There are no save backups yet.",
   L"還沒有存檔備份。")
TR(L"automatic",
   L"自動")
TR(L"before import",
   L"匯入前")
TR(L"before restore",
   L"恢復前")
TR(L"before update",
   L"更新前")
TR(L"Restore the selected backup\nYour current save is backed up first.",
   L"恢復所選備份\n會先備份你當前的存檔。")
TR(L"Choose the backup to restore",
   L"選擇要恢復的備份")
TR(L"save on this PC",
   L"此電腦上的存檔")
TR(L"Current save: %d %ls, saved on %ls.\n%ls",
   L"當前存檔：%d %ls，儲存於 %ls。\n%ls")
TR(L"No save yet.\n%ls",
   L"還沒有存檔。\n%ls")
TR(L"Play",
   L"開始遊戲")
TR(L"Restore a backup…",
   L"恢復備份…")
TR(L"Make a backup now",
   L"立即備份")
TR(L"Saves",
   L"存檔")
TR(L"Backup made:\n%ls",
   L"已備份：\n%ls")
TR(L"Couldn't make the backup.",
   L"無法備份。")
TR(L"There's no save to back up.",
   L"沒有可備份的存檔。")
TR(L"There are TotK saves you can import",
   L"有可以匯入的 TotK 存檔")
TR(L"Do you want to import one before you start? You can also do it later by holding Shift while opening the launcher.",
   L"要在開始前匯入一個嗎？之後也可以在開啟啟動器時按住 Shift 來匯入。")
TR(L"Open the new version\n%ls",
   L"開啟新版本\n%ls")
TR(L"There's a newer version of the game next to this folder:\n%ls\n\nYour save, your settings and the shader cache carry over on their own (your save is backed up before its first start).",
   L"此資料夾旁有更新版本的遊戲：\n%ls\n\n你的存檔、設定和著色器快取會自動沿用（首次啟動前會先備份存檔）。")
TR(L"Keep this one\nI won't ask about that folder again.",
   L"保留當前版本\n不再詢問該資料夾。")
TR(L"A newer version is available",
   L"有新版本可用")
TR(L"Couldn't open the new version (error %lu):\n%ls\n\nStaying on this one.",
   L"無法開啟新版本（錯誤 %lu）：\n%ls\n\n繼續使用當前版本。")
TR(L"A new version is out: %hs",
   L"新版本已釋出：%hs")
TR(L"This update compiles the game again on your PC: it takes 15 to 45 minutes, and you can't play meanwhile. Your save and your settings are kept.",
   L"此更新會在你的電腦上重新編譯遊戲：需要 15 到 45 分鐘，期間無法遊玩。你的存檔和設定會保留。")
TR(L"Update now\nThe game opens by itself when it's done.",
   L"立即更新\n完成後遊戲會自動開啟。")
TR(L"Next time\nPlay the installed version.",
   L"下次再說\n遊玩已安裝的版本。")
TR(L"Skip this version\nI won't ask about it again.",
   L"跳過此版本\n不再詢問此版本。")
TR(L"Downloading the installer: %lld of %lld MB",
   L"正在下載安裝程式：%lld / %lld MB")
TR(L"Installing: step %d of %d (%ls)",
   L"正在安裝：第 %d 步，共 %d 步（%ls）")
TR(L"Installing...",
   L"正在安裝……")
TR(L"Cancelling...",
   L"正在取消……")
TR(L"Getting ready...",
   L"正在準備……")
TR(L"Updating to %hs",
   L"正在更新到 %hs")
TR(L"Couldn't update to %hs; the installed version opens.\n\n%ls",
   L"無法更新到 %hs；將開啟已安裝的版本。\n\n%ls")
TR(L"AVX (enabled by Windows)",
   L"AVX（由 Windows 啟用）")
TR(L"This Windows doesn't have the Microsoft Store's \"VP9 Video Extensions\"%ls.\n\nCutscenes look the same: the game brings its own video decoder (libvpx) and decodes them on the processor instead of the graphics card. If you want the graphics card to do it, install \"VP9 Video Extensions\" from the Microsoft Store.\n\nThis notice only shows once.",
   L"此 Windows 沒有 Microsoft Store 的「VP9 視訊延伸模組」%ls。\n\n過場動畫看起來一樣：遊戲自帶視訊解碼器（libvpx），在處理器而非顯示卡上解碼。如果想讓顯示卡來解碼，請從 Microsoft Store 安裝「VP9 視訊延伸模組」。\n\n此提示只顯示一次。")
TR(L" (nor Media Foundation: an N edition without the Media Feature Pack)",
   L"（也沒有 Media Foundation：未安裝媒體功能包的 N 版）")
TR(L"it could not be opened. Unpack it yourself and drop its folder on the launcher",
   L"無法開啟它。請自行解壓縮，然後把資料夾拖到啟動器上")
TR(L"it has no romfs, exefs or cheats folder. If the mod is packed inside it, unpack it and drop that folder instead",
   L"其中沒有 romfs、exefs 或 cheats 資料夾。如果模組打包在裡面，請解壓縮後拖入那個資料夾")
TR(L"it is a mod for another game",
   L"這是其他遊戲的模組")
TR(L"it loads code of its own (subsdk / main), which a recompiled game cannot load",
   L"它會載入自己的程式碼（subsdk / main），重新編譯的遊戲無法載入")
TR(L"it could not be copied. Check that the mods folder beside the game can be written",
   L"無法複製它。請檢查遊戲旁的 mods 資料夾是否可寫入")
TR(L"The mod was not installed:\n%ls\n\nWhy: %ls.",
   L"模組未安裝：\n%ls\n\n原因：%ls。")
TR(L" + code patches",
   L" + 程式碼補丁")
TR(L"code patches",
   L"程式碼補丁")
TR(L" + cheats",
   L" + 金手指")
TR(L"cheats",
   L"金手指")
TR(L"\nIts files don't name a game version: it was put with the one you play (%ls).",
   L"\n其檔案未註明遊戲版本：已放到你遊玩的版本（%ls）中。")
TR(L"\nIt is made for game version %ls, which the port does not build: it was put with the 1.4.3 mods and may not work.",
   L"\n它是為遊戲版本 %ls 製作的，移植版不建置該版本：已放到 1.4.3 的模組中，可能無法使用。")
TR(L"Mod installed: %ls (%ls, for %ls)\nin %ls%ls\n\nOrder and switches: F10 > Mods.",
   L"模組已安裝：%ls（%ls，適用於 %ls）\n位於 %ls%ls\n\n順序和開關：F10 > Mod。")
TR(L"The game reads its data from your game dump, which is no longer at:\n%ls\n\nIf you moved it, pick it again. If you'll delete it, install again with Setup and tick \"Copy the game files\".",
   L"遊戲從你的遊戲轉儲中讀取資料，但它已不在：\n%ls\n\n如果你移動了它，請重新選擇。如果要刪除它，請用 Setup 重新安裝並勾選「複製遊戲檔案」。")
TR(L"Pick the dump",
   L"選擇遊戲轉儲")
TR(L"Close",
   L"關閉")
TR(L"Can't find your game dump",
   L"找不到你的遊戲轉儲")
TR(L"Switch game (NSP, XCI)",
   L"Switch 遊戲（NSP、XCI）")
TR(L"Your game dump (the one Setup read)",
   L"你的遊戲轉儲（Setup 讀取的那個）")
TR(L"The file that says where the game data is, is damaged:\n%ls\n\nInstall again with Setup.",
   L"記錄遊戲資料位置的檔案已損壞：\n%ls\n\n請用 Setup 重新安裝。")
TR(L"That file is not the game dump Setup read:\n%ls",
   L"該檔案不是 Setup 讀取的遊戲轉儲：\n%ls")
TR(L"Couldn't save the new location in:\n%ls",
   L"無法將新位置儲存到：\n%ls")
TR(L"Settings",
   L"設定")
TR(L"%ls: incomplete (totk.exe, player.txt or img\\main.bin is missing)",
   L"%ls：不完整（缺少 totk.exe、player.txt 或 img\\main.bin）")
TR(L"%ls: this build of the game was made for processors with AVX2, and this one lacks %ls. Make it again with the Builder on this PC: it picks the build for processors without AVX2 by itself.",
   L"%ls：此遊戲版本是為支援 AVX2 的處理器建置的，而此處理器缺少 %ls。請在這台電腦上用 Builder 重新建置：它會自動選擇適用於無 AVX2 處理器的版本。")
TR(L" Your 1.0.0 saves were copied to this version once: what you play later in 1.0.0 will not show up here, and %ls saves do not go back to 1.0.0.",
   L"你的 1.0.0 存檔已複製到此版本一次：之後在 1.0.0 中的進度不會出現在這裡，%ls 的存檔也不會回到 1.0.0。")
TR(L"TotK %ls (beta). This version runs at full frame rate, but it is less tested than 1.0.0. The keyboard and mouse controls work as on 1.0.0 in play, except the ability keys 4 to 9 (pick abilities from the ability wheel instead). In menus, use the keyboard or a controller: the mouse pointer does not work in 1.4.3's menus yet.%ls",
   L"TotK %ls（測試版）。此版本以完整影格率執行，但測試不如 1.0.0 充分。遊戲中的鍵盤和滑鼠操作與 1.0.0 相同，但能力鍵 4 至 9 除外（請改在能力輪盤中選擇能力）。在選單中請使用鍵盤或控制器：1.4.3 的選單中暫不支援滑鼠指標。%ls")
TR(L"%ls: couldn't start (error %lu)",
   L"%ls：無法啟動（錯誤 %lu）")
TR(L"%ls: the game used an instruction this processor doesn't have (code 0xC000001D)\nLog: %ls",
   L"%ls：遊戲使用了此處理器沒有的指令（代碼 0xC000001D）\n日誌：%ls")
TR(L"%ls: the game closed while starting (code 0x%08lx)\nCrash report: %ls",
   L"%ls：遊戲在啟動時關閉（代碼 0x%08lx）\n崩潰報告：%ls")
TR(L"%ls: the game closed while starting (code 0x%08lx)\nLog: %ls",
   L"%ls：遊戲在啟動時關閉（代碼 0x%08lx）\n日誌：%ls")
TR(L"The game closed because of an error.\n\nCrash report:\n%ls",
   L"遊戲因錯誤而關閉。\n\n崩潰報告：\n%ls")
TR(L"The game closed with an error (code 0x%08lx).\n\nLogs: %ls",
   L"遊戲因錯誤關閉（代碼 0x%08lx）。\n\n日誌：%ls")
TR(L"The game restarted %d times in under a minute; it is not opened again.\n\nStart it again from the launcher.",
   L"遊戲在一分鐘內重啟了 %d 次，不再自動開啟。\n\n請從啟動器重新啟動。")
TR(L"The game is already running.",
   L"遊戲已在執行。")
TR(L"This processor can't run the game: it lacks instructions the game needs (%ls).\n\nIt needs at least SSE4.2 and POPCNT: a 1st-generation Intel Core (2008) or later, or an AMD FX or Ryzen.",
   L"此處理器無法執行遊戲：缺少遊戲所需的指令（%ls）。\n\n至少需要 SSE4.2 和 POPCNT：第 1 代 Intel Core（2008 年）或更新，或 AMD FX、Ryzen。")
TR(L"No Vulkan driver is installed (vulkan-1.dll is missing), and the game needs one.\n\nInstall the latest driver for your graphics card from the NVIDIA, AMD or Intel website. A Vulkan 1.3 GPU is required: NVIDIA GTX 900 or later, AMD RX 400 or later, Intel Arc or Iris Xe.",
   L"未安裝 Vulkan 驅動程式（缺少 vulkan-1.dll），而遊戲需要它。\n\n請從 NVIDIA、AMD 或 Intel 官網安裝顯示卡的最新驅動程式。需要支援 Vulkan 1.3 的 GPU：NVIDIA GTX 900 或更新、AMD RX 400 或更新、Intel Arc 或 Iris Xe。")
TR(L"(launcher folder)",
   L"（啟動器資料夾）")
TR(L"Can't find the game: there's no packages\\ with current.txt and no totk.exe next to the launcher.\n\n%ls",
   L"找不到遊戲：啟動器旁沒有含 current.txt 的 packages\\，也沒有 totk.exe。\n\n%ls")
TR(L"Can't find the game data (extracted RomFS) in:\n%ls",
   L"在以下位置找不到遊戲資料（解包的 RomFS）：\n%ls")
TR(L"Can't find the game data in:\n%ls\n\nInstall again with Setup.",
   L"在以下位置找不到遊戲資料：\n%ls\n\n請用 Setup 重新安裝。")
TR(L"Change settings and try again\nSafe mode, window, resolution scale, frame rate...",
   L"更改設定後重試\n安全模式、視窗、解析度縮放、影格率……")
TR(L"Show the log",
   L"顯示日誌")
TR(L"Couldn't start the game",
   L"無法啟動遊戲")
TR(L"The game closed while starting. Safe mode, a lower resolution scale or a window may help.",
   L"遊戲在啟動時關閉。安全模式、較低的解析度縮放或視窗模式可能有幫助。")
TR(L"Couldn't start the game.\n\n%ls\n%ls\n\nLauncher log: %ls",
   L"無法啟動遊戲。\n\n%ls\n%ls\n\n啟動器日誌：%ls")
TR(L"(current: %hs)",
   L"（當前：%hs）")
TR(L"Every setting goes back to how the game was installed (all of them, F10's and the controls too). Your current file is kept as a copy beside it.\n\nContinue?",
   L"所有設定都會恢復到安裝時的狀態（全部，包括 F10 和操作設定）。當前檔案會作為副本保留在旁邊。\n\n繼續嗎？")
TR(L"settings.ini could not be moved aside (is another program holding it?).",
   L"無法將 settings.ini 移到一旁（是否有其他程式佔用？）。")
TR(L"The settings could not be saved to settings.ini.",
   L"無法將設定儲存到 settings.ini。")
TR(L"They apply when the game starts. Every setting is in the game under F10.",
   L"這些設定在遊戲啟動時生效。所有設定都在遊戲中的 F10 裡。")
TR(L"Reset all settings...",
   L"重設所有設定……")
TR(L"Cancel",
   L"取消")
TR(L"Windowed",
   L"視窗化")
TR(L"Borderless fullscreen (default)",
   L"無邊框全螢幕（預設）")
TR(L"Exclusive fullscreen",
   L"獨佔全螢幕")
TR(L"1280 x 720 (default)",
   L"1280 x 720（預設）")
TR(L"30 (the game's own)",
   L"30（遊戲自帶）")
TR(L"Unlocked (monitor rate)",
   L"不限影格率（顯示器更新率）")
TR(L"Unlocked, steady (a rate it can hold)",
   L"不限影格率，穩定（可保持的影格率）")
TR(L"On (default, no tearing)",
   L"開啟（預設，無撕裂）")
TR(L"Off (tearing, lowest latency)",
   L"關閉（有撕裂，延遲最低）")
TR(L"100 % (native)",
   L"100 %（原生）")
TR(L"Follow the window height",
   L"跟隨視窗高度")
TR(L"Off",
   L"關閉")
TR(L"FSR (any GPU)",
   L"FSR（任意 GPU）")
TR(L"Limit to 0.1 s (default)",
   L"限制為 0.1 s（預設）")
TR(L"Always wait (nothing missing)",
   L"始終等待（無缺失）")
TR(L"On: slower, for a game that crashes or hangs",
   L"開啟：較慢，用於會崩潰或卡死的遊戲")
TR(L"Window",
   L"視窗")
TR(L"Window size (windowed)",
   L"視窗大小（視窗化）")
TR(L"Frame rate",
   L"影格率")
TR(L"Vertical sync",
   L"垂直同步")
TR(L"Resolution scale",
   L"解析度縮放")
TR(L"Upscaler",
   L"超解析度")
TR(L"Frame generation",
   L"影格生成")
TR(L"Waiting for new shaders",
   L"等待新著色器")
TR(L"Safe mode",
   L"安全模式")
TR(L"%ls comes from a game version the converter doesn't know (revision %u). It converts saves of 1.1.x, 1.2.x and 1.4.x.",
   L"%ls 來自轉換器不認識的遊戲版本（修訂號 %u）。它可以轉換 1.1.x、1.2.x 和 1.4.x 的存檔。")
TR(L"%ls has data that version 1.0.0 doesn't have and that the converter doesn't know (%u; a modified save, or one from a version after 1.4.x). Converted, the save would be broken.",
   L"%ls 含有 1.0.0 版本沒有、轉換器也不認識的資料（%u；被修改過的存檔，或來自 1.4.x 之後的版本）。轉換後存檔會損壞。")
TR(L"%ls holds more than a 1.0.0 save has room for.",
   L"%ls 的內容超出了 1.0.0 存檔的容量。")
TR(L"The picture in %ls couldn't be converted.",
   L"無法轉換 %ls 中的圖片。")
TR(L"%ls couldn't be read or written.",
   L"無法讀取或寫入 %ls。")
TR(L"%ls is damaged or incomplete.",
   L"%ls 已損壞或不完整。")
TR(L"\n\nThe save was from version %ls and is now a 1.0.0 save:",
   L"\n\n該存檔來自 %ls 版本，現已轉換為 1.0.0 存檔：")
TR(L"\n· %d entries from later updates that 1.0.0 doesn't have were left out.",
   L"\n· 已去除 %d 條 1.0.0 沒有的後續更新資料。")
TR(L"\n· %d values that exist only in later versions were set back to their default.",
   L"\n· 已將 %d 個僅存在於後續版本的數值恢復為預設值。")
TR(L"\n· %d slot pictures were made smaller (%d couldn't be read and are now black).",
   L"\n· 已縮小 %d 張存檔點陣圖片（%d 張無法讀取，現為黑色）。")
TR(L"\n· The game settings (option.sav) couldn't be converted: the game starts with its defaults.",
   L"\n· 無法轉換遊戲設定（option.sav）：遊戲將以預設設定啟動。")
TR(L"Couldn't keep a copy of the save you wanted to import; nothing was changed.",
   L"無法保留你要匯入的存檔的副本；未做任何更改。")
TR(L"The save wasn't imported; your previous save is unchanged.",
   L"存檔未匯入；你之前的存檔保持不變。")
TR(L"Done.",
   L"完成。")
TR(L"\n\nThe original save, not converted, is kept in:\n",
   L"\n\n未轉換的原始存檔儲存在：\n")
TR(L"unknown version",
   L"未知版本")
TR(L"The game is version 1.0.0. A save from 1.1.x, 1.2.x or 1.4.x is converted to 1.0.0 when you import it; the original is kept, untouched, in the backups.",
   L"遊戲版本為 1.0.0。匯入 1.1.x、1.2.x 或 1.4.x 的存檔時會轉換為 1.0.0；原始存檔原封不動地保留在備份中。")
TR(L"imported original, not converted",
   L"匯入的原始存檔，未轉換")
TR(L"Import a save…\nFrom yuzu, Eden, Ryujinx, the previous version of nxrecomp or a folder (JKSV). Saves of later game versions (1.1.x to 1.4.x) are converted to 1.0.0.",
   L"匯入存檔…\n來自 yuzu、Eden、Ryujinx、nxrecomp 的舊版本或資料夾（JKSV）。後續遊戲版本（1.1.x 至 1.4.x）的存檔會轉換為 1.0.0。")
TR(L"Community\nr/WeepingOfTheEmpire on Reddit: news, help, screenshots",
   L"社群\nReddit 上的 r/WeepingOfTheEmpire：新聞、幫助、截圖")
TR(L"Community\nAsk for help on r/WeepingOfTheEmpire (Reddit)",
   L"社群\n在 r/WeepingOfTheEmpire（Reddit）尋求幫助")
