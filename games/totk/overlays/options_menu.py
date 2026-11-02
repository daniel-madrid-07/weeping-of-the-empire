"""The texts of our PC rows in the game's own Options screen: a patcher of the mals set.

    python -m games.totk.overlays.mals --romfs <work>/extracted/v100 --out <work>/overlays

The rows themselves are code (engine/runtime/totk_options_menu.cpp appends them to game::ui::OptionsMgr's list, type
50 and up); the screen clones its template parts for them, so the layout needs no change. What the game looks up for
an entry of type T, in LayoutMsg/SystemOption_00.msbt of the language's Mals archive:
    "10%02d"       T          the label (T_Option_00 of the toggle / selection part)
    "10%02d_%02d"  T, choice  each choice of a selection entry (T_Text_00)
    "%04d"         T          the caption shown while the cursor is on it (T_Caption_00)
mals_patches adds those messages for each of the 14 languages (texts below; a language without its own text gets the
English one) and keeps every shipped message but the few SHIPPED rewrites in place (game row 5's label and caption);
mals.py writes the archives (one set for every message edit),
build.py overlay merges it into the boots' romfs, and engine_dir.py raises the grown archives' RSTB entries.
ENTRIES must match the C table (kRows in totk_options_menu.cpp): the same types and keys, one choice text per value of
the row's value list, in its order (check_c_table refuses the build otherwise).
"""
import os
import re
import struct
from pathlib import Path

from .mals import LANG_KEY, msbt_add, msbt_build, msbt_parse

MSBT = 'LayoutMsg/SystemOption_00.msbt'

# mouse sensitivity (kSens): 0.1 steps up to 2, then 0.2, 0.5 and 1 up to 20
SENS = tuple(f'{i / 10:g}' for i in range(1, 21)) + tuple(f'{i / 10:g}' for i in range(22, 41, 2)) + \
    tuple(f'{i / 2:g}' for i in range(9, 17)) + tuple(str(i) for i in range(9, 21))
# the bow's aiming multiplier (kAimScale): 0.1 .. 1.5 in 0.05 steps, then 1.6 .. 3
AIM = tuple(f'{i / 100:g}' for i in range(10, 151, 5)) + ('1.6', '1.7', '1.8', '1.9', '2', '2.25', '2.5', '2.75', '3')
VOLUME = [{'en': f'{p} %'} for p in range(0, 101, 5)]   # the volume rows' choices (kVolume: 0 .. 1 by 0.05)
# the choice words of an on / off row
OFF = {'en': 'Off', 'es': 'No', 'fr': 'Non', 'de': 'Aus', 'it': 'No', 'nl': 'Uit', 'ru': 'Выкл.', 'ja': 'オフ', 'ko': '끔',
       'zhs': '关', 'zht': '關'}
ON = {'en': 'On', 'es': 'Sí', 'fr': 'Oui', 'de': 'An', 'it': 'Sì', 'nl': 'Aan', 'ru': 'Вкл.', 'ja': 'オン', 'ko': '켬',
      'zhs': '开', 'zht': '開'}

# type -> label, caption, choices (each a dict of language -> text; 'en' is the fallback)
# Choice 0 of the exclusive size and refresh rows: the desktop's own (the "" value)
DESKTOPS = {'en': "The desktop's", 'es': 'La del escritorio', 'fr': 'Celle du bureau', 'de': 'Wie der Desktop',
            'it': 'Quella del desktop', 'nl': 'Van het bureaublad', 'ru': 'Как у рабочего стола', 'ja': 'デスクトップと同じ',
            'ko': '데스크톱과 같음', 'zhs': '与桌面相同', 'zht': '與桌面相同'}

