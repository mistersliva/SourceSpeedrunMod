/*
 * i18n.js — interface language for index.html and hud.html.
 *
 * The pages are written in English. With Russian selected (Settings ->
 * Interface -> Language, pref "ui.lang"; "auto" follows the game language)
 * every text node and placeholder/title is translated by exact match against
 * the table below, plus a few patterns for text with numbers. A
 * MutationObserver translates whatever the scripts add later, so the rest of
 * the code keeps writing English.
 */
(function (global) {
  'use strict';

  const RU = {
    // Main menu
    'Resume': 'Продолжить', 'New game': 'Новая игра', 'New Game': 'Новая игра', 'Load game': 'Загрузить', 'Load Game': 'Загрузить игру',
    'Save game': 'Сохранить', 'Save Game': 'Сохранить игру', 'Replays': 'Реплеи', 'Settings': 'Настройки', 'Console': 'Консоль',
    'Disconnect': 'Отключиться', 'Quit': 'Выход', 'Movement': 'Движение', 'Auto bhop': 'Автобхоп',
    'Half-Life 2 content': 'Контент Half-Life 2', 'HL2': 'HL2', 'HL2 OE': 'HL2 OE', 'HL1': 'HL1',
    // Common buttons
    'Cancel': 'Отмена', 'Delete': 'Удалить', 'Load': 'Загрузить', 'Save': 'Сохранить', 'Apply': 'Применить', 'Back': 'Назад',
    'Close': 'Закрыть', 'OK': 'ОК', 'Clear': 'Очистить', 'Submit': 'Выполнить', 'Refresh': 'Обновить', 'Open folder': 'Открыть папку',
    'Use Defaults': 'По умолчанию', 'Overwrite': 'Перезаписать', 'None': 'Нет', 'On': 'Вкл', 'Off': 'Выкл', 'Enabled': 'Вкл', 'Disabled': 'Выкл',
    'Auto': 'Авто',
    // New game / saves
    'Difficulty': 'Сложность', 'Easy': 'Лёгкая', 'Normal': 'Нормальная', 'Medium': 'Нормальная', 'Hard': 'Тяжёлая',
    'Commentary': 'Комментарии', 'Start New Game': 'Начать игру', 'Chapter': 'Глава',
    'Autosave': 'Автосохранение', 'Quick Save': 'Быстрое сохранение', 'New Saved Game': 'Новое сохранение',
    'No saved games to display': 'Нет сохранений', 'Delete saved game': 'Удалить сохранение',
    'Are you sure you want to delete this saved game?': 'Удалить это сохранение?',
    'Overwrite saved game': 'Перезаписать сохранение', 'Are you sure you want to overwrite this saved game?': 'Перезаписать это сохранение?',
    'Game saved': 'Игра сохранена', 'Starting a new game will lose any unsaved progress. Continue?': 'Несохранённый прогресс будет потерян. Продолжить?',
    'Are you sure you want to disconnect from the game?': 'Отключиться от игры?', 'Disconnected': 'Отключено',
    'Quit game': 'Выход из игры', 'Are you sure you want to quit?': 'Выйти из игры?', 'Loading': 'Загрузка', 'Loading…': 'Загрузка…',
    // Replays + demo bar
    'Watch': 'Смотреть', 'Render MP4': 'Рендер в MP4', 'Delete replay': 'Удалить реплей', 'MP4': 'MP4',
    'No replays yet. Runs are recorded while the timer runs.': 'Реплеев пока нет. Раны записываются, пока идёт таймер.',
    'Delete this replay?': 'Удалить этот реплей?', 'Delete this replay? Its rendered MP4 is deleted too.': 'Удалить этот реплей? Отрендеренный MP4 тоже удалится.',
    'Render': 'Рендер', 'Pause': 'Пауза', 'Play': 'Играть', 'Stop': 'Стоп', '−10s': '−10с', '+10s': '+10с',
    'Back 10 s': 'Назад на 10 с', 'Forward 10 s': 'Вперёд на 10 с', 'Play / pause': 'Пуск / пауза', 'Previous part': 'Предыдущая часть', 'Next part': 'Следующая часть',
    'Install ffmpeg in the launcher (Settings) to render videos': 'Установите ffmpeg в лаунчере (Настройки), чтобы рендерить видео',
    // Ghost race
    'Ghost race': 'Гонка с призраками', 'Your name': 'Ваше имя', 'Join': 'Войти', 'Create lobby': 'Создать лобби',
    'Lobby code': 'Код лобби', 'Copy': 'Копировать', 'Leave lobby': 'Покинуть лобби', 'Start race': 'Начать гонку', 'Host': 'Хост',
    'Race board in HUD': 'Таблица гонки в HUD', 'Show ghosts': 'Показывать призраков', 'Ghost opacity': 'Непрозрачность призраков',
    'Race server': 'Сервер гонок', 'Race': 'Гонка', 'Finished': 'Финиш', 'Connecting…': 'Подключение…', 'CODE': 'КОД',
    'Everyone plays their own game; the others show up as ghosts and on the race board. Create a lobby and send the code to your friends, or join with their code.':
      'Каждый играет в своей игре; остальные видны как призраки и в таблице гонки. Создайте лобби и отправьте код друзьям или войдите по их коду.',
    'When everyone is here, press Start race: after a 5 second countdown every player starts a new game.':
      'Когда все соберутся, нажмите «Начать гонку»: после отсчёта 5 секунд у всех начнётся новая игра.',
    'Waiting for the host to start the race.': 'Ждём, пока хост начнёт гонку.',
    'No race server set (Settings → Multiplayer).': 'Не задан сервер гонок.',
    'Disconnected from the lobby.': 'Соединение с лобби потеряно.', 'Could not connect to the race server.': 'Не удалось подключиться к серверу гонок.',
    'Lobby is full': 'Лобби заполнено', 'Say': 'Чат', 'Message to the lobby (Y in game)': 'Сообщение в лобби (Y в игре)',
    'Ghost race chat': 'Чат гонки',
    // Settings tabs
    'Game': 'Игра', 'Interface': 'Интерфейс', 'HUD': 'HUD', 'Timer': 'Таймер', 'Keyboard': 'Клавиатура', 'Mouse': 'Мышь', 'Audio': 'Звук', 'Video': 'Видео',
    // Game tab
    'SourceSpeedrunMod': 'SourceSpeedrunMod', 'Movement physics': 'Физика движения',
    'Half-Life 2 — accelerated back hop': 'Half-Life 2 — ускорение назад (ABH)',
    'Half-Life 2 old engine — accelerated forward hop': 'Half-Life 2 старый движок — ускорение вперёд (AFH)',
    'Half-Life 1 — bunnyhop cap': 'Half-Life 1 — ограничение банихопа', 'Auto bunnyhop': 'Автобанихоп',
    'Cutsceneless maps': 'Карты Cutsceneless',
    "peng's Cutsceneless patch. Takes effect from the next map; start a new game after switching, since saves made on the other maps do not load correctly.":
      'Патч Cutsceneless от peng. Действует со следующей карты; после переключения начните новую игру — сейвы с других карт загружаются неправильно.',
    'Aim assistance': 'Помощь в прицеливании', 'Quick info (health / ammo)': 'Быстрая информация (здоровье / патроны)',
    'View bob': 'Покачивание камеры', 'Block the Windows key in game': 'Блокировать клавишу Windows в игре',
    'Enable developer console (~)': 'Консоль разработчика (~)',
    // Interface tab
    'Language': 'Язык', 'Automatic (game language)': 'Автоматически (язык игры)', 'English': 'English', 'Русский': 'Русский', 'Accent colour': 'Цвет акцента', 'Font': 'Шрифт', 'Interface scale': 'Масштаб интерфейса',
    'Background dimming': 'Затемнение фона', 'Animations': 'Анимации', 'Interface sounds': 'Звуки интерфейса',
    'Blur background': 'Размытие фона', 'Blur intensity': 'Сила размытия', 'Quick settings in main menu': 'Быстрые настройки в меню',
    'Reset interface': 'Сбросить интерфейс', 'Light blue': 'Голубой', 'White': 'Белый', 'Green': 'Зелёный', 'Orange': 'Оранжевый',
    'Pink': 'Розовый', 'Red': 'Красный', 'Yellow': 'Жёлтый', 'Cyan': 'Бирюзовый', 'Low': 'Низкое', 'High': 'Высокое', 'Full': 'Полное',
    // Timer tab
    'Splits (.lss)': 'Сплиты (.lss)', "LiveSplit files in the mod's splits folder": 'Файлы LiveSplit из папки splits мода',
    'Timing method': 'Метод отсчёта', 'Game time (no loads)': 'Игровое время (без загрузок)', 'Real time': 'Реальное время',
    'Count pauses as game time': 'Считать паузы игровым временем', 'Auto splitter (SourceSplit rules)': 'Автосплиттер (правила SourceSplit)',
    'Auto start, end and reset': 'Автостарт, финиш и сброс', 'Split on level transitions': 'Сплит при переходе уровня',
    'Split every N maps': 'Сплит каждые N карт', 'Never split on maps': 'Не сплитить на картах', 'Replays': 'Реплеи',
    'Record every run': 'Записывать каждый ран', 'Keep replays of reset runs': 'Сохранять реплеи сброшенных ранов',
    'Splits panel': 'Панель сплитов', 'Panel position': 'Положение панели', 'Top right': 'Справа сверху', 'Top left': 'Слева сверху',
    'Visible splits': 'Видимых сплитов', 'Hotkeys: bind': 'Горячие клавиши: назначьте', 'and': 'и',
    'Timer: start / split': 'Таймер: старт / сплит', 'reset': 'сброс', 'undo': 'отмена', 'skip': 'пропуск',
    'in Keyboard → SourceSpeedrunMod (console: timer_split, timer_reset, timer_undo, timer_skip). Best segments, attempts and new personal bests are saved back to the .lss file; the original is kept once as .lss.bak. Auto splitting follows':
      'в Клавиатура → SourceSpeedrunMod (консоль: timer_split, timer_reset, timer_undo, timer_skip). Лучшие сегменты, попытки и новые рекорды сохраняются в .lss; оригинал один раз сохраняется как .lss.bak. Автосплиты работают по правилам',
    ': a new game resets the timer, Half-Life 2 starts when you gain control on the train and ends at the final flash on d3_breen_01, and the timer splits the first time you leave each map through a level transition (loading an older save does not split again).':
      ': новая игра сбрасывает таймер, Half-Life 2 стартует, когда появляется управление в поезде, и заканчивается на финальной вспышке на d3_breen_01; сплит происходит при первом выходе с каждой карты через переход (загрузка старого сейва повторно не сплитит).',
    // HUD tab
    'Show speed': 'Показывать скорость', 'Speed position': 'Положение скорости', 'Bottom panel': 'Нижняя панель', 'Under crosshair': 'Под прицелом',
    'Timer in bottom panel': 'Таймер в нижней панели', 'Run timer': 'Таймер рана', 'Map time': 'Время карты', 'Movement mode': 'Режим движения',
    'Health and armor': 'Здоровье и броня', 'Weapon and ammo': 'Оружие и патроны', 'Crosshair': 'Прицел', 'Show crosshair': 'Показывать прицел',
    'Style': 'Стиль', 'Colour': 'Цвет', 'Length': 'Длина', 'Gap': 'Зазор', 'Thickness': 'Толщина', 'Outline': 'Обводка', 'Opacity': 'Непрозрачность',
    'Cross': 'Крест', 'Cross with dot': 'Крест с точкой', 'T': 'T', 'Dot': 'Точка', 'Circle': 'Круг', 'Circle with dot': 'Круг с точкой',
    'Panel backgrounds': 'Фон панелей', 'HUD scale': 'Масштаб HUD', 'HUD opacity': 'Непрозрачность HUD', 'Reset HUD': 'Сбросить HUD',
    // Keyboard / mouse
    'Press a key…': 'Нажмите клавишу…', 'Restore all key bindings to their default values?': 'Вернуть все клавиши по умолчанию?',
    'Speedrun': 'Спидран', 'Cycle movement mode': 'Сменить режим движения', 'Duck + jump': 'Присесть + прыжок', 'Kill (respawn)': 'Убить себя (респавн)',
    'Movement: Half-Life 1': 'Движение: Half-Life 1', 'Movement: Half-Life 2 (ABH)': 'Движение: Half-Life 2 (ABH)',
    'Movement: Half-Life 2 old engine (AFH)': 'Движение: Half-Life 2 старый движок (AFH)', 'Practice load': 'Тренировочная загрузка',
    'Practice save': 'Тренировочное сохранение', 'Quick load': 'Быстрая загрузка', 'Quick save': 'Быстрое сохранение',
    'Reload (restart from the last save)': 'Перезагрузка (с последнего сейва)', 'Saveload (save + load)': 'Сейвлоад (сохранить + загрузить)',
    'Save deletion (save ""; reload)': 'Удаление сейва (save ""; reload)', 'Timer: reset': 'Таймер: сброс', 'Timer: skip split': 'Таймер: пропустить сплит',
    'Timer: undo split': 'Таймер: отменить сплит', 'Toggle auto bunnyhop': 'Вкл/выкл автобанихоп',
    'Toggle position display (cl_showpos)': 'Показ позиции (cl_showpos)',
    'Demo playback controls (right mouse button while watching)': 'Управление просмотром демо (правая кнопка мыши при просмотре)',
    'Mouse sensitivity': 'Чувствительность мыши', 'Zoom sensitivity': 'Чувствительность в зуме', 'Raw input': 'Прямой ввод (raw input)',
    'Mouse filter': 'Сглаживание мыши', 'Reverse mouse': 'Инверсия мыши', 'Field of view': 'Поле зрения',
    // Valve key binding list (kb_act.lst) and engine strings
    'Auto-Aim': 'Автоприцел', 'MOVEMENT': 'ДВИЖЕНИЕ', 'Move forward': 'Вперёд', 'Move back': 'Назад', 'Move left (strafe)': 'Влево (стрейф)',
    'Move right (strafe)': 'Вправо (стрейф)', 'Sprint (Move Quickly)': 'Бег', 'Jump': 'Прыжок', 'Duck': 'Присесть', 'COMBAT': 'БОЙ',
    'Primary attack': 'Основная атака', 'Secondary attack': 'Дополнительная атака', 'Reload weapon': 'Перезарядка',
    'Use item (buttons, machines, ...)': 'Использовать (кнопки, механизмы...)', 'Gravity Gun': 'Гравипушка', 'Suit Zoom': 'Зум костюма',
    'Send/Recall Squad': 'Отправить/вернуть отряд', 'Weapon category 1': 'Оружие, слот 1', 'Weapon category 2': 'Оружие, слот 2',
    'Weapon category 3': 'Оружие, слот 3', 'Weapon category 4': 'Оружие, слот 4', 'Weapon category 5': 'Оружие, слот 5',
    'Weapon category 6': 'Оружие, слот 6', 'Next weapon': 'Следующее оружие', 'Previous weapon': 'Предыдущее оружие',
    'Last weapon used': 'Последнее оружие', 'MISCELLANEOUS': 'РАЗНОЕ', 'Toggle close caption sound effects': 'Субтитры звуков вкл/выкл',
    'Take screen shot': 'Скриншот', 'Pause game': 'Пауза', 'Toggle developer console': 'Консоль разработчика',
    'MISCELLANEOUS MOVEMENT': 'ДОПОЛНИТЕЛЬНОЕ ДВИЖЕНИЕ', 'Walk (Move Slowly)': 'Шаг', 'Turn left': 'Поворот влево', 'Turn right': 'Поворот вправо',
    'Swim up': 'Всплыть', 'Swim down': 'Нырнуть', 'Look up': 'Смотреть вверх', 'Look down': 'Смотреть вниз', 'Strafe modifier': 'Модификатор стрейфа',
    'Keyboard look modifier': 'Обзор клавиатурой', 'Flashlight': 'Фонарик', 'Use': 'Использовать',
    'Captioning': 'Субтитры', 'Closed Captions': 'Субтитры и звуки', 'Subtitles (dialog only)': 'Только речь', 'Full screen': 'Полный экран',
    'Advanced...': 'Дополнительно...', 'Antialiasing mode': 'Сглаживание', 'Filtering mode': 'Фильтрация текстур', 'High Dynamic Range': 'HDR',
    'Bloom (if available)': 'Bloom (если доступно)', 'Full (if available)': 'Полный (если доступно)', 'Motion Blur': 'Размытие в движении',
    'Anisotropic 2×': 'Анизотропная 2×', 'Anisotropic 4×': 'Анизотропная 4×', 'Anisotropic 8×': 'Анизотропная 8×', 'Anisotropic 16×': 'Анизотропная 16×',
    // Audio
    'Game volume': 'Громкость игры', 'Music volume': 'Громкость музыки', 'Speaker configuration': 'Конфигурация динамиков',
    'Headphones': 'Наушники', '2 Speakers': '2 динамика', '4 Speakers': '4 динамика', '5.1 Speakers': '5.1', '7.1 Speakers': '7.1',
    'Sound quality': 'Качество звука', 'Captions': 'Субтитры', 'No captions': 'Без субтитров', 'Subtitles only': 'Только речь',
    'Subtitles and sound effects': 'Речь и звуки',
    // Video
    'Resolution': 'Разрешение', 'Display mode': 'Режим экрана', 'Display': 'Экран', 'Fullscreen': 'Полный экран', 'Windowed': 'В окне',
    'Windowed (borderless)': 'Окно без рамки', 'Gamma (lower is brighter)': 'Гамма (меньше — ярче)', 'Advanced': 'Дополнительно',
    'Model detail': 'Детализация моделей', 'Texture detail': 'Детализация текстур', 'Shader detail': 'Детализация шейдеров',
    'Water detail': 'Детализация воды', 'Shadow detail': 'Детализация теней', 'Color correction': 'Цветокоррекция',
    'Antialiasing': 'Сглаживание', 'Texture filtering': 'Фильтрация текстур', 'Trilinear': 'Трилинейная',
    'Wait for vertical sync': 'Вертикальная синхронизация', 'Motion blur': 'Размытие в движении', 'High dynamic range': 'HDR',
    'Bloom': 'Bloom', 'Max FPS': 'Макс. FPS', 'Multicore rendering': 'Многоядерный рендеринг',
    'Off removes rare light streaks in the sky, at the cost of some FPS': 'Выкл убирает редкие полосы света в небе ценой части FPS',
    'Very High': 'Очень высокое', 'Maximum': 'Максимум', 'Reflect world': 'Отражать мир', 'Reflect all': 'Отражать всё',
    // HUD page
    'Speed:': 'Скорость:', 'u/s': 'ед/с', 'Aux power': 'Энергия костюма', 'Sprint': 'Бег', 'Flashlight': 'Фонарик', 'Oxygen': 'Кислород',
    'Alt': 'Доп', 'Previous segment': 'Прошлый сегмент', 'Sum of best': 'Сумма лучших', 'FOLLOWING': 'СЛЕДУЮТ',
    'HOLDING POSITION': 'ДЕРЖАТ ПОЗИЦИЮ', 'Movement': 'Движение',
    'Half-Life 2 — ABH': 'Half-Life 2 — ABH', 'Half-Life 2 Old Engine — AFH': 'Half-Life 2 старый движок — AFH',
  };

  // Text with numbers or names in it.
  const RU_PATTERNS = [
    [/^Part (\d+) \/ (\d+)$/, 'Часть $1 / $2'],
    [/^(\d+) parts · (.+)$/, 'частей: $1 · $2'],
    [/^1 part · (.+)$/, '1 часть · $1'],
    [/^Chapter (\d+)$/, 'Глава $1'],
    [/^(\d+ × \d+)\s+\(native\)$/, '$1  (родное)'],
    [/^The replay plays back while frames are saved to (.+)\. The game is silent while it renders; the video is ready when the replay ends\.$/,
      'Реплей проигрывается, а кадры сохраняются в $1. Во время рендера звука нет; видео будет готово, когда реплей закончится.'],
  ];

  let lang = 'en';
  const KEEP = new WeakMap(); // node -> English original

  function translate(text) {
    if (lang !== 'ru') return null;
    const key = text.replace(/\s+/g, ' ').trim();
    if (!key) return null;
    if (Object.prototype.hasOwnProperty.call(RU, key)) return RU[key];
    for (const [re, to] of RU_PATTERNS) if (re.test(key)) return key.replace(re, to);
    return null;
  }

  function t(text) { return translate(text) ?? text; }

  function translateNode(node) {
    if (node.nodeType === 3) {
      const tr = translate(node.nodeValue);
      if (tr != null && tr !== node.nodeValue.trim()) {
        const lead = node.nodeValue.match(/^\s*/)[0], trail = node.nodeValue.match(/\s*$/)[0];
        KEEP.set(node, node.nodeValue);
        node.nodeValue = lead + tr + trail;
      }
      return;
    }
    if (node.nodeType !== 1 || node.tagName === 'SCRIPT' || node.tagName === 'STYLE') return;
    for (const attr of ['placeholder', 'title', 'aria-label']) {
      const v = node.getAttribute(attr);
      const tr = v && translate(v);
      if (tr != null) node.setAttribute(attr, tr);
    }
    for (const c of node.childNodes) translateNode(c);
  }

  function start(gameLanguage) {
    const pref = global.Prefs ? Prefs.get('ui.lang') : 'auto';
    lang = pref === 'ru' || (pref === 'auto' && /russian/i.test(gameLanguage || '')) ? 'ru' : 'en';
    document.documentElement.lang = lang;
    if (lang !== 'ru') return;
    translateNode(document.body);
    new MutationObserver((list) => {
      for (const m of list) {
        if (m.type === 'characterData') translateNode(m.target);
        else if (m.type === 'attributes') translateNode(m.target);
        else for (const n of m.addedNodes) translateNode(n);
      }
    }).observe(document.body, { subtree: true, childList: true, characterData: true, attributes: true, attributeFilter: ['placeholder', 'title'] });
  }

  // Switching the language reloads both pages.
  if (global.Prefs) Prefs.onChange((key) => { if (key === 'ui.lang') location.reload(); });

  global.I18N = { start, t, get lang() { return lang; } };
})(window);
