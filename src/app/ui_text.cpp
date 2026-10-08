#include "app/ui_text.hpp"

#include <array>

namespace voicetyper::app {

namespace {

struct Entry {
    const char* ru;
    const char* en;
};

/// One row per UiKey, in the same order as the enum.
constexpr std::array<Entry, 182> kTexts{{
    Entry{" мс", " ms"},
    Entry{"%1 (недоступен)", "%1 (unavailable)"},
    Entry{"%1 · Скорость: %2 · Качество: %3", "%1 · Speed: %2 · Quality: %3"},
    Entry{"Авто", "Auto"},
    Entry{"В режиме VAD запись остановится после этой тишины", "In VAD mode recording stops after this much silence"},
    Entry{"Внешний вид", "Appearance"},
    Entry{"Вставлять автоматически", "Paste automatically"},
    Entry{"Движок распознавания", "Recognition engine"},
    Entry{"Движок распознавания ещё не готов", "The recognition engine is not ready yet"},
    Entry{"Журнал", "Log"},
    Entry{"Записать", "Record"},
    Entry{"Записать комбинацию", "Capture combination"},
    Entry{"Запись", "Record"},
    Entry{"Запуск", "Startup"},
    Entry{"Запускать вместе с Windows", "Start with Windows"},
    Entry{"Запускать свёрнутым", "Start minimized"},
    Entry{"Комбинация: %1", "Combination: %1"},
    Entry{"Микрофон", "Microphone"},
    Entry{"Модели", "Models"},
    Entry{"На этой системе захват комбинации недоступен", "Capturing a combination is not available on this system"},
    Entry{"Нажмите кнопку, затем нажмите нужную комбинацию (Escape — отмена)", "Press the button, then press the combination (Escape to cancel)"},
    Entry{"Нажмите комбинацию… (Escape — отмена)", "Press a combination… (Escape to cancel)"},
    Entry{"О программе", "About"},
    Entry{"Общие", "General"},
    Entry{"Ожидание комбинации…", "Waiting for a combination…"},
    Entry{"Остановить", "Stop"},
    Entry{"Отмена", "Cancel"},
    Entry{"По умолчанию", "Default"},
    Entry{"Порог тишины (VAD)", "Silence threshold (VAD)"},
    Entry{"Режим записи", "Recording mode"},
    Entry{"Русский", "Russian"},
    Entry{"Скачать", "Download"},
    Entry{"Скрывать при потере фокуса", "Hide on focus loss"},
    Entry{"Словарь терминов", "Terms dictionary"},
    Entry{"Тема", "Theme"},
    Entry{"Тема: %1 · Автосохранение: %2 · Режим: %3", "Theme: %1 · Autosave: %2 · Mode: %3"},
    Entry{"Температура", "Temperature"},
    Entry{"Удалить", "Delete"},
    Entry{"Удалить файл модели с диска", "Delete the model file from disk"},
    Entry{"Устройство", "Device"},
    Entry{"Учитывать предыдущий текст", "Condition on previous text"},
    Entry{"Финальный результат всегда использует 3 кандидата", "The final result always uses 3 candidates"},
    Entry{"Хоткеи", "Hotkeys"},
    Entry{"Шумоподавление", "Noise reduction"},
    Entry{"Язык интерфейса", "Interface language"},
    Entry{"Язык распознавания", "Recognition language"},
    Entry{"Движок не инициализирован", "Engine not initialized"},
    Entry{"диктовка голосом", "voice dictation"},
    Entry{"изменено", "modified"},
    Entry{"микрофон недоступен на этой платформе", "the microphone is unavailable on this platform"},
    Entry{"светлая", "light"},
    Entry{"системная", "system"},
    Entry{"сохранено", "saved"},
    Entry{"тёмная", "dark"},
    Entry{"хоткеи не зарегистрированы", "hotkeys are not registered"},
    Entry{"Вариантов распознавания", "Recognition candidates"},
    Entry{"Сколько вариантов распознавания движок сравнивает между собой. Больше — точнее, но медленнее",
        "How many recognition candidates the engine compares. More is more accurate but slower"},
    Entry{"Чувствительность микрофона", "Microphone sensitivity"},
    Entry{"Проверить микрофон", "Test the microphone"},
    Entry{"Проверка…", "Testing…"},
    Entry{"Система не даёт управлять уровнем микрофона", "The system does not allow controlling the microphone level"},
    Entry{"микрофон слышит звук", "the microphone hears sound"},
    Entry{"ничего не слышно: проверьте, что микрофон подключён и уровень не на нуле",
        "nothing heard: check that the microphone is connected and its level is not zero"},
    Entry{"Версия", "Version"},
    Entry{"Проверить обновления", "Check for updates"},
    Entry{"Проверка обновлений…", "Checking for updates…"},
    Entry{"Обновлений нет", "You have the latest version"},
    Entry{"Доступна версия %1", "Version %1 is available"},
    Entry{"Скачать и установить", "Download and install"},
    Entry{"Загрузка обновления…", "Downloading the update…"},
    Entry{"Обновление установлено, приложение перезапустится", "The update is installed; the application will restart"},
    Entry{"Не удалось проверить обновления", "Could not check for updates"},
    Entry{"Не удалось установить обновление", "Could not install the update"},
    Entry{"Обновления ещё не проверялись", "Updates have not been checked yet"},
    Entry{"VoiceTyper — нативная версия на C++20 и Qt 6.", "VoiceTyper is a native C++20 and Qt 6 build."},
    Entry{"Распознавание выполняется локально; данные никуда не отправляются.",
        "Recognition runs locally; nothing is sent anywhere."},
    Entry{"Обновление", "Update"},
    Entry{"Захват", "Capture"},
    Entry{"Распознавание", "Recognizing"},
    Entry{"Ошибка", "Error"},
    Entry{"Открыть настройки", "Open settings"},
    Entry{"Записать", "Record"},
    Entry{"Остановить", "Stop"},
    Entry{"Выход", "Quit"},
    Entry{"запись", "recording"},
    Entry{"ошибка", "error"},
    Entry{"текст передан", "text pasted"},
    Entry{"Движок готов", "Engine ready"},
    Entry{"загрузка модели…", "loading the model…"},
    Entry{"прогрев модели…", "warming the model up…"},
    Entry{"модель не выбрана", "no model selected"},
    Entry{"Движок недоступен", "Engine unavailable"},
    Entry{"ошибка загрузки", "load failed"},
    Entry{"Движок не инициализирован", "The engine is not initialised"},
    Entry{"Хоткей", "Hotkey"},
    Entry{"Хоткей не зарегистрирован", "The hotkey is not registered"},
    Entry{"журнал недоступен", "the log is unavailable"},
    Entry{"микрофон недоступен: устройство отключено или занято",
        "microphone unavailable: the device is disconnected or busy"},
    Entry{"не удалось зарегистрировать запуск вместе с Windows",
        "could not register the run-at-Windows-startup entry"},
    Entry{"Удалить модель", "Delete model"},
    Entry{"Удалить файл модели «%1» с диска?", "Delete the model file \u201c%1\u201d from disk?"},
    Entry{"движок не инициализирован (нет активного бэкенда)",
        "the engine is not initialised (no active backend)"},
    Entry{"хоткеи не зарегистрированы", "the hotkeys are not registered"},
    Entry{"сборка обновления для нативной версии ещё не опубликована",
        "the update build for the native version is not published yet"},
    Entry{"Whisper — классический движок (модели 40–800 МБ). Parakeet (NVIDIA, 0.6B) — "
          "мультиязычный (25 языков, вкл. русский), качество уровня large при скорости small; "
          "модель 0.6–0.9 ГБ.",
        "Whisper is the classic engine (models 40–800 MB). Parakeet (NVIDIA, 0.6B) is "
        "multilingual (25 languages, Russian included), with large-level quality at small "
        "speed; the model is 0.6–0.9 GB."},
    Entry{"Скачивание…", "Downloading…"},
    Entry{"Не удалось скачать модель", "Could not download the model"},
    Entry{"Скачивание: %1%", "Downloading: %1%"},
    Entry{"Загрузка отменена", "The download was cancelled"},
    Entry{"Отменить загрузку", "Cancel the download"},
    Entry{"Термины через запятую ищутся и по звучанию (\u00abкомит\u00bb \u2192 commit). Пара со знаком "
          "\u00ab=\u00bb \u2014 строгое правило: \u00abкомит=commit\u00bb.",
        "Comma-separated terms are also matched by sound (\u00ab\u043a\u043e\u043c\u0438\u0442\u00bb "
        "\u2192 commit). A pair with '=' is an exact rule: \u00ab\u043a\u043e\u043c\u0438\u0442=commit\u00bb."},
    Entry{"Настройки", "Settings"},
    Entry{"Совет", "Tip"},
    Entry{"Язык и ввод", "Language and input"},
    Entry{"Основные параметры распознавания", "Core recognition settings"},
    Entry{"Язык меню и системных сообщений", "The language of menus and system messages"},
    Entry{"Основной язык вашей речи", "The language you speak"},
    Entry{"Как запускать и останавливать диктовку", "How dictation starts and stops"},
    Entry{"Поведение", "Behaviour"},
    Entry{"Обработка и вставка распознанного текста", "How recognised text is handled and pasted"},
    Entry{"Отправлять текст в активное поле после распознавания", "Send the text to the focused field after recognition"},
    Entry{"Сохранять контекст для более точной пунктуации", "Keep the context for more accurate punctuation"},
    Entry{"Пауза, после которой фраза считается завершённой", "The pause after which a phrase is finished"},
    Entry{"Для быстрой диктовки удерживайте %1 в любом приложении.", "For quick dictation hold %1 in any application."},
    // Страница «Запуск».
    Entry{"Автозапуск", "Autostart"},
    Entry{"Как VoiceTyper запускается вместе с системой", "How VoiceTyper starts together with the system"},
    Entry{"VoiceTyper будет готов к работе сразу после входа", "VoiceTyper will be ready right after you sign in"},
    Entry{"Не показывать главное окно при автоматическом запуске", "Do not show the main window on an automatic start"},
    Entry{"VoiceTyper работает в фоне", "VoiceTyper runs in the background"},
    Entry{"Открыть приложение можно через значок в системном трее.",
        "You can open the application from the system tray icon."},
    // Страница «Хоткеи».
    Entry{"Глобальные сочетания", "Global shortcuts"},
    Entry{"Работают в любом приложении, пока VoiceTyper запущен",
        "They work in any application while VoiceTyper is running"},
    Entry{"Начать или завершить диктовку", "Start or finish dictation"},
    Entry{"Отменить текущую запись без вставки текста", "Cancel the current recording without pasting text"},
    Entry{"Изменить", "Change"},
    Entry{"Нажмите клавиши…", "Press the keys…"},
    Entry{"Подсказка", "Tip"},
    Entry{"Сочетания не должны конфликтовать с системными горячими клавишами Windows",
        "Shortcuts must not conflict with Windows system hotkeys"},
    // Страница «Журнал».
    Entry{"Системный журнал", "System log"},
    Entry{"События текущего сеанса VoiceTyper", "Events of the current VoiceTyper session"},
    Entry{"Копировать", "Copy"},
    Entry{"Очистить", "Clear"},
    Entry{"Журнал очищен", "The log is cleared"},
    // Страница «Микрофон» (макет Figma Make: index.css .range-row, .microphone-test).
    Entry{"Источник звука", "Sound source"},
    Entry{"Выберите микрофон и настройте уровень входного сигнала",
        "Choose a microphone and set the input level"},
    Entry{"Используется для записи и проверки звука", "Used for recording and checking the sound"},
    Entry{"Отрегулируйте усиление входного сигнала", "Adjust the input gain"},
    Entry{"Убедитесь, что VoiceTyper хорошо вас слышит", "Make sure VoiceTyper hears you well"},
    Entry{"Говорите — мы анализируем уровень сигнала", "Speak — we are analysing the signal level"},
    Entry{"Начать проверку", "Start the test"},
    Entry{"Обработка звука", "Audio processing"},
    Entry{"Улучшение качества записи перед распознаванием",
        "Improving the recording quality before recognition"},
    Entry{"Уменьшает постоянный фоновый шум", "Reduces constant background noise"},
    // Страница «О программе» (макет Figma Make: index.css .about-hero, .about-links).
    Entry{"Локальная голосовая диктовка для Windows", "Local voice dictation for Windows"},
    Entry{"О приложении", "About the application"},
    Entry{"Информация о сборке и обновлениях", "Build and update information"},
    Entry{"Ваши данные остаются на устройстве", "Your data stays on your device"},
    Entry{"Лицензии", "Licences"},
    Entry{"Политика конфиденциальности", "Privacy policy"},
    Entry{"Сообщить о проблеме", "Report a problem"},
    Entry{"Проверить наличие новой версии приложения", "Check whether a new version is available"},
    Entry{"Тема приложения", "Application theme"},
    Entry{"Выберите комфортное оформление интерфейса", "Choose a comfortable look for the interface"},
    Entry{"Системная", "System"},
    Entry{"Светлая", "Light"},
    Entry{"Тёмная", "Dark"},
    Entry{"Интерфейс", "Interface"},
    Entry{"Плотность и визуальные эффекты", "Density and visual effects"},
    Entry{"Сворачивать окно после перехода в другое приложение",
        "Minimise the window after switching to another application"},
    Entry{"Вернуть стандартное оформление", "Restore the default look"},
    Entry{"Активна", "Active"},
    Entry{"Движок и модели", "Engine and models"},
    Entry{"Локальное распознавание речи без отправки данных в облако",
        "Local speech recognition without sending data to the cloud"},
    Entry{"Активный движок обработки аудио", "The active audio processing engine"},
    Entry{"Точность распознавания", "Recognition accuracy"},
    Entry{"Расширенные параметры генерации текста", "Advanced text generation parameters"},
    Entry{"Низкие значения дают более предсказуемый результат", "Lower values give a more predictable result"},
    Entry{"Количество вариантов для выбора лучшего результата",
        "How many candidates the best result is chosen from"},
    Entry{"Слова и названия, которые модель должна распознавать точнее",
        "Words and names the model should recognize more accurately"},
    Entry{"ожидание", "idle"},
    Entry{"идёт запись", "recording"},
    Entry{"Хоткей", "Hotkey"},
}};

} // namespace

// The single guard that makes the table and the enum impossible to drift apart.
static_assert(kTexts.size() == static_cast<std::size_t>(UiKey::kCount),
    "ui_text: every UiKey needs exactly one table entry, in the same order");

namespace {
domain::AppLanguage g_current_language = domain::AppLanguage::ru;
} // namespace

void set_current_language(domain::AppLanguage language)
{
    g_current_language = language;
}

domain::AppLanguage current_language()
{
    return g_current_language;
}

QString ui_text(UiKey key, domain::AppLanguage language)
{
    const auto index = static_cast<std::size_t>(key);
    if (index >= kTexts.size()) {
        return QString();
    }
    const Entry& entry = kTexts[index];
    const char* text = language == domain::AppLanguage::en ? entry.en : entry.ru;
    return QString::fromUtf8(text);
}

} // namespace voicetyper::app
