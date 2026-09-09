using System.Globalization;
using Avalonia.Data.Converters;

namespace VoiceTyper.App;

/// <summary>bool → bool (инверсия) для IsEnabled/IsVisible.</summary>
public sealed class InvertBoolConverter : IValueConverter
{
    public static readonly InvertBoolConverter Instance = new();

    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        return value is not bool b || !b;
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        throw new NotSupportedException();
}
