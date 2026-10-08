#include "app/main_window.hpp"
#include "app/ui_text.hpp"

#include "domain/error.hpp"
#include "domain/version.hpp"


#include <QComboBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QBoxLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMargins>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QListWidget>
#include <QPainterPath>
#include <QPointer>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QSlider>
#include <QScreen>
#include <QScrollBar>
#include <QScrollArea>
#include <QFontDatabase>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QSizePolicy>
#include <QMouseEvent>
#include <QSizeGrip>
#include <QStackedWidget>
#include <QStyleHints>
#include <QUrl>
#include <QVariant>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <QApplication>
#include <QDebug>
#include <QWindow>
#include <QHash>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextEdit>
#include <QTextOption>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace voicetyper::app {

using domain::AppLanguage;
using domain::AppTheme;
using domain::GigaamModelSize;
using domain::ModelSize;
using domain::ParakeetModelSize;
using domain::RecognitionLanguage;
using domain::RecordingMode;
using domain::TranscriptionEngine;
using domain::kBestOfMax;
using domain::kBestOfMin;
using domain::kSilenceThresholdMsMax;
using domain::kSilenceThresholdMsMin;

namespace {

QString to_q(const std::string& text)
{
    return QString::fromStdString(text);
}

/// Russian display names for the settings enums. The persisted wire values stay
/// English; only the UI label is localized, so switching the UI language never
/// touches settings.json.
QString model_size_text(ModelSize size)
{
    switch (size) {
    case ModelSize::tiny: return QStringLiteral("tiny");
    case ModelSize::base: return QStringLiteral("base");
    case ModelSize::small: return QStringLiteral("small");
    case ModelSize::medium: return QStringLiteral("medium");
    case ModelSize::large: return QStringLiteral("large");
    }
    return QStringLiteral("?");
}

QString parakeet_size_text(ParakeetModelSize size)
{
    switch (size) {
    case ParakeetModelSize::q4k: return QStringLiteral("q4_k");
    case ParakeetModelSize::q5k: return QStringLiteral("q5_k");
    case ParakeetModelSize::q6k: return QStringLiteral("q6_k");
    case ParakeetModelSize::q8_0: return QStringLiteral("q8_0");
    }
    return QStringLiteral("?");
}

QString gigaam_size_text(GigaamModelSize size)
{
    switch (size) {
    case GigaamModelSize::q4_k_m: return QStringLiteral("q4_k_m");
    case GigaamModelSize::q5_k_m: return QStringLiteral("q5_k_m");
    case GigaamModelSize::q6_k: return QStringLiteral("q6_k");
    case GigaamModelSize::q8_0: return QStringLiteral("q8_0");
    }
    return QStringLiteral("?");
}

QString theme_text(AppTheme theme, AppLanguage language)
{
    switch (theme) {
    case AppTheme::light: return ui_text(UiKey::k50, language);
    case AppTheme::dark: return ui_text(UiKey::k53, language);
    case AppTheme::system: return ui_text(UiKey::k51, language);
    }
    return QStringLiteral("?");
}

} // namespace

MainWindow::MainWindow(
    SettingsPresenter& presenter,
    WindowServices services,
    std::shared_ptr<StatusChannel> adopted_channel,
    QWidget* parent)
    : QMainWindow(parent)
    , presenter_(presenter)
    , services_(std::move(services))
    , language_(presenter.settings().app_language)
{
    setWindowTitle(QStringLiteral("VoiceTyper"));
    // MainWindow.axaml:10-14: 980x640, min 760x480, SystemDecorations=None. The
    // frame is drawn by the window itself (title bar below), which is what makes
    // the port look like the app it replaces.
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    // The mockup is min(1220px, 96vw) x min(800px, 94vh) with min-height 650
    // (index.css: .window). The window still opens inside whatever screen it is on
    // (Alexander: "чтобы на любом разрешении окно было видно целиком и все настройки
    // были доступны"), so both are capped by the available geometry.
    const QRect available = screen() != nullptr ? screen()->availableGeometry() : QRect(0, 0, 1280, 720);
    resize(qMin(1220, qMax(560, available.width() * 96 / 100)),
        qMin(800, qMax(460, available.height() * 94 / 100)));
    // min-height 650 comes from the mockup, but a screen shorter than that must still
    // be able to show the window: the minimum shrinks with the screen instead of
    // making the window taller than the desktop.
    setMinimumSize(520, qBound(460, 650, qMax(460, available.height() - 80)));
    // The window opened in the top-left corner of the monitor; the mockup's window is a
    // centred dialog (Alexander, 2026-10-08).
    move(available.center() - QPoint(width() / 2, height() / 2));
    build_tabs();
    bind_settings_to_controls();
    bind_microphone_controls();
    bind_update_controls();
    apply_theme();
    refresh_status();
    refresh_devices();

    // Adopt the channel the composition already created, so events fired before
    // the window existed are not lost and the window owns it from here on.
    status_channel_ = std::move(adopted_channel);
    if (!status_channel_) {
        status_channel_ = std::make_shared<StatusChannel>(this);
    } else {
        status_channel_->adopt(this);
    }

    autosave_ = new QTimer(this);
    autosave_->setSingleShot(false);
    autosave_->setInterval(100);
    connect(autosave_, &QTimer::timeout, this, &MainWindow::on_autosave_timeout);
    autosave_->start();

    refresh_ = new QTimer(this);
    refresh_->setInterval(500);
    connect(refresh_, &QTimer::timeout, this, &MainWindow::on_presentation_refresh);
    refresh_->start();
}

MainWindow::~MainWindow()
{
    save_now();
}

namespace {

/// The Windows 11 Settings palette: the goal is the app Alexander showed
/// (`docs/migration/cpp/reference/win11-settings.png`) in both themes, so the
/// settings window reads like a system page.
///   window  - page background (Win11 draws it under everything)
///   control - card surface
///   hover   - navigation hover/selection fill
///   text    - primary text
///   muted   - secondary text
///   border  - separators, card edge, control stroke
///   accent  - system accent: selection bar, switches, focus
struct Palette {
    const char* window;
    const char* control;
    const char* hover;
    const char* text;
    const char* muted;
    const char* border;
    const char* accent;
    /// Fill of a selected navigation item (the reference uses a pale blue, dark mode a
    /// slightly lighter grey).
    const char* selection;
    /// Background of the page-title band.
    const char* header;
    /// Background behind the setting cards (the Figma Make mockup's --surface-raised):
    /// the cards themselves are painted with `window`/--surface, so the two must differ.
    const char* surface_raised;
    /// Hairline between rows inside a card (--line-soft): lighter than `border`.
    const char* line_soft;
    /// Second text level: brand tagline, status strip, hint copy (--text-secondary).
    const char* text_secondary;
    /// Track of a switch that is off (--preview-line).
    const char* preview_line;
    /// Accent of a switch that is on: the mockup uses #32AEEA here and #36AEEA for
    /// every other accent, so the two are kept apart.
    const char* accent_switch;
    /// The separator under the footer. @{border} is nearly the footer's own colour in the
    /// dark theme, so the strip looked as if the line stopped halfway (Alexander, 08.10.2026).
    const char* footer_line;
};

constexpr Palette kDarkPalette{
    "#1D1F22", "#25282C", "#292C30", "#F2F5F7", "#7E858C", "#34373B", "#36AEEA", "#17364A", "#1D1F22",
    "#23262A", "#2B2E31", "#AAB1B8", "#52565B", "#32AEEA", "#3E4348"};
constexpr Palette kLightPalette{
    "#FFFFFF", "#F8FAFB", "#F0F4F6", "#182026", "#879098", "#DCE1E4", "#36AEEA", "#E2F3FC", "#FFFFFF",
    "#F7F9FA", "#E8ECEE", "#59646D", "#CCD3D8", "#32AEEA", "#D5DBDF"};

/// Right inset of one settings row, in pixels: the controls stop short of the
/// window edge instead of touching it.
constexpr int kRowRightInset = 10;

/// The mockup's level meter (index.css: .level-meter): one rounded trough with 22 bars
/// of 5 px that the microphone test fills. It replaces the QProgressBar the page used to
/// carry, because a progress bar reads as "work in progress" while this is a signal level.
///
/// Deliberately no Q_OBJECT: the class only paints itself, and the palette reaches it
/// through level_meter_set_colors(), so no signal, slot or meta-object is needed.
class LevelMeter final : public QWidget {
public:
    explicit LevelMeter(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("levelMeter"));
        setAttribute(Qt::WA_StyledBackground, true);
        // index.css: .level-meter height 42px; the bars never make it taller, so the
        // page keeps one fixed row whatever the level is.
        setMinimumHeight(42);
        setMaximumHeight(42);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    /// 0..100: the share of the 22 bars the reported peak reached.
    void set_level(int level)
    {
        const int clamped = std::clamp(level, 0, 100);
        if (clamped == level_) {
            return;
        }
        level_ = clamped;
        update();
    }

    /// One live sample (0..100) of the running probe: the bars scroll, so the movement
    /// on screen comes from the voice.
    void push_level(int level)
    {
        for (std::size_t index = 1; index < 22; ++index) {
            history_[index - 1] = history_[index];
        }
        history_[21] = std::clamp(level, 0, 100);
        update();
    }

    /// True while the probe runs: the mockup fills the trough then (index.css:
    /// .level-meter.testing), which is the only way to show anything before the
    /// capture reports its peak at the end.
    void set_testing(bool testing)
    {
        if (testing == testing_) {
            return;
        }
        testing_ = testing;
        update();
    }

    /// The palette lives in apply_theme, which owns the colours.
    void set_colors(const QColor& bar, const QColor& active, const QColor& highlight)
    {
        bar_ = bar;
        active_ = active;
        highlight_ = highlight;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        // The trough itself is the QSS background and border of #levelMeter; only the
        // bars are painted here. index.css: .level-meter padding 6px 9px, gap 3px.
        const QRectF inner = QRectF(rect()).adjusted(9, 6, -9, -6);
        constexpr int kBarCount = 22;
        constexpr qreal kGap = 3.0;
        const qreal bar_width = (inner.width() - kGap * (kBarCount - 1)) / kBarCount;
        if (bar_width <= 0.0 || inner.height() <= 0.0) {
            return;
        }
        // The shape of the bars follows the mockup's keyframes: the trough is 30 px of
        // usable height, a lit bar reaches up to 27 px there and a quiet one stays at 5.
        static constexpr qreal kHeights[kBarCount] = {
            6, 11, 17, 23, 27, 21, 13, 8, 15, 25, 19, 11,
            7, 13, 22, 26, 18, 10, 6, 12, 20, 15};
        const int lit = testing_ ? kBarCount : (level_ * kBarCount + 99) / 100;
        painter.setPen(Qt::NoPen);
        for (int index = 0; index < kBarCount; ++index) {
            bool on = index < lit;
            qreal height = on ? std::min(kHeights[index], inner.height()) : 5.0;
            if (testing_) {
                // The live level of the running probe drives the height, so the trough
                // moves exactly as loud as the voice is.
                const int sample = history_[static_cast<std::size_t>(index)];
                height = std::max(5.0, std::min(inner.height(), inner.height() * sample / 100.0));
                on = sample > 4;
            }
            const QRectF bar(inner.left() + index * (bar_width + kGap),
                inner.center().y() - height / 2.0, bar_width, height);
            // Every fourth bar is green while the meter runs (index.css:
            // .level-meter.testing i:nth-child(4n) is #43c78f).
            painter.setBrush(on ? (index % 4 == 3 ? highlight_ : active_) : bar_);
            painter.drawRoundedRect(bar, 2.0, 2.0);
        }
    }

private:
    int level_ = 0;
    bool testing_ = false;
    /// The last 22 samples of the live probe, oldest first.
    int history_[22]{};
    QColor bar_{QStringLiteral("#DCE1E4")};
    QColor active_{QStringLiteral("#36AEEA")};
    QColor highlight_{QStringLiteral("#43C78F")};
};

/// A word-wrapped label that reports the height its text really needs at the current width.
/// QLabel describes a single line in its size hint, which is why every model row reserved
/// two lines for its meta and note and came out taller than the mockup (Alexander,
/// 08.10.2026). This label grows only when its text actually wraps.
class WrappingLabel final : public QLabel {
public:
    using QLabel::QLabel;

    QSize sizeHint() const override
    {
        QSize hint = QLabel::sizeHint();
        const int width = this->width() > 0 ? this->width() : hint.width();
        const int needed = heightForWidth(width);
        if (needed > 0) {
            hint.setHeight(needed);
        }
        return hint;
    }

    QSize minimumSizeHint() const override
    {
        QSize hint = QLabel::minimumSizeHint();
        const int needed = heightForWidth(width());
        if (needed > 0) {
            hint.setHeight(needed);
        }
        return hint;
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QLabel::resizeEvent(event);
        const int needed = heightForWidth(width());
        if (needed > 0 && minimumHeight() != needed) {
            setMinimumHeight(needed);
        }
    }
};

/// The microphone sensitivity slider is painted by hand. It used to be styled through QSS
/// sub-controls (::groove/::sub-page/::handle), and those are laid out by the style engine:
/// at a fractional device ratio the sub-page filled the whole widget instead of the 4 px
/// track, so the slider grew a wide light band (Alexander, 08.10.2026). Drawing the track,
/// the fill and the knob in logical coordinates is exact at any scale and in both themes.
class LevelSlider final : public QSlider {
public:
    explicit LevelSlider(QWidget* parent = nullptr)
        : QSlider(Qt::Horizontal, parent)
    {
        setObjectName(QStringLiteral("microphoneLevelSlider"));
        setFixedHeight(20);
    }

    void set_colors(const QColor& track, const QColor& fill, const QColor& knob, const QColor& knob_hover)
    {
        track_ = track;
        fill_ = fill;
        knob_ = knob;
        knob_hover_ = knob_hover;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        constexpr qreal kTrack = 4.0;
        constexpr qreal kKnob = 14.0;
        const qreal margin = kKnob / 2.0;
        const qreal span = qMax(0.0, width() - kKnob);
        const double range = maximum() - minimum();
        const double fraction = range > 0.0 ? (value() - minimum()) / range : 0.0;
        const qreal centre = margin + span * fraction;
        const QRectF track(margin, (height() - kTrack) / 2.0, span, kTrack);
        painter.setPen(Qt::NoPen);
        painter.setBrush(track_);
        painter.drawRoundedRect(track, kTrack / 2.0, kTrack / 2.0);
        QRectF filled = track;
        filled.setWidth(qMax(0.0, centre - track.left()));
        painter.setBrush(fill_);
        painter.drawRoundedRect(filled, kTrack / 2.0, kTrack / 2.0);
        const QRectF knob(centre - kKnob / 2.0, (height() - kKnob) / 2.0, kKnob, kKnob);
        const bool pointed_at = underMouse();
        painter.setBrush(QColor(QStringLiteral("#FFFFFF")));
        painter.setPen(QPen(pointed_at ? knob_hover_ : knob_, pointed_at ? 1.5 : 1.0));
        painter.drawEllipse(knob);
    }

    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }

private:
    QColor track_{QStringLiteral("#DCE1E4")};
    QColor fill_{QStringLiteral("#36AEEA")};
    QColor knob_{QStringLiteral("#DCE1E4")};
    QColor knob_hover_{QStringLiteral("#36AEEA")};
};

/// The palette lives in apply_theme; the class is in this anonymous namespace, so the
/// member stays a plain QSlider* and these helpers are the only door to it.
void level_slider_set_colors(QSlider* slider, const QColor& track, const QColor& fill,
    const QColor& knob, const QColor& knob_hover)
{
    if (auto* level = dynamic_cast<LevelSlider*>(slider); level != nullptr) {
        level->set_colors(track, fill, knob, knob_hover);
    }
}

/// The three calls MainWindow makes on the meter. The class is in this anonymous
/// namespace, so its methods cannot be named in main_window.hpp; the member stays a
/// plain QWidget* and these helpers are the only door to it.
void level_meter_set_level(QWidget* meter, int level)
{
    static_cast<LevelMeter*>(meter)->set_level(level);
}

void level_meter_set_testing(QWidget* meter, bool testing)
{
    static_cast<LevelMeter*>(meter)->set_testing(testing);
}

void level_meter_set_colors(QWidget* meter, const QColor& bar, const QColor& active, const QColor& highlight)
{
    static_cast<LevelMeter*>(meter)->set_colors(bar, active, highlight);
}

void level_meter_push_level(QWidget* meter, int level)
{
    if (meter == nullptr) {
        return;
    }
    static_cast<LevelMeter*>(meter)->push_level(level);
}

/// The navigation from `SettingsViewModel.cs:307-316`: key order and the Segoe
/// Sidebar icon box, logical pixels: the .NET template drew a 16 px glyph inside it.
inline constexpr int kNavigationIconSizePx = 19;
inline constexpr int kNavigationGlyphSizePx = 16;

/// MDL2 Assets glyphs of the .NET build.
struct NavEntry {
    const char* glyph;
    /// The Russian label, kept for the settings-file page name and the tests.
    const char* label;
    /// The user-visible label, so the navigation follows the interface language too.
    UiKey text;
    const char* key;
};

/// The card heading of a page, from the .NET page titles (Strings.resx:
/// General_Title, Hotkeys_Title, ...). Uppercase there, and uppercase here.
const char* section_title_for(const char* key)
{
    if (std::strcmp(key, "Main") == 0) return "ОСНОВНЫЕ";
    if (std::strcmp(key, "Appearance") == 0) return "ВНЕШНИЙ ВИД";
    if (std::strcmp(key, "Models") == 0) return "МОДЕЛИ";
    if (std::strcmp(key, "Hotkeys") == 0) return "ГОРЯЧИЕ КЛАВИШИ";
    if (std::strcmp(key, "Microphone") == 0) return "МИКРОФОН";
    if (std::strcmp(key, "Startup") == 0) return "ЗАПУСК";
    if (std::strcmp(key, "Log") == 0) return "ЖУРНАЛ";
    if (std::strcmp(key, "About") == 0) return "О ПРОГРАММЕ";
    return "";
}

constexpr NavEntry kNavigation[] = {
    {"\uE80F", "Общие", UiKey::k23, "Main"},
    {"\uE90F", "Внешний вид", UiKey::k5, "Appearance"},
    {"\uE7B8", "Модели", UiKey::k18, "Models"},
    {"\uE765", "Хоткеи", UiKey::k42, "Hotkeys"},
    {"\uE720", "Микрофон", UiKey::k17, "Microphone"},
    {"\uE768", "Запуск", UiKey::k13, "Startup"},
    {"\uE7C3", "Журнал", UiKey::k9, "Log"},
    {"\uE946", "О программе", UiKey::k22, "About"},
};
/// A model row of the Models page, in both interface languages. The .NET page kept these
/// in Models.cs and Strings.resx; the port keeps them here because they are presentation
/// data, not labels: names stay as they are and the size unit follows the language.
struct ModelRow {
    const char* name;
    const char* size_ru;
    const char* size_en;
    const char* speed_ru;
    const char* speed_en;
    const char* quality_ru;
    const char* quality_en;
    const char* description_ru;
    const char* description_en;
};

constexpr ModelRow kWhisperModelRows[] = {
    {"Tiny (q8)", "≈ 42 МБ", "≈ 42 MB", "Очень быстро", "Very fast", "Низкое", "Low",
        "Минимальная модель (q8). Быстро распознаёт, экономна по памяти.",
        "Smallest model (q8). Recognizes quickly and is easy on memory."},
    {"Base (q8)", "≈ 78 МБ", "≈ 78 MB", "Быстро", "Fast", "Среднее", "Medium",
        "Базовая модель (q8). Хороший баланс скорости и качества на CPU.",
        "Base model (q8). A good balance of speed and quality on the CPU."},
    {"Small (q8)", "≈ 252 МБ", "≈ 252 MB", "Средне", "Medium", "Высокое", "High",
        "Рекомендуемая модель (q8). Точное распознавание, умеренная нагрузка.",
        "The recommended model (q8). Accurate recognition at a moderate load."},
    {"Medium (q8)", "≈ 785 МБ", "≈ 785 MB", "Медленно", "Slow", "Очень высокое", "Very high",
        "Качественное распознавание (q8), заметно медленнее на CPU.",
        "Accurate recognition (q8), noticeably slower on the CPU."},
    {"Large (turbo, q8)", "≈ 834 МБ", "≈ 834 MB", "Очень медленно", "Very slow",
        "Максимальное", "Maximum",
        "Максимальное качество (q8, turbo). Для мощных процессоров.",
        "The best quality (q8, turbo). For powerful processors."},
};

constexpr ModelRow kParakeetModelRows[] = {
    {"Parakeet v3 (q4_k)", "≈ 0.64 ГБ", "≈ 0.64 GB", "Очень быстро", "Very fast",
        "Высокое", "High",
        "Компактный квант: меньше памяти и диска, немного ниже точность.",
        "A compact quant: less memory and disk, slightly lower accuracy."},
    {"Parakeet v3 (q5_k)", "≈ 0.71 ГБ", "≈ 0.71 GB", "Очень быстро", "Very fast",
        "Высокое", "High",
        "Баланс размера и точности: чуть выше качество, чем у q4_k.",
        "A balance of size and accuracy: a little better than q4_k."},
    {"Parakeet v3 (q6_k)", "≈ 0.78 ГБ", "≈ 0.78 GB", "Очень быстро", "Very fast",
        "Высокое", "High",
        "Повышенная точность при умеренном размере файла.",
        "Higher accuracy at a moderate file size."},
    {"Parakeet v3 (q8_0)", "≈ 0.9 ГБ", "≈ 0.9 GB", "Очень быстро", "Very fast",
        "Высокое", "High",
        "Максимальная точность кванта при самом большом размере файла.",
        "The most accurate quant, at the largest file size."},
};

/// GigaAM-v3 e2e-rnnt (Russian only; punctuation and casing come from the model
/// itself). The published WER of every quant is within 0.07 pp of the others, so
/// the sizes differ but the descriptions do not overclaim.
constexpr ModelRow kGigaamModelRows[] = {
    {"GigaAM v3 (q4_k_m)", "≈ 184 МБ", "≈ 184 MB", "Очень быстро", "Very fast", "Высокое", "High",
        "Самый компактный квант: минимум места на диске, точность как у остальных.",
        "The most compact quant: least disk space, accuracy on par with the rest."},
    {"GigaAM v3 (q5_k_m)", "≈ 206 МБ", "≈ 206 MB", "Очень быстро", "Very fast", "Высокое", "High",
        "Чуть больше файл, чуть выше устойчивость на шумной записи.",
        "A slightly larger file and slightly better robustness on noisy audio."},
    {"GigaAM v3 (q6_k)", "≈ 228 МБ", "≈ 228 MB", "Очень быстро", "Very fast", "Высокое", "High",
        "Промежуточный вариант между компактностью и точностью.",
        "A middle ground between compactness and accuracy."},
    {"GigaAM v3 (q8_0)", "≈ 274 МБ", "≈ 274 MB", "Очень быстро", "Very fast", "Высокое", "High",
        "Эталонный квант: русский текст с пунктуацией и регистром сразу, окно ~25 с на фразу.",
        "Reference quant: Russian with punctuation and casing out of the box, ~25 s per utterance."},
};

/// The object-name prefix of a model list and its rows: one place decides which
/// list belongs to which engine, so a row can never be wired to another engine's
/// download.
const char* model_list_prefix(ModelList list) noexcept
{
    switch (list) {
    case ModelList::whisper: return "whisper";
    case ModelList::parakeet: return "parakeet";
    case ModelList::gigaam: return "gigaam";
    }
    return "whisper";
}

const ModelRow* model_rows(ModelList list, std::size_t& count) noexcept
{
    switch (list) {
    case ModelList::whisper: count = std::size(kWhisperModelRows); return kWhisperModelRows;
    case ModelList::parakeet: count = std::size(kParakeetModelRows); return kParakeetModelRows;
    case ModelList::gigaam: count = std::size(kGigaamModelRows); return kGigaamModelRows;
    }
    count = 0;
    return nullptr;
}

/// The field of a row for the language being shown.
[[nodiscard]] inline const char* model_field(const char* ru, const char* en, AppLanguage language)
{
    return language == AppLanguage::en ? en : ru;
}


/// One settings row: the label pinned to the left edge, the control to the right
/// in its own column. The .NET cards do exactly this (`Border Classes="row"`),
/// and Alexander asked for it explicitly: "подпись всегда слева, а сами настройки
/// выровнены по правому краю".
/// The left column of a settings row: the label and, when the mockup has one, the
/// sentence under it (index.css: .row-copy strong 12 px, .row-copy span 10 px muted).
QWidget* row_copy(UiKey key, const UiKey* description, AppLanguage language)
{
    auto* text_column = new QWidget();
    text_column->setObjectName(QStringLiteral("rowCopy"));
    auto* text_layout = new QVBoxLayout(text_column);
    text_layout->setContentsMargins(0, 0, 0, 0);
    text_layout->setSpacing(4);
    auto* text = new QLabel(ui_text(key, language), text_column);
    text->setObjectName(QStringLiteral("rowTitle"));
    // The key is remembered on the widget: that is what lets a language change re-letter
    // the whole window in place instead of destroying and rebuilding it (Alexander:
    // "хотелось бы, чтобы без закрытия окна просто менялись все строки").
    text->setProperty("uiKey", static_cast<int>(key));
    text->setWordWrap(true);
    text_layout->addWidget(text);
    if (description != nullptr) {
        // The second line of the mockup's row: what the setting actually does.
        auto* caption = new QLabel(ui_text(*description, language), text_column);
        caption->setObjectName(QStringLiteral("rowDescription"));
        caption->setProperty("uiKey", static_cast<int>(*description));
        caption->setWordWrap(true);
        text_layout->addWidget(caption);
    }
    return text_column;
}

/// One row of the mockup's list (index.css: .settings-row): a ready copy column on the
/// left and the control on the right. `has_value_column` keeps the proportional 4:1
/// split; it is off for a heading row and for a switch, which keeps its own size.
QWidget* settings_row_shell(QWidget* copy, QWidget* control, bool has_value_column)
{
    auto* row = new QWidget();
    row->setObjectName(QStringLiteral("settingsRow"));
    // A 1 px separator under each row, as the system settings list draws it. A plain
    // QWidget needs the attribute before a stylesheet border is painted at all.
    row->setAttribute(Qt::WA_StyledBackground, true);
    // Minimum, not Fixed: a Fixed row keeps the height it was given, so a label
    // that wraps to a second line is clipped. The page's trailing stretch is what
    // keeps the rows from absorbing the leftover height, so they stay at the top.
    row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    auto* layout = new QHBoxLayout(row);
    // The mockup's row insets are 12px 18px with a 20 px gap between the copy and the
    // control (index.css: .settings-row); the card, not the window edge, is now what
    // the content stops short of.
    layout->setContentsMargins(18, 12, 18, 12);
    layout->setSpacing(20);
    if (control == nullptr) {
        // A heading row: the control is the widget added on the next line.
        layout->addWidget(copy, 1);
        return row;
    }
    // No alignment flag on the label: an aligned item is laid out at its size hint,
    // which for a wrapping label is one line, so the second line was clipped no
    // matter how tall the row was allowed to become (reported from the running
    // build, and caught here: "Вставлять автоматически" needed 32 px and had 22).
    //
    // The split is proportional, not fixed: the label keeps about four fifths of the
    // row and the value the rest (Alexander's sketch), at any window width, and the
    // row can never demand more than it has - which is what made pages wider than
    // the viewport and cut the right border off every control.
    layout->addWidget(copy, has_value_column ? 4 : 1);
    layout->addWidget(control, has_value_column ? 1 : 0);
    return row;
}

