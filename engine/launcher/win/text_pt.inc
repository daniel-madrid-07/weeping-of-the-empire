/* text_pt.inc: the launcher's (launcher.c ES_EN) and the settings dialog's (preplay.c PP, its rows) texts in
 * Brazilian Portuguese, PT(english, translation) each, looked up by the English (text_pt.h). Checked by
 * tools/engine/check_texts.py (same printf specifiers and line breaks, no entry the sources lack). lang-ptbr 11-02. */
PT(L"\n\nOpen %ls now?",
   L"\n\nAbrir %ls agora?")
PT(L"Folder with the save (for example, the JKSV backup on the SD card)",
   L"Pasta com o jogo salvo (por exemplo, o backup do JKSV no cartão SD)")
PT(L"Couldn't back up the current save; nothing was changed.",
   L"Não foi possível fazer backup do jogo salvo atual; nada foi alterado.")
PT(L"Couldn't copy the save (step %d). Your previous save is unchanged.\n\n%ls",
   L"Não foi possível copiar o jogo salvo (etapa %d). Seu jogo salvo anterior não foi alterado.\n\n%ls")
PT(L"\n\nYour previous save is backed up in:\n",
   L"\n\nSeu jogo salvo anterior está em backup em:\n")
PT(L"folder",
   L"pasta")
PT(L"slot",
   L"slot")
PT(L"slots",
   L"slots")
PT(L"Import the selected save\nYour current save is backed up first.",
   L"Importar o jogo salvo selecionado\nSeu jogo salvo atual passa por backup antes.")
PT(L"Look in another folder…\nFor example, a JKSV backup from the console or a portable emulator.",
   L"Procurar em outra pasta…\nPor exemplo, um backup do JKSV do console ou de um emulador portátil.")
PT(L"Choose the save to import",
   L"Escolha o jogo salvo a importar")
PT(L"No TotK saves found",
   L"Nenhum jogo salvo de TotK encontrado")
PT(L"There are no save backups yet.",
   L"Ainda não há backups do jogo salvo.")
PT(L"automatic",
   L"automático")
PT(L"before import",
   L"antes de importar")
PT(L"before restore",
   L"antes de restaurar")
PT(L"before update",
   L"antes de atualizar")
PT(L"Restore the selected backup\nYour current save is backed up first.",
   L"Restaurar o backup selecionado\nSeu jogo salvo atual passa por backup antes.")
PT(L"Choose the backup to restore",
   L"Escolha o backup a restaurar")
PT(L"save on this PC",
   L"jogo salvo neste PC")
PT(L"Current save: %d %ls, saved on %ls.\n%ls",
   L"Jogo salvo atual: %d %ls, salvo em %ls.\n%ls")
PT(L"No save yet.\n%ls",
   L"Ainda não há jogo salvo.\n%ls")
PT(L"Play",
   L"Jogar")
PT(L"Restore a backup…",
   L"Restaurar um backup…")
PT(L"Make a backup now",
   L"Fazer backup agora")
PT(L"Saves",
   L"Jogos salvos")
PT(L"Backup made:\n%ls",
   L"Backup feito:\n%ls")
PT(L"Couldn't make the backup.",
   L"Não foi possível fazer o backup.")
PT(L"There's no save to back up.",
   L"Não há jogo salvo para fazer backup.")
PT(L"There are TotK saves you can import",
   L"Há jogos salvos de TotK que você pode importar")
PT(L"Do you want to import one before you start? You can also do it later by holding Shift while opening the launcher.",
   L"Deseja importar um antes de começar? Você também pode fazer isso depois, segurando Shift ao abrir o launcher.")
PT(L"Open the new version\n%ls",
   L"Abrir a nova versão\n%ls")
PT(L"There's a newer version of the game next to this folder:\n%ls\n\nYour save, your settings and the shader cache carry over on their own (your save is backed up before its first start).",
   L"Há uma versão mais nova do jogo ao lado desta pasta:\n%ls\n\nSeu jogo salvo, suas configurações e o cache de shaders são transferidos automaticamente (seu jogo salvo passa por backup antes da primeira inicialização).")
PT(L"Keep this one\nI won't ask about that folder again.",
   L"Manter esta\nNão vou perguntar sobre essa pasta de novo.")
PT(L"A newer version is available",
   L"Há uma versão mais nova disponível")
PT(L"Couldn't open the new version (error %lu):\n%ls\n\nStaying on this one.",
   L"Não foi possível abrir a nova versão (erro %lu):\n%ls\n\nContinuando nesta.")
