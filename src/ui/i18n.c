#define I18N_IMPLEMENTATION
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "i18n.h"

typedef struct {
  const char *en;
  const char *ru;
  const char *uk;
  const char *be;
} Translation;

/* Keep this intentionally conservative.  Abbreviations, product/protocol
 * names, modulation modes and established radio/DSP terms stay in English. */
static const Translation translations[] = {
  {"About", "О программе", "Про програму", "Аб праграме"},
  {"Add", "Добавить", "Додати", "Дадаць"},
  {"Add Bookmark", "Добавить закладку", "Додати закладку", "Дадаць закладку"},
  {"Add Receiver", "Добавить приемник", "Додати приймач", "Дадаць прыёмнік"},
  {"Add Wideband", "Добавить Wideband", "Додати Wideband", "Дадаць Wideband"},
  {"Add at VFO", "Добавить на VFO", "Додати на VFO", "Дадаць на VFO"},
  {"Aircraft", "Воздушные суда", "Повітряні судна", "Паветраныя судны"},
  {"Antenna", "Антенна", "Антена", "Антэна"},
  {"Antenna:", "Антенна:", "Антена:", "Антэна:"},
  {"Appearance", "Внешний вид", "Вигляд", "Выгляд"},
  {"Audio", "Аудио", "Аудіо", "Аўдыя"},
  {"Auto-phase", "Автофаза", "Автофаза", "Аўтафаза"},
  {"Auto-save", "Автосохранение", "Автозбереження", "Аўтазахаванне"},
  {"Auto-save page", "Автосохранение страницы", "Автозбереження сторінки", "Аўтазахаванне старонкі"},
  {"Auto-save pass", "Автосохранение прохода", "Автозбереження проходу", "Аўтазахаванне праходу"},
  {"Auto-start", "Автозапуск", "Автозапуск", "Аўтазапуск"},
  {"Automatic", "Автоматически", "Автоматично", "Аўтаматычна"},
  {"Average:", "Усреднение:", "Усереднення:", "Асерадненне:"},
  {"Averaging:", "Усреднение:", "Усереднення:", "Асерадненне:"},
  {"Bands", "Диапазоны", "Діапазони", "Дыяпазоны"},
  {"Both", "Оба", "Обидва", "Абодва"},
  {"Brightness:", "Яркость:", "Яскравість:", "Яркасць:"},
  {"Broadcast FM (WFM)", "Вещательный FM (WFM)", "Мовний FM (WFM)", "Вяшчальны FM (WFM)"},
  {"Browse…", "Обзор…", "Огляд…", "Агляд…"},
  {"Button Labels", "Подписи кнопок", "Підписи кнопок", "Подпісы кнопак"},
  {"Calibrate", "Калибровать", "Калібрувати", "Калібраваць"},
  {"Calibrate TX", "Калибровать TX", "Калібрувати TX", "Калібраваць TX"},
  {"Callsign:", "Позывной:", "Позивний:", "Пазыўны:"},
  {"Color Theme:", "Цветовая схема:", "Колірна схема:", "Колеравая схема:"},
  {"Choose recording folder", "Выберите папку записи", "Виберіть папку запису", "Выберыце папку запісу"},
  {"Clear", "Очистить", "Очистити", "Ачысціць"},
  {"Clear all", "Очистить все", "Очистити все", "Ачысціць усё"},
  {"Colour label by DXCC entity", "Цвет подписи по DXCC entity", "Колір підпису за DXCC entity", "Колер подпісу паводле DXCC entity"},
  {"Configure", "Настроить", "Налаштувати", "Наладзіць"},
  {"Configuration", "Настройки", "Налаштування", "Налады"},
  {"Connect to DX cluster", "Подключаться к DX Cluster", "Підключатися до DX Cluster", "Падключацца да DX Cluster"},
  {"Connecting…", "Подключение…", "Підключення…", "Падключэнне…"},
  {"Contrast:", "Контраст:", "Контраст:", "Кантраст:"},
  {"Correction interval (s):", "Интервал коррекции (с):", "Інтервал корекції (с):", "Інтэрвал карэкцыі (с):"},
  {"Correction (ppm):", "Коррекция (ppm):", "Корекція (ppm):", "Карэкцыя (ppm):"},
  {"Create the two transverter entries", "Создать две записи transverter", "Створити два записи transverter", "Стварыць два запісы transverter"},
  {"Delete", "Удалить", "Видалити", "Выдаліць"},
  {"Denoise", "Подавление шума", "Приглушення шуму", "Падаўленне шуму"},
  {"Device:", "Устройство:", "Пристрій:", "Прылада:"},
  {"Display", "Отображение", "Відображення", "Адлюстраванне"},
  {"Enable diversity", "Включить разнесенный прием", "Увімкнути рознесене приймання", "Уключыць разнесены прыём"},
  {"Enable Equalizer", "Включить Equalizer", "Увімкнути Equalizer", "Уключыць Equalizer"},
  {"Enable Phase Rotator", "Включить Phase Rotator", "Увімкнути Phase Rotator", "Уключыць Phase Rotator"},
  {"Enable PureSignal", "Включить PureSignal", "Увімкнути PureSignal", "Уключыць PureSignal"},
  {"Enable TCI server", "Включить сервер TCI", "Увімкнути сервер TCI", "Уключыць сервер TCI"},
  {"Enable compressor", "Включить compressor", "Увімкнути compressor", "Уключыць compressor"},
  {"Enable leveler", "Включить leveler", "Увімкнути leveler", "Уключыць leveler"},
  {"Enter the address first", "Сначала введите адрес", "Спочатку введіть адресу", "Спачатку ўвядзіце адрас"},
  {"Erase", "Стереть", "Стерти", "Сцерці"},
  {"Exit", "Выход", "Вихід", "Выхад"},
  {"Folder…", "Папка…", "Тека…", "Папка…"},
  {"Folder:", "Папка:", "Тека:", "Папка:"},
  {"Forget", "Забыть", "Забути", "Забыць"},
  {"Forgotten", "Удалено", "Видалено", "Выдалена"},
  {"Frequency A: ", "Частота A: ", "Частота A: ", "Частата A: "},
  {"Frequency B: ", "Частота B: ", "Частота B: ", "Частата B: "},
  {"Frequency offset (Hz):", "Смещение частоты (Hz):", "Зсув частоти (Hz):", "Зрух частаты (Hz):"},
  {"Frequency Calibration (PPM)", "Калибровка частоты (PPM)", "Калібрування частоти (PPM)", "Каліброўка частаты (PPM)"},
  {"High:", "Верх:", "Верх:", "Верх:"},
  {"Hotkeys", "Горячие клавиши", "Гарячі клавіші", "Гарачыя клавішы"},
  {"Host / IP:", "Хост / IP:", "Хост / IP:", "Хост / IP:"},
  {"Interface font:", "Шрифт интерфейса:", "Шрифт інтерфейсу:", "Шрыфт інтэрфейсу:"},
  {"Invert", "Инвертировать", "Інвертувати", "Інвертаваць"},
  {"Language:", "Язык:", "Мова:", "Мова:"},
  {"Load", "Загрузить", "Завантажити", "Загрузіць"},
  {"Load Original", "Загрузить исходный", "Завантажити початковий", "Загрузіць зыходны"},
  {"Load…", "Загрузить…", "Завантажити…", "Загрузіць…"},
  {"Left", "Левый", "Лівий", "Левы"},
  {"Local Audio", "Локальное аудио", "Локальне аудіо", "Лакальнае аўдыя"},
  {"Local Microphone", "Локальный микрофон", "Локальний мікрофон", "Лакальны мікрафон"},
  {"Log", "Журнал", "Журнал", "Журнал"},
  {"Low:", "Низ:", "Низ:", "Ніз:"},
  {"Manual", "Вручную", "Вручну", "Уручную"},
  {"Map", "Карта", "Мапа", "Карта"},
  {"Mappings", "Назначения", "Призначення", "Прызначэнні"},
  {"Messages", "Сообщения", "Повідомлення", "Паведамленні"},
  {"Microphone", "Микрофон", "Мікрофон", "Мікрафон"},
  {"Mute while TX", "Без звука во время TX", "Без звуку під час TX", "Без гуку падчас TX"},
  {"Name: ", "Имя: ", "Назва: ", "Назва: "},
  {"Network", "Сеть", "Мережа", "Сетка"},
  {"Network device:", "Сетевое устройство:", "Мережевий пристрій:", "Сеткавая прылада:"},
  {"Network Logging", "Сетевой журнал", "Мережевий журнал", "Сеткавы журнал"},
  {"New Mapping", "Новое назначение", "Нове призначення", "Новае прызначэнне"},
  {"No HPSDR devices found", "Устройства HPSDR не найдены", "Пристрої HPSDR не знайдено", "Прылады HPSDR не знойдзены"},
  {"No MIDI devices found!", "Устройства MIDI не найдены!", "Пристрої MIDI не знайдено!", "Прылады MIDI не знойдзены!"},
  {"No settings found", "Настройки не найдены", "Налаштування не знайдено", "Налады не знойдзены"},
  {"Open MIDI File", "Открыть файл MIDI", "Відкрити файл MIDI", "Адкрыць файл MIDI"},
  {"Other", "Другое", "Інше", "Іншае"},
  {"PA / Linearity", "PA / Линейность", "PA / Лінійність", "PA / Лінейнасць"},
  {"Please restart MacHPSDR to apply the language to every window.", "Перезапустите MacHPSDR, чтобы применить язык ко всем окнам.", "Перезапустіть MacHPSDR, щоб застосувати мову до всіх вікон.", "Перазапусціце MacHPSDR, каб ужыць мову ва ўсіх вокнах."},
  {"Radio", "Радио", "Радіо", "Радыё"},
  {"Radio Model", "Модель радио", "Модель радіо", "Мадэль радыё"},
  {"Re-acquire", "Повторный захват", "Повторне захоплення", "Паўторны захоп"},
  {"Record", "Запись", "Запис", "Запіс"},
  {"Recording", "Запись", "Запис", "Запіс"},
  {"Reload cty.dat", "Перезагрузить cty.dat", "Перезавантажити cty.dat", "Перазагрузіць cty.dat"},
  {"Remote Audio", "Удаленное аудио", "Віддалене аудіо", "Аддаленае аўдыя"},
  {"Report spots to PSK Reporter", "Отправлять spots в PSK Reporter", "Надсилати spots до PSK Reporter", "Адпраўляць spots у PSK Reporter"},
  {"Reference station:", "Опорная станция:", "Опорна станція:", "Апорная станцыя:"},
  {"Reset", "Сбросить", "Скинути", "Скінуць"},
  {"Retry Discovery", "Повторить поиск", "Повторити пошук", "Паўтарыць пошук"},
  {"Retry now", "Повторить сейчас", "Повторити зараз", "Паўтарыць зараз"},
  {"Right", "Правый", "Правий", "Правы"},
  {"Save", "Сохранить", "Зберегти", "Захаваць"},
  {"Save bias", "Сохранить bias", "Зберегти bias", "Захаваць bias"},
  {"Save File", "Сохранить файл", "Зберегти файл", "Захаваць файл"},
  {"Scan band", "Сканировать диапазон", "Сканувати діапазон", "Сканаваць дыяпазон"},
  {"Search setting names and show matching sections (Ctrl/Cmd+F)", "Искать настройки и показывать подходящие разделы (Ctrl/Cmd+F)", "Шукати налаштування й показувати відповідні розділи (Ctrl/Cmd+F)", "Шукаць налады і паказваць адпаведныя раздзелы (Ctrl/Cmd+F)"},
  {"Search settings", "Поиск настроек", "Пошук налаштувань", "Пошук налад"},
  {"Send", "Отправить", "Надіслати", "Адправіць"},
  {"Send QSOs over the network", "Отправлять QSO по сети", "Надсилати QSO мережею", "Адпраўляць QSO праз сетку"},
  {"Show Panadapter", "Показывать Panadapter", "Показувати Panadapter", "Паказваць Panadapter"},
  {"Show spots", "Показывать spots", "Показувати spots", "Паказваць spots"},
  {"Slant +", "Наклон +", "Нахил +", "Нахіл +"},
  {"Slant −", "Наклон −", "Нахил −", "Нахіл −"},
  {"Start", "Запустить", "Запустити", "Запусціць"},
  {"Start Radio", "Запустить радио", "Запустити радіо", "Запусціць радыё"},
  {"Station", "Станция", "Станція", "Станцыя"},
  {"Stations", "Станции", "Станції", "Станцыі"},
  {"Stereo", "Стерео", "Стерео", "Стэрэа"},
  {"Stop", "Остановить", "Зупинити", "Спыніць"},
  {"Readout font (monospaced):", "Шрифт индикаторов (моноширинный):", "Шрифт індикаторів (моноширинний):", "Шрыфт індыкатараў (монашырынны):"},
  {"Region", "Регион", "Регіон", "Рэгіён"},
  {"Rotate:", "Поворот:", "Обертання:", "Паварот:"},
  {"Skin:", "Тема:", "Тема:", "Тэма:"},
  {"Spot Reporting", "Отправка spots", "Надсилання spots", "Адпраўка spots"},
  {"Update", "Обновить", "Оновити", "Абнавіць"},
  {"Update Bookmark", "Обновить закладку", "Оновити закладку", "Абнавіць закладку"},
  {"Use platform default", "Использовать системный", "Використовувати системний", "Выкарыстоўваць сістэмны"},
  {"View:", "Вид:", "Вигляд:", "Выгляд:"},
  {"Waiting for APT…", "Ожидание APT…", "Очікування APT…", "Чаканне APT…"},
  {"Waiting for SSTV…", "Ожидание SSTV…", "Очікування SSTV…", "Чаканне SSTV…"},
  {"Waiting for WEFAX…", "Ожидание WEFAX…", "Очікування WEFAX…", "Чаканне WEFAX…"},
  {"idle", "ожидание", "очікування", "чаканне"},
};