ENTRIES = [
    # the display rows. 109-111 are kDynamic in the C table: their choices are the setting's own (the monitors
    # and modes at start), and only choice 0 has texts here; the others show the setting's names
    dict(type=108, key='display.window_mode',
         label={'en': 'Window mode', 'es': 'Modo de ventana', 'fr': "Mode d'affichage", 'de': 'Fenstermodus',
                'it': 'Modalità finestra', 'nl': 'Venstermodus', 'ru': 'Режим окна', 'ja': 'ウィンドウモード',
                'ko': '창 모드', 'zhs': '窗口模式', 'zht': '視窗模式'},
         caption={'en': 'Windowed, borderless or exclusive fullscreen.\nF11 or Alt+Enter: windowed and back (PC).',
                  'es': 'En ventana, pantalla completa sin bordes o\nexclusiva. F11 o Alt+Intro cambian a\n'
                        'ventana y vuelven (PC).',
                  'fr': "Fenêtré, plein écran sans bordure ou exclusif.\nF11 ou Alt+Entrée : fenêtré et retour (PC).",
                  'de': 'Fenster, randlos oder exklusiv Vollbild.\nF11 / Alt+Enter: Fenster und zurück (PC).',
                  'it': 'Finestra, schermo intero senza bordi o\nesclusivo. F11 o Alt+Invio: finestra e\nritorno (PC).'},
         choices=[{'en': 'Windowed', 'es': 'En ventana', 'fr': 'Fenêtré', 'de': 'Fenster', 'it': 'Finestra',
                   'nl': 'Venster', 'ru': 'В окне', 'ja': 'ウィンドウ', 'ko': '창', 'zhs': '窗口', 'zht': '視窗'},
                  {'en': 'Borderless fullscreen', 'es': 'Completa sin bordes', 'fr': 'Plein écran sans bordure',
                   'de': 'Randloses Vollbild', 'it': 'Intero senza bordi', 'nl': 'Randloos volledig',
                   'ru': 'Полный без рамки', 'ja': 'ボーダーレス全画面', 'ko': '테두리 없는 전체 화면',
                   'zhs': '无边框全屏', 'zht': '無邊框全螢幕'},
                  {'en': 'Exclusive fullscreen', 'es': 'Completa exclusiva', 'fr': 'Plein écran exclusif',
                   'de': 'Exklusives Vollbild', 'it': 'Intero esclusivo', 'nl': 'Exclusief volledig',
                   'ru': 'Эксклюзивный полный', 'ja': '排他的全画面', 'ko': '전용 전체 화면', 'zhs': '独占全屏',
                   'zht': '獨佔全螢幕'}]),
    dict(type=124, key='display.window_size',
         label={'en': 'Window resolution', 'es': 'Resolución de ventana', 'fr': 'Résolution de la fenêtre',
                'de': 'Fensterauflösung', 'it': 'Risoluzione finestra', 'nl': 'Vensterresolutie',
                'ru': 'Разрешение окна', 'ja': 'ウィンドウ解像度', 'ko': '창 해상도', 'zhs': '窗口分辨率',
                'zht': '視窗解析度'},
         caption={'en': "The window's size in windowed mode (PC).\nBorderless and exclusive fill the monitor.",
                  'es': 'El tamaño de la ventana en modo ventana\n(PC). Sin bordes y exclusiva llenan el\nmonitor.',
                  'fr': "La taille de la fenêtre en mode fenêtré\n(PC). Sans bordure et exclusif remplissent\n"
                        "l'écran.",
                  'de': 'Die Fenstergröße im Fenstermodus (PC).\nRandlos und exklusiv füllen den Monitor.',
                  'it': 'La dimensione della finestra in modalità\nfinestra (PC). Senza bordi ed esclusivo\n'
                        'riempiono il monitor.'},
         choices=[{'en': '1280 x 720 (default)', 'es': '1280 x 720 (por defecto)', 'fr': '1280 x 720 (par défaut)',
                   'de': '1280 x 720 (Standard)', 'it': '1280 x 720 (predefinita)', 'nl': '1280 x 720 (standaard)',
                   'ru': '1280 x 720 (по умолчанию)', 'ja': '1280 x 720 (標準)', 'ko': '1280 x 720 (기본)',
                   'zhs': '1280 x 720 (默认)', 'zht': '1280 x 720 (預設)'}]),
    dict(type=109, key='display.monitor',
         label={'en': 'Monitor', 'es': 'Monitor', 'fr': 'Écran', 'de': 'Monitor', 'it': 'Monitor', 'nl': 'Beeldscherm',
                'ru': 'Монитор', 'ja': 'モニター', 'ko': '모니터', 'zhs': '显示器', 'zht': '螢幕'},
         caption={'en': 'Where fullscreen goes and a windowed start opens.\nRemember: the one played on last (PC).',
                  'es': 'Dónde va la pantalla completa y dónde se abre\nla ventana. Recordar: el último usado (PC).',
                  'fr': "L'écran du plein écran et de la fenêtre au\ndémarrage. Mémoriser : le dernier (PC).",
                  'de': 'Wohin Vollbild geht und wo das Fenster\nstartet. Merken: der zuletzt genutzte\nMonitor (PC).',
                  'it': "Dove va lo schermo intero e dove si apre la\nfinestra. Ricorda: l'ultimo usato (PC)."},
         choices=[{'en': 'Remember', 'es': 'Recordar', 'fr': 'Mémoriser', 'de': 'Merken', 'it': 'Ricorda',
                   'nl': 'Onthouden', 'ru': 'Запомнить', 'ja': '記憶する', 'ko': '기억', 'zhs': '记住', 'zht': '記住'}]),
    dict(type=110, key='display.exclusive_size',
         label={'en': 'Fullscreen resolution', 'es': 'Resolución de pantalla completa',
                'fr': 'Résolution plein écran', 'de': 'Vollbildauflösung', 'it': 'Risoluzione a schermo intero',
                'nl': 'Resolutie volledig scherm', 'ru': 'Разрешение полного экрана', 'ja': '全画面解像度',
                'ko': '전체 화면 해상도', 'zhs': '全屏分辨率', 'zht': '全螢幕解析度'},
         caption={'en': "The monitor's mode in exclusive fullscreen.\nBorderless and windowed keep the desktop's (PC).",
                  'es': 'El modo del monitor en pantalla completa\nexclusiva. Sin bordes y en ventana usan el\n'
                        'del escritorio (PC).',
                  'fr': "Le mode de l'écran en plein écran exclusif.\nSans bordure et fenêtré gardent celui du\n"
                        'bureau (PC).',
                  'de': 'Der Monitormodus im exklusiven Vollbild.\nRandlos und Fenster nutzen den Desktop\n(PC).',
                  'it': "La modalità del monitor a schermo intero\nesclusivo. Senza bordi e finestra usano\n"
                        'quella del desktop (PC).'},
         choices=[DESKTOPS]),
    dict(type=111, key='display.exclusive_refresh',
         label={'en': 'Refresh rate', 'es': 'Frecuencia de refresco', 'fr': 'Fréquence de rafraîchissement',
                'de': 'Bildwiederholrate', 'it': 'Frequenza di aggiornamento', 'nl': 'Verversingssnelheid',
                'ru': 'Частота обновления', 'ja': 'リフレッシュレート', 'ko': '주사율', 'zhs': '刷新率', 'zht': '更新率'},
         caption={'en': 'The refresh rate in exclusive fullscreen; a rate\nthe resolution lacks gives the nearest below '
                        '(PC).',
                  'es': 'La frecuencia en pantalla completa exclusiva;\nsi la resolución no la tiene, se usa la más\n'
                        'cercana por debajo (PC).',
                  'fr': "La fréquence en plein écran exclusif ; si la\nrésolution ne l'a pas, la plus proche en\n"
                        'dessous (PC).',
                  'de': 'Die Bildrate im exklusiven Vollbild; fehlt\nsie der Auflösung, die nächstniedrigere\n(PC).',
                  'it': "La frequenza a schermo intero esclusivo; se\nla risoluzione non l'ha, la più vicina sotto\n"
                        '(PC).'},
         choices=[DESKTOPS]),
    dict(type=50, key='display.fps_cap',
         label={'en': 'Frame rate limit', 'es': 'Límite de FPS', 'fr': "Limite d'images par seconde",
                'de': 'Bildratenbegrenzung', 'it': 'Limite di FPS', 'nl': 'FPS-limiet', 'ru': 'Ограничение FPS',
                'ja': 'フレームレート上限', 'ko': '프레임 레이트 제한', 'zhs': '帧率上限', 'zht': '影格率上限'},
         caption={'en': 'The highest frame rate the game runs at (PC).\n'
                        'Higher costs performance. Applies at once.',
                  'es': 'La tasa de fotogramas máxima del juego (PC).\n'
                        'Más alta cuesta rendimiento. Se aplica al\n'
                        'momento.',
                  'fr': "La fréquence d'images maximale du jeu (PC).\n"
                        'Plus haute coûte en performances. Effet\n'
                        'immédiat.',
                  'de': 'Die höchste Bildrate des Spiels (PC).\n'
                        'Höher kostet Leistung. Sofort wirksam.',
                  'it': 'La frequenza di fotogrammi massima del gioco\n'
                        '(PC). Più alta costa prestazioni. Effetto\n'
                        'immediato.'},
         choices=[{'en': '30'}, {'en': '40'}, {'en': '45'}, {'en': '60'}, {'en': '75'}, {'en': '90'}, {'en': '120'},
                  {'en': '144'}, {'en': '165'}, {'en': '180'}, {'en': '200'}, {'en': '240'},
                  {'en': 'Unlocked', 'es': 'Sin límite', 'fr': 'Illimitée', 'de': 'Unbegrenzt', 'it': 'Illimitato',
                   'nl': 'Onbeperkt', 'ru': 'Без ограничения', 'ja': '無制限', 'ko': '무제한', 'zhs': '无限制',
                   'zht': '無限制'},
                  # steady: unlocked, held at the highest rate the scene keeps (whole refreshes of the monitor)
                  {'en': 'Unlocked, steady', 'es': 'Sin límite, estable', 'fr': 'Illimitée, stable',
                   'de': 'Unbegrenzt, stabil', 'it': 'Illimitato, stabile', 'nl': 'Onbeperkt, stabiel',
                   'ru': 'Без лимита, стабильно', 'ja': '無制限（安定）', 'ko': '무제한 (안정)', 'zhs': '无限制（稳定）',
                   'zht': '無限制（穩定）'}]),
    # the fps readout: pc_overlay.cpp draws it, F3 shows or hides it
    dict(type=125, key='display.fps_readout',
         label={'en': 'FPS counter', 'es': 'Contador de FPS', 'fr': 'Compteur de FPS', 'de': 'FPS-Anzeige',
                'it': 'Contatore FPS', 'nl': 'FPS-teller', 'ru': 'Счётчик FPS', 'ja': 'FPS表示', 'ko': 'FPS 표시',
                'zhs': '帧率显示', 'zht': '影格率顯示'},
         caption={'en': 'Frames per second over the game (PC). F3\nshows or hides it. Detailed adds the frame\n'
                        'time, the 1% low and the worst frame.',
                  'es': 'Imágenes por segundo sobre el juego (PC).\nF3 lo muestra u oculta. Detallado añade el\n'
                        'tiempo de fotograma, 1% mínimo y el peor.',
                  'fr': "Images par seconde sur le jeu (PC). F3\nl'affiche ou le masque. Détaillé ajoute le\n"
                        "temps d'image, le 1 % bas et le pire.",
                  'de': 'Bilder pro Sekunde über dem Spiel (PC). F3\nzeigt oder verbirgt sie. Detailliert: dazu\n'
                        'Frametime, 1%-Tief, schlechtester Frame.',
                  'it': 'Fotogrammi al secondo sul gioco (PC). F3\nlo mostra o nasconde. Dettagliato aggiunge\n'
                        'tempo fotogramma, 1% minimo e il peggiore.'},
         choices=[OFF,
                  {'en': 'Minimal', 'es': 'Mínimo', 'fr': 'Minimal', 'de': 'Minimal', 'it': 'Minimo', 'nl': 'Minimaal',
                   'ru': 'Минимум', 'ja': 'シンプル', 'ko': '간단히', 'zhs': '简洁', 'zht': '簡潔'},
                  {'en': 'Detailed', 'es': 'Detallado', 'fr': 'Détaillé', 'de': 'Detailliert', 'it': 'Dettagliato',
                   'nl': 'Uitgebreid', 'ru': 'Подробно', 'ja': '詳細', 'ko': '자세히', 'zhs': '详细', 'zht': '詳細'}]),
    dict(type=126, key='display.fps_position',
         label={'en': 'FPS counter position', 'es': 'Posición del contador', 'fr': 'Position du compteur',
                'de': 'Position der FPS-Anzeige', 'it': 'Posizione del contatore', 'nl': 'Positie FPS-teller',
                'ru': 'Положение счётчика', 'ja': 'FPS表示の位置', 'ko': 'FPS 표시 위치', 'zhs': '帧率显示位置',
                'zht': '影格率顯示位置'},
         caption={'en': 'The corner the FPS counter sits in (PC).\nTop right keeps clear of the hearts and\nthe map.',
                  'es': 'La esquina del contador de FPS (PC).\nArriba a la derecha no tapa los corazones\nni el mapa.',
                  'fr': 'Le coin du compteur de FPS (PC). En haut à\ndroite, il ne cache ni les cœurs ni la\ncarte.',
                  'de': 'Die Ecke der FPS-Anzeige (PC). Oben rechts\nverdeckt sie weder Herzen noch Karte.',
                  'it': "L'angolo del contatore FPS (PC). In alto a\ndestra non copre i cuori né la mappa."},
         choices=[{'en': 'Top right', 'es': 'Arriba a la derecha', 'fr': 'En haut à droite', 'de': 'Oben rechts',
                   'it': 'In alto a destra', 'nl': 'Rechtsboven', 'ru': 'Вверху справа', 'ja': '右上',
                   'ko': '오른쪽 위', 'zhs': '右上', 'zht': '右上'},
                  {'en': 'Top left', 'es': 'Arriba a la izquierda', 'fr': 'En haut à gauche', 'de': 'Oben links',
                   'it': 'In alto a sinistra', 'nl': 'Linksboven', 'ru': 'Вверху слева', 'ja': '左上', 'ko': '왼쪽 위',
                   'zhs': '左上', 'zht': '左上'},
                  {'en': 'Bottom right', 'es': 'Abajo a la derecha', 'fr': 'En bas à droite', 'de': 'Unten rechts',
                   'it': 'In basso a destra', 'nl': 'Rechtsonder', 'ru': 'Внизу справа', 'ja': '右下',
                   'ko': '오른쪽 아래', 'zhs': '右下', 'zht': '右下'},
                  {'en': 'Bottom left', 'es': 'Abajo a la izquierda', 'fr': 'En bas à gauche', 'de': 'Unten links',
                   'it': 'In basso a sinistra', 'nl': 'Linksonder', 'ru': 'Внизу слева', 'ja': '左下',
                   'ko': '왼쪽 아래', 'zhs': '左下', 'zht': '左下'}]),
    dict(type=127, key='display.fps_size',
         label={'en': 'FPS counter size', 'es': 'Tamaño del contador', 'fr': 'Taille du compteur',
                'de': 'Größe der FPS-Anzeige', 'it': 'Dimensione del contatore', 'nl': 'Grootte FPS-teller',
                'ru': 'Размер счётчика', 'ja': 'FPS表示の大きさ', 'ko': 'FPS 표시 크기', 'zhs': '帧率显示大小',
                'zht': '影格率顯示大小'},
         caption={'en': 'The size of the FPS counter (PC). It grows\nwith the window, from 1080p to 4K.',
                  'es': 'El tamaño del contador de FPS (PC). Crece\ncon la ventana, de 1080p a 4K.',
                  'fr': 'La taille du compteur de FPS (PC). Elle\nsuit la fenêtre, de 1080p à la 4K.',
                  'de': 'Die Größe der FPS-Anzeige (PC). Sie wächst\nmit dem Fenster, von 1080p bis 4K.',
                  'it': 'La dimensione del contatore FPS (PC).\nCresce con la finestra, da 1080p a 4K.'},
         choices=[{'en': 'Small', 'es': 'Pequeño', 'fr': 'Petit', 'de': 'Klein', 'it': 'Piccolo', 'nl': 'Klein',
                   'ru': 'Маленький', 'ja': '小', 'ko': '작게', 'zhs': '小', 'zht': '小'},
                  {'en': 'Medium', 'es': 'Mediano', 'fr': 'Moyen', 'de': 'Mittel', 'it': 'Medio', 'nl': 'Middel',
                   'ru': 'Средний', 'ja': '中', 'ko': '보통', 'zhs': '中', 'zht': '中'},
                  {'en': 'Large', 'es': 'Grande', 'fr': 'Grand', 'de': 'Groß', 'it': 'Grande', 'nl': 'Groot',
                   'ru': 'Большой', 'ja': '大', 'ko': '크게', 'zhs': '大', 'zht': '大'}]),
    dict(type=51, key='graphics.preset',
         label={'en': 'Graphics preset', 'es': 'Calidad gráfica', 'fr': 'Préréglage graphique',
                'de': 'Grafikvoreinstellung', 'it': 'Preimpostazione grafica', 'nl': 'Grafische voorinstelling',
                'ru': 'Набор настроек графики', 'ja': 'グラフィックプリセット', 'ko': '그래픽 프리셋', 'zhs': '图形预设',
                'zht': '圖形預設'},
         caption={'en': 'Sets render resolution, shadows and detail\n'
                        'together (PC). Higher costs performance;\n'
                        'part of it applies at the next start.',
                  'es': 'Ajusta a la vez la resolución de render,\n'
                        'las sombras y el detalle (PC). Más alta cuesta\n'
                        'rendimiento. Parte se aplica al reiniciar.',
                  'fr': 'Règle la résolution de rendu, les ombres\n'
                        'et les détails (PC). Plus haut coûte en\n'
                        'performances ; une partie au redémarrage.',
                  'de': 'Stellt Renderauflösung, Schatten und\n'
                        'Details gemeinsam ein (PC). Höher kostet\n'
                        'Leistung; ein Teil gilt ab Neustart.',
                  'it': 'Imposta insieme risoluzione di rendering,\n'
                        'ombre e dettagli (PC). Più alta costa\n'
                        'prestazioni; in parte al prossimo avvio.'},
         choices=[{'en': 'Low', 'es': 'Baja', 'fr': 'Basse', 'de': 'Niedrig', 'it': 'Bassa', 'nl': 'Laag',
                   'ru': 'Низкие', 'ja': '低', 'ko': '낮음', 'zhs': '低', 'zht': '低'},
                  {'en': 'Medium', 'es': 'Media', 'fr': 'Moyenne', 'de': 'Mittel', 'it': 'Media', 'nl': 'Gemiddeld',
                   'ru': 'Средние', 'ja': '中', 'ko': '보통', 'zhs': '中', 'zht': '中'},
                  {'en': 'High', 'es': 'Alta', 'fr': 'Haute', 'de': 'Hoch', 'it': 'Alta', 'nl': 'Hoog',
                   'ru': 'Высокие', 'ja': '高', 'ko': '높음', 'zhs': '高', 'zht': '高'},
                  {'en': 'Ultra'},
                  {'en': 'Ultra+'},
                  {'en': 'Custom', 'es': 'Personalizada', 'fr': 'Personnalisé', 'de': 'Benutzerdefiniert',
                   'it': 'Personalizzata', 'nl': 'Aangepast', 'ru': 'Особые', 'ja': 'カスタム', 'ko': '사용자 지정',
                   'zhs': '自定义', 'zht': '自訂'}]),
    dict(type=52, key='graphics.render_scale',
         label={'en': 'Render resolution', 'es': 'Resolución de render', 'fr': 'Résolution de rendu',
                'de': 'Renderauflösung', 'it': 'Risoluzione di rendering', 'nl': 'Renderresolutie',
                'ru': 'Разрешение рендеринга', 'ja': 'レンダリング解像度', 'ko': '렌더링 해상도', 'zhs': '渲染分辨率',
                'zht': '渲染解析度'},
         caption={'en': 'The resolution the 3D scene is drawn at,\n'
                        "relative to the game's own (PC). Higher costs\n"
                        'performance. Applies at once.',
                  'es': 'La resolución a la que se dibuja la escena 3D,\n'
                        'respecto a la del juego (PC). Más alta cuesta\n'
                        'rendimiento. Se aplica al momento.',
                  'fr': 'La résolution de la scène 3D, par rapport à\n'
                        'celle du jeu (PC). Plus haute coûte en\n'
                        "performances. S'applique aussitôt.",
                  'de': 'Die Auflösung der 3D-Szene, bezogen auf\n'
                        'die des Spiels (PC). Höher kostet\n'
                        'Leistung. Gilt sofort.',
                  'it': 'La risoluzione della scena 3D rispetto a\n'
                        'quella del gioco (PC). Più alta costa\n'
                        'prestazioni. Si applica subito.'},
         choices=[{'en': '67 %'}, {'en': '80 %'}, {'en': '90 %'},
                  {'en': '100 % (native)', 'es': '100 % (nativa)', 'fr': '100 % (native)', 'de': '100 % (nativ)',
                   'it': '100 % (nativa)', 'nl': '100 % (native)', 'ru': '100 % (исходное)', 'ja': '100 %（ネイティブ）',
                   'ko': '100 % (기본)', 'zhs': '100 %（原生）', 'zht': '100 %（原生）'},
                  {'en': '150 %'}, {'en': '200 %'},
                  {'en': 'Window height', 'es': 'Altura de la ventana', 'fr': 'Hauteur de la fenêtre',
                   'de': 'Fensterhöhe', 'it': 'Altezza della finestra', 'nl': 'Vensterhoogte', 'ru': 'Высота окна',
                   'ja': 'ウィンドウの高さ', 'ko': '창 높이', 'zhs': '窗口高度', 'zht': '視窗高度'}]),
    dict(type=64, key='graphics.dlss',   # dlss's (pc_dlss.cpp): DLSS or FSR 3.1 (ffx.c), live; pc-ui's texts 11-02
         label={'en': 'Upscaler', 'es': 'Escalador', 'fr': "Mise à l'échelle", 'de': 'Hochskalierung', 'it': 'Upscaler',
                'nl': 'Opschaling', 'ru': 'Масштабирование', 'ja': 'アップスケーラー', 'ko': '업스케일러', 'zhs': '超分辨率',
                'zht': '超解析度'},
         caption={'en': 'Temporal upscaling of the 3D scene (PC).\n'
                        'DLSS: NVIDIA RTX only; FSR: any GPU.\n'
                        'Costs some GPU. Applies at once.',
                  'es': 'Escalado temporal de la escena 3D (PC).\n'
                        'DLSS: solo NVIDIA RTX; FSR: cualquier GPU.\n'
                        'Cuesta algo de GPU. Se aplica al momento.',
                  'fr': "Mise à l'échelle temporelle de la 3D (PC).\n"
                        'DLSS : NVIDIA RTX seulement ; FSR : tout\n'
                        "GPU. Coûte du GPU. S'applique aussitôt.",
                  'de': 'Zeitliche Hochskalierung der 3D-Szene\n'
                        '(PC). DLSS: nur NVIDIA RTX; FSR: jede GPU.\n'
                        'Kostet GPU. Gilt sofort.',
                  'it': 'Upscaling temporale della scena 3D (PC).\n'
                        'DLSS: solo NVIDIA RTX; FSR: ogni GPU.\n'
                        'Costa GPU. Si applica subito.'},
         choices=[OFF, {'en': 'DLSS (NVIDIA RTX)', 'es': 'DLSS (NVIDIA RTX)'}, {'en': 'DLAA (NVIDIA RTX)', 'es': 'DLAA (NVIDIA RTX)'},
                  {'en': 'FSR (any GPU)', 'es': 'FSR (cualquier GPU)', 'fr': 'FSR (tout GPU)', 'de': 'FSR (jede GPU)',
                   'it': 'FSR (ogni GPU)', 'nl': 'FSR (elke GPU)', 'ru': 'FSR (любой GPU)', 'ja': 'FSR（全GPU）',
                   'ko': 'FSR (모든 GPU)', 'zhs': 'FSR（任意 GPU）', 'zht': 'FSR（任何 GPU）'}]),
    dict(type=65, key='graphics.frame_gen',   # dlss's (pc_dlss.cpp): DLSS-G or FSR 3.1 (ffx.c), live; pc-ui's texts 11-02
         label={'en': 'Frame generation', 'es': 'Generación de fotogramas', 'fr': "Génération d'images",
                'de': 'Bildgenerierung', 'it': 'Generazione fotogrammi', 'nl': 'Framegeneratie', 'ru': 'Генерация кадров',
                'ja': 'フレーム生成', 'ko': '프레임 생성', 'zhs': '帧生成', 'zht': '影格生成'},
         caption={'en': 'Generated frames between the real ones\n'
                        '(PC); adds some input latency. DLSS x2:\n'
                        'RTX 40+, x3/x4: RTX 50; FSR x2: any GPU.',
                  'es': 'Fotogramas generados entre los reales (PC);\n'
                        'añade algo de retraso. DLSS x2: RTX 40+,\n'
                        'x3/x4: RTX 50; FSR x2: cualquier GPU.',
                  'fr': 'Images générées entre les vraies (PC) ;\n'
                        'ajoute de la latence. DLSS x2 : RTX 40+,\n'
                        'x3/x4 : RTX 50 ; FSR x2 : tout GPU.',
                  'de': 'Erzeugte Bilder zwischen den echten (PC);\n'
                        'etwas mehr Latenz. DLSS x2: RTX 40+,\n'
                        'x3/x4: RTX 50; FSR x2: jede GPU.',
                  'it': 'Fotogrammi generati tra quelli veri (PC);\n'
                        'più latenza. DLSS x2: RTX 40+, x3/x4:\n'
                        'RTX 50; FSR x2: ogni GPU.'},
         choices=[OFF, {'en': 'DLSS x2', 'es': 'DLSS x2'}, {'en': 'DLSS x3', 'es': 'DLSS x3'},
                  {'en': 'DLSS x4', 'es': 'DLSS x4'}, {'en': 'FSR x2', 'es': 'FSR x2'}]),
    dict(type=67, key='graphics.fov',  # the live row (totk_engine.c); the engine adds the degrees
         label={'en': 'Field of view', 'es': 'Campo de visión', 'fr': 'Champ de vision', 'de': 'Sichtfeld',
                'it': 'Campo visivo', 'nl': 'Gezichtsveld', 'ru': 'Поле зрения', 'ja': '視野角', 'ko': '시야각',
                'zhs': '视野', 'zht': '視野'},
         caption={'en': 'Wider shows more of the world on screen (PC)\n'
                        'and costs some performance. Aiming and\n'
                        'cutscenes keep their proportion.',
                  'es': 'Un campo más amplio muestra más del mundo (PC)\n'
                        'y cuesta algo de rendimiento. Al apuntar y en\n'
                        'las cinemáticas se conserva la proporción.',
                  'fr': "Plus large : plus du monde à l'écran (PC),\n"
                        'coûte un peu en performances. La visée et\n'
                        'les cinématiques gardent leurs proportions.',
                  'de': 'Breiter zeigt mehr Welt und kostet etwas\n'
                        'Leistung (PC). Zielen und Zwischen-\n'
                        'sequenzen behalten ihre Proportionen.',
                  'it': 'Più ampio mostra più mondo (PC) e costa un\n'
                        "po' di prestazioni. Mira e filmati mantengono\n"
                        'le proporzioni.'},
         choices=[{'en': f'{v:.2f}x' + (' (Normal)' if v == 1 else ''),
                   **{k: f'{v:.2f}x'.replace('.', ',') + (f' ({n})' if v == 1 else '')
                      for k, n in (('es', 'Normal'), ('fr', 'Normal'), ('de', 'Normal'), ('it', 'Normale'),
                                   ('nl', 'Normaal'), ('ru', 'Обычное'))},
                   **({'ja': '1.00x（通常）', 'ko': '1.00x (보통)', 'zhs': '1.00x（正常）', 'zht': '1.00x（正常）'}
                      if v == 1 else {})}
                  for v in (0.8, 0.9, 1, 1.1, 1.2, 1.3, 1.4, 1.5)]),
    dict(type=68, key='graphics.aspect_ratio',  # the ultrawide (totk_engine.c, TOTK_WIDE), restart
         label={'en': 'Aspect ratio', 'es': 'Relación de aspecto', 'fr': "Format d'image", 'de': 'Seitenverhältnis',
                'it': 'Proporzioni', 'nl': 'Beeldverhouding', 'ru': 'Соотношение сторон', 'ja': 'アスペクト比',
                'ko': '화면 비율', 'zhs': '宽高比', 'zht': '長寬比'},
         caption={'en': 'On a 16:10, 21:9 or 32:9 screen the game\n'
                        'fills it, HUD placed for it (PC). Wider\n'
                        'costs performance. Applies at next start.',
                  'es': 'En pantallas 16:10, 21:9 o 32:9 el juego\n'
                        'la llena, HUD adaptado (PC). Más ancha\n'
                        'cuesta rendimiento. Al reiniciar el juego.',
                  'fr': 'Sur un écran 16:10, 21:9 ou 32:9, le jeu\n'
                        'le remplit, HUD adapté (PC). Plus large\n'
                        'coûte en performances. Au redémarrage.',
                  'de': 'Füllt 16:10-, 21:9- und 32:9-Bildschirme,\n'
                        'HUD angepasst (PC). Breiter kostet\n'
                        'Leistung. Gilt ab dem nächsten Start.',
                  'it': 'Su schermi 16:10, 21:9 o 32:9 il gioco li\n'
                        'riempie, HUD adattato (PC). Più largo\n'
                        'costa prestazioni. Al prossimo avvio.'},
         choices=[{'en': '16:9'},
                  {'en': "Window's shape", 'es': 'Forma de la ventana', 'fr': 'Forme de la fenêtre',
                   'de': 'Fensterform', 'it': 'Forma della finestra', 'nl': 'Vorm van het venster', 'ru': 'Форма окна',
                   'ja': 'ウィンドウの形', 'ko': '창 모양', 'zhs': '窗口形状', 'zht': '視窗形狀'},
                  {'en': '16:10'}, {'en': '21:9'}, {'en': '32:9'}]),
    dict(type=128, key='graphics.hud_layout',   # the ultrawide HUD at the edges or in the middle 16:9 (totk_engine.c)
         label={'en': 'HUD position', 'es': 'Posición del HUD', 'fr': 'Position du HUD', 'de': 'HUD-Position',
                'it': 'Posizione HUD', 'nl': 'HUD-positie', 'ru': 'Положение HUD', 'ja': 'HUDの位置', 'ko': 'HUD 위치',
                'zhs': 'HUD 位置', 'zht': 'HUD 位置'},
         caption={'en': 'On a 21:9 or 32:9 screen: the HUD at the\n'
                        'edges, or in the middle 16:9 (PC).\n'
                        'Applies at the next start.',
                  'es': 'En una pantalla 21:9 o 32:9: el HUD en los\n'
                        'bordes o en el centro 16:9 (PC).\n'
                        'Se aplica al reiniciar el juego.',
                  'fr': 'Sur un écran 21:9 ou 32:9 : le HUD sur les\n'
                        'bords ou au centre 16:9 (PC).\n'
                        'Au prochain démarrage du jeu.',
                  'de': 'Bei 21:9 und 32:9: HUD an den Rändern\n'
                        'oder in der 16:9-Mitte (PC).\n'
                        'Gilt ab dem nächsten Spielstart.',
                  'it': 'Su uno schermo 21:9 o 32:9: HUD ai bordi\n'
                        'o al centro 16:9 (PC).\n'
                        'Al prossimo avvio del gioco.'},
         choices=[{'en': 'Screen edges', 'es': 'Bordes de la pantalla', 'fr': "Bords de l'écran", 'de': 'Bildschirmränder',
                   'it': 'Bordi dello schermo', 'nl': 'Schermranden', 'ru': 'Края экрана', 'ja': '画面の端',
                   'ko': '화면 가장자리', 'zhs': '屏幕边缘', 'zht': '螢幕邊緣'},
                  {'en': 'Middle 16:9', 'es': 'Centro 16:9', 'fr': 'Centre 16:9', 'de': '16:9-Mitte', 'it': 'Centro 16:9',
                   'nl': '16:9-midden', 'ru': 'Центр 16:9', 'ja': '中央16:9', 'ko': '가운데 16:9', 'zhs': '中间 16:9',
                   'zht': '中間 16:9'}]),
    dict(type=130, key='graphics.cutscene_aspect',   # ultrawide cutscenes wide or 16:9 (totk_engine.c, totk_hooks.c), live
         label={'en': 'Cutscenes', 'es': 'Cinemáticas', 'fr': 'Cinématiques', 'de': 'Zwischensequenzen',
                'it': 'Filmati', 'nl': 'Tussenfilmpjes', 'ru': 'Ролики', 'ja': 'カットシーン', 'ko': '컷신',
                'zhs': '过场动画', 'zht': '過場動畫'},
         caption={'en': 'On a 21:9 or 32:9 screen: cutscenes wide,\n'
                        'or the original 16:9 framing with bars\n'
                        '(PC). Videos are always 16:9.',
                  'es': 'En una pantalla 21:9 o 32:9: cinemáticas\n'
                        'panorámicas o con el encuadre 16:9 original\n'
                        'y bandas (PC). Los vídeos siempre son 16:9.',
                  'fr': 'Sur un écran 21:9 ou 32:9 : cinématiques\n'
                        "larges, ou cadrage 16:9 d'origine avec\n"
                        'bandes (PC). Les vidéos restent en 16:9.',
                  'de': 'Bei 21:9 und 32:9: Sequenzen breit oder\n'
                        'im originalen 16:9 mit Balken (PC).\n'
                        'Videos sind immer 16:9.',
                  'it': 'Su uno schermo 21:9 o 32:9: filmati larghi\n'
                        "o l'inquadratura 16:9 originale con bande\n"
                        '(PC). I video sono sempre 16:9.'},
         choices=[{'en': 'Wide', 'es': 'Panorámicas', 'fr': 'Larges', 'de': 'Breit', 'it': 'Larghi', 'nl': 'Breed',
                   'ru': 'Широкие', 'ja': 'ワイド', 'ko': '와이드', 'zhs': '宽屏', 'zht': '寬螢幕'},
                  {'en': '16:9'}]),
    dict(type=117, key='graphics.native_ui',  # the (textures.c TOTK_OUT_SCALE, pc_settings.cpp), live (live-settings 11-02)
         label={'en': 'Native-resolution interface', 'es': 'Interfaz a resolución nativa',
                'fr': 'Interface en résolution native', 'de': 'Oberfläche in nativer Auflösung',
                'it': 'Interfaccia a risoluzione nativa', 'nl': 'Interface op native resolutie',
                'ru': 'Интерфейс в родном разрешении', 'ja': 'ネイティブ解像度のUI', 'ko': '네이티브 해상도 UI',
                'zhs': '原生分辨率界面', 'zht': '原生解析度介面'},
         caption={'en': 'Above 1080p, menus, text and the HUD are\n'
                        "drawn at the screen's resolution, sharper\n"
                        '(PC). Applies at once.',
                  'es': 'Por encima de 1080p, menús, textos y HUD se\n'
                        'dibujan a la resolución de la pantalla, más\n'
                        'nítidos (PC). Se aplica al momento.',
                  'fr': 'Au-delà de 1080p, menus, textes et HUD sont\n'
                        "affichés à la résolution de l'écran, plus\n"
                        "nets (PC). S'applique aussitôt.",
                  'de': 'Über 1080p: Menüs, Texte und HUD in\n'
                        'Bildschirmauflösung, schärfer (PC).\n'
                        'Gilt sofort.',
                  'it': 'Oltre 1080p, menu, testi e HUD sono disegnati\n'
                        'alla risoluzione dello schermo, più nitidi\n'
                        '(PC). Si applica subito.'},
         choices=[ON, OFF]),   # "" (the default) = on, "0" = off
    dict(type=53, key='controls.mouse_sensitivity',
         label={'en': 'Mouse sensitivity', 'es': 'Sensibilidad del ratón', 'fr': 'Sensibilité de la souris',
                'de': 'Mausempfindlichkeit', 'it': 'Sensibilità del mouse', 'nl': 'Muisgevoeligheid',
                'ru': 'Чувствительность мыши', 'ja': 'マウス感度', 'ko': '마우스 감도', 'zhs': '鼠标灵敏度',
                'zht': '滑鼠靈敏度'},
         caption={'en': 'How fast the mouse turns the camera\nand aims (PC).',
                  'es': 'La rapidez con la que el ratón gira la cámara\ny apunta (PC).',
                  'fr': 'La vitesse à laquelle la souris tourne la\ncaméra et vise (PC).',
                  'de': 'Wie schnell die Maus die Kamera dreht\nund zielt (PC).',
                  'it': 'La velocità con cui il mouse ruota la\ntelecamera e mira (PC).'},
         choices=[{'en': v, **{k: v.replace('.', ',') for k in ('es', 'fr', 'de', 'it', 'nl', 'ru')}}
                  for v in SENS]),
    dict(type=116, key='controls.mouse_aim_scale',
         label={'en': 'Aiming sensitivity (bow)', 'es': 'Sensibilidad al apuntar (arco)',
                'fr': 'Sensibilité de visée (arc)', 'de': 'Zielempfindlichkeit (Bogen)', 'it': 'Sensibilità di mira (arco)',
                'nl': 'Richtgevoeligheid (boog)', 'ru': 'Чувствительность прицела (лук)', 'ja': '弓の照準感度',
                'ko': '활 조준 감도', 'zhs': '瞄准灵敏度（弓）', 'zht': '瞄準靈敏度（弓）'},
         caption={'en': 'Scales the mouse while the bow is drawn.\n1 = the same as the camera (PC).',
                  'es': 'Multiplica la sensibilidad del ratón mientras\ntensas el arco. 1 = igual que al mover la\n'
                        'cámara (PC).',
                  'fr': "Multiplie la sensibilité de la souris quand\nl'arc est bandé. 1 = comme la caméra (PC).",
                  'de': 'Skaliert die Maus bei gespanntem Bogen.\n1 = wie bei der Kamera (PC).',
                  'it': "Moltiplica la sensibilità del mouse mentre\ntendi l'arco. 1 = come la telecamera (PC)."},
         choices=[{'en': v, **{k: v.replace('.', ',') for k in ('es', 'fr', 'de', 'it', 'nl', 'ru')}} for v in AIM]),
    dict(type=119, key='controls.key_min_hold_ms',  # the, live: the W / A / D tap slide
         label={'en': 'Minimum movement key press', 'es': 'Pulsación mínima de movimiento (teclado)',
                'fr': 'Appui minimal des touches de déplacement', 'de': 'Mindestdruck der Bewegungstasten',
                'it': 'Pressione minima dei tasti di movimento', 'nl': 'Minimale druk bewegingstoets',
                'ru': 'Мин. нажатие клавиш движения', 'ja': '移動キーの最短入力', 'ko': '이동 키 최소 입력',
                'zhs': '移动键最短按下', 'zht': '移動鍵最短按下'},
         caption={'en': 'How long a quick tap of WASD lasts at least, so\nLink takes a step instead of sliding.\n'
                        '0 = off (PC).',
                  'es': 'Lo que dura como mínimo un toque rápido de WASD,\npara que Link dé un paso en vez de deslizarse.\n'
                        '0 = desactivado (PC).',
                  'fr': "Durée minimale d'un appui bref sur ZQSD, pour\nque Link fasse un pas au lieu de glisser.\n"
                        '0 = désactivé (PC).',
                  'de': 'So lange dauert ein kurzer WASD-Druck\nmindestens: Link macht einen Schritt statt\n'
                        'zu rutschen. 0 = aus (PC).',
                  'it': 'La durata minima di un tocco rapido di WASD,\nperché Link faccia un passo invece di\n'
                        'scivolare. 0 = disattivato (PC).'},
         choices=[OFF] + [{'en': f'{v} ms'} for v in range(10, 251, 10)]),
    dict(type=54, key='controls.glyphs',
         label={'en': 'Button icons', 'es': 'Iconos de botones', 'fr': 'Icônes des boutons', 'de': 'Tastensymbole',
                'it': 'Icone dei pulsanti', 'nl': 'Knoppictogrammen', 'ru': 'Значки кнопок', 'ja': 'ボタンアイコン',
                'ko': '버튼 아이콘', 'zhs': '按键图标', 'zht': '按鍵圖示'},
         caption={'en': 'Which controller the button pictures show (PC).\nApplies at once.',
                  'es': 'Qué mando muestran los iconos de los botones\n(PC). Se aplica al momento.',
                  'fr': 'La manette que montrent les boutons (PC).\nEffet immédiat.',
                  'de': 'Welchen Controller die Tastenbilder\nzeigen (PC). Sofort wirksam.',
                  'it': 'Il controller mostrato dalle immagini dei\npulsanti (PC). Effetto immediato.'},
         choices=[{'en': 'Automatic', 'es': 'Automático', 'fr': 'Automatique', 'de': 'Automatisch',
                   'it': 'Automatico', 'nl': 'Automatisch', 'ru': 'Автоматически', 'ja': '自動', 'ko': '자동',
                   'zhs': '自动', 'zht': '自動'},
                  {'en': 'Nintendo Switch'}, {'en': 'Xbox'}, {'en': 'PlayStation'},
                  {'en': 'Keyboard and mouse', 'es': 'Teclado y ratón', 'fr': 'Clavier et souris',
                   'de': 'Tastatur und Maus', 'it': 'Tastiera e mouse', 'nl': 'Toetsenbord en muis',
                   'ru': 'Клавиатура и мышь', 'ja': 'キーボードとマウス', 'ko': '키보드와 마우스', 'zhs': '键盘和鼠标',
                   'zht': '鍵盤和滑鼠'}]),
    dict(type=55, key='game.fast_travel',
         label={'en': 'Fast-travel speed', 'es': 'Velocidad del viaje rápido', 'fr': 'Vitesse du voyage rapide',
                'de': 'Schnellreise-Tempo', 'it': 'Velocità del viaggio rapido', 'nl': 'Snelheid van snel reizen',
                'ru': 'Скорость быстрого перемещения', 'ja': 'ワープの速さ', 'ko': '순간 이동 속도', 'zhs': '传送速度',
                'zht': '傳送速度'},
         caption={'en': "Fast: Link's departure when travelling\nis about 2.5 s shorter (PC).\n"
                        'Applies the next time the game starts.',
                  'es': 'Rápida: la partida de Link al viajar dura\nunos 2,5 s menos (PC).\n'
                        'Se aplica al volver a iniciar el juego.',
                  'fr': 'Rapide : le départ de Link en voyage\ndure 2,5 s de moins (PC).\n'
                        "S'applique au prochain démarrage du jeu.",
                  'de': 'Schnell: Links Abreise beim Reisen\nist etwa 2,5 s kürzer (PC).\n'
                        'Gilt ab dem nächsten Spielstart.',
                  'it': 'Rapida: la partenza di Link in viaggio\ndura 2,5 s in meno (PC).\n'
                        'Si applica al prossimo avvio del gioco.'},
         choices=[{'en': 'Normal', 'es': 'Normal', 'fr': 'Normale', 'de': 'Normal', 'it': 'Normale', 'nl': 'Normaal',
                   'ru': 'Обычная', 'ja': '通常', 'ko': '보통', 'zhs': '正常', 'zht': '正常'},
                  {'en': 'Fast', 'es': 'Rápida', 'fr': 'Rapide', 'de': 'Schnell', 'it': 'Rapida', 'nl': 'Snel',
                   'ru': 'Быстрая', 'ja': '速い', 'ko': '빠름', 'zhs': '快速', 'zht': '快速'}]),
    dict(type=56, key='controls.menu_cursor',  # the setting (totk_menu_cursor.cpp), live
         label={'en': 'Mouse cursor in menus', 'es': 'Cursor del ratón en menús', 'fr': 'Curseur dans les menus',
                'de': 'Mauszeiger in Menüs', 'it': 'Cursore del mouse nei menu', 'nl': "Muiscursor in menu's",
                'ru': 'Курсор мыши в меню', 'ja': 'メニューのマウスカーソル', 'ko': '메뉴 마우스 커서', 'zhs': '菜单中的鼠标指针',
                'zht': '選單中的滑鼠指標'},
         caption={'en': 'Show the mouse pointer in menus: it selects\nwhat it points at; left click confirms, right\n'
                        'click goes back (PC).',
                  'es': 'Muestra el puntero del ratón en los menús:\nelige lo que señala; clic izquierdo confirma,\n'
                        'clic derecho vuelve atrás (PC).',
                  'fr': "Affiche le pointeur de la souris dans les\nmenus : il sélectionne ce qu'il vise ;\n"
                        'clic gauche valide, clic droit revient (PC).',
                  'de': 'Zeigt den Mauszeiger in Menüs: Er wählt,\nworauf er zeigt; Linksklick bestätigt,\n'
                        'Rechtsklick geht zurück (PC).',
                  'it': 'Mostra il puntatore del mouse nei menu:\nseleziona ciò che indica; clic sinistro\n'
                        'conferma, clic destro torna indietro (PC).'},
         choices=[{'en': 'Off', 'es': 'No', 'fr': 'Non', 'de': 'Aus', 'it': 'No', 'nl': 'Uit', 'ru': 'Выкл.',
                   'ja': 'オフ', 'ko': '끔', 'zhs': '关', 'zht': '關'},
                  {'en': 'On', 'es': 'Sí', 'fr': 'Oui', 'de': 'An', 'it': 'Sì', 'nl': 'Aan', 'ru': 'Вкл.',
                   'ja': 'オン', 'ko': '켬', 'zhs': '开', 'zht': '開'}]),
    dict(type=57, key='display.vsync',   # present's
         label={'en': 'Vertical sync', 'es': 'Sincronización vertical', 'fr': 'Synchronisation verticale',
                'de': 'Vertikale Synchronisation', 'it': 'Sincronizzazione verticale', 'nl': 'Verticale synchronisatie',
                'ru': 'Вертикальная синхронизация', 'ja': '垂直同期', 'ko': '수직 동기화', 'zhs': '垂直同步', 'zht': '垂直同步'},
         caption={'en': 'Off removes the wait for the screen: lowest\nlatency, but the picture can tear (PC).',
                  'es': 'Desactivada quita la espera a la pantalla:\nmenos latencia, pero la imagen puede\n'
                        'cortarse (PC).',
                  'fr': "Désactivée, le jeu n'attend plus l'écran :\nlatence minimale, mais l'image peut se\n"
                        'déchirer (PC).',
                  'de': 'Aus: kein Warten auf den Bildschirm,\ngeringste Latenz, aber das Bild kann\nreißen (PC).',
                  'it': "Disattivata non attende lo schermo: latenza\nminima, ma l'immagine può strapparsi (PC)."},
         choices=[ON, OFF]),
    dict(type=58, key='display.vrr',   # present's
         label={'en': 'Variable refresh rate', 'es': 'Frecuencia variable (VRR)', 'fr': 'Fréquence variable (VRR)',
                'de': 'Variable Bildwiederholrate', 'it': 'Frequenza variabile (VRR)', 'nl': 'Variabele verversing (VRR)',
                'ru': 'Переменная частота (VRR)', 'ja': '可変リフレッシュレート', 'ko': '가변 주사율', 'zhs': '可变刷新率',
                'zht': '可變更新率'},
         caption={'en': "For a G-Sync or FreeSync monitor: the screen\nfollows the game's frame rate (PC).",
                  'es': 'Para monitores G-Sync o FreeSync: la pantalla\nsigue la tasa de fotogramas del juego (PC).',
                  'fr': "Pour un écran G-Sync ou FreeSync : l'écran\nsuit la fréquence d'images du jeu (PC).",
                  'de': 'Für G-Sync- oder FreeSync-Monitore: Der\nBildschirm folgt der Bildrate (PC).',
                  'it': 'Per monitor G-Sync o FreeSync: lo schermo\nsegue la frequenza del gioco (PC).'},
         choices=[OFF, ON]),
    dict(type=60, key='controls.gyro_scale',
         label={'en': 'Motion sensitivity', 'es': 'Sensibilidad del giroscopio', 'fr': 'Sensibilité du gyroscope',
                'de': 'Bewegungsempfindlichkeit', 'it': 'Sensibilità del giroscopio', 'nl': 'Bewegingsgevoeligheid',
                'ru': 'Чувствительность гироскопа', 'ja': 'ジャイロ感度', 'ko': '자이로 감도', 'zhs': '体感灵敏度',
                'zht': '體感靈敏度'},
         caption={'en': 'Pad motion controls only: how far they turn\nthe camera and aim. The mouse is not affected\n(PC).',
                  'es': 'Solo para el giroscopio de un mando: cuánto\ngiran la cámara y la mira. No cambia el\nratón (PC).',
                  'fr': 'Gyroscope de la manette seulement : rotation\nde la caméra et de la visée. Pas la souris\n(PC).',
                  'de': 'Nur Bewegungssteuerung des Controllers:\nDrehung von Kamera und Zielen. Nicht die\nMaus (PC).',
                  'it': 'Solo giroscopio del controller: quanto\nruotano telecamera e mira. Non il mouse (PC).'},
         choices=[{'en': v, **{k: v.replace('.', ',') for k in ('es', 'fr', 'de', 'it', 'nl', 'ru')}}
                  for v in ('0.25', '0.5', '0.75', '1', '1.25', '1.5', '1.75', '2', '2.5', '3')]),
    dict(type=61, key='display.hdr',   # present's
         label={'en': 'HDR output', 'es': 'Salida HDR', 'fr': 'Sortie HDR', 'de': 'HDR-Ausgabe', 'it': 'Uscita HDR',
                'nl': 'HDR-uitvoer', 'ru': 'Вывод HDR', 'ja': 'HDR出力', 'ko': 'HDR 출력', 'zhs': 'HDR 输出', 'zht': 'HDR 輸出'},
         caption={'en': 'Needs Windows HDR on for this monitor;\notherwise the game stays SDR (PC).',
                  'es': 'Requiere HDR activado en Windows para este\nmonitor; si no, el juego sigue en SDR (PC).',
                  'fr': "Demande le HDR de Windows activé pour cet\nécran ; sinon le jeu reste en SDR (PC).",
                  'de': 'Braucht Windows-HDR für diesen Monitor;\nsonst bleibt das Spiel SDR (PC).',
                  'it': 'Richiede HDR di Windows attivo per questo\nmonitor; altrimenti il gioco resta SDR (PC).'},
         choices=[OFF, {'en': 'HDR10'}, {'en': 'scRGB'}]),
    dict(type=62, key='display.hdr_peak',   # present's
         label={'en': 'HDR peak brightness', 'es': 'Brillo máximo HDR', 'fr': 'Luminosité max. HDR',
                'de': 'HDR-Spitzenhelligkeit', 'it': 'Luminosità massima HDR', 'nl': 'HDR-piekhelderheid',
                'ru': 'Пиковая яркость HDR', 'ja': 'HDRピーク輝度', 'ko': 'HDR 최대 밝기', 'zhs': 'HDR 峰值亮度',
                'zht': 'HDR 峰值亮度'},
         caption={'en': "The display's peak brightness (PC).",
                  'es': 'El brillo máximo de la pantalla (PC).',
                  'fr': "La luminosité maximale de l'écran (PC).",
                  'de': 'Die Spitzenhelligkeit des Displays (PC).',
                  'it': 'La luminosità massima dello schermo (PC).'},
         choices=[{'en': f'{v} nits'} for v in (400, 500, 600, 700, 800, 1000, 1200, 1500, 2000, 2500, 3000, 4000)]),
    dict(type=63, key='display.hdr_paper',   # present's
         label={'en': 'HDR paper white', 'es': 'Blanco de papel HDR', 'fr': 'Blanc de référence HDR',
                'de': 'HDR-Papierweiß', 'it': 'Bianco carta HDR', 'nl': 'HDR-papierwit', 'ru': 'Уровень белого HDR',
                'ja': 'HDRペーパーホワイト', 'ko': 'HDR 페이퍼 화이트', 'zhs': 'HDR 纸白', 'zht': 'HDR 紙白'},
         caption={'en': "How bright the game's white mid-tones are (PC).",
                  'es': 'Lo brillantes que son los blancos medios del\njuego (PC).',
                  'fr': 'La luminosité des blancs et tons moyens du\njeu (PC).',
                  'de': 'Wie hell die weißen Mitteltöne des Spiels\nsind (PC).',
                  'it': 'Quanto sono luminosi i bianchi e i toni medi\ndel gioco (PC).'},
         choices=[{'en': f'{v} nits'} for v in (80, 100, 120, 140, 160, 180, 200, 220, 250, 300, 400, 500)]),
    dict(type=114, key='game.pause_unfocused',  # the row (hle_hid.cpp), live; hle_oe.cpp's hold
         label={'en': 'Pause when unfocused', 'es': 'Pausar al perder el foco', 'fr': 'Pause hors premier plan',
                'de': 'Pause im Hintergrund', 'it': 'Pausa senza focus', 'nl': 'Pauzeren zonder focus',
                'ru': 'Пауза вне фокуса', 'ja': '非アクティブ時に一時停止', 'ko': '포커스를 잃으면 일시 정지',
                'zhs': '失去焦点时暂停', 'zht': '失去焦點時暫停'},
         caption={'en': 'The game stops, picture and sound, while its\nwindow is not in front, and goes on where it\nwas (PC).',
                  'es': 'El juego se detiene, imagen y sonido, mientras\nsu ventana no está delante, y sigue donde\nestaba (PC).',
                  'fr': "Le jeu s'arrête, image et son, tant que sa\nfenêtre n'est pas devant, puis reprend (PC).",
                  'de': 'Das Spiel hält an, Bild und Ton, solange\nsein Fenster nicht vorn ist, und läuft\ndann weiter (PC).',
                  'it': 'Il gioco si ferma, immagine e audio, finché\nla finestra non è davanti, poi riprende (PC).'},
         choices=[OFF, ON]),
    dict(type=115, key='audio.mute_unfocused',  # pc_settings.cpp's row, live; hle_audio.cpp
         label={'en': 'Mute in the background', 'es': 'Silenciar en segundo plano', 'fr': 'Muet en arrière-plan',
                'de': 'Stumm im Hintergrund', 'it': 'Muto in background', 'nl': 'Dempen op de achtergrond',
                'ru': 'Без звука в фоне', 'ja': 'バックグラウンドで消音', 'ko': '백그라운드에서 음소거',
                'zhs': '后台时静音', 'zht': '背景時靜音'},
         caption={'en': 'No sound while the game window is not in\nfront; the game keeps running (PC).',
                  'es': 'Sin sonido mientras la ventana del juego no\nestá delante; el juego sigue (PC).',
                  'fr': "Pas de son tant que la fenêtre du jeu n'est\npas devant ; le jeu continue (PC).",
                  'de': 'Kein Ton, solange das Spielfenster nicht\nvorn ist; das Spiel läuft weiter (PC).',
                  'it': 'Niente audio finché la finestra del gioco\nnon è davanti; il gioco continua (PC).'},
         choices=[OFF, ON]),
    # the PC audio rows F10 had alone, all live; the volumes in 5 % steps
    dict(type=120, key='audio.volume',   # pc_settings.cpp's master gain
         label={'en': 'Volume', 'es': 'Volumen', 'fr': 'Volume', 'de': 'Lautstärke', 'it': 'Volume', 'nl': 'Volume',
                'ru': 'Громкость', 'ja': '音量', 'ko': '음량', 'zhs': '音量', 'zht': '音量'},
         caption={'en': "All of the game's sound, on top of the\nWindows volume (PC).",
                  'es': 'Todo el sonido del juego, además del\nvolumen de Windows (PC).',
                  'fr': 'Tout le son du jeu, en plus du volume\nde Windows (PC).',
                  'de': 'Der gesamte Ton des Spiels, zusätzlich\nzur Windows-Lautstärke (PC).',
                  'it': "Tutto l'audio del gioco, oltre al volume\ndi Windows (PC)."},
         choices=VOLUME),
    dict(type=121, key='audio.music',   # hle_audio.cpp's music sub mix
         label={'en': 'Music', 'es': 'Música', 'fr': 'Musique', 'de': 'Musik', 'it': 'Musica', 'nl': 'Muziek',
                'ru': 'Музыка', 'ja': '音楽', 'ko': '음악', 'zhs': '音乐', 'zht': '音樂'},
         caption={'en': 'The music alone (PC).', 'es': 'Solo la música (PC).', 'fr': 'La musique seule (PC).',
                  'de': 'Nur die Musik (PC).', 'it': 'Solo la musica (PC).'},
         choices=VOLUME),
    dict(type=122, key='audio.effects',   # hle_audio.cpp's effects sub mix
         label={'en': 'Effects', 'es': 'Efectos', 'fr': 'Effets', 'de': 'Effekte', 'it': 'Effetti', 'nl': 'Effecten',
                'ru': 'Эффекты', 'ja': '効果音', 'ko': '효과음', 'zhs': '音效', 'zht': '音效'},
         caption={'en': 'The sound effects alone (PC).', 'es': 'Solo los efectos de sonido (PC).',
                  'fr': 'Les effets sonores seuls (PC).', 'de': 'Nur die Soundeffekte (PC).',
                  'it': 'Solo gli effetti sonori (PC).'},
         choices=VOLUME),
    dict(type=123, key='audio.follow_device',   # hle_audio.cpp: to the new default device, a lost one recovered
         label={'en': 'Follow the Windows device', 'es': 'Seguir el dispositivo de Windows',
                'fr': 'Suivre le périphérique Windows', 'de': 'Windows-Gerät folgen',
                'it': 'Segui il dispositivo di Windows', 'nl': 'Windows-apparaat volgen',
                'ru': 'Следовать за устройством Windows', 'ja': 'Windowsの出力先に従う', 'ko': 'Windows 장치 따르기',
                'zhs': '跟随 Windows 设备', 'zht': '跟隨 Windows 裝置'},
         caption={'en': 'The sound moves to headphones plugged in\nor out, and comes back after a lost device,\n'
                        'without a restart (PC).',
                  'es': 'El sonido pasa a los auriculares al\nconectarlos o quitarlos, y vuelve tras perder\n'
                        'el dispositivo, sin reiniciar (PC).',
                  'fr': 'Le son passe au casque branché ou retiré,\net revient après une perte du\n'
                        'périphérique, sans redémarrer (PC).',
                  'de': 'Der Ton wechselt auf eingesteckte oder\nentfernte Kopfhörer, kehrt nach verlorenem\n'
                        'Gerät zurück, ohne Neustart (PC).',
                  'it': "L'audio passa alle cuffie collegate o tolte\ne torna dopo un dispositivo perso, senza\n"
                        'riavviare (PC).'},
         choices=[ON, OFF]),
    # F10's graphics rows a player looks for first (v0.1.8, pc-ui), in the Graphics section
    dict(type=140, key='graphics.dynamic_resolution',   # pc_settings.cpp, live (its frame-rate aim too, since 11-02)
         label={'en': 'Dynamic resolution', 'es': 'Resolución dinámica', 'fr': 'Résolution dynamique',
                'de': 'Dynamische Auflösung', 'it': 'Risoluzione dinamica', 'nl': 'Dynamische resolutie',
                'ru': 'Динамическое разрешение', 'ja': '動的解像度', 'ko': '동적 해상도', 'zhs': '动态分辨率',
                'zht': '動態解析度'},
         caption={'en': 'Lowers the 3D resolution only when the GPU\nis short of time, never below the floor\n'
                        'chosen (PC).',
                  'es': 'Baja la resolución 3D solo cuando a la GPU\nle falta tiempo, nunca por debajo del\n'
                        'mínimo elegido (PC).',
                  'fr': 'Baisse la résolution 3D seulement quand\nle GPU manque de temps, jamais sous le\n'
                        'minimum choisi (PC).',
                  'de': 'Senkt die 3D-Auflösung nur, wenn der GPU\ndie Zeit fehlt, nie unter das gewählte\n'
                        'Minimum (PC).',
                  'it': 'Abbassa la risoluzione 3D solo quando la\nGPU è a corto di tempo, mai sotto il\n'
                        'minimo scelto (PC).'},
         choices=[{'en': 'Off (fixed)', 'es': 'No (fija)', 'fr': 'Non (fixe)', 'de': 'Aus (fest)', 'it': 'No (fissa)',
                   'nl': 'Uit (vast)', 'ru': 'Выкл. (фикс.)', 'ja': 'オフ（固定）', 'ko': '끔 (고정)', 'zhs': '关（固定）',
                   'zht': '關（固定）'},
                  {'en': 'Down to 90 %', 'es': 'Hasta el 90 %', 'fr': "Jusqu'à 90 %", 'de': 'Bis 90 %',
                   'it': 'Fino al 90 %', 'nl': 'Tot 90 %', 'ru': 'До 90 %', 'ja': '最低 90 %', 'ko': '최소 90 %',
                   'zhs': '最低 90 %', 'zht': '最低 90 %'},
                  {'en': 'Down to 75 %', 'es': 'Hasta el 75 %', 'fr': "Jusqu'à 75 %", 'de': 'Bis 75 %',
                   'it': 'Fino al 75 %', 'nl': 'Tot 75 %', 'ru': 'До 75 %', 'ja': '最低 75 %', 'ko': '최소 75 %',
                   'zhs': '最低 75 %', 'zht': '最低 75 %'},
                  {'en': 'Down to 50 %', 'es': 'Hasta el 50 %', 'fr': "Jusqu'à 50 %", 'de': 'Bis 50 %',
                   'it': 'Fino al 50 %', 'nl': 'Tot 50 %', 'ru': 'До 50 %', 'ja': '最低 50 %', 'ko': '최소 50 %',
                   'zhs': '最低 50 %', 'zht': '最低 50 %'},
                  {'en': "The game's own", 'es': 'La del juego', 'fr': 'Celle du jeu', 'de': 'Wie im Spiel',
                   'it': 'Quella del gioco', 'nl': 'Die van het spel', 'ru': 'Как в игре', 'ja': 'ゲーム標準',
                   'ko': '게임 기본', 'zhs': '游戏默认', 'zht': '遊戲預設'}]),
    dict(type=141, key='graphics.shadow_fixed',  # the live row (totk_engine.c); patched main only
         label={'en': 'Sun shadow map', 'es': 'Mapa de sombras del sol', 'fr': "Carte d'ombres du soleil",
                'de': 'Sonnen-Schattenkarte', 'it': 'Mappa ombre del sole', 'nl': 'Schaduwkaart van de zon',
                'ru': 'Карта теней солнца', 'ja': '太陽の影マップ', 'ko': '태양 그림자 맵', 'zhs': '太阳阴影贴图',
                'zht': '太陽陰影貼圖'},
         caption={'en': "Fixed keeps it at the largest size the game\npicks: steady, sharper leaf shadows. Costs\n"
                        'some GPU (PC).',
                  'es': 'Fijo lo deja en el tamaño mayor que usa el\njuego: sombras de hojas estables y más\n'
                        'nítidas. Cuesta algo de GPU (PC).',
                  'fr': 'Fixe la garde à la plus grande taille du\njeu : ombres des feuilles stables et plus\n'
                        'nettes. Coûte un peu de GPU (PC).',
                  'de': 'Fest: immer die größte Größe des Spiels,\nruhige, schärfere Blattschatten. Kostet\n'
                        'etwas GPU (PC).',
                  'it': 'Fissa la tiene alla dimensione massima del\ngioco: ombre delle foglie stabili e più\n'
                        'nitide. Costa un po\' di GPU (PC).'},
         choices=[{'en': "The game's", 'es': 'La del juego', 'fr': 'Celle du jeu', 'de': 'Wie im Spiel',
                   'it': 'Quella del gioco', 'nl': 'Van het spel', 'ru': 'Как в игре', 'ja': 'ゲーム標準',
                   'ko': '게임 기본', 'zhs': '游戏默认', 'zht': '遊戲預設'},
                  {'en': 'Fixed', 'es': 'Fijo', 'fr': 'Fixe', 'de': 'Fest', 'it': 'Fissa', 'nl': 'Vast',
                   'ru': 'Фиксированная', 'ja': '固定', 'ko': '고정', 'zhs': '固定', 'zht': '固定'}]),
    dict(type=142, key='graphics.soft_shadows',  # shadows, live (exec.c knob variants, live-settings 11-02)
         label={'en': 'Soft shadows', 'es': 'Sombras suaves', 'fr': 'Ombres douces', 'de': 'Weiche Schatten',
                'it': 'Ombre morbide', 'nl': 'Zachte schaduwen', 'ru': 'Мягкие тени', 'ja': 'ソフトシャドウ',
                'ko': '부드러운 그림자', 'zhs': '柔和阴影', 'zht': '柔和陰影'},
         caption={'en': "Sun shadow edges widen with the distance\nto what casts them. Off is the game's look.\n"
                        'Applies at once (PC).',
                  'es': 'Los bordes de las sombras se ensanchan con\nla distancia. No: el aspecto del juego.\n'
                        'Se aplica al momento (PC).',
                  'fr': "Les bords des ombres s'élargissent avec la\ndistance. Non : l'aspect du jeu.\n"
                        "S'applique aussitôt (PC).",
                  'de': 'Schattenkanten werden mit dem Abstand\nbreiter. Aus: der Look des Spiels.\n'
                        'Gilt sofort (PC).',
                  'it': 'I bordi delle ombre si allargano con la\ndistanza. No: l\'aspetto del gioco.\n'
                        'Si applica subito (PC).'},
         choices=[OFF,
                  {'en': 'Soft', 'es': 'Suaves', 'fr': 'Douces', 'de': 'Weich', 'it': 'Morbide', 'nl': 'Zacht',
                   'ru': 'Мягкие', 'ja': '弱', 'ko': '약하게', 'zhs': '弱', 'zht': '弱'},
                  {'en': 'Softer', 'es': 'Más suaves', 'fr': 'Plus douces', 'de': 'Weicher', 'it': 'Più morbide',
                   'nl': 'Zachter', 'ru': 'Мягче', 'ja': '中', 'ko': '중간', 'zhs': '中', 'zht': '中'},
                  {'en': 'Softest', 'es': 'Muy suaves', 'fr': 'Très douces', 'de': 'Am weichsten',
                   'it': 'Molto morbide', 'nl': 'Zachtst', 'ru': 'Очень мягкие', 'ja': '強', 'ko': '강하게',
                   'zhs': '强', 'zht': '強'}]),
    dict(type=143, key='graphics.contact_shadows',  # shadows, live (contact.c, live-settings 11-02)
         label={'en': 'Contact shadows', 'es': 'Sombras de contacto', 'fr': 'Ombres de contact',
                'de': 'Kontaktschatten', 'it': 'Ombre di contatto', 'nl': 'Contactschaduwen', 'ru': 'Контактные тени',
                'ja': 'コンタクトシャドウ', 'ko': '접촉 그림자', 'zhs': '接触阴影', 'zht': '接觸陰影'},
         caption={'en': "Small sun shadows at feet, under stones\nand in grass. Off is the game's look; costs\n"
                        'some GPU. Applies at once (PC).',
                  'es': 'Pequeñas sombras a los pies, bajo piedras\ny en la hierba. No: el aspecto del juego.\n'
                        'Cuesta GPU. Se aplica al momento (PC).',
                  'fr': "Petites ombres aux pieds, sous les pierres\net dans l'herbe. Non : l'aspect du jeu.\n"
                        "Coûte du GPU. S'applique aussitôt (PC).",
                  'de': 'Kleine Schatten an Füßen, unter Steinen,\nim Gras. Aus: der Look des Spiels.\n'
                        'Kostet GPU. Gilt sofort (PC).',
                  'it': "Piccole ombre ai piedi, sotto i sassi e\nnell'erba. No: l'aspetto del gioco.\n"
                        'Costa GPU. Si applica subito (PC).'},
         choices=[OFF,
                  {'en': 'Subtle', 'es': 'Sutiles', 'fr': 'Subtiles', 'de': 'Dezent', 'it': 'Leggere',
                   'nl': 'Subtiel', 'ru': 'Слабые', 'ja': '弱', 'ko': '약하게', 'zhs': '弱', 'zht': '弱'},
                  {'en': 'Medium', 'es': 'Medias', 'fr': 'Moyennes', 'de': 'Mittel', 'it': 'Medie',
                   'nl': 'Gemiddeld', 'ru': 'Средние', 'ja': '中', 'ko': '중간', 'zhs': '中', 'zht': '中'},
                  {'en': 'Strong', 'es': 'Fuertes', 'fr': 'Fortes', 'de': 'Stark', 'it': 'Forti', 'nl': 'Sterk',
                   'ru': 'Сильные', 'ja': '強', 'ko': '강하게', 'zhs': '强', 'zht': '強'}]),
    dict(type=131, key='graphics.shader_wait',  # pc_window.cpp's row, live
         label={'en': 'Shader wait', 'es': 'Espera de sombreadores', 'fr': 'Attente des shaders',
                'de': 'Shader-Wartezeit', 'it': 'Attesa shader', 'nl': 'Shader-wachttijd', 'ru': 'Ожидание шейдеров',
                'ja': 'シェーダー待機', 'ko': '셰이더 대기', 'zhs': '着色器等待', 'zht': '著色器等待'},
         caption={'en': 'A new shader can freeze the picture while\nit builds. Limit: after 0.1 s some objects\n'
                        'appear a moment later. Always: no limit (PC).',
                  'es': 'Un sombreador nuevo puede congelar la\nimagen. Límite: tras 0,1 s algunos objetos\n'
                        'aparecen un instante después (PC).',
                  'fr': "Un nouveau shader peut figer l'image.\nLimite : après 0,1 s, des objets\n"
                        'apparaissent un instant plus tard (PC).',
                  'de': 'Ein neuer Shader kann das Bild einfrieren.\nLimit: nach 0,1 s erscheinen manche\n'
                        'Objekte einen Moment später (PC).',
                  'it': "Uno shader nuovo può bloccare l'immagine.\nLimite: dopo 0,1 s alcuni oggetti\n"
                        'appaiono un attimo dopo (PC).'},
         choices=[{'en': 'Limit to 0.1 s', 'es': 'Límite 0,1 s', 'fr': 'Limite 0,1 s', 'de': 'Limit 0,1 s',
                   'it': 'Limite 0,1 s', 'nl': 'Limiet 0,1 s', 'ru': 'Предел 0,1 с', 'ja': '0.1秒まで', 'ko': '0.1초 제한',
                   'zhs': '最多0.1秒', 'zht': '最多0.1秒'},
                  {'en': 'Always wait', 'es': 'Esperar siempre', 'fr': 'Toujours attendre', 'de': 'Immer warten',
                   'it': 'Attendi sempre', 'nl': 'Altijd wachten', 'ru': 'Всегда ждать', 'ja': '常に待つ',
                   'ko': '항상 대기', 'zhs': '总是等待', 'zht': '總是等待'}]),
    dict(type=129, key='game.temperature_unit',  # pc_settings.cpp's row, live (the tempunit patch, v0.1.8)
         label={'en': 'Temperature unit', 'es': 'Unidad de temperatura', 'fr': 'Unité de température',
                'de': 'Temperatureinheit', 'it': 'Unità di temperatura', 'nl': 'Temperatuureenheid',
                'ru': 'Единицы температуры', 'ja': '温度の単位', 'ko': '온도 단위', 'zhs': '温度单位', 'zht': '溫度單位'},
         caption={'en': "The Purah Pad map's temperature. Automatic:\nthe game's (°F in English (US)) (PC).",
                  'es': 'La temperatura del mapa de la tableta.\nAutomática: la del juego (°F en inglés de\n'
                        'EE. UU.) (PC).',
                  'fr': 'La température de la carte de la\ntablette. Auto : celle du jeu (°F en\nanglais US) (PC).',
                  'de': 'Die Temperatur der Karte des Tablets.\nAutomatisch: die des Spiels (°F in\nEnglisch (US)) (PC).',
                  'it': 'La temperatura della mappa del tablet.\nAutomatica: quella del gioco (°F in\ninglese USA) (PC).'},
         choices=[{'en': 'Automatic', 'es': 'Automática', 'fr': 'Automatique', 'de': 'Automatisch', 'it': 'Automatica',
                   'nl': 'Automatisch', 'ru': 'Автоматически', 'ja': '自動', 'ko': '자동', 'zhs': '自动', 'zht': '自動'},
                  {'en': '°C', 'es': '°C'}, {'en': '°F', 'es': '°F'}]),   # (the same everywhere)
    dict(type=66, key='controls.gameplay',  # the (pc_input.cpp), live; the key block's first row
         label={'en': 'Keyboard controls', 'es': 'Controles de teclado', 'fr': 'Commandes clavier',
                'de': 'Tastatursteuerung', 'it': 'Comandi da tastiera', 'nl': 'Toetsenbordbediening',
                'ru': 'Управление с клавиатуры', 'ja': 'キーボード操作', 'ko': '키보드 조작', 'zhs': '键盘操作',
                'zht': '鍵盤操作'},
         caption={'en': "Console layout: keys act as the controller's\nbuttons. PC: each action has its own key,\n"
                        'set in the rows below (PC).',
                  'es': 'Como en consola: las teclas hacen de botones\ndel mando. PC: cada acción tiene su tecla,\n'
                        'que eliges abajo (PC).',
                  'fr': 'Comme sur console : les touches imitent les\nboutons. PC : chaque action a sa touche,\n'
                        'choisie ci-dessous (PC).',
                  'de': 'Wie auf Konsole: Tasten wirken als\nController-Knöpfe. PC: jede Aktion hat\n'
                        'ihre Taste, unten wählbar (PC).',
                  'it': 'Come su console: i tasti fanno da pulsanti\ndel controller. PC: ogni azione ha il suo\n'
                        'tasto, scelto qui sotto (PC).'},
         choices=[{'en': 'Console layout', 'es': 'Como en consola', 'fr': 'Comme sur console', 'de': 'Wie auf Konsole',
                   'it': 'Come su console', 'nl': 'Zoals op console', 'ru': 'Как на консоли', 'ja': 'コンソール配置',
                   'ko': '콘솔 배치', 'zhs': '主机布局', 'zht': '主機配置'},
                  {'en': 'PC', 'ru': 'ПК'}]),  # the default from the package that flips it
]