PT(L"A new version is out: %hs",
   L"Há uma nova versão: %hs")
PT(L"This update compiles the game again on your PC: it takes 15 to 45 minutes, and you can't play meanwhile. Your save and your settings are kept.",
   L"Esta atualização compila o jogo novamente no seu PC: leva de 15 a 45 minutos e você não pode jogar enquanto isso. Seu jogo salvo e suas configurações são mantidos.")
PT(L"Update now\nThe game opens by itself when it's done.",
   L"Atualizar agora\nO jogo abre sozinho ao terminar.")
PT(L"Next time\nPlay the installed version.",
   L"Da próxima vez\nJogar com a versão instalada.")
PT(L"Skip this version\nI won't ask about it again.",
   L"Ignorar esta versão\nNão vou perguntar sobre ela de novo.")
PT(L"Downloading the installer: %lld of %lld MB",
   L"Baixando o instalador: %lld de %lld MB")
PT(L"Installing: step %d of %d (%ls)",
   L"Instalando: etapa %d de %d (%ls)")
PT(L"Installing...",
   L"Instalando...")
PT(L"Cancelling...",
   L"Cancelando...")
PT(L"Getting ready...",
   L"Preparando...")
PT(L"Updating to %hs",
   L"Atualizando para %hs")
PT(L"Couldn't update to %hs; the installed version opens.\n\n%ls",
   L"Não foi possível atualizar para %hs; a versão instalada será aberta.\n\n%ls")
PT(L"AVX (enabled by Windows)",
   L"AVX (ativado pelo Windows)")
PT(L"This Windows doesn't have the Microsoft Store's \"VP9 Video Extensions\"%ls.\n\nCutscenes look the same: the game brings its own video decoder (libvpx) and decodes them on the processor instead of the graphics card. If you want the graphics card to do it, install \"VP9 Video Extensions\" from the Microsoft Store.\n\nThis notice only shows once.",
   L"Este Windows não tem as \"Extensões de Vídeo VP9\" da Microsoft Store%ls.\n\nAs cutscenes ficam iguais: o jogo traz seu próprio decodificador de vídeo (libvpx) e as decodifica no processador em vez de na placa de vídeo. Se quiser que a placa de vídeo faça isso, instale as \"Extensões de Vídeo VP9\" pela Microsoft Store.\n\nEste aviso aparece só uma vez.")
PT(L" (nor Media Foundation: an N edition without the Media Feature Pack)",
   L" (nem o Media Foundation: uma edição N sem o Media Feature Pack)")
PT(L"it could not be opened. Unpack it yourself and drop its folder on the launcher",
   L"não foi possível abri-lo. Descompacte-o você mesmo e solte a pasta dele no launcher")
PT(L"it has no romfs, exefs or cheats folder. If the mod is packed inside it, unpack it and drop that folder instead",
   L"ele não tem pasta romfs, exefs nem cheats. Se o mod estiver compactado dentro dele, descompacte-o e solte essa pasta")
PT(L"it is a mod for another game",
   L"é um mod de outro jogo")
PT(L"it loads code of its own (subsdk / main), which a recompiled game cannot load",
   L"carrega código próprio (subsdk / main), que um jogo recompilado não consegue carregar")
PT(L"it could not be copied. Check that the mods folder beside the game can be written",
   L"não foi possível copiá-lo. Verifique se é possível gravar na pasta mods ao lado do jogo")
PT(L"The mod was not installed:\n%ls\n\nWhy: %ls.",
   L"O mod não foi instalado:\n%ls\n\nMotivo: %ls.")
PT(L" + code patches",
   L" + patches de código")
PT(L"code patches",
   L"patches de código")
PT(L" + cheats",
   L" + cheats")
PT(L"cheats",
   L"cheats")
PT(L"\nIts files don't name a game version: it was put with the one you play (%ls).",
   L"\nOs arquivos dele não indicam a versão do jogo: ele foi colocado na versão que você joga (%ls).")
PT(L"\nIt is made for game version %ls, which the port does not build: it was put with the 1.4.3 mods and may not work.",
   L"\nFoi feito para a versão %ls do jogo, que o port não gera: foi colocado com os mods da 1.4.3 e pode não funcionar.")
PT(L"Mod installed: %ls (%ls, for %ls)\nin %ls%ls\n\nOrder and switches: F10 > Mods.",
   L"Mod instalado: %ls (%ls, para %ls)\nem %ls%ls\n\nOrdem e ativação: F10 > Mods.")
