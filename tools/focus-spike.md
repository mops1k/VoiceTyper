# focus-spike — пробник «поле ввода + прямая вставка» (Windows)

Диагностический инструмент фазы 0 плана
[«Windows: определение поля ввода и прямая вставка текста»](../../.dsh/plans/windows-opredelenie-polya-vvoda-i-pryamaya-vstav.md).
В приложение не входит, в `voicetyper-deploy` не собирается и нужен только для ответа на
вопрос: видит ли обычный непривилегированный процесс, что фокус стоит в текстовом поле, и
может ли он напечатать туда текст, не трогая буфер обмена.

## Что он делает

- **Проба.** UI Automation: `IUIAutomation::GetFocusedElement` плюс
  `UIA_IsTextPatternAvailablePropertyId`, `UIA_IsValuePatternAvailablePropertyId`,
  `UIA_ValueIsReadOnlyPropertyId`, `UIA_IsPasswordPropertyId` и legacy-роль/состояние MSAA
  (`UIA_LegacyIAccessibleRolePropertyId`/`StatePropertyId`). Вердикт:
  `editable | read_only | none | unknown` с причиной, по которой он выставлен.
  Win32-каретка (`GetGUIThreadInfo` → `hwndCaret`) намеренно не используется: Chromium
  (в том числе Brave) её не выставляет, поэтому инструменты, опирающиеся на каретку, в
  браузере «не видят поле».
- **Вставка.** `SendInput` с `KEYEVENTF_UNICODE`, по одной паре событий на UTF-16 code unit:
  раскладка не важна (кириллица набирается), буфер обмена не используется. До и после
  вставки снимается `GetClipboardSequenceNumber()`, и в лог пишется, изменился ли буфер.
- **Повторная проба** через 150 мс: первый запрос UIA как раз заставляет Chromium построить
  дерево доступности (в его исходниках это `UiaRootObjectId` → `SetUiaRequested()`), поэтому
  одного запроса может не хватить.

## Сборка

Пробник без Qt и без CMake-конфигурации всего проекта — собирается одним вызовом компилятора
(пример пути — MinGW-w64 из WinLibs, установленный через winget):

```powershell
$mingw = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.MCF.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
& "$mingw\g++.exe" -std=c++20 -O2 -Wall -Wextra `
    -o build\focus-spike\focus-spike.exe tools\focus-spike.cpp `
    -luiautomationcore -lole32 -loleaut32 -loleacc -luuid
```

Проверено этой командой из WSL (через interop) 16.10.2026: сборка без предупреждений.

## Запуск

```
focus-spike --watch 300 --hotkey          # матрица полей + вставка по Ctrl+Alt+Shift+F9
focus-spike --insert-after 5000           # подождать 5 с и напечатать образец
focus-spike --insert-after 5000 --text "свой текст"
```

- `--watch <сек>` — раз в 500 мс пишет вердикт в `focus-spike.log` рядом с exe (и в stdout).
- `--insert-after <мс>` — ждёт, пока пользователь переведёт курсор в поле, делает пробу
  дважды, печатает образец и отчитывается о числе символов и о буфере обмена.
- `--hotkey` — `Ctrl+Alt+Shift+F9` делает то же самое в любой момент; окно не забирает фокус.

Образец по умолчанию: `Проверка вставки VoiceTyper: тест 123 — Test 123.`

## Матрица проверок

Поле под фокусом → строка в логе (`watch verdict=…`); вставка → `hotkey type: …` и
`hotkey clipboard: unchanged`.

| Что проверяется | Ожидание |
|---|---|
| Блокнот | `editable` (text_pattern), текст появляется, буфер не меняется |
| Brave: адресная строка (омнибокс) | `editable`, вставка работает |
| Brave: поле ввода на странице | `editable` (Edit/text_pattern), вставка работает |
| contenteditable (комментарий, письмо) | `editable` (Document/text_pattern) |
| Поиск в проводнике | `editable` |
| Windows Terminal / PowerShell | `editable` либо `unknown` — терминал проверяем отдельно |
| Поле в настройках самого VoiceTyper | `editable` |
| Окно, запущенное от администратора | `read_only`/`unknown` (тот же UAC-предел, что у `paste`) |
| Рабочий стол без поля | `none` |

## Что пробник не проверяет

- Приложения, которые вовсе не экспортируют доступность (часть кастомных рендереров) — для них
  вердикт будет `none`, и решение о поведении остаётся за политикой доставки, а не за пробником.
- Прокрутку, drag-and-drop, вставку в терминальные приложения со своим режимом ввода.
- Пробник ничего никуда не пишет, кроме собственного лог-файла, и не меняет настройки VoiceTyper.