QWidget* setting_row_impl(UiKey key, const UiKey* description, AppLanguage language,
    QWidget* control, int control_width)
{
    const bool has_value_column = control != nullptr && control_width > 0
        && !control->inherits("voicetyper::app::ToggleSwitch");
    return settings_row_shell(row_copy(key, description, language), control, has_value_column);
}

QWidget* setting_row(UiKey key, AppLanguage language, QWidget* control, int control_width = 220)
{
    return setting_row_impl(key, nullptr, language, control, control_width);
}

/// The same row with the mockup's second line: what the setting does.
QWidget* setting_row(UiKey key, UiKey description, AppLanguage language, QWidget* control,
    int control_width = 220)
{
    return setting_row_impl(key, &description, language, control, control_width);
}

/// The mockup's slider row (index.css: .range-row and .range-control): the same copy
/// column as a settings row, then the slider and its value in a control that is
/// min(310 px, 48%) wide. The 52/48 split reproduces the 48% share, the cap is the
/// mockup's 310 px, and at the 820 px content width the two come out exactly as there.
QWidget* range_row(UiKey key, UiKey description, AppLanguage language, QWidget* slider,
    QLabel* value)
{
    auto* row = new QWidget();
    row->setObjectName(QStringLiteral("rangeRow"));
    row->setAttribute(Qt::WA_StyledBackground, true);
    row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    auto* layout = new QHBoxLayout(row);
    // .range-row: min-height 72px, padding 12px 18px, gap 24px.
    layout->setContentsMargins(18, 12, 18, 12);
    layout->setSpacing(24);

    auto* control = new QWidget(row);
    control->setObjectName(QStringLiteral("rangeControl"));
    control->setAttribute(Qt::WA_StyledBackground, true);
    control->setMaximumWidth(310);
    auto* control_layout = new QHBoxLayout(control);
    control_layout->setContentsMargins(0, 0, 0, 0);
    // .range-control: gap 11px, the value is a right-aligned 34 px column (.range-control b).
    control_layout->setSpacing(11);
    control_layout->addWidget(slider, 1);
    control_layout->addWidget(value, 0, Qt::AlignVCenter);

    layout->addWidget(row_copy(key, &description, language), 52);
    layout->addWidget(control, 48, Qt::AlignVCenter);
    return row;
}

/// One row whose title is a live label instead of a table key (the update status on the
/// About page): the same shell as a settings row, the sentence under it from the table.
QWidget* status_row(QWidget* status, UiKey description, AppLanguage language, QWidget* control)
{
    auto* copy = new QWidget();
    copy->setObjectName(QStringLiteral("rowCopy"));
    auto* copy_layout = new QVBoxLayout(copy);
    copy_layout->setContentsMargins(0, 0, 0, 0);
    copy_layout->setSpacing(4);
    copy_layout->addWidget(status);
    auto* caption = new QLabel(ui_text(description, language), copy);
    caption->setObjectName(QStringLiteral("rowDescription"));
    caption->setProperty("uiKey", static_cast<int>(description));
    caption->setWordWrap(true);
    copy_layout->addWidget(caption);
    return settings_row_shell(copy, control, true);
}

/// A widget inside a card that is not a row of the mockup (the update notes and the
/// progress bar): it keeps the card's insets so it does not touch the border.
QWidget* inset_block(QWidget* parent, QWidget* widget, const QMargins& margins)
{
    auto* wrapper = new QWidget(parent);
    wrapper->setObjectName(QStringLiteral("insetBlock"));
    auto* layout = new QVBoxLayout(wrapper);
    layout->setContentsMargins(margins);
    layout->setSpacing(0);
    layout->addWidget(widget);
    return wrapper;
}

/// Shows or hides an inset block together with the widget inside it. Hiding only the
/// inner widget left an empty card on the About page (the mockup has no such block).
void set_inset_visible(QWidget* widget, bool visible)
{
    if (widget == nullptr) {
        return;
    }
    widget->setVisible(visible);
    QWidget* wrapper = widget->parentWidget();
    if (wrapper != nullptr && wrapper->objectName() == QStringLiteral("insetBlock")) {
        wrapper->setVisible(visible);
    }
}

/// The message column follows the theme (index.css: .log-line span uses --text-secondary);
/// apply_theme() keeps this in step. The time and the level keep the mockup's own colours.
QColor g_log_message_color = QColor(QStringLiteral("#59646D"));

/// The mockup's log view (index.css .log-line): three columns - the time at 92px, the
/// level at 142px (bold, coloured by severity) and the message - in a monospaced font.
/// Lines the logger wrapped (details on their own line) belong to the entry above them.
void render_log_lines(QTextEdit* view, const QString& text)
{
    if (view == nullptr) {
        return;
    }
    QTextDocument* document = view->document();
    document->clear();
    QTextCursor cursor(document);
    view->setLineWrapMode(QTextEdit::NoWrap);

    const QColor time_color(QStringLiteral("#7F8992"));
    const QColor info_color(QStringLiteral("#43BD8A"));
    const QColor warn_color(QStringLiteral("#D9A343"));
    const QColor error_color(QStringLiteral("#EC5F67"));
    const QColor detail_color(QStringLiteral("#879098"));

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        if (line.isEmpty()) {
            continue;
        }
        QTextBlockFormat block;
        QList<QTextOption::Tab> tabs;
        tabs.append(QTextOption::Tab(92.0, QTextOption::LeftTab));
        tabs.append(QTextOption::Tab(142.0, QTextOption::LeftTab));
        block.setTabPositions(tabs);
        cursor.setBlockFormat(block);

        QString time;
        QString level;
        QString message = line;
        // "2026-10-08 19:08:14.149 [INFO] message"
        if (line.size() > 24 && line.at(4) == QLatin1Char('-') && line.at(10) == QLatin1Char(' ')
            && line.at(13) == QLatin1Char(':') && line.at(16) == QLatin1Char(':')
            && line.at(19) == QLatin1Char('.')) {
            time = line.mid(11, 8);
            const int open = line.indexOf(QLatin1Char('['), 24);
            const int close = open >= 0 ? line.indexOf(QLatin1Char(']'), open) : -1;
            if (close > open) {
                level = line.mid(open + 1, close - open - 1);
                message = line.mid(close + 1).trimmed();
            } else {
                message = line.mid(24);
            }
        }
        if (time.isEmpty()) {
            QTextCharFormat detail;
            detail.setForeground(detail_color);
            cursor.insertText(QStringLiteral("\t\t") + message + QLatin1Char('\n'), detail);
            continue;
        }
        QTextCharFormat time_format;
        time_format.setForeground(time_color);
        QTextCharFormat level_format;
        level_format.setForeground(level == QLatin1String("ERROR") ? error_color
            : (level == QLatin1String("WARN") ? warn_color : info_color));
        level_format.setFontWeight(QFont::DemiBold);
        QTextCharFormat message_format;
        message_format.setForeground(g_log_message_color);
        cursor.insertText(time, time_format);
        cursor.insertText(QStringLiteral("\t") + level, level_format);
        cursor.insertText(QStringLiteral("\t") + message + QLatin1Char('\n'), message_format);
    }
}

/// One font for glyph and label: Qt walks the family list per code point, so the
/// private-use glyph comes from the icon font while Cyrillic uses the UI font.
QFont navigation_font(int pixel_size)
{
    QFont font;
    font.setFamilies({QStringLiteral("Segoe UI"), QStringLiteral("Segoe MDL2 Assets"), QStringLiteral("Segoe UI Symbol")});
    font.setPixelSize(pixel_size);
    return font;
}

/// One card of the mockup: heading with a caption, then the rows inside
/// (index.css: .group / .group-title / .settings-row). A page that puts actions in
/// the heading (the journal's copy/clear buttons, index.css: .log-heading) passes
/// them as `trailing`, so the heading keeps one implementation.
struct SettingsGroup {
    QWidget* card = nullptr;
    QVBoxLayout* rows = nullptr;
};

SettingsGroup settings_group(QWidget* parent, UiKey title, UiKey caption, AppLanguage language,
    QWidget* trailing = nullptr)
{
    SettingsGroup group;
    auto* card = new QWidget(parent);
    card->setObjectName(QStringLiteral("card"));
    card->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* header = new QWidget(card);
    header->setObjectName(QStringLiteral("cardHeader"));
    header->setAttribute(Qt::WA_StyledBackground, true);
    // The title block and the optional actions share the heading row; without actions
    // the copy column takes the whole width, exactly as the plain version did.
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(18, 14, 18, 14);
    // index.css: .log-heading gap 16px.
    header_layout->setSpacing(16);
    auto* header_copy = new QWidget(header);
    header_copy->setObjectName(QStringLiteral("cardHeaderCopy"));
    auto* copy_layout = new QVBoxLayout(header_copy);
    copy_layout->setContentsMargins(0, 0, 0, 0);
    copy_layout->setSpacing(3);
    auto* title_label = new QLabel(ui_text(title, language), header_copy);
    title_label->setObjectName(QStringLiteral("cardTitle"));
    title_label->setFont(navigation_font(13));
    title_label->setProperty("uiKey", static_cast<int>(title));
    auto* caption_label = new QLabel(ui_text(caption, language), header_copy);
    caption_label->setObjectName(QStringLiteral("cardCaption"));
    caption_label->setFont(navigation_font(10));
    caption_label->setProperty("uiKey", static_cast<int>(caption));
    copy_layout->addWidget(title_label);
    copy_layout->addWidget(caption_label);
    header_layout->addWidget(header_copy, 1);
    if (trailing != nullptr) {
        header_layout->addWidget(trailing, 0, Qt::AlignVCenter);
    }
    layout->addWidget(header);

    group.card = card;
    group.rows = layout;
    return group;
}

/// The page surface of the mockup: a centred 820 px column (index.css: .page-content).
/// The column is returned and its layout goes into `rows`; the caller appends a trailing
/// stretch, so the cards stay at the top of the page.
QWidget* page_column(QWidget* page, QVBoxLayout*& rows_out)
{
    auto* layout = new QHBoxLayout(page);
    layout->setContentsMargins(30, 28, 30, 60);
    layout->setSpacing(0);
    auto* content = new QWidget(page);
    content->setObjectName(QStringLiteral("pageColumn"));
    content->setMaximumWidth(820);
    auto* rows = new QVBoxLayout(content);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(20);
    layout->addStretch(1);
    layout->addWidget(content);
    layout->addStretch(1);
    rows_out = rows;
    return content;
}

/// The mockup's hint strip: an "i" badge, a bold title and one sentence
/// (index.css: .hint). The hotkey inside the sentence is substituted by the caller.
QWidget* hint_bar(UiKey title, UiKey text, AppLanguage language, const QString& hotkey, QWidget* parent)
{
    auto* bar = new QWidget(parent);
    bar->setObjectName(QStringLiteral("hintBar"));
    bar->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(15, 13, 15, 13);
    layout->setSpacing(11);
    auto* badge = new QLabel(QStringLiteral("i"), bar);
    badge->setObjectName(QStringLiteral("hintBadge"));
    badge->setFixedSize(19, 19);
    badge->setAlignment(Qt::AlignCenter);
    badge->setFont(navigation_font(11));
    auto* title_label = new QLabel(ui_text(title, language), bar);
    title_label->setObjectName(QStringLiteral("hintTitle"));
    title_label->setFont(navigation_font(10));
    title_label->setProperty("uiKey", static_cast<int>(title));
    // Not every hint carries a hotkey (the hotkeys page has none): .arg() on a
    // sentence without %1 would warn and the label would lose its text.
    const bool has_hotkey = !hotkey.isEmpty();
    auto* text_label = new QLabel(
        has_hotkey ? ui_text(text, language).arg(hotkey) : ui_text(text, language), bar);
    text_label->setObjectName(QStringLiteral("hintText"));
    text_label->setFont(navigation_font(10));
    text_label->setWordWrap(true);
    if (has_hotkey) {
        // Not the plain uiKey property: the sentence carries the hotkey, so a language
        // change has to substitute it again (see MainWindow::retranslate).
        text_label->setProperty("hotkeyHintKey", static_cast<int>(text));
    } else {
        // A plain sentence is re-lettered by the generic uiKey pass.
        text_label->setProperty("uiKey", static_cast<int>(text));
    }
    layout->addWidget(badge, 0, Qt::AlignTop);
    layout->addWidget(title_label, 0, Qt::AlignTop);
    layout->addWidget(text_label, 1);
    return bar;
}

/// The mockup's "VoiceTyper runs in the background" banner of the Launch page: a
/// tinted rounded glyph box, a title and one sentence (index.css: .launch-card,
/// .launch-icon). The glyph itself is painted by apply_theme, which knows the
/// palette, so the label is created empty here.
QWidget* launch_card(AppLanguage language, QWidget* parent, QLabel*& icon_out)
{
    auto* card = new QWidget(parent);
    card->setObjectName(QStringLiteral("launchCard"));
    card->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QHBoxLayout(card);
    // index.css: .launch-card padding 16px 18px, gap 13px.
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(13);

    auto* icon = new QLabel(card);
    icon->setObjectName(QStringLiteral("launchIcon"));
    icon->setFixedSize(40, 40);
    icon->setAlignment(Qt::AlignCenter);
    icon_out = icon;

    auto* copy = new QWidget(card);
    copy->setObjectName(QStringLiteral("launchCopy"));
    auto* copy_layout = new QVBoxLayout(copy);
    copy_layout->setContentsMargins(0, 0, 0, 0);
    copy_layout->setSpacing(4);
    auto* title = new QLabel(ui_text(UiKey::k128, language), copy);
    title->setObjectName(QStringLiteral("launchTitle"));
    title->setFont(navigation_font(11));
    title->setProperty("uiKey", static_cast<int>(UiKey::k128));
    auto* text = new QLabel(ui_text(UiKey::k129, language), copy);
    text->setObjectName(QStringLiteral("launchText"));
    text->setFont(navigation_font(10));
    text->setProperty("uiKey", static_cast<int>(UiKey::k129));
    text->setWordWrap(true);
    copy_layout->addWidget(title);
    copy_layout->addWidget(text);

    layout->addWidget(icon, 0, Qt::AlignVCenter);
    layout->addWidget(copy, 1);
    return card;
}

/// The About page's hero (index.css: .about-hero): the brand wave, the product name with
/// its tagline, and the running version in a pill on the right. The wave is painted by
/// apply_theme, which owns the palette, so the label is created empty here.
QWidget* about_hero(AppLanguage language, QWidget* parent, QLabel*& logo_out, QLabel*& version_out)
{
    auto* hero = new QWidget(parent);
    hero->setObjectName(QStringLiteral("aboutHero"));
    hero->setAttribute(Qt::WA_StyledBackground, true);
    // .about-hero: min-height 92px, padding 18px 20px, gap 13px.
    hero->setMinimumHeight(92);
    auto* layout = new QHBoxLayout(hero);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(13);

    auto* logo = new QLabel(hero);
    logo->setObjectName(QStringLiteral("aboutHeroLogo"));
    // index.css: .wave-logo is 42x34.
    logo->setFixedSize(42, 34);
    logo->setAlignment(Qt::AlignCenter);
    logo_out = logo;

    auto* copy = new QWidget(hero);
    copy->setObjectName(QStringLiteral("aboutHeroCopy"));
    auto* copy_layout = new QVBoxLayout(copy);
    copy_layout->setContentsMargins(0, 0, 0, 0);
    copy_layout->setSpacing(4);
    auto* title = new QLabel(QStringLiteral("VoiceTyper"), copy);
    title->setObjectName(QStringLiteral("aboutHeroTitle"));
    title->setFont(navigation_font(18));
    auto* caption = new QLabel(ui_text(UiKey::k153, language), copy);
    caption->setObjectName(QStringLiteral("aboutHeroCaption"));
    caption->setFont(navigation_font(10));
    caption->setProperty("uiKey", static_cast<int>(UiKey::k153));
    caption->setWordWrap(true);
    copy_layout->addWidget(title);
    copy_layout->addWidget(caption);

    auto* chip = new QLabel(hero);
    chip->setObjectName(QStringLiteral("aboutVersionChip"));
    chip->setFont(navigation_font(9));
    chip->setAlignment(Qt::AlignCenter);
    // "Версия <X.Y.Z>": the number arrives with the services, so the label keeps the key
    // and the value apart, and retranslate() rebuilds the sentence in the new language.
    chip->setProperty("versionChipKey", static_cast<int>(UiKey::k63));
    // "Версия <X.Y.Z>" is built from the build's own version, exactly as the page header
    // does; bind_update_controls() replaces it with the services' answer when there is one.
    chip->setProperty("versionValue", QString::fromUtf8(domain::version()));
    chip->setText(ui_text(UiKey::k63, language) + QLatin1Char(' ')
        + QString::fromUtf8(domain::version()));
    version_out = chip;

    layout->addWidget(logo, 0, Qt::AlignVCenter);
    layout->addWidget(copy, 1);
    layout->addWidget(chip, 0, Qt::AlignVCenter);
    return hero;
}

/// The green "your data stays here" line of the card (index.css: .privacy-note). Its tick
/// is painted by apply_theme for the same reason as the hero wave.
QWidget* privacy_note(AppLanguage language, QWidget* parent, QLabel*& icon_out)
{
    auto* note = new QWidget(parent);
    note->setObjectName(QStringLiteral("privacyNote"));
    note->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QHBoxLayout(note);
    // .privacy-note: margin-top 13px, gap 7px.
    layout->setContentsMargins(0, 13, 0, 0);
    layout->setSpacing(7);
    auto* icon = new QLabel(note);
    icon->setObjectName(QStringLiteral("privacyCheck"));
    icon->setFixedSize(16, 16);
    icon->setAlignment(Qt::AlignCenter);
    icon_out = icon;
    auto* text = new QLabel(ui_text(UiKey::k156, language), note);
    text->setObjectName(QStringLiteral("privacyText"));
    text->setFont(navigation_font(10));
    text->setProperty("uiKey", static_cast<int>(UiKey::k156));
    layout->addWidget(icon, 0, Qt::AlignVCenter);
    layout->addWidget(text, 1);
    return note;
}

/// The paragraph and the privacy line inside the About card (index.css: .about-copy).
/// The two sentences keep their own keys, so the generic re-lettering pass cannot touch
/// them: they are joined here and rebuilt together in retranslate().
QWidget* about_copy(AppLanguage language, QWidget* parent, QLabel*& privacy_icon_out)
{
    auto* block = new QWidget(parent);
    block->setObjectName(QStringLiteral("aboutCopy"));
    block->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(block);
    // .about-copy: padding 17px 18px.
    layout->setContentsMargins(18, 17, 18, 17);
    layout->setSpacing(0);
    auto* paragraph = new QLabel(ui_text(UiKey::k74, language) + QLatin1Char(' ')
            + ui_text(UiKey::k75, language), block);
    paragraph->setObjectName(QStringLiteral("aboutParagraph"));
    paragraph->setFont(navigation_font(11));
    paragraph->setWordWrap(true);
    paragraph->setProperty("aboutCopyKeys",
        QVariantList{static_cast<int>(UiKey::k74), static_cast<int>(UiKey::k75)});
    layout->addWidget(paragraph);
    layout->addWidget(privacy_note(language, block, privacy_icon_out));
    return block;
}

/// The repository the update check already talks to (core/support/update_service.hpp:
/// `Repository "mops1k/VoiceTyper"`), as the web address a reader can open. The three
/// links below are derived from it, so moving the project is a change to this one line.
constexpr const char* kRepositoryWebUrl = "https://github.com/mops1k/VoiceTyper";

/// The three link paths of the About page, in the order of the mockup's .about-links.
constexpr const char* kAboutLinkPaths[] = {
    "/blob/main/THIRD_PARTY_NOTICES.md",
    "/blob/main/README.ru.md",
    "/issues",
};

/// The three link-buttons under the About card (index.css: .about-links). They are real
/// buttons, not labels, because QDesktopServices opens the platform's default browser.
QWidget* about_links(AppLanguage language, QWidget* parent)
{
    auto* row = new QWidget(parent);
    row->setObjectName(QStringLiteral("aboutLinks"));
    row->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    // .about-links: gap 17px.
    layout->setSpacing(17);
    const std::array<UiKey, 3> keys{UiKey::k157, UiKey::k158, UiKey::k159};
    static_assert(std::size(kAboutLinkPaths) == 3, "one address per link");
    for (std::size_t index = 0; index < keys.size(); ++index) {
        auto* button = new QPushButton(ui_text(keys[index], language), row);
        button->setObjectName(QStringLiteral("aboutLink"));
        button->setProperty("uiKey", static_cast<int>(keys[index]));
        button->setCursor(Qt::PointingHandCursor);
        const QString url = QString::fromLatin1(kRepositoryWebUrl)
            + QString::fromLatin1(kAboutLinkPaths[index]);
        // The button is its own connection context: this helper is a free function, so
        // there is no window to hang the connection on.
        QObject::connect(button, &QPushButton::clicked, button,
            [url] { QDesktopServices::openUrl(QUrl(url)); });
        layout->addWidget(button, 0);
    }
    layout->addStretch(1);
    return row;
}

/// The microphone test of the mockup (index.css: .microphone-test): the copy and the
/// button on one line, the level meter across both under them. The button and the state
/// line belong to MainWindow - it re-letters and re-states them - so they are passed in
/// and returned through the out-parameters.
QWidget* microphone_test_block(AppLanguage language, QWidget* parent, QPushButton* button,
    QLabel*& state_out, QLabel*& result_out, QWidget*& meter_out)
{
    auto* block = new QWidget(parent);
    block->setObjectName(QStringLiteral("microphoneTest"));
    block->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QGridLayout(block);
    // .microphone-test: padding 14px 18px 18px, gap 12px 20px.
    layout->setContentsMargins(18, 14, 18, 18);
    layout->setHorizontalSpacing(20);
    layout->setVerticalSpacing(12);

    auto* copy = new QWidget(block);
    copy->setObjectName(QStringLiteral("microphoneTestCopy"));
    auto* copy_layout = new QVBoxLayout(copy);
    copy_layout->setContentsMargins(0, 0, 0, 0);
    copy_layout->setSpacing(4);
    auto* title = new QLabel(ui_text(UiKey::k58, language), copy);
    title->setObjectName(QStringLiteral("microphoneTestTitle"));
    title->setFont(navigation_font(12));
    title->setProperty("uiKey", static_cast<int>(UiKey::k58));
    auto* state = new QLabel(ui_text(UiKey::k147, language), copy);
    state->setObjectName(QStringLiteral("microphoneTestState"));
    state->setFont(navigation_font(10));
    // The state is one of two table sentences, so the key is kept on the label and the
    // generic re-lettering pass follows it (bind_microphone_controls swaps it).
    state->setProperty("uiKey", static_cast<int>(UiKey::k147));
    state->setWordWrap(true);
    // No height is reserved here: fontMetrics() before the stylesheet is polished is not
    // the font the label ends up with. apply_theme() reserves the two lines, once the
    // stylesheet font is in effect (the Qt nested-label limit is described there).
    copy_layout->addWidget(title);
    copy_layout->addWidget(state);
    state_out = state;

    auto* result = new QLabel(block);
    result->setObjectName(QStringLiteral("microphoneTestResult"));
    result->setFont(navigation_font(10));
    result->setWordWrap(true);
    // The verdict appears only when the probe has one: an empty line would push the row
    // taller for nothing. No uiKey property either - the text is the service's answer,
    // not a table string.
    result->hide();
    result_out = result;

    auto* meter = new LevelMeter(block);
    meter_out = meter;

    layout->addWidget(copy, 0, 0);
    layout->addWidget(button, 0, 1, Qt::AlignVCenter);
    layout->addWidget(meter, 1, 0, 1, 2);
    layout->addWidget(result, 2, 0, 1, 2);
    layout->setColumnStretch(0, 1);
    layout->setColumnStretch(1, 0);
    return block;
}

/// Icons are drawn, not taken from a glyph font.
///
/// The .NET build used Segoe MDL2 Assets, and the port copied its code points - but those
/// glyphs do not render on this machine (the same finding is recorded on the title bar
/// buttons below). Geometry cannot depend on a font, so the navigation icons, the model
/// buttons and the record button are painted from primitives.
enum class DrawnIcon { general, appearance, models, hotkeys, microphone, startup, launch, log, about,
    download, remove, record, stop, cancel, wave, check, sun, moon, monitor, reset };

