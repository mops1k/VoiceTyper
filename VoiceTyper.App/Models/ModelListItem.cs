using CommunityToolkit.Mvvm.ComponentModel;
using VoiceTyper.Core.Models;

namespace VoiceTyper.App.Models;

/// <summary>Карточка модели для раздела «Модели».</summary>
public sealed partial class ModelListItem : ObservableObject
{
    [ObservableProperty]
    private bool _isDownloaded;

    [ObservableProperty]
    private bool _isSelected;

    public ModelListItem(ModelSize size, string name, string description, string speed, string quality, string approxSize)
    {
        Size = size;
        Name = name;
        Description = description;
        Speed = speed;
        Quality = quality;
        ApproxSize = approxSize;
    }

    public ModelSize Size { get; }
    public string Name { get; }
    public string Description { get; }
    public string Speed { get; }
    public string Quality { get; }
    public string ApproxSize { get; }

    public bool CanDownload => !IsDownloaded;
    public bool CanDelete => IsDownloaded && !IsSelected;

    partial void OnIsDownloadedChanged(bool value)
    {
        OnPropertyChanged(nameof(CanDownload));
        OnPropertyChanged(nameof(CanDelete));
    }

    partial void OnIsSelectedChanged(bool value) => OnPropertyChanged(nameof(CanDelete));
}
