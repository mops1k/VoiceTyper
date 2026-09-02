using System.Globalization;
using System.Windows.Data;

namespace VoiceTyper.App;

/// <summary>Преобразует название раздела навигации в глиф Segoe MDL2.</summary>
public sealed class NavGlyphConverter : IValueConverter
{
    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        value as string switch
        {
            "Основные" => "\uE80F",
            "Внешний вид" => "\uE90F",
            "Модели" => "\uE7B8",
            "Горячие клавиши" => "\uE765",
            "Микрофон" => "\uE720",
            "Запуск" => "\uE768",
            "О программе" => "\uE946",
            _ => "\uE713",
        };

    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        throw new NotSupportedException();
}