PT(L"The game reads its data from your game dump, which is no longer at:\n%ls\n\nIf you moved it, pick it again. If you'll delete it, install again with Setup and tick \"Copy the game files\".",
   L"O jogo lê seus dados do seu dump do jogo, que não está mais em:\n%ls\n\nSe você o moveu, escolha-o de novo. Se for apagá-lo, instale novamente com o Setup marcando \"Copiar os arquivos do jogo\".")
PT(L"Pick the dump",
   L"Escolher o dump")
PT(L"Close",
   L"Fechar")
PT(L"Can't find your game dump",
   L"Não foi possível encontrar seu dump do jogo")
PT(L"Switch game (NSP, XCI)",
   L"Jogo de Switch (NSP, XCI)")
PT(L"Your game dump (the one Setup read)",
   L"Seu dump do jogo (o mesmo que o Setup leu)")
PT(L"The file that says where the game data is, is damaged:\n%ls\n\nInstall again with Setup.",
   L"O arquivo que indica onde estão os dados do jogo está danificado:\n%ls\n\nInstale novamente com o Setup.")
PT(L"That file is not the game dump Setup read:\n%ls",
   L"Esse arquivo não é o dump do jogo que o Setup leu:\n%ls")
PT(L"Couldn't save the new location in:\n%ls",
   L"Não foi possível salvar o novo local em:\n%ls")
PT(L"Settings",
   L"Configurações")
PT(L"%ls: incomplete (totk.exe, player.txt or img\\main.bin is missing)",
   L"%ls: incompleto (falta totk.exe, player.txt ou img\\main.bin)")
PT(L"%ls: this build of the game was made for processors with AVX2, and this one lacks %ls. Make it again with the Builder on this PC: it picks the build for processors without AVX2 by itself.",
   L"%ls: esta versão do jogo foi criada para processadores com AVX2, e este não tem %ls. Crie-a novamente com o Builder neste PC: ele escolhe sozinho a versão para processadores sem AVX2.")
PT(L" Your 1.0.0 saves were copied to this version once: what you play later in 1.0.0 will not show up here, and %ls saves do not go back to 1.0.0.",
   L" Seus jogos salvos da 1.0.0 foram copiados uma vez para esta versão: o que você jogar depois na 1.0.0 não aparecerá aqui, e os jogos salvos da %ls não voltam para a 1.0.0.")
PT(L"TotK %ls (beta). This version plays at 30 fps, with the controls as a pad (keyboard and mouse act as Switch buttons) and without the in-game PC options screen (settings are in F10).%ls",
   L"TotK %ls (beta). Esta versão roda a 30 fps, com os controles como um gamepad (teclado e mouse funcionam como botões do Switch) e sem a tela de opções de PC dentro do jogo (as configurações ficam em F10).%ls")
PT(L"%ls: couldn't start (error %lu)",
   L"%ls: não foi possível iniciar (erro %lu)")
PT(L"%ls: the game used an instruction this processor doesn't have (code 0xC000001D)\nLog: %ls",
   L"%ls: o jogo usou uma instrução que este processador não tem (código 0xC000001D)\nLog: %ls")
PT(L"%ls: the game closed while starting (code 0x%08lx)\nCrash report: %ls",
   L"%ls: o jogo foi fechado ao iniciar (código 0x%08lx)\nRelatório de falha: %ls")
PT(L"%ls: the game closed while starting (code 0x%08lx)\nLog: %ls",
   L"%ls: o jogo foi fechado ao iniciar (código 0x%08lx)\nLog: %ls")
PT(L"The game closed because of an error.\n\nCrash report:\n%ls",
   L"O jogo foi fechado devido a um erro.\n\nRelatório de falha:\n%ls")
PT(L"The game closed with an error (code 0x%08lx).\n\nLogs: %ls",
   L"O jogo foi fechado com um erro (código 0x%08lx).\n\nLogs: %ls")
PT(L"The game restarted %d times in under a minute; it is not opened again.\n\nStart it again from the launcher.",
   L"O jogo reiniciou %d vezes em menos de um minuto; ele não será aberto novamente.\n\nInicie-o novamente pelo launcher.")
PT(L"The game is already running.",
   L"O jogo já está em execução.")
PT(L"This processor can't run the game: it lacks instructions the game needs (%ls).\n\nIt needs at least SSE4.2 and POPCNT: a 1st-generation Intel Core (2008) or later, or an AMD FX or Ryzen.",
   L"Este processador não consegue executar o jogo: faltam instruções de que o jogo precisa (%ls).\n\nSão necessários pelo menos SSE4.2 e POPCNT: um Intel Core de 1ª geração (2008) ou mais recente, ou um AMD FX ou Ryzen.")