void paint_drawn_icon(QPainter& painter, DrawnIcon icon, const QRectF& box, const QColor& colour)
{
    painter.setPen(QPen(colour, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    const qreal x = box.left();
    const qreal y = box.top();
    const qreal w = box.width();
    const qreal h = box.height();
    const auto point = [&](qreal fx, qreal fy) { return QPointF(x + w * fx, y + h * fy); };
    switch (icon) {
    case DrawnIcon::general: // a gear: circle, hub and four teeth
        painter.drawEllipse(box.adjusted(w * 0.18, h * 0.18, -w * 0.18, -h * 0.18));
        painter.drawEllipse(box.adjusted(w * 0.42, h * 0.42, -w * 0.42, -h * 0.42));
        painter.drawLine(point(0.5, 0.0), point(0.5, 0.16));
        painter.drawLine(point(0.5, 0.84), point(0.5, 1.0));
        painter.drawLine(point(0.0, 0.5), point(0.16, 0.5));
        painter.drawLine(point(0.84, 0.5), point(1.0, 0.5));
        break;
    case DrawnIcon::appearance: // a palette: circle with three dots
        painter.drawEllipse(box.adjusted(w * 0.08, h * 0.08, -w * 0.08, -h * 0.08));
        painter.setBrush(colour);
        painter.drawEllipse(point(0.32, 0.36), w * 0.07, w * 0.07);
        painter.drawEllipse(point(0.60, 0.34), w * 0.07, w * 0.07);
        painter.drawEllipse(point(0.44, 0.62), w * 0.07, w * 0.07);
        break;
    case DrawnIcon::models: // a stack of layers
        painter.drawPolygon(QPolygonF{point(0.5, 0.05), point(0.95, 0.30), point(0.5, 0.55), point(0.05, 0.30)});
        painter.drawPolyline(QPolygonF{point(0.05, 0.55), point(0.5, 0.80), point(0.95, 0.55)});
        break;
    case DrawnIcon::hotkeys: // a keyboard: a rounded box with key dots
        painter.drawRoundedRect(box.adjusted(0, h * 0.22, 0, -h * 0.22), 2.0, 2.0);
        painter.setBrush(colour);
        painter.drawEllipse(point(0.22, 0.5), w * 0.05, w * 0.05);
        painter.drawEllipse(point(0.5, 0.5), w * 0.05, w * 0.05);
        painter.drawEllipse(point(0.78, 0.5), w * 0.05, w * 0.05);
        break;
    case DrawnIcon::microphone: // a capsule with a stand
        painter.drawRoundedRect(QRectF(point(0.32, 0.05), point(0.68, 0.52)), w * 0.18, w * 0.18);
        painter.drawArc(QRectF(point(0.18, 0.28), point(0.82, 0.72)), 200 * 16, 140 * 16);
        painter.drawLine(point(0.5, 0.72), point(0.5, 0.92));
        painter.drawLine(point(0.30, 0.95), point(0.70, 0.95));
        break;
    case DrawnIcon::startup: // a power symbol
        painter.drawArc(box.adjusted(w * 0.08, h * 0.10, -w * 0.08, -h * 0.02), 60 * 16, 60 * 16);
        painter.drawArc(box.adjusted(w * 0.08, h * 0.10, -w * 0.08, -h * 0.02), 240 * 16, 60 * 16);
        painter.drawLine(point(0.5, 0.02), point(0.5, 0.42));
        break;
    case DrawnIcon::launch: // a play triangle (the mockup's `launch` glyph)
        painter.drawPolygon(QPolygonF{point(0.33, 0.21), point(0.79, 0.5), point(0.33, 0.79)});
        break;
    case DrawnIcon::log: // lines of text
        for (int line = 0; line < 4; ++line) {
            const qreal fy = 0.12 + 0.25 * line;
            painter.drawLine(point(0.08, fy), point(line == 3 ? 0.55 : 0.92, fy));
        }
        break;
    case DrawnIcon::about: // an information circle
        painter.drawEllipse(box.adjusted(w * 0.05, h * 0.05, -w * 0.05, -h * 0.05));
        painter.setBrush(colour);
        painter.drawEllipse(point(0.5, 0.26), w * 0.055, w * 0.055);
        painter.drawLine(point(0.5, 0.42), point(0.5, 0.74));
        break;
    case DrawnIcon::download: // an arrow into a tray
        painter.drawLine(point(0.5, 0.05), point(0.5, 0.62));
        painter.drawPolyline(QPolygonF{point(0.30, 0.44), point(0.5, 0.66), point(0.70, 0.44)});
        painter.drawPolyline(QPolygonF{point(0.12, 0.78), point(0.12, 0.95), point(0.88, 0.95), point(0.88, 0.78)});
        break;
    case DrawnIcon::remove: // a bin with a lid
        painter.drawLine(point(0.10, 0.24), point(0.90, 0.24));
        painter.drawLine(point(0.38, 0.24), point(0.38, 0.12));
        painter.drawLine(point(0.62, 0.24), point(0.62, 0.12));
        painter.drawLine(point(0.38, 0.12), point(0.62, 0.12));
        painter.drawPolyline(QPolygonF{point(0.20, 0.24), point(0.26, 0.94), point(0.74, 0.94), point(0.80, 0.24)});
        break;
    case DrawnIcon::record: // a filled dot
        painter.setBrush(colour);
        painter.drawEllipse(box.adjusted(w * 0.14, h * 0.14, -w * 0.14, -h * 0.14));
        break;
    case DrawnIcon::cancel: // a cross
        painter.drawLine(point(0.15, 0.15), point(0.85, 0.85));
        painter.drawLine(point(0.85, 0.15), point(0.15, 0.85));
        break;
    case DrawnIcon::stop: // a rounded square
        painter.setBrush(colour);
        painter.drawRoundedRect(box.adjusted(w * 0.16, h * 0.16, -w * 0.16, -h * 0.16), 2.0, 2.0);
        break;
    case DrawnIcon::wave: {
        // The brand wave of the mockup (App.tsx: <WaveLogo/>, a 48x32 viewBox): the very
        // path of the reference, with its relative cubic segments resolved to absolute
        // control points. The box should keep the viewBox's 3:2 aspect (about_hero does).
        painter.setPen(QPen(colour, std::max(1.2, box.width() * 0.075), Qt::SolidLine,
            Qt::RoundCap, Qt::RoundJoin));
        QPainterPath path;
        path.moveTo(point(2.0 / 48, 17.0 / 32));
        path.lineTo(point(6.0 / 48, 17.0 / 32));
        const auto cubic = [&](double c1x, double c1y, double c2x, double c2y, double ex, double ey) {
            path.cubicTo(point(c1x / 48, c1y / 32), point(c2x / 48, c2y / 32), point(ex / 48, ey / 32));
        };
        cubic(8, 17, 8, 10, 10, 10);
        cubic(12, 10, 12, 23, 14, 23);
        cubic(16, 23, 16, 4, 19, 4);
        cubic(22, 4, 21, 28, 24, 28);
        cubic(27, 28, 27, 4, 30, 4);
        cubic(33, 4, 32, 23, 35, 23);
        cubic(38, 23, 37, 10, 39, 10);
        cubic(41, 10, 41, 17, 43, 17);
        path.lineTo(point(46.0 / 48, 17.0 / 32));
        painter.drawPath(path);
        break;
    }
    case DrawnIcon::check: // the tick of the privacy line
        painter.setPen(QPen(colour, std::max(1.6, box.width() * 0.17), Qt::SolidLine,
            Qt::RoundCap, Qt::RoundJoin));
        painter.drawPolyline(QPolygonF{point(0.12, 0.55), point(0.40, 0.82), point(0.88, 0.18)});
        break;
    case DrawnIcon::sun: // the disc with eight rays of the "Светлая" tile
        painter.drawEllipse(box.center(), w * 0.21, h * 0.21);
        painter.drawLine(point(0.50, 0.02), point(0.50, 0.16));
        painter.drawLine(point(0.50, 0.84), point(0.50, 0.98));
        painter.drawLine(point(0.02, 0.50), point(0.16, 0.50));
        painter.drawLine(point(0.84, 0.50), point(0.98, 0.50));
        painter.drawLine(point(0.17, 0.17), point(0.27, 0.27));
        painter.drawLine(point(0.73, 0.73), point(0.83, 0.83));
        painter.drawLine(point(0.83, 0.17), point(0.73, 0.27));
        painter.drawLine(point(0.27, 0.73), point(0.17, 0.83));
        break;
    case DrawnIcon::moon: {
        // The crescent of the mockup (App.tsx: the moon glyph): the 9/24 disc with the
        // 7/24 circle at (11.2, 3) taken out of it.
        QPainterPath disc;
        disc.addEllipse(box.center(), w * 0.375, h * 0.375);
        QPainterPath bite;
        bite.addEllipse(point(0.467, 0.125), w * 0.292, h * 0.292);
        painter.drawPath(disc.subtracted(bite));
        break;
    }
    case DrawnIcon::monitor: // the screen with its stand of the "Системная" tile
        painter.drawRoundedRect(QRectF(point(0.10, 0.17), point(0.90, 0.69)), 2.0, 2.0);
        painter.drawLine(point(0.50, 0.69), point(0.50, 0.86));
        painter.drawLine(point(0.33, 0.86), point(0.67, 0.86));
        break;
    case DrawnIcon::reset: // the circular arrow of the reset button
        painter.drawArc(QRectF(point(0.125, 0.125), point(0.875, 0.875)), 60 * 16, 300 * 16);
        painter.drawPolyline(QPolygonF{point(0.125, 0.125), point(0.125, 0.333), point(0.333, 0.333)});
        break;
    }
}

/// The navigation key names the same icon the .NET template used per entry.
DrawnIcon navigation_drawn_icon(std::string_view key)
{
    if (key == "Main") return DrawnIcon::general;
    if (key == "Appearance") return DrawnIcon::appearance;
    if (key == "Models") return DrawnIcon::models;
    if (key == "Hotkeys") return DrawnIcon::hotkeys;
    if (key == "Microphone") return DrawnIcon::microphone;
    if (key == "Startup") return DrawnIcon::startup;
    if (key == "Log") return DrawnIcon::log;
    return DrawnIcon::about;
}

QIcon drawn_icon(DrawnIcon icon, int size, const QColor& colour)
{
    // Device pixels, not logical ones: an 18x18 pixmap carrying a 1.25 ratio has only
    // 14.4 logical px of paper, so the glyph was painted partly outside it and came out
    // clipped and looking off centre on a 125% display (Alexander, 08.10.2026). The ratio
    // is clamped to at least 1 because a zero ratio used to give a null pixmap.
    const qreal ratio = (qApp != nullptr && qApp->devicePixelRatio() > 1.0)
        ? qApp->devicePixelRatio()
        : 1.0;
    QPixmap pixmap(qRound(size * ratio), qRound(size * ratio));
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(ratio);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal inset = size * 0.16;
    paint_drawn_icon(painter, icon, QRectF(inset, inset, size - 2 * inset, size - 2 * inset), colour);
    return QIcon(pixmap);
}

/// The hero's wave at the mockup's own 42x34 (index.css: .wave-logo). It is not drawn
/// through drawn_icon() because that box is square and the reference glyph is 3:2 - a
/// square box would squeeze the wave vertically.
QPixmap brand_wave_pixmap(int width, int height, const QColor& colour)
{
    // Same device-pixel rule as drawn_icon(): the hero's wave must not be clipped by its
    // own pixmap on a scaled display.
    const qreal ratio = (qApp != nullptr && qApp->devicePixelRatio() > 1.0)
        ? qApp->devicePixelRatio()
        : 1.0;
    QPixmap pixmap(qRound(width * ratio), qRound(height * ratio));
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(ratio);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    qreal glyph_height = height * 0.78;
    qreal glyph_width = glyph_height * 1.5;
    if (glyph_width > width * 0.96) {
        glyph_width = width * 0.96;
        glyph_height = glyph_width / 1.5;
    }
    paint_drawn_icon(painter, DrawnIcon::wave,
        QRectF((width - glyph_width) / 2.0, (height - glyph_height) / 2.0, glyph_width, glyph_height),
        colour);
    return pixmap;
}

/// The fixed colours of a theme preview (index.css: .theme-preview, .dark-preview,
/// .system-preview). They describe a picture of a theme, not the theme the window is in,
/// so they are deliberately not part of struct Palette: the light tile must look light
/// while the dark theme is applied, and the other way round.
struct PreviewColours {
    const char* sidebar;
    const char* sidebar_line;
    const char* content;
    const char* bar;
    const char* wide_bar;
    const char* border;
};

constexpr PreviewColours kLightPreview{
    "#F1F3F5", "#DCE1E5", "#FFFFFF", "#DBE0E4", "#EFF2F4", "#DCE1E5"};
constexpr PreviewColours kDarkPreview{
    "#1D2023", "#34383C", "#25282B", "#464B4F", "#30353A", "#363A3E"};
/// .system-preview border and the diagonal that separates its two halves.
constexpr const char* kSystemPreviewBorder = "#C9D0D5";
constexpr const char* kSystemPreviewDivider = "#8C959C";

/// The two bars of a preview, at the mockup's 18/14 px inset (index.css: .theme-preview b
/// i: 7 px with a 10 px margin, then a 32 px block; the .system-half variant spaces them
/// by 8 px instead of 10).
void paint_preview_bars(QPainter& painter, const QRectF& field, const char* bar, const char* wide_bar,
    qreal second_top)
{
    const qreal width = field.width() - 28.0;
    if (width <= 0.0 || field.height() <= 0.0) {
        return;
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QString::fromLatin1(bar)));
    painter.drawRoundedRect(QRectF(field.left() + 14.0, field.top() + 18.0, width, 7.0), 3.0, 3.0);
    painter.setBrush(QColor(QString::fromLatin1(wide_bar)));
    painter.drawRoundedRect(QRectF(field.left() + 14.0, field.top() + second_top, width, 32.0), 3.0, 3.0);
}

/// The miniature of the window a theme tile shows (index.css: .theme-preview): a 27%
/// sidebar with two bars beside it, in the three variants of the mockup. The system
/// variant is the other two split diagonally (index.css: .system-half clip-path).
class ThemePreview final : public QWidget {
public:
    explicit ThemePreview(AppTheme theme, QWidget* parent = nullptr)
        : QWidget(parent)
        , theme_(theme)
    {
        setObjectName(QStringLiteral("themePreview"));
        setAttribute(Qt::WA_StyledBackground, true);
        // index.css: .theme-preview height 100px.
        setFixedHeight(100);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        // The 1px frame of .theme-preview (radius 5px, overflow hidden): everything is
        // painted inside the field it leaves, so the corners stay round.
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const QRectF field = box.adjusted(1.0, 1.0, -1.0, -1.0);
        QPainterPath frame;
        frame.addRoundedRect(field, 4.0, 4.0);

        QColor border;
        if (theme_ == AppTheme::system) {
            border = QColor(QString::fromLatin1(kSystemPreviewBorder));
            const qreal w = field.width();
            const qreal h = field.height();
            // .system-half.light: polygon(0 0, 58% 0, 42% 100%, 0 100%) and its dark
            // counterpart; each half is clipped to its own polygon, so the bars of one
            // cannot run over the diagonal.
            const QPolygonF light{field.topLeft(), field.topLeft() + QPointF(w * 0.58, 0.0),
                field.topLeft() + QPointF(w * 0.42, h), field.bottomLeft()};
            const QPolygonF dark{field.topLeft() + QPointF(w * 0.58, 0.0), field.topRight(),
                field.bottomRight(), field.topLeft() + QPointF(w * 0.42, h)};
            painter.save();
            painter.setClipPath(frame);
            const auto paint_half = [&painter, &field](const QPolygonF& half, const char* background,
                                        const char* bar, const char* wide_bar) {
                painter.save();
                QPainterPath half_path;
                half_path.addPolygon(half);
                painter.setClipPath(half_path, Qt::IntersectClip);
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(QString::fromLatin1(background)));
                painter.drawRect(field);
                paint_preview_bars(painter, field, bar, wide_bar, 33.0);
                painter.restore();
            };
            paint_half(light, kLightPreview.sidebar, kLightPreview.bar, kLightPreview.wide_bar);
            paint_half(dark, kDarkPreview.content, kDarkPreview.bar, kDarkPreview.wide_bar);
            // The two clip paths meet on one line: it is drawn so the halves read as a
            // deliberate split rather than as a rendering seam.
            painter.setClipping(false);
            painter.setPen(QPen(QColor(QString::fromLatin1(kSystemPreviewDivider)), 1.0));
            painter.drawLine(QPointF(field.left() + w * 0.58, field.top()),
                QPointF(field.left() + w * 0.42, field.bottom()));
            painter.restore();
        } else {
            const PreviewColours& colours = theme_ == AppTheme::dark ? kDarkPreview : kLightPreview;
            border = QColor(QString::fromLatin1(colours.border));
            painter.save();
            painter.setClipPath(frame);
            // index.css: .theme-preview grid-template-columns 27% 1fr.
            const qreal sidebar = field.width() * 0.27;
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(QString::fromLatin1(colours.content)));
            painter.drawRect(field);
            painter.setBrush(QColor(QString::fromLatin1(colours.sidebar)));
            painter.drawRect(QRectF(field.left(), field.top(), sidebar, field.height()));
            painter.setPen(QPen(QColor(QString::fromLatin1(colours.sidebar_line)), 1.0));
            painter.drawLine(QPointF(field.left() + sidebar, field.top()),
                QPointF(field.left() + sidebar, field.bottom()));
            painter.setPen(Qt::NoPen);
            paint_preview_bars(painter, field, colours.bar, colours.wide_bar, 35.0);
            painter.restore();
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(border, 1.0));
        painter.drawRoundedRect(box, 5.0, 5.0);
    }

private:
    AppTheme theme_;
};

/// The object name of a tile: the layout test and a snapshot review find them by it.
QString theme_tile_object_name(AppTheme theme)
{
    switch (theme) {
    case AppTheme::light: return QStringLiteral("themeTileLight");
    case AppTheme::dark: return QStringLiteral("themeTileDark");
    case AppTheme::system: return QStringLiteral("themeTileSystem");
    }
    return QStringLiteral("themeTileSystem");
}

/// The caption of a tile (App.tsx:126-137: "Системная", "Светлая", "Тёмная").
UiKey theme_tile_name_key(AppTheme theme)
{
    switch (theme) {
    case AppTheme::light: return UiKey::k164;
    case AppTheme::dark: return UiKey::k165;
    case AppTheme::system: return UiKey::k163;
    }
    return UiKey::k163;
}

/// The glyph of a tile (App.tsx:126-137: a monitor, a sun, a moon).
DrawnIcon theme_tile_icon(AppTheme theme)
{
    switch (theme) {
    case AppTheme::light: return DrawnIcon::sun;
    case AppTheme::dark: return DrawnIcon::moon;
    case AppTheme::system: return DrawnIcon::monitor;
    }
    return DrawnIcon::monitor;
}

/// One tile of the mockup's .theme-options: the preview of a theme with its name under
/// it, clickable as a whole. Deliberately no Q_OBJECT (the same choice as LevelMeter
/// above): the tile paints itself, shows its selected state and reports a click through
/// a callback, so no signal, slot or meta-object is needed.
class ThemeTile final : public QWidget {
public:
    ThemeTile(AppTheme theme, AppLanguage language, QWidget* parent = nullptr)
        : QWidget(parent)
        , theme_(theme)
    {
        setObjectName(theme_tile_object_name(theme));
        setAttribute(Qt::WA_StyledBackground, true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::ClickFocus);

        auto* layout = new QVBoxLayout(this);
        // index.css: .theme-options > button padding 7px.
        layout->setContentsMargins(7, 7, 7, 7);
        layout->setSpacing(0);
        layout->addWidget(new ThemePreview(theme, this));

        // The caption row: the 17px glyph beside the 11px name, centred, with the
        // mockup's 10px 0 3px padding (.theme-options > button > span).
        auto* labels = new QWidget(this);
        labels->setObjectName(QStringLiteral("themeTileLabels"));
        labels->setAttribute(Qt::WA_StyledBackground, true);
        auto* labels_layout = new QHBoxLayout(labels);
        labels_layout->setContentsMargins(0, 10, 0, 3);
        labels_layout->setSpacing(7);
        icon_ = new QLabel(labels);
        icon_->setObjectName(QStringLiteral("themeTileIcon"));
        icon_->setFixedSize(17, 17);
        icon_->setAlignment(Qt::AlignCenter);
        name_ = new QLabel(ui_text(theme_tile_name_key(theme), language), labels);
        name_->setObjectName(QStringLiteral("themeTileName"));
        name_->setProperty("uiKey", static_cast<int>(theme_tile_name_key(theme)));
        labels_layout->addStretch(1);
        labels_layout->addWidget(icon_, 0, Qt::AlignVCenter);
        labels_layout->addWidget(name_, 0, Qt::AlignVCenter);
        labels_layout->addStretch(1);
        layout->addWidget(labels);

        // The check of the selected tile (index.css: .theme-options em): a 19px accent
        // disc with a white tick, 4px inside the top-right corner. It is a widget of its
        // own because the tile cannot paint over its own children.
        check_ = new QLabel(this);
        check_->setObjectName(QStringLiteral("themeCheck"));
        check_->setFixedSize(19, 19);
        check_->setAlignment(Qt::AlignCenter);
        check_->setPixmap(drawn_icon(DrawnIcon::check, 12, QColor(QStringLiteral("#FFFFFF"))).pixmap(12, 12));
        check_->hide();
        // Both carry the state from the start: the stylesheet selects on the property of
        // the tile, the name colour on the one of the label.
        setProperty("selected", false);
        name_->setProperty("selected", false);
    }

    [[nodiscard]] AppTheme theme() const noexcept { return theme_; }

    /// The selected state of the mockup: the accent border comes from the stylesheet,
    /// the glow ring is painted below and the disc appears in the corner.
    void set_selected(bool selected)
    {
        if (selected == selected_) {
            return;
        }
        selected_ = selected;
        // The stylesheet selects the tile on this property, and the name colour on the
        // one of the label: index.css: .theme-options > button.selected { color: var(--text) }.
        setProperty("selected", selected);
        name_->setProperty("selected", selected);
        check_->setVisible(selected);
        for (QWidget* widget : {static_cast<QWidget*>(this), static_cast<QWidget*>(name_)}) {
            widget->style()->unpolish(widget);
            widget->style()->polish(widget);
        }
        update();
    }

    void set_label_icon(const QPixmap& pixmap) { icon_->setPixmap(pixmap); }

    /// What a click does; the window sets it, because it owns the presenter.
    std::function<void(AppTheme)> on_click;

protected:
    void paintEvent(QPaintEvent*) override
    {
        if (!selected_) {
            return;
        }
        // The mockup's glow (box-shadow 0 0 0 2px rgba(54,174,234,.12)): QSS has no
        // box-shadow, so the ring is painted over the accent border the stylesheet draws.
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(54, 174, 234, 31), 3.0));
        painter.drawRoundedRect(QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5), 7.5, 7.5);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
            activate();
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return
            || event->key() == Qt::Key_Enter) {
            activate();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void resizeEvent(QResizeEvent* event) override
    {
        // index.css: .theme-options em { right: 4px; top: 4px }: an absolutely positioned
        // child is placed against the padding box, which the 1px border insets.
        check_->move(width() - 24, 5);
        check_->raise();
        QWidget::resizeEvent(event);
    }

private:
    void activate()
    {
        if (on_click) {
            on_click(theme_);
        }
    }

    AppTheme theme_;
    bool selected_ = false;
    QLabel* icon_ = nullptr;
    QLabel* name_ = nullptr;
    QLabel* check_ = nullptr;
};

/// The three calls MainWindow makes on a theme tile. The class is in this anonymous
/// namespace, so its methods cannot be named in main_window.hpp: the member stays a plain
/// QWidget* and these helpers are the only door to it.
AppTheme theme_tile_theme(const QWidget* tile)
{
    return static_cast<const ThemeTile*>(tile)->theme();
}

void theme_tile_set_selected(QWidget* tile, bool selected)
{
    static_cast<ThemeTile*>(tile)->set_selected(selected);
}

void theme_tile_set_label_icon(QWidget* tile, const QPixmap& pixmap)
{
    static_cast<ThemeTile*>(tile)->set_label_icon(pixmap);
}


} // namespace

/// Whether a registered page belongs to a navigation entry.
///
/// The page is registered under the label it was built with, and that label is localised -
/// so comparing it with the current language alone made the English interface attach pages
/// to the wrong entries (reported from the running build: the appearance page showed the
/// models). Both spellings are accepted, so the order always follows kNavigation.
[[nodiscard]] bool page_matches_entry(const QString& registered, const NavEntry& entry,
    AppLanguage language)
{
    return registered == ui_text(entry.text, language)
        || registered == QString::fromUtf8(entry.label);
}