# The key rows (kKeyRows of the C file: one per shared action, after row 66). Labels: the game's own words where it has
# them (CtrlGuide_00 0008 jump, 0068 guard; ActionGuide_00 0000_0 attack, 0018 crouch, 0015 lock on;
# ActorMsg/PouchContent Obj_*_Name the six abilities; AppMenuOverlay_00 Map; GetWindow_00 T_PouchGuide_00 inventory),
# ours where the game's is a context's prompt (its "Dash" is the horse's: "Más rápido", "Guizza"; "Tieni premuto" for the bow). The choice pane
# shows the key's name, written by the engine; KEY_PLACEHOLDER is the one choice text the screen finds first.
KEY_ENTRIES = [
    dict(type=87, action='move_up', label={'en': 'Move forward', 'es': 'Avanzar', 'fr': 'Avancer', 'de': 'Vorwärts',
                                           'it': 'Avanti', 'nl': 'Vooruit', 'ru': 'Вперёд', 'ja': '前へ移動',
                                           'ko': '앞으로 이동', 'zhs': '向前移动', 'zht': '向前移動'}),
    dict(type=88, action='move_down', label={'en': 'Move back', 'es': 'Retroceder', 'fr': 'Reculer',
                                             'de': 'Rückwärts', 'it': 'Indietro', 'nl': 'Achteruit', 'ru': 'Назад',
                                             'ja': '後ろへ移動', 'ko': '뒤로 이동', 'zhs': '向后移动', 'zht': '向後移動'}),
    dict(type=89, action='move_left', label={'en': 'Move left', 'es': 'Ir a la izquierda', 'fr': 'Aller à gauche',
                                             'de': 'Nach links', 'it': 'A sinistra', 'nl': 'Naar links',
                                             'ru': 'Влево', 'ja': '左へ移動', 'ko': '왼쪽으로 이동', 'zhs': '向左移动',
                                             'zht': '向左移動'}),
    dict(type=90, action='move_right', label={'en': 'Move right', 'es': 'Ir a la derecha', 'fr': 'Aller à droite',
                                              'de': 'Nach rechts', 'it': 'A destra', 'nl': 'Naar rechts',
                                              'ru': 'Вправо', 'ja': '右へ移動', 'ko': '오른쪽으로 이동', 'zhs': '向右移动',
                                              'zht': '向右移動'}),
    dict(type=70, action='jump', label={'en': 'Jump', 'es': 'Saltar', 'fr': 'Sauter', 'de': 'Springen', 'it': 'Salta',
                                        'nl': 'Springen', 'ru': 'Прыжок', 'ja': 'ジャンプ', 'ko': '점프', 'zhs': '跳跃',
                                        'zht': '跳躍'}),
    dict(type=71, action='sprint', label={'en': 'Sprint', 'es': 'Correr', 'fr': 'Sprinter', 'de': 'Sprinten',
                                          'it': 'Scatto', 'nl': 'Sprinten', 'ru': 'Бег', 'ja': 'ダッシュ', 'ko': '달리기',
                                          'zhs': '冲刺', 'zht': '衝刺'}),
    dict(type=85, action='crouch', label={'en': 'Crouch', 'es': 'Agacharse', 'fr': "S'accroupir", 'de': 'Ducken',
                                          'it': 'Accucciarsi', 'nl': 'Hurken', 'ru': 'Ходьба пригнувшись',
                                          'ja': 'しゃがむ', 'ko': '숙이기', 'zhs': '蹲下', 'zht': '蹲下'}),
    dict(type=91, action='dive', label={'en': 'Dive', 'es': 'Planear', 'fr': 'Chute contrôlée', 'de': 'Sturzflug',
                                        'it': 'Caduta libera', 'nl': 'Duiken', 'ru': 'Пикировать', 'ja': 'ダイビング',
                                        'ko': '다이빙', 'zhs': '俯冲', 'zht': '俯衝'}),
    dict(type=73, action='attack', label={'en': 'Attack', 'es': 'Atacar', 'fr': 'Attaquer', 'de': 'Angreifen',
                                          'it': 'Attacca', 'nl': 'Aanvallen', 'ru': 'Атака', 'ja': '攻撃', 'ko': '공격',
                                          'zhs': '攻击', 'zht': '攻擊'}),
    # 4.26: one key, the pad's ZL: the shield raised and the lock-on (Q's guard row is RETIRED)
    dict(type=86, action='lock_on', label={'en': 'Shield and lock on', 'es': 'Escudo y fijar enemigo',
                                           'fr': 'Bouclier et cibler', 'de': 'Schild und Ziel erfassen',
                                           'it': 'Scudo e aggancio', 'nl': 'Schild en focussen',
                                           'ru': 'Щит и нацеливание', 'ja': '盾と注目', 'ko': '방패와 주목',
                                           'zhs': '盾牌与注视', 'zht': '盾牌與注視'}),
    dict(type=106, action='holster', label={'en': 'Put away / take out weapon', 'es': 'Guardar / sacar el arma',
                                            'fr': "Ranger / sortir l'arme", 'de': 'Waffe wegstecken / ziehen',
                                            'it': "Riponi / estrai l'arma", 'nl': 'Wapen opbergen / pakken',
                                            'ru': 'Убрать / достать оружие', 'ja': '武器をしまう / 構える',
                                            'ko': '무기 넣기 / 꺼내기', 'zhs': '收起 / 拿出武器', 'zht': '收起 / 拿出武器'}),
    # 4.26: F takes the bow out and puts it away (holster moved to Q)
    dict(type=112, action='bow_toggle', label={'en': 'Take out / put away the bow', 'es': 'Sacar / guardar el arco',
                                               'fr': "Sortir / ranger l'arc", 'de': 'Bogen ziehen / wegstecken',
                                               'it': "Estrai / riponi l'arco", 'nl': 'Boog pakken / opbergen',
                                               'ru': 'Достать / убрать лук', 'ja': '弓を構える / しまう',
                                               'ko': '활 꺼내기 / 넣기', 'zhs': '拿出 / 收起弓', 'zht': '拿出 / 收起弓'}),
    dict(type=92, action='throw', label={'en': 'Throw', 'es': 'Lanzar', 'fr': 'Lancer', 'de': 'Werfen',
                                         'it': 'Lancia', 'nl': 'Gooien', 'ru': 'Бросить', 'ja': '投げる', 'ko': '던지기',
                                         'zhs': '投掷', 'zht': '投擲'}),
    dict(type=93, action='weapon_next', label={'en': 'Next weapon', 'es': 'Arma siguiente', 'fr': 'Arme suivante',
                                               'de': 'Nächste Waffe', 'it': 'Arma successiva', 'nl': 'Volgend wapen',
                                               'ru': 'Следующее оружие', 'ja': '次の武器', 'ko': '다음 무기', 'zhs': '下一个武器',
                                               'zht': '下一個武器'}),
    dict(type=94, action='weapon_prev', label={'en': 'Previous weapon', 'es': 'Arma anterior',
                                               'fr': 'Arme précédente', 'de': 'Vorherige Waffe',
                                               'it': 'Arma precedente', 'nl': 'Vorig wapen',
                                               'ru': 'Предыдущее оружие', 'ja': '前の武器', 'ko': '이전 무기', 'zhs': '上一个武器',
                                               'zht': '上一個武器'}),
    dict(type=72, action='interact', label={'en': 'Interact', 'es': 'Interactuar', 'fr': 'Interagir',
                                            'de': 'Interagieren', 'it': 'Interagisci', 'nl': 'Interactie',
                                            'ru': 'Взаимодействие', 'ja': 'アクション', 'ko': '상호작용', 'zhs': '互动',
                                            'zht': '互動'}),
    dict(type=95, action='weapons', label={'en': 'Weapons', 'es': 'Armas', 'fr': 'Armes', 'de': 'Waffen',
                                           'it': 'Armi', 'nl': 'Wapens', 'ru': 'Оружие', 'ja': '武器', 'ko': '무기',
                                           'zhs': '武器', 'zht': '武器'}),
    dict(type=96, action='shields', label={'en': 'Shields', 'es': 'Escudos', 'fr': 'Boucliers', 'de': 'Schilde',
                                           'it': 'Scudi', 'nl': 'Schilden', 'ru': 'Щиты', 'ja': '盾', 'ko': '방패',
                                           'zhs': '盾', 'zht': '盾'}),
    dict(type=97, action='bows', label={'en': 'Bows and Arrows', 'es': 'Arcos y flechas', 'fr': 'Arcs et flèches',
                                        'de': 'Bogen', 'it': 'Archi', 'nl': 'Bogen en pijlen', 'ru': 'Луки и стрелы',
                                        'ja': '弓矢', 'ko': '활과 화살', 'zhs': '弓箭', 'zht': '弓箭'}),
    dict(type=98, action='materials', label={'en': 'Materials', 'es': 'Materiales', 'fr': 'Matériaux',
                                             'de': 'Materialien', 'it': 'Materiali', 'nl': 'Materialen',
                                             'ru': 'Материалы', 'ja': '素材', 'ko': '소재', 'zhs': '材料', 'zht': '材料'}),
    dict(type=99, action='scope', label={'en': 'Scope', 'es': 'Catalejo', 'fr': 'Longue-vue', 'de': 'Fernglas',
                                         'it': 'Cannocchiale', 'nl': 'Verrekijker', 'ru': 'Визор', 'ja': '望遠鏡',
                                         'ko': '망원경', 'zhs': '望远镜', 'zht': '望遠鏡'}),
    dict(type=100, action='whistle', label={'en': 'Whistle', 'es': 'Silbar', 'fr': 'Siffler', 'de': 'Pfeifen',
                                            'it': 'Fischiare', 'nl': 'Fluiten', 'ru': 'Свист', 'ja': '口笛を吹く',
                                            'ko': '휘파람 불기', 'zhs': '吹口哨', 'zht': '吹口哨'}),
    dict(type=84, action='inventory', label={'en': 'Inventory', 'es': 'Alforja', 'fr': 'Sacoche', 'de': 'Tasche',
                                             'it': 'Borsa', 'nl': 'Inventaris', 'ru': 'Сумка', 'ja': 'ポーチ',
                                             'ko': '주머니', 'zhs': '袋子', 'zht': '袋子'}),
    dict(type=83, action='map', label={'en': 'Map', 'es': 'Mapa', 'fr': 'Carte', 'de': 'Karte', 'it': 'Mappa',
                                       'nl': 'Kaart', 'ru': 'Карта', 'ja': 'マップ', 'ko': '맵', 'zhs': '地图',
                                       'zht': '地圖'}),
    dict(type=101, action='adventure_log', label={'en': 'Adventure Log', 'es': 'Diario de viaje',
                                                  'fr': "Journal d'aventure", 'de': 'Tagebuch',
                                                  'it': 'Diario di viaggio', 'nl': 'Missies', 'ru': 'Дневник',
                                                  'ja': '冒険手帳', 'ko': '모험 수첩', 'zhs': '冒险笔记', 'zht': '冒險筆記'}),
    dict(type=102, action='pause', label={'en': 'Pause Menu', 'es': 'Menú de pausa', 'fr': 'Menu de pause',
                                          'de': 'Pausenmenü', 'it': 'Menu di pausa', 'nl': 'Pauzeren',
                                          'ru': 'Меню паузы', 'ja': 'ポーズメニュー', 'ko': '일시 정지 메뉴', 'zhs': '暂停菜单',
                                          'zht': '暫停選單'}),
    dict(type=77, action='ultrahand', label={'en': 'Ultrahand', 'es': 'Ultramano', 'fr': 'Emprise', 'de': 'Ultra-Hand',
                                             'it': 'Ultramano', 'nl': 'Ultrahand', 'ru': 'Комбикинез',
                                             'ja': 'ウルトラハンド', 'ko': '울트라 핸드', 'zhs': '究极手', 'zht': '究極手'}),
    dict(type=78, action='fuse', label={'en': 'Fuse', 'es': 'Combinación', 'fr': 'Amalgame', 'de': 'Synthese',
                                        'it': 'Compositor', 'nl': 'Hechten', 'ru': 'Чаромонтаж', 'ja': 'スクラビルド',
                                        'ko': '스크래빌드', 'zhs': '余料建造', 'zht': '餘料建造'}),
    dict(type=79, action='ascend', label={'en': 'Ascend', 'es': 'Infiltración', 'fr': 'Infiltration',
                                          'de': 'Deckensprung', 'it': 'Ascensus', 'nl': 'Plafondduiken',
                                          'ru': 'Чаропортал', 'ja': 'トーレルーフ', 'ko': '트레루프', 'zhs': '通天术',
                                          'zht': '通天術'}),
    dict(type=80, action='recall', label={'en': 'Recall', 'es': 'Retroceso', 'fr': 'Rétrospective',
                                          'de': 'Zeitumkehr', 'it': 'Reverto', 'nl': 'Terugspoelen', 'ru': 'Автореверс',
                                          'ja': 'モドレコ', 'ko': '리버레코', 'zhs': '倒转乾坤', 'zht': '倒轉乾坤'}),
    dict(type=81, action='autobuild', label={'en': 'Autobuild', 'es': 'Generador de esquemas', 'fr': 'Duplicata',
                                             'de': 'Bautomatik', 'it': 'Schematrix', 'nl': 'Bouwtekeningen',
                                             'ru': 'Реконструктор', 'ja': 'ブループリント', 'ko': '블루프린트',
                                             'zhs': '蓝图', 'zht': '藍圖'}),
    dict(type=82, action='camera', label={'en': 'Camera', 'es': 'Cámara', 'fr': 'Appareil photo', 'de': 'Fotomodul',
                                          'it': 'Scattaimmagini', 'nl': "Foto's", 'ru': 'Фотоаппарат', 'ja': 'ウツシエ',
                                          'ko': '사진기', 'zhs': '照相机', 'zht': '照相機'}),
    dict(type=103, action='ability_wheel', label={'en': 'Set Ability', 'es': 'Elegir un poder',
                                                  'fr': 'Choisir un pouvoir', 'de': 'Armkraft auswählen',
                                                  'it': 'Scegliere un potere', 'nl': 'Rechterhandkracht kiezen',
                                                  'ru': 'Выбор функции', 'ja': '右手の能力を選ぶ', 'ko': '오른손의 능력을 선택하기',
                                                  'zhs': '选择右手能力', 'zht': '選擇右手能力'}),
    # ROOT's quit: Esc backs out of the camera, the Scope, the quick menu and the bow (4.25: no longer an ability)
    dict(type=104, action='quit', label={'en': 'Leave camera / Scope', 'es': 'Salir de cámara / catalejo',
                                         'fr': 'Quitter appareil / longue-vue', 'de': 'Fotomodul / Fernglas verlassen',
                                         'it': 'Esci da foto / cannocchiale', 'nl': "Foto's / verrekijker verlaten",
                                         'ru': 'Выйти: фото / визор', 'ja': 'ウツシエ / 望遠鏡をやめる',
                                         'ko': '사진기 / 망원경 그만두기', 'zhs': '退出照相机 / 望远镜', 'zht': '退出照相機 / 望遠鏡'}),
    # 4.25: Tab leaves an ability (ABILITY's own "leave"; the C file's kKeyCtxRows gives it a row)
    dict(type=113, action='leave', label={'en': 'Leave the ability', 'es': 'Salir del poder',
                                          'fr': 'Quitter le pouvoir', 'de': 'Armkraft beenden',
                                          'it': 'Esci dal potere', 'nl': 'Kracht stoppen', 'ru': 'Выйти из функции',
                                          'ja': '右手の能力をやめる', 'ko': '오른손의 능력 그만두기', 'zhs': '退出右手能力',
                                          'zht': '退出右手能力'}),
]
KEY_CAPTION = {'en': 'Press A, then the new key or mouse button.\nEsc cancels; a key another action uses is\n'
                     'swapped. Used when Keyboard controls is PC.',
               'es': 'Pulsa A y luego la tecla o botón del ratón.\nEsc cancela; si otra acción la usa, se\n'
                     'intercambian. Con Controles de teclado: PC.',
               'fr': 'Appuyez sur A, puis la touche ou le bouton\nde souris. Échap annule ; une touche prise\n'
                     'est échangée. Avec Commandes clavier : PC.',
               'de': 'A, dann die neue Taste oder Maustaste.\nEsc bricht ab; belegte Tasten werden\n'
                     'getauscht. Gilt bei Tastatursteuerung PC.',
               'it': 'Premi A, poi il tasto o pulsante del mouse.\nEsc annulla; un tasto già usato viene\n'
                     'scambiato. Con Comandi da tastiera: PC.'}
