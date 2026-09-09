using System.Globalization;
using Avalonia.Data.Converters;

namespace VoiceTyper.App;

/// <summary>Преобразует строку в bool (true, если равна параметру) — для IsVisible.</summary>
public sealed class NavVisibilityConverter : IValueConverter
{
    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        return string.Equals(value as string, parameter as string, StringComparison.Ordinal);
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        throw new NotSupportedException();
}