PT(L"No Vulkan driver is installed (vulkan-1.dll is missing), and the game needs one.\n\nInstall the latest driver for your graphics card from the NVIDIA, AMD or Intel website. A Vulkan 1.3 GPU is required: NVIDIA GTX 900 or later, AMD RX 400 or later, Intel Arc or Iris Xe.",
   L"Nenhum driver Vulkan está instalado (falta vulkan-1.dll), e o jogo precisa de um.\n\nInstale o driver mais recente da sua placa de vídeo pelo site da NVIDIA, AMD ou Intel. É necessária uma GPU com Vulkan 1.3: NVIDIA GTX 900 ou mais recente, AMD RX 400 ou mais recente, Intel Arc ou Iris Xe.")
PT(L"(launcher folder)",
   L"(pasta do launcher)")
PT(L"Can't find the game: there's no packages\\ with current.txt and no totk.exe next to the launcher.\n\n%ls",
   L"Não foi possível encontrar o jogo: não há packages\\ com current.txt nem totk.exe ao lado do launcher.\n\n%ls")
PT(L"Can't find the game data (extracted RomFS) in:\n%ls",
   L"Não foi possível encontrar os dados do jogo (RomFS extraído) em:\n%ls")
PT(L"Can't find the game data in:\n%ls\n\nInstall again with Setup.",
   L"Não foi possível encontrar os dados do jogo em:\n%ls\n\nInstale novamente com o Setup.")
PT(L"Change settings and try again\nSafe mode, window, resolution scale, frame rate...",
   L"Alterar configurações e tentar de novo\nModo seguro, janela, escala de resolução, taxa de quadros...")
PT(L"Show the log",
   L"Ver o log")
PT(L"Couldn't start the game",
   L"Não foi possível iniciar o jogo")
PT(L"The game closed while starting. Safe mode, a lower resolution scale or a window may help.",
   L"O jogo foi fechado ao iniciar. O modo seguro, uma escala de resolução menor ou o modo janela podem ajudar.")
PT(L"Couldn't start the game.\n\n%ls\n%ls\n\nLauncher log: %ls",
   L"Não foi possível iniciar o jogo.\n\n%ls\n%ls\n\nLog do launcher: %ls")
PT(L"(current: %hs)",
   L"(atual: %hs)")
PT(L"Every setting goes back to how the game was installed (all of them, F10's and the controls too). Your current file is kept as a copy beside it.\n\nContinue?",
   L"Todas as configurações voltam ao estado em que o jogo foi instalado (todas, inclusive as do F10 e os controles). Seu arquivo atual é mantido como uma cópia ao lado dele.\n\nContinuar?")
PT(L"settings.ini could not be moved aside (is another program holding it?).",
   L"Não foi possível pôr o settings.ini de lado (outro programa o está usando?).")
PT(L"The settings could not be saved to settings.ini.",
   L"Não foi possível salvar as configurações em settings.ini.")
PT(L"They apply when the game starts. Every setting is in the game under F10.",
   L"Elas são aplicadas quando o jogo inicia. Todas as configurações estão no jogo, em F10.")
PT(L"Reset all settings...",
   L"Redefinir todas as configurações...")
PT(L"Cancel",
   L"Cancelar")
PT(L"Windowed",
   L"Janela")
PT(L"Borderless fullscreen (default)",
   L"Tela cheia sem bordas (padrão)")
PT(L"Exclusive fullscreen",
   L"Tela cheia exclusiva")
PT(L"1280 x 720 (default)",
   L"1280 x 720 (padrão)")
PT(L"30 (the game's own)",
   L"30 (do próprio jogo)")
PT(L"Unlocked (monitor rate)",
   L"Sem limite (taxa do monitor)")
PT(L"Unlocked, steady (a rate it can hold)",
   L"Sem limite, estável (uma taxa que consiga manter)")
PT(L"On (default, no tearing)",
   L"Ativada (padrão, sem tearing)")
PT(L"Off (tearing, lowest latency)",
   L"Desativada (tearing, menor latência)")
PT(L"100 % (native)",
   L"100 % (nativa)")
PT(L"Follow the window height",
   L"Seguir a altura da janela")
PT(L"Off",
   L"Desativado")
PT(L"FSR (any GPU)",
   L"FSR (qualquer GPU)")