void MainWindow::build_tabs()
{
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // --- title bar (MainWindow.axaml:137-150) --------------------------------
    auto* title_bar = new QWidget(central);
    title_bar->setObjectName(QStringLiteral("titleBar"));
    // The mockup's title bar is 40 px tall with a 14 px inset (index.css: the 40px
    // grid row and .titlebar padding-left 14px).
    title_bar->setFixedHeight(40);
    auto* title_layout = new QHBoxLayout(title_bar);
    title_layout->setContentsMargins(14, 0, 0, 0);
    title_layout->setSpacing(0);
    auto* title_text = new QLabel(QStringLiteral("VoiceTyper"), title_bar);
    title_text->setObjectName(QStringLiteral("titleText"));
    QFont title_font = navigation_font(12);
    title_font.setWeight(QFont::DemiBold);
    title_text->setFont(title_font);
    title_layout->addWidget(title_text);
    title_layout->addStretch(1);
    // The window buttons must use the icon font explicitly. Relying on a font
    // family list made Qt pick the glyph out of a fallback font, and the shapes it
    // produced were not the minimize/close buttons the .NET window shows - reported
    // from the running build. Without the icon font the text fallback is used, so
    // the buttons are still legible on a machine that has no MDL2 at all.
    const auto add_title_button = [&](const char* object_name, TitleButtonKind kind) {
        auto* button = new QPushButton(title_bar);
        button->setObjectName(QString::fromUtf8(object_name));
        // MainWindow.axaml:73-83: Width 46, Height 31, Padding 0, content centred,
        // no corner radius. The symbol itself is drawn as geometry: the Segoe MDL2
        // Assets glyphs E921/E8BB did not render as the window buttons of the .NET
        // build on this machine, and a drawn line/cross cannot depend on a font.
        // 46 px wide, full height of the 40 px bar, like the mockup's window buttons.
        button->setFixedSize(46, 40);
        button->setIconSize(QSize(10, 10));
        title_button_icons_.emplace_back(button, kind);
        button->setFlat(true);
        button->setCursor(Qt::ArrowCursor);
        button->setFocusPolicy(Qt::NoFocus);
        switch (kind) {
        case TitleButtonKind::close:
            // The tray owns shutdown (compatibility-contracts.md §2): closing the
            // window hides it, exactly like the .NET build.
            connect(button, &QPushButton::clicked, this, &QWidget::hide);
            break;
        case TitleButtonKind::minimize:
            connect(button, &QPushButton::clicked, this, &QWidget::showMinimized);
            break;
        }
        title_layout->addWidget(button);
    };
    add_title_button("titleBarMinimize", TitleButtonKind::minimize);
    add_title_button("titleBarClose", TitleButtonKind::close);
    layout->addWidget(title_bar);

    // --- body: sidebar + pages (MainWindow.axaml:152-200) --------------------
    auto* body = new QHBoxLayout();
    // A right margin on the content column: the page's scrollbar used to be drawn
    // flush against the window edge, and any page that needed one shifted its whole
    // column 10 px left compared with the pages that did not.
    body->setContentsMargins(0, 0, 10, 0);
    body->setSpacing(0);

    auto* sidebar = new QWidget(central);
    sidebar->setObjectName(QStringLiteral("sidebar"));
    // The navigation column takes about a third of the window, like the reference,
    // but never so much that the settings are squeezed (resizeEvent keeps it in
    // step with the width).
    sidebar->setFixedWidth(300);
    sidebar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
    sidebar->setMinimumSize(210, 1);
    auto* sidebar_layout = new QVBoxLayout(sidebar);
    sidebar_layout->setContentsMargins(12, 22, 12, 14);
    sidebar_layout->setSpacing(0);

    auto* brand = new QWidget(sidebar);
    auto* brand_layout = new QHBoxLayout(brand);
    brand_layout->setContentsMargins(17, 0, 17, 13);
    brand_layout->setSpacing(10);
    auto* brand_icon = new QLabel(brand);
    const auto app_icon = windowIcon();
    if (!app_icon.isNull()) {
        brand_icon->setPixmap(app_icon.pixmap(34, 34));
    }
    brand_layout->addWidget(brand_icon, 0, Qt::AlignTop);
    auto* brand_text = new QWidget(brand);
    auto* brand_text_layout = new QVBoxLayout(brand_text);
    brand_text_layout->setContentsMargins(0, 0, 0, 0);
    brand_text_layout->setSpacing(2);
    auto* brand_name = new QLabel(QStringLiteral("VoiceTyper"), brand_text);
    QFont brand_font = navigation_font(14);
    brand_font.setBold(true);
    brand_name->setFont(brand_font);
    // The mockup uppercases the tagline with text-transform and sets 9 px; Qt styles
    // have no text-transform, so the string is uppercased here.
    auto* brand_tagline = new QLabel(ui_text(UiKey::k47, language_), brand_text);
    brand_tagline->setObjectName(QStringLiteral("brandTagline"));
    // Both properties: the key so a language switch re-letters the label, and the
    // uppercasing the mockup does with text-transform (Qt styles have no such property).
    brand_tagline->setProperty("uiKey", static_cast<int>(UiKey::k47));
    brand_tagline->setProperty("uppercaseText", true);
    brand_tagline->setFont(navigation_font(9));
    brand_text_layout->addWidget(brand_name);
    brand_text_layout->addWidget(brand_tagline);
    brand_layout->addWidget(brand_text, 1);
    sidebar_layout->addWidget(brand);

    nav_ = new QListWidget(sidebar);
    nav_->setObjectName(QStringLiteral("sideNav"));
    nav_->setFrameShape(QFrame::NoFrame);
    nav_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    nav_->setFont(navigation_font(13));
    // The mockup spaces navigation rows by 3 px (index.css: nav gap 3px).
    nav_->setSpacing(3);
    nav_->setFocusPolicy(Qt::NoFocus);
    // The sidebar is a `*` row as well: its list must not force a minimum height
    // that pushes the footer out of the 640 px window.
    nav_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    nav_->setMinimumSize(0, 1);
    // The bar lives in the list's viewport, above the rows: Qt styles can draw a border
    // on a whole item, but the mockup's bar is a short 3x19 px pill centred on the row.
    nav_accent_ = new QWidget(nav_->viewport());
    nav_accent_->setObjectName(QStringLiteral("navAccent"));
    nav_accent_->setAttribute(Qt::WA_StyledBackground, true);
    nav_accent_->setFixedSize(3, 19);
    nav_accent_->hide();
    sidebar_layout->addWidget(nav_, 1);
    body->addWidget(sidebar);

    // The page title lives in its own band above the pages, like the reference
    // ("User Activities" on a tinted strip), instead of scrolling away with the rows.
    page_header_ = new QWidget(central);
    page_header_->setObjectName(QStringLiteral("pageHeader"));
    page_header_->setAttribute(Qt::WA_StyledBackground, true);
    auto* header_layout = new QHBoxLayout(page_header_);
    // The mockup insets the header band (index.css: .page-header padding 12px 28px 13px 38px).
    header_layout->setContentsMargins(38, 12, 28, 13);
    auto* header_text = new QWidget(page_header_);
    auto* header_text_layout = new QVBoxLayout(header_text);
    header_text_layout->setContentsMargins(0, 0, 0, 0);
    header_text_layout->setSpacing(3);
    page_breadcrumb_ = new QLabel(ui_text(UiKey::k111, language_).toUpper(), header_text);
    page_breadcrumb_->setObjectName(QStringLiteral("pageBreadcrumb"));
    // Qt styles have no letter-spacing and no text-transform, so the tracking of the
    // mockup (10px, .08em, uppercase) is set on the font and the string here.
    QFont breadcrumb_font = navigation_font(10);
    breadcrumb_font.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
    page_breadcrumb_->setFont(breadcrumb_font);
    page_title_ = new QLabel(header_text);
    page_title_->setObjectName(QStringLiteral("pageTitle"));
    header_text_layout->addWidget(page_breadcrumb_);
    header_text_layout->addWidget(page_title_);
    header_layout->addWidget(header_text);
    header_layout->addStretch(1);
    page_version_ = new QLabel(page_header_);
    page_version_->setObjectName(QStringLiteral("pageVersion"));
    page_version_->setFont(navigation_font(10));
    // The About page takes its version from the window services; the header shows the
    // build version directly, so it is never empty (the mockup has it always).
    page_version_->setText(QStringLiteral("VoiceTyper %1").arg(QString::fromUtf8(domain::version())));
    header_layout->addWidget(page_version_, 0, Qt::AlignVCenter);
    layout->addWidget(page_header_);

    pages_ = new QStackedWidget(central);
    pages_->setObjectName(QStringLiteral("settingsPages"));
    pages_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    // A non-zero explicit minimum is what makes Qt ignore the stack's
    // minimumSizeHint (the tallest page); (0,0) counts as "not set".
    pages_->setMinimumSize(0, 1);
    body->addWidget(pages_, 1);
    layout->addLayout(body, 1);

    // --- footer (MainWindow.axaml:Row 2) ------------------------------------
    auto* footer = new QWidget(central);
    footer->setObjectName(QStringLiteral("footer"));
    // Fixed height and a Fixed vertical policy: the footer must never be the row
    // that a tall page squeezes out of the window, and the .NET Grid gives it an
    // `Auto` row exactly like this.
    footer->setFixedHeight(56);
    footer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* footer_layout = new QHBoxLayout(footer);
    // The right margin matches the content column (10 px) so the record button lines
    // up with the fields above it instead of sitting 10 px further left.
    footer_layout->setContentsMargins(24, 0, 24, 0);
    // index.css: .status-right gap 22px between the groups, 5px inside a group.
    footer_layout->setSpacing(22);
    status_ = new QLabel(footer);
    status_->setObjectName(QStringLiteral("footerStatus"));
    status_->setFont(navigation_font(12));
    record_button_ = new QPushButton(footer);
    record_button_->setObjectName(QStringLiteral("recordButton"));
    record_button_->setProperty("buttonRole", QStringLiteral("record"));
    record_button_->setIconSize(QSize(18, 18));
    connect(record_button_, &QPushButton::clicked, this, &MainWindow::on_recording_toggled);
    engine_state_ = new QLabel(footer);
    hotkey_state_ = new QLabel(footer);
    // index.css: .status-right > span > i - a green dot in front of the engine state.
    engine_dot_ = new QWidget(footer);
    engine_dot_->setObjectName(QStringLiteral("engineDot"));
    engine_dot_->setAttribute(Qt::WA_StyledBackground, true);
    engine_dot_->setFixedSize(6, 6);
    // index.css: .status-right span kbd - the combination is a row of key chips.
    hotkey_label_ = new QLabel(ui_text(UiKey::k181, language_), footer);
    hotkey_label_->setObjectName(QStringLiteral("hotkeyLabel"));
    hotkey_label_->setProperty("uiKey", static_cast<int>(UiKey::k181));
    hotkey_chips_ = new QWidget(footer);
    hotkey_chips_->setObjectName(QStringLiteral("hotkeyChips"));
    // A chip is a small box: without a fixed vertical policy the footer's 56 px stretch it.
    hotkey_chips_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto* chips_layout = new QHBoxLayout(hotkey_chips_);
    chips_layout->setContentsMargins(0, 0, 0, 0);
    chips_layout->setSpacing(3);
    hotkey_state_->hide();
    // The mockup's status strip starts with a state dot (index.css: .status-dot).
    status_dot_ = new QWidget(footer);
    status_dot_->setObjectName(QStringLiteral("statusDot"));
    status_dot_->setAttribute(Qt::WA_StyledBackground, true);
    status_dot_->setFixedSize(7, 7);
    // A light poll of the recording state machine keeps the button and the dot honest.
    record_button_timer_ = new QTimer(this);
    record_button_timer_->setInterval(400);
    connect(record_button_timer_, &QTimer::timeout, this, [this] {
        sync_record_button();
        sync_hotkey_display();
        sync_engine_dot();
    });
    record_button_timer_->start();
    // The dot belongs to its state text, not to the whole strip: the mockup keeps 5px
    // between an indicator and the words it marks, and 22px between the groups
    // (Alexander, 08.10.2026).
    auto* status_group = new QWidget(footer);
    auto* status_group_layout = new QHBoxLayout(status_group);
    status_group_layout->setContentsMargins(0, 0, 0, 0);
    status_group_layout->setSpacing(4);
    status_group_layout->addWidget(status_dot_, 0, Qt::AlignVCenter);
    // The prefix is its own quiet label: rich text in the status label made Qt create a
    // document for it, and the document's children were then built from the thread that
    // posted a status line - which is what crashed the window (Alexander, 08.10.2026).
    status_prefix_ = new QLabel(ui_text(UiKey::k12, language_), footer);
    status_prefix_->setObjectName(QStringLiteral("statusPrefix"));
    status_prefix_->setProperty("uiKey", static_cast<int>(UiKey::k12));
    status_group_layout->addWidget(status_prefix_);
    status_group_layout->addWidget(status_);
    // Without this the state label expands and its text drifts into the middle of the
    // window (Alexander, 08.10.2026).
    status_group_layout->addStretch(1);
    engine_group_ = new QWidget(footer);
    auto* engine_group_layout = new QHBoxLayout(engine_group_);
    engine_group_layout->setContentsMargins(0, 0, 0, 0);
    engine_group_layout->setSpacing(5);
    engine_group_layout->addWidget(engine_dot_, 0, Qt::AlignVCenter);
    engine_group_layout->addWidget(engine_state_);
    hotkey_group_ = new QWidget(footer);
    auto* hotkey_group_layout = new QHBoxLayout(hotkey_group_);
    hotkey_group_layout->setContentsMargins(0, 0, 0, 0);
    hotkey_group_layout->setSpacing(5);
    hotkey_group_layout->addWidget(hotkey_label_);
    hotkey_group_layout->addWidget(hotkey_chips_, 0, Qt::AlignVCenter);
    hotkey_group_layout->addWidget(hotkey_state_);
    footer_layout->addWidget(status_group, 1);
    footer_layout->addWidget(engine_group_);
    footer_layout->addWidget(hotkey_group_);
    footer_layout->addWidget(record_button_);
    // The footer goes into QMainWindow's status-bar row rather than a row of the
    // central layout: the status bar is laid out by QMainWindow itself, so no
    // page's minimum height can push it off the bottom of the window (which is
    // exactly what happened with a plain QVBoxLayout row).
    statusBar()->addWidget(footer, 1);
    statusBar()->setSizeGripEnabled(false);
    // The footer lives in the window's status bar, and the bar's own frame and margins were
    // the white line along the bottom edge (Alexander, 08.10.2026). The widget fills it.
    statusBar()->setContentsMargins(0, 0, 0, 0);
    statusBar()->show();

    auto* grip = new QSizeGrip(footer);
    grip->setObjectName(QStringLiteral("footerGrip"));
    footer_layout->addWidget(grip, 0, Qt::AlignBottom | Qt::AlignRight);

    // --- Общие -------------------------------------------------------------
    {
        auto* page = new QWidget(central);
        QVBoxLayout* rows = nullptr;
        auto* content = page_column(page, rows);
        page_columns_.push_back(content);
        app_language_ = new QComboBox(content);
        app_language_->setObjectName(QStringLiteral("appLanguageCombo"));
        app_language_->addItem(ui_text(UiKey::k30, language_), static_cast<int>(AppLanguage::ru));
        app_language_->addItem(QStringLiteral("English"), static_cast<int>(AppLanguage::en));
        recognition_language_ = new QComboBox(page);
        recognition_language_->addItem(ui_text(UiKey::k3, language_), static_cast<int>(RecognitionLanguage::automatic));
        recognition_language_->addItem(ui_text(UiKey::k30, language_), static_cast<int>(RecognitionLanguage::ru));
        recognition_language_->addItem(QStringLiteral("English"), static_cast<int>(RecognitionLanguage::en));
        recording_mode_ = new QComboBox(page);
        recording_mode_->addItem(QStringLiteral("Push-to-talk"), static_cast<int>(RecordingMode::push_to_talk));
        recording_mode_->addItem(QStringLiteral("Toggle"), static_cast<int>(RecordingMode::toggle));
        recording_mode_->addItem(QStringLiteral("VAD"), static_cast<int>(RecordingMode::vad));
        auto_paste_ = new ToggleSwitch(page);
        noise_reduction_ = new ToggleSwitch(page);
        noise_reduction_->setObjectName(QStringLiteral("noiseReductionToggle"));
        condition_on_previous_text_ = new ToggleSwitch(page);
        hide_on_focus_loss_ = new ToggleSwitch(page);
        hide_on_focus_loss_->setObjectName(QStringLiteral("hideOnFocusLossToggle"));
        silence_threshold_ = new QSpinBox(page);
        silence_threshold_->setObjectName(QStringLiteral("silenceThresholdSpin"));
        silence_threshold_->setRange(kSilenceThresholdMsMin, kSilenceThresholdMsMax);
        silence_threshold_->setSuffix(ui_text(UiKey::k0, language_));
        silence_threshold_->setToolTip(
            ui_text(UiKey::k4, language_));
        // Two cards and a hint strip, exactly as the mockup lays the page out.
        const auto language_group = settings_group(content, UiKey::k113, UiKey::k114, language_);
        language_group.rows->addWidget(setting_row(UiKey::k44, UiKey::k115, language_, app_language_));
        language_group.rows->addWidget(setting_row(UiKey::k45, UiKey::k116, language_, recognition_language_));
        language_group.rows->addWidget(setting_row(UiKey::k29, UiKey::k117, language_, recording_mode_));
        rows->addWidget(language_group.card);

        const auto behaviour_group = settings_group(content, UiKey::k118, UiKey::k119, language_);
        behaviour_group.rows->addWidget(setting_row(UiKey::k6, UiKey::k120, language_, auto_paste_, 0));
        behaviour_group.rows->addWidget(setting_row(UiKey::k40, UiKey::k121, language_, condition_on_previous_text_, 0));
        behaviour_group.rows->addWidget(setting_row(UiKey::k28, UiKey::k122, language_, silence_threshold_));
        rows->addWidget(behaviour_group.card);

        rows->addWidget(hint_bar(UiKey::k112, UiKey::k123, language_,
            QString::fromStdString(presenter_.settings().record_hotkey), content));
        rows->addStretch(1);
        add_page_to_nav(page, ui_text(UiKey::k23, language_));
    }

    // --- Модели ------------------------------------------------------------
    {
        auto* page = new QWidget(central);
        QVBoxLayout* rows = nullptr;
        auto* content = page_column(page, rows);
        page_columns_.push_back(content);
        engine_ = new QComboBox(content);
        engine_->setObjectName(QStringLiteral("engineCombo"));
        engine_->addItem(QStringLiteral("Whisper"), static_cast<int>(TranscriptionEngine::whisper));
        engine_->addItem(QStringLiteral("Parakeet"), static_cast<int>(TranscriptionEngine::parakeet));
        engine_->addItem(QStringLiteral("GigaAM"), static_cast<int>(TranscriptionEngine::gigaam));
        whisper_size_ = new QComboBox(content);
        whisper_size_->setObjectName(QStringLiteral("whisperSizeCombo"));
        for (const auto size : {ModelSize::tiny, ModelSize::base, ModelSize::small, ModelSize::medium, ModelSize::large}) {
            whisper_size_->addItem(model_size_text(size), static_cast<int>(size));
        }
        parakeet_size_ = new QComboBox(content);
        parakeet_size_->setObjectName(QStringLiteral("parakeetSizeCombo"));
        for (const auto size : {ParakeetModelSize::q4k, ParakeetModelSize::q5k, ParakeetModelSize::q6k, ParakeetModelSize::q8_0}) {
            parakeet_size_->addItem(parakeet_size_text(size), static_cast<int>(size));
        }
        gigaam_size_ = new QComboBox(content);
        gigaam_size_->setObjectName(QStringLiteral("gigaamSizeCombo"));
        for (const auto size : {GigaamModelSize::q4_k_m, GigaamModelSize::q5_k_m, GigaamModelSize::q6_k, GigaamModelSize::q8_0}) {
            gigaam_size_->addItem(gigaam_size_text(size), static_cast<int>(size));
        }
        temperature_ = new QDoubleSpinBox(content);
        temperature_->setRange(0.0, 1.0);
        temperature_->setSingleStep(0.05);
        best_of_ = new QSpinBox(content);
        best_of_->setObjectName(QStringLiteral("bestOfSpin"));
        // The engine's own bounds: the same 1..8 asr/engine_parameters.hpp accepts,
        // so the control cannot produce a value validate() would reject.
        best_of_->setRange(kBestOfMin, kBestOfMax);
        // The terms dictionary is the mockup's textarea now (.dictionary textarea: 82px
        // tall, the monospace face comes from #termsEdit in the stylesheet). It stays a
        // multi-line editor: the user has to see the terms at once (Alexander, 06.10.2026).
        terms_ = new QPlainTextEdit(content);
        terms_->setObjectName(QStringLiteral("termsEdit"));
        terms_->setFixedHeight(82);
        terms_->setTabChangesFocus(true);
        engine_->setMaximumWidth(220);
        // Temperature and the number of candidates are Whisper's own parameters: GigaAM and
        // Parakeet ignore them (asr/engine_parameters.cpp), so those rows appear only for
        // Whisper (Alexander, 08.10.2026). The slot tolerates being called before the rows
        // exist, because populating the combo already emits the signal.
        connect(engine_, &QComboBox::currentIndexChanged, this, [this] { update_engine_specific_rows(); });

        // The .NET page picks a model from a list that carries its size, speed,
        // quality and description, so "which model" is answerable without knowing
        // the product. The three size combos stay as hidden state holders, because
        // the presenter reads one control per engine.
        whisper_size_->hide();
        parakeet_size_->hide();
        gigaam_size_->hide();

        // «Движок и модели»: the engine picker, then the list of the models of the
        // active engine (index.css: .model-list / .model-item).
        const auto engine_group = settings_group(content, UiKey::k171, UiKey::k172, language_);
        engine_group.rows->addWidget(setting_row(UiKey::k7, UiKey::k173, language_, engine_));

        const auto build_model_card = [&](ModelList list) -> QWidget* {
            std::size_t count = 0;
            const ModelRow* specs = model_rows(list, count);
            const QString prefix = QString::fromLatin1(model_list_prefix(list));
            auto* card = new QWidget(content);
            card->setObjectName(prefix + QStringLiteral("ModelsCard"));
            card->setAttribute(Qt::WA_StyledBackground, true);
            // .model-list carries the hairline above its first row: Qt styles have no
            // :first-child selector, so that row drops its own border instead (firstRow).
            card->setProperty("modelList", true);
            auto* card_layout = new QVBoxLayout(card);
            card_layout->setContentsMargins(0, 0, 0, 0);
            card_layout->setSpacing(0);
            for (std::size_t index = 0; index < count; ++index) {
                const ModelRow& spec = specs[index];
                // .model-item: min-height 92px, the grid "switch | copy | action",
                // padding 13px 18px, a hairline above every row but the first.
                auto* row = new QWidget(card);
                row->setObjectName(QStringLiteral("modelItem"));
                row->setAttribute(Qt::WA_StyledBackground, true);
                row->setProperty("firstRow", index == 0);
                row->setProperty("active", false);
                row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
                auto* row_layout = new QHBoxLayout(row);
                row_layout->setContentsMargins(18, 13, 18, 13);
                row_layout->setSpacing(14);
                auto* toggle = new ToggleSwitch(row);
                toggle->setObjectName(prefix + QStringLiteral("ModelToggle%1").arg(index));
                row_layout->addWidget(toggle, 0, Qt::AlignVCenter);
                auto* text_box = new QWidget(row);
                text_box->setObjectName(QStringLiteral("modelCopy"));
                auto* text_layout = new QVBoxLayout(text_box);
                text_layout->setContentsMargins(0, 0, 0, 0);
                // .model-item strong / small / p: 4px between the three lines.
                text_layout->setSpacing(4);
                // The name and the badge share one line (.model-item strong + .model-badge).
                auto* name_line = new QWidget(text_box);
                name_line->setObjectName(QStringLiteral("modelNameLine"));
                auto* name_line_layout = new QHBoxLayout(name_line);
                name_line_layout->setContentsMargins(0, 0, 0, 0);
                // .model-badge { margin-left: 8px }: a layout gap is what a hidden badge
                // drops, which a QSS margin on a laid-out widget would not do.
                name_line_layout->setSpacing(8);
                auto* name = new QLabel(QString::fromUtf8(spec.name), name_line);
                name->setObjectName(QStringLiteral("modelName"));
                QFont name_font = name->font();
                // Bold, not DemiBold: a weight the family does not ship resolves to Regular.
                name_font.setWeight(QFont::Bold);
                name->setFont(name_font);
                auto* badge = new QLabel(ui_text(UiKey::k170, language_), name_line);
                badge->setObjectName(QStringLiteral("modelBadge"));
                badge->setProperty("uiKey", static_cast<int>(UiKey::k170));
                // .model-badge is 8px uppercase (text-transform); QSS cannot uppercase a
                // label, so the string is raised here and again in retranslate().
                badge->setProperty("uppercaseText", true);
                badge->setText(badge->text().toUpper());
                badge->hide();
                name_line_layout->addWidget(name, 0, Qt::AlignVCenter);
                name_line_layout->addWidget(badge, 0, Qt::AlignVCenter);
                name_line_layout->addStretch(1);
                auto* meta = new WrappingLabel(
                    ui_text(UiKey::k2, language_)
                        .arg(QString::fromUtf8(model_field(spec.size_ru, spec.size_en, language_)),
                            QString::fromUtf8(model_field(spec.speed_ru, spec.speed_en, language_)),
                            QString::fromUtf8(model_field(spec.quality_ru, spec.quality_en, language_))),
                    text_box);
                meta->setObjectName(QStringLiteral("modelMeta"));
                // Wrapped, so the row's minimum is its longest word: otherwise the
                // meta line alone forced the whole page wider than the viewport.
                meta->setWordWrap(true);
                auto* description = new WrappingLabel(
                    QString::fromUtf8(model_field(spec.description_ru, spec.description_en, language_)),
                    text_box);
                description->setObjectName(QStringLiteral("modelNote"));
                description->setWordWrap(true);
                text_layout->addWidget(name_line);
                text_layout->addWidget(meta);
                text_layout->addWidget(description);
                // Registered so a language change re-letters the card in place, like the
                // rest of the window.
                model_card_labels_.push_back(
                    ModelCardLabels{name, meta, description, badge, list, static_cast<int>(index)});
                row_layout->addWidget(text_box, 1);
                auto* download = new QPushButton(row);
                download->setProperty("buttonRole", QStringLiteral("download"));
                download->setIconSize(QSize(18, 18));
                // .model-action: a 34x34 square with a 6px radius.
                download->setFixedSize(34, 34);
                // The download indicator lives in the row and only appears while a transfer
                // is running, so the row keeps its shape otherwise.
                auto* transfer = new QProgressBar(row);
                transfer->setObjectName(prefix + QStringLiteral("ModelProgress%1").arg(index));
                transfer->setRange(0, 100);
                transfer->setValue(0);
                transfer->setFixedWidth(90);
                transfer->setTextVisible(false);
                transfer->hide();
                // The percentage sits to the right of the bar (Alexander, 08.10.2026): a
                // slim bar has no room for the text, so it simply disappeared.
                auto* percent = new QLabel(row);
                percent->setObjectName(transfer->objectName() + QStringLiteral("Percent"));
                percent->setProperty("progressPercent", true);
                percent->hide();
                progress_for(list).push_back(transfer);
                busy_for(list).push_back(false);
                download->setObjectName(prefix + QStringLiteral("ModelButton%1").arg(index));
                row_layout->addWidget(transfer, 0, Qt::AlignVCenter);
                row_layout->addWidget(percent, 0, Qt::AlignVCenter);
                row_layout->addWidget(download, 0, Qt::AlignVCenter);
                // The label depends on whether the model is on disk: a downloaded model
                // must offer "Удалить", not a pointless "Скачать" (reported from the
                // running build, about the models that were already working).
                // One handler for both states of the button: delete a model that is on disk,
                // download one that is not.
                connect(download, &QPushButton::clicked, this, [this, list, index] {
                    const auto& busy = busy_for(list);
                    if (static_cast<std::size_t>(index) < busy.size() && busy[static_cast<std::size_t>(index)]) {
                        // The same button cancels a transfer that is already running.
                        if (services_.model_download_cancel) {
                            services_.model_download_cancel(list, index);
                        }
                        return;
                    }
                    const bool downloaded = services_.model_is_downloaded
                        && services_.model_is_downloaded(list, static_cast<int>(index));
                    if (downloaded) {
                        if (services_.model_delete && services_.model_delete(list, static_cast<int>(index))) {
                            refresh_model_buttons();
                        }
                        return;
                    }
                    start_model_download(list, static_cast<int>(index));
                });
                buttons_for(list).push_back(download);
                connect(toggle, &QAbstractButton::clicked, this, [this, list, index] {
                    select_model(list, static_cast<int>(index));
                });
                toggles_for(list).push_back(toggle);
                card_layout->addWidget(row);
            }
            return card;
        };
        whisper_models_card_ = build_model_card(ModelList::whisper);
        parakeet_models_card_ = build_model_card(ModelList::parakeet);
        gigaam_models_card_ = build_model_card(ModelList::gigaam);
        engine_group.rows->addWidget(whisper_models_card_);
        engine_group.rows->addWidget(parakeet_models_card_);
        engine_group.rows->addWidget(gigaam_models_card_);
        rows->addWidget(engine_group.card);

        connect(engine_, &QComboBox::currentIndexChanged, this, [this](int) { refresh_model_list(); });
        connect(whisper_size_, &QComboBox::currentIndexChanged, this, [this](int) { refresh_model_list(); });
        connect(parakeet_size_, &QComboBox::currentIndexChanged, this, [this](int) { refresh_model_list(); });
        connect(gigaam_size_, &QComboBox::currentIndexChanged, this, [this](int) { refresh_model_list(); });

        // «Точность распознавания»: the two generation parameters and the dictionary.
        const auto accuracy_group = settings_group(content, UiKey::k174, UiKey::k175, language_);
        temperature_row_ = setting_row(UiKey::k36, UiKey::k176, language_, temperature_);
        accuracy_group.rows->addWidget(temperature_row_);
        // k56 is the same sentence the disabled control used to carry, now about the
        // live setting: how many candidates are compared, and that more is slower.
        best_of_->setToolTip(ui_text(UiKey::k56, language_));
        best_of_row_ = setting_row(UiKey::k55, UiKey::k177, language_, best_of_);
        accuracy_group.rows->addWidget(best_of_row_);

        // The dictionary block of the mockup (.dictionary): its title, one sentence about
        // the field, the textarea and the hint that explains how a term is understood.
        auto* dictionary = new QWidget(content);
        dictionary->setObjectName(QStringLiteral("dictionary"));
        dictionary->setAttribute(Qt::WA_StyledBackground, true);
        auto* dictionary_layout = new QVBoxLayout(dictionary);
        // .dictionary: padding 14px 18px 18px.
        dictionary_layout->setContentsMargins(18, 14, 18, 18);
        // .dictionary strong + span: 4px apart; the textarea follows 11px below.
        dictionary_layout->setSpacing(4);
        auto* dictionary_title = new QLabel(ui_text(UiKey::k33, language_), dictionary);
        dictionary_title->setObjectName(QStringLiteral("dictionaryTitle"));
        dictionary_title->setProperty("uiKey", static_cast<int>(UiKey::k33));
        auto* dictionary_caption = new QLabel(ui_text(UiKey::k178, language_), dictionary);
        dictionary_caption->setObjectName(QStringLiteral("dictionaryCaption"));
        dictionary_caption->setProperty("uiKey", static_cast<int>(UiKey::k178));
        dictionary_caption->setWordWrap(true);
        dictionary_layout->addWidget(dictionary_title);
        dictionary_layout->addWidget(dictionary_caption);
        dictionary_layout->addSpacing(7);
        dictionary_layout->addWidget(terms_);
        // How the field is understood, in one line. Without it the editor looks
        // like a plain prompt list, and "the term is in the dictionary but the text
        // still says комит" was exactly the reported confusion.
        auto* terms_hint = new QLabel(ui_text(UiKey::k110, language_), dictionary);
        terms_hint->setObjectName(QStringLiteral("dictionaryHint"));
        terms_hint->setWordWrap(true);
        terms_hint->setProperty("uiKey", static_cast<int>(UiKey::k110));
        dictionary_layout->addSpacing(7);
        dictionary_layout->addWidget(terms_hint);
        accuracy_group.rows->addWidget(dictionary);
        rows->addWidget(accuracy_group.card);

        update_engine_specific_rows();
        rows->addStretch(1);
        add_page_to_nav(page, ui_text(UiKey::k18, language_));
    }

    // --- Хоткеи ------------------------------------------------------------
    {
        auto* page = new QWidget(central);
        QVBoxLayout* rows = nullptr;
        auto* content = page_column(page, rows);
        page_columns_.push_back(content);
        record_hotkey_ = new QLineEdit(content);
        record_hotkey_->setObjectName(QStringLiteral("recordHotkeyEdit"));
        cancel_hotkey_ = new QLineEdit(content);
        cancel_hotkey_->setObjectName(QStringLiteral("cancelHotkeyEdit"));
        // The combinations are informational: they are set through "Изменить" only, so the
        // fields never take focus and never show a selected text (Alexander, 08.10.2026).
        for (QLineEdit* field : {record_hotkey_, cancel_hotkey_}) {
            field->setReadOnly(true);
            field->setFocusPolicy(Qt::NoFocus);
            field->setCursor(Qt::ArrowCursor);
        }
        // The capture button is the only workable way to set a combination: the
        // user presses it, then presses the keys. Disabled with a reason when the
        // platform offers no hook. The mockup labels it "Изменить" and swaps the
        // word for "Слушаю…" while it waits for the keys (index.css: .hotkey-control).
        record_hotkey_capture_ = new QPushButton(ui_text(UiKey::k134, language_), content);
        record_hotkey_capture_->setObjectName(QStringLiteral("recordHotkeyCapture"));
        record_hotkey_capture_->setProperty("uiKey", static_cast<int>(UiKey::k134));
        // No focus: the dashed focus rectangle of the style has no place in the mockup's
        // window and was showing up on the button that was clicked (Alexander, 08.10.2026).
        record_hotkey_capture_->setFocusPolicy(Qt::NoFocus);
        connect(record_hotkey_capture_, &QPushButton::clicked, this, [this] { capture_hotkey_into(true); });
        cancel_hotkey_capture_ = new QPushButton(ui_text(UiKey::k134, language_), content);
        cancel_hotkey_capture_->setObjectName(QStringLiteral("cancelHotkeyCapture"));
        cancel_hotkey_capture_->setProperty("uiKey", static_cast<int>(UiKey::k134));
        cancel_hotkey_capture_->setFocusPolicy(Qt::NoFocus);
        connect(cancel_hotkey_capture_, &QPushButton::clicked, this, [this] { capture_hotkey_into(false); });

        // The mockup's .hotkey-control: the combination is a kbd chip (still a
        // QLineEdit - the keys are typed into it) with the button beside it.
        const auto hotkey_control = [content](QLineEdit* field, QPushButton* button) {
            auto* box = new QWidget(content);
            box->setObjectName(QStringLiteral("hotkeyControl"));
            auto* layout = new QHBoxLayout(box);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(8);
            field->setMaximumWidth(240);
            // The mockup's kbd min-width is 146 px including its padding and border
            // (index.css sets box-sizing: border-box), so the minimum belongs on the
            // widget: a QSS min-width is a content width and would add 24 px more.
            field->setMinimumWidth(146);
            field->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            layout->addWidget(field, 0, Qt::AlignVCenter);
            layout->addWidget(button, 0, Qt::AlignVCenter);
            return box;
        };

        const auto hotkeys_group = settings_group(content, UiKey::k130, UiKey::k131, language_);
        hotkeys_group.rows->addWidget(setting_row(
            UiKey::k12, UiKey::k132, language_, hotkey_control(record_hotkey_, record_hotkey_capture_)));
        hotkeys_group.rows->addWidget(setting_row(
            UiKey::k26, UiKey::k133, language_, hotkey_control(cancel_hotkey_, cancel_hotkey_capture_)));
        rows->addWidget(hotkeys_group.card);

        const bool can_capture = static_cast<bool>(services_.capture_hotkey);
        for (auto* button : {record_hotkey_capture_, cancel_hotkey_capture_}) {
            button->setEnabled(can_capture);
            button->setToolTip(can_capture
                    ? ui_text(UiKey::k20, language_)
                    : ui_text(UiKey::k19, language_));
        }
        // The hint of this page carries no hotkey: hint_bar must not .arg() it.
        rows->addWidget(hint_bar(UiKey::k136, UiKey::k137, language_, QString(), content));
        rows->addStretch(1);
        add_page_to_nav(page, ui_text(UiKey::k42, language_));
    }

    // --- Микрофон ----------------------------------------------------------
    {
        auto* page = new QWidget(central);
        QVBoxLayout* rows = nullptr;
        auto* content = page_column(page, rows);
        // The centred 820 px column of the mockup's page-content, like every other
        // converted page.
        page_columns_.push_back(content);

        // «Источник звука»: the device and the input level (index.css: .range-row).
        const auto source_group = settings_group(content, UiKey::k143, UiKey::k144, language_);
        microphone_ = new QComboBox(content);
        microphone_->setObjectName(QStringLiteral("microphoneCombo"));
        // A combo box sizes itself to its longest item, and a microphone name here
        // is "Набор микрофонов (Технология Intel® Smart Sound для цифровых
        // микрофонов)" - the widget demanded 517 px, so the page grew wider than the
        // scroll viewport and its whole content was cut off (caught by the layout
        // test on Windows, at the minimum window size). Elide the text instead.
        microphone_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        microphone_->setMinimumContentsLength(12);
        source_group.rows->addWidget(setting_row(UiKey::k39, UiKey::k145, language_, microphone_));

        // Sensitivity: the very value the Windows Sound panel edits, so the slider and the
        // system never disagree. Disabled (with the reason) where the platform has no level
        // control, because a slider that writes nowhere is worse than no slider.
        microphone_level_ = new LevelSlider(content);
        microphone_level_->setObjectName(QStringLiteral("microphoneLevelSlider"));
        microphone_level_->setRange(0, 100);
        microphone_level_->setMinimumWidth(160);
        microphone_level_value_ = new QLabel(content);
        microphone_level_value_->setObjectName(QStringLiteral("microphoneLevelValue"));
        microphone_level_value_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        source_group.rows->addWidget(range_row(
            UiKey::k57, UiKey::k146, language_, microphone_level_, microphone_level_value_));

        // The test the .NET build did not have: it told the user to check the level and
        // left them to the control panel. It is the mockup's .microphone-test block now,
        // state line, button and level meter together.
        microphone_test_ = new QPushButton(ui_text(UiKey::k149, language_), content);
        microphone_test_->setObjectName(QStringLiteral("microphoneTestButton"));
        microphone_test_->setProperty("uiKey", static_cast<int>(UiKey::k149));
        source_group.rows->addWidget(microphone_test_block(language_, content, microphone_test_,
            microphone_test_state_, microphone_test_result_, microphone_level_meter_));
        rows->addWidget(source_group.card);

        // Noise reduction belongs with the microphone: it describes how the input is
        // recorded, not how the engine transcribes (Alexander, 2026-10-06).
        const auto processing_group = settings_group(content, UiKey::k150, UiKey::k151, language_);
        processing_group.rows->addWidget(setting_row(
            UiKey::k43, UiKey::k152, language_, noise_reduction_, 0));
        rows->addWidget(processing_group.card);

        rows->addStretch(1);
        add_page_to_nav(page, ui_text(UiKey::k17, language_));
    }

    // --- Запуск ------------------------------------------------------------
    {
        auto* page = new QWidget(central);
        QVBoxLayout* rows = nullptr;
        auto* content = page_column(page, rows);
        page_columns_.push_back(content);
        start_with_windows_ = new ToggleSwitch(content);
        start_minimized_ = new ToggleSwitch(content);
        // The card of the mockup, then the "runs in the background" banner.
        const auto autostart_group = settings_group(content, UiKey::k124, UiKey::k125, language_);
        autostart_group.rows->addWidget(setting_row(UiKey::k14, UiKey::k126, language_, start_with_windows_, 0));
        autostart_group.rows->addWidget(setting_row(UiKey::k15, UiKey::k127, language_, start_minimized_, 0));
        rows->addWidget(autostart_group.card);
        rows->addWidget(launch_card(language_, content, launch_icon_));
        rows->addStretch(1);
        add_page_to_nav(page, ui_text(UiKey::k13, language_));
    }

    // --- Внешний вид -------------------------------------------------------
    {
        auto* page = new QWidget(central);
        QVBoxLayout* rows = nullptr;
        auto* content = page_column(page, rows);
        page_columns_.push_back(content);

        // «Тема приложения»: the three preview tiles of the mockup
        // (index.css: .theme-options) in its order - system, light, dark
        // (App.tsx:123-138) - instead of the combo box the page used to carry.
        const auto theme_group = settings_group(content, UiKey::k161, UiKey::k162, language_);
        auto* options = new QWidget(content);
        options->setObjectName(QStringLiteral("themeOptions"));
        options->setAttribute(Qt::WA_StyledBackground, true);
        auto* options_layout = new QGridLayout(options);
        // .theme-options: padding 18px, gap 14px, three equal columns.
        options_layout->setContentsMargins(18, 18, 18, 18);
        options_layout->setHorizontalSpacing(14);
        options_layout->setVerticalSpacing(14);
        const std::array<AppTheme, 3> tile_themes{AppTheme::system, AppTheme::light, AppTheme::dark};
        for (std::size_t index = 0; index < tile_themes.size(); ++index) {
            auto* tile = new ThemeTile(tile_themes[index], language_, options);
            // The click walks the very path the combo box used: the presenter stores the
            // theme, then apply_theme() re-dresses the window and re-marks the tiles.
            tile->on_click = [this](AppTheme chosen) {
                presenter_.update(SettingsChange::appearance, [chosen](domain::AppSettings& settings) {
                    settings.theme = chosen;
                });
                apply_theme();
            };
            theme_tiles_.push_back(tile);
            options_layout->addWidget(tile, 0, static_cast<int>(index));
            options_layout->setColumnStretch(static_cast<int>(index), 1);
        }
        theme_group.rows->addWidget(options);
        rows->addWidget(theme_group.card);

        // «Интерфейс»: the one switch the mockup keeps on this page. The "Компактный
        // режим" and "Анимации интерфейса" rows of the audit are gone from the reference.
        const auto interface_group = settings_group(content, UiKey::k166, UiKey::k167, language_);
        interface_group.rows->addWidget(
            setting_row(UiKey::k32, UiKey::k168, language_, hide_on_focus_loss_, 0));
        rows->addWidget(interface_group.card);

        // The mockup's .reset-button, under the card: it puts the appearance back to the
        // shipped defaults through the same presenter path as every other edit.
        appearance_reset_ = new QPushButton(ui_text(UiKey::k169, language_), content);
        appearance_reset_->setObjectName(QStringLiteral("appearanceResetButton"));
        appearance_reset_->setProperty("uiKey", static_cast<int>(UiKey::k169));
        appearance_reset_->setCursor(Qt::PointingHandCursor);
        connect(appearance_reset_, &QPushButton::clicked, this, [this] {
            const domain::AppSettings defaults = domain::AppSettings::defaults();
            presenter_.update(SettingsChange::appearance, [defaults](domain::AppSettings& settings) {
                settings.theme = defaults.theme;
            });
            {
                // The switch is set by hand as well: a programmatic reset is not a click
                // of the user, and the value reaches the presenter exactly once below.
                const QSignalBlocker blocker(hide_on_focus_loss_);
                hide_on_focus_loss_->setChecked(defaults.hide_on_focus_loss);
            }
            presenter_.update(SettingsChange::behaviour, [defaults](domain::AppSettings& settings) {
                settings.hide_on_focus_loss = defaults.hide_on_focus_loss;
            });
            apply_theme();
        });
        rows->addWidget(appearance_reset_, 0, Qt::AlignLeft);

        rows->addStretch(1);
        add_page_to_nav(page, ui_text(UiKey::k5, language_));
    }

    // --- Журнал ------------------------------------------------------------
    {
        auto* page = new QWidget(central);
        QVBoxLayout* rows = nullptr;
        auto* content = page_column(page, rows);
        page_columns_.push_back(content);

        log_view_ = new QTextEdit(content);
        log_view_->setObjectName(QStringLiteral("logView"));
        log_view_->setReadOnly(true);
        // The log is the one thing on its page that should be tall: the trailing
        // stretch that keeps the other pages top-aligned would otherwise squeeze it
        // into a couple of lines.
        log_view_->setMinimumHeight(320);
        // The empty state of the mockup (index.css: .log-empty). It shares a stack
        // with the view, so clearing the log does not make the card jump.
        log_empty_ = new QLabel(ui_text(UiKey::k142, language_), content);
        log_empty_->setObjectName(QStringLiteral("logEmpty"));
        log_empty_->setProperty("uiKey", static_cast<int>(UiKey::k142));
        log_empty_->setAlignment(Qt::AlignCenter);
        log_empty_->setMinimumHeight(320);
        log_stack_ = new QStackedWidget(content);
        log_stack_->setObjectName(QStringLiteral("logStack"));
        log_stack_->addWidget(log_view_);
        log_stack_->addWidget(log_empty_);
        log_stack_->setCurrentWidget(log_view_);

        // The heading carries the two actions on the right (index.css: .log-heading).
        auto* actions = new QWidget(content);
        actions->setObjectName(QStringLiteral("logActions"));
        auto* actions_layout = new QHBoxLayout(actions);
        actions_layout->setContentsMargins(0, 0, 0, 0);
        actions_layout->setSpacing(7);
        log_copy_ = new QPushButton(ui_text(UiKey::k140, language_), actions);
        log_copy_->setObjectName(QStringLiteral("logCopyButton"));
        log_copy_->setProperty("uiKey", static_cast<int>(UiKey::k140));
        log_clear_ = new QPushButton(ui_text(UiKey::k141, language_), actions);
        log_clear_->setObjectName(QStringLiteral("logClearButton"));
        log_clear_->setProperty("uiKey", static_cast<int>(UiKey::k141));
        actions_layout->addWidget(log_copy_);
        actions_layout->addWidget(log_clear_);

        const auto journal_group = settings_group(
            content, UiKey::k138, UiKey::k139, language_, actions);
        journal_group.rows->addWidget(log_stack_, 1);
        // index.css: .log-group min-height 430px; the view flexes inside it. The card
        // keeps that height (the mockup does not stretch it to the page either).
        journal_group.card->setMinimumHeight(430);
        rows->addWidget(journal_group.card);

        // Copy puts the very lines the services report on the clipboard - what is on
        // screen, not a re-rendered version of it.
        connect(log_copy_, &QPushButton::clicked, this, [this] {
            // What is on screen, not a re-rendered version of it: after a clear that is
            // only the lines that arrived afterwards.
            const QString text = log_cleared_ ? log_shown_text_
                : (services_.log_text ? services_.log_text() : log_view_->toPlainText());
            QGuiApplication::clipboard()->setText(text);
        });
        // Clearing empties the view only: the log file itself is the running
        // services' business (Alexander, 08.10.2026). "Журнал очищен" stays until
        // the services really produce a new line.
        connect(log_clear_, &QPushButton::clicked, this, [this] {
            // The text on screen at this moment is the baseline: everything up to it stays
            // hidden, and the view shows only the lines that arrive afterwards.
            log_cleared_ = true;
            log_cleared_at_ = services_.log_text ? services_.log_text() : log_view_->toPlainText();
            log_shown_text_.clear();
            render_log_lines(log_view_, QString());
            log_stack_->setCurrentWidget(log_empty_);
        });

        rows->addStretch(1);
        add_page_to_nav(page, ui_text(UiKey::k9, language_));
    }

    // --- О программе -------------------------------------------------------
    {
        auto* page = new QWidget(central);
        QVBoxLayout* rows = nullptr;
        auto* content = page_column(page, rows);
        page_columns_.push_back(content);

        // The hero of the mockup (index.css: .about-hero): the brand wave, the product
        // name with its tagline, and the running version in a pill on the right.
        rows->addWidget(about_hero(language_, content, about_hero_logo_, update_version_));

        const auto about_group = settings_group(content, UiKey::k154, UiKey::k155, language_);
        about_group.rows->addWidget(about_copy(language_, content, about_privacy_icon_));

        // The update controls keep the whole flow of the .NET build (check, notes,
        // installer, progress); only their frame changed: they are rows of this card now,
        // and the action is the mockup's filled primary button.
        update_status_ = new QLabel(ui_text(UiKey::k73, language_), content);
        update_status_->setObjectName(QStringLiteral("updateStatus"));
        update_status_->setWordWrap(true);
        update_check_ = new QPushButton(ui_text(UiKey::k64, language_), content);
        update_check_->setObjectName(QStringLiteral("updateCheckButton"));
        update_check_->setProperty("uiKey", static_cast<int>(UiKey::k64));
        update_check_->setProperty("buttonRole", QStringLiteral("primary"));
        about_group.rows->addWidget(
            status_row(update_status_, UiKey::k160, language_, update_check_));

        // The notes and the progress bar are states of the flow, not rows of the mockup:
        // they keep their own widget and only take the card's insets.
        // WrappingLabel: it answers with the height its text needs at the current width, so
        // the block no longer has to be given a height by hand.
        update_notes_ = new WrappingLabel(content);
        update_notes_->setObjectName(QStringLiteral("updateNotes"));
        update_notes_->setWordWrap(true);
        update_notes_->hide();
        about_group.rows->addWidget(
            inset_block(content, update_notes_, QMargins(18, 12, 18, 12)));
        set_inset_visible(update_notes_, false);

        update_install_ = new QPushButton(ui_text(UiKey::k68, language_), content);
        update_install_->setObjectName(QStringLiteral("updateInstallButton"));
        update_install_->setProperty("uiKey", static_cast<int>(UiKey::k68));
        update_install_->hide();
        // The whole row is hidden until there is something to install: hiding only the
        // button left an empty "Обновление" card, which the mockup does not have.
        update_row_ = setting_row(UiKey::k76, language_, update_install_);
        update_row_->hide();
        about_group.rows->addWidget(update_row_);

        update_progress_ = new QProgressBar(content);
        update_progress_->setObjectName(QStringLiteral("updateProgress"));
        update_progress_->setRange(0, 100);
        update_progress_->hide();
        about_group.rows->addWidget(
            inset_block(content, update_progress_, QMargins(18, 12, 18, 12)));
        set_inset_visible(update_progress_, false);
        rows->addWidget(about_group.card);

        // The three links sit under the card, outside it, as index.css: .about-links does.
        rows->addWidget(about_links(language_, content));

        rows->addStretch(1);
        add_page_to_nav(page, ui_text(UiKey::k22, language_));
    }

    // The .NET order (SettingsViewModel.cs:307-316) wins over build order: the
    // navigation list is what the user sees, and a page may be constructed in any
    // order as long as it lands under the right entry.
    finalize_pages();
    nav_->setCurrentRow(0);
    pages_->setCurrentIndex(0);

    // Open a given page straight away, so every page can be looked at (and
    // captured) without clicking the sidebar: VOICETYPER_START_PAGE=Models.
    if (const char* start_page = std::getenv("VOICETYPER_START_PAGE");
        start_page != nullptr && *start_page != '\0') {
        const QString requested = QString::fromUtf8(start_page);
        for (int index = 0; index < nav_->count(); ++index) {
            if (nav_->item(index)->data(Qt::UserRole + 2).toString().compare(requested, Qt::CaseInsensitive) == 0) {
                nav_->setCurrentRow(index);
                pages_->setCurrentIndex(index);
                break;
            }
        }
    }
    connect(nav_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && row < pages_->count()) {
            pages_->setCurrentIndex(row);
            refresh_page_title(row);
            position_navigation_accent();
        }
    });
    connect(pages_, &QStackedWidget::currentChanged, this, [this](int index) {
        refresh_page_title(index);
    });
    refresh_page_title(nav_->currentRow());
    setCentralWidget(central);
    // The rows are only laid out once the widget is shown, so the bar is placed again
    // on the first event-loop turn, and then by resizeEvent and every row change.
    QTimer::singleShot(0, this, [this] {
        position_navigation_accent();
        layout_page_columns(pages_ != nullptr ? pages_->width() - 72 : 0);
    });
}