KEY_PLACEHOLDER = {'en': '...'}
# "1069_%02d": the key rows' status texts, in the order of KeyStatus in the C file (no row has type 69)
STATUS = [
    {'en': 'Press a key...', 'es': 'Pulsa una tecla...', 'fr': 'Appuyez sur une touche...', 'de': 'Taste drücken...',
     'it': 'Premi un tasto...', 'nl': 'Druk op een toets...', 'ru': 'Нажмите клавишу...', 'ja': 'キーを押してください',
     'ko': '키를 누르세요', 'zhs': '请按键...', 'zht': '請按鍵...'},
    {'en': 'Not set', 'es': 'Sin asignar', 'fr': 'Non attribuée', 'de': 'Nicht belegt', 'it': 'Non assegnato',
     'nl': 'Niet ingesteld', 'ru': 'Не назначено', 'ja': '未設定', 'ko': '미설정', 'zhs': '未设置', 'zht': '未設定'},
    {'en': 'Not available yet', 'es': 'Aún no disponible', 'fr': 'Pas encore disponible', 'de': 'Noch nicht verfügbar',
     'it': 'Non ancora disponibile', 'nl': 'Nog niet beschikbaar', 'ru': 'Пока недоступно', 'ja': 'まだ使えません',
     'ko': '아직 사용할 수 없음', 'zhs': '暂不可用', 'zht': '暫不可用'},
    {'en': 'Menus need this key', 'es': 'Los menús la necesitan', 'fr': 'Requise par les menus',
     'de': 'Für Menüs nötig', 'it': 'Serve ai menu', 'nl': "Nodig voor menu's", 'ru': 'Нужна для меню',
     'ja': 'メニューで必要です', 'ko': '메뉴에 필요함', 'zhs': '菜单需要此键', 'zht': '選單需要此鍵'},
    {'en': 'Reserved key', 'es': 'Tecla reservada', 'fr': 'Touche réservée', 'de': 'Reservierte Taste',
     'it': 'Tasto riservato', 'nl': 'Gereserveerde toets', 'ru': 'Зарезервирована', 'ja': '予約済みのキー',
     'ko': '예약된 키', 'zhs': '保留键', 'zht': '保留鍵'},
    {'en': 'Cannot be changed', 'es': 'No se puede cambiar', 'fr': 'Non modifiable', 'de': 'Nicht änderbar',
     'it': 'Non modificabile', 'nl': 'Niet te wijzigen', 'ru': 'Нельзя изменить', 'ja': '変更できません',
     'ko': '변경할 수 없음', 'zhs': '无法更改', 'zht': '無法變更'},
    {'en': 'Clash: pick another', 'es': 'Choca: elige otra', 'fr': 'Conflit : autre touche',
     'de': 'Konflikt: andere Taste', 'it': 'Conflitto: altro tasto', 'nl': 'Conflict: kies andere',
     'ru': 'Конфликт: другую', 'ja': '競合：別のキーを', 'ko': '충돌: 다른 키 선택', 'zhs': '冲突：请换一个',
     'zht': '衝突：請換一個'},
    {'en': "Wheel can't be held", 'es': 'La rueda no se mantiene', 'fr': 'Molette : pas de maintien',
     'de': 'Mausrad nicht haltbar', 'it': 'Rotella: non si tiene', 'nl': 'Wiel kan niet vast',
     'ru': 'Колесо не удержать', 'ja': 'ホイールは長押し不可', 'ko': '휠은 누를 수 없음', 'zhs': '滚轮无法按住',
     'zht': '滾輪無法按住'},
    {'en': 'Not in this scheme', 'es': 'No está en el esquema', 'fr': 'Absente du schéma', 'de': 'Nicht im Schema',
     'it': 'Non nello schema', 'nl': 'Niet in dit schema', 'ru': 'Нет в схеме', 'ja': 'このスキームにありません',
     'ko': '이 구성에 없음', 'zhs': '方案中没有', 'zht': '方案中沒有'},
    # KS_SWAPPED: the other action's row after a swap, followed by its new key ("Swapped: Q")
    {'en': 'Swapped:', 'es': 'Intercambiada:', 'fr': 'Échangée :', 'de': 'Getauscht:', 'it': 'Scambiato:',
     'nl': 'Gewisseld:', 'ru': 'Обмен:', 'ja': '入れ替え：', 'ko': '교체:', 'zhs': '已交换：', 'zht': '已交換：'},
    # KS_CONFIRM_RESET: the reset row after the first confirm (no button name: Enter / E / a click on PC)
    {'en': 'Confirm again', 'es': 'Vuelve a confirmar', 'fr': 'Confirmez encore', 'de': 'Erneut bestätigen',
     'it': 'Conferma di nuovo', 'nl': 'Bevestig opnieuw', 'ru': 'Подтвердите снова', 'ja': 'もう一度決定',
     'ko': '다시 확인', 'zhs': '请再次确认', 'zht': '請再次確認'},
    # KS_RESET_DONE
    {'en': 'Keys restored', 'es': 'Teclas restablecidas', 'fr': 'Touches rétablies', 'de': 'Tasten zurückgesetzt',
     'it': 'Tasti ripristinati', 'nl': 'Toetsen hersteld', 'ru': 'Клавиши сброшены', 'ja': 'キーを初期化しました',
     'ko': '키를 초기화함', 'zhs': '按键已恢复', 'zht': '按鍵已恢復'},
]
STATUS_TYPE = 69

