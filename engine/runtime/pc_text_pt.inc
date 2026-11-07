// pc_text_pt.inc: the Brazilian Portuguese of the PC port's own texts (pc_text.cpp, pc_ui_lang "pt"), one
// PC_TR(english, translation) each. A text missing here shows in English; checked by tools/engine/check_texts.py
// (the same printf specifiers, no entry the sources lack). pc-ui 11-02.
PC_TR("Audio",
      "Áudio")
PC_TR("Output",
      "Saída")
PC_TR("Speakers",
      "Alto-falantes")
PC_TR("TOTK_AUDIO_CHANNELS. 5.1 and 7.1 tell the game it has surround output (7.1 puts the rears on the back and side pairs); Windows remaps to the device. Experimental: with 6 channels the game's effect on one sub mix diverges and is muted.",
      "TOTK_AUDIO_CHANNELS. 5.1 e 7.1 informam ao jogo que há saída surround (o 7.1 coloca os canais traseiros nos pares de trás e laterais); o Windows adapta ao dispositivo. Experimental: com 6 canais, o efeito do jogo em um submix diverge e fica mudo.")
PC_TR("Stereo (default)",
      "Estéreo (padrão)")
PC_TR("Match the Windows device (experimental)",
      "Igual ao Windows (experimental)")
PC_TR("Latency",
      "Latência")
PC_TR("TOTK_AUDIO_LATENCY_MS: audio queued ahead of the device. Lower is more responsive; raise it if the sound crackles.",
      "TOTK_AUDIO_LATENCY_MS: áudio enfileirado antes do dispositivo. Menor responde mais rápido; aumente se o som estiver estalando.")
PC_TR("20 ms (default)",
      "20 ms (padrão)")
PC_TR("80 ms (wireless headsets)",
      "80 ms (fones sem fio)")
PC_TR("Follow the Windows device",
      "Seguir o dispositivo do Windows")
PC_TR("TOTK_AUDIO_FOLLOW: switch to the new default device (headphones plugged in or out) and recover a lost one without a restart.",
      "TOTK_AUDIO_FOLLOW: muda para o novo dispositivo padrão (ao conectar ou desconectar fones) e recupera um dispositivo perdido sem reiniciar.")
PC_TR("On (default)",
      "Ligado (padrão)")
PC_TR("Off",
      "Desligado")
PC_TR("Mix",
      "Mixagem")
PC_TR("Music",
      "Música")
PC_TR("Effects",
      "Efeitos")
PC_TR("Voice",
      "Vozes")
PC_TR("TOTK_AUDIO_VOICE. No sub mix carries voice alone yet, so by default this does nothing.",
      "TOTK_AUDIO_VOICE. Nenhum submix leva só a voz ainda, então por padrão isto não faz nada.")
PC_TR("the stick now",
      "o analógico agora")
PC_TR("Controls",
      "Controles")
PC_TR("Keyboard layout",
      "Layout do teclado")
PC_TR("For the console scheme, and with the PC scheme for the world while Gameplay keys is Console (menus use the PC keys). Classic: Space jump, Shift sprint, E interact, F attack, WASD, arrows camera. PC: E interact, Shift sprint, Space jump, left mouse attack, right mouse bow, Q target, F abilities. Custom: the PC keys changed by the bindings below.",
      "Para o esquema de console, e com o esquema de PC para o mundo enquanto Teclas de jogo estiver em Console (os menus usam as teclas de PC). Clássico: Espaço pula, Shift corre, E interage, F ataca, WASD, setas na câmera. PC: E interage, Shift corre, Espaço pula, botão esquerdo do mouse ataca, direito usa o arco, Q mira, F habilidades. Personalizado: as teclas de PC alteradas pelas atribuições abaixo.")
PC_TR("Classic (the engine's keys)",
      "Clássico (teclas do engine)")
PC_TR("Custom (the bindings below)",
      "Personalizado (atribuições abaixo)")
PC_TR("Minimum movement key press",
      "Toque mínimo de movimento")
PC_TR("Milliseconds a quick tap of a movement key (WASD) keeps Link moving, so he takes a step instead of sliding with his legs still. 0 = off (a tap as short as the key press).",
      "Milissegundos que um toque rápido numa tecla de movimento (WASD) mantém Link andando, para ele dar um passo em vez de deslizar com as pernas paradas. 0 = desligado (um toque tão curto quanto o da tecla).")
PC_TR("Sprint toggle",
      "Alternar corrida")
PC_TR("A press of the sprint key starts sprinting and the next one stops it; it also stops when Link stops moving or a menu opens. Off: hold it, as on the Switch.",
      "Um toque na tecla de corrida começa a correr e o seguinte interrompe; também termina quando Link para de se mover ou um menu abre. Desligado: segure, como no Switch.")
PC_TR("On",
      "Ligado")
PC_TR("Shield and target toggle",
      "Alternar escudo e mira")
PC_TR("A press of the shield / target key raises the shield and targets, the next press lowers it; a menu lets it go too. Off: hold it, as on the Switch.",
      "Um toque na tecla de escudo / mira levanta o escudo e fixa o alvo, o seguinte o baixa; abrir um menu também o solta. Desligado: segure, como no Switch.")
PC_TR("Mouse camera",
      "Câmera do mouse")
PC_TR("The mouse turns the camera (click the window to capture the pointer; Esc, F10 or Alt+Tab frees it). 0 = off; above 1 turns faster.",
      "O mouse gira a câmera (clique na janela para capturar o ponteiro; Esc, F10 ou Alt+Tab o libera). 0 = desligado; acima de 1 gira mais rápido.")
PC_TR("Pad",
      "Controle")
PC_TR("Button prompts",
      "Ícones de botões")
PC_TR("The button pictures in every prompt and menu, for the keys and buttons bound. Automatic follows the device used last: keyboard and mouse, an Xbox (XInput) pad, a DualSense / DualShock 4, or the game's own with a Nintendo pad. A change shows at once, in every prompt and label; with a pad in use, menus hide the mouse pointer until the mouse moves.",
      "As imagens dos botões em todos os avisos e menus, para as teclas e botões atribuídos. Automático segue o último dispositivo usado: teclado e mouse, controle Xbox (XInput), DualSense / DualShock 4, ou os do próprio jogo com controle Nintendo. A mudança aparece na hora, em todos os avisos e rótulos; com um controle em uso, os menus ocultam o ponteiro do mouse até o mouse se mover.")
PC_TR("Automatic (the device used last)",
      "Automático (último dispositivo usado)")
PC_TR("Menu buttons",
      "Botões nos menus")
PC_TR("Same as the game",
      "Como no jogo")
PC_TR("Printed A accepts, B goes back",
      "O A impresso confirma, o B volta")
PC_TR("Switch layout (right accepts, bottom goes back)",
      "Layout do Switch (direita confirma, baixo volta)")
PC_TR("The pad's four face buttons in menus, dialogues and the title screen. Same as the game: as Face buttons and the Pad buttons rows give them. Printed A accepts, B goes back: the button printed A (Cross) confirms and B (Circle) goes back, whatever jump and interact use in the world. Switch layout: the right button confirms, the bottom one goes back. Nothing changes in the world. A change applies at once.",
      "Os quatro botões frontais do controle em menus, diálogos e na tela de título. Como no jogo: como Botões frontais e as linhas de Botões do controle os definem. O A impresso confirma, o B volta: o botão com A (Xis) confirma e o com B (Círculo) volta, seja qual for o botão de pular e interagir no mundo. Layout do Switch: o botão da direita confirma, o de baixo volta. Nada muda no mundo. A mudança vale na hora.")
PC_TR("Switch (the game's)",
      "Switch (do jogo)")
PC_TR("Keyboard and mouse",
      "Teclado e mouse")
PC_TR("Left mouse shoots while aiming",
      "Botão esquerdo atira ao mirar")
PC_TR("With the bow drawn (a ZR key or button held), the left button shoots instead of attacking.",
      "Com o arco puxado (uma tecla ou botão ZR pressionado), o botão esquerdo atira em vez de atacar.")
PC_TR("Mouse look version",
      "Modo da câmera do mouse")
PC_TR("Stick",
      "Analógico")
PC_TR("Direct",
      "Direto")
PC_TR("Mouse sensitivity",
      "Sensibilidade do mouse")
PC_TR("Scales the camera and the motion aim.",
      "Ajusta a câmera e a mira por movimento.")
PC_TR("Mouse speed for a full camera turn",
      "Velocidade do mouse p/ giro total")
PC_TR("Mouse counts per second that turn the camera at full stick speed.",
      "Contagens do mouse por segundo que giram a câmera na velocidade máxima do analógico.")
PC_TR("Invert mouse vertical",
      "Inverter mouse na vertical")
PC_TR("Mouse as motion aim",
      "Mouse na mira por movimento")
PC_TR("While aiming, the mouse turns a virtual controller, so the game's motion aiming gets the mouse's precision (when no pad with motion is connected).",
      "Ao mirar, o mouse gira um controle virtual, e a mira por movimento do jogo ganha a precisão do mouse (quando nenhum controle com movimento está conectado).")
PC_TR("While aiming (bow, throw)",
      "Ao mirar (arco, arremesso)")
PC_TR("Always",
      "Sempre")
PC_TR("Motion aim speed",
      "Vel. da mira por movimento")
PC_TR("Aiming sensitivity (bow)",
      "Sensibilidade da mira (arco)")
PC_TR("With the bow drawn: the mouse's turn times this (1 = as when not aiming), with or without the mouse as motion aim. A pad's motion sensitivity is separate.",
      "Com o arco puxado: o giro do mouse multiplicado por isto (1 = como ao não mirar), com ou sem o mouse como mira por movimento. A sensibilidade de movimento do controle é separada.")
PC_TR("Mouse wheel scrolls the quick menus",
      "Roda do mouse nos menus rápidos")
PC_TR("While a D-pad quick menu is held open (weapons, shields, items, bows), each notch moves one entry.",
      "Com um menu rápido do direcional aberto (armas, escudos, itens, arcos), cada clique da roda move uma entrada.")
PC_TR("Map with the mouse",
      "Mapa com o mouse")
PC_TR("On the map the mouse moves the map under the reticle (it stays at the centre) as far as the pointer would move, the wheel zooms, a left click places a pin or stamp and a right click goes back.",
      "No mapa, o mouse move o mapa sob a mira (que fica no centro) tanto quanto o ponteiro se moveria, a roda dá zoom, clique esquerdo coloca um marcador ou carimbo e clique direito volta.")
PC_TR("Map cursor sensitivity",
      "Sensibilidade do cursor do mapa")
PC_TR("How far the map moves for the mouse's motion: 1 = as far as the desktop pointer would move on the screen, 2 = twice as far.",
      "Quanto o mapa se move com o movimento do mouse: 1 = tanto quanto o ponteiro da área de trabalho se moveria na tela, 2 = o dobro.")
PC_TR("How far the map moves under the reticle for the mouse's motion: 1 = like the desktop pointer, 2 = twice as far.",
      "Quanto o mapa se move sob a mira com o movimento do mouse: 1 = como o ponteiro da área de trabalho, 2 = o dobro.")
PC_TR("Menu keys",
      "Teclas de menu")
PC_TR("In the pause menu and the Purah Pad: Esc goes back, PageUp / PageDown (and the wheel in the pause menu) switch tabs, and in the pause menu clicks select (left) and go back (right).",
      "No menu de pausa e na Tabuleta da Purah: Esc volta, PageUp / PageDown (e a roda no menu de pausa) trocam de aba, e no menu de pausa os cliques selecionam (esquerdo) e voltam (direito).")
PC_TR("Rumble curve",
      "Curva da vibração")
PC_TR("How light rumbles feel next to strong ones: below 1 light rumbles come through stronger, above 1 only strong ones; 1 = the game's own.",
      "Como as vibrações leves se sentem perto das fortes: abaixo de 1 as leves chegam mais fortes, acima de 1 só as fortes; 1 = a do jogo.")
PC_TR("Trigger rumble (Xbox)",
      "Vibração nos gatilhos (Xbox)")
PC_TR("An Xbox One or Series pad's trigger motors buzz with the game's rumble, each trigger with its side's. Off by default: the Switch pad has no trigger motors.",
      "Os motores dos gatilhos de um controle Xbox One ou Series vibram com a vibração do jogo, cada gatilho com a do seu lado. Desligada por padrão: o controle do Switch não tem motores nos gatilhos.")
PC_TR("Light",
      "Leve")
PC_TR("Full strength",
      "Máxima")
PC_TR("Camera speed (right stick)",
      "Velocidade da câmera (analógico dir.)")
PC_TR("The camera's turn with the right stick (and the camera keys) times this, at each of the game's own camera speeds, top speed included; the mouse keeps its own sensitivity. At the next start.",
      "O giro da câmera com o analógico direito (e com as teclas de câmera) multiplicado por este valor, em cada uma das velocidades de câmera do próprio jogo, incluindo a máxima; o mouse mantém sua própria sensibilidade. Na próxima inicialização.")
PC_TR("Rumble strength",
      "Força da vibração")
PC_TR("The pad's motors (Xbox, PlayStation and Switch pads): 0 = off, 1 = the game's own.",
      "Os motores do controle (controles Xbox, PlayStation e Switch): 0 = desligado, 1 = o do próprio jogo.")
PC_TR("Motion (gyro)",
      "Giroscópio")
PC_TR("The motion sensors of a DualSense, DualShock 4, Pro Controller or Joy-Con, for the game's motion aiming (the game's own Motion Controls option turns it on there). Off: the pad lies still for the game.",
      "Os sensores de movimento de um DualSense, DualShock 4, Pro Controller ou Joy-Con, para a mira por movimento do jogo (a opção Controles de movimento do próprio jogo a ativa lá). Desligado: o controle fica parado para o jogo.")
PC_TR("Motion sensitivity",
      "Sensib. de movimento")
PC_TR("How far the aim turns for a turn of the pad: 1 = the pad's own, 2 = twice as far.",
      "Quanto a mira gira para um giro do controle: 1 = o do próprio controle, 2 = o dobro.")
PC_TR("Left stick dead zone",
      "Zona morta, analógico esq.")
PC_TR("How far the left stick must move before it counts, in percent of its travel. Raise it if Link moves with the stick at rest (a worn stick); lower it for finer small steps. 0 = the pad's own (Xbox 24, PlayStation 15, Switch 12).",
      "Quanto o analógico esquerdo precisa se mover para contar, em porcentagem do curso. Aumente se Link anda com o analógico parado (analógico gasto); diminua para passos pequenos mais precisos. 0 = o do próprio controle (Xbox 24, PlayStation 15, Switch 12).")
PC_TR("Right stick dead zone",
      "Zona morta, analógico dir.")
PC_TR("How far the right stick must move before it counts, in percent of its travel. Raise it if the camera turns with the stick at rest; lower it for finer aiming. 0 = the pad's own (Xbox 27, PlayStation 15, Switch 12).",
      "Quanto o analógico direito precisa se mover para contar, em porcentagem do curso. Aumente se a câmera gira com o analógico parado; diminua para mira mais precisa. 0 = o do próprio controle (Xbox 27, PlayStation 15, Switch 12).")
PC_TR("Walk speed",
      "Velocidade ao andar")
PC_TR("With the walk key held (Alt in the PC layout): how far the movement keys push the stick, in its travel (1 = a full push, as without the key).",
      "Com a tecla de andar pressionada (Alt no layout de PC): quanto as teclas de movimento empurram o analógico (1 = até o fim, como sem a tecla).")
PC_TR("None",
      "Nenhum")
PC_TR("Left stick click",
      "Clique analógico esq.")
PC_TR("Right stick click",
      "Clique analógico dir.")
PC_TR("D-pad up",
      "Direcional cima")
PC_TR("D-pad down",
      "Direcional baixo")
PC_TR("D-pad left",
      "Direcional esq.")
PC_TR("D-pad right",
      "Direcional dir.")
PC_TR("Back paddle P1 (right, top)",
      "Alavanca traseira P1 (dir., cima)")
PC_TR("The Switch button this back paddle presses: an Xbox Elite's paddles (unless the Xbox Accessories app maps them to buttons), a DualSense Edge's back buttons (the right one).",
      "O botão do Switch que esta alavanca traseira aperta: as alavancas do Xbox Elite (a menos que o app Acessórios Xbox as mapeie para botões), os botões traseiros do DualSense Edge (a direita).")
PC_TR("Back paddle P2 (right, bottom)",
      "Alavanca traseira P2 (dir., baixo)")
PC_TR("The Switch button this back paddle presses: an Xbox Elite's paddles (unless the Xbox Accessories app maps them to buttons), a DualSense Edge's back buttons (none: P1 and P3 only).",
      "O botão do Switch que esta alavanca traseira aperta: as alavancas do Xbox Elite (a menos que o app Acessórios Xbox as mapeie para botões), os botões traseiros do DualSense Edge (nenhum: só P1 e P3).")
PC_TR("Back paddle P3 (left, top)",
      "Alavanca traseira P3 (esq., cima)")
