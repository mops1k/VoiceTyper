# THIRD_PARTY_NOTICES / Сторонние компоненты

## parakeet.cpp (MIT)

Нативный рантайм распознавания Parakeet (`native/parakeet.dll`,
сборка описана в `native/parakeet/BUILD.txt`,
репозиторий: https://github.com/mudler/parakeet.cpp).

Сборка VoiceTyper включает производные файлы лицензии MIT:

```
MIT License

Copyright (c) mudler

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Веса модели Parakeet v3 (CC-BY-4.0)

Модель распознавания `parakeet-tdt-0.6b-v3` (и её GGUF-кванты в репозитории
mudler/parakeet-cpp-gguf) выпущена NVIDIA под лицензией Creative Commons
Attribution 4.0 (CC-BY-4.0):

- Модель: https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3
- GGUF-кванты: https://huggingface.co/mudler/parakeet-cpp-gguf
- Лицензия: https://creativecommons.org/licenses/by/4.0/

Веса НЕ включены в поставку VoiceTyper: пользователь загружает модель самостоятельно
через приложение (раздел «Модели») с HuggingFace. После загрузки распознавание выполняется
полностью локально.

## whisper.cpp

- whisper.cpp: https://github.com/ggml-org/whisper.cpp (MIT).
- Весовые модели Whisper с https://huggingface.co/ggerganov/whisper.cpp — см. карточки моделей
  (MIT/CC-BY в зависимости от модели).
- Silero VAD: https://github.com/snakers4/silero-vad (MIT).

## llama.cpp (MIT)

Рантайм необязательной локальной чистки текста (этап 2, план p_5d68d66503ca).

- llama.cpp: https://github.com/ggml-org/llama.cpp (MIT), пин — коммит
  `d81235049384534c167caea52b85a694f6103d14` (upstream v0.6.0, 2026-10-05), архив
  37 887 730 байт,
  sha256 `6b58785f0a82898f4c3442417ff962e2d4b231b1bee5d033902ad90b27901e14`.
- ggml, входящий в llama.cpp: Copyright (c) 2023-2026 The ggml authors, MIT, версия 0.26.0.
- Собирается в отдельную разделяемую библиотеку `voicetyper_llm.so` / `voicetyper_llm.dll`,
  llama.cpp и ggml лежат внутри неё статически. Причина отдельной библиотеки: whisper.cpp,
  который линкуется в это же приложение, несёт ggml 0.25.1, и общий набор символов `ggml_*`
  их обслуживать не может.
- Пин (репозиторий, коммит, адрес архива, его размер и sha256, контент-хеши файлов API и
  сборки) — `native/llm/LlamaPins.cmake`; проверка идёт при скачивании и повторно после
  распаковки, расхождение — фатальная ошибка сборки.

## Модели чистки текста (этап 2, план p_5d68d66503ca)

Необязательные локальные модели, которые убирают слова-паразиты из готового транскрипта.
Скачиваются по требованию пользователя через магазин моделей (`ModelKind::cleanup`), после
скачивания интернет не нужен; работают через llama.cpp (раздел ниже).

| Размер | Модель | Файл | Байт | sha256 | Лицензия |
|---|---|---|---|---|---|
| маленькая | Qwen2.5-0.5B-Instruct | `qwen2.5-0.5b-instruct-q4_k_m.gguf` | 491 400 032 | `74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db` | Apache-2.0 |
| средняя | Qwen2.5-1.5B-Instruct | `qwen2.5-1.5b-instruct-q4_k_m.gguf` | 1 117 320 736 | `6a1a2eb6d15622bf3c96857206351ba97e1af16c30d7a74ee38970e434e9407e` | Apache-2.0 |
| большая | Llama-3.2-3B-Instruct | `Llama-3.2-3B-Instruct-Q4_K_M.gguf` | 2 019 377 696 | `6c1a2b41161032677be168d354123594c0e6e67d2b9227c84f296ad037c728ff` | Llama 3.2 Community License |

Источники весов:

- https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF (Apache-2.0);
- https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF (Apache-2.0);
- https://huggingface.co/bartowski/Llama-3.2-3B-Instruct-GGUF — Llama 3.2 Community License;
  текст лицензии прилагается к сборке вместе с уведомлениями.

Qwen2.5-3B-Instruct в список не вошла: её карточка помечена лицензией `other` (Qwen Research
License, только исследования), что для продукта не годится (проверено 14.10.2026, решение
Александра — взять Llama-3.2-3B вместо неё).

## transcribe.cpp (MIT)

Нативный рантайм распознавания GigaAM (`native/transcribe/libtranscribe.dll` и его
спутники `ggml.dll`, `ggml-base.dll`, `ggml-cpu.dll`; сборка описана в
`native/transcribe/BUILD.txt`, репозиторий:
https://github.com/handy-computer/transcribe.cpp, pin коммита
`3f32fbcc7bb3246851a0234263438bc3c0fa1cac`, тег v0.3.1).

Сборка VoiceTyper включает производные файлы лицензии MIT:

```
MIT License

Copyright (c) 2026 the transcribe.cpp authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

Вендоренные внутри transcribe.cpp компоненты ggml и miniz также распространяются
под MIT (https://github.com/ggml-org/ggml, https://github.com/richgel999/miniz).

## Веса модели GigaAM-v3 (MIT)

Модель распознавания `GigaAM-v3` (вариант `e2e_rnnt`, GGUF-кванты в репозитории
handy-computer/gigaam-v3-e2e-rnnt-gguf) выпущена SberDevices/SaluteDevices под
лицензией MIT — мягче, чем CC-BY-4.0 у весов Parakeet:

- Модель: https://huggingface.co/ai-sage/GigaAM-v3
- GGUF-кванты: https://huggingface.co/handy-computer/gigaam-v3-e2e-rnnt-gguf
- Лицензия: https://github.com/salute-developers/GigaAM/blob/main/LICENSE

Веса НЕ включены в поставку VoiceTyper: пользователь загружает файл
`gigaam-v3-e2e-rnnt-<квант>.gguf` через раздел «Модели» с HuggingFace. После
загрузки распознавание выполняется полностью локально, без сети (PRIV-01).

## Встроенные шрифты

### Fluent System Icons (MIT)

Иконки левого меню (`assets/fonts/FluentSystemIcons-Regular.ttf`, код в
`src/ui/main_window.cpp` и `src/ui/application_font.cpp`), вшиты в исполняемый файл
ресурсом `:/fonts/FluentSystemIcons-Regular.ttf` (VT-UI-1005b).
Источник: https://github.com/microsoft/fluentui-system-icons.
Текст лицензии: `assets/fonts/LICENSE-FluentSystemIcons.txt`.

```
MIT License

Copyright (c) 2020 Microsoft Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### Source Sans 3 (SIL OFL 1.1)

Единственный шрифт интерфейса — и для русского, и для английского, и для остальных языков
(`assets/fonts/SourceSans3-Regular.ttf`, `SourceSans3-Semibold.ttf`, `SourceSans3-Bold.ttf`),
вшит ресурсом `:/fonts/SourceSans3-*.ttf` (VT-UI-1005).
Источник: https://github.com/adobe-fonts/source-sans (release, `TTF/SourceSans3-*.ttf`).
Copyright 2010-2024 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'.
Source is a trademark of Adobe in the United States and/or other countries.
Текст лицензии: `assets/fonts/LICENSE-SourceSans3.txt`.