# The row that stands for the key rows when they don't all fit on the Options screen (kKeyLinkType, the kSafeEntries
# cap): A opens the F10 window at its Keys tab, which has every one. (F10 has no translations: its tab is "Keys".)
KEY_LINK = dict(
    type=105,
    label={'en': 'All keys', 'es': 'Todas las teclas', 'fr': 'Toutes les touches', 'de': 'Alle Tasten',
           'it': 'Tutti i tasti', 'nl': 'Alle toetsen', 'ru': 'Все клавиши', 'ja': 'すべてのキー', 'ko': '모든 키',
           'zhs': '所有按键', 'zht': '所有按鍵'},
    # (no button name: with PC keys the confirm is Enter / E / a click, not A, the wording)
    caption={'en': "Every action's key and mouse button is in the\nF10 settings window, Keys tab. Select this row\n"
                   "(or press F10) to open it.",
             'es': 'La tecla y el botón del ratón de cada acción\nestán en la ventana F10, pestaña Keys. Elige\n'
                   'esta fila (o pulsa F10) para abrirla.',
             'fr': "La touche de chaque action est dans la fenêtre\nF10, onglet Keys. Choisissez cette ligne\n"
                   "(ou appuyez sur F10) pour l'ouvrir.",
             'de': 'Die Tasten aller Aktionen stehen im F10-\nFenster, Reiter Keys. Diese Zeile wählen\n'
                   '(oder F10 drücken) öffnet es.',
             'it': 'Il tasto di ogni azione è nella finestra F10,\nscheda Keys. Scegli questa riga (o premi\nF10) per aprirla.',
             'ja': '各アクションのキーとマウスボタンは\nF10 設定ウィンドウの Keys タブにあります。\nこの行を選ぶか F10 を押すと開きます。'},
    choices=[{'en': 'Open (F10)', 'es': 'Abrir (F10)', 'fr': 'Ouvrir (F10)', 'de': 'Öffnen (F10)', 'it': 'Apri (F10)',
              'nl': 'Openen (F10)', 'ru': 'Открыть (F10)', 'ja': '開く（F10）', 'ko': '열기 (F10)', 'zhs': '打开（F10）',
              'zht': '開啟（F10）'}])

