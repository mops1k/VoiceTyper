using System.Globalization;
using System.Windows;
using System.Windows.Data;

namespace VoiceTyper.App;

/// <summary>Преобразует строку в <see cref="Visibility"/> (Visible, если равна параметру).</summary>
public sealed class NavVisibilityConverter : IValueConverter
{
    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        var compare = string.Equals(value as string, parameter as string, StringComparison.Ordinal);
        return compare ? Visibility.Visible : Visibility.Collapsed;
    }

    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        throw new NotSupportedException();
}