PC_TR("The Switch button this back paddle presses: an Xbox Elite's paddles (unless the Xbox Accessories app maps them to buttons), a DualSense Edge's back buttons (the left one).",
      "O botão do Switch que esta alavanca traseira aperta: as alavancas do Xbox Elite (a menos que o app Acessórios Xbox as mapeie para botões), os botões traseiros do DualSense Edge (a esquerda).")
PC_TR("Back paddle P4 (left, bottom)",
      "Alavanca traseira P4 (esq., baixo)")
PC_TR("Game",
      "Jogo")
PC_TR("Window",
      "Janela")
PC_TR("Pause when the window loses focus",
      "Pausar ao perder o foco da janela")
PC_TR("TOTK_PAUSE_UNFOCUSED: while the game window is not in front the game stops, picture, sound and its clock, and goes on where it was (hle_oe.cpp oe_host_paused).",
      "TOTK_PAUSE_UNFOCUSED: enquanto a janela do jogo não está em primeiro plano, o jogo para, imagem, som e relógio, e continua de onde estava (hle_oe.cpp oe_host_paused).")
PC_TR("Key bindings",
      "Atribuições")
PC_TR("Up to 4 keys or mouse buttons. Editing one switches the layout to Custom; empty = the PC layout's.",
      "Até 4 teclas ou botões do mouse. Editar uma muda o layout para Personalizado; vazio = o do layout de PC.")
PC_TR("A (interact)",
      "A (interagir)")
PC_TR("B (sprint / back)",
      "B (correr / voltar)")
PC_TR("X (jump)",
      "X (pular)")
PC_TR("Y (attack)",
      "Y (atacar)")
PC_TR("Left stick click (crouch)",
      "Clique analógico esq. (agachar)")
PC_TR("Right stick click (look)",
      "Clique analógico dir. (olhar)")
PC_TR("L (abilities)",
      "L (habilidades)")
PC_TR("R (throw)",
      "R (arremessar)")
PC_TR("ZL (target / shield)",
      "ZL (mirar / escudo)")
PC_TR("ZR (bow)",
      "ZR (arco)")
PC_TR("- (map)",
      "- (mapa)")
PC_TR("D-pad left (shields)",
      "Direcional esq. (escudos)")
PC_TR("D-pad up (items)",
      "Direcional cima (itens)")
PC_TR("D-pad right (weapons)",
      "Direcional dir. (armas)")
PC_TR("Move left",
      "Ir à esquerda")
PC_TR("Move forward",
      "Ir à frente")
PC_TR("Move right",
      "Ir à direita")
PC_TR("Move back",
      "Ir para trás")
PC_TR("Camera left",
      "Câmera: esquerda")
PC_TR("Camera up",
      "Câmera: cima")
PC_TR("Camera right",
      "Câmera: direita")
PC_TR("Camera down",
      "Câmera: baixo")
PC_TR("Walk (hold)",
      "Andar (segurar)")
PC_TR("Pad buttons",
      "Botões do controle")
PC_TR("Up to 4 pad buttons, by position: A = the right face button (Xbox B, PlayStation Circle), B = bottom (Xbox A, Cross), X = top (Xbox Y, Triangle), Y = left (Xbox X, Square). Empty = its own button; None = unbound.",
      "Até 4 botões do controle, por posição: A = o botão direito (Xbox B, PlayStation Círculo), B = embaixo (Xbox A, Xis), X = em cima (Xbox Y, Triângulo), Y = à esquerda (Xbox X, Quadrado). Vazio = o próprio botão; Nenhum = sem atribuição.")
PC_TR("Skipping to Great Sky Island in %.1f s... (release Minus to cancel)",
      "Indo para a Great Sky Island em %.1f s... (solte Menos para cancelar)")
PC_TR("Skip to Great Sky Island: F2 or hold Minus (restarts into a new game)",
      "Pular para a Great Sky Island: F2 ou segure Menos (reinicia em um novo jogo)")
PC_TR("Skip opening: ON   (F2 / Minus to change)",
      "Pular abertura: LIGADO   (F2 / Menos para mudar)")
PC_TR("Skip opening: OFF   (F2 / Minus to change)",
      "Pular abertura: DESLIGADO   (F2 / Menos para mudar)")
PC_TR("Graphics settings: F10  (pad: L3 + R3)",
      "Configurações gráficas: F10  (controle: L3 + R3)")
PC_TR("Shader warm-up: %d / %d (%d %%) - press A to play now, it keeps compiling in the background",
      "Pré-compilação de shaders: %d / %d (%d %%) - aperte A para jogar agora, a compilação continua em segundo plano")
PC_TR("Preparing the shaders for this PC (first start): %d %%",
      "Preparando os shaders para este PC (primeira inicialização): %d %%")
PC_TR("Compiling shaders %d %%",
      "Compilando shaders %d %%")
PC_TR("Loading shaders %d %%",
      "Carregando shaders %d %%")
PC_TR("(paused: low memory)",
      "(em pausa: pouca memória)")
PC_TR("Windows runs your monitor at %lu Hz: set it to %lu Hz in Settings > Display > Advanced display",
      "O Windows roda seu monitor a %lu Hz: ajuste para %lu Hz em Configurações > Vídeo > Configurações de vídeo avançadas")
PC_TR("It writes the NSO header.",
      "Grava o cabeçalho NSO.")
PC_TR("It writes past main's image (bss).",
      "Grava além da imagem do main (bss).")
PC_TR("It changes code the port already patches (%s).",
      "Altera um código que o port já corrige (%s).")
PC_TR("It overlaps %s, higher in the list.",
      "Sobrepõe %s, mais acima na lista.")
PC_TR("It is cut short at word %s.",
      "É cortado na palavra %s.")
PC_TR("It has an opcode the cheat engine does not know (%s).",
      "Tem um opcode que o motor de cheats não conhece (%s).")
PC_TR("It uses the alias or ASLR memory region, which the port does not have.",
      "Usa a região de memória alias ou ASLR, que o port não tem.")
PC_TR("It changes the game's code only under a condition (a button, a value), which the port cannot do yet.",
      "Altera o código do jogo só sob uma condição (um botão, um valor), o que o port ainda não consegue fazer.")
PC_TR("It cannot be read (%s).",
      "Não pode ser lido (%s).")
PC_TR("%d ExeFS patches in use",
      "%d patches de ExeFS em uso")
PC_TR("%d code patches need the game built on this PC (Builder install), not used",
      "%d patches de código exigem o jogo compilado neste PC (instalação Builder), não usados")
PC_TR("the build with its code patches failed (see exefs/%s/build.log next to totk.exe), not used",
      "a compilação com seus patches de código falhou (veja exefs/%s/build.log ao lado do totk.exe), não usado")
PC_TR("%d code patches: the game is rebuilt with them at this start",
      "%d patches de código: o jogo é recompilado com eles nesta inicialização")
PC_TR("%d ExeFS patches for another game version, not used",
      "%d patches de ExeFS para outra versão do jogo, não usados")
PC_TR("left out: %s",
      "deixado de fora: %s")
PC_TR("%d cheats (F10 Cheats tab)",
      "%d cheats (aba Cheats do F10)")
PC_TR("no cheat file for this game version (%s)",
      "nenhum arquivo de cheats para esta versão do jogo (%s)")
PC_TR("It changes the game's code: the game is rebuilt with it at the next start.",
      "Altera o código do jogo: o jogo é recompilado com ele na próxima inicialização.")
PC_TR("Not an amiibo dump: an amiibo image is 540 bytes.",
      "Não é um dump de amiibo: a imagem de um amiibo tem 540 bytes.")
PC_TR("Not an amiibo dump: the amiibo marker is missing.",
      "Não é um dump de amiibo: falta a marca do amiibo.")
PC_TR("Not an amiibo dump: unknown figure data.",
      "Não é um dump de amiibo: dados de figura desconhecidos.")
PC_TR("The file cannot be read.",
      "Não foi possível ler o arquivo.")
PC_TR("amiibo: pick one in F10 (or L3 + R3) > Amiibo",
      "amiibo: escolha um em F10 (ou L3 + R3) > Amiibo")
PC_TR("Your own amiibo dumps (.bin, 540 bytes, or Flipper .nfc) in the amiibo folder. In the game, open its amiibo screen; when it waits for a figure, press Scan on one here.",
      "Seus próprios dumps de amiibo (.bin, 540 bytes, ou .nfc do Flipper) na pasta amiibo. No jogo, abra a tela de amiibo; quando ele esperar uma figura, aperte Escanear em uma daqui.")
PC_TR("Open amiibo folder",
      "Abrir pasta amiibo")
PC_TR("The game is waiting for an amiibo.",
      "O jogo está esperando um amiibo.")
PC_TR("On the reader: %s",
      "No leitor: %s")
PC_TR("Open the game's amiibo screen first: Scan works while it waits for a figure.",
      "Abra primeiro a tela de amiibo do jogo: Escanear funciona enquanto ele espera uma figura.")
PC_TR("No dumps yet: put .bin or .nfc files in the amiibo folder.",
      "Nenhum dump ainda: coloque arquivos .bin ou .nfc na pasta amiibo.")
PC_TR("Scan",
      "Escanear")
PC_TR("Movement",
      "Movimento")
PC_TR("Jump",
      "Pular")
PC_TR("Sprint",
      "Correr")
PC_TR("Crouch",
      "Agachar")
PC_TR("Let go (a wall, a ride)",
      "Soltar (parede, montaria)")
PC_TR("Dive",
      "Mergulhar")
PC_TR("Close the paraglider",
      "Fechar a asa-delta")
PC_TR("Swim dash",
      "Impulso na água")
PC_TR("Combat",
      "Combate")
PC_TR("Attack / draw the bow",
      "Atacar / puxar o arco")
PC_TR("Lower the bow (cancel)",
      "Baixar o arco (cancelar)")
PC_TR("Next arrow material",
      "Próximo material de flecha")
PC_TR("Previous arrow material",
      "Material de flecha anterior")
PC_TR("Shield and lock on",
      "Escudo e fixar alvo")
PC_TR("Put away / take out the weapon",
      "Guardar / sacar a arma")
PC_TR("Take out / put away the bow",
      "Sacar / guardar o arco")
PC_TR("Throw",
      "Arremessar")
PC_TR("Next weapon",
      "Próxima arma")
PC_TR("Previous weapon",
      "Arma anterior")
PC_TR("Construct: right arm",
      "Construto: braço direito")
PC_TR("Construct: left arm",
      "Construto: braço esquerdo")
PC_TR("Construct: back part",
      "Construto: parte traseira")
PC_TR("Construct: guard",
      "Construto: proteção")
PC_TR("Construct: attach a device",
      "Construto: acoplar um dispositivo")
PC_TR("Construct: get off",
      "Construto: descer")
PC_TR("Items and tools",
      "Itens e ferramentas")
PC_TR("Interact",
      "Interagir")
PC_TR("Melee weapons",
      "Armas brancas")
PC_TR("Shields",
      "Escudos")
PC_TR("Bows",
      "Arcos")
PC_TR("Materials",
      "Materiais")
PC_TR("Scope",
      "Luneta")
PC_TR("Whistle",
      "Assobio")
PC_TR("Inventory",
      "Inventário")
PC_TR("Map",
      "Mapa")
PC_TR("Adventure log",
      "Diário de aventuras")
PC_TR("Pause menu",
      "Menu de pausa")
PC_TR("Abilities",
      "Habilidades")
PC_TR("Purah Pad camera",
      "Câmera da Tabuleta da Purah")
PC_TR("Ability wheel",
      "Roda de habilidades")
PC_TR("Use the ability",
      "Usar a habilidade")
PC_TR("Secondary action",
      "Ação secundária")
PC_TR("Pull closer",
      "Puxar para perto")
PC_TR("Push away",
      "Empurrar")
PC_TR("Rotate",
      "Girar")
PC_TR("Reset rotation",
      "Resetar rotação")
PC_TR("Raise",
      "Subir")
PC_TR("Lower",
      "Descer")
PC_TR("Unstick",
      "Desprender")
PC_TR("Leave the ability",
      "Sair da habilidade")
PC_TR("Leave (camera, Scope, quick menu)",
      "Sair (câmera, Luneta, menu rápido)")
PC_TR("Take a photo",
      "Tirar uma foto")
PC_TR("Self-portrait",
      "Selfie")
PC_TR("Camera: zoom in",
      "Câmera: aproximar")
PC_TR("Camera: zoom out",
      "Câmera: afastar")
PC_TR("Place a pin (Scope)",
      "Colocar marcador (Luneta)")
PC_TR("Horse",
      "Cavalo")
PC_TR("Spur",
      "Esporear")
PC_TR("Dismount",
      "Desmontar")
PC_TR("Soothe",
      "Acalmar")
PC_TR("Reset the camera",
      "Resetar a câmera")
PC_TR("Confirm",
      "Confirmar")
PC_TR("Back",
      "Voltar")
PC_TR("Previous tab",
      "Aba anterior")
PC_TR("Next tab",
      "Próxima aba")
PC_TR("Rotate left",
      "Girar à esquerda")
PC_TR("Rotate right",
      "Girar à direita")
PC_TR("Selection up",
      "Seleção para cima")
PC_TR("Selection down",
      "Seleção para baixo")
PC_TR("Selection left",
      "Seleção à esquerda")
PC_TR("Selection right",
      "Seleção à direita")
PC_TR("D-pad up (selection)",
      "Direcional cima (seleção)")
PC_TR("D-pad down (selection)",
      "Direcional baixo (seleção)")
PC_TR("D-pad left (selection, value)",
      "Direcional esq. (seleção, valor)")
PC_TR("D-pad right (selection, value)",
      "Direcional dir. (seleção, valor)")
PC_TR("Scroll up",
      "Rolar para cima")
PC_TR("Scroll down",
      "Rolar para baixo")
PC_TR("Item action (X prompt)",
      "Ação do item (aviso X)")
PC_TR("Menu: ZL (previous page)",
      "Menu: ZL (página anterior)")
PC_TR("Menu: ZR (next page)",
      "Menu: ZR (próxima página)")
PC_TR("Menu: left stick click",
      "Menu: clique analógico esq.")
PC_TR("Menu: right stick click",
      "Menu: clique analógico dir.")
PC_TR("Item action (Y prompt)",
      "Ação do item (aviso Y)")
PC_TR("Recipe book: open (Food tab)",
      "Livro de receitas: abrir (aba Comida)")
PC_TR("Recipe book: close",
      "Livro de receitas: fechar")
PC_TR("Recipe book: cooking history",
      "Receitas: histórico")
PC_TR("Quick menu: choose",
      "Menu rápido: escolher")
PC_TR("Quick menu: sort",
      "Menu rápido: ordenar")
PC_TR("Quick menu: D-pad up",
      "Menu rápido: direcional cima")
PC_TR("Quick menu: D-pad down",
      "Menu rápido: direcional baixo")
PC_TR("Quick menu: D-pad left",
      "Menu rápido: direcional esq.")
PC_TR("Quick menu: D-pad right",
      "Menu rápido: direcional dir.")
PC_TR("Quick menu: drop the item",
      "Menu rápido: largar o item")
PC_TR("Advance dialogue",
      "Avançar diálogo")
PC_TR("Conversation log",
      "Registro de conversas")
PC_TR("Pick the answer",
      "Escolher a resposta")
PC_TR("Answer 1",
      "Resposta 1")
PC_TR("Answer 2",
      "Resposta 2")
PC_TR("Answer 3",
      "Resposta 3")
PC_TR("Answer 4",
      "Resposta 4")
PC_TR("Advance (events)",
      "Avançar (eventos)")
PC_TR("Skip",
      "Pular")
PC_TR("Skip (short scenes)",
      "Pular (cenas curtas)")
PC_TR("Rebinding is not available yet",
      "A reatribuição ainda não está disponível")
PC_TR("No such action here",
      "Não existe essa ação aqui")
PC_TR("A fixed control has that key",
      "Um controle fixo usa essa tecla")
PC_TR("Reserved key (F10, F12, Windows, Print Screen)",
      "Tecla reservada (F10, F12, Windows, Print Screen)")
PC_TR("Confirm, back and pause must keep a key",
      "Confirmar, voltar e pausa precisam manter uma tecla")
PC_TR("That swap would clash again: choose another key",
      "Essa troca geraria um conflito de novo: escolha outra tecla")
PC_TR("The wheel only works for single presses",
      "A roda só funciona para toques simples")
PC_TR("Refused",
      "Recusado")
PC_TR("Swapped with ",
      "Trocada com ")
PC_TR("Stored now; acts once the PC gameplay keys are on (Controls tab).",
      "Salvo; só vale quando as teclas de jogo de PC estiverem ativas (aba Controles).")
PC_TR("Press a key, mouse button or wheel (Esc: cancel)",
      "Aperte uma tecla, botão do mouse ou a roda (Esc: cancelar)")
PC_TR("Change",
      "Alterar")
PC_TR("Add",
      "Adicionar")
PC_TR("Default",
      "Padrão")
PC_TR("Key moved by",
      "Tecla movida por")