void MainWindow::position_navigation_accent()
{
    if (nav_ == nullptr || nav_accent_ == nullptr) {
        return;
    }
    const int row = nav_->currentRow();
    if (row < 0 || row >= nav_->count()) {
        nav_accent_->hide();
        return;
    }
    const QRect rect = nav_->visualItemRect(nav_->item(row));
    if (!rect.isValid()) {
        nav_accent_->hide();
        return;
    }
    nav_accent_->move(rect.left(), rect.top() + (rect.height() - nav_accent_->height()) / 2);
    nav_accent_->show();
    nav_accent_->raise();
}

void MainWindow::add_page_to_nav(QWidget* page, const QString& label)
{
    built_pages_.emplace_back(label, page);
}

void MainWindow::finalize_pages()
{
    // Each registered page is wrapped and added exactly once, in kNavigation
    // order, so navigation row N and stack index N are the same page. The first
    // version had a fallback loop that re-wrapped pages it had already added:
    // setWidget() reparents the page out of its first scroll area, which left
    // empty pages in the stack and content showing up under the wrong entry.
    for (const auto& entry : kNavigation) {
        const QString label = QString::fromUtf8(entry.label);
        const auto found = std::find_if(built_pages_.begin(), built_pages_.end(),
            [&entry, this](const auto& pair) { return page_matches_entry(pair.first, entry, language_); });
        if (found == built_pages_.end()) {
            continue;
        }
        static_cast<void>(label);
        QWidget* page = found->second;
        // The page itself is transparent: the raised colour comes from the scroll area
        // and its viewport, so the cards sit on it (index.css: .main-scroll).
        page->setObjectName(QStringLiteral("pageBody"));

        auto* scroll = new QScrollArea(pages_);
        scroll->setObjectName(QStringLiteral("pageScroll_") + QString::fromUtf8(entry.key));
        scroll->viewport()->installEventFilter(this);
        page_viewports_.insert(scroll->viewport());
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        // The content row is `*` in the .NET Grid: the scroll area must be able
        // to shrink below its content's minimum height, otherwise the page stack
        // makes the 640 px window taller and the footer falls off the bottom.
        scroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
        scroll->setMinimumSize(0, 1);
        page->setMinimumWidth(0);
        page->setObjectName(QStringLiteral("page_") + QString::fromUtf8(entry.key));
        // Rows sit directly on the page background with a hairline between them, as
        // the reference does - no card, no border, no shadow.
        if (auto* page_layout = qobject_cast<QBoxLayout*>(page->layout()); page_layout != nullptr) {
            page_layout->setContentsMargins(15, 8, 15, 8);
            page_layout->addStretch(1);
        }
        auto* outer = new QWidget();
        outer->setObjectName(QStringLiteral("pageSurface"));
        auto* outer_layout = new QVBoxLayout(outer);
        outer_layout->setContentsMargins(0, 0, 0, 0);
        outer_layout->setSpacing(0);
        outer_layout->addWidget(page, 1);
        scroll->setWidget(outer);
        scroll->setProperty("pageWidget", QVariant::fromValue(static_cast<QObject*>(page)));
        // A trailing stretch: without it the page layout spreads its rows over the whole
        // height, which is what made the settings look scattered (Alexander, 06.10.2026).
        if (auto* box = qobject_cast<QVBoxLayout*>(page->layout()); box != nullptr) {
            box->addStretch(1);
        }
        pages_->addWidget(scroll);

        // The item shows the label in the interface language; the Russian label stays
        // in UserRole as the stable identifier the page lookup and the tests use.
        auto* item = new QListWidgetItem(ui_text(entry.text, language_), nav_);
        item->setData(Qt::UserRole, label);
        item->setData(Qt::UserRole + 1, QString::fromUtf8(entry.glyph));
        // The .NET page key (Main, Appearance, Models, ...): it is what a screenshot
        // run and the reviewers name, independently of the Russian label.
        item->setData(Qt::UserRole + 2, QString::fromUtf8(entry.key));
        // Drawn right here as well: the theme is applied before this loop runs, and the
        // icon must exist even if nothing ever refreshes it again.
        const QColor nav_icon_colour(muted_text_.isEmpty() ? QStringLiteral("#E6E6E6") : muted_text_);
        item->setIcon(drawn_icon(navigation_drawn_icon(entry.key), kNavigationIconSizePx,
            nav_icon_colour));
        // The mockup's navigation row is 43 px tall whatever the font metrics say
        // (index.css: nav button height 43px).
        item->setSizeHint(QSize(0, 43));
    }
    // The theme may have been applied before the menu existed, so the icons are drawn
    // here as well as on every theme change.
    refresh_navigation_icons();

    // A page that is not in the navigation table would otherwise be unreachable.
    for (const auto& pair : built_pages_) {
        const bool known = std::any_of(kNavigation, kNavigation + std::size(kNavigation),
            [&pair, this](const NavEntry& entry) { return page_matches_entry(pair.first, entry, language_); });
        if (known) {
            continue; // already added above, exactly once
        }
        auto* scroll = new QScrollArea(pages_);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
        scroll->setMinimumSize(0, 1);
        scroll->setWidget(pair.second);
        pages_->addWidget(scroll);
        // This pass adds entries whose label is the one the page registered - which is the
        // localised label, so in English it never matched kNavigation: the menu was built
        // here, and addItem(text) creates an item without an icon and without the entry
        // data. The entry is found by the same localised label and the item is created with
        // its icon and its data, exactly like the first pass.
        const QColor colour(muted_text_.isEmpty() ? QStringLiteral("#E6E6E6") : muted_text_);
        DrawnIcon drawn = DrawnIcon::about;
        std::string_view entry_key = "About";
        QString entry_glyph;
        for (const auto& entry : kNavigation) {
            if (page_matches_entry(pair.first, entry, language_)) {
                drawn = navigation_drawn_icon(entry.key);
                entry_key = entry.key;
                entry_glyph = QString::fromUtf8(entry.glyph);
                break;
            }
        }
        auto* added = new QListWidgetItem(drawn_icon(drawn, kNavigationIconSizePx, colour),
            pair.first, nav_);
        added->setData(Qt::UserRole + 1, entry_glyph);
        added->setData(Qt::UserRole + 2, QString::fromUtf8(entry_key.data()));
        added->setSizeHint(QSize(0, 43));
    }
    // Everything is in the menu now: dress the icons once, for every pass.
    refresh_navigation_icons();
}

QWidget* MainWindow::page_scroll(int index) const
{
    if (pages_ == nullptr || index < 0 || index >= pages_->count()) {
        return nullptr;
    }
    return pages_->widget(index);
}

int MainWindow::page_count() const
{
    return nav_ == nullptr ? 0 : nav_->count();
}

QWidget* MainWindow::page_widget(int index) const
{
    if (pages_ == nullptr || index < 0 || index >= pages_->count()) {
        return nullptr;
    }
    QWidget* scroll = pages_->widget(index);
    if (scroll == nullptr) {
        return nullptr;
    }
    // The scroll area holds a wrapper (section title + card) whose card holds the
    // page, so the page is what a caller means by "the page": controls and tests
    // both look for it.
    const QVariant stored = scroll->property("pageWidget");
    if (stored.canConvert<QObject*>()) {
        if (auto* page = qobject_cast<QWidget*>(stored.value<QObject*>())) {
            return page;
        }
    }
    return scroll;
}

