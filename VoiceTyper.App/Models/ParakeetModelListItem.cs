using CommunityToolkit.Mvvm.ComponentModel;
using VoiceTyper.Core.Localization;
using VoiceTyper.Core.Models;

namespace VoiceTyper.App.Models;

/// <summary>
/// Карточка кванта модели Parakeet (по образцу <see cref="ModelListItem"/>, но для
/// <see cref="ParakeetModelSize"/>). Локализуемые подписи резолвятся из ресурсов по ключам
/// и обновляются при смене языка интерфейса.
/// </summary>
public sealed partial class ParakeetModelListItem : ObservableObject
{
    /// <summary>Размер в ГБ.</summary>
    private readonly double _sizeGb;

    [ObservableProperty]
    private bool _isDownloaded;

    [ObservableProperty]
    private bool _isSelected;

    public ParakeetModelListItem(ParakeetModelSize size, string name, string descriptionKey,
        string speedKey, string qualityKey, double sizeGb)
    {
        Size = size;
        Name = name;
        DescriptionKey = descriptionKey;
        SpeedKey = speedKey;
        QualityKey = qualityKey;
        _sizeGb = sizeGb;

        Loc.Instance.PropertyChanged += (_, _) =>
        {
            OnPropertyChanged(nameof(Description));
            OnPropertyChanged(nameof(Speed));
            OnPropertyChanged(nameof(Quality));
            OnPropertyChanged(nameof(ApproxSize));
        };
    }

    public ParakeetModelSize Size { get; }

    public string Name { get; }

    public string DescriptionKey { get; }

    public string SpeedKey { get; }

    public string QualityKey { get; }

    public string Description => Loc.Instance[DescriptionKey];

    public string Speed => Loc.Instance[SpeedKey];

    public string Quality => Loc.Instance[QualityKey];

    public string ApproxSize => "≈ " + Loc.Format("Models_SizeGb", _sizeGb);

    public bool CanDownload => !IsDownloaded;
    public bool CanDelete => IsDownloaded && !IsSelected;

    partial void OnIsDownloadedChanged(bool value)
    {
        OnPropertyChanged(nameof(CanDownload));
        OnPropertyChanged(nameof(CanDelete));
    }

    partial void OnIsSelectedChanged(bool value) => OnPropertyChanged(nameof(CanDelete));
}