PC_TR("changed",
      "alterado")
PC_TR("in use by",
      "em uso por")
PC_TR("settings.ini gives this key to both; it works for the other one. Change one of them.",
      "O settings.ini dá essa tecla aos dois; ela funciona para o outro. Altere um deles.")
PC_TR("Key rebinding is not available in this build yet.",
      "A reatribuição de teclas ainda não está disponível nesta versão.")
PC_TR("Restore defaults",
      "Restaurar padrões")
PC_TR("Cheats from your mods' cheats folders (Atmosphere format). They switch at once; one that changes the game's code once the game is built with it (at the start after its mod is turned on or changed). Cheats can break a save: keep a copy.",
      "Cheats das pastas de cheats dos seus mods (formato Atmosphere). Ativam na hora; os que alteram o código do jogo só valem quando o jogo é recompilado com eles (na inicialização após ativar ou alterar o mod). Cheats podem corromper um save: guarde uma cópia.")
PC_TR("%s, %d x %d at %d Hz",
      "%s, %d x %d a %d Hz")
PC_TR(" (it can do %d Hz: Windows display settings)",
      " (suporta %d Hz: configurações de tela do Windows)")
PC_TR(" (native %d x %d)",
      " (nativa %d x %d)")
PC_TR(". HDR on in Windows",
      ". HDR ligado no Windows")
PC_TR(". HDR off in Windows (the monitor can)",
      ". HDR desligado no Windows (o monitor suporta)")
PC_TR(". No HDR",
      ". Sem HDR")
PC_TR(". VRR on (%s",
      ". VRR ligado (%s")
PC_TR(". VRR off in the driver (%s monitor",
      ". VRR desligado no driver (monitor %s")
PC_TR(". VRR: a %s monitor, not confirmed by the driver (Options: VRR on if it is on there",
      ". VRR: monitor %s, não confirmado pelo driver (Opções: VRR ligado se estiver ligado lá")
PC_TR(". No VRR",
      ". Sem VRR")
PC_TR("Graphics",
      "Gráficos")
PC_TR("Rendering",
      "Renderização")
PC_TR("Frame generation",
      "Geração de quadros")
PC_TR("Ultrahand rotation speed",
      "Velocidade de rotação da Ultramão")
PC_TR("TOTK_PC_ROTATE_SPEED: with R held in Ultrahand or Autobuild, the mouse moved sideways turns the object in 45-degree steps (1x: the move that turns the camera 45 degrees) and the wheel turns it vertically, a step a notch.",
      "TOTK_PC_ROTATE_SPEED: com R segurado na Ultramão ou na Autoconstrução, mover o mouse para os lados gira o objeto em passos de 45 graus (1x: o movimento que gira a câmera 45 graus) e a roda o gira na vertical, um passo por clique da roda.")
PC_TR("0.5x (slow)",
      "0.5x (lento)")
PC_TR("2x (fast)",
      "2x (rápido)")
PC_TR("Automatic (a dedicated GPU first)",
      "Automático (GPU dedicada primeiro)")
PC_TR("Display",
      "Tela")
PC_TR("Graphics card",
      "Placa de vídeo")
PC_TR("The graphics card the game runs on, for a laptop with two. Automatic picks the dedicated one, the faster; the integrated one uses less power but is much slower.",
      "A placa de vídeo em que o jogo roda, para notebooks com duas. Automático escolhe a dedicada, a mais rápida; a integrada gasta menos energia, mas é bem mais lenta.")
PC_TR("Sets every quality row below at once. Ultra is the game at its best on an 8 GB card; Low and Medium draw less than the Switch does. Changing a row by hand makes it Custom.",
      "Define de uma vez todas as opções de qualidade abaixo. Ultra é o jogo no melhor nível numa placa de 8 GB; Baixo e Médio desenham menos que o Switch. Mudar uma opção à mão passa para Personalizado.")
PC_TR("Windowed, borderless fullscreen (the default: Alt+Tab is instant) or exclusive fullscreen (the monitor switches to the resolution and refresh rate below). F11 or Alt+Enter also switch it.",
      "Em janela, tela cheia sem bordas (o padrão: Alt+Tab é instantâneo) ou tela cheia exclusiva (o monitor muda para a resolução e a taxa de atualização abaixo). F11 ou Alt+Enter também alternam.")
PC_TR("The window's size in windowed mode. Fullscreen uses the monitor's resolution.",
      "O tamanho da janela no modo em janela. A tela cheia usa a resolução do monitor.")
PC_TR("The monitor fullscreen uses and a windowed start opens on. Remember: the one the game was on last.",
      "O monitor que a tela cheia usa e em que o início em janela abre. Lembrar: o último em que o jogo esteve.")
PC_TR("The resolution the monitor switches to in exclusive fullscreen. Borderless and windowed keep the desktop's.",
      "A resolução para a qual o monitor muda na tela cheia exclusiva. Sem bordas e em janela mantêm a da área de trabalho.")
PC_TR("The refresh rate in exclusive fullscreen. Higher is smoother when the frame rate keeps up.",
      "A taxa de atualização na tela cheia exclusiva. Maior é mais suave quando a taxa de quadros acompanha.")
PC_TR("The most frames per second the game shows. Higher is smoother and costs more GPU and CPU; 30 is the Switch's. Unlocked follows the monitor's refresh rate (240 at most); steady holds a rate the scene can keep. For an even picture pick a cap that divides the refresh rate (30 or 60 on 60 Hz, 40 on 120 Hz), or turn VRR on. Steam Deck: 60 (OLED: 90) at first; if it is not held, lower it to 40 with the screen at 40 Hz (Quick Access > Performance).",
      "O máximo de quadros por segundo que o jogo mostra. Maior é mais suave e custa mais GPU e CPU; 30 é o do Switch. Livre segue a taxa de atualização do monitor (240 no máximo); estável mantém uma taxa que a cena sustenta. Para uma imagem uniforme, escolha um limite que divida a taxa de atualização (30 ou 60 em 60 Hz, 40 em 120 Hz) ou ligue o VRR. Steam Deck: 60 (OLED: 90) de início; se não se mantiver, baixe para 40 com a tela a 40 Hz (Acesso rápido > Desempenho).")
PC_TR("Frame rate \"Unlocked, steady\" only: the lowest rate it holds. 30 (default) suits most PCs. On one that cannot hold 30, 20 or 15 show every frame for the same time (3 or 4 refreshes on 60 Hz) instead of an uneven 15-20.",
      "Só com a taxa de quadros “Livre, estável”: a menor taxa que ela mantém. 30 (padrão) serve para a maioria dos PCs. Em um que não segura 30, 20 ou 15 mostram cada quadro pelo mesmo tempo (3 ou 4 atualizações em 60 Hz) em vez de 15-20 irregulares.")
PC_TR("%d fps on this %d Hz monitor shows frames unevenly (one refresh, then two). Even: %s, steady, VRR on, or the monitor at %d Hz or a multiple (Steam Deck: Quick Access > Performance).",
      "%d fps neste monitor de %d Hz mostram os quadros de forma irregular (uma atualização, depois duas). Uniforme: %s, estável, VRR ligado, ou o monitor a %d Hz ou um múltiplo (Steam Deck: Acesso rápido > Desempenho).")
PC_TR("uneven",
      "irregular")
PC_TR("On (default): no tearing. Off: slightly less input latency, with tearing lines.",
      "Ligado (padrão): sem tearing. Desligado: um pouco menos de latência de entrada, com linhas de tearing.")
PC_TR("Variable refresh rate (G-SYNC, FreeSync): the monitor waits for each frame, smooth below its refresh rate and without tearing. Needs a VRR monitor with VRR on in Windows or the driver.",
      "Taxa de atualização variável (G-SYNC, FreeSync): o monitor espera cada quadro, suave abaixo da sua taxa de atualização e sem tearing. Exige monitor VRR com VRR ligado no Windows ou no driver.")
PC_TR("Shows fps, the 1% low and a frame-time graph over the game (F9), to check performance.",
      "Mostra fps, o 1% baixo e um gráfico de tempo de quadro sobre o jogo (F9), para checar o desempenho.")
PC_TR("A small frame-rate counter in a corner (F3 shows or hides it). Minimal: fps; Detailed adds the frame time, the 1% low and the worst frame. Off costs nothing.",
      "Um pequeno contador de quadros num canto (F3 mostra ou oculta). Mínimo: fps; Detalhado acrescenta o tempo de quadro, o 1% mais baixo e o pior quadro. Desligado não custa nada.")
PC_TR("The counter's corner. Top right keeps clear of the hearts and the map.",
      "O canto do contador. Superior direito não cobre os corações nem o mapa.")
PC_TR("The counter's size. It also grows with the window's height.",
      "O tamanho do contador. Também cresce com a altura da janela.")
PC_TR("Colour adjustment of the game's picture (not these menus). Off shows the game's own colours and costs nothing; Vivid is livelier; Brighter dark tones lightens caves, the Depths and nights; Custom uses the four rows below.",
      "Ajuste de cor da imagem do jogo (não destes menus). Desligado mostra as cores do próprio jogo e não custa nada; Vívido é mais vivo; Tons escuros mais claros clareia cavernas, as Depths e noites; Personalizado usa as 4 opções abaixo.")
PC_TR("Custom only: makes every colour brighter or darker; 0 is the game's.",
      "Só em Personalizado: deixa todas as cores mais claras ou escuras; 0 é o do jogo.")
PC_TR("Custom only: more or less difference between light and dark; 1 is the game's.",
      "Só em Personalizado: mais ou menos diferença entre claro e escuro; 1 é o do jogo.")
PC_TR("Custom only: how colourful the picture is; 1 is the game's, 0.5 greyer, 2 twice as colourful.",
      "Só em Personalizado: quão colorida é a imagem; 1 é o do jogo, 0,5 mais cinza, 2 o dobro de cor.")
PC_TR("Custom only: below 1 brightens the dark tones, above 1 darkens them; 1 is the game's.",
      "Só em Personalizado: abaixo de 1 clareia os tons escuros, acima de 1 os escurece; 1 é o do jogo.")
PC_TR("The resolution the 3D scene is drawn at, against the game's own. Higher is sharper and costs much more GPU; lower is faster and blurrier. The default: the window's resolution.",
      "A resolução em que a cena 3D é desenhada, em relação à do jogo. Maior é mais nítido e custa bem mais GPU; menor é mais rápido e mais borrado. Padrão: a resolução da janela.")
PC_TR("Lowers the 3D resolution only when the GPU falls behind, never below the floor chosen, to hold the frame rate. Off keeps the picture sharp all the time; On is smoother on a slower GPU.",
      "Reduz a resolução 3D só quando a GPU não acompanha, nunca abaixo do mínimo escolhido, para manter a taxa de quadros. Desligado mantém a imagem nítida o tempo todo; Ligado é mais suave numa GPU mais lenta.")
PC_TR("Above 1080p, menus, text and the HUD are drawn at the window's resolution instead of the game's 1080p stretched, and the 3D scene (render scale, DLSS / DLAA) reaches the window's resolution too. Sharper picture for a little GPU.",
      "Acima de 1080p, menus, textos e HUD são desenhados na resolução da janela em vez dos 1080p do jogo esticados, e a cena 3D (resolução de renderização, DLSS / DLAA) também chega à resolução da janela. Imagem mais nítida por um pouco de GPU.")
PC_TR("Generated frames between the real ones make motion look smoother, but the game itself does not run faster and input latency rises a little. DLSS x2: RTX 40 and newer, x3 and x4: RTX 50; FSR x2: any GPU. Best when the game already runs well.",
      "Os quadros gerados entre os reais deixam o movimento mais suave, mas o jogo em si não fica mais rápido e a latência de entrada sobe um pouco. DLSS x2: RTX 40 ou mais nova, x3 e x4: RTX 50; FSR x2: qualquer GPU. Melhor quando o jogo já roda bem.")
PC_TR("How the 3D scene is scaled up to the output: Bicubic (the port's) or FSR 1 (the game's own). Matters only below 100 % render resolution; either costs little.",
      "Como a cena 3D é ampliada até a saída: Bicúbico (o do port) ou FSR 1 (o do próprio jogo). Só importa abaixo de 100 % de resolução de renderização; ambos custam pouco.")
PC_TR("The filter that scales the 3D scene when the render resolution is not 100 %: Catmull-Rom, or FSR 1 EASU (sharper edges). Either costs little.",
      "O filtro que amplia a cena 3D quando a resolução de renderização não é 100 %: Catmull-Rom ou FSR 1 EASU (bordas mais nítidas). Ambos custam pouco.")
PC_TR("How the finished picture is scaled to the window: Bilinear, or FSR 1 for a window above 1080p (sharper). Costs little.",
      "Como a imagem final é escalada para a janela: Bilinear ou FSR 1 para janela acima de 1080p (mais nítido). Custa pouco.")
PC_TR("Smooths jagged edges on the final picture. SMAA 1x: smoother edges for a little GPU; Off is the game's own look.",
      "Suaviza as bordas serrilhadas da imagem final. SMAA 1x: bordas mais suaves por um pouco de GPU; Desligado é o visual do próprio jogo.")
PC_TR("Sharpens the final picture (AMD RCAS). More is crisper but can look grainy; Off is the game's look. Costs very little.",
      "Aumenta a nitidez da imagem final (AMD RCAS). Mais deixa mais definido, mas pode ficar granulado; Desligado é o visual do jogo. Custa muito pouco.")
PC_TR("Keeps ground textures sharp when seen at a shallow angle. Higher is sharper for a little GPU; 16x suits most GPUs. The game's: its own setting.",
      "Mantém nítidas as texturas do chão vistas em ângulo raso. Maior é mais nítido por um pouco de GPU; 16x serve para a maioria das GPUs. A do jogo: sua própria configuração.")
PC_TR("Which texture detail level is used; 0 is the game's. Below 0 is sharper but can shimmer; above 0 is blurrier and can break textures (red grass). Not a quality slider: leave it at 0.",
      "Qual nível de detalhe de textura é usado; 0 é o do jogo. Abaixo de 0 é mais nítido, mas pode cintilar; acima de 0 é mais borrado e pode quebrar texturas (grama vermelha). Não é um controle de qualidade: deixe em 0.")
PC_TR("The resolution of the renderer's shadow maps: 1x is the Switch's, 2x (default) sharper, 3x and 4x sharper still for more GPU time and memory. With little video memory they are lowered to what fits (4 GB cards: 2x).",
      "A resolução dos mapas de sombras do renderer: 1x é a do Switch, 2x (padrão) mais nítida, 3x e 4x ainda mais nítidas, por mais tempo de GPU e memória. Com pouca memória de vídeo, são reduzidos ao que cabe (placas de 4 GB: 2x).")
PC_TR("Sun shadows grow softer the farther they fall from what casts them, as in real light. Off is the game's look; on costs some GPU. A change rebuilds the shadow shaders in the background: the old shadows show until then.",
      "As sombras do sol ficam mais suaves quanto mais longe caem do que as projeta, como na luz real. Desligado é o visual do jogo; ligado custa um pouco de GPU. Mudar recria os shaders de sombra em segundo plano: as sombras antigas aparecem até lá.")
PC_TR("Adds the small sun shadows the shadow maps miss: at feet, under stones, in grass. Off is the game's look; Subtle to Strong cost some GPU.",
      "Acrescenta as pequenas sombras do sol que os mapas de sombras perdem: nos pés, sob pedras, na grama. Desligado é o visual do jogo; de Sutis a Fortes custam um pouco de GPU.")
PC_TR("Prepares the shaders shipped with this version before you meet them, so new places stutter less the first time. Off: only the shaders this PC has built itself. On is better.",
      "Prepara os shaders que vêm com esta versão antes de você chegar neles, para que lugares novos engasguem menos na primeira vez. Desligado: só os shaders que este PC compilou. Ligado é melhor.")
PC_TR("A shader still being built: Limit to 0.1 s (default) shows the frame after 0.1 s and the missing objects appear a moment later; Always wait never shows anything incomplete but can freeze the picture for a second or more.",
      "Com um shader ainda em compilação: Limitar a 0,1 s (padrão) mostra o quadro após 0,1 s e os objetos faltantes aparecem pouco depois; Sempre esperar nunca mostra nada incompleto, mas pode travar a imagem por um segundo ou mais.")
PC_TR("Where the frame cap waits: at the present (default, steadier frame times) or before the frame (slightly less input latency).",
      "Onde o limite de quadros espera: na apresentação (padrão, tempos de quadro mais estáveis) ou antes do quadro (um pouco menos de latência de entrada).")
PC_TR("What the game's 60 Hz tick follows: a 60 Hz clock (default, right for any monitor) or the monitor's vblank (a fixed-refresh monitor only). Leave the default.",
      "O que o tick de 60 Hz do jogo segue: um relógio de 60 Hz (padrão, certo para qualquer monitor) ou o vblank do monitor (só para monitor de taxa fixa). Deixe o padrão.")