# The last row of the key block (kKeyResetType): every key back to the PC defaults; the first confirm asks again
# (STATUS' 'Confirm again'), a second within 3 s resets
KEY_RESET = dict(
    type=107,
    label={'en': 'Reset keys', 'es': 'Restablecer teclas', 'fr': 'Rétablir les touches', 'de': 'Tasten zurücksetzen',
           'it': 'Ripristina tasti', 'nl': 'Toetsen herstellen', 'ru': 'Сбросить клавиши', 'ja': 'キーを初期化',
           'ko': '키 초기화', 'zhs': '恢复默认按键', 'zht': '恢復預設按鍵'},
    caption={'en': 'Every key back to the PC defaults. Confirm, then\nconfirm again within 3 seconds to reset them.',
             'es': 'Todas las teclas vuelven a las de serie. Vuelve\na confirmar para restablecer todas las teclas\n'
                   '(antes de 3 segundos).',
             'fr': 'Toutes les touches reviennent à celles par\ndéfaut. Confirmez, puis confirmez encore dans\n'
                   'les 3 secondes.',
             'de': 'Alle Tasten auf die PC-Vorgabe zurück.\nBestätigen, dann binnen 3 Sekunden erneut\nbestätigen.',
             'it': 'Tutti i tasti tornano a quelli predefiniti.\nConferma, poi conferma di nuovo entro 3 secondi.',
             'ja': 'すべてのキーをPCの初期設定に戻します。\n決定したあと、3秒以内にもう一度決定します。'},
    choices=[{'en': 'Reset', 'es': 'Restablecer', 'fr': 'Rétablir', 'de': 'Zurücksetzen', 'it': 'Ripristina',
              'nl': 'Herstellen', 'ru': 'Сбросить', 'ja': '初期化', 'ko': '초기화', 'zhs': '恢复', 'zht': '恢復'}])

