# Финальный план: VoiceTyper — локальный speech-to-text с глобальными хоткеями

> Сформирован 2026-09-01 в ходе архитектурного брейншторма (brainstorm skill).
> Стек: C# / WPF / .NET 10 (net10.0-windows).

## Зафиксированные решения

- **Стек:** C# / WPF / .NET 10 (net10.0-windows)
- **Архитектура:** MVVM (CommunityToolkit.Mvvm) + Service Layer, DI
- **Приложение:** живёт в трее, окно настроек открывается по клику на иконку
- **Триггер записи:** настраиваемый (push-to-talk / toggle / авто-стоп по тишине через VAD)
- **Языки:** русский + английский (мультиязычная модель Whisper), техтермины — через словарь в настройках (initial prompt)
- **Автовставка:** вкл по умолчанию, настройка-переключатель
- **Обработка речи:** после остановки записи (не стриминг)

## 1. Архитектура и стек

| Слой | Технология |
|---|---|
| Платформа | Windows Desktop, WPF, `net10.0-windows` |
| Паттерн | MVVM + Service Layer, DI |
| Инференс | Whisper на CPU (whisper.cpp через Whisper.net) |
| Запись | NAudio (WASAPI), 16 кГц / моно / 16-bit PCM |
| Хоткеи | NHotkey.Wpf (глобальные `RegisterHotKey`) |
| Вставка | WindowsInput (SendInput → Ctrl+V) + WPF Clipboard |
| Настройки | System.Text.Json → `%APPDATA%\VoiceTyper\settings.json` |
| Модели | `%LOCALAPPDATA%\VoiceTyper\models\` (скачиваются по URL с HuggingFace; q5-квантизация ggml) |

### Структура решения (3 проекта)

```
VoiceTyper.sln
├── VoiceTyper.Core/            # net10.0, без WPF-зависимостей
│   ├── Models/AppSettings.cs   # модель настроек + enum'ы
│   ├── Services/SettingsService.cs
│   ├── Services/AudioRecorder.cs          (NAudio)
│   ├── Services/TranscriptionService.cs   (Whisper.net)
│   ├── Services/ModelManager.cs           (URL-загрузка ggml, Silero VAD)
│   ├── Services/TextOutputService.cs      (Clipboard + Paste)
│   └── Abstractions/*.cs       # интерфейсы для тестируемости
├── VoiceTyper.App/             # WPF (переносим App.xaml, MainWindow.xaml)
│   ├── ViewModels/SettingsViewModel.cs
│   ├── Views/MainWindow.xaml   # страница настроек
│   ├── Services/HotkeyService.cs          (NHotkey.Wpf)
│   ├── Services/RecordingStateMachine.cs  (Idle→Recording→Processing→Output)
│   ├── Tray/TrayIcon.cs        (NotifyIcon)
│   └── App.xaml.cs             # CompositionRoot (DI), single-instance mutex
└── VoiceTyper.Tests/           # xUnit
```

### Поток данных

```
горячая клавиша → RecordingStateMachine → AudioRecorder (WAV)
  → TranscriptionService (Whisper CPU) → TextOutputService
  → буфер обмена → [опционально] Ctrl+V в активное поле
```

## 2. Библиотеки на борту (все, со ссылками)

| Библиотека | Назначение | Официальная ссылка |
|---|---|---|
| **Whisper.net** + **Whisper.net.Runtime** (CPU-рантайм) | Инференс Whisper на CPU | https://github.com/sandrohanea/whisper.net |
| **whisper.cpp** (движок под капотом) | Нативная реализация Whisper, ggml-модели | https://github.com/ggml-org/whisper.cpp |
| Модели ggml (tiny/base/small/medium/large) | Мультиязычные модели для RU+EN (НЕ `.en`-варианты) | https://github.com/ggml-org/whisper.cpp/blob/master/models/README.md |
| **Silero VAD** (ggml-silero) | Авто-стоп записи по тишине (режим VAD) | https://github.com/snakers4/silero-vad |
| **NAudio** | Захват звука с микрофона (WASAPI), запись WAV | https://github.com/naudio/NAudio |
| **NHotkey.Wpf** | Глобальные хоткеи без фокуса окна | https://github.com/thomaslevesque/NHotkey |
| **WindowsInput (InputSimulator)** | Симуляция Ctrl+V через SendInput | https://github.com/michaelnoonan/inputsimulator |
| **CommunityToolkit.Mvvm** | MVVM-база (ObservableObject, RelayCommand) | https://github.com/CommunityToolkit/dotnet |
| **Microsoft.Extensions.DependencyInjection** | DI-контейнер | https://learn.microsoft.com/en-us/dotnet/core/extensions/dependency-injection |
| **System.Text.Json** (встроен) | Сериализация настроек | https://learn.microsoft.com/en-us/dotnet/standard/serialization/system-text-json/overview |
| **xUnit** | Юнит-тесты | https://xunit.net |
| **WPF Clipboard / NotifyIcon** (встроены) | Буфер обмена и трей | https://learn.microsoft.com/en-us/dotnet/api/system.windows.clipboard |

### Опционально (расширения)

- **sherpa-onnx** — специализированные RU-модели (Zipformer-ru, GigaAM-russian), если точность Whisper на русском не устроит | https://github.com/k2-fsa/sherpa-onnx
- **Inno Setup** — инсталлятор | https://jrsoftware.org/isinfo.php
- **GitHub Actions** — CI (build + test + publish) | https://docs.github.com/en/actions

## 3. Дополнительное ПО

- **.NET 10 SDK** — https://dotnet.microsoft.com/download/dotnet/10.0 (уже используется)
- **Microsoft Visual C++ Redistributable 2015–2022 (x64)** — требуется whisper.cpp на Windows | https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist
- **Windows 11+** — рекомендовано (Whisper.net), CPU с **AVX/AVX2** (иначе рантайм `Whisper.net.Runtime.NoAvx`)
- **Базы данных / брокеры сообщений / Docker / Kubernetes — НЕ нужны** (локальное десктоп-приложение, всё состояние — JSON-файл). Указано явно для полноты плана.

## 4. Модель настроек (`settings.json`)

```json
{
  "RecordingMode": "PushToTalk",        // PushToTalk | Toggle | Vad
  "RecordHotkey": "Ctrl+Alt+Space",
  "CancelHotkey": "Ctrl+Alt+Escape",
  "Language": "ru",                     // ru | en | auto
  "ModelSize": "Small",                 // Tiny | Base | Small | Medium | Large
  "AutoPasteEnabled": true,             // вкл по умолчанию
  "TermsDictionary": "CPU,GPU,ASR,TTS,API,LLM,on-premise,...",
  "SilenceThresholdMs": 1200,           // для режима VAD
  "StartWithWindows": false,
  "StartMinimized": true
}
```

Технические термины из `TermsDictionary` подмешиваются в **initial prompt** (`WithPrompt`) — Whisper биасит распознавание к этим словам.

## 5. Пошаговая реализация

### Этап 0. Инициализация и структура решения

- [ ] `git init` + `.gitignore` (`bin/`, `obj/`, `*.user`, `.vs/`)
- [ ] Создать `VoiceTyper.Core` (classlib), `VoiceTyper.Tests` (xunit); перенести `App.xaml`/`MainWindow.xaml` в `VoiceTyper.App`
- [ ] Прописать ссылки проектов, собрать решение (`dotnet build` — зелёный)

### Этап 1. Настройки (Core)

- [ ] `AppSettings`, enum'ы, парсер хоткея `string ↔ (Key, ModifierKeys)`
- [ ] `SettingsService`: загрузка/сохранение JSON, значения по умолчанию, атомарная запись (tmp-файл + rename)
- [ ] Юнит-тесты: сериализация, парсинг хоткеев, дефолты

### Этап 2. Запись звука (Core)

- [ ] `IAudioRecorder` + реализация на NAudio (`WasapiCapture` → WAV 16 кГц моно; ресемплинг, если устройство отдаёт 48 кГц)
- [ ] `Start()`/`Stop()`/`Cancel()`, контроль уровня (необязательный пик-индикатор)
- [ ] Тест: валидность WAV-заголовка, длительность

### Этап 3. Транскрибация (Core)

- [ ] `ModelManager`: загрузка ggml-модели выбранного размера по URL с HuggingFace (q8-квантизация) в `%LOCALAPPDATA%\VoiceTyper\models` (с проверкой `File.Exists` и атомарной записью)
- [ ] Загрузка Silero VAD-модели для режима Vad
- [ ] `ITranscriptionService` + `WhisperFactory.FromPath(model)`; builder: `WithLanguage("ru"|"en"|"auto")`, `WithPrompt(termsDictionary)`, `WithThreads(CPU/2)`
- [ ] Кэшировать `WhisperFactory`/processor (не пересоздавать на каждое распознавание — модель грузится ~1–3 сек)
- [ ] Юнит-тесты с фейковым движком (интерфейс отделяет Whisper.net от логики)

### Этап 4. Вывод текста (Core)

- [ ] `IClipboardService.SetText` — машаллинг на STA-поток (Dispatcher), т.к. обработка идёт в фоне
- [ ] `IPasteService.Paste` — WindowsInput `SendInput` Ctrl+V (с задержкой ~50–100 мс после записи в буфер)
- [ ] `TextOutputService`: буфер → если `AutoPasteEnabled` — вставка, иначе звуковой сигнал/уведомление
- [ ] Тесты: логика выбора ветки (mock-интерфейсы)

### Этап 5. Хоткеи и конечный автомат (App)

- [ ] `HotkeyService` на NHotkey.Wpf: регистрация Record + Cancel из настроек; перерегистрация при изменении настроек; обработка коллизий (уведомление)
- [ ] `RecordingStateMachine`: `Idle → Recording → Processing → Output → Idle`; обработка отмены
- [ ] Режимы: PushToTalk (держишь — пишешь), Toggle (нажал-нажал), Vad (фреймы идут в Silero VAD; тишина `SilenceThresholdMs` → стоп)
- [ ] Защита от повторного входа (пока `Processing` — повторный хоткей игнорируется)

### Этап 6. UI (App, MVVM)

- [ ] Окно настроек: поля горячих клавиш (с захватом нажатия), язык, размер модели, режим записи (RadioButton), чекбокс автовставки, словарь терминов, автозапуск (`Registry.CurrentUser\...\Run`), старт свёрнутым
- [ ] Индикатор состояния (Idle/Recording/Processing) + уведомления трея
- [ ] `NotifyIcon`: меню «Открыть настройки / Запись / Выход», закрытие окна → в трей

### Этап 7. Оркестрация (App)

- [ ] `App.xaml.cs`: DI-регистрации, single-instance `Mutex`, загрузка настроек до показа окна
- [ ] Применение настроек на лету (сохранение → перерегистрация хоткеев без перезапуска)

### Этап 8. Тесты и полировка

- [ ] Юнит-тесты всех сервисов Core (настройки, парсинг, автомат, ветки вывода)
- [ ] Ручной E2E-сценарий: запись → распознавание → буфер → автовставка в Word/браузер/IDE
- [ ] Проверка на русском + английском + с техтерминами

### Этап 9. Публикация и деплой

- [ ] `dotnet publish -c Release -r win-x64 --self-contained` (native-рантаймы Whisper идут рядом с exe)
- [ ] README: требования (VC++ redist, AVX), инструкция первого запуска (скачивание модели)
- [ ] Опционально: Inno Setup-инсталлятор, GitHub Actions (build → test → publish artifact)

## 6. Известные ограничения и риски

- **Автовставка (Ctrl+V) не работает в окнах, запущенных с повышенными правами (UAC)**, если приложение запущено не от администратора — зафиксировать в README; работающее решение — оба процесса с одинаковым уровнем прав.
- Качество русского у `small` среднее — по умолчанию ставим `Small` (q8, ~252 МБ), для точности — `Medium` (q8, ~785 МБ). Словарь терминов компенсирует недостаток модели.
- Настройки приватности Windows («Доступ к микрофону») должны разрешать приложению запись.
- Хоткей может конфликтовать с системным — NHotkey вернёт ошибку регистрации, покажем уведомление.
- Модель качается один раз (~42 МБ–834 МБ, q8-квантизация) — нужен интернет при первом запуске.

## 7. Критерии готовности MVP

- [ ] Горячая клавиша запускает запись в любом приложении (окно не в фокусе)
- [ ] Распознавание RU/EN на CPU без GPU за приемлемое время
- [ ] Текст попадает в буфер обмена; при включённой автовставке — сразу в активное поле
- [ ] Все настройки (хоткеи, язык, модель, режим, автовставка, словарь) редактируются в окне и применяются без перезапуска
- [ ] Приложение стартует с Windows (опционально) и сворачивается в трей
