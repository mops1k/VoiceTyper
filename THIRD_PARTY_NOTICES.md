# THIRD_PARTY_NOTICES / Сторонние компоненты

## parakeet.cpp (MIT)

Нативный рантайм распознавания Parakeet (`VoiceTyper.App/Native/parakeet.dll`,
сборка описана в `VoiceTyper.App/Native/parakeet/BUILD.txt`,
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

## whisper.cpp / Whisper.net

- whisper.cpp: https://github.com/ggml-org/whisper.cpp (MIT).
- Whisper.net: https://github.com/sandrohanea/whisper.net (MIT).
- Весовые модели Whisper с https://huggingface.co/ggerganov/whisper.cpp — см. карточки моделей
  (MIT/CC-BY в зависимости от модели).
- Silero VAD: https://github.com/snakers4/silero-vad (MIT).