PC_TR("The last part of the frame cap's wait busy-waits for exact timing: steadier frame times for some CPU. Off is the default.",
      "A última parte da espera do limite de quadros usa espera ativa para um tempo exato: tempos de quadro mais estáveis por um pouco de CPU. Desligado é o padrão.")
PC_TR("How many finished frames may wait to be shown. 1 lowers input latency, 3 evens out uneven frames; 2 is the default.",
      "Quantos quadros prontos podem esperar para serem exibidos. 1 reduz a latência de entrada, 3 equilibra quadros irregulares; 2 é o padrão.")
PC_TR("How many frames the CPU may prepare ahead of the GPU. 2 (default) is the balance; 1 lowers latency but is slower when both are busy; 3 adds latency.",
      "Quantos quadros a CPU pode preparar à frente da GPU. 2 (padrão) é o equilíbrio; 1 reduz a latência, mas é mais lento com ambas ocupadas; 3 aumenta a latência.")
PC_TR("Holds each frame back just before it reads your input, so it waits less afterwards: lower input latency. NVIDIA uses Reflex; Boost also keeps the GPU clocks up (more power).",
      "Segura cada quadro logo antes de ele ler seu comando, para esperar menos depois: menor latência de entrada. A NVIDIA usa Reflex; Boost também mantém os clocks da GPU altos (mais energia).")
PC_TR("HDR output for an HDR monitor (HDR10 or scRGB). Needs HDR on for that monitor in Windows; otherwise the picture stays SDR.",
      "Saída HDR para um monitor HDR (HDR10 ou scRGB). Exige HDR ligado para esse monitor no Windows; senão a imagem continua SDR.")
PC_TR("Your monitor's peak brightness in nits, so highlights are neither clipped nor dim. Use the value the monitor states.",
      "O brilho máximo do seu monitor em nits, para que os realces não estourem nem fiquem fracos. Use o valor informado pelo monitor.")
PC_TR("How bright ordinary white (menus, the sky) is in HDR. Raise it if the picture looks dim, lower it if it is too bright.",
      "Quão brilhante é o branco comum (menus, o céu) em HDR. Aumente se a imagem parecer escura, diminua se estiver clara demais.")
PC_TR("How much of their real colour bright lights keep in HDR: a torch flame or lava stays orange instead of turning yellow-white. 0 (the default) shows the picture as in SDR, only brighter; the HUD is left as it is.",
      "Quanto da sua cor real as luzes fortes mantêm em HDR: a chama de uma tocha ou a lava continuam laranja em vez de ficarem branco-amareladas. Com 0 (o padrão) a imagem fica como em SDR, só que mais clara; o HUD fica como está.")
PC_TR("The window's (the default): on a 3:2, 16:10, 21:9, 32:9 or 48:9 window or monitor the picture takes that shape, 21:9, 32:9 and 48:9 with a wider view; other shapes, 16:9 among them, keep the game's own. 16:9 / 16:10 / 21:9 / 32:9 / 48:9 / 3:2: that shape whatever the window. A new shape's layouts are made at its first start (about 30 s, once). A wider picture costs more GPU.",
      "A da janela (padrão): numa janela ou monitor 3:2, 16:10, 21:9, 32:9 ou 48:9 a imagem assume esse formato, 21:9, 32:9 e 48:9 com visão mais ampla; os outros formatos, 16:9 entre eles, mantêm a do próprio jogo. 16:9 / 16:10 / 21:9 / 32:9 / 48:9 / 3:2: esse formato seja qual for a janela. A interface de um formato novo é preparada no primeiro início (uns 30 s, uma vez). Uma imagem mais larga exige mais da GPU.")
PC_TR("On 21:9, 32:9 or 48:9: the HUD at the screen's edges, or in the middle 16:9 (easier to see on a very wide screen). Nothing changes at 16:9. Automatic (the default): the middle on 48:9, the edges otherwise.",
      "Em 21:9, 32:9 ou 48:9: o HUD nas bordas da tela ou no meio em 16:9 (mais fácil de ver numa tela muito larga). Nada muda em 16:9. Automático (padrão): o centro em 48:9 e as bordas nos outros.")
PC_TR("On 21:9, 32:9 or 48:9: real-time cutscenes wide like the rest of the game, or the original 16:9 with bars at the sides.",
      "Em 21:9, 32:9 ou 48:9: cutscenes em tempo real largas como o resto do jogo, ou o 16:9 original com barras nas laterais.")
PC_TR("Widens or narrows the camera's view (1 = the game's). Wider shows more of the world and costs some GPU and CPU.",
      "Amplia ou estreita a visão da câmera (1 = a do jogo). Mais largo mostra mais do mundo e custa um pouco de GPU e CPU.")
PC_TR("More memory for textures: fewer blurry textures when you turn around, for about 490 MiB of video memory per step.",
      "Mais memória para texturas: menos texturas borradas ao se virar, por cerca de 490 MiB de memória de vídeo por nível.")
PC_TR("More grass, flowers and pebbles. Higher looks fuller and costs GPU and CPU.",
      "Mais grama, flores e pedrinhas. Maior fica mais cheio e custa GPU e CPU.")
PC_TR("Sharper shadows: 2 doubles the shadow map's width and height, for 4x its memory and more GPU.",
      "Sombras mais nítidas: 2 dobra a largura e a altura do mapa de sombras, por 4x a memória e mais GPU.")
PC_TR("Moves the lines where the sun's sharp near shadows hand over to coarser ones further away (1 = the game's, 2 = twice as far). Near shadows get a little softer: raise Shadow resolution with it. Costs a little GPU and CPU. With Shadow distance above 1 the two multiplied stay at 4 at most, so at Shadow distance 4 this does nothing.",
      "Afasta as linhas onde as sombras nítidas próximas do sol passam para outras mais grosseiras (1 = o do jogo, 2 = o dobro da distância). As sombras próximas ficam um pouco mais suaves: aumente a Resolução das sombras junto. Custa um pouco de GPU e CPU. Com Distância das sombras acima de 1, as duas multiplicadas ficam em no máximo 4, então com Distância das sombras 4 isto não faz nada.")
PC_TR("Fixed keeps the sun's shadow map at its largest size: steadier, sharper leaf shadows for some GPU. The game's changes its size with the view.",
      "Fixo mantém o mapa de sombras do sol no maior tamanho: sombras de folhas mais estáveis e nítidas por um pouco de GPU. O do jogo muda o tamanho conforme a visão.")
PC_TR("How far the sharp shadows reach. Higher looks better in the distance and costs GPU.",
      "Até onde chegam as sombras nítidas. Maior fica melhor à distância e custa GPU.")
PC_TR("Pushes the distance fog back: clearer far mountains. Higher shows more and costs some GPU.",
      "Empurra a névoa de distância para longe: montanhas distantes mais claras. Maior mostra mais e custa um pouco de GPU.")
PC_TR("How far models keep their full detail. Higher looks better in the distance and costs GPU and CPU.",
      "Até onde os modelos mantêm o detalhe total. Maior fica melhor à distância e custa GPU e CPU.")
PC_TR("How far characters, enemies and objects keep full animation. Higher is smoother in the distance and costs CPU.",
      "Até onde personagens, inimigos e objetos mantêm a animação total. Maior é mais suave à distância e custa CPU.")
PC_TR("How far the ground and water keep their full detail. Higher looks better and costs CPU and GPU.",
      "Até onde o chão e a água mantêm o detalhe total. Maior fica melhor e custa CPU e GPU.")
PC_TR("How far from the camera characters, enemies and physics keep being simulated (1 = the game's). Higher keeps distant things alive and costs CPU.",
      "Até que distância da câmera personagens, inimigos e a física continuam simulados (1 = o do jogo). Maior mantém coisas distantes vivas e custa CPU.")
PC_TR("Fast: Link's departure when travelling is about 2.5 s shorter. Normal is the game's own.",
      "Rápida: a partida de Link nas viagens fica cerca de 2,5 s mais curta. Normal é a do próprio jogo.")
PC_TR("The game's overall volume, on top of the Windows volume.",
      "O volume geral do jogo, somado ao volume do Windows.")
PC_TR("The music's volume.",
      "O volume da música.")
PC_TR("The sound effects' volume.",
      "O volume dos efeitos sonoros.")
PC_TR("The voices' volume. No part of the game's mix carries voices alone yet, so this does nothing for now.",
      "O volume das vozes. Nenhuma parte da mixagem do jogo leva só as vozes ainda, então isto não faz nada por ora.")
PC_TR("The speakers the game mixes for: stereo, or 5.1 / 7.1 surround (experimental). Pick what your speakers or headphones use.",
      "Os alto-falantes para os quais o jogo mixa: estéreo, ou surround 5.1 / 7.1 (experimental). Escolha o que seus alto-falantes ou fones usam.")
PC_TR("How much sound is queued ahead. Lower responds sooner; raise it if the sound crackles.",
      "Quanto som fica enfileirado à frente. Menor responde mais rápido; aumente se o som estiver estalando.")
PC_TR("The sound moves to headphones or speakers as you plug them in or out, without a restart.",
      "O som passa para fones ou alto-falantes ao conectá-los ou desconectá-los, sem reiniciar.")
PC_TR("No sound while the game window is not in front; the game keeps running.",
      "Sem som enquanto a janela do jogo não está em primeiro plano; o jogo continua rodando.")
PC_TR("PC: keyboard and mouse controls in the menus (Q / E tabs, Esc back, the mouse pointer) and PC keys in the prompts. Console: the Switch layout on the keyboard everywhere.",
      "PC: controles de teclado e mouse nos menus (abas Q / E, Esc volta, ponteiro do mouse) e teclas de PC nos avisos. Console: o layout do Switch no teclado em todo lugar.")
PC_TR("PC (default): one key per action (Space jumps, Shift sprints, F attacks...). Console layout: the old keys in the world. Menus use the PC keys either way.",
      "PC (padrão): uma tecla por ação (Espaço pula, Shift corre, F ataca...). Layout de console: as teclas antigas no mundo. Os menus usam as teclas de PC nos dois casos.")
PC_TR("The keys of the Console scheme: Classic, PC (WASD), or Custom (the key bindings below).",
      "As teclas do esquema Console: Clássico, PC (WASD) ou Personalizado (atribuições abaixo).")
PC_TR("The button pictures in prompts and menus. Automatic follows the device you used last: keyboard and mouse, Xbox, PlayStation or Nintendo.",
      "As imagens dos botões em avisos e menus. Automático segue o último dispositivo usado: teclado e mouse, Xbox, PlayStation ou Nintendo.")
PC_TR("The pad's face buttons in menus and dialogues: as the game, printed A accepts and B goes back, or the Switch layout (right accepts). The world keeps its layout.",
      "Os botões frontais do controle em menus e diálogos: como no jogo, o A impresso confirma e o B volta, ou o layout do Switch (direita confirma). O mundo mantém seu layout.")
PC_TR("In menus a mouse pointer shows and selects what it points at; left click is A, Esc goes back.",
      "Nos menus, um ponteiro do mouse aparece e seleciona o que aponta; clique esquerdo é A, Esc volta.")
PC_TR("In the pause menu and the Purah Pad: Esc goes back, PageUp / PageDown or the wheel switch tabs, left click selects and right click goes back.",
      "No menu de pausa e na Tabuleta da Purah: Esc volta, PageUp / PageDown ou a roda trocam de aba, clique esquerdo seleciona e clique direito volta.")
PC_TR("How fast the mouse turns the camera (click the window to capture the pointer; Esc, F10 or Alt+Tab frees it). 0 = off; above 1 turns faster.",
      "A velocidade com que o mouse gira a câmera (clique na janela para capturar o ponteiro; Esc, F10 ou Alt+Tab o libera). 0 = desligado; acima de 1 gira mais rápido.")
PC_TR("How fast the mouse turns the camera and aims. Higher turns faster.",
      "A velocidade com que o mouse gira a câmera e mira. Maior gira mais rápido.")
PC_TR("The mouse's speed while the bow is drawn, times the usual one (1 = the same). Lower aims more finely.",
      "A velocidade do mouse com o arco puxado, multiplicada pela usual (1 = a mesma). Menor mira com mais precisão.")
PC_TR("How much mouse movement a second counts as the stick pushed all the way. Lower turns faster.",
      "Quanto movimento do mouse por segundo conta como o analógico todo empurrado. Menor gira mais rápido.")
PC_TR("Moving the mouse up looks down, as in flight games.",
      "Mover o mouse para cima olha para baixo, como em jogos de voo.")
PC_TR("The mouse buttons aim and shoot.",
      "Os botões do mouse miram e atiram.")
PC_TR("The mouse stands in for a pad's motion aim: while aiming, always, or off.",
      "O mouse faz o papel da mira por movimento do controle: ao mirar, sempre ou desligado.")
PC_TR("How far motion aim turns for a mouse movement. Higher turns further.",
      "Quanto a mira por movimento gira para um movimento do mouse. Maior gira mais.")
PC_TR("While a D-pad quick menu is open (weapons, shields, items, bows), each wheel notch moves one entry.",
      "Com um menu rápido do direcional aberto (armas, escudos, itens, arcos), cada clique da roda move uma entrada.")
PC_TR("On the map the mouse pans, the wheel zooms, a left click places a pin and a right click goes back.",
      "No mapa, o mouse desloca, a roda dá zoom, clique esquerdo coloca um marcador e clique direito volta.")
PC_TR("With the walk key held (Alt in the PC layout), how far the movement keys push the stick: lower walks slower; 1 is as without the key.",
      "Com a tecla de andar pressionada (Alt no layout de PC), quanto as teclas de movimento empurram o analógico: menos anda mais devagar; 1 é como sem a tecla.")
PC_TR("How long a quick tap of W, A, S or D keeps Link moving, so he takes a step instead of sliding. 0 = off.",
      "Por quanto tempo um toque rápido em W, A, S ou D mantém Link andando, para ele dar um passo em vez de deslizar. 0 = desligado.")
PC_TR("One press of the sprint key starts sprinting and the next stops it. Off: hold it, as on the Switch.",
      "Um toque na tecla de corrida começa a correr e o seguinte interrompe. Desligado: segure, como no Switch.")
PC_TR("One press of the shield / target key raises the shield and targets, the next lowers it. Off: hold it, as on the Switch.",
      "Um toque na tecla de escudo / mira levanta o escudo e fixa o alvo, o seguinte o baixa. Desligado: segure, como no Switch.")
PC_TR("How fast moving the mouse sideways turns an object while R is held in Ultrahand or Autobuild (1x: as far as the camera turns). The wheel turns it vertically; moving the mouse up or down does not turn it.",
      "Quão rápido mover o mouse para os lados gira um objeto com R segurado na Ultramão ou na Autoconstrução (1x: o mesmo que a câmera gira). A roda o gira na vertical; mover o mouse para cima ou para baixo não o gira.")
PC_TR("A non-Nintendo pad's face buttons: by position (the Switch layout: the right button confirms) or by label (Xbox A confirms).",
      "Os botões frontais de um controle não Nintendo: por posição (layout do Switch: o botão direito confirma) ou por rótulo (A do Xbox confirma).")
PC_TR("Controller vibration: off, normal, or stronger (x2 to x4).",
      "Vibração do controle: desligada, normal ou mais forte (x2 a x4).")
PC_TR("A pad's motion controls (aiming, some puzzles). Off ignores the gyro.",
      "Controles de movimento de um gamepad (mira, alguns quebra-cabeças). Desligado ignora o giroscópio.")
PC_TR("How far motion aim turns for a turn of the pad: 1 = the pad's own, 2 = twice as far.",
      "Quanto a mira por movimento gira para um giro do controle: 1 = o do próprio controle, 2 = o dobro.")
PC_TR("How light rumbles feel next to strong ones: below 1 light ones come through stronger, above 1 only strong ones do. 1 = the game's own.",
      "Como as vibrações leves se comparam às fortes: abaixo de 1 as leves chegam mais fortes, acima de 1 só as fortes. 1 = o do jogo.")
PC_TR("Xbox One / Series pads: the trigger motors buzz with the game's rumble. Off by default (the Switch pad has none).",
      "Controles Xbox One / Series: os motores dos gatilhos vibram com a vibração do jogo. Desligado por padrão (o controle do Switch não os tem).")
PC_TR("How fast the right stick turns the camera: 1 = the game's own, 2 = twice as fast. The mouse keeps its own sensitivity.",
      "A velocidade com que o analógico direito gira a câmera: 1 = a do jogo, 2 = o dobro. O mouse mantém a própria sensibilidade.")
PC_TR("How far the left stick must move before it counts. Raise it if Link moves with the stick at rest; lower it for finer steps. 0 = the pad's own.",
      "Quanto o analógico esquerdo precisa se mover para contar. Aumente se Link anda com o analógico parado; diminua para passos mais precisos. 0 = o do próprio controle.")
PC_TR("How far the right stick must move before it counts. Raise it if the camera turns with the stick at rest; lower it for finer aiming. 0 = the pad's own.",
      "Quanto o analógico direito precisa se mover para contar. Aumente se a câmera gira com o analógico parado; diminua para mira mais precisa. 0 = o do próprio controle.")
