using VoiceTyper.Core.Abstractions;
using WindowsInput.Events;

namespace VoiceTyper.App.Services;

/// <summary>Симуляция Ctrl+V в активное окно через SendInput (WindowsInput, event-based API).</summary>
public sealed class InputSimulatorPaster : IPasteSimulator
{
    public void Paste()
    {
        EventBuilder.Create()
            .ClickChord(new[] { KeyCode.Control, KeyCode.V })
            .Invoke(new InvokeOptions())
            .GetAwaiter()
            .GetResult();
    }
}