PT(L"Limit to 0.1 s (default)",
   L"Limitar a 0,1 s (padrão)")
PT(L"Always wait (nothing missing)",
   L"Sempre aguardar (nada fica faltando)")
PT(L"On: slower, for a game that crashes or hangs",
   L"Ativado: mais lento, para um jogo que fecha ou trava")
PT(L"Window",
   L"Janela")
PT(L"Window size (windowed)",
   L"Tamanho da janela (modo janela)")
PT(L"Frame rate",
   L"Taxa de quadros")
PT(L"Vertical sync",
   L"Sincronização vertical")
PT(L"Resolution scale",
   L"Escala de resolução")
PT(L"Upscaler",
   L"Upscaler")
PT(L"Frame generation",
   L"Geração de quadros")
PT(L"Waiting for new shaders",
   L"Aguardando novos shaders")
PT(L"Safe mode",
   L"Modo seguro")
PT(L"%ls comes from a game version the converter doesn't know (revision %u). It converts saves of 1.1.x, 1.2.x and 1.4.x.",
   L"%ls vem de uma versão do jogo que o conversor não conhece (revisão %u). Ele converte jogos salvos da 1.1.x, 1.2.x e 1.4.x.")
PT(L"%ls has data that version 1.0.0 doesn't have and that the converter doesn't know (%u; a modified save, or one from a version after 1.4.x). Converted, the save would be broken.",
   L"%ls tem dados que a versão 1.0.0 não tem e que o conversor não conhece (%u; um jogo salvo modificado ou de uma versão posterior à 1.4.x). Convertido, o jogo salvo ficaria corrompido.")
PT(L"%ls holds more than a 1.0.0 save has room for.",
   L"%ls tem mais dados do que cabem em um jogo salvo da 1.0.0.")
PT(L"The picture in %ls couldn't be converted.",
   L"Não foi possível converter a imagem de %ls.")
PT(L"%ls couldn't be read or written.",
   L"Não foi possível ler ou gravar %ls.")
PT(L"%ls is damaged or incomplete.",
   L"%ls está danificado ou incompleto.")
PT(L"\n\nThe save was from version %ls and is now a 1.0.0 save:",
   L"\n\nO jogo salvo era da versão %ls e agora é da 1.0.0:")
PT(L"\n· %d entries from later updates that 1.0.0 doesn't have were left out.",
   L"\n· %d dados de atualizações posteriores que a 1.0.0 não tem foram removidos.")
PT(L"\n· %d values that exist only in later versions were set back to their default.",
   L"\n· %d valores que só existem em versões posteriores voltaram ao padrão.")
PT(L"\n· %d slot pictures were made smaller (%d couldn't be read and are now black).",
   L"\n· %d imagens de espaço foram reduzidas (%d não puderam ser lidas e agora estão pretas).")
PT(L"\n· The game settings (option.sav) couldn't be converted: the game starts with its defaults.",
   L"\n· Não foi possível converter as configurações do jogo (option.sav): o jogo começa com as padrão.")
PT(L"Couldn't keep a copy of the save you wanted to import; nothing was changed.",
   L"Não foi possível guardar uma cópia do jogo salvo que você queria importar; nada foi alterado.")
PT(L"The save wasn't imported; your previous save is unchanged.",
   L"O jogo salvo não foi importado; o anterior continua igual.")
PT(L"Done.",
   L"Pronto.")
PT(L"\n\nThe original save, not converted, is kept in:\n",
   L"\n\nO jogo salvo original, sem converter, está guardado em:\n")
PT(L"unknown version",
   L"versão desconhecida")
PT(L"The game is version 1.0.0. A save from 1.1.x, 1.2.x or 1.4.x is converted to 1.0.0 when you import it; the original is kept, untouched, in the backups.",
   L"O jogo está na versão 1.0.0. Um jogo salvo da 1.1.x, 1.2.x ou 1.4.x é convertido para a 1.0.0 ao ser importado; o original fica guardado, intacto, nos backups.")
PT(L"imported original, not converted",
   L"original importado, sem converter")
PT(L"Import a save…\nFrom yuzu, Eden, Ryujinx, the previous version of nxrecomp or a folder (JKSV). Saves of later game versions (1.1.x to 1.4.x) are converted to 1.0.0.",
   L"Importar um jogo salvo…\nDo yuzu, Eden, Ryujinx, da versão anterior do nxrecomp ou de uma pasta (JKSV). Os de versões posteriores do jogo (1.1.x a 1.4.x) são convertidos para a 1.0.0.")