void MainWindow::show_page(int index)
{
    if (nav_ == nullptr || index < 0 || index >= nav_->count()) {
        return;
    }
    nav_->setCurrentRow(index);
    pages_->setCurrentIndex(index);
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    if (auto* sidebar = findChild<QWidget*>(QStringLiteral("sidebar")); sidebar != nullptr) {
        // ~30% like the reference, clamped so a phone-width window still shows the
        // settings and a wide one does not waste a third of the screen on navigation.
        sidebar->setFixedWidth(qBound(180, width() * 30 / 100, 300));
    }
    layout_page_columns(pages_ != nullptr ? pages_->width() - 72 : 0);
    position_navigation_accent();
}

void MainWindow::layout_page_columns(int viewport_width)
{
    if (page_columns_.empty()) {
        return;
    }
    // The mockup centres an 820 px column (index.css: .page-content max-width 820px).
    // In a QHBoxLayout the two stretches would otherwise take every spare pixel and
    // leave the column at its size hint, so the width is computed here: min(820, what
    // the viewport has), which is what keeps a narrow window free of a horizontal
    // scrollbar as well.
    if (viewport_width <= 0) {
        return;
    }
    const int column = qBound(240, viewport_width - 60, 820);
    for (auto* widget : page_columns_) {
        widget->setFixedWidth(column);
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    // The scroll area's viewport, not the stack, is what actually knows how much room
    // a page has: reading the stack in resizeEvent happened before the layout had run
    // and produced a 240 px column, which wrapped and clipped the row titles (caught
    // by ui-settings-test: "a wrapped label needs 28 px but has 22").
    if (event->type() == QEvent::Resize && page_viewports_.contains(watched)) {
        // The viewport that resized may belong to a hidden page (the stack gives those
        // a few dozen pixels), so the width comes from the stack, which is the same for
        // every page and is what the column has to fit into.
        layout_page_columns(pages_ != nullptr ? pages_->width() - 72 : 0);
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::mousePressEvent(QMouseEvent* event)
{
    // Only the title bar drags the frameless window; everywhere else the event
    // belongs to the controls.
    const QPoint position = event->position().toPoint();
    const QWidget* title_bar = findChild<QWidget*>(QStringLiteral("titleBar"));
    if (title_bar != nullptr && title_bar->geometry().contains(position)) {
        dragging_ = true;
        drag_offset_ = event->globalPosition().toPoint() - frameGeometry().topLeft();
        event->accept();
        return;
    }
    QMainWindow::mousePressEvent(event);
}

void MainWindow::mouseMoveEvent(QMouseEvent* event)
{
    if (dragging_) {
        move(event->globalPosition().toPoint() - drag_offset_);
        event->accept();
        return;
    }
    QMainWindow::mouseMoveEvent(event);
}

void MainWindow::mouseReleaseEvent(QMouseEvent* event)
{
    dragging_ = false;
    QMainWindow::mouseReleaseEvent(event);
}

void MainWindow::set_status_message(const QString& message)
{
    status_message_ = message;
    refresh_status();
}

void MainWindow::sync_record_button()
{
    if (record_button_ == nullptr) {
        return;
    }
    // The state machine is the truth about a session: it can end by itself (silence, the
    // hotkey, a failure). Reading it on a timer is what finally made the button return to
    // "Записать" - the window had no periodic tick, so a state that arrived without a user
    // action never re-drew the button (reported twice from the running build).
    if (services_.recording_active) {
        recording_ = services_.recording_active();
    }
    const bool can_record = services_.start_recording != nullptr;
    record_button_->setEnabled(can_record);
    record_button_->setText(recording_ ? ui_text(UiKey::k25, language_) : ui_text(UiKey::k10, language_));
    // The mockup puts a microphone on the button and a stop glyph while it records.
    record_button_->setIconSize(QSize(16, 16));
    record_button_->setIcon(drawn_icon(recording_ ? DrawnIcon::stop : DrawnIcon::microphone, 16,
        QColor(QStringLiteral("#FFFFFF"))));
    record_button_->setToolTip(can_record
            ? (recording_ ? ui_text(UiKey::k25, language_) : ui_text(UiKey::k10, language_))
            : ui_text(UiKey::k8, language_));
    record_button_->setProperty("recording", recording_);
    record_button_->style()->unpolish(record_button_);
    record_button_->style()->polish(record_button_);
    if (status_dot_ != nullptr) {
        status_dot_->setProperty("recording", recording_);
        status_dot_->style()->unpolish(status_dot_);
        status_dot_->style()->polish(status_dot_);
    }
}

void MainWindow::sync_engine_dot()
{
    if (engine_dot_ == nullptr) {
        return;
    }
    // The service answers from the engine's own readiness; without it (a host with no
    // engine at all) the dot stays red, because nothing can transcribe there.
    const bool ready = services_.engine_ready && services_.engine_ready();
    if (engine_dot_->property("ready").toBool() == ready) {
        return;
    }
    engine_dot_->setProperty("ready", ready);
    engine_dot_->style()->unpolish(engine_dot_);
    engine_dot_->style()->polish(engine_dot_);
}

void MainWindow::sync_hotkey_display()
{
    if (hotkey_chips_ == nullptr || hotkey_label_ == nullptr) {
        return;
    }
    const QString combination = QString::fromStdString(presenter_.settings().record_hotkey);
    const QString service = hotkey_state_ != nullptr ? hotkey_state_->text() : QString();
    // The service line also carries "the hook could not register it": that must keep the
    // floor instead of chips that would look as if everything were fine.
    const bool problem = !service.isEmpty()
        && (combination.isEmpty() || !service.contains(combination, Qt::CaseInsensitive));
    hotkey_label_->setVisible(!problem);
    hotkey_chips_->setVisible(!problem);
    hotkey_state_->setVisible(problem);
    if (problem || combination == hotkey_chips_source_) {
        return;
    }
    hotkey_chips_source_ = combination;
    auto* layout = static_cast<QBoxLayout*>(hotkey_chips_->layout());
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }
    const QStringList keys = combination.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    for (int index = 0; index < keys.size(); ++index) {
        if (index > 0) {
            auto* plus = new QLabel(QStringLiteral("+"), hotkey_chips_);
            plus->setObjectName(QStringLiteral("hotkeyPlus"));
            plus->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            layout->addWidget(plus, 0, Qt::AlignVCenter);
        }
        auto* chip = new QLabel(keys.at(index).trimmed(), hotkey_chips_);
        chip->setObjectName(QStringLiteral("hotkeyChip"));
        chip->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        layout->addWidget(chip, 0, Qt::AlignVCenter);
    }
}

void MainWindow::update_engine_specific_rows()
{
    const bool whisper = engine_ != nullptr
        && engine_->currentData().toInt() == static_cast<int>(TranscriptionEngine::whisper);
    if (temperature_row_ != nullptr) {
        temperature_row_->setVisible(whisper);
    }
    if (best_of_row_ != nullptr) {
        best_of_row_->setVisible(whisper);
    }
}

void MainWindow::capture_hotkey_into(bool record)
{
    if (!services_.capture_hotkey) {
        return;
    }
    QLineEdit* field = record ? record_hotkey_ : cancel_hotkey_;
    QPushButton* button = record ? record_hotkey_capture_ : cancel_hotkey_capture_;
    if (field == nullptr || button == nullptr) {
        return;
    }

    button->setEnabled(false);
    // The mockup swaps the word while the window waits for the keys.
    button->setText(ui_text(UiKey::k135, language_));
    field->setReadOnly(true);
    field->deselect();
    const QString previous_placeholder = field->placeholderText();
    field->setPlaceholderText(ui_text(UiKey::k21, language_));
    set_status_message(ui_text(UiKey::k24, language_));

    services_.capture_hotkey([this, field, button, previous_placeholder](
                                 std::optional<std::string> gesture, QString error) {
        button->setEnabled(true);
        button->setText(ui_text(UiKey::k134, language_));
        // Read-only stays: the field is a readout, and the capture path writes it
        // programmatically (textChanged still fires, so saving keeps working).
        field->setPlaceholderText(previous_placeholder);
        if (!gesture.has_value()) {
            // Cancelled (Escape) is not an error and must not leave a message.
            set_status_message(error.isEmpty() ? QString() : error);
            return;
        }
        // Assigning fires textChanged, which goes through the presenter and the
        // autosave tick, so the combination is stored and re-registered.
        field->setText(QString::fromStdString(*gesture));
        field->deselect();
        set_status_message(ui_text(UiKey::k16, language_).arg(QString::fromStdString(*gesture)));
    });
}

std::vector<ToggleSwitch*>& MainWindow::toggles_for(ModelList list) noexcept
{
    switch (list) {
    case ModelList::whisper: return whisper_model_toggles_;
    case ModelList::parakeet: return parakeet_model_toggles_;
    case ModelList::gigaam: return gigaam_model_toggles_;
    }
    return whisper_model_toggles_;
}

std::vector<QPushButton*>& MainWindow::buttons_for(ModelList list) noexcept
{
    switch (list) {
    case ModelList::whisper: return whisper_model_buttons_;
    case ModelList::parakeet: return parakeet_model_buttons_;
    case ModelList::gigaam: return gigaam_model_buttons_;
    }
    return whisper_model_buttons_;
}

std::vector<QProgressBar*>& MainWindow::progress_for(ModelList list) noexcept
{
    switch (list) {
    case ModelList::whisper: return whisper_model_progress_;
    case ModelList::parakeet: return parakeet_model_progress_;
    case ModelList::gigaam: return gigaam_model_progress_;
    }
    return whisper_model_progress_;
}

std::vector<bool>& MainWindow::busy_for(ModelList list) noexcept
{
    switch (list) {
    case ModelList::whisper: return whisper_model_busy_;
    case ModelList::parakeet: return parakeet_model_busy_;
    case ModelList::gigaam: return gigaam_model_busy_;
    }
    return whisper_model_busy_;
}

QComboBox* MainWindow::size_combo_for(ModelList list) const noexcept
{
    switch (list) {
    case ModelList::whisper: return whisper_size_;
    case ModelList::parakeet: return parakeet_size_;
    case ModelList::gigaam: return gigaam_size_;
    }
    return nullptr;
}

QWidget* MainWindow::card_for(ModelList list) const noexcept
{
    switch (list) {
    case ModelList::whisper: return whisper_models_card_;
    case ModelList::parakeet: return parakeet_models_card_;
    case ModelList::gigaam: return gigaam_models_card_;
    }
    return nullptr;
}

void MainWindow::select_model(ModelList list, int index)
{
    // The hidden combo carries the choice to the presenter, exactly as if the user
    // had picked it in the old combo box; the other toggles are cleared because a
    // model choice is exclusive.
    QComboBox* combo = size_combo_for(list);
    if (combo == nullptr || index < 0 || index >= combo->count()) {
        return;
    }
    combo->setCurrentIndex(index);
    refresh_model_list();
}

void MainWindow::refresh_model_list()
{
    const auto engine = static_cast<TranscriptionEngine>(engine_->currentData().toInt());
    for (const auto list : {ModelList::whisper, ModelList::parakeet, ModelList::gigaam}) {
        if (QWidget* card = card_for(list)) {
            card->setVisible(list == engine);
        }
    }

    const auto sync = [](const std::vector<ToggleSwitch*>& toggles, int selected) {
        for (std::size_t index = 0; index < toggles.size(); ++index) {
            const bool on = static_cast<int>(index) == selected;
            if (toggles[index]->isChecked() != on) {
                QSignalBlocker blocker(toggles[index]);
                toggles[index]->setChecked(on);
            }
        }
    };
    refresh_model_buttons();
    for (const auto list : {ModelList::whisper, ModelList::parakeet, ModelList::gigaam}) {
        QComboBox* combo = size_combo_for(list);
        if (combo == nullptr) {
            continue;
        }
        const int selected = combo->currentIndex();
        sync(toggles_for(list), selected);
        // The row of the model in use is tinted and carries the "Активна" badge
        // (index.css: .model-item.active and .model-badge).
        const auto& toggles = toggles_for(list);
        for (std::size_t index = 0; index < toggles.size(); ++index) {
            const bool active = static_cast<int>(index) == selected;
            QWidget* row = toggles[index]->parentWidget();
            if (row != nullptr && row->property("active").toBool() != active) {
                row->setProperty("active", active);
                // A dynamic property only reaches the stylesheet after a repolish.
                row->style()->unpolish(row);
                row->style()->polish(row);
            }
        }
        for (const auto& labels : model_card_labels_) {
            if (labels.list == list && labels.badge != nullptr) {
                labels.badge->setVisible(labels.index == selected);
            }
        }
    }
}

void MainWindow::bind_settings_to_controls()
{
    const auto& settings = presenter_.settings();

    const auto select = [](QComboBox* box, int value) {
        const auto index = box->findData(value);
        if (index >= 0) {
            box->setCurrentIndex(index);
        }
    };
    select(app_language_, static_cast<int>(settings.app_language));
    select(recognition_language_, static_cast<int>(settings.language));
    select(recording_mode_, static_cast<int>(settings.recording_mode));
    select(engine_, static_cast<int>(settings.transcription_engine));
    select(whisper_size_, static_cast<int>(settings.model_size));
    select(parakeet_size_, static_cast<int>(settings.parakeet_model_size));
    select(gigaam_size_, static_cast<int>(settings.gigaam_model_size));
    temperature_->setValue(settings.temperature);
    // The control carries the engine's range, so a hand-edited out-of-range value is
    // shown clamped. The stored number is left as it is until the user edits the
    // control, exactly like silenceThresholdMs above.
    best_of_->setValue(std::clamp(settings.best_of, kBestOfMin, kBestOfMax));
    terms_->setPlainText(to_q(settings.terms_dictionary));
    record_hotkey_->setText(to_q(settings.record_hotkey));
    cancel_hotkey_->setText(to_q(settings.cancel_hotkey));
    auto_paste_->setChecked(settings.auto_paste_enabled);
    noise_reduction_->setChecked(settings.noise_reduction_enabled);
    condition_on_previous_text_->setChecked(settings.condition_on_previous_text);
    hide_on_focus_loss_->setChecked(settings.hide_on_focus_loss);
    start_with_windows_->setChecked(settings.start_with_windows);
    start_minimized_->setChecked(settings.start_minimized);
    silence_threshold_->setValue(settings.silence_threshold_ms);

    connect(app_language_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const auto chosen = static_cast<AppLanguage>(app_language_->itemData(index).toInt());
        presenter_.update(SettingsChange::language, [chosen](domain::AppSettings& settings) {
            settings.app_language = chosen;
        });
        // In place: the window keeps its position, its size and the current page, and the
        // strings change under the user's hands (Alexander asked for exactly that).
        language_ = chosen;
        retranslate();
    });
    connect(recognition_language_, &QComboBox::currentIndexChanged, this, [this](int index) {
        // The recognition language shares the change code with the interface language and
        // reaches the engine that way.
        pending_service_change_ = SettingsChange::language;
        presenter_.update(SettingsChange::language, [this, index](domain::AppSettings& settings) {
            settings.language = static_cast<RecognitionLanguage>(recognition_language_->itemData(index).toInt());
        });
    });
    connect(recording_mode_, &QComboBox::currentIndexChanged, this, [this](int index) {
        presenter_.update(SettingsChange::behaviour, [this, index](domain::AppSettings& settings) {
            settings.recording_mode = static_cast<RecordingMode>(recording_mode_->itemData(index).toInt());
        });
    });
    connect(engine_, &QComboBox::currentIndexChanged, this, [this](int index) {
        presenter_.update(SettingsChange::engine, [this, index](domain::AppSettings& settings) {
            settings.transcription_engine = static_cast<TranscriptionEngine>(engine_->itemData(index).toInt());
        });
        refresh_status();
        pending_service_change_ = SettingsChange::engine;
    });
    connect(whisper_size_, &QComboBox::currentIndexChanged, this, [this](int index) {
        presenter_.update(SettingsChange::model, [this, index](domain::AppSettings& settings) {
            settings.model_size = static_cast<ModelSize>(whisper_size_->itemData(index).toInt());
        });
        pending_service_change_ = SettingsChange::model;
    });
    connect(parakeet_size_, &QComboBox::currentIndexChanged, this, [this](int index) {
        presenter_.update(SettingsChange::model, [this, index](domain::AppSettings& settings) {
            settings.parakeet_model_size = static_cast<ParakeetModelSize>(parakeet_size_->itemData(index).toInt());
        });
    });
    connect(gigaam_size_, &QComboBox::currentIndexChanged, this, [this](int index) {
        presenter_.update(SettingsChange::model, [this, index](domain::AppSettings& settings) {
            settings.gigaam_model_size = static_cast<GigaamModelSize>(gigaam_size_->itemData(index).toInt());
        });
        pending_service_change_ = SettingsChange::model;
    });
    connect(temperature_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        presenter_.update(SettingsChange::model, [value](domain::AppSettings& settings) { settings.temperature = value; });
    });
    // The candidate count travels the same way as temperature: it is a decoder
    // parameter, so it is stored, autosaved by the presenter, and picked up by the
    // next dictation through the session options provider. No service restart.
    connect(best_of_, &QSpinBox::valueChanged, this, [this](int value) {
        presenter_.update(SettingsChange::model, [value](domain::AppSettings& settings) { settings.best_of = value; });
    });
    connect(terms_, &QPlainTextEdit::textChanged, this, [this] {
        const QString text = terms_->toPlainText();
        presenter_.update(SettingsChange::model, [text](domain::AppSettings& settings) {
            settings.terms_dictionary = text.toStdString();
        });
    });
    connect(record_hotkey_, &QLineEdit::textChanged, this, [this](const QString& text) {
        presenter_.update(SettingsChange::hotkeys, [text](domain::AppSettings& settings) {
            settings.record_hotkey = text.toStdString();
        });
        refresh_status();
        pending_service_change_ = SettingsChange::hotkeys;
    });
    connect(cancel_hotkey_, &QLineEdit::textChanged, this, [this](const QString& text) {
        presenter_.update(SettingsChange::hotkeys, [text](domain::AppSettings& settings) {
            settings.cancel_hotkey = text.toStdString();
        });
    });
    connect(microphone_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0) {
            return;
        }
        const auto id = microphone_->itemData(index).toString();
        presenter_.update(SettingsChange::microphone, [id](domain::AppSettings& settings) {
            if (id.isEmpty()) {
                settings.microphone_device_id.reset();
            } else {
                settings.microphone_device_id = id.toStdString();
            }
        });
        // This line used to sit outside the lambda, where it ran once during
        // binding instead of on a user edit, so a microphone change never reached
        // the running capture backend.
        pending_service_change_ = SettingsChange::microphone;
    });
    connect(auto_paste_, &QAbstractButton::toggled, this, [this](bool value) {
        presenter_.update(SettingsChange::behaviour, [value](domain::AppSettings& settings) {
            settings.auto_paste_enabled = value;
        });
    });
    connect(noise_reduction_, &QAbstractButton::toggled, this, [this](bool value) {
        presenter_.update(SettingsChange::behaviour, [value](domain::AppSettings& settings) {
            settings.noise_reduction_enabled = value;
        });
    });
    connect(condition_on_previous_text_, &QAbstractButton::toggled, this, [this](bool value) {
        presenter_.update(SettingsChange::behaviour, [value](domain::AppSettings& settings) {
            settings.condition_on_previous_text = value;
        });
    });
    connect(silence_threshold_, &QSpinBox::valueChanged, this, [this](int value) {
        presenter_.update(SettingsChange::behaviour, [value](domain::AppSettings& settings) {
            settings.silence_threshold_ms = value;
        });
        pending_service_change_ = SettingsChange::behaviour;
    });
    connect(hide_on_focus_loss_, &QAbstractButton::toggled, this, [this](bool value) {
        presenter_.update(SettingsChange::behaviour, [value](domain::AppSettings& settings) {
            settings.hide_on_focus_loss = value;
        });
    });
    connect(start_with_windows_, &QAbstractButton::toggled, this, [this](bool value) {
        presenter_.update(SettingsChange::startup, [value](domain::AppSettings& settings) {
            settings.start_with_windows = value;
        });
    });
    connect(start_minimized_, &QAbstractButton::toggled, this, [this](bool value) {
        presenter_.update(SettingsChange::startup, [value](domain::AppSettings& settings) {
            settings.start_minimized = value;
        });
    });
}

