using System.Resources;
using System.Runtime.CompilerServices;

// Нейтральный (базовый) ресурс — русский. Значит, для русской локали используется
// встроенный в сборку ресурс, а спутниковая сборка для 'ru' не создаётся.
// Для английского создаётся спутниковая сборка en\VoiceTyper.Core.resources.dll.
[assembly: NeutralResourcesLanguage("ru")]

// Тесты работают напрямую с внутренним P/Invoke-слоем parakeet.dll.
[assembly: InternalsVisibleTo("VoiceTyper.Tests")]
