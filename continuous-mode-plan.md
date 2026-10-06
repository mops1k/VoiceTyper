# Финальный план: режим «Непрерывная диктовка» (Continuous Session) для VoiceTyper

> Сформирован 2026-09-03 в ходе брейншторма поверх существующего проекта (`project-plan.md`).
> Базис без изменений: **C# / WPF / .NET 10**, MVVM + Service Layer, DI, три проекта (Core/App/Tests).

---

## 0. Зафиксированные решения (по итогам уточнений)

| № | Вопрос | Решение |
|---|---|---|
| 1 | Где включается | **4-й режим записи** `RecordingMode.Continuous` в существующем переключателе |
| 2 | Вывод текста | **Вариант А**: каждая фраза сразу → буфер обмена → Ctrl+V в активное поле (как сейчас) |
| 3 | Автоотключение | Отсчёт от **конца последней фразы**; настройка в **секундах**, дефолт **30**; **0 = никогда** |
| 4 | Порог тишины (конец фразы) | Переиспользуем существующий `SilenceThresholdMs` |
| 5 | Повторное нажатие хоткея / Cancel | Прервать текущую фразу и сессию **с отбрасыванием** (без ожидания распознавания) |
| 6 | Речь во время распознавания | Микрофон не закрывается; фразы копятся в **FIFO-очередь**, текст уходит с задержкой |
| 7 | Поведение после старта | **Микрофон занят сразу** (устройство открыто), но **захват фразы начинается ТОЛЬКО при начале речи** (VAD onset); до этого — состояние ожидания |

---

## 1. Архитектура: модель состояний и поток данных

### 1.1 Новое состояние `RecordingState.Listening`

Текущий enum `Idle / Recording / Processing` расширяется до `Idle / Listening / Recording / Processing`.

```
RecordingMode.Continuous — конечный автомат сессии:

                  PressRecord (хоткей)
   Idle ─────────────────────────────────────┐
     ▲                                        ▼
     │  auto-off (тишина ≥ N с) /            Listening ──── речь началась (VAD onset) ────► Recording
     │  PressRecord / CancelHotkey            │  (микрофон ОТКРЫТ, захвата нет,              │
     │                                        │   копится только pre-roll ~0.5 с)            │
     │                                        │                                             ▼
     │                                        │                     тишина ≥ SilenceThresholdMs / лимит длины
     │                                        │                                             │
     │                                        └────────────◄──────────── Processing ◄────────┘
     │                                                        │    (распознавание фразы)
     │                                                        ▼
     │                                          TextReady → буфер → автовставка → сессия ЖИВА
     └────────────────────────────────────────────────────────┘  (снова Listening; цикл)
```

Ключевая особенность: **во время Processing и между фразами микрофон не закрывается** — это главное отличие от существующего режима Vad, который делает одноразовую запись `Idle→Recording→Processing→Idle`.

Поведение методов автомата в режиме Continuous:
- `PressRecord()` из `Idle` → старт сессии (`Listening`).
- `PressRecord()` из `Listening/Recording/Processing` → **стоп сессии с отбрасыванием** (решение 5А).
- `Cancel()` → то же (стоп с отбрасыванием).
- `ReleaseRecord()` → no-op (не push-to-talk).
- Автоотключение по таймеру → **аккуратный стоп**: текущая обрабатываемая фраза доигрывается, очередь отбрасывается, уведомление в трей.

### 1.2 Пайплайн захвата (сессионный, без перезапуска устройства)

Внутри автомата для Continuous запускается **один фоновый цикл сессии** (тик ~100 мс), который работает поверх уже открытого `AudioRecorder`:

```
непрерывный цикл (пока сессия активна):
  raw = recorder.DrainNewBytes()                     // устройство НЕ останавливаем
  floats = WavBuilder.ConvertTo16KHzMonoFloats(raw, CaptureFormat)
  utterance = assembler.Process(floats)              // новый класс: VAD onset/offset + сборка
  if utterance != null → channel.Writer.TryWrite(utterance)   // FIFO (bounded)
  проверка idle-таймера (см. §1.3)
```