# The Mods section (v0.1.8, pc-ui): its first row opens the mods folder (kModsFolderType; the section's only row while
# no mod is installed), then one row per usable mod (kModFirst + its place, up to kMaxMods), its label the mod's folder
# name written by the engine over MOD_ROW's placeholder, Off / On for the next start. The order and the reasons a mod
# is not used stay in F10's Mods tab.
MODS_FOLDER = dict(
    type=144,
    label={'en': 'Mods folder', 'es': 'Carpeta de mods', 'fr': 'Dossier des mods', 'de': 'Mod-Ordner',
           'it': 'Cartella delle mod', 'nl': 'Modmap', 'ru': 'Папка модов', 'ja': 'Modフォルダー', 'ko': '모드 폴더',
           'zhs': '模组文件夹', 'zht': '模組資料夾'},
    caption={'en': 'One folder per mod, loaded at the next\nstart. Select this row to open it. The\n'
                   'order and any problem: F10, Mods tab (PC).',
             'es': 'Una carpeta por mod; se cargan al reiniciar\nel juego. Elige esta fila para abrirla.\n'
                   'El orden y los problemas: F10, Mods (PC).',
             'fr': 'Un dossier par mod, chargé au prochain\ndémarrage. Choisissez cette ligne pour\n'
                   "l'ouvrir. Ordre et problèmes : F10, Mods (PC).",
             'de': 'Ein Ordner pro Mod, geladen beim nächsten\nStart. Diese Zeile wählen öffnet ihn.\n'
                   'Reihenfolge, Probleme: F10, Mods (PC).',
             'it': 'Una cartella per mod, caricata al prossimo\navvio. Scegli questa riga per aprirla.\n'
                   'Ordine e problemi: F10, scheda Mods (PC).'},
    choices=[{'en': 'Open', 'es': 'Abrir', 'fr': 'Ouvrir', 'de': 'Öffnen', 'it': 'Apri', 'nl': 'Openen',
              'ru': 'Открыть', 'ja': '開く', 'ko': '열기', 'zhs': '打开', 'zht': '開啟'}])
MOD_FIRST, MOD_MAX = 145, 32   # kModFirst, kMaxMods
MOD_ROW = dict(
    label={'en': 'Mod'},   # (the engine writes the mod's folder name over it)
    caption={'en': 'This mod on or off, from the next start.\nThe load order: F10, Mods tab (PC).',
             'es': 'Este mod activado o no, al reiniciar el\njuego. El orden de carga: F10, Mods (PC).',
             'fr': 'Ce mod activé ou non, au prochain\ndémarrage. Ordre de chargement : F10 (PC).',
             'de': 'Diese Mod an oder aus, ab dem nächsten\nStart. Ladereihenfolge: F10, Mods (PC).',
             'it': 'Questa mod attiva o no, dal prossimo\navvio. Ordine di caricamento: F10 (PC).'},
    choices=[OFF, ON])
MOD_ENTRIES = [dict(MOD_ROW, type=MOD_FIRST + i) for i in range(MOD_MAX)]

# The section row (kZoneType): the first row of every section of the Options screen; its choices are the sections,
# in the order of the C file's Zone enum (Z_GENERAL.. Z_INTERFACE)
ZONES = dict(
    type=118,
    label={'en': 'Section', 'es': 'Sección', 'fr': 'Section', 'de': 'Bereich', 'it': 'Sezione', 'nl': 'Sectie',
           'ru': 'Раздел', 'ja': 'カテゴリー', 'ko': '카테고리', 'zhs': '类别', 'zht': '類別'},
    caption={'en': 'The settings come in sections: left / right\n(or a click on < or >) shows another one (PC).',
             'es': 'Los ajustes van por secciones: izquierda o\nderecha (o un clic en < o >) cambian de\n'
                   'sección (PC).',
             'fr': 'Les réglages sont rangés par sections :\ngauche / droite (ou un clic sur < ou >)\n'
                   'change de section (PC).',
             'de': 'Die Einstellungen sind in Bereiche\ngeteilt: links / rechts (oder ein Klick\n'
                   'auf < oder >) wechselt den Bereich (PC).',
             'it': 'Le impostazioni sono divise in sezioni:\nsinistra / destra (o un clic su < o >)\n'
                   'cambia sezione (PC).'},
    choices=[{'en': 'General', 'es': 'General', 'fr': 'Général', 'de': 'Allgemein', 'it': 'Generale', 'nl': 'Algemeen',
              'ru': 'Общие', 'ja': '全般', 'ko': '일반', 'zhs': '常规', 'zht': '一般'},
             {'en': 'Display', 'es': 'Pantalla', 'fr': 'Affichage', 'de': 'Anzeige', 'it': 'Schermo', 'nl': 'Beeld',
              'ru': 'Экран', 'ja': '画面', 'ko': '화면', 'zhs': '显示', 'zht': '顯示'},
             {'en': 'Graphics', 'es': 'Gráficos', 'fr': 'Graphismes', 'de': 'Grafik', 'it': 'Grafica',
              'nl': 'Graphics', 'ru': 'Графика', 'ja': 'グラフィック', 'ko': '그래픽', 'zhs': '图形', 'zht': '圖形'},
             {'en': 'Audio', 'es': 'Sonido', 'fr': 'Son', 'de': 'Ton', 'it': 'Audio', 'nl': 'Geluid', 'ru': 'Звук',
              'ja': 'サウンド', 'ko': '사운드', 'zhs': '声音', 'zht': '聲音'},
             {'en': 'Controls', 'es': 'Controles', 'fr': 'Commandes', 'de': 'Steuerung', 'it': 'Comandi',
              'nl': 'Besturing', 'ru': 'Управление', 'ja': '操作', 'ko': '조작', 'zhs': '操作', 'zht': '操作'},
             {'en': 'Keys', 'es': 'Teclas', 'fr': 'Touches', 'de': 'Tasten', 'it': 'Tasti', 'nl': 'Toetsen',
              'ru': 'Клавиши', 'ja': 'キー設定', 'ko': '키 설정', 'zhs': '按键', 'zht': '按鍵'},
             {'en': 'Interface', 'es': 'Interfaz', 'fr': 'Interface', 'de': 'Oberfläche', 'it': 'Interfaccia',
              'nl': 'Interface', 'ru': 'Интерфейс', 'ja': 'インターフェース', 'ko': '인터페이스', 'zhs': '界面',
              'zht': '介面'},
             {'en': 'Mods', 'es': 'Mods', 'fr': 'Mods', 'de': 'Mods', 'it': 'Mod', 'nl': 'Mods', 'ru': 'Моды',
              'ja': 'Mod', 'ko': '모드', 'zhs': '模组', 'zht': '模組'}])