PC_TR("The game's texts and subtitles, and these settings' own (English, Spanish, Russian, Brazilian Portuguese, Simplified Chinese, Traditional Chinese). Português (Brasil): 1.4.3's texts; with 1.0.0 they need the 1.4.3 update given to Setup (English otherwise), voices stay English. Automatic: Windows' language if the game has it, else English.",
      "Os textos e legendas do jogo, e os destas configurações (inglês, espanhol, russo, português do Brasil, chinês simplificado, chinês tradicional). Português (Brasil): os textos da 1.4.3; na 1.0.0 eles precisam da atualização 1.4.3 entregue ao Setup (senão, inglês); as vozes continuam em inglês. Automático: o idioma do Windows, se o jogo o tiver; senão, inglês.")
PC_TR("A new game skips the opening and starts on the Great Sky Island. F2 or Minus on the title screen also switch it. Game version 1.0.0 only (on 1.4.x the opening plays and the row is hidden).",
      "Um novo jogo pula a abertura e começa na Great Sky Island. F2 ou Menos na tela de título também alternam. Só na versão 1.0.0 do jogo (na 1.4.x a abertura é exibida e a linha fica oculta).")
PC_TR("Docked (default) or handheld: handheld renders at the Switch's lower handheld resolution (faster, blurrier).",
      "Modo TV (padrão) ou portátil: o portátil renderiza na resolução portátil mais baixa do Switch (mais rápido, mais borrado).")
PC_TR("Pauses the game, picture and sound, while its window is not in front.",
      "Pausa o jogo, imagem e som, enquanto sua janela não está em primeiro plano.")
PC_TR("The temperature on the Purah Pad's map: °C or °F. Automatic follows the game's language.",
      "A temperatura no mapa da Tabuleta da Purah: °C ou °F. Automático segue o idioma do jogo.")
PC_TR("How many older copies of your saves are kept, in case a save goes wrong.",
      "Quantas cópias antigas dos seus saves são mantidas, caso um save dê problema.")
PC_TR("F12 saves a screenshot as a PNG in the Screenshots folder.",
      "F12 salva uma captura de tela em PNG na pasta Screenshots.")
PC_TR("After a crash, a report and a minidump are written to attach to a bug report. Nothing is sent anywhere.",
      "Após uma falha, um relatório e um minidump são gravados para anexar a um relato de bug. Nada é enviado a lugar nenhum.")
PC_TR("Checks GitHub for a newer version when the game starts (at most once a day); asks first if the update rebuilds the game.",
      "Procura no GitHub uma versão mais nova ao iniciar o jogo (no máximo uma vez por dia); pergunta antes se a atualização recompila o jogo.")
PC_TR("%s: get off",
      "%s: soltar")
PC_TR("Control scheme",
      "Esquema de controles")
PC_TR("PC: the keyboard and mouse controls of the menus (Q / E tabs, Esc back, the mouse pointer), Esc / I / M / J in the world and the prompts with PC keys. Console: the keyboard layout below in every screen.",
      "PC: os controles de teclado e mouse nos menus (abas Q / E, Esc volta, o ponteiro do mouse), Esc / I / M / J no mundo e os avisos com teclas de PC. Console: o layout de teclado abaixo em todas as telas.")
PC_TR("Gameplay keys",
      "Teclas de jogo")
PC_TR("TOTK_GAMEPLAY. With the PC scheme: PC (default) = one key per action: Space jumps, Shift sprints, F attacks, Q guards, 4-9 pick an ability, Tab opens the wheel...; Console layout = the keys as before in the world. Menus use the PC keys either way.",
      "TOTK_GAMEPLAY. Com o esquema PC: PC (padrão) = uma tecla por ação: Espaço pula, Shift corre, F ataca, Q defende, 4-9 escolhem uma habilidade, Tab abre a roda...; Layout de console = as teclas de antes no mundo. Os menus usam as teclas de PC nos dois casos.")
PC_TR("Console layout",
      "Layout de console")
PC_TR("Language",
      "Idioma")
PC_TR("TOTK_LANG: the game's texts and voices' subtitles, and the PC settings' own texts (English, Spanish, Russian, Brazilian Portuguese, Simplified Chinese, Traditional Chinese). Português (Brasil): 1.4.3's texts; with 1.0.0 they need the 1.4.3 update given to Setup (English otherwise), voices stay English. Automatic: Windows' display language if the game has it, else English.",
      "TOTK_LANG: os textos do jogo, as legendas das vozes e os textos das configurações do PC (inglês, espanhol, russo, português do Brasil, chinês simplificado, chinês tradicional). Português (Brasil): os textos da 1.4.3; na 1.0.0 eles precisam da atualização 1.4.3 entregue ao Setup (senão, inglês); as vozes continuam em inglês. Automático: o idioma de exibição do Windows, se o jogo o tiver; senão, inglês.")
PC_TR("Automatic (Windows' language)",
      "Automático (idioma do Windows)")
PC_TR("Russian",
      "Russo")
PC_TR("Japanese",
      "Japonês")
PC_TR("Korean",
      "Coreano")
PC_TR("Chinese (Simplified)",
      "Chinês (simplificado)")
PC_TR("Chinese (Traditional)",
      "Chinês (tradicional)")
PC_TR("This folder has no romfs, exefs or cheats files.",
      "Esta pasta não tem arquivos romfs, exefs ou cheats.")
PC_TR("The folder name contains |, which the mod list uses between names. Rename the folder.",
      "O nome da pasta contém |, que a lista de mods usa entre nomes. Renomeie a pasta.")
PC_TR("It is made for another game (title %s).",
      "Foi feito para outro jogo (título %s).")
PC_TR("An earlier mods folder has a mod with this name; this one is left out.",
      "Uma pasta de mods anterior tem um mod com este nome; este fica de fora.")
PC_TR("It has code modules (subsdk or main), which the port cannot load, so the whole mod stays off.",
      "Tem módulos de código (subsdk ou main), que o port não consegue carregar, então o mod inteiro fica desligado.")
PC_TR("It only has code patches (exefs) or cheats, which this version cannot use yet.",
      "Só tem patches de código (exefs) ou cheats, que esta versão ainda não consegue usar.")
PC_TR("A mod's folder",
      "Pasta de um mod")
PC_TR("A mod (.zip, .7z or .rar)",
      "Um mod (.zip, .7z ou .rar)")
PC_TR("code patches",
      "patches de código")
PC_TR("Installed %s (%s, for %s) in %s. It loads at the next start.",
      "Instalado %s (%s, para %s) em %s. Ele carrega na próxima vez que o jogo iniciar.")
PC_TR("Its files don't name a game version: it was put with the one you play.",
      "Os arquivos dele não indicam uma versão do jogo: foi colocado com a que você joga.")
PC_TR("It is made for game version %s, which the port does not build: it was put with the 1.4.3 mods and may not work.",
      "Ele foi feito para a versão %s do jogo, que o port não compila: foi colocado com os mods da 1.4.3 e pode não funcionar.")
PC_TR("Not installed: it could not be opened. Unpack it yourself and choose its folder with Install a mod folder.",
      "Não instalado: não foi possível abri-lo. Descompacte-o você mesmo e escolha a pasta em Instalar pasta de mod.")
PC_TR("Not installed: it has no romfs, exefs or cheats folder. If the mod is packed inside it, unpack it and choose that folder instead.",
      "Não instalado: ele não tem pasta romfs, exefs ou cheats. Se o mod estiver dentro de outra pasta, descompacte e escolha essa pasta.")
PC_TR("Not installed: it is a mod for another game.",
      "Não instalado: é um mod para outro jogo.")
PC_TR("Not installed: it loads code of its own (subsdk / main), which a recompiled game cannot load.",
      "Não instalado: ele carrega código próprio (subsdk / main), que um jogo recompilado não consegue carregar.")
PC_TR("Not installed: it could not be copied. Check that the mods folder can be written (Open mods folder).",
      "Não instalado: não foi possível copiá-lo. Verifique se a pasta de mods pode ser gravada (Abrir pasta de mods).")
PC_TR("Mods load at the next start. The top mod wins a file two mods change; the port's own files (PC button prompts, fixes) stay over a mod's.",
      "Os mods carregam na próxima inicialização. O mod do topo vence quando dois mods mudam o mesmo arquivo; os arquivos do próprio port (ícones de botões do PC, correções) ficam acima dos de um mod.")
PC_TR("The launcher chose these mods.",
      "O launcher escolheu estes mods.")
PC_TR("Open mods folder",
      "Abrir pasta de mods")
PC_TR("Install a mod...",
      "Instalar um mod...")
PC_TR("Install a mod folder...",
      "Instalar pasta de mod...")
PC_TR("Installing...",
      "Instalando...")
PC_TR("In exclusive fullscreen the game goes to the desktop while the dialog is open.",
      "Em tela cheia exclusiva, o jogo vai para a área de trabalho enquanto a janela estiver aberta.")
PC_TR("No mods yet. Put each mod in a folder of its own in the mods folder (its romfs, exefs and cheats folders as emulators take them), then start the game again.",
      "Ainda não há mods. Coloque cada mod em uma pasta própria dentro da pasta de mods (com as pastas romfs, exefs e cheats como os emuladores esperam) e inicie o jogo de novo.")
PC_TR("Ctrl+Up / Ctrl+Down (pad: Y / X) move the mod in focus.",
      "Ctrl+Cima / Ctrl+Baixo (controle: Y / X) movem o mod em foco.")
PC_TR("%d files replaced, %d added",
      "%d arquivos substituídos, %d adicionados")
PC_TR("; %d left to the port's own files",
      "; %d deixados para os arquivos do próprio port")
PC_TR("; %d left to %s, higher in the list",
      "; %d deixados para %s, mais acima na lista")
PC_TR("; %d for another game version, not used",
      "; %d para outra versão do jogo, não usados")
PC_TR("Processor: %s, %d cores, %lu threads. Memory: %.0f GB.",
      "Processador: %s, %d núcleos, %lu threads. Memória: %.0f GB.")
PC_TR("unknown",
      "desconhecido")
PC_TR("Graphics card: %s, %.0f GB.",
      "Placa de vídeo: %s, %.0f GB.")
PC_TR("Display: %s",
      "Tela: %s")
PC_TR("1%% low %.1f fps (%.1f ms)",
      "1%% baixo %.1f fps (%.1f ms)")
PC_TR("1%% low: collecting",
      "1%% baixo: coletando")
PC_TR("frame time 0-50 ms  (F9)",
      "tempo de quadro 0-50 ms  (F9)")
PC_TR("frame",
      "quadro")
PC_TR("1% low",
      "1% baixo")
PC_TR("worst",
      "pior")
PC_TR("real",
      "reais")
PC_TR("Press a key or mouse button (Esc: cancel)",
      "Pressione uma tecla ou botão do mouse (Esc: cancelar)")
PC_TR("(the layout's keys)",
      "(as teclas do layout)")
PC_TR("Add key",
      "Adicionar tecla")
PC_TR("Clear",
      "Limpar")
PC_TR("A (right)",
      "A (direita)")
PC_TR("B (bottom)",
      "B (baixo)")
PC_TR("X (top)",
      "X (cima)")
PC_TR("Y (left)",
      "Y (esquerda)")
PC_TR("L stick",
      "Analógico L")
PC_TR("R stick",
      "Analógico R")
PC_TR("none",
      "nenhum")
PC_TR("Press a pad button (Esc: cancel)",
      "Pressione um botão do controle (Esc: cancelar)")
PC_TR("(its own button)",
      "(o próprio botão)")
PC_TR("Add button",
      "Adicionar botão")
PC_TR("this run",
      "nesta sessão")
PC_TR("Set by NX_SET_ for this run only; not saved.",
      "Definido por NX_SET_ só para esta sessão; não é salvo.")
PC_TR("at next start",
      "na próxima inicialização")
PC_TR("restart",
      "reiniciar")
PC_TR("Point at a setting, or move to it, to see what it does.",
      "Aponte para uma opção, ou vá até ela, para ver o que faz.")
PC_TR("Applies when the game starts again.",
      "Vale quando o jogo iniciar de novo.")
PC_TR("No settings on this page yet.",
      "Ainda não há opções nesta página.")
PC_TR("Reset this page to defaults",
      "Restaurar o padrão desta página")
PC_TR("Settings  (F10)",
      "Configurações  (F10)")
PC_TR("Keys",
      "Teclas")
PC_TR("Some changes apply when the game starts again.",
      "Algumas mudanças valem quando o jogo iniciar de novo.")
PC_TR("Restart the game now",
      "Reiniciar o jogo agora")
PC_TR("The game closes and starts again with the new settings. Progress since your last save is lost.",
      "O jogo fecha e inicia de novo com as novas configurações. O progresso desde o último save é perdido.")
PC_TR("Restart",
      "Reiniciar")
PC_TR("Cancel",
      "Cancelar")
PC_TR("Saved to %s",
      "Salvo em %s")
PC_TR("Not saved (TOTK_SETTINGS=0)",
      "Não salvo (TOTK_SETTINGS=0)")
PC_TR("Copy system info",
      "Copiar info do sistema")
PC_TR("Copied: paste it into your bug report.",
      "Copiado: cole no seu relato de bug.")
PC_TR("Could not copy.",
      "Não foi possível copiar.")
PC_TR("F10, Esc, B or L3 + R3 closes. A: select. L / R: pages. F9: perf overlay.",
      "F10, Esc, B ou L3 + R3 fecham. A: selecionar. L / R: páginas. F9: overlay de desempenho.")
PC_TR("Low",
      "Baixo")
PC_TR("Medium",
      "Médio")
PC_TR("High",
      "Alto")
PC_TR("Welcome",
      "Boas-vindas")
PC_TR("Pick graphics settings for this PC. Everything can be changed later with F10 (or L3 + R3).",
      "Escolha as configurações gráficas para este PC. Tudo pode ser mudado depois com F10 (ou L3 + R3).")
PC_TR("Set for this display: frame rate %s%s.%s",
      "Ajustado para esta tela: taxa de quadros %s%s.%s")
PC_TR("Unlocked (up to the refresh)",
      "Livre (até a taxa de atualização)")
PC_TR(", VRR on",
      ", VRR ligado")
PC_TR(" HDR output (off) is in F10 > Graphics > HDR.",
      " A saída HDR (desligada) fica em F10 > Gráficos > HDR.")
PC_TR("%s (recommended)",
      "%s (recomendado)")
PC_TR("Keep the current settings",
      "Manter as configurações atuais")
PC_TR("Some choices apply when the game starts again. Esc keeps the current settings.",
      "Algumas escolhas valem quando o jogo iniciar de novo. Esc mantém as configurações atuais.")
PC_TR("Applies when the game starts again: F10 > Restart the game now",
      "Vale quando o jogo iniciar de novo: F10 > Reiniciar o jogo agora")
PC_TR("Graphics preset",
      "Preset gráfico")
PC_TR("Sets the rows below it; any change by hand makes it Custom.",
      "Define as opções abaixo; qualquer mudança manual a torna Personalizado.")
PC_TR("Custom",
      "Personalizado")
PC_TR("First-run screen done",
      "Tela inicial concluída")
PC_TR("F10 hint shown",
      "Dica do F10 mostrada")
PC_TR("Window mode",
      "Modo de janela")
PC_TR("F11 or Alt+Enter also switch it.",
      "F11 ou Alt+Enter também alternam.")
PC_TR("Windowed",
      "Em janela")
PC_TR("Borderless fullscreen (default)",
      "Tela cheia sem bordas (padrão)")
PC_TR("Exclusive fullscreen",
      "Tela cheia exclusiva")
PC_TR("Window size",
      "Tamanho da janela")
PC_TR("Windowed mode only (LARECOMP_WINDOW at start).",
      "Só no modo em janela (LARECOMP_WINDOW na inicialização).")
PC_TR("1280 x 720 (default)",
      "1280 x 720 (padrão)")
PC_TR("Frame rate",
      "Taxa de quadros")
PC_TR("TOTK_FPS / TOTK_UNLOCK_FPS. Unlocked: up to the monitor's refresh rate (60 to 240), loading screens at the game's own pace. Steady: unlocked, but held at the highest rate the scene keeps, in whole refreshes of the monitor (240, 120, 80, 60 on a 240 Hz one), instead of swinging. 40 and 45: for a PC that cannot hold 60; smooth on a monitor whose refresh they divide (40 on 120 Hz, 45 on 90 or 180 Hz) or with VRR, uneven on a fixed 60 Hz.",
      "TOTK_FPS / TOTK_UNLOCK_FPS. Livre: até a taxa de atualização do monitor (60 a 240), com as telas de carregamento no ritmo do jogo. Estável: livre, mas mantida na maior taxa que a cena sustenta, em atualizações inteiras do monitor (240, 120, 80, 60 em um de 240 Hz), em vez de oscilar. 40 e 45: para um PC que não segura 60; fluidas em um monitor cuja taxa as divide (40 em 120 Hz, 45 em 90 ou 180 Hz) ou com VRR, irregulares em 60 Hz fixos.")