bool MainWindow::system_theme_is_dark() const
{
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

bool MainWindow::event(QEvent* event)
{
    // «Скрывать при потере фокуса» (moved to the appearance page with the rest of the
    // window behaviour): switching to another window sends the settings window to the
    // tray while the application keeps running. A modal dialog of our own deactivates the
    // window too, and hiding then would make the dialog look like it killed the app, so
    // those are excluded.
    if (event->type() == QEvent::WindowDeactivate && hide_on_focus_loss_ != nullptr
        && hide_on_focus_loss_->isChecked() && isVisible()
        && QApplication::activeModalWidget() == nullptr && QApplication::activePopupWidget() == nullptr) {
        hide();
    }
    return QMainWindow::event(event);
}

void MainWindow::bring_to_front()
{
    if (isMinimized()) {
        // show() alone does not un-minimise, so the state is cleared first.
        setWindowState(windowState() & ~Qt::WindowMinimized);
    }
    show();
    raise();
    // raise() only reorders the window inside the application; activateWindow() is what
    // asks the system to give it the foreground and the keyboard focus.
    activateWindow();
    if (auto* handle = windowHandle(); handle != nullptr) {
        // The documented second half: on Windows activation can be refused when the
        // process is not the foreground one, and requesting it on the handle is the way
        // Qt retries it without the stay-on-top hack.
        handle->requestActivate();
    }
}

void MainWindow::retranslate()
{
    language_ = presenter_.settings().app_language;
    // Every label carries the key it was created from, so the window can be re-lettered
    // exactly, without losing its position, its size or the page the user is on.
    // Labels created without a key are invisible to the property pass below. The port
    // keeps both languages in one table, so the text itself is an exact key: looking it
    // up catches those labels (Alexander, 08.10.2026: the brand tagline stayed Russian
    // after switching to English).
    QHash<QString, UiKey> key_by_text;
    for (int index = 0; index < static_cast<int>(UiKey::kCount); ++index) {
        const auto key = static_cast<UiKey>(index);
        key_by_text.insert(ui_text(key, AppLanguage::ru), key);
        key_by_text.insert(ui_text(key, AppLanguage::en), key);
    }
    for (auto* label : findChildren<QLabel*>()) {
        if (label->property("uiKey").isValid() || label->property("uiTextKey").isValid()) {
            continue;
        }
        const auto found = key_by_text.constFind(label->text());
        if (found != key_by_text.constEnd()) {
            label->setText(ui_text(*found, language_));
        }
    }
    for (auto* label : findChildren<QLabel*>()) {
        const QVariant key = label->property("uiKey");
        if (key.isValid()) {
            label->setText(ui_text(static_cast<UiKey>(key.toInt()), language_));
        }
        const QVariant text_key = label->property("uiTextKey");
        if (text_key.isValid()) {
            label->setText(ui_text(static_cast<UiKey>(text_key.toInt()), language_));
        }
        const QVariant hotkey_key = label->property("hotkeyHintKey");
        if (hotkey_key.isValid()) {
            label->setText(ui_text(static_cast<UiKey>(hotkey_key.toInt()), language_)
                    .arg(QString::fromStdString(presenter_.settings().record_hotkey)));
        }
        const QVariant copy_keys = label->property("aboutCopyKeys");
        if (copy_keys.isValid()) {
            // One paragraph made of two table sentences (k74 + k75): the generic pass
            // would replace the whole text with one of them, so the pair is rejoined here.
            QString paragraph;
            for (const QVariant& part : copy_keys.toList()) {
                if (!paragraph.isEmpty()) {
                    paragraph += QLatin1Char(' ');
                }
                paragraph += ui_text(static_cast<UiKey>(part.toInt()), language_);
            }
            label->setText(paragraph);
        }
        const QVariant chip_key = label->property("versionChipKey");
        if (chip_key.isValid()) {
            // "Версия X.Y.Z": the word comes from the table, the number from the build.
            label->setText(ui_text(static_cast<UiKey>(chip_key.toInt()), language_)
                + QLatin1Char(' ') + label->property("versionValue").toString());
        }
        if (label->property("uppercaseText").isValid()) {
            // The mockup uppercases this label with text-transform, which QSS does not
            // have: the property says so, and the raised text is rebuilt here too.
            label->setText(label->text().toUpper());
        }
    }
    for (auto* button : findChildren<QPushButton*>()) {
        const QVariant key = button->property("uiKey");
        if (key.isValid()) {
            button->setText(ui_text(static_cast<UiKey>(key.toInt()), language_));
        }
    }
    for (auto* widget : findChildren<QWidget*>()) {
        const QVariant key = widget->property("uiTooltipKey");
        if (key.isValid()) {
            widget->setToolTip(ui_text(static_cast<UiKey>(key.toInt()), language_));
        }
    }
    // Navigation items, the page title and the parts that are not plain labels.
    if (nav_ != nullptr) {
        for (int index = 0; index < nav_->count(); ++index) {
            QListWidgetItem* item = nav_->item(index);
            const QString key = item->data(Qt::UserRole + 2).toString();
            for (const auto& entry : kNavigation) {
                if (key == QString::fromUtf8(entry.key)) {
                    item->setText(ui_text(entry.text, language_));
                    break;
                }
            }
        }
        refresh_page_title(nav_->currentRow());
    }
    if (page_breadcrumb_ != nullptr) {
        // Not driven by the uiKey property: the mockup uppercases this line, and the
        // generic re-lettering pass would drop the uppercase.
        page_breadcrumb_->setText(ui_text(UiKey::k111, language_).toUpper());
    }
    if (silence_threshold_ != nullptr) {
        silence_threshold_->setSuffix(ui_text(UiKey::k0, language_));
    }
    // The combo ITEM texts are built once, when the window is built, so a language change
    // left them in the old language: the interface was Russian while "Язык интерфейса"
    // showed "Russian" (Alexander, 06.10.2026). Only the two language combos carry text
    // from the table; the others are product names and mode names.
    if (app_language_ != nullptr && app_language_->count() >= 2) {
        app_language_->setItemText(0, ui_text(UiKey::k30, language_));
    }
    if (recognition_language_ != nullptr && recognition_language_->count() >= 3) {
        recognition_language_->setItemText(0, ui_text(UiKey::k3, language_));
        recognition_language_->setItemText(1, ui_text(UiKey::k30, language_));
    }
    // The model cards are built once, so their four texts are re-applied here.
    for (const auto& card : model_card_labels_) {
        if (card.name == nullptr || card.index < 0) {
            continue;
        }
        std::size_t count = 0;
        const ModelRow* rows = model_rows(card.list, count);
        if (static_cast<std::size_t>(card.index) >= count) {
            continue;
        }
        const ModelRow& row = rows[card.index];
        card.name->setText(QString::fromUtf8(row.name));
        card.meta->setText(ui_text(UiKey::k2, language_)
            .arg(QString::fromUtf8(model_field(row.size_ru, row.size_en, language_)),
                QString::fromUtf8(model_field(row.speed_ru, row.speed_en, language_)),
                QString::fromUtf8(model_field(row.quality_ru, row.quality_en, language_))));
        card.description->setText(
            QString::fromUtf8(model_field(row.description_ru, row.description_en, language_)));
    }
    refresh_model_list();
    refresh_status();
}

void MainWindow::bind_update_controls()
{
    if (update_check_ == nullptr) {
        return;
    }
    if (services_.application_version) {
        const QString version = services_.application_version();
        // The hero's chip reads "Версия X.Y.Z" (index.css: .version-chip). The number is
        // kept on the label as well, so retranslate() rebuilds the sentence around it.
        update_version_->setProperty("versionValue", version);
        update_version_->setText(ui_text(UiKey::k63, language_) + QLatin1Char(' ') + version);
        if (page_version_ != nullptr) {
            // The brand name is not translated anywhere else in the window either.
            page_version_->setText(QStringLiteral("VoiceTyper %1").arg(version));
        }
    }

    if (!services_.update_check) {
        update_check_->setEnabled(false);
        update_check_->setToolTip(ui_text(UiKey::k71, language_));
        return;
    }
    // See bind_microphone_controls: the window is rebuilt on a language change while a
    // request may be in flight, so the reply is guarded.
    connect(update_check_, &QPushButton::clicked, this, [this] {
        QPointer<MainWindow> self(this);
        update_check_->setEnabled(false);
        update_status_->setText(ui_text(UiKey::k65, language_));
        services_.update_check([self](bool available, QString version, QString notes, QString error) {
            if (self == nullptr) {
                return;
            }
            self->update_check_->setEnabled(true);
            self->update_available_ = available;
            if (!error.isEmpty()) {
                self->update_status_->setText(ui_text(UiKey::k71, self->language_)
                    + QStringLiteral(": ") + error);
                return;
            }
            if (!available) {
                self->update_status_->setText(ui_text(UiKey::k66, self->language_));
                set_inset_visible(self->update_notes_, false);
                set_inset_visible(self->update_progress_, false);
                if (self->update_row_ != nullptr) {
                    self->update_row_->setVisible(false);
                }
                return;
            }
            self->update_status_->setText(ui_text(UiKey::k67, self->language_).arg(version));
            // The release body is markdown and the card is plain text: the emphasis markers
            // are dropped instead of shown raw ("**Full Changelog**:").
            QString shown_notes = notes;
            shown_notes.remove(QStringLiteral("**"));
            self->update_notes_->setText(shown_notes);
            set_inset_visible(self->update_notes_, !notes.isEmpty());
            if (self->update_row_ != nullptr) {
                self->update_row_->setVisible(true);
            }
            self->update_install_->setVisible(true);
            // The height comes from WrappingLabel now: the deferred setMinimumHeight that
            // used to stand here only ever grew, so every check added a little more blank
            // space to the block (Alexander, 08.10.2026).
        });
    });

    if (services_.update_install) {
        connect(update_install_, &QPushButton::clicked, this, [this] {
            QPointer<MainWindow> self(this);
            update_install_->setEnabled(false);
            update_progress_->setRange(0, 0); // unknown until the first progress report
            set_inset_visible(update_progress_, true);
            update_status_->setText(ui_text(UiKey::k69, language_));
            services_.update_install([self](int percent, QString stage, QString error) {
                if (self == nullptr) {
                    return;
                }
                if (!error.isEmpty()) {
                    self->update_install_->setEnabled(true);
                    self->update_status_->setText(ui_text(UiKey::k72, self->language_)
                        + QStringLiteral(": ") + error);
                    return;
                }
                if (percent >= 0) {
                    self->update_progress_->setRange(0, 100);
                    self->update_progress_->setValue(percent);
                }
                if (stage == QStringLiteral("done")) {
                    self->update_status_->setText(ui_text(UiKey::k70, self->language_));
                }
            });
        });
    } else {
        update_install_->setEnabled(false);
    }
}

void MainWindow::bind_microphone_controls()
{
    if (microphone_level_ == nullptr) {
        return;
    }
    if (services_.microphone_level_get) {
        const int level = std::clamp(services_.microphone_level_get(), 0, 100);
        microphone_level_->setValue(level);
        microphone_level_value_->setText(QString::number(level) + QStringLiteral("%"));
    } else {
        microphone_level_->setEnabled(false);
        microphone_level_->setToolTip(ui_text(UiKey::k60, language_));
        microphone_level_->setProperty("uiTooltipKey", static_cast<int>(UiKey::k60));
        microphone_level_value_->setText(QStringLiteral("—"));
    }
    if (!services_.microphone_level_set) {
        microphone_level_->setEnabled(false);
        microphone_level_->setToolTip(ui_text(UiKey::k60, language_));
    }

    connect(microphone_level_, &QSlider::valueChanged, this, [this](int value) {
        microphone_level_value_->setText(QString::number(value) + QStringLiteral("%"));
        if (services_.microphone_level_set) {
            static_cast<void>(services_.microphone_level_set(value));
        }
    });

    if (!services_.microphone_probe) {
        microphone_test_->setEnabled(false);
        microphone_test_->setToolTip(ui_text(UiKey::k60, language_));
        microphone_test_->setProperty("uiTooltipKey", static_cast<int>(UiKey::k60));
        return;
    }
    // The two states of the mockup's .microphone-test: the sentence under the title and
    // the button swap together ("Начать проверку" / "Остановить"). Both carry the key of
    // the string they show, so a language change re-letters whichever is current.
    const auto show_state = [this](UiKey state_key, UiKey button_key) {
        microphone_test_state_->setProperty("uiKey", static_cast<int>(state_key));
        microphone_test_state_->setText(ui_text(state_key, language_));
        microphone_test_->setProperty("uiKey", static_cast<int>(button_key));
        microphone_test_->setText(ui_text(button_key, language_));
    };
    const auto show_verdict = [this](const QString& text) {
        microphone_test_result_->setText(text);
        // An empty verdict would leave a blank line under the meter.
        microphone_test_result_->setVisible(!text.isEmpty());
    };
    // The window can be rebuilt on a language change while the probe is running, so
    // the reply is guarded by a QPointer rather than a raw this; the generation counter
    // additionally drops the reply of a probe the user stopped.
    // While the probe runs the meter follows the live level, so what moves on screen is
    // the voice: the old code painted a decorative profile and the level arrived only at
    // the end (Alexander, 08.10.2026).
    microphone_level_timer_ = new QTimer(this);
    microphone_level_timer_->setInterval(80);
    connect(microphone_level_timer_, &QTimer::timeout, this, [this] {
        if (!microphone_testing_ || microphone_level_meter_ == nullptr || !services_.microphone_probe_level) {
            return;
        }
        // Linear scaling was invisible: a speaking voice peaks at a few percent of full
        // scale, so the bars stayed at their floor and the meter looked dead. Decibels over
        // a 60 dB range below full scale are what the ear reads as loudness (Alexander,
        // 08.10.2026: the indicator must show that the microphone hears the voice).
        const double peak = std::clamp(services_.microphone_probe_level(), 0.0, 1.0);
        const double decibels = 20.0 * std::log10(std::max(peak, 1e-6));
        const int percent = static_cast<int>(
            std::lround(std::clamp((decibels + 60.0) / 60.0, 0.0, 1.0) * 100.0));
        level_meter_push_level(microphone_level_meter_, percent);
    });
    connect(microphone_test_, &QPushButton::clicked, this, [this, show_state, show_verdict] {
        QPointer<MainWindow> self(this);
        if (microphone_testing_) {
            // "Остановить": the capture worker reports only at the end (2.5 s) and has no
            // cancel path of its own, so stopping means dropping that reply and returning
            // the block to its idle state at once.
            // The generation is deliberately NOT bumped: the capture worker ends its loop,
            // stops the device and reports, so stopping shows the verdict instead of leaving
            // the block hanging with nothing (reported from the running build).
            microphone_testing_ = false;
            if (services_.microphone_probe_cancel) {
                services_.microphone_probe_cancel();
            }
            if (microphone_level_timer_ != nullptr) {
                microphone_level_timer_->stop();
            }
            level_meter_set_testing(microphone_level_meter_, false);
            level_meter_set_level(microphone_level_meter_, 0);
            show_verdict(QString());
            show_state(UiKey::k147, UiKey::k149);
            return;
        }
        microphone_testing_ = true;
        if (microphone_level_timer_ != nullptr) {
            microphone_level_timer_->start();
        }
        const int generation = ++microphone_probe_generation_;
        // k25 is the table's own "Остановить" (the record button's stop label).
        show_state(UiKey::k148, UiKey::k25);
        level_meter_set_testing(microphone_level_meter_, true);
        show_verdict(ui_text(UiKey::k59, language_));
        services_.microphone_probe([self, generation](bool heard, double peak, QString detail) {
            if (self == nullptr || generation != self->microphone_probe_generation_) {
                return;
            }
            self->microphone_testing_ = false;
            if (self->microphone_level_timer_ != nullptr) {
                self->microphone_level_timer_->stop();
            }
            level_meter_set_testing(self->microphone_level_meter_, false);
            level_meter_set_level(self->microphone_level_meter_,
                static_cast<int>(std::lround(std::clamp(peak, 0.0, 1.0) * 100.0)));
            self->microphone_test_state_->setProperty("uiKey", static_cast<int>(UiKey::k147));
            self->microphone_test_state_->setText(ui_text(UiKey::k147, self->language_));
            self->microphone_test_->setProperty("uiKey", static_cast<int>(UiKey::k149));
            self->microphone_test_->setText(ui_text(UiKey::k149, self->language_));
            QString verdict = ui_text(heard ? UiKey::k61 : UiKey::k62, self->language_);
            // The backend text is for the journal, not for the card: on success the user
            // needs the plain sentence, and the reason matters only when the probe failed.
            if (!detail.isEmpty() && !heard) {
                verdict += QStringLiteral(" (") + detail + QStringLiteral(")");
            }
            self->microphone_test_result_->setText(verdict);
            self->microphone_test_result_->setVisible(true);
        });
    });
}

void MainWindow::start_model_download(ModelList list, int index)
{
    if (!services_.model_download) {
        return;
    }
    const auto& bars = progress_for(list);
    if (index < 0 || static_cast<std::size_t>(index) >= bars.size()) {
        return;
    }
    QProgressBar* bar = bars[static_cast<std::size_t>(index)];
    auto& busy = busy_for(list);
    // A percentage, not a cycling animation: the server reports the length, so the bar shows
    // real progress from the first report on.
    bar->setRange(0, 100);
    bar->setValue(0);
    bar->setFormat(QStringLiteral("%p%"));
    bar->setTextVisible(false);
    bar->show();
    if (bar->parentWidget() != nullptr) {
        if (QLabel* percent = bar->parentWidget()->findChild<QLabel*>(bar->objectName() + QStringLiteral("Percent"))) {
            percent->setText(QStringLiteral("0%"));
            percent->show();
        }
    }
    busy[static_cast<std::size_t>(index)] = true;
    refresh_model_buttons();
    set_status_message(ui_text(UiKey::k105, language_));

    QPointer<MainWindow> self(this);
    services_.model_download(list, index,
        [self, list, index](WindowServices::ModelTransfer report) {
        if (self == nullptr) {
            return;
        }
        // The report arrives on a worker thread, and everything below touches widgets: the
        // progress bar, the button icons and their stylesheet roles. Painting from two threads
        // is what produced «QWidget::repaint: Recursive repaint detected» and then a crash
        // inside Qt6Gui on the device (07.10.2026), so the whole handler is marshalled onto
        // the interface thread, the way the rest of this file already does it.
        QMetaObject::invokeMethod(self, [self, list, index, report] {
        if (self == nullptr) {
            return;
        }
        const auto& progress = self->progress_for(list);
        auto& busy = self->busy_for(list);
        if (index < 0 || static_cast<std::size_t>(index) >= progress.size()
            || static_cast<std::size_t>(index) >= busy.size()) {
            return;
        }
        QProgressBar* bar = progress[static_cast<std::size_t>(index)];
        const auto percent_of = [bar]() -> QLabel* {
            return bar->parentWidget() != nullptr
                ? bar->parentWidget()->findChild<QLabel*>(bar->objectName() + QStringLiteral("Percent"))
                : nullptr;
        };
        const auto finish = [&] {
            busy[static_cast<std::size_t>(index)] = false;
            bar->hide();
            if (QLabel* percent = percent_of()) {
                percent->hide();
            }
            self->refresh_model_buttons();
        };
        if (!report.error.isEmpty()) {
            // A cancellation is a normal outcome, not a failure: say so, without the reason.
            finish();
            self->set_status_message(report.cancelled
                    ? ui_text(UiKey::k108, self->language_)
                    : ui_text(UiKey::k106, self->language_) + QStringLiteral(": ") + report.error);
            return;
        }
        if (report.percent >= 0) {
            bar->setValue(std::min(100, report.percent));
            if (QLabel* percent = percent_of()) {
                percent->setText(QString::number(report.percent) + QStringLiteral("%"));
            }
            self->set_status_message(ui_text(UiKey::k107, self->language_)
                .arg(report.percent));
        }
        if (report.percent >= 100) {
            finish();
            self->set_status_message(QString());
        }
        }, Qt::QueuedConnection);
    });
}

void MainWindow::refresh_model_buttons()
{
    // "Скачать" is disabled with the reason, because the model download service is
    // not written yet; "Удалить" is live as soon as the file exists.
    const auto update = [this](ModelList list) {
        const auto& buttons = buttons_for(list);
        for (std::size_t index = 0; index < buttons.size(); ++index) {
            QPushButton* button = buttons[index];
            const auto& busy_flags = busy_for(list);
            const bool busy = static_cast<std::size_t>(index) < busy_flags.size()
                && busy_flags[static_cast<std::size_t>(index)];
            const bool downloaded = !busy && services_.model_is_downloaded
                && services_.model_is_downloaded(list, static_cast<int>(index));
            if (busy) {
                button->setIcon(drawn_icon(DrawnIcon::cancel, 18, QColor(QStringLiteral("#EC5F67"))));
                button->setToolTip(ui_text(UiKey::k109, language_));
                button->setProperty("buttonRole", QStringLiteral("remove"));
                button->setEnabled(services_.model_download_cancel != nullptr);
            } else if (downloaded) {
                button->setIcon(drawn_icon(DrawnIcon::remove, 18, QColor(QStringLiteral("#EC5F67"))));
                button->setToolTip(ui_text(UiKey::k37, language_));
                button->setProperty("buttonRole", QStringLiteral("remove"));
                button->setEnabled(static_cast<bool>(services_.model_delete));
                button->setToolTip(ui_text(UiKey::k38, language_));
            } else {
                button->setIcon(drawn_icon(DrawnIcon::download, 18, QColor(QStringLiteral("#39C395"))));
                button->setToolTip(ui_text(UiKey::k31, language_));
                button->setProperty("buttonRole", QStringLiteral("download"));
                button->setEnabled(static_cast<bool>(services_.model_download));
                button->setToolTip(services_.model_download ? ui_text(UiKey::k31, language_)
                                                            : ui_text(UiKey::k106, language_));
            }
            // A dynamic property change only reaches the stylesheet after a repolish.
            button->style()->unpolish(button);
            button->style()->polish(button);
        }
    };
    update(ModelList::whisper);
    update(ModelList::parakeet);
    update(ModelList::gigaam);
}

void MainWindow::refresh_page_title(int index)
{
    if (page_title_ == nullptr || nav_ == nullptr || index < 0 || index >= nav_->count()) {
        return;
    }
    // The .NET page labels are a sentence fragment ("Общие"); the band shows the same
    // word capitalised as a heading, as the reference does.
    page_title_->setText(nav_->item(index)->text());
}

void MainWindow::refresh_navigation_icons()
{
    if (nav_ == nullptr || nav_->count() <= 0) {
        return;
    }
    const QColor icon_colour(muted_text_.isEmpty() ? QStringLiteral("#E6E6E6") : muted_text_);
    nav_->setIconSize(QSize(kNavigationIconSizePx, kNavigationIconSizePx));
    for (int row = 0; row < nav_->count(); ++row) {
        QListWidgetItem* item = nav_->item(row);
        if (item == nullptr) {
            continue;
        }
        // Matched by the visible label: the menu is filled by two different passes, and
        // only this one runs whatever path created the item (the item data was missing on
        // the items the second pass added, which is why the icons never appeared).
        DrawnIcon icon = DrawnIcon::about;
        const QString item_key = item->data(Qt::UserRole + 2).toString();
        for (const auto& entry : kNavigation) {
            if (item_key == QString::fromUtf8(entry.key)
                || item->text() == ui_text(entry.text, language_)) {
                icon = navigation_drawn_icon(entry.key);
                item->setData(Qt::UserRole + 1, QString::fromUtf8(entry.glyph));
                item->setData(Qt::UserRole + 2, QString::fromUtf8(entry.key));
                break;
            }
        }
        item->setIcon(drawn_icon(icon, kNavigationIconSizePx, icon_colour));
    }
}

void MainWindow::refresh_title_button_icons()
{
    refresh_navigation_icons();
    const QColor colour(muted_text_.isEmpty() ? QStringLiteral("#E6E6E6") : muted_text_);
    for (const auto& [button, kind] : title_button_icons_) {
        QPixmap pixmap(10, 10);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QPen(colour, 1.0));
        switch (kind) {
        case TitleButtonKind::close:
            painter.drawLine(0, 0, 9, 9);
            painter.drawLine(9, 0, 0, 9);
            break;
        case TitleButtonKind::minimize:
            painter.drawLine(0, 5, 9, 5);
            break;
        }
        painter.end();
        button->setIcon(QIcon(pixmap));
    }
}

void MainWindow::refresh_nav_icons()
{
    // The .NET navigation uses the Segoe MDL2 Assets glyph font. Rendering the
    // glyph into an icon keeps the label in the UI font; when the font is not
    // installed (any non-Windows machine, stripped Windows images) the entries
    // simply stay text-only instead of showing replacement boxes.
    static const bool has_icon_font = QFontDatabase::families().contains(QStringLiteral("Segoe MDL2 Assets"));
    if (nav_ == nullptr) {
        return;
    }
    QFont glyph_font(QStringLiteral("Segoe MDL2 Assets"));
    glyph_font.setPixelSize(16);
    for (int row = 0; row < nav_->count(); ++row) {
        QListWidgetItem* item = nav_->item(row);
        const QString glyph = item->data(Qt::UserRole + 1).toString();
        if (!has_icon_font || glyph.isEmpty()) {
            item->setIcon(QIcon());
            continue;
        }
        QPixmap pixmap(20, 20);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setFont(glyph_font);
        painter.setPen(QColor(muted_text_));
        painter.drawText(pixmap.rect(), Qt::AlignCenter, glyph);
        painter.end();
        item->setIcon(QIcon(pixmap));
    }
}