Новый чистый класс **`ContinuousSpeechAssembler`** (Core/Audio, чистый C# на `float[]` 16 кГц моно, без NAudio-зависимостей, тестируем):
- держит **pre-roll-кольцо ~0.5 с** — поэтому «захват начинается при начале речи», но первый слог не теряется (VAD-онсет имеет задержку ≥250 мс);
- пока речи нет — фраза **не создаётся** (просто состояние `Listening`);
- речь началась → кольцо pre-roll + последующие чанки накапливаются в буфер фразы;
- тишина после речи ≥ `SilenceThresholdMs` **или** длина фразы > лимит (~30 с, Whisper-контекст) → фраза финализируется, возвращается `float[]`; VAD-состояние сбрасывается для следующей фразы;
- защита от «фантомных» пустых фраз (говорил → замолчал — если речи вообще не было, фраза не создаётся).

**Распознавание — отдельный единственный worker** (однопоточность обязательна: Whisper-процессор не потокобезопасен):
```
FIFO-очередь (Channel.CreateBounded, ~10 элементов) → по одному:
  options = GetTranscriptionOptions() (живые настройки)
  text = transcription.TranscribeAsync(wav, ...)
  if не пусто и сессия активна → output.OutputAsync(text, autoPaste) → TextReady
```
Пока worker занят, цикл захвата **продолжает работать** — новая фраза попадает в очередь (решение 6). Если очередь заполнена (CPU не успевает), новые фразы не открываются — выводится предупреждение в статус/лог (см. риски).

### 1.3 Idle-таймер автоотключения (в секундах, 0 = выкл)

- Момент отсчёта — **`_lastPhraseEndUtc`**: устанавливается при финализации каждой фразы, а при старте сессии (если фраз ещё не было) — на момент старта.
- Таймер проверяется в цикле **только когда фраза не собирается** (`assembler` пуст).
- Если worker ещё обрабатывает последнюю фразу — таймер «ждёт» её завершения и стопается сразу после вывода текста (ничего не теряется).
- Срабатывание: `recorder` закрывается, состояние → `Idle`, в трей — balloon «Непрерывный режим отключён: тишина N с», событие `SessionEnded`.
- `ContinuousIdleTimeoutSec = 0` → таймер не заводится вовсе (микрофон остаётся занят, пока не нажмут хоткей) — фиксируется в README как риск.

---

## 2. Библиотеки на борту (все, со ссылками)

Новых внешних библиотек **не требуется** — вся логика поверх уже стоящего стека. Полный список с официальными ссылками:

| Библиотека / инструмент | Назначение в рамках фичи | Официальная ссылка |
|---|---|---|
| **NAudio** | Захват с микрофона (устройство держится открытым всю сессию), ресемплинг | https://github.com/naudio/NAudio |
| **Whisper.net** + **Whisper.net.Runtime** | Распознавание каждой фразы на CPU (вызовы строго последовательные) | https://github.com/sandrohanea/whisper.net |
| **whisper.cpp** (движок) | ggml-инференс под капотом | https://github.com/ggml-org/whisper.cpp |
| ggml-модели (tiny…large) | RU/EN распознавание | https://github.com/ggml-org/whisper.cpp/blob/master/models/README.md |
| **Silero VAD** (ggml-silero через Whisper.net) | Детекция начала/конца речи в сессии | https://github.com/snakers4/silero-vad |
| **NHotkey.Wpf** | Глобальный хоткей вкл/выкл сессии (без фокуса окна) | https://github.com/thomaslevesque/NHotkey |
| **WindowsInput (InputSimulator)** | Ctrl+V после каждой фразы | https://github.com/michaelnoonan/inputsimulator |
| **CommunityToolkit.Mvvm** | ObservableProperty/RelayCommand в SettingsViewModel | https://github.com/CommunityToolkit/dotnet |
| **Microsoft.Extensions.DependencyInjection** | DI-контейнер | https://learn.microsoft.com/en-us/dotnet/core/extensions/dependency-injection |
| **System.Text.Json** (встроен) | Сериализация новых настроек | https://learn.microsoft.com/en-us/dotnet/standard/serialization/system-text-json/overview |
| **System.Threading.Channels** (встроен в BCL) | FIFO-очередь фраз между циклом захвата и worker'ом распознавания | https://learn.microsoft.com/en-us/dotnet/api/system.threading.channels |
| **xUnit** | Юнит-тесты (assembler, автомат, настройки) | https://xunit.net |
| WPF Clipboard / NotifyIcon (встроены) | Буфер обмена и уведомления трея | https://learn.microsoft.com/en-us/dotnet/api/system.windows.clipboard |
| .resx-локализация (встроена) | RU/EN строки для новой секции настроек и состояний | https://learn.microsoft.com/en-us/dotnet/core/extensions/ |

---

## 3. Модель настроек (`settings.json`)

```json
{
  "recordingMode": "continuous",      // pushToTalk | toggle | vad | continuous  (NEW)
  "silenceThresholdMs": 1200,         // конец фразы — переиспользуется как есть
  "continuousIdleTimeoutSec": 30,     // NEW: автоотключение по тишине; 0 = никогда
  ...
}
```

`AppSettings`:
```csharp
public enum RecordingMode { PushToTalk, Toggle, Vad, Continuous }   // + новый член

/// <summary>Автоотключение непрерывной сессии при тишине, в секундах. 0 = не отключать.</summary>
public int ContinuousIdleTimeoutSec { get; set; } = 30;
```

Валидация в ViewModel: `Math.Clamp(ContinuousIdleTimeoutSec, 0, 3600)`. Валидация порога фразы — без изменений.

---

## 4. Пошаговая реализация (TODO)

### Этап 1. Модель и настройки (Core)
- [ ] `Models/AppSettings.cs`: добавить `RecordingMode.Continuous`, поле `ContinuousIdleTimeoutSec = 30` (+ XML-doc).
- [ ] Убедиться, что camelCase-сериализация даёт `"continuous"` (проверить `SettingsServiceTests`).
- [ ] Тесты (`SettingsServiceTests`): дефолт 30; сериализация/десериализация 0 и 300; чтение старого файла без поля (должен получить дефолт 30).

### Этап 2. Пайплайн захвата (Core, аудио)
- [ ] `Audio/WavBuilder.cs`: добавить публичный `static byte[] Build16KHzMonoWav(float[] mono16kSamples, bool noiseReduction = false)` (переиспользовать внутренний `FloatListSampleProvider` + `SampleToWaveProvider16`; при флаге — `NoiseSuppressor.Process`).
- [ ] **Новый** `Audio/ContinuousSpeechAssembler.cs`:
  ```csharp
  public sealed class ContinuousSpeechAssembler
  {
      public ContinuousSpeechAssembler(ISpeechSegmenter vad,
          TimeSpan silenceThreshold, TimeSpan? preRoll = null, TimeSpan? maxUtterance = null);
      public bool IsCollecting { get; }
      /// <summary>null — фраза не готова; иначе — финализированные сэмплы 16 кГц моно.</summary>
      public float[]? Process(float[] chunk16kMono);
      public void Reset();
  }
  ```
  Логика: pre-roll-кольцо (~0.5 с), онсет → начало сбора, тишина ≥ threshold или лимит ~30 с → финализация; без речи фраза не создаётся.
- [ ] Тесты (`Audio/ContinuousSpeechAssemblerTests.cs` с фейковым `ISpeechSegmenter` по образцу `FakeVadSegmenter` из `RecordingStateMachineTests`): нет речи → нет фраз; онсет со pre-roll; тишина завершает фразу; две фразы подряд; лимит длины; Reset между сессиями.
- [ ] Тесты `WavBuilderTests`: `Build16KHzMonoWav` валидный заголовок/длительность.

### Этап 3. Конечный автомат (Core)
- [ ] `RecordingState` += `Listening` (там же, где объявлен).
- [ ] Разбить `RecordingStateMachine` на `partial`: общий каркас + новый файл `RecordingStateMachine.Continuous.cs` (режим Continuous).
- [ ] Конструктор: добавить параметр `TimeSpan continuousIdleTimeout`.
- [ ] `PressRecord`/`ReleaseRecord`/`Cancel` — ветвления по режиму (§1.1).
- [ ] `StartContinuousSession()`: `recorder.Start()`, состояние `Listening`, создание `assembler` + `Channel.CreateBounded<byte[]>(10)`, запуск цикла сессии и worker'а.
- [ ] Цикл сессии (§1.2) + пересчёт отображаемого состояния (`RefreshContinuousState()`: `Recording`> `Processing` > `Listening`).
- [ ] Worker очереди: последовательная транскрибация, проверка «сессия ещё активна» перед `OutputAsync`, отбрасывание при отмене.
- [ ] `StopContinuousSession(bool discard)`, CancelHotkey и повторный PressRecord — отбрасывают текущее и очередь (решение 5А).
- [ ] Idle-таймер (§1.3) с учётом «фраза в обработке доигрывается».
- [ ] Тесты (`RecordingStateMachineTests`): press → `Listening`; микрофон не закрыт между фразами; две фразы подряд → два `TextReady` в FIFO-порядке; второй press из `Listening` → `Idle`; авто-off по малому idle-тайму (например 300 мс); cancel отбрасывает очередь; в режиме PushToTalk/Toggle/Vad поведение не изменилось (существующие тесты зелёные).

### Этап 4. Оркестрация и статусы (App)
- [ ] `App.xaml.cs` `InitializeEngineAsync`: передать `TimeSpan.FromSeconds(_currentSettings.ContinuousIdleTimeoutSec)` в конструктор автомата.
- [ ] `OnSettingsApplied`: в `behaviorChanged` добавить сравнение `ContinuousIdleTimeoutSec` (пересоздание автомата при изменении, как для `SilenceThresholdMs`).
- [ ] `OnStateChanged`: трей/статус для `Listening` («Ожидание речи…», микрофон «занят» — иконка записи активна для `Listening|Recording`, overlay скрывается только на `Idle`).
- [ ] `OnStateChanged`/overlay: `UpdateStatusOverlay` — case для `Listening` (новый текст + цвет); на авто-off — balloon «Сессия отключена по тишине».
- [ ] Проверить, что «речь во время обработки» не теряется (DrainNewBytes продолжается — тест интеграционный ручной).

### Этап 5. UI и локализация (App + Core)
- [ ] `SettingsViewModel`: добавить `ContinuousIdleTimeoutSec` (ObservableProperty), загрузка/сохранение/кламп/`ResetDefaults`; в `Save()` — `Math.Clamp(0..3600)`.
- [ ] `BuildRecordingModes()` += `new LocalizedOption<RecordingMode>(RecordingMode.Continuous, "Enum_RecordingMode_Continuous")`.
- [ ] `OnRecordingModeChanged`: обновить computed-свойство `IsContinuousMode` (для видимости строки таймаута).
- [ ] `MainWindow.xaml` (секция General): новая строка «Автоотключение по тишине, сек» (TextBox, `Visible` только при `IsContinuousMode` — converter `BoolVis`), рядом строка `SilenceThresholdMs` помечена как «конец фразы» (для Vad и Continuous).
- [ ] resx `Strings.resx` (RU) + `Strings.en.resx` (EN) — новые ключи:
  - `Enum_RecordingMode_Continuous`
  - `General_ContinuousIdleTimeout` / `_Tooltip` («0 — не отключать автоматически»)
  - `Status_Listening`, `Overlay_Listening`
  - `RSM_ContinuousSessionEnded` (balloon авто-off, с форматом N секунд)
  - `Log_*` записи старта/стопа сессии, авто-off, переполнения очереди
  - уточнить tooltip `General_RecordingMode_Tooltip` и `General_SilenceThreshold_Tooltip` (упомянуть Continuous).
- [ ] Тест локализации (`LocTests`) — новые ключи существуют в обоих языках (если такой тест есть — дополнить списком).

### Этап 6. Тесты, полировка, ограничения
- [ ] `dotnet build VoiceTyper.slnx -c Release` зелёный; `dotnet test VoiceTyper.Tests -c Release` зелёный.
- [ ] Ручной E2E: включить режим Continuous; хоткей → появился «Ожидание речи»; сказал фразу → стоп по тишине → текст в поле; пауза 5 с → вторая фраза; хоткей → выход. Сценарий с авто-off (установить 5 с) — balloon, микрофон освобождён. Сценарий «речь во время распознавания» (медленная модель Medium).
- [ ] README.md + README.ru.md: описание режима, настройки `continuousIdleTimeoutSec`, индикатор «микрофон используется», ограничения.

### Этап 7. Публикация
- [ ] Обновить версию продукта (при необходимости) — следовать существующему `.github/workflows/release.yml`.
- [ ] Собрать `dotnet publish VoiceTyper.App -c Release -r win-x64 --self-contained true`; smoke-тест на чистой машине.

---

## 5. Дополнительное ПО (обязательный блок плана)

- **.NET 10 SDK** — https://dotnet.microsoft.com/download/dotnet/10.0
- **MS Visual C++ Redistributable 2015–2022 (x64)** — требуется whisper.cpp — https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist
- Windows 10/11, CPU с AVX/AVX2 — см. README.
- **Базы данных / брокеры сообщений / Docker / Kubernetes — НЕ нужны**: приложение локальное, состояние — `settings.json`. Указано для полноты плана.

---

## 6. Риски и ограничения

1. **Речь во время распознавания** на слабом CPU: очередь FIFO накапливается; текст приходит с задержкой. При переполнении очереди (канал полон) новые фразы не открываются + статус/лог предупреждает. Рекомендация для документации: в Continuous использовать модель не больше `Small`.
2. **Захват «фразы» начинается с pre-roll ~0.5 с**, т.к. VAD-онсет задерживается — первый слог не теряется, но «чистого» нуля до речи в WAV нет (это нормально для Whisper).
3. **Длина фразы ограничена ~30 с** (контекст Whisper): непрерывная речь без пауз будет разбита на несколько фраз и вставлена частями.
4. **0 = «никогда не отключать»**: микрофон остаётся занят и индикатор Windows «микрофон используется» горит постоянно, пока не нажмут хоткей/не выйдут из приложения.
5. **Авто-off при лаге распознавания**: таймер ждёт завершения текущей фразы — автоотключение может наступить чуть позже заданных N секунд (ничего не теряется).
6. **Shared-mode WASAPI**: другие приложения технически могут продолжать захватывать звук одновременно; приложение использует общий режим, поэтому «занятость» не эксклюзивная — зафиксировать в README.
7. Ложные срабатывания VAD от шума/фонового ТВ — лечится порогом тишины и (опционально) включённым подавлением шума.

---

## 7. Критерии готовности

- [ ] 4-й режим выбирается в настройках и применяется без перезапуска.
- [ ] Хоткей запускает сессию: микрофон занят, overlay «Ожидание речи…», **захвата до начала речи нет**.
- [ ] Каждая фраза (речь → тишина ≥ порога) распознаётся и вставляется в активное поле в FIFO-порядке.
- [ ] Микрофон не закрывается между фразами и во время распознавания.
- [ ] Повторный хоткей / Cancel — мгновенный выход с отбрасыванием.
- [ ] Тишина N секунд (настройка, дефолт 30, 0 = выкл) от конца последней фразы → автоотключение + уведомление.
- [ ] Режимы PushToTalk/Toggle/Vad работают как раньше; все тесты зелёные; RU/EN локализация полная.