PC_TR("30 (the game's own)",
      "30 (a do jogo)")
PC_TR("Unlocked (monitor rate)",
      "Livre (taxa do monitor)")
PC_TR("Unlocked, steady (a rate it can hold)",
      "Livre, estável (uma taxa sustentável)")
PC_TR("Steady's lowest rate",
      "Mínimo do modo estável")
PC_TR("TOTK_STEADY_FLOOR. Frame rate Steady only: the lowest rate it holds. 20 or 15 keep a PC that cannot hold 30 even (3 or 4 refreshes a frame on 60 Hz).",
      "TOTK_STEADY_FLOOR. Só com a taxa de quadros Estável: a menor taxa que ela mantém. 20 ou 15 deixam regular um PC que não segura 30 (3 ou 4 atualizações por quadro em 60 Hz).")
PC_TR("30 (default)",
      "30 (padrão)")
PC_TR("Perf overlay (F9)",
      "Overlay de desempenho (F9)")
PC_TR("Fps, 1% low and the frame-time graph.",
      "Fps, 1% baixo e o gráfico de tempo de quadro.")
PC_TR("Fps readout (F3)",
      "Contador de fps (F3)")
PC_TR("TOTK_FPS_OVERLAY. F3 shows or hides it in game.",
      "TOTK_FPS_OVERLAY. F3 mostra ou oculta no jogo.")
PC_TR("Off (default; F3 shows it)",
      "Desligado (padrão; F3 mostra)")
PC_TR("Minimal: fps",
      "Mínimo: fps")
PC_TR("Detailed: fps, frame time, 1% low, worst",
      "Detalhado: fps, quadro, 1% baixo, pior")
PC_TR("Fps readout corner",
      "Canto do contador")
PC_TR("Top right keeps clear of the hearts and the map.",
      "No canto superior direito, não cobre os corações nem o mapa.")
PC_TR("Top right (default)",
      "Superior direito (padrão)")
PC_TR("Top left",
      "Superior esquerdo")
PC_TR("Bottom right",
      "Inferior direito")
PC_TR("Bottom left",
      "Inferior esquerdo")
PC_TR("Fps readout size",
      "Tamanho do contador")
PC_TR("Follows the window height (1080p to 4K).",
      "Acompanha a altura da janela (1080p a 4K).")
PC_TR("Small (default)",
      "Pequeno (padrão)")
PC_TR("Medium size",
      "Tamanho médio")
PC_TR("Large size",
      "Tamanho grande")
PC_TR("Render resolution",
      "Resolução de renderização")
PC_TR("TOTK_RES_SCALE, of the game's 1600 x 900. Window resolution: the 3D scene as large as the picture in the window (x0.83 of it on an integrated GPU or one under 8 GB; two thirds of it with DLSS, FSR or XeSS, their Quality). Applies at once.",
      "TOTK_RES_SCALE, sobre os 1600 x 900 do jogo. Resolução da janela: a cena 3D do tamanho da imagem na janela (x0,83 dela numa GPU integrada ou com menos de 8 GB; dois terços com DLSS, FSR ou XeSS, o modo Qualidade). Vale na hora.")
PC_TR("100 % (native)",
      "100 % (nativa)")
PC_TR("Window resolution (default)",
      "Resolução da janela (padrão)")
PC_TR("Window resolution x0.83 (default: integrated GPU or under 8 GB)",
      "Resolução da janela x0,83 (padrão: GPU integrada ou com menos de 8 GB)")
PC_TR("Native-resolution interface",
      "Interface em resolução nativa")
PC_TR("TOTK_OUT_SCALE: above 1080p, menus, text and the HUD drawn at the window's resolution instead of the game's 1920 x 1080 stretched; the 3D scene reaches it too. On by default. Applies at once.",
      "TOTK_OUT_SCALE: acima de 1080p, menus, texto e HUD são desenhados na resolução da janela, em vez dos 1920 x 1080 do jogo esticados; a cena 3D também chega a ela. Ligado por padrão. Vale na hora.")
PC_TR("Dynamic resolution",
      "Resolução dinâmica")
PC_TR("Off: the scene always at the render resolution. On: the game lowers it only when the GPU is short of time, never below the chosen floor, aiming at the frame rate. Applies at once.",
      "Desligada: a cena sempre na resolução de renderização. Ligada: o jogo a reduz só quando a GPU está sem tempo, nunca abaixo do piso escolhido, visando a taxa de quadros. Vale na hora.")
PC_TR("Off (fixed size)",
      "Desligada (fixa)")
PC_TR("On, never below 90 %",
      "Ligada, mín. 90 %")
PC_TR("On, never below 75 %",
      "Ligada, mín. 75 %")
PC_TR("On, never below 50 %",
      "Ligada, mín. 50 %")
PC_TR("On, the game's own (no minimum)",
      "Ligada, a do jogo (sem mínimo)")
PC_TR("Dynamic resolution (clock)",
      "Resolução dinâmica (relógio)")
PC_TR("TOTK_DYNRES: the game's own, aimed at the frame rate.",
      "TOTK_DYNRES: a do próprio jogo, visando a taxa de quadros.")
PC_TR("3D upscaler",
      "Upscaler 3D")
PC_TR("TOTK_SSAA. Applies at once.",
      "TOTK_SSAA. Vale na hora.")
PC_TR("Bicubic (recomp)",
      "Bicúbico (recomp)")
PC_TR("FSR 1 (the game's)",
      "FSR 1 (o do jogo)")
PC_TR("Shipped shader list",
      "Lista de shaders incluída")
PC_TR("TOTK_SHADER_SEED: on, the shader list that comes with the build lets the game prepare those shaders before you meet them (fewer stutters the first time). Off: only the shaders this PC has met itself. Takes effect at the next start.",
      "TOTK_SHADER_SEED: ligada, a lista de shaders que acompanha a versão deixa o jogo preparar esses shaders antes de você encontrá-los (menos travadas na primeira vez). Desligada: só os shaders que este PC já encontrou. Vale na próxima inicialização.")
PC_TR("Window upscaler",
      "Upscaler da janela")
PC_TR("FSR 1 (window above 1080p)",
      "FSR 1 (janela acima de 1080p)")
PC_TR("Anti-aliasing",
      "Antisserrilhado")
PC_TR("TOTK_AA, on the presented image.",
      "TOTK_AA, sobre a imagem apresentada.")
PC_TR("Sharpening",
      "Nitidez")
PC_TR("RCAS low",
      "RCAS baixo")
PC_TR("RCAS medium",
      "RCAS médio")
PC_TR("RCAS high",
      "RCAS alto")
PC_TR("Colour",
      "Cor")
PC_TR("Colour adjustment",
      "Ajuste de cor")
PC_TR("TOTK_COLOR: the game's picture, its HUD included (not this menu). Off shows the game's own colours unchanged and costs nothing. Vivid: livelier colours. Brighter dark tones: caves, the Depths and nights lighter. Custom: the four rows below. Applies at once.",
      "TOTK_COLOR: a imagem do jogo, com o HUD (não este menu). Desligado mostra as cores do jogo sem mudança e não custa nada. Vívido: cores mais vivas. Tons escuros mais claros: cavernas, as Depths e as noites mais claras. Personalizado: as 4 opções abaixo. Vale na hora.")
PC_TR("Off (the game's own colours; default)",
      "Desligado (cores do jogo; padrão)")
PC_TR("Vivid",
      "Vívido")
PC_TR("Brighter dark tones",
      "Tons escuros mais claros")
PC_TR("Custom (the four rows below)",
      "Personalizado (as 4 opções abaixo)")
PC_TR("Brightness",
      "Brilho")
PC_TR("TOTK_COLOR_BRIGHTNESS (Custom only): added to every colour; 0 is the game's.",
      "TOTK_COLOR_BRIGHTNESS (só Personalizado): somado a cada cor; 0 é o do jogo.")
PC_TR("Contrast",
      "Contraste")
PC_TR("TOTK_COLOR_CONTRAST (Custom only): around mid grey; 1 is the game's.",
      "TOTK_COLOR_CONTRAST (só Personalizado): em torno do cinza médio; 1 é o do jogo.")
PC_TR("Saturation",
      "Saturação")
PC_TR("TOTK_COLOR_SATURATION (Custom only): 1 is the game's, 0.5 greyer, 2 twice as colourful.",
      "TOTK_COLOR_SATURATION (só Personalizado): 1 é o do jogo, 0,5 mais acinzentado, 2 com o dobro de cor.")
PC_TR("Gamma",
      "Gama")
PC_TR("TOTK_COLOR_GAMMA (Custom only): below 1 brightens dark tones, above 1 darkens them; 1 is the game's.",
      "TOTK_COLOR_GAMMA (só Personalizado): abaixo de 1 clareia os tons escuros, acima de 1 os escurece; 1 é o do jogo.")
PC_TR("Shadow map scale (renderer)",
      "Escala do mapa de sombras (renderer)")
PC_TR("TOTK_SHADOW_SCALE: the renderer's shadow targets. The game patches' Shadow resolution is separate. Applies at once.",
      "TOTK_SHADOW_SCALE: os alvos de sombra do renderer. A Resolução das sombras dos patches do jogo é separada. Vale na hora.")
PC_TR("2x (default)",
      "2x (padrão)")
PC_TR("Soft shadows",
      "Sombras suaves")
PC_TR("TOTK_SOFT_SHADOWS: the sun's shadow edges widen with the distance to what casts them (contact-hardening, PCSS) instead of keeping one width. Off is the game's look. A change rebuilds the shadowed shaders in the background: the old shadows show until they are ready.",
      "TOTK_SOFT_SHADOWS: as bordas das sombras do sol se alargam com a distância até o que as projeta (contact-hardening, PCSS), em vez de manter uma largura só. Desligado é o visual do jogo. Uma mudança recompila em segundo plano os shaders de sombra: as sombras antigas aparecem até ficarem prontos.")
PC_TR("Off (the game's)",
      "Desligadas (do jogo)")
PC_TR("Soft",
      "Suaves")
PC_TR("Softer",
      "Mais suaves")
PC_TR("Softest",
      "Suavíssimas")
PC_TR("Contact shadows",
      "Sombras de contato")
PC_TR("TOTK_CONTACT_SHADOWS: small sun shadows at feet, under stones and in grass, which the shadow maps are too coarse for (a short march toward the sun through the scene's depth, up to 80 m). Off is the game's look; costs some GPU. Applies at once.",
      "TOTK_CONTACT_SHADOWS: pequenas sombras do sol nos pés, sob pedras e na grama, que os mapas de sombras são grosseiros demais para mostrar (uma curta varredura rumo ao sol pela profundidade da cena, até 80 m). Desligado é o visual do jogo; custa um pouco de GPU. Vale na hora.")
PC_TR("Subtle",
      "Sutis")
PC_TR("Strong",
      "Fortes")
PC_TR("TOTK_VOLUME, master gain.",
      "TOTK_VOLUME, ganho geral.")
PC_TR("Mute in the background",
      "Silenciar em segundo plano")
PC_TR("TOTK_MUTE_UNFOCUSED: no sound while the game window is not in front; the game runs on (Pause when the window loses focus stops it too).",
      "TOTK_MUTE_UNFOCUSED: sem som enquanto a janela do jogo não está em primeiro plano; o jogo continua (Pausar ao perder o foco da janela também o para).")
PC_TR("Face buttons",
      "Botões frontais")
PC_TR("By position (Switch layout)",
      "Por posição (layout do Switch)")
PC_TR("By label (Xbox A = A)",
      "Por rótulo (A do Xbox = A)")
PC_TR("Rumble",
      "Vibração")
PC_TR("Motion controls",
      "Controles de movimento")
PC_TR("Start",
      "Início")
PC_TR("Skip the opening",
      "Pular a abertura")
PC_TR("TOTK_SKIP_OPENING. F2 or Minus on the title screen also toggles it.",
      "TOTK_SKIP_OPENING. F2 ou Menos na tela de título também alterna.")
PC_TR("On (start at the Great Sky Island)",
      "Ligado (começa na Great Sky Island)")
PC_TR("Console mode",
      "Modo console")
PC_TR("TOTK_HANDHELD: handheld renders at the lower resolution.",
      "TOTK_HANDHELD: o portátil renderiza na resolução menor.")
PC_TR("Docked",
      "Modo TV")
PC_TR("Handheld",
      "Portátil")
PC_TR("Classic",
      "Clássico")
PC_TR("Custom (TOTK_KBM_<ACTION>)",
      "Personalizado (TOTK_KBM_<ACTION>)")
PC_TR("Mouse buttons aim and shoot",
      "Mirar e atirar com o mouse")
PC_TR("Mouse counts for a full stick",
      "Contagens do mouse p/ analógico cheio")
PC_TR("TOTK_MOUSE_STICK_SPEED (per second).",
      "TOTK_MOUSE_STICK_SPEED (por segundo).")
PC_TR("While aiming",
      "Ao mirar")
PC_TR("Anisotropic filtering",
      "Filtragem anisotrópica")
PC_TR("TOTK_ANISO: a floor under the game's. Applies at once.",
      "TOTK_ANISO: um piso sob o do jogo. Vale na hora.")
PC_TR("The game's",
      "A do jogo")
PC_TR("Texture LOD bias",
      "Viés de LOD de textura")
PC_TR("TOTK_LOD_BIAS: below 0 is sharper. Applies at once.",
      "TOTK_LOD_BIAS: abaixo de 0 fica mais nítido. Vale na hora.")
PC_TR("Compact textures (low VRAM)",
      "Texturas compactas (pouca VRAM)")
PC_TR("LARECOMP_ASTC_BC3: the game's textures kept in a quarter of the video memory (BC3), with slightly softer "
      "colour detail. Automatic: on only for graphics cards with 4 GB or less. Takes effect at the next start.",
      "LARECOMP_ASTC_BC3: as texturas do jogo ocupam um quarto da memória de vídeo (BC3), com o detalhe de cor um "
      "pouco mais suave. Automático: ligado só em placas de vídeo de 4 GB ou menos. Vale na próxima inicialização.")
PC_TR("Automatic (on with 4 GB of VRAM or less)",
      "Automático (ligado com até 4 GB de VRAM)")
PC_TR("For graphics cards with 4 GB of video memory or less: the textures take a quarter of the memory, so the game "
      "no longer runs out of it (stutters, blurry or late textures). Fine colour detail gets a little softer. "
      "Automatic turns it on only on such cards.",
      "Para placas de vídeo com 4 GB de memória de vídeo ou menos: as texturas ocupam um quarto da memória, então o "
      "jogo não fica mais sem ela (travadas, texturas borradas ou atrasadas). O detalhe fino de cor fica um pouco "
      "mais suave. Automático liga só nessas placas.")
PC_TR("Scene upscaler",
      "Upscaler da cena")
PC_TR("TOTK_SCENE_UPSCALE. Applies at once.",
      "TOTK_SCENE_UPSCALE. Vale na hora.")
PC_TR("Save backups kept",
      "Backups de save")
PC_TR("10 (default)",
      "10 (padrão)")
PC_TR("F12 screenshots",
      "Capturas de tela F12")
PC_TR("TOTK_PC_SHOT: PNGs in the Screenshots folder.",
      "TOTK_PC_SHOT: PNGs na pasta Screenshots.")
PC_TR("Temperature unit",
      "Unidade de temperatura")
PC_TR("TOTK_TEMP_UNIT: the temperature on the Purah Pad's map. Automatic: the game's (°F in English (US), °C in the other languages). The number changes at once, the °C / °F sign beside it the next time the map is opened.",
      "TOTK_TEMP_UNIT: a temperatura no mapa da Tabuleta da Purah. Automática: a do jogo (°F em English (US), °C nos outros idiomas). O número muda na hora; o símbolo °C / °F ao lado dele, na próxima vez que o mapa for aberto.")
PC_TR("Automatic (by the game's language)",
      "Automática (pelo idioma do jogo)")
PC_TR("Crash reports",
      "Relatórios de falhas")
PC_TR("TOTK_PC_CRASH: a report and a minidump per crash.",
      "TOTK_PC_CRASH: um relatório e um minidump por falha.")
PC_TR("Automatic updates",
      "Atualização automática")
PC_TR("Discord status",
      "Status no Discord")
PC_TR("Shows \"Playing\", the time played and a \"Join the community\" button (the game's subreddit) on your Discord profile while Discord runs. Nothing else is sent.",
      "Mostra \"Jogando\", o tempo de jogo e um botão \"Join the community\" (o subreddit do jogo) no seu perfil do Discord enquanto o Discord está aberto. Nada mais é enviado.")
PC_TR("Key bindings (Custom layout)",
      "Atribuições (layout Personalizado)")