static I18nLanguage active_language = I18N_EN;
static char *language_path;

static I18nLanguage language_from_code(const char *code) {
  if(code==NULL) return I18N_EN;
  if(g_ascii_strncasecmp(code,"ru",2)==0) return I18N_RU;
  if(g_ascii_strncasecmp(code,"uk",2)==0) return I18N_UK;
  if(g_ascii_strncasecmp(code,"be",2)==0) return I18N_BE;
  return I18N_EN;
}

void i18n_init(const char *config_dir) {
  char *saved=NULL;
  g_free(language_path);
  language_path=g_build_filename(config_dir,"language",NULL);
  if(g_file_get_contents(language_path,&saved,NULL,NULL)) {
    g_strstrip(saved);
    active_language=language_from_code(saved);
    g_free(saved);
    return;
  }

  /* First launch follows the desktop locale when it matches a documented
   * language. g_get_language_names() already accounts for LANGUAGE/LC_ALL. */
  const char * const *names=g_get_language_names();
  for(int i=0;names[i]!=NULL;i++) {
    I18nLanguage candidate=language_from_code(names[i]);
    if(candidate!=I18N_EN || g_ascii_strncasecmp(names[i],"en",2)==0) {
      active_language=candidate;
      break;
    }
  }
}

I18nLanguage i18n_language(void) { return active_language; }

const char *i18n_language_code(void) {
  static const char *codes[]={"en","ru","uk","be"};
  return codes[active_language];
}

const char *i18n_language_name(I18nLanguage language) {
  static const char *names[]={"English","Русский","Українська","Беларуская"};
  return (language>=0 && language<I18N_LANGUAGE_COUNT)?names[language]:names[0];
}

void i18n_set_language(I18nLanguage language) {
  if(language<0 || language>=I18N_LANGUAGE_COUNT) return;
  active_language=language;
  if(language_path!=NULL)
    (void)g_file_set_contents(language_path,i18n_language_code(),-1,NULL);
}

const char *i18n_tr(const char *english) {
  if(english==NULL || active_language==I18N_EN) return english;
  for(gsize i=0;i<G_N_ELEMENTS(translations);i++) {
    if(strcmp(english,translations[i].en)==0) {
      if(active_language==I18N_RU) return translations[i].ru;
      if(active_language==I18N_UK) return translations[i].uk;
      if(active_language==I18N_BE) return translations[i].be;
    }
  }
  return english;
}
