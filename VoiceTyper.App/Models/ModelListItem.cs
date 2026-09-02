using CommunityToolkit.Mvvm.ComponentModel;
using VoiceTyper.Core.Localization;
using VoiceTyper.Core.Models;

namespace VoiceTyper.App.Models;

/// <summary>
/// Карточка модели для раздела «Модели». Локализуемые подписи
/// (<see cref="Description"/>, <see cref="Speed"/>, <see cref="Quality"/>,
/// <see cref="ApproxSize"/>) резолвятся из ресурсов по ключам и подписаны на
/// <see cref="Loc"/>, поэтому при смене языка интерфейса обновляются на лету.
/// </summary>
public sealed partial class ModelListItem : ObservableObject
{
    /// <summary>Размер в МБ (если задан) — для формата «≈ N МБ».</summary>
    private readonly double? _sizeMb;

    /// <summary>Размер в ГБ (если задан) — для формата «≈ N ГБ».</summary>
    private readonly double? _sizeGb;

    [ObservableProperty]
    private bool _isDownloaded;

    [ObservableProperty]
    private bool _isSelected;

    public ModelListItem(ModelSize size, string name, string descriptionKey, string speedKey, string qualityKey,
        double? sizeMb = null, double? sizeGb = null)
    {
        Size = size;
        Name = name;
        DescriptionKey = descriptionKey;
        SpeedKey = speedKey;
        QualityKey = qualityKey;
        _sizeMb = sizeMb;
        _sizeGb = sizeGb;

        Loc.Instance.PropertyChanged += (_, _) =>
        {
            OnPropertyChanged(nameof(Description));
            OnPropertyChanged(nameof(Speed));
            OnPropertyChanged(nameof(Quality));
            OnPropertyChanged(nameof(ApproxSize));
        };
    }

    public ModelSize Size { get; }

    public string Name { get; }

    public string DescriptionKey { get; }

    public string SpeedKey { get; }

    public string QualityKey { get; }

    public string Description => Loc.Instance[DescriptionKey];

    public string Speed => Loc.Instance[SpeedKey];

    public string Quality => Loc.Instance[QualityKey];

    public string ApproxSize => _sizeGb is { } gb
        ? "≈ " + Loc.Format("Models_SizeGb", gb)
        : "≈ " + Loc.Format("Models_SizeMb", _sizeMb!.Value);

    public bool CanDownload => !IsDownloaded;
    public bool CanDelete => IsDownloaded && !IsSelected;

    partial void OnIsDownloadedChanged(bool value)
    {
        OnPropertyChanged(nameof(CanDownload));
        OnPropertyChanged(nameof(CanDelete));
    }

    partial void OnIsSelectedChanged(bool value) => OnPropertyChanged(nameof(CanDelete));
}