PC_TR("TOTK_KBM_A. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_A. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_B. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_B. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_X. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_X. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_Y. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_Y. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_LSTICK. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_LSTICK. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_RSTICK. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_RSTICK. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_L. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_L. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_R. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_R. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_ZL. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_ZL. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_ZR. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_ZR. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_PLUS. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_PLUS. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_MINUS. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_MINUS. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_DPAD_LEFT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_DPAD_LEFT. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_DPAD_UP. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_DPAD_UP. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_DPAD_RIGHT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_DPAD_RIGHT. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_DPAD_DOWN. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_DPAD_DOWN. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_MOVE_LEFT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_MOVE_LEFT. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_MOVE_UP. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_MOVE_UP. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_MOVE_RIGHT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_MOVE_RIGHT. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_MOVE_DOWN. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_MOVE_DOWN. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_CAMERA_LEFT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_CAMERA_LEFT. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_CAMERA_UP. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_CAMERA_UP. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_CAMERA_RIGHT. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_CAMERA_RIGHT. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_CAMERA_DOWN. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_CAMERA_DOWN. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("TOTK_KBM_WALK. Used by the console scheme, and in the world with Gameplay keys: Console. Empty: the PC layout's keys.",
      "TOTK_KBM_WALK. Usado pelo esquema de console e, no mundo, com Teclas de jogo: Console. Vazio: as teclas do layout PC.")
PC_TR("Screenshot saved: %s",
      "Captura de tela salva: %s")
PC_TR("Screenshot failed (%s)",
      "Falha na captura de tela (%s)")
PC_TR("Screenshot skipped (still saving the previous ones)",
      "Captura de tela ignorada (ainda salvando as anteriores)")
PC_TR("TOTK_WINDOW_MODE. F11 or Alt+Enter switch between windowed and the fullscreen kind chosen here.",
      "TOTK_WINDOW_MODE. F11 ou Alt+Enter alternam entre janela e o tipo de tela cheia escolhido aqui.")
PC_TR("TOTK_MONITOR: where fullscreen goes and a windowed start opens. Remember: the monitor the game was on last (drag the window there, or pick it once), at the same window position.",
      "TOTK_MONITOR: para onde vai a tela cheia e onde abre uma inicialização em janela. Lembrar: o monitor em que o jogo esteve por último (arraste a janela até lá, ou escolha uma vez), na mesma posição da janela.")
PC_TR("Exclusive resolution",
      "Resolução exclusiva")
PC_TR("TOTK_EXCLUSIVE_SIZE: the monitor's mode in exclusive fullscreen; the list is the modes of the monitor the game started on. Borderless and windowed keep the desktop's.",
      "TOTK_EXCLUSIVE_SIZE: o modo do monitor na tela cheia exclusiva; a lista tem os modos do monitor em que o jogo iniciou. Sem bordas e em janela mantêm os da área de trabalho.")
PC_TR("Exclusive refresh rate",
      "Taxa de atualização exclusiva")
PC_TR("TOTK_EXCLUSIVE_HZ: the refresh rate in exclusive fullscreen. A rate the resolution lacks gives the fastest it has under it.",
      "TOTK_EXCLUSIVE_HZ: a taxa de atualização na tela cheia exclusiva. Uma taxa que a resolução não tem dá a mais rápida abaixo dela.")
PC_TR("Presentation",
      "Apresentação")
PC_TR("Off: frames go out at once and may tear (LARECOMP_VK_PRESENT=immediate); on: mailbox, or FIFO with VRR.",
      "Desligado: os quadros saem na hora e podem ter tearing (LARECOMP_VK_PRESENT=immediate); ligado: mailbox, ou FIFO com VRR.")
PC_TR("On (default, no tearing)",
      "Ligado (padrão, sem tearing)")
PC_TR("Off (tearing, lowest latency)",
      "Desligado (tearing, menor latência)")
PC_TR("Variable refresh (VRR)",
      "Taxa variável (VRR)")
PC_TR("Only with a G-SYNC / FreeSync monitor (and, windowed, the driver's windowed VRR on): the panel follows each frame (FIFO). Without one it is plain vsync at a fixed refresh, and a cap under it repeats frames. Best with a frame cap a little under the refresh (116 at 120 Hz, 138 at 144 Hz); Unlocked and DLSS frame generation keep under it by themselves.",
      "Só com monitor G-SYNC / FreeSync (e, em janela, com o VRR em janela do driver ligado): o painel segue cada quadro (FIFO). Sem isso é vsync comum em taxa fixa, e um limite abaixo dela repete quadros. O melhor é um limite de quadros pouco abaixo da taxa (116 a 120 Hz, 138 a 144 Hz); Livre e a geração de quadros do DLSS ficam abaixo dela sozinhos.")
PC_TR("Off (default)",
      "Desligada (padrão)")
PC_TR("On (G-Sync / FreeSync monitor)",
      "Ligada (monitor G-Sync / FreeSync)")
PC_TR("Frame pacing",
      "Ritmo de quadros")
PC_TR("TOTK_PACE_AT: where the frame cap waits.",
      "TOTK_PACE_AT: onde o limite de quadros espera.")
PC_TR("At the present (default, steadier)",
      "Na apresentação (padrão, mais estável)")
PC_TR("Before the frame (lower latency)",
      "Antes do quadro (menor latência)")
PC_TR("Game tick source",
      "Fonte do tick do jogo")
PC_TR("TOTK_VSYNC: what the game's 60 Hz tick follows.",
      "TOTK_VSYNC: o que o tick de 60 Hz do jogo segue.")
PC_TR("60 Hz clock (default)",
      "Relógio de 60 Hz (padrão)")
PC_TR("The monitor's vblank (fixed refresh only)",
      "O vblank do monitor (só taxa fixa)")
PC_TR("Precise pacing",
      "Ritmo preciso")
PC_TR("TOTK_PACE_SPIN_US: the cap's last part spins (CPU).",
      "TOTK_PACE_SPIN_US: a parte final do limite fica em espera ativa (CPU).")
PC_TR("0.3 ms",
      "0,3 ms")
PC_TR("0.5 ms",
      "0,5 ms")
PC_TR("Queued frames",
      "Quadros na fila")
PC_TR("TOTK_MAX_QUEUED: frames waiting to be shown (needs present wait).",
      "TOTK_MAX_QUEUED: quadros esperando para serem exibidos (precisa de present wait).")
PC_TR("No limit",
      "Sem limite")
PC_TR("2 (default)",
      "2 (padrão)")
PC_TR("GPU frames in flight",
      "Quadros da GPU em andamento")
PC_TR("1 (lockstep: slower when CPU and GPU are both busy)",
      "1 (sincronizado: mais lento com CPU e GPU ocupadas)")
PC_TR("Low latency",
      "Baixa latência")
PC_TR("TOTK_REFLEX: each frame is held back before it reads input, by as much as it would otherwise wait for the GPU, the render thread or the frame cap later. NVIDIA: Reflex's sleep (VK_NV_low_latency2); other GPUs, or jit: the engine's own. Boost keeps the GPU clocks up (Reflex).",
      "TOTK_REFLEX: cada quadro é retido antes de ler a entrada, pelo tempo que de outro modo esperaria depois pela GPU, pela thread de renderização ou pelo limite de quadros. NVIDIA: o sleep do Reflex (VK_NV_low_latency2); outras GPUs, ou jit: o do próprio engine. Boost mantém os clocks da GPU altos (Reflex).")
PC_TR("On (NVIDIA Reflex; other GPUs: the engine's own)",
      "Ligada (NVIDIA Reflex; outras GPUs: a do engine)")
PC_TR("On + boost",
      "Ligada + boost")
PC_TR("On (the engine's own, any GPU)",
      "Ligada (a do engine, qualquer GPU)")
PC_TR("Shader wait",
      "Espera por shader")
PC_TR("A frame that needs a shader still being built waits for it. Limit to 0.1 s: past that, the frame is shown and the objects whose shaders are not ready yet appear a moment later (fullscreen passes and compute always wait). Always wait: nothing is ever drawn incomplete, but a shader the first time can freeze the picture for a second or more. TOTK_HOLD_MAX_MS overrides.",
      "Um quadro que precisa de um shader ainda em compilação espera por ele. Limitar a 0,1 s: depois disso o quadro é exibido e os objetos cujos shaders ainda não estão prontos aparecem um instante depois (passes de tela cheia e compute sempre esperam). Sempre esperar: nada é desenhado incompleto, mas um shader na primeira vez pode congelar a imagem por um segundo ou mais. TOTK_HOLD_MAX_MS substitui.")
PC_TR("Limit to 0.1 s (default)",
      "Limitar a 0,1 s (padrão)")
PC_TR("Always wait (nothing missing)",
      "Sempre esperar (nada faltando)")
PC_TR("HDR output",
      "Saída HDR")
PC_TR("TOTK_HDR: needs Windows HDR on for the monitor.",
      "TOTK_HDR: precisa do HDR do Windows ligado para o monitor.")
PC_TR("Off (SDR, default)",
      "Desligada (SDR, padrão)")
PC_TR("Peak brightness (nits)",
      "Brilho máximo (nits)")
PC_TR("TOTK_HDR_PEAK: the display's peak.",
      "TOTK_HDR_PEAK: o pico da tela.")
PC_TR("Paper white (nits)",
      "Branco do papel (nits)")
PC_TR("TOTK_HDR_PAPER: the brightness of SDR white.",
      "TOTK_HDR_PAPER: o brilho do branco SDR.")
PC_TR("Highlight colour",
      "Cor das luzes")
PC_TR("TOTK_HDR_HUE: the hue of bright lights from the game's HDR scene, 0 to 100.",
      "TOTK_HDR_HUE: o tom das luzes fortes, tirado da cena HDR do jogo, de 0 a 100.")
PC_TR("Mouse cursor in menus",
      "Cursor do mouse nos menus")
PC_TR("TOTK_MENU_CURSOR: in a menu the pointer shows and selects what it points at; left click is A (back is Esc).",
      "TOTK_MENU_CURSOR: em um menu o ponteiro aparece e seleciona o que aponta; clique esquerdo é A (voltar é Esc).")
PC_TR("Minimum render resolution",
      "Resolução de renderização mínima")
PC_TR("100 %: the scene is always rendered at the render resolution (dynamic resolution off). 0 %: the game's own dynamic resolution. In between: dynamic, never below that fraction.",
      "100 %: a cena é sempre renderizada na resolução de renderização (resolução dinâmica desligada). 0 %: a resolução dinâmica do próprio jogo. No meio: dinâmica, nunca abaixo dessa fração.")
PC_TR("Field of view",
      "Campo de visão")
PC_TR("Multiplier of the camera's field of view (1 = the game's). The degrees are the camera's view right now: aiming and some cutscenes narrow or widen it, and the multiplier keeps their proportion. Wider costs some GPU and CPU (more of the world on screen).",
      "Multiplicador do campo de visão da câmera (1 = o do jogo). Os graus são a visão da câmera agora: mirar e algumas cenas o estreitam ou alargam, e o multiplicador mantém a proporção. Mais largo custa um pouco de GPU e CPU (mais mundo na tela).")
PC_TR("Shadow resolution",
      "Resolução das sombras")
PC_TR("Multiplier of the shadow map's width and height (2 = sharper, 4x the memory).",
      "Multiplicador da largura e da altura do mapa de sombras (2 = mais nítido, 4x a memória).")
PC_TR("Near shadow reach",
      "Alcance das sombras próximas")
PC_TR("Multiplier of how far the sun's sharpest shadow cascades reach (1 = the game's: the first two hand over 32 m and 64 m away, where the change in sharpness shows as a line, mostly at 4K). 2 moves those lines to 64 m and 128 m; the far shadows stay. The same shadow map then covers more ground, so near shadows get softer by that factor: raise Shadow resolution with it. Costs a little GPU and CPU (more objects drawn into the near shadow maps).",
      "Multiplicador de até onde chegam as cascatas de sombra mais nítidas do sol (1 = o do jogo: as duas primeiras passam para as seguintes a 32 m e 64 m, onde a mudança de nitidez aparece como uma linha, sobretudo em 4K). 2 leva essas linhas para 64 m e 128 m; as sombras distantes não mudam. O mesmo mapa de sombras cobre então mais terreno, então as sombras próximas ficam mais suaves nessa proporção: aumente a Resolução das sombras junto. Custa um pouco de GPU e CPU (mais objetos desenhados nos mapas de sombras próximas).")
PC_TR("Sun shadow map size",
      "Mapa de sombras do sol")
PC_TR("The game's: the sun's shadow map shrinks and grows with the area its cascades cover, so near the ground foliage shadows turn coarse, and every size change moves the shadow texels (leaf shadows jump instead of sliding). Fixed: always the largest size the game picks (times the shadow resolution), steady and sharper; costs some GPU.",
      "O do jogo: o mapa de sombras do sol encolhe e cresce com a área que suas cascatas cobrem, então perto do chão as sombras da folhagem ficam grosseiras, e cada mudança de tamanho move os texels da sombra (as sombras das folhas pulam em vez de deslizar). Fixo: sempre o maior tamanho que o jogo escolhe (vezes a resolução das sombras), estável e mais nítido; custa um pouco de GPU.")
PC_TR("Detail distance",
      "Distância de detalhe")
PC_TR("Multiplier of the distance at which models switch to lower detail (2 = full detail twice as far).",
      "Multiplicador da distância em que os modelos passam a um detalhe menor (2 = detalhe total o dobro de longe).")
PC_TR("Actor detail distance",
      "Distância dos atores")
PC_TR("Multiplier of the distances at which characters, enemies and objects start skipping update, animation and draw frames (2 = full rate twice as far; costs CPU).",
      "Multiplicador das distâncias em que personagens, inimigos e objetos começam a pular quadros de atualização, animação e desenho (2 = taxa total o dobro de longe; custa CPU).")
PC_TR("Terrain detail distance",
      "Distância de detalhe do terreno")
PC_TR("Multiplier of the distances at which the ground and water switch to coarser levels (2 = full detail twice as far; costs CPU and GPU).",
      "Multiplicador das distâncias em que o chão e a água passam a níveis mais grosseiros (2 = detalhe total o dobro de longe; custa CPU e GPU).")
PC_TR("Simulation distance",
      "Distância de simulação")
PC_TR("Multiplier of the distance from the camera at which characters, enemies, objects and their physics stop being updated (1 = the game's; 1.5-2 = high). Costs CPU: more actors are simulated every frame, and distant ones keep reacting (fights, physics) instead of freezing.",
      "Multiplicador da distância da câmera em que personagens, inimigos, objetos e sua física deixam de ser atualizados (1 = o do jogo; 1,5-2 = alto). Custa CPU: mais atores são simulados a cada quadro, e os distantes continuam reagindo (lutas, física) em vez de congelar.")
PC_TR("Mouse camera (direct)",
      "Câmera do mouse (direta)")
PC_TR("Multiplier of the camera turn the game itself applies per mouse count (on top of the mouse sensitivity). 0 = off: the mouse turns the camera through emulated right-stick deflection instead.",
      "Multiplicador do giro de câmera que o próprio jogo aplica por contagem do mouse (além da sensibilidade do mouse). 0 = desligado: o mouse gira a câmera por uma deflexão emulada do analógico direito.")
PC_TR("Fixed (steady)",
      "Fixo (estável)")
PC_TR("The game's (changes)",
      "O do jogo (muda)")
PC_TR("Game patches",
      "Patches do jogo")
PC_TR("Aspect ratio",
      "Proporção de tela")
PC_TR("The window's: on a 3:2, 16:10, 21:9, 32:9 or 48:9 window or monitor the game draws a canvas of that shape at start (1920x1280, 1920x1200, 3440x1440, 5120x1440 or 7680x1440), the HUD laid out for it: 21:9, 32:9 and 48:9 a wider view, 3:2 and 16:10 a taller one (the same height of view, a little less at the sides). Other shapes, and 16:9, keep the game's 16:9 picture. 16:10 / 21:9 / 32:9 / 48:9 / 3:2: that canvas whatever the window (on another shape it is shown whole, with bars; 21:9, 32:9 and 48:9 are heavier than the window's own shape: the whole canvas is drawn, then shown smaller). Needs the game patches' RomFS sets (TOTK_PATCH_SETS) and the overlay's layout for that shape. At the next start: the canvas and the game files laid out for it are chosen as the game starts. Setup prepares the layouts of this PC's monitors' shapes; another shape's are made at the start after you pick it (about 30 s, once).",
      "A da janela: em uma janela ou monitor 3:2, 16:10, 21:9, 32:9 ou 48:9 o jogo desenha, ao iniciar, uma tela com esse formato (1920x1280, 1920x1200, 3440x1440, 5120x1440 ou 7680x1440), com o HUD disposto para ela: 21:9, 32:9 e 48:9 uma visão mais larga, 3:2 e 16:10 uma mais alta (a mesma altura de visão, um pouco menos nas laterais). Outros formatos, e 16:9, mantêm a imagem 16:9 do jogo. 16:10 / 21:9 / 32:9 / 48:9 / 3:2: essa tela, seja qual for a janela (em outro formato ela aparece inteira, com barras; 21:9, 32:9 e 48:9 são mais pesadas que o formato da própria janela: a tela inteira é desenhada e depois exibida menor). Precisa dos conjuntos RomFS dos patches do jogo (TOTK_PATCH_SETS) e do layout do overlay para esse formato. Na próxima inicialização: a tela e os arquivos do jogo preparados para ela são escolhidos quando o jogo inicia. O Setup prepara os layouts dos formatos dos monitores deste PC; os de outro formato são feitos na inicialização depois que você o escolhe (cerca de 30 s, uma vez).")