# Key rows no longer in kKeyRows (as RETIRED below): parry left the PC scheme; aim went with 4.23 (A): LMB is attack,
# and with the bow in hand its draw
RETIRED_KEYS = [
    dict(type=74, action='aim', label={'en': 'Ready bow', 'es': 'Tensar el arco', 'fr': "Bander l'arc",
                                       'de': 'Bogen zücken', 'it': "Tendi l'arco", 'nl': 'Boog vasthouden',
                                       'ru': 'Натянуть лук', 'ja': '弓をかまえる', 'ko': '활 겨누기', 'zhs': '举弓',
                                       'zht': '舉弓'}),
    dict(type=75, action='guard', label={'en': 'Guard', 'es': 'Defenderse', 'fr': 'Se protéger', 'de': 'Blocken',
                                         'it': 'Difesa', 'nl': 'Verdedigen', 'ru': 'Выставить щит', 'ja': 'ガード',
                                         'ko': '가드', 'zhs': '防御', 'zht': '防禦'}),
    dict(type=76, action='parry', label={'en': 'Parry', 'es': 'Contratacar', 'fr': 'Repousser', 'de': 'Parieren',
                                         'it': 'Parata', 'nl': 'Afweren', 'ru': 'Отразить удар', 'ja': '攻撃をはじく',
                                         'ko': '공격 튕기기', 'zhs': '弹开攻击', 'zht': '彈開攻擊'}),
]

# Rows no longer in kRows whose texts stay in the archives: an exe linked before the row went still adds it, and on a
# rebuilt shared overlay it would show no text (check_c_table does not see these; types are never reused).
RETIRED = [
    dict(type=59, key='display.low_latency',   # present's (NVIDIA Reflex)
         label={'en': 'Low latency (NVIDIA Reflex)', 'es': 'Baja latencia (NVIDIA Reflex)',
                'fr': 'Faible latence (NVIDIA Reflex)', 'de': 'Niedrige Latenz (NVIDIA Reflex)',
                'it': 'Bassa latenza (NVIDIA Reflex)', 'nl': 'Lage latentie (NVIDIA Reflex)',
                'ru': 'Низкая задержка (NVIDIA Reflex)', 'ja': '低遅延 (NVIDIA Reflex)', 'ko': '저지연 (NVIDIA Reflex)',
                'zhs': '低延迟 (NVIDIA Reflex)', 'zht': '低延遲 (NVIDIA Reflex)'},
         caption={'en': 'NVIDIA Reflex: shorter delay between your\ninput and the picture. Boost keeps the GPU\n'
                        'clock up as well. NVIDIA GPUs only (PC).',
                  'es': 'NVIDIA Reflex: menos retraso entre lo que\npulsas y la imagen. Boost además mantiene\n'
                        'alto el reloj de la GPU. Solo GPU NVIDIA (PC).',
                  'fr': "NVIDIA Reflex : moins de délai entre vos\nactions et l'image. Boost garde aussi la\n"
                        'fréquence GPU élevée. NVIDIA uniquement (PC).',
                  'de': 'NVIDIA Reflex: weniger Verzögerung\nzwischen Eingabe und Bild. Boost hält\n'
                        'den GPU-Takt hoch. Nur NVIDIA-GPUs (PC).',
                  'it': "NVIDIA Reflex: meno ritardo tra i comandi\ne l'immagine. Boost mantiene anche alta\n"
                        'la frequenza della GPU. Solo NVIDIA (PC).'},
         choices=[OFF, ON,
                  {'en': 'On + boost', 'es': 'Sí + boost', 'fr': 'Oui + boost', 'de': 'An + Boost', 'it': 'Sì + boost',
                   'nl': 'Aan + boost', 'ru': 'Вкл. + boost', 'ja': 'オン + ブースト', 'ko': '켬 + 부스트', 'zhs': '开 + 加速',
                   'zht': '開 + 加速'}]),
]


# The game's own messages rewritten in place (same label and index; mals_patches). Row 5 "Swap jump controls" is a
# controller-only setting under the PC keys: the PC rows write the jump / sprint bits themselves, so Space jumps and
# Shift sprints whatever it says. Its caption named the two buttons with glyph tags that a keyboard player would read
# as keys; the new one has no tags.
SHIPPED = {
    '1005': {'en': 'Jump button (controller)', 'es': 'Botón de salto (mando)', 'fr': 'Bouton de saut (manette)',
             'de': 'Sprungtaste (Controller)', 'it': 'Pulsante di salto (controller)',
             'nl': 'Springknop (controller)', 'ru': 'Кнопка прыжка (геймпад)', 'ja': 'ジャンプボタン（コントローラー）',
             'ko': '점프 버튼 (컨트롤러)', 'zhs': '跳跃按键（手柄）', 'zht': '跳躍按鍵（手把）'},
    '0005': {'en': "Controller only: swaps the jump and dash buttons.\nKeyboard keys don't change.",
             'es': 'Solo mando: intercambia los botones de saltar y\ncorrer. Las teclas del teclado no cambian.',
             'fr': 'Manette uniquement : échange les boutons de\nsaut et de course. Les touches du clavier ne\n'
                   'changent pas.',
             'de': 'Nur Controller: tauscht die Tasten für\nSpringen und Sprinten. Die Tasten der\n'
                   'Tastatur bleiben gleich.',
             'it': 'Solo controller: scambia i pulsanti di salto e\nscatto. I tasti della tastiera non cambiano.',
             'nl': 'Alleen controller: wisselt de knoppen voor\nspringen en sprinten. Toetsen veranderen niet.',
             'ru': 'Только для геймпада: меняет\nместами кнопки прыжка и бега.\nКлавиши клавиатуры не меняются.',
             'ja': 'コントローラーのみ：ジャンプとダッシュのボタンを\n入れ替えます。キーボードのキーは変わりません。',
             'ko': '컨트롤러 전용: 점프와 대시 버튼을 바꿉니다.\n키보드 키는 바뀌지 않습니다.',
             'zhs': '仅限手柄：交换跳跃和冲刺按键。\n键盘按键不会改变。',
             'zht': '僅限手把：交換跳躍和衝刺按鍵。\n鍵盤按鍵不會改變。'},
}

C_TABLE = Path(__file__).resolve().parents[3] / 'engine/runtime/totk_options_menu.cpp'


def check_c_table(path=C_TABLE):
    """ENTRIES against kRows of the C file: the same types, keys and number of choices, in the same order; KEY_ENTRIES
    against kKeyRows (types, actions, order) and STATUS against KeyStatus (count); no type used twice or retired."""
    src = path.read_text(encoding='utf-8')
    lists = {m[1]: re.findall(r'"([^"]*)"', m[2])
             for m in re.finditer(r'const char \*const (k\w+)\[\] = \{(.*?)nullptr\};', src, re.S)}
    table = src[src.index('const PcRow kRows[]'):]
    table = table[:table.index('};')]
    rows = [(int(t), k, len(lists[v])) for t, k, v in re.findall(r'\{(\d+), "([^"]+)", (k\w+)\}', table)]
    ours = [(e['type'], e['key'], len(e['choices'])) for e in ENTRIES]
    if rows != ours:
        raise ValueError(f'ENTRIES {ours} do not match kRows {rows} of {path}')
    table = src[src.index('const KeyRow kKeyRows[]'):]
    keys = [(int(t), a) for t, a in re.findall(r'\{(\d+), "(\w+)"\}', table[:table.index('};')])]
    ours = [(e['type'], e['action']) for e in KEY_ENTRIES]
    if keys != ours:
        raise ValueError(f'KEY_ENTRIES {ours} do not match kKeyRows {keys} of {path}')
    status = re.search(r'enum KeyStatus \{([^}]*)\}', src)[1].split(',')
    if len(status) != len(STATUS):
        raise ValueError(f'STATUS has {len(STATUS)} texts, KeyStatus {len(status)} values')
    link = int(re.search(r'constexpr s32 kKeyLinkType = (\d+);', src)[1])
    if link != KEY_LINK['type']:
        raise ValueError(f'KEY_LINK type {KEY_LINK["type"]} is not kKeyLinkType {link} of {path}')
    reset = int(re.search(r'constexpr s32 kKeyResetType = (\d+);', src)[1])
    if reset != KEY_RESET['type']:
        raise ValueError(f'KEY_RESET type {KEY_RESET["type"]} is not kKeyResetType {reset} of {path}')
    zone = int(re.search(r'constexpr s32 kZoneType = (\d+);', src)[1])
    if zone != ZONES['type']:
        raise ValueError(f'ZONES type {ZONES["type"]} is not kZoneType {zone} of {path}')
    sections = re.search(r'enum Zone : s32 \{([^}]*)\}', src)[1].split(',')
    if len(sections) != len(ZONES['choices']) + 1:  # (+1: Z_COUNT)
        raise ValueError(f'ZONES has {len(ZONES["choices"])} section names, the Zone enum {len(sections) - 1}')
    folder = int(re.search(r'constexpr s32 kModsFolderType = (\d+);', src)[1])
    first = int(re.search(r'constexpr s32 kModFirst = (\d+);', src)[1])
    most = int(re.search(r'constexpr size_t kMaxMods = (\d+);', src)[1])
    if (folder, first, most) != (MODS_FOLDER['type'], MOD_FIRST, MOD_MAX):
        raise ValueError(f'MODS_FOLDER / MOD_FIRST / MOD_MAX {MODS_FOLDER["type"], MOD_FIRST, MOD_MAX} are not '
                         f'kModsFolderType / kModFirst / kMaxMods {folder, first, most} of {path}')
    taken = [r[0] for r in rows] + [k[0] for k in keys] + [STATUS_TYPE, link, reset, zone, folder] + \
        list(range(first, first + most))
    if len(set(taken)) != len(taken):
        raise ValueError(f'a type is used twice: {sorted(taken)}')
    reused = set(taken) & {e['type'] for e in RETIRED + RETIRED_KEYS}
    if reused:
        raise ValueError(f'types {sorted(reused)} are RETIRED: old exes would show their texts; take a new type')
    fallback = int(re.search(r'kKeyFallback = (\d+);', src)[1])
    if max(taken) >= fallback or fallback + 120 > 999:
        raise ValueError(f'types must stay below kKeyFallback ({fallback}), and its rows below 1000')
    check_pages(src)


PAGES = C_TABLE.parent / 'pc_bind_pages.inc'


def check_pages(src):
    """The key rows are pc_bind_pages.inc's on-foot entries (kKeyPages, at ROOT / GAMEPLAY / WORLD and kKeyCtxRows'
    (context, action) pairs), generated at run time (key_lines). One whose action has no KEY_ENTRIES texts still gets its row, labelled with the page's English
    label and no caption: say so here (the overlay still builds; add its texts and a kKeyRows type)."""
    if not PAGES.exists():
        return
    pages = re.findall(r'"([^"]+)"', re.search(r'kKeyPages\[\] = \{(.*?)\};', src, re.S)[1])
    ours = {e['action'] for e in KEY_ENTRIES}
    table = src[src.index('const KeyCtxRow kKeyCtxRows[]'):]
    ctx = set(re.findall(r'\{PCX_(\w+), "(\w+)"\}', table[:table.index('};')]))
    for page, scope, action, label in re.findall(r'^PC_PAGE\("([^"]+)",\s*(\w+),\s*"(\w+)",\s*"([^"]*)"\)',
                                                 PAGES.read_text(encoding='utf-8'), re.M):
        row = scope in ('ROOT', 'GAMEPLAY', 'WORLD') or (scope, action) in ctx
        if page in pages and row and action not in ours:
            print(f'options_menu: Options key row "{action}" ({page}) has no texts: it shows "{label}" in English')


def text(d, lang):
    # OPTIONS_MENU_LONGEST=1 (a test overlay, never a shipped one): every language gets the text's longest variant,
    # so a boot (always en-US) draws the most characters any language would: eui's per-frame font constant buffer
    # grows per drawn character
    if os.environ.get('OPTIONS_MENU_LONGEST') == '1':  # (Latin-script ones: the English font draws them)
        return max((t for k, t in d.items() if k in ('en', 'es', 'fr', 'de', 'it', 'nl')), key=len)
    return d.get(LANG_KEY[lang], d['en'])


def messages(lang):
    """[(label, text)] that this set adds for one Mals language."""
    out = []
    for e in ENTRIES + RETIRED + [ZONES, MODS_FOLDER] + MOD_ENTRIES:
        t = e['type']
        assert 50 <= t <= 999, 'PC entries use types 50..999 ("10%02d": 1050 .. 10999, past the game\'s 1000 .. 1013)'
        out.append((f'10{t:02d}', text(e['label'], lang)))
        out.append((f'{t:04d}', text(e['caption'], lang)))
        for i, c in enumerate(e.get('choices', [])):
            out.append((f'10{t:02d}_{i:02d}', text(c, lang)))
    for e in KEY_ENTRIES + RETIRED_KEYS:
        t = e['type']
        out.append((f'10{t:02d}', text(e['label'], lang)))
        out.append((f'{t:04d}', text(KEY_CAPTION, lang)))
        out.append((f'10{t:02d}_00', text(KEY_PLACEHOLDER, lang)))
    out += [(f'10{KEY_LINK["type"]:02d}', text(KEY_LINK['label'], lang)),
            (f'{KEY_LINK["type"]:04d}', text(KEY_LINK['caption'], lang)),
            (f'10{KEY_LINK["type"]:02d}_00', text(KEY_LINK['choices'][0], lang)),
            (f'10{KEY_RESET["type"]:02d}', text(KEY_RESET['label'], lang)),
            (f'{KEY_RESET["type"]:04d}', text(KEY_RESET['caption'], lang)),
            (f'10{KEY_RESET["type"]:02d}_00', text(KEY_RESET['choices'][0], lang))]
    for i, st in enumerate(STATUS):
        out.append((f'10{STATUS_TYPE:02d}_{i:02d}', text(st, lang)))
    return out


def plain(raw):
    """The characters of an MSBT text (UTF-16, terminator included); each control tag (0x0e) counts as one."""
    out, i = [], 0
    while i + 1 < len(raw):
        c = struct.unpack_from('<H', raw, i)[0]
        if c == 0:
            break
        if c == 0x0e:  # group, type, size, size bytes of parameters
            out.append('￼')
            i += 8 + struct.unpack_from('<H', raw, i + 6)[0]
            continue
        out.append(chr(c))
        i += 2
    return ''.join(out)


def check_captions(lang, shipped):
    """T_Caption_00 does not wrap: a caption line longer than the pane is cut at its edge (52 characters of English
    were, in game). Our captions written in this archive's language may not have a line longer than the longest line
    of the game's own captions in it (USen 50, EUes 49, EUfr 47, EUde 42 characters ...). Archives that show the
    English fallback are covered by the check of the English ones."""
    width = max(len(ln) for label, raw in shipped if re.fullmatch(r'\d{4}', label) for ln in plain(raw).split('\n'))
    for e in ENTRIES + RETIRED + [KEY_LINK, KEY_RESET, ZONES, MODS_FOLDER, MOD_ENTRIES[0]] + \
            [dict(type=k['type'], caption=KEY_CAPTION) for k in KEY_ENTRIES] + \
            [dict(type=int(label), caption=t) for label, t in SHIPPED.items() if len(label) == 4 and label[0] == '0']:
        if LANG_KEY[lang] in e['caption']:
            for ln in e['caption'][LANG_KEY[lang]].split('\n'):
                if len(ln) > width:
                    raise ValueError(f'{lang} caption of row {e["type"]}: {ln!r} is {len(ln)} characters, '
                                     f'the game\'s longest caption line is {width}')


def mals_patches(lang):
    """The mals set's patcher (mals.py): our messages appended to SystemOption_00; every shipped one kept but the
    texts SHIPPED rewrites (in place: same label, same index)."""
    check_c_table()
    new = messages(lang)
    rewrite = {label: text(t, lang).encode('utf-16-le') + b'\0\0' for label, t in SHIPPED.items()}

    def patch(data):
        header, buckets, before = msbt_parse(data)
        check_captions(lang, before)
        have = {label for label, _ in before}
        assert set(rewrite) <= have, f'{MSBT}: SHIPPED labels {sorted(set(rewrite) - have)} not in the game\'s file'
        out = msbt_add(msbt_build(header, buckets, [(label, rewrite.get(label, raw)) for label, raw in before]),
                       new, MSBT)
        _, _, after = msbt_parse(out)
        assert len(after) == len(before) + len(new)
        assert all(a == b or a[0] in rewrite for a, b in zip(after, before))
        return out
    return {MSBT: patch}
