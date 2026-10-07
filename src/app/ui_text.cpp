#include "app/ui_text.hpp"

#include <array>

namespace voicetyper::app {

namespace {

struct Entry {
    const char* ru;
    const char* en;
};

/// One row per UiKey, in the same order as the enum.
constexpr std::array<Entry, 107> kTexts{{
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
    Entry{"движок не инициализирован", "engine not initialized"},
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
    Entry{"движок готов", "engine ready"},
    Entry{"загрузка модели…", "loading the model…"},
    Entry{"прогрев модели…", "warming the model up…"},
    Entry{"модель не выбрана", "no model selected"},
    Entry{"движок недоступен", "engine unavailable"},
    Entry{"ошибка загрузки", "load failed"},
    Entry{"движок не инициализирован", "the engine is not initialised"},
    Entry{"хоткей", "hotkey"},
    Entry{"хоткей не зарегистрирован", "the hotkey is not registered"},
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
    Entry{"Скачивание…", "Downloading…"},
    Entry{"Не удалось скачать модель", "Could not download the model"},
    Entry{"Whisper — классический движок (модели 40–800 МБ). Parakeet (NVIDIA, 0.6B) — "
          "мультиязычный (25 языков, вкл. русский), качество уровня large при скорости small; "
          "модель 0.6–0.9 ГБ.",
        "Whisper is the classic engine (models 40–800 MB). Parakeet (NVIDIA, 0.6B) is "
        "multilingual (25 languages, Russian included), with large-level quality at small "
        "speed; the model is 0.6–0.9 GB."},
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