PC_TR("HUD position (21:9, 32:9, 48:9)",
      "Posição do HUD (21:9, 32:9, 48:9)")
PC_TR("Where the HUD goes on a 21:9, 32:9 or 48:9 canvas: at the screen's edges, or in the middle 16:9 (closer to the eyes on a very wide screen). Menus are the same either way. Nothing changes at 16:9. At the next start: the game loads its interface layouts once, as it starts. Automatic (the default): in the middle on 48:9, at the edges otherwise.",
      "Onde o HUD fica em uma tela 21:9, 32:9 ou 48:9: nas bordas da tela, ou no 16:9 central (mais perto dos olhos em uma tela muito larga). Os menus são iguais nos dois casos. Nada muda em 16:9. Na próxima inicialização: o jogo carrega seus layouts de interface uma vez, ao iniciar. Automático (o padrão): no centro em 48:9, nas bordas nos outros casos.")
PC_TR("Texture cache",
      "Cache de texturas")
PC_TR("Multiplier of the game's texture cache (fewer blurry textures when turning back; 490 MiB more per step). Needs the game patches' RomFS sets (TOTK_PATCH_SETS). At the next start: the game sizes its memory once, as it starts.",
      "Multiplicador do cache de texturas do jogo (menos texturas borradas ao se virar; 490 MiB a mais por passo). Precisa dos conjuntos RomFS dos patches do jogo (TOTK_PATCH_SETS). Na próxima inicialização: o jogo dimensiona sua memória uma vez, ao iniciar.")
PC_TR("Grass density",
      "Densidade de grama")
PC_TR("Multiplier of grass, flowers and pebbles. Needs the game patches' RomFS sets (TOTK_PATCH_SETS). At the next start: the game reads its grass table once, as it starts.",
      "Multiplicador de grama, flores e pedrinhas. Precisa dos conjuntos RomFS dos patches do jogo (TOTK_PATCH_SETS). Na próxima inicialização: o jogo lê sua tabela de grama uma vez, ao iniciar.")
PC_TR("Shadow distance",
      "Distância das sombras")
PC_TR("Multiplier of the sharp shadow cascades' distances. Needs the game patches' RomFS sets (TOTK_PATCH_SETS). At the next start: the value is in the shader package, which the game reads once, as it starts.",
      "Multiplicador das distâncias das cascatas de sombras nítidas. Precisa dos conjuntos RomFS dos patches do jogo (TOTK_PATCH_SETS). Na próxima inicialização: o valor está no pacote de shaders, que o jogo lê uma vez, ao iniciar.")
PC_TR("Fog distance",
      "Distância da névoa")
PC_TR("Multiplier of the distance fog's start and end (clearer distant mountains). Needs the game patches' RomFS sets (TOTK_PATCH_SETS). At the next start: the game reads its sky and fog palettes once, as it starts.",
      "Multiplicador do início e do fim da névoa de distância (montanhas distantes mais nítidas). Precisa dos conjuntos RomFS dos patches do jogo (TOTK_PATCH_SETS). Na próxima inicialização: o jogo lê suas paletas de céu e névoa uma vez, ao iniciar.")
PC_TR("Fast-travel speed",
      "Viagem rápida: velocidade")
PC_TR("Fast: Link's departure when travelling is about 2.5 s shorter (its long waits halved, his dissolve as on the Switch); Normal is the game's own. Needs the game patches' RomFS sets (TOTK_PATCH_SETS). At the next start: the game loads the travel scene the first time you travel and keeps it.",
      "Rápida: a partida de Link ao viajar é cerca de 2,5 s mais curta (suas esperas longas pela metade, a dissolução dele como no Switch); Normal é a do jogo. Precisa dos conjuntos RomFS dos patches do jogo (TOTK_PATCH_SETS). Na próxima inicialização: o jogo carrega a cena da viagem na primeira vez que você viaja e a mantém.")
PC_TR("On a 21:9, 32:9 or 48:9 canvas, real-time cutscenes and conversations: wide like the rest of the game, or the original 16:9 framing with bars at the sides (for a scene that looks odd at the edges). Pre-rendered movies are 16:9 video: always shown in the middle with bars.",
      "Em uma tela 21:9, 32:9 ou 48:9, cutscenes e conversas em tempo real: largas como o resto do jogo, ou o enquadramento 16:9 original com barras nas laterais (para uma cena que fica estranha nas bordas). Os vídeos pré-renderizados são 16:9: sempre exibidos no meio, com barras.")
PC_TR("Upscales the 3D scene with a temporal upscaler that builds detail over several frames. DLSS: NVIDIA RTX only; DLAA: DLSS at full resolution, the best image for more GPU. FSR and XeSS: any GPU (XeSS runs fastest on Intel Arc). All cost some GPU; Off is the game's own.",
      "Amplia a cena 3D com um upscaler temporal que constrói os detalhes ao longo de vários quadros. DLSS: só NVIDIA RTX; DLAA: DLSS em resolução total, a melhor imagem em troca de mais GPU. FSR e XeSS: qualquer GPU (o XeSS é mais rápido em Intel Arc). Todos custam um pouco de GPU; Desligado é o do próprio jogo.")
PC_TR("Safe mode (if the game crashes)",
      "Modo seguro (se o jogo fechar)")
PC_TR("For a game that crashes or hangs: the renderer without its faster paths, DLSS, FSR and frame generation, and without the driver paths some GPUs crash in. Slower. At the next start.",
      "Para um jogo que fecha ou trava: o renderizador sem os caminhos mais rápidos, sem DLSS, FSR e geração de quadros e sem os caminhos do driver em que algumas GPUs falham. Mais lento. No próximo início.")
PC_TR("Wide picture: F10 > Restart the game now to use %s",
      "Imagem panorâmica: F10 > Reiniciar o jogo agora para usar %s")
PC_TR("The window changed shape: F10 > Restart the game now to fit it",
      "A janela mudou de formato: F10 > Reiniciar o jogo agora para ajustar a imagem")
PC_TR("Screen shape: start the game once from TotK Launcher.exe to prepare its layout (16:9 until then)",
      "Formato da tela: inicie o jogo uma vez pelo TotK Launcher.exe para preparar a interface (16:9 até lá)")
PC_TR("This screen shape's layout was not prepared: 16:9 until Setup runs again",
      "A interface deste formato de tela não foi preparada: 16:9 até o Setup rodar de novo")
PC_TR("Safe mode is on: DLSS, FSR and frame generation stay off",
      "Modo seguro ativado: DLSS, FSR e a geração de quadros continuam desligados")
PC_TR("Frame generation mode",
      "Modo de geração de quadros")
PC_TR("Smooth (matched to the refresh)",
      "Suave (ajustado à taxa de atualização)")
PC_TR("Maximum fps",
      "Máximo de fps")
PC_TR("TOTK_FG_PACE. Smooth: the frames shown match the monitor (real ones at the cap divided by the multiplier). Maximum fps: the real frames as without frame generation and the multiplier's generated ones after each, past the refresh rate, with VRR too (mailbox, else immediate, which can tear; the panel may leave VRR; TOTK_FG_MAX_VRR_FIFO=1 keeps VRR's FIFO).",
      "TOTK_FG_PACE. Suave: os quadros exibidos acompanham o monitor (os reais no limite dividido pelo multiplicador). Máximo de fps: os quadros reais como sem geração de quadros e os gerados do multiplicador após cada um, acima da taxa de atualização, também com VRR (mailbox, senão immediate, que pode causar tearing; o monitor pode sair do VRR; TOTK_FG_MAX_VRR_FIFO=1 mantém o FIFO do VRR).")
PC_TR("Smooth keeps the frames shown in step with the monitor's refresh. Maximum fps adds the generated frames to every real frame the PC makes, even past the refresh rate: more frames than the screen shows (the fps readout counts them all), for VRR panels or low latency; Smooth looks steadier on a fixed refresh. With V-sync off the picture can tear.",
      "Suave mantém os quadros exibidos no ritmo da atualização do monitor. Máximo de fps soma os quadros gerados a cada quadro real que o PC produz, mesmo acima da taxa de atualização: mais quadros do que a tela mostra (o contador de fps conta todos), para monitores VRR ou baixa latência; Suave fica mais estável com taxa fixa. Com o V-sync desligado a imagem pode apresentar tearing.")
PC_TR("5.1 (experimental)",
      "5.1 (experimental)")
PC_TR("7.1 (experimental)",
      "7.1 (experimental)")
PC_TR("PC (WASD, mouse)",
      "PC (WASD, mouse)")
PC_TR("+ (menu)",
      "+ (menu)")
PC_TR("Ultrahand",
      "Ultramão")
PC_TR("Fuse",
      "Fusão")
PC_TR("Ascend",
      "Ascensão")
PC_TR("Recall",
      "Reversão")
PC_TR("Autobuild",
      "Autoconstrução")
PC_TR("Menus",
      "Menus")
PC_TR(", %d nits",
      ", %d nits")
PC_TR("Upscaler",
      "Upscaler")
PC_TR("TOTK_DLSS_SR / TOTK_FSR_SR / TOTK_XESS_SR. DLSS (NVIDIA RTX), FSR or XeSS (any GPU) upscales the 3D scene to the output in place of the game's upscaler and its FXAA (DLAA / native AA when Render resolution matches the output).",
      "TOTK_DLSS_SR / TOTK_FSR_SR / TOTK_XESS_SR. DLSS (NVIDIA RTX), FSR ou XeSS (qualquer GPU) amplia a cena 3D para a saída no lugar do upscaler do jogo e do seu FXAA (DLAA / AA nativo quando a Resolução de renderização é igual à da saída).")
PC_TR("FSR (any GPU)",
      "FSR (qualquer GPU)")
PC_TR("TOTK_DLSS_FG / TOTK_FSR_FG. Generated frames between real ones: DLSS x2 on NVIDIA RTX 40 series and newer, x3 and x4 on RTX 50; FSR x2 on any GPU. The frame cap counts the frames shown (at a 30 cap: x2, 60 shown). Adds some input latency.",
      "TOTK_DLSS_FG / TOTK_FSR_FG. Quadros gerados entre os reais: DLSS x2 na NVIDIA RTX série 40 ou mais nova, x3 e x4 na RTX 50; FSR x2 em qualquer GPU. O limite de quadros conta os exibidos (com limite 30: x2, 60 exibidos). Adiciona alguma latência de entrada.")
PC_TR("Console",
      "Console")
PC_TR("cheats",
      "cheats")
PC_TR("Mods",
      "Mods")
PC_TR("Cheats",
      "Cheats")
PC_TR("Preset",
      "Predefinição")
PC_TR("Bilinear",
      "Bilinear")
PC_TR("1x (Switch)",
      "1x (Switch)")
PC_TR("Volume",
      "Volume")
PC_TR("Normal",
      "Normal")
PC_TR("PC (WASD)",
      "PC (WASD)")
PC_TR("Saves",
      "Saves")
PC_TR("°C (Celsius)",
      "°C (Celsius)")
PC_TR("°F (Fahrenheit)",
      "°F (Fahrenheit)")
PC_TR("Vsync",
      "Sincronização vertical")
PC_TR("Shaders",
      "Shaders")
PC_TR("HDR",
      "HDR")
PC_TR("Cutscenes (21:9, 32:9, 48:9)",
      "Cutscenes (21:9, 32:9, 48:9)")
PC_TR("0.1% low",
      "0,1% baixo")
PC_TR("game",
      "jogo")
PC_TR("DLSS model",
      "Modelo do DLSS")
PC_TR("NVIDIA's default",
      "O padrão da NVIDIA")
PC_TR("Preset K",
      "Preset K")
PC_TR("Preset J",
      "Preset J")
PC_TR("TOTK_DLSS_PRESET. The AI model DLSS and DLAA use: NVIDIA's default for each quality, or preset K (sharpest) or J (a little less ghosting, a little more flicker) for all.",
      "TOTK_DLSS_PRESET. O modelo de IA que o DLSS e o DLAA usam: o padrão da NVIDIA para cada qualidade, ou o preset K (o mais nítido) ou o J (um pouco menos de fantasmas, um pouco mais de cintilação) para todas.")
PC_TR("DLSS character trails",
      "Rastros de personagens no DLSS")
PC_TR("Fewest trails",
      "Menos rastros")
PC_TR("Between",
      "Intermediário")
PC_TR("Smoothest (more trails)",
      "Mais suave (mais rastros)")
PC_TR("TOTK_DLSS_TRAILS. Where something moved (Link, animals, a swung weapon), how much DLSS and DLAA trust the new frame over the old ones: fewest trails, or smoother edges with more trails.",
      "TOTK_DLSS_TRAILS. Onde algo se moveu (Link, animais, uma arma sendo brandida), o quanto o DLSS e o DLAA confiam no quadro novo em vez dos anteriores: menos rastros, ou bordas mais suaves com mais rastros.")
PC_TR("Which AI model DLSS and DLAA use. NVIDIA's default picks one for each quality. Preset K is the sharpest; preset J ghosts a little less behind moving things but flickers a little more. Changing it rebuilds DLSS once (a short pause).",
      "Qual modelo de IA o DLSS e o DLAA usam. O padrão da NVIDIA escolhe um para cada qualidade. O preset K é o mais nítido; o preset J deixa um pouco menos de rastro atrás do que se move, mas cintila um pouco mais. Ao trocá-lo, o DLSS é reconstruído uma vez (uma pausa curta).")
PC_TR("Where something moved (Link, animals, a swung weapon), DLSS and DLAA can keep blending in older frames, which leaves faint trails, or trust the new frame, which removes them but can make those edges flicker a little. Fewest trails is the default.",
      "Onde algo se moveu (Link, animais, uma arma sendo brandida), o DLSS e o DLAA podem continuar misturando quadros anteriores, o que deixa rastros leves, ou confiar no quadro novo, o que os remove mas pode fazer essas bordas cintilarem um pouco. Menos rastros é o padrão.")
PC_TR("Safe GPU mode: the graphics driver crashed in the last runs, so shaders now build the slow, safe way",
      "Modo seguro de GPU: o driver gráfico travou nas últimas sessões, então os shaders agora são compilados do jeito lento e seguro")
PC_TR("Safe GPU mode: the graphics driver kept crashing, so shaders are also built unoptimised; the game may run slower until it has run cleanly a few times",
      "Modo seguro de GPU: o driver gráfico continuou travando, então os shaders também são compilados sem otimização; o jogo pode ficar mais lento até rodar sem problemas algumas vezes")
PC_TR("XeSS (any GPU)",
      "XeSS (qualquer GPU)")
PC_TR("Shader compile indicator",
      "Indicador de compilação de shaders")
PC_TR("Bottom right while shaders compile: the percentage on a first start, how many are left in play.",
      "No canto inferior direito enquanto os shaders compilam: a porcentagem na primeira inicialização e quantos faltam durante o jogo.")
PC_TR("Shows when shaders are being compiled (bottom right): a short stutter while it is up is the compile, not your PC. Off hides it in play (a first start still shows its progress); the compiling still happens.",
      "Mostra quando os shaders estão sendo compilados (canto inferior direito): um engasgo curto enquanto ele aparece é a compilação, não o seu PC. Desligado o esconde durante o jogo (a primeira inicialização ainda mostra o progresso); a compilação continua acontecendo.")
PC_TR("Shaders left to compile:",
      "Shaders a compilar:")
PC_TR("Last hitch",
      "Último engasgo")
PC_TR("No hitch yet",
      "Nenhum engasgo ainda")
PC_TR("GPU wait",
      "espera da GPU")
PC_TR("shader builds",
      "compilação de shaders")
PC_TR("texture uploads",
      "envio de texturas")
PC_TR("new images",
      "imagens novas")
PC_TR("file reads",
      "leitura de arquivos")
PC_TR("present (driver / window)",
      "apresentação (driver / janela)")
PC_TR("game CPU",
      "CPU do jogo")
PC_TR("render CPU",
      "CPU de renderização")
PC_TR("s ago",
      "s atrás")
PC_TR("Upscaler:",
      "Upscaler:")
PC_TR("off",
      "desligado")
PC_TR("plain scaling",
      "escala simples")
PC_TR("native",
      "nativo")
PC_TR("Quality",
      "Qualidade")
PC_TR("Balanced",
      "Equilibrado")
PC_TR("Performance",
      "Desempenho")
PC_TR("Ultra Performance",
      "Ultra desempenho")
PC_TR("idle",
      "ocioso")
PC_TR("3D output is",
      "a saída 3D é")
PC_TR("scene",
      "cena")
PC_TR("it failed to start",
      "não conseguiu iniciar")
PC_TR("starting",
      "iniciando")
PC_TR("no 3D view this frame",
      "sem visão 3D neste quadro")
PC_TR("no scene seen",
      "nenhuma cena vista")