void MainWindow::apply_theme()
{
    // The palette and the control styles are the .NET ones (ThemeManager.cs:72-78
    // plus Window.Styles in MainWindow.axaml), expressed as one stylesheet so
    // every page inherits the same look.
    const AppTheme theme = presenter_.settings().theme;
    const bool dark = theme == AppTheme::dark || (theme == AppTheme::system && system_theme_is_dark());
    const Palette& p = dark ? kDarkPalette : kLightPalette;
    // Icons drawn by hand (navigation, window buttons) follow the mockup's second text
    // level: `nav button` and `.window-actions button` are both --text-secondary there.
    muted_text_ = QString::fromLatin1(p.text_secondary);
    // The log's message column follows the theme, so a theme switch re-renders the view.
    g_log_message_color = QColor(QString::fromLatin1(p.text_secondary));
    log_shown_text_.clear();

    QString qss = QStringLiteral(R"(
        QWidget {
            background: @{window}; color: @{text};
            /* One family for both scripts. The list used to start with "Segoe UI Variable",
               and Qt picks a font per character: Latin took the variable face while Cyrillic
               fell through to Inter, so Russian and English looked like different typefaces
               (Alexander, 08.10.2026). "Segoe UI" is the same design family, covers both
               scripts itself, and "Inter" stays as the shipped fallback. */
            font-family: "Segoe UI", "Inter", sans-serif; font-size: 12px;
        }
        /* Title bar: flat, like the system window chrome. */
        #titleBar { background: @{window}; border-bottom: 1px solid @{border}; }
        #titleBar QLabel { background: transparent; font-size: 13px; }
        #titleBar QPushButton { background: transparent; border: none; color: @{text}; padding: 0px; border-radius: 0px; }
        #titleBar QPushButton:hover { background: @{hover}; }
        #titleBarClose:hover { background: #C42B1C; color: #FFFFFF; }
        QPushButton#titleBarClose:hover { background-color: #C42B1C; }
        /* Navigation column: rounded items, an accent bar on the selected one. */
        #sidebar { background: @{window}; border-right: 1px solid @{border}; }
        #sideNav { background: transparent; border: none; outline: none; }
        /* Rounded rows, no separators (index.css: nav button radius 6px, and the
           selected row is filled with --surface-active). */
        #sideNav::item {
            color: @{text_secondary}; padding: 0px 15px; margin: 0px; border: none; border-radius: 6px;
            font-weight: normal;
        }
        #sideNav::item:hover { background: @{hover}; color: @{text}; }
        #sideNav::item:selected { background: @{selection}; color: @{text}; }
        #brandTagline { color: @{text_secondary}; font-size: 12px; }
        /* Page: a large heading, then one card of setting rows. */
        #sectionTitle { color: @{text}; font-size: 18px; font-weight: 600; letter-spacing: 0px; }
        /* The page title strip: a tinted band that does not scroll with the rows. */
        #pageHeader { background: @{header}; border-bottom: 1px solid @{border}; min-height: 80px; max-height: 80px; }
        #pageTitle { color: @{text}; font-size: 23px; font-weight: 600; background: transparent; }
        #pageBreadcrumb { color: @{accent}; background: transparent; }
        #pageVersion { color: @{muted}; background: transparent; }
        /* The selected navigation row carries a short accent pill (index.css:
           nav button.active i, 3x19 px with the right corners rounded). */
        #navAccent {
            background: @{accent};
            border-top-right-radius: 2px; border-bottom-right-radius: 2px;
        }
        #card {
            background: @{window}; border: 1px solid @{border}; border-radius: 9px;
        }
        #card QLabel { background: transparent; }
        #cardHeader { background: transparent; border-bottom: 1px solid @{border}; }
        #cardHeaderCopy { background: transparent; }
        #cardTitle {
            /* Weight 700 on purpose: Segoe UI exposes its semibold as a separate family, and
               600 silently resolved to Regular on Windows (Alexander, 08.10.2026). */
            font-weight: 700; color: @{text}; }
        #cardCaption { color: @{muted}; }
        /* The pages sit on --surface-raised while the cards stay --surface. */
        QScrollArea { background: @{surface_raised}; border: none; }
        QScrollArea > QWidget { background: @{surface_raised}; }
        #pageSurface, #pageBody, #pageColumn { background: transparent; }
        /* The hint strip of the mockup: an accent-tinted panel with an "i" badge. */
        #hintBar { background: rgba(54,174,234,6%); border: 1px solid rgba(54,174,234,20%); border-radius: 8px; }
        #hintBadge { color: @{accent}; background: rgba(54,174,234,13%); border-radius: 9px; }
        #hintTitle { color: @{text}; background: transparent; }
        #hintText { color: @{text_secondary}; background: transparent; }
        /* The Launch page's banner (index.css: .launch-card / .launch-icon). */
        #launchCard { background: rgba(54,174,234,6%); border: 1px solid rgba(54,174,234,18%); border-radius: 8px; }
        /* The two texts carry their own transparent background: a blanket
           "#launchCard QLabel" rule would outrank "#launchIcon" by specificity and
           erase the glyph tile. */
        #launchTitle, #launchText { background: transparent; }
        #launchIcon { background: rgba(54,174,234,12%); border-radius: 11px; }
        #launchTitle { color: @{text}; font-weight: 700; }
        #launchText { color: @{muted}; }
        /* The About page's hero (index.css: .about-hero, .version-chip): the mockup's
           diagonal accent wash, the name and its tagline, the version in a pill. */
        #aboutHero {
            background: qlineargradient(x1: 0, y1: 0, x2: 1, y2: 0.36,
                stop: 0 rgba(54,174,234,11%), stop: 1 rgba(54,174,234,2.5%));
            border: 1px solid rgba(54,174,234,18%); border-radius: 9px;
        }
        /* Every child of the hero is transparent: the blanket QWidget background would
           otherwise paint over the gradient (the same reason the launch card names its
           labels one by one). */
        #aboutHeroLogo, #aboutHeroCopy, #aboutHeroTitle, #aboutHeroCaption { background: transparent; }
        /* Every size sits in the stylesheet, not only in setFont(): a font set in code is
           overridden by the QWidget rule above, so a size written only there never shows
           (measured: the hero title came out at 14 px instead of 18). */
        #aboutHeroTitle { color: @{text}; font-size: 18px; }
        #aboutHeroCaption { color: @{text_secondary}; font-size: 10px; }
        #aboutVersionChip {
            color: @{accent}; background: rgba(54,174,234,7%); font-size: 9px;
            border: 1px solid rgba(54,174,234,22%); border-radius: 99px; padding: 5px 8px;
        }
        /* The About card's paragraph, privacy line and link row
           (index.css: .about-copy, .privacy-note, .about-links). */
        #aboutCopy { background: transparent; }
        #aboutParagraph { color: @{text_secondary}; font-size: 11px; }
        #privacyNote { background: transparent; }
        #privacyText { color: #42BD86; font-size: 10px; }
        #aboutLinks { background: transparent; }
        #aboutLink {
            background: transparent; border: none; padding: 0px;
            color: @{accent}; font-size: 9.5px;
        }
        #aboutLink:hover { color: @{accent}; text-decoration: underline; }
        /* The microphone page: the mockup's slider row (.range-row / .range-control) and
           its test block with the level meter (.microphone-test / .level-meter). */
        #rangeRow { background: transparent; border-bottom: 1px solid @{line_soft}; min-height: 72px; }
        #rangeRow:hover { background: @{hover}; }
        #rangeRow QLabel, #rangeControl { background: transparent; }
        /* A fixed slot for the number: with a flexible width the label grew with "100%" and
           shrank with "9%", and the slider next to it changed width (Alexander, 08.10.2026). */
        #microphoneLevelValue {
            min-width: 42px; max-width: 42px; color: @{text_secondary}; font-size: 10px; }
        /* LevelSlider paints itself: with no sub-controls the style engine cannot stretch the
           track at a fractional device ratio (Alexander, 08.10.2026). */
        #microphoneLevelSlider { background: transparent; }
        #microphoneTest { background: transparent; border-top: 1px solid @{line_soft}; }
        #microphoneTestCopy { background: transparent; }
        #microphoneTestTitle { color: @{text}; font-size: 12px; font-weight: 700; }
        #microphoneTestState, #microphoneTestResult { color: @{muted}; font-size: 10px; }
        #microphoneTestButton {
            min-height: 32px; max-height: 32px; padding: 0px 13px; border-radius: 6px;
            background: rgba(54,174,234,7%); border: 1px solid #278FCA; color: @{accent};
            font-size: 10px;
        }
        #microphoneTestButton:hover { background: rgba(54,174,234,14%); }
        #microphoneTestButton:disabled { color: @{muted}; border: 1px solid @{border}; background: transparent; }
        /* The trough is the widget's own background: LevelMeter paints only the 22 bars. */
        #levelMeter { background: @{control}; border: 1px solid @{border}; border-radius: 5px; }
        /* The Hotkeys page keeps a real QLineEdit - the keys are typed into it - but
           draws it as the mockup's kbd chip (index.css: .hotkey-control kbd): 36 px
           tall with its 1 px border, radius 6, monospace, min-width 146. */
        #recordHotkeyEdit, #cancelHotkeyEdit {
            min-height: 34px; max-height: 34px; padding: 0px 11px; border-radius: 6px;
            background: @{control}; border: 1px solid @{border};
            font-family: "Cascadia Code", Consolas, monospace; font-size: 10px;
            /* Belt and braces: even if something selects the readout, it must not look
               selected - the value is changed through the button, not in the field. */
            selection-background-color: transparent; selection-color: @{text};
        }
        #recordHotkeyCapture, #cancelHotkeyCapture {
            min-height: 34px; max-height: 34px; padding: 0px 13px; border-radius: 6px;
            background: @{hover}; border: 1px solid @{border}; font-size: 10px;
        }
        #recordHotkeyCapture:hover, #cancelHotkeyCapture:hover { border: 1px solid @{accent}; color: @{accent}; }
        #recordHotkeyCapture:disabled, #cancelHotkeyCapture:disabled { color: @{muted}; border: 1px solid @{border}; }
        /* The Journal page: quiet heading actions over a monospace log surface
           (index.css: .log-heading button, .log-view, .log-empty). */
        #logActions { background: transparent; }
        #logCopyButton, #logClearButton {
            min-height: 27px; max-height: 27px; padding: 0px 9px; border-radius: 5px;
            background: @{surface_raised}; border: 1px solid @{border};
            color: @{text_secondary}; font-size: 9px;
        }
        #logCopyButton:hover, #logClearButton:hover { color: @{text}; }
        #logClearButton:hover { color: #E55A63; border: 1px solid rgba(229,90,99,50%); }
        #logView, #logEmpty {
            background: @{control}; border: none; border-radius: 0px;
            border-bottom-left-radius: 8px; border-bottom-right-radius: 8px;
            color: @{text_secondary}; font-family: "Cascadia Code", Consolas, monospace; font-size: 10px;
        }
        #logEmpty { color: @{muted}; }
        /* «Внешний вид»: the theme tiles of the mockup (index.css: .theme-options,
           .theme-preview, .theme-options em, .reset-button). The preview itself is
           painted by ThemePreview, which owns the fixed colours of the three variants. */
        #themeOptions, #themeTileLabels, #themeTileIcon { background: transparent; }
        #themeTile { background: @{window}; border: 1px solid @{border}; border-radius: 8px; }
        #themeTile:hover { border: 1px solid @{accent}; }
        #themeTile[selected="true"] { border: 1px solid @{accent}; }
        #themePreview { background: transparent; border: none; }
        #themeTileName { color: @{text_secondary}; font-size: 11px; }
        #themeTileName[selected="true"] { color: @{text}; }
        /* The disc of the selected tile, 19px with the 2px ring of the mockup. The two
           ids are what keeps the rule above "#card QLabel { background: transparent }",
           which would otherwise erase the fill of this very label. */
        #themeTile #themeCheck { background: @{accent}; border: 2px solid @{window}; border-radius: 9px; }
        #appearanceResetButton {
            background: @{window}; border: 1px solid @{border}; border-radius: 6px;
            color: @{text_secondary}; font-size: 10px; padding: 8px 11px;
        }
        #appearanceResetButton:hover { color: @{text}; border: 1px solid @{muted}; }
        /* «Модели»: the mockup's model list (index.css: .model-list, .model-item,
           .model-badge, .dictionary). */
        QWidget[modelList="true"] { background: transparent; border-top: 1px solid @{line_soft}; }
        /* The download indicator: the mockup draws no such control, so it borrows the
           app's own tokens and then follows both themes (Alexander, 08.10.2026). */
        QWidget[modelList="true"] QProgressBar {
            background: @{control}; border: 1px solid @{border}; border-radius: 4px;
            min-height: 6px; max-height: 6px;
        }
        QWidget[modelList="true"] QProgressBar::chunk { background: @{accent}; border-radius: 3px; }
        QLabel[progressPercent="true"] {
            color: @{text_secondary}; background: transparent; font-size: 10px; min-width: 36px;
        }
        #updateProgress {
            background: @{control}; border: 1px solid @{border}; border-radius: 5px;
            min-height: 8px; max-height: 8px;
        }
        #updateProgress::chunk { background: @{accent}; border-radius: 4px; }
        #modelItem { background: transparent; border-top: 1px solid @{line_soft}; min-height: 92px; }
        #modelItem[firstRow="true"] { border-top: 0px; }
        #modelItem[active="true"] { background: rgba(54,174,234,5%); }
        #modelCopy, #modelNameLine { background: transparent; }
        /* index.css: .model-item strong - the model name carries the weight. */
        #modelName { color: @{text}; font-size: 12px; font-weight: 700; }
        #modelMeta { color: @{text_secondary}; font-size: 9.5px; }
        #modelNote { color: @{muted}; font-size: 9.5px; }
        /* Two ids on purpose: "#card QLabel" would otherwise outrank a single one and
           take the accent fill of the badge away (the same trap as the launch icon). */
        #modelItem #modelBadge {
            color: @{accent}; background: rgba(54,174,234,11%); border-radius: 4px;
            padding: 2px 5px; font-size: 8px; font-weight: 700;
        }
        #dictionary { background: transparent; border-top: 1px solid @{line_soft}; }
        #dictionaryTitle { color: @{text}; font-size: 12px; font-weight: 700; }
        #dictionaryCaption, #dictionaryHint { color: @{muted}; font-size: 10px; }
        #termsEdit {
            background: @{control}; color: @{text}; border: 1px solid @{border}; border-radius: 6px;
            padding: 10px 11px; font-family: "Cascadia Code", Consolas, monospace; font-size: 10px;
        }
        #termsEdit:focus { border: 1px solid @{accent}; }
        #settingsRow {
            background: transparent; border-bottom: 1px solid @{line_soft};
            min-height: 68px;
        }
        #settingsRow:hover { background: @{hover}; }
        #settingsRow QLabel { background: transparent; }
        /* Layout-only containers must not paint a background: the generic QWidget rule
           otherwise covers the :hover fill of the row they live in, so the row lights up
           in patches instead of as a whole (Alexander, 2026-10-08). */
        #rowCopy, #rangeRow, #rangeControl, #insetBlock, #cardHeader, #cardHeaderCopy,
        #launchCopy, #aboutHeroCopy, #privacyNote, #aboutCopy, #aboutLinks,
        #microphoneTest, #microphoneTestCopy, #themeTileLabels, #modelCopy, #modelNameLine,
        #hotkeyControl, #themeOptions, #pageColumn, #pageBody, #pageSurface {
            background: transparent;
        }
        /* index.css: the row's text is a <strong> - the title carries the weight. */
        #rowTitle { color: @{text}; font-size: 12px; font-weight: 700; }
        /* The update status plays the part of a row title on the About page. */
        #updateStatus { font-size: 12px; }
        #rowDescription { color: @{muted}; font-size: 10px; }
        #mutedLabel, #hintLabel { color: @{muted}; font-size: 12px; }
        /* Controls: Win11 fields are flat, slightly rounded, and outlined. */
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
            background: @{control}; color: @{text}; border: 1px solid @{border}; border-radius: 4px;
            padding: 5px 8px; min-height: 24px; font-size: 11px;
        }
        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border: 1px solid @{accent}; }
        /* The .NET NumberBox keeps its steppers inside the field; here they were drawn as
           two separate boxes next to it (Alexander, 06.10.2026). They now sit inside the
           field, separated by the same hairline as the border. */
        QSpinBox::up-button, QDoubleSpinBox::up-button {
            subcontrol-origin: border; subcontrol-position: top right;
            width: 18px; margin: 1px 1px 0px 0px; border: none; border-left: 1px solid @{border};
            background: @{control};
        }
        QSpinBox::down-button, QDoubleSpinBox::down-button {
            subcontrol-origin: border; subcontrol-position: bottom right;
            width: 18px; margin: 0px 1px 1px 0px; border: none; border-left: 1px solid @{border};
            background: @{control};
        }
        QSpinBox::up-button:hover, QSpinBox::down-button:hover,
        QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover { background: @{hover}; }
        /* Real triangles: Qt draws the border-based trick as flat bars, so the arrows are
           images from the resource. */
        QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {
            image: url(:/assets/spin-up.png); width: 9px; height: 6px;
        }
        QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {
            image: url(:/assets/spin-down.png); width: 9px; height: 6px;
        }
        QComboBox::drop-down { border: none; width: 22px; }
        QComboBox QAbstractItemView {
            background: @{control}; color: @{text}; border: 1px solid @{border}; selection-background-color: @{hover};
            outline: none;
        }
        QPushButton {
            /* The mockup's buttons are all 6px rounded (index.css: .primary-button,
               .record-button, .hotkey-control button). */
            background: @{hover}; color: @{text}; border: 1px solid @{border}; border-radius: 6px;
            padding: 5px 14px; min-height: 24px; font-size: 10px;
        }
        QPushButton:hover { background: @{control}; border: 1px solid @{accent}; }
        QPushButton:pressed { background: @{control}; }
        QPushButton:disabled { color: @{muted}; border: 1px solid @{border}; }
        /* Semantic buttons: the Figma Make mockup tints a transparent fill and keeps the
           border and glyph saturated (index.css: .model-action, .model-action.danger,
           .primary-button). */
        QPushButton[buttonRole="download"] {
            background: rgba(46,190,141,7%); border: 1px solid #36B98B; color: #39C395;
            border-radius: 6px; padding: 0px;
            /* The 34x34 square of .model-action: QSS measures the content box, so 32 plus
               the two 1px borders is the mockup's 34. Without this the generic
               "min-height: 24px" won and the buttons rendered 34x26 (measured on the
               running build), which is what pushed the glyph off centre. */
            min-width: 32px; max-width: 32px; min-height: 32px; max-height: 32px;
        }
        QPushButton[buttonRole="download"]:hover { background: rgba(46,190,141,14%); }
        QPushButton[buttonRole="download"]:disabled { border: 1px solid #2C5F4E; color: #3E8F72; background: transparent; }
        QPushButton[buttonRole="remove"] {
            background: rgba(216,75,85,6%); border: 1px solid #D84B55; color: #EC5F67;
            border-radius: 6px; padding: 0px;
            min-width: 32px; max-width: 32px; min-height: 32px; max-height: 32px;
        }
        QPushButton[buttonRole="remove"]:hover { background: rgba(216,75,85,13%); }
        /* The mockup's .primary-button: the filled accent action of the About page. The
           mockup fades a disabled one with opacity .65, which QSS has no property for -
           a translucent fill and label say the same. */
        QPushButton[buttonRole="primary"] {
            background: #168FD0; border: 1px solid #278FCA; color: #FFFFFF;
            border-radius: 6px;
        }
        QPushButton[buttonRole="primary"]:hover { background: #117DB7; }
        QPushButton[buttonRole="primary"]:disabled {
            background: rgba(22,143,208,65%); border: 1px solid rgba(39,143,202,65%);
            color: rgba(255,255,255,65%);
        }
        /* The record button is the mockup's primary button: filled accent, white glyph. */
        QPushButton[buttonRole="record"] {
            background: #168FD0; border: 1px solid #278FCA; color: #FFFFFF;
            /* Symmetric padding: the icon and the word are centred as one group. The radius
               is the mockup's 6px, not the generic 4px of a plain button. */
            padding: 0px 14px; min-height: 32px; border-radius: 6px;
        }
        QPushButton[buttonRole="record"]:hover { background: #117DB7; }
        QPushButton[buttonRole="record"][recording="true"] { background: #EC5F67; border: 1px solid #D84B55; color: #FFFFFF; }
        QPushButton[buttonRole="record"][recording="true"]:hover { background: #D84B55; }
        QCheckBox { color: @{text}; spacing: 8px; }
        QTextEdit { background: @{control}; color: @{text}; border: 1px solid @{border}; border-radius: 8px; }
        /* Footer: a quiet status strip with the record button. */
        /* The status bar only hosts the footer: it must not draw a frame or a background of
           its own, which is what left a light strip along the bottom edge. */
        /* The separator belongs to the status bar, which spans the window: drawn on the
           footer inside it the line stopped short of the edges. */
        QStatusBar { background: transparent; border-top: 1px solid @{footer_line}; }
        QStatusBar::item { background: transparent; border: none; }
        /* The style's dashed focus rectangle is not part of the mockup. */
        QPushButton:focus { outline: none; }
        #footer { background: @{window}; }
        #footer QLabel { color: @{text_secondary}; background: transparent; font-size: 10px; }
        /* Two ids on purpose: "#footer QLabel" would otherwise win over the id rule. */
        #footer #footerStatus { color: @{text}; font-weight: 700; }
        /* A status message is prose, not a state: it is not emphasised. */
        #footer #footerStatus[message="true"] { font-weight: 400; }
        #statusPrefix { color: @{muted}; background: transparent; font-size: 10px; }
        #engineDot { background: #42BD86; border-radius: 3px; }
        /* A model that is missing, a load in progress or a failed start: the dot is red. */
        #engineDot[ready="false"] { background: #EC5F67; }
        #hotkeyLabel { color: @{text_secondary}; font-size: 10px; background: transparent; }
        #hotkeyChips { background: transparent; }
        /* index.css: kbd - a small outlined chip per key, joined by a quiet plus. */
        #hotkeyChip {
            /* index.css: kbd carries box-shadow 0 1px 0, which Qt styles have no property
               for - a thicker bottom border draws the same little step. */
            padding: 2px 5px; border: 1px solid @{border}; border-bottom: 2px solid @{border};
            border-radius: 4px; color: @{text_secondary}; background: @{window}; font-size: 9px;
        }
        #hotkeyPlus { color: @{muted}; font-size: 9px; background: transparent; }
        #statusDot { background: @{muted}; border-radius: 3px; }
        #statusDot[recording="true"] { background: #EC5F67; }
        QScrollBar:vertical { background: transparent; width: 12px; margin: 0px; }
        QScrollBar::handle:vertical { background: @{border}; border-radius: 6px; min-height: 28px; margin: 3px; }
        QScrollBar::handle:vertical:hover { background: @{muted}; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }
        QScrollBar:horizontal { height: 0px; }
    )");
    // Named tokens instead of QString::arg: %N is positional, so one unused role used to
    // shift every later colour silently (QString::arg warned about it in the test log).
    // A brace-delimited token cannot be a prefix of another token, so the order of the
    // calls below does not matter.
    const auto put = [&qss](const char* token, const char* value) {
        qss.replace(QString::fromLatin1(token), QString::fromLatin1(value));
    };
    put("@{window}", p.window);
    put("@{footer_line}", p.footer_line);
    put("@{control}", p.control);
    put("@{hover}", p.hover);
    put("@{text}", p.text);
    put("@{muted}", p.muted);
    put("@{border}", p.border);
    put("@{accent}", p.accent);
    put("@{selection}", p.selection);
    put("@{header}", p.header);
    put("@{surface_raised}", p.surface_raised);
    put("@{line_soft}", p.line_soft);
    put("@{text_secondary}", p.text_secondary);
    put("@{preview_line}", p.preview_line);
    put("@{accent_switch}", p.accent_switch);
    qApp->setStyleSheet(qss);
    refresh_title_button_icons();
    if (launch_icon_ != nullptr) {
        // The banner's play glyph in the palette accent (index.css: .launch-icon
        // colour #36AEEA): painted here because only the theme knows the colour.
        launch_icon_->setPixmap(
            drawn_icon(DrawnIcon::launch, 24, QColor(QString::fromLatin1(p.accent))).pixmap(24, 24));
    }
    if (about_hero_logo_ != nullptr) {
        // The brand wave of the hero (index.css: .wave-logo colour #36aeea).
        about_hero_logo_->setPixmap(
            brand_wave_pixmap(42, 34, QColor(QString::fromLatin1(p.accent))));
    }
    if (about_privacy_icon_ != nullptr) {
        // The tick of "Ваши данные остаются на устройстве" (index.css: .privacy-note
        // colour #42bd86).
        about_privacy_icon_->setPixmap(
            drawn_icon(DrawnIcon::check, 16, QColor(QStringLiteral("#42BD86"))).pixmap(16, 16));
    }
    if (microphone_level_meter_ != nullptr) {
        // index.css: .level-meter i is var(--line); a lit one is #36aeea, and every
        // fourth turns #43c78f while the meter runs.
        level_meter_set_colors(microphone_level_meter_,
            QColor(QString::fromLatin1(p.border)), QColor(QString::fromLatin1(p.accent)),
            QColor(QStringLiteral("#43C78F")));
    }
    if (microphone_test_state_ != nullptr) {
        // The known Qt limit: QBoxLayout never asks a nested wrapping label for
        // heightForWidth, so the sentence under "Проверить микрофон" does not make its
        // block taller and a second line would be clipped (the test walks every wrapping
        // label of every page). The mockup's subtitle is a two-line element, so two lines
        // are reserved - here, where the stylesheet font is already in effect, which is
        // what makes the reservation the right height in either theme.
        // QLabel measures a wrapped text as a bounding rect, which comes out one pixel
        // per line taller than lineSpacing(): two lines of this font are
        // 2 * (lineSpacing + 1) px, and that is what is reserved.
        const int line = microphone_test_state_->fontMetrics().lineSpacing() + 1;
        microphone_test_state_->setMinimumHeight(line * 2);
    }
    // The model rows have the same shape two levels down: a wrapped size line and a
    // wrapped note inside the row's own column. Measured on the narrowest window the
    // layout test checks (820x560, an 820px page column squeezed to 432px) the longest
    // meta line wraps to two lines, and a column that never asks for heightForWidth
    // would give it one - so two lines are reserved for both, after the stylesheet font
    // is in effect.
    // The model rows need no reserved height any more: WrappingLabel answers with the
    // height its text needs at the current width, so a single-line row stays single-line
    // and only a wrapped one grows (Alexander, 08.10.2026).
    for (auto* toggle : findChildren<ToggleSwitch*>()) {
        // Win11 toggle: accent when on, a grey track when off, white knob.
        toggle->set_colors(QColor(QString::fromLatin1(p.accent_switch)), QColor(QString::fromLatin1(p.preview_line)),
            QColor(QString::fromLatin1(p.border)), QColor(QStringLiteral("#FFFFFF")));
    }
    // The appearance page: the tile of the stored theme is the selected one, and every
    // tile caption carries its glyph in the palette (index.css: .theme-options > button
    // is --text-secondary, the selected one --text).
    for (auto* tile : theme_tiles_) {
        const AppTheme tile_theme = theme_tile_theme(tile);
        const bool selected = tile_theme == theme;
        theme_tile_set_selected(tile, selected);
        theme_tile_set_label_icon(tile, drawn_icon(theme_tile_icon(tile_theme), 17,
            QColor(QString::fromLatin1(selected ? p.text : p.text_secondary))).pixmap(17, 17));
    }
    if (microphone_level_ != nullptr) {
        level_slider_set_colors(microphone_level_, QColor(QString::fromLatin1(p.border)),
            QColor(QString::fromLatin1(p.accent)), QColor(QString::fromLatin1(p.border)),
            QColor(QString::fromLatin1(p.accent)));
    }
    refresh_nav_icons();
}

void MainWindow::refresh_status_summary()
{
    const auto& settings = presenter_.settings();
    // The recording state machine is the truth about a running session: a dictation can end
    // by itself (silence, the hotkey, a failure), and the button stayed on "Остановить"
    // until it was pressed again (reported from the running build).
    if (services_.recording_active) {
        recording_ = services_.recording_active();
    }
    engine_state_->setText(
        services_.engine_status
            ? services_.engine_status()
            : ui_text(UiKey::k46, language_));
    hotkey_state_->setText(
        services_.record_hotkey_state
            ? services_.record_hotkey_state()
            : ui_text(UiKey::k54, language_));
    // The mockup's footer reads "Запись: <state>" with a dot in front; the summary the
    // strip used to show (theme, autosave, mode) is one hover away in the tooltip.
    const QString summary = ui_text(UiKey::k35, language_)
            .arg(theme_text(settings.theme, language_),
                 presenter_.dirty() ? ui_text(UiKey::k48, language_) : ui_text(UiKey::k52, language_),
                 recording_mode_->currentText());
    // index.css: .statusbar strong - the state is emphasised, the label stays quiet. Plain
    // text and a stylesheet weight: no rich text, so nothing is built off the UI thread.
    if (status_message_.isEmpty()) {
        status_->setText(recording_ ? ui_text(UiKey::k180, language_) : ui_text(UiKey::k179, language_));
    }
    status_->setToolTip(summary);
    if (status_dot_ != nullptr) {
        status_dot_->setToolTip(summary);
        status_dot_->setProperty("recording", recording_);
        status_dot_->style()->unpolish(status_dot_);
        status_dot_->style()->polish(status_dot_);
    }
    const auto can_record = services_.start_recording != nullptr;
    record_button_->setEnabled(can_record);
    // The mockup's button carries a microphone, or a stop glyph while it records.
    record_button_->setIconSize(QSize(16, 16));
    record_button_->setIcon(drawn_icon(recording_ ? DrawnIcon::stop : DrawnIcon::microphone, 16,
        QColor(QStringLiteral("#FFFFFF"))));
    record_button_->setToolTip(recording_ ? ui_text(UiKey::k25, language_) : ui_text(UiKey::k10, language_));
    // The mockup's record button carries a word next to its icon.
    record_button_->setText(recording_ ? ui_text(UiKey::k25, language_) : ui_text(UiKey::k10, language_));
    record_button_->setProperty("recording", recording_);
    record_button_->style()->unpolish(record_button_);
    record_button_->style()->polish(record_button_);
    if (!can_record) {
        record_button_->setToolTip(ui_text(UiKey::k8, language_));
    }
}

void MainWindow::refresh_devices()
{
    // Populating the list is not a user edit. Without this guard the clear() and
    // addItem() sequence emits currentIndexChanged, the handler below reads
    // "По умолчанию" as the chosen device and erases the stored id: a launch that
    // enumerates no device (busy, unplugged, not yet ready) silently lost the
    // user's microphone choice and rewrote settings.json with a null.
    const QSignalBlocker blocker(microphone_);
    microphone_->clear();
    if (!services_.microphones) {
        microphone_->addItem(ui_text(UiKey::k49, language_));
        microphone_->setEnabled(false);
        return;
    }
    microphone_->setEnabled(true);
    const auto devices = services_.microphones();
    microphone_->addItem(ui_text(UiKey::k27, language_), QString());
    for (const auto& device : devices) {
        microphone_->addItem(to_q(device.second), to_q(device.first));
    }
    const auto& selected = presenter_.settings().microphone_device_id;
    int index = selected.has_value() ? microphone_->findData(to_q(*selected)) : 0;
    if (selected.has_value() && index < 0) {
        // The remembered device is not available right now. Show it as such
        // instead of silently switching to something else, and keep it in the
        // settings so the next launch can use it again.
        microphone_->addItem(
            ui_text(UiKey::k1, language_).arg(to_q(*selected)), to_q(*selected));
        index = microphone_->count() - 1;
    }
    microphone_->setCurrentIndex(index >= 0 ? index : 0);
}

void MainWindow::refresh_status()
{
    // A message from a running service wins over the settings summary until the
    // next state change, because it is the one carrying the reason.
    if (!status_message_.isEmpty()) {
        status_->setText(status_message_);
        return;
    }
    refresh_status_summary();
}

void MainWindow::on_presentation_refresh()
{
    if (log_view_ != nullptr && services_.log_text) {
        const QString text = services_.log_text();
        // "Очистить" hides everything logged so far for good: the view shows only the
        // lines that arrive afterwards. Comparing the whole text (the first attempt)
        // brought the old lines back on the very next event, because one new line makes
        // the text different (Alexander, 08.10.2026).
        QString visible = text;
        bool has_new_lines = true;
        if (log_cleared_) {
            if (text.startsWith(log_cleared_at_)) {
                visible = text.mid(log_cleared_at_.size());
                has_new_lines = !visible.trimmed().isEmpty();
            } else {
                // The services rotated or truncated the log, so the baseline cannot be
                // trusted any more: what they report now is what the user gets.
                log_cleared_ = false;
            }
        }
        if (log_cleared_ && !has_new_lines) {
            if (log_stack_->currentWidget() != log_empty_) {
                log_stack_->setCurrentWidget(log_empty_);
            }
        } else if (visible != log_shown_text_) {
            // Rewriting the whole document on every tick put the view back at the top,
            // so the user could not read anything (reported from the running build).
            // Write only when the text really changed, and keep the reader where they
            // were - following the tail only when they were already at the end.
            QScrollBar* bar = log_view_->verticalScrollBar();
            const int previous = bar->value();
            const bool was_at_end = previous >= bar->maximum() - 4;
            render_log_lines(log_view_, visible);
            log_shown_text_ = visible;
            bar->setValue(was_at_end ? bar->maximum() : qMin(previous, bar->maximum()));
            if (log_stack_->currentWidget() != log_view_) {
                log_stack_->setCurrentWidget(log_view_);
            }
        }
    }
    refresh_status();
}

bool MainWindow::needs_service_apply() const
{
    switch (pending_service_change_) {
    case SettingsChange::engine:
    case SettingsChange::model:
    case SettingsChange::hotkeys:
    case SettingsChange::microphone:
        return true;
    case SettingsChange::behaviour:
        // Behaviour covers auto-paste, the recording mode and the VAD silence
        // threshold, all of which the running machine reads per session.
        return true;
    case SettingsChange::language:
        // The interface language cannot be applied to a built window: every label is
        // created once, so the composition has to hear about the change and build the
        // window again (Alexander: "переключил интерфейс на английский, но сам
        // интерфейс не переключился" - the change never left the window at all).
        // The recognition language shares this change code and reaches the engine the
        // same way.
        return true;
    default:
        return false;
    }
}

void MainWindow::on_autosave_timeout()
{
    if (!presenter_.debounce_elapsed()) {
        return;
    }
    (void)presenter_.flush();
    if (needs_service_apply()) {
        // Re-applying after the debounce is what keeps a model reload from
        // happening on every keystroke in the hotkey field.
        pending_service_change_ = SettingsChange::none;
        if (services_.settings_applied) {
            services_.settings_applied(presenter_.settings());
        }
    }
    refresh_status();
}

void MainWindow::on_recording_toggled()
{
    if (recording_) {
        if (services_.stop_recording) {
            services_.stop_recording();
        }
        recording_ = false;
    } else {
        if (services_.start_recording) {
            services_.start_recording();
        }
        recording_ = true;
    }
    refresh_status();
}

void MainWindow::save_now()
{
    if (presenter_.dirty()) {
        (void)presenter_.flush();
    }
}

StatusChannel::StatusChannel(MainWindow* target) : target_(target) {}

StatusChannel::~StatusChannel()
{
    target_.store(nullptr);
}

void StatusChannel::post(const QString& text)
{
    auto* window = target_.load();
    if (window == nullptr) {
        return; // the window is gone; dropping a status line is the safe answer
    }
    QMetaObject::invokeMethod(
        window, [window, text] { window->show_status_message(text); }, Qt::QueuedConnection);
}

void MainWindow::show_status_message(const QString& text)
{
    status_message_ = text;
    if (status_prefix_ != nullptr) {
        status_prefix_->setVisible(text.isEmpty());
    }
    status_->setProperty("message", !text.isEmpty());
    status_->style()->unpolish(status_);
    status_->style()->polish(status_);
    if (!text.isEmpty()) {
        status_->setText(text);
    }
}

QString MainWindow::status_text() const
{
    return status_->text();
}

QString MainWindow::tab_title(int index) const
{
    if (nav_ == nullptr || index < 0 || index >= nav_->count()) {
        return QString();
    }
    return nav_->item(index)->text();
}

} // namespace voicetyper::app
