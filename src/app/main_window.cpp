#include "app/main_window.hpp"
#include "app/ui_text.hpp"

#include "domain/error.hpp"


#include <QComboBox>
#include <QDoubleSpinBox>
#include <QBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QGuiApplication>
#include <QListWidget>
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
#include <QVariant>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <QApplication>
#include <QDebug>
#include <QWindow>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace voicetyper::app {

using domain::AppLanguage;
using domain::AppTheme;
using domain::ModelSize;
using domain::ParakeetModelSize;
using domain::RecognitionLanguage;
using domain::RecordingMode;
using domain::TranscriptionEngine;
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
    // Responsive like a web page: the window opens inside whatever screen it is on
    // (Alexander: "чтобы на любом разрешении окно было видно целиком и все настройки
    // были доступны"), and it may be shrunk well below the reference size - every
    // page scrolls, so nothing becomes unreachable.
    const QRect available = screen() != nullptr ? screen()->availableGeometry() : QRect(0, 0, 1280, 720);
    resize(qMin(980, qMax(560, available.width() - 80)), qMin(640, qMax(420, available.height() - 80)));
    setMinimumSize(520, 400);
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
};

/// Right inset of one settings row, in pixels: the controls stop short of the
/// window edge instead of touching it.
constexpr int kRowRightInset = 10;

constexpr Palette kDarkPalette{
    "#202020", "#202020", "#2D2D2D", "#FFFFFF", "#C5C5C5", "#3D3D3D", "#4CC2FF", "#2D2D2D", "#202020"};
constexpr Palette kLightPalette{
    "#FFFFFF", "#FFFFFF", "#E8EFF6", "#333333", "#6A6A6A", "#EFEFEF", "#0067C0", "#E8EFF6", "#EFF4F9"};

/// The navigation from `SettingsViewModel.cs:307-316`: key order and the Segoe
/// Sidebar icon box, logical pixels: the .NET template drew a 16 px glyph inside it.
inline constexpr int kNavigationIconSizePx = 18;
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

/// The field of a row for the language being shown.
[[nodiscard]] inline const char* model_field(const char* ru, const char* en, AppLanguage language)
{
    return language == AppLanguage::en ? en : ru;
}


/// One settings row: the label pinned to the left edge, the control to the right
/// in its own column. The .NET cards do exactly this (`Border Classes="row"`),
/// and Alexander asked for it explicitly: "подпись всегда слева, а сами настройки
/// выровнены по правому краю".
QWidget* setting_row(UiKey key, AppLanguage language, QWidget* control, int control_width = 220)
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
    // A right inset on purpose: the controls used to sit flush against the window
    // edge, where the right border was cut off (reported from the running build).
    // Alexander asked for a small gap rather than a flush edge.
    // Win11 rows are about 52 px tall: 32 px of control plus this padding.
    layout->setContentsMargins(4, 6, kRowRightInset, 6);
    layout->setSpacing(12);
    auto* text = new QLabel(ui_text(key, language), row);
    // The key is remembered on the widget: that is what lets a language change re-letter
    // the whole window in place instead of destroying and rebuilding it (Alexander:
    // "хотелось бы, чтобы без закрытия окна просто менялись все строки").
    text->setProperty("uiKey", static_cast<int>(key));
    text->setWordWrap(true);
    // No alignment flag on the label: an aligned item is laid out at its size hint,
    // which for a wrapping label is one line, so the second line was clipped no
    // matter how tall the row was allowed to become (reported from the running
    // build, and caught here: "Вставлять автоматически" needed 32 px and had 22).
    //
    // The split is proportional, not fixed: the label keeps about four fifths of the
    // row and the value the rest (Alexander's sketch), at any window width, and the
    // row can never demand more than it has - which is what made pages wider than
    // the viewport and cut the right border off every control.
    if (control == nullptr) {
        // A heading row: the control is the widget added on the next line.
        layout->addWidget(text, 1);
        return row;
    }
    const bool has_value_column = control_width > 0 && !control->inherits("voicetyper::app::ToggleSwitch");
    layout->addWidget(text, has_value_column ? 4 : 1);
    layout->addWidget(control, has_value_column ? 1 : 0);
    return row;
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

/// Icons are drawn, not taken from a glyph font.
///
/// The .NET build used Segoe MDL2 Assets, and the port copied its code points - but those
/// glyphs do not render on this machine (the same finding is recorded on the title bar
/// buttons below). Geometry cannot depend on a font, so the navigation icons, the model
/// buttons and the record button are painted from primitives.
enum class DrawnIcon { general, appearance, models, hotkeys, microphone, startup, log, about,
    download, remove, record, stop };

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
    case DrawnIcon::stop: // a rounded square
        painter.setBrush(colour);
        painter.drawRoundedRect(box.adjusted(w * 0.16, h * 0.16, -w * 0.16, -h * 0.16), 2.0, 2.0);
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
    // A pixmap of a fixed size: multiplying the size by devicePixelRatio produced a null
    // pixmap (and therefore an empty icon) when that ratio came back as zero, which is why
    // the navigation icons never appeared while the same drawing worked on buttons.
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    if (qApp != nullptr && qApp->devicePixelRatio() > 0.0) {
        pixmap.setDevicePixelRatio(qApp->devicePixelRatio());
    }
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal inset = size * 0.16;
    paint_drawn_icon(painter, icon, QRectF(inset, inset, size - 2 * inset, size - 2 * inset), colour);
    return QIcon(pixmap);
}


} // namespace

void MainWindow::build_tabs()
{
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // --- title bar (MainWindow.axaml:137-150) --------------------------------
    auto* title_bar = new QWidget(central);
    title_bar->setObjectName(QStringLiteral("titleBar"));
    title_bar->setFixedHeight(32);
    auto* title_layout = new QHBoxLayout(title_bar);
    title_layout->setContentsMargins(12, 0, 0, 0);
    title_layout->setSpacing(0);
    auto* title_text = new QLabel(QStringLiteral("VoiceTyper"), title_bar);
    title_text->setObjectName(QStringLiteral("titleText"));
    QFont title_font = navigation_font(13);
    title_font.setWeight(QFont::DemiBold);
    title_text->setFont(title_font);
    title_layout->addWidget(title_text);
    title_layout->addStretch(1);
    // The window buttons must use the icon font explicitly. Relying on a font
    // family list made Qt pick the glyph out of a fallback font, and the shapes it
    // produced were not the minimize/close buttons the .NET window shows - reported
    // from the running build. Without the icon font the text fallback is used, so
    // the buttons are still legible on a machine that has no MDL2 at all.
    const auto add_title_button = [&](const char* glyph, const char* object_name, bool is_close) {
        auto* button = new QPushButton(title_bar);
        button->setObjectName(QString::fromUtf8(object_name));
        static_cast<void>(glyph);
        // MainWindow.axaml:73-83: Width 46, Height 31, Padding 0, content centred,
        // no corner radius. The symbol itself is drawn as geometry: the Segoe MDL2
        // Assets glyphs E921/E8BB did not render as the window buttons of the .NET
        // build on this machine, and a drawn line/cross cannot depend on a font.
        button->setFixedSize(46, 31);
        button->setIconSize(QSize(10, 10));
        title_button_icons_.emplace_back(button, is_close);
        button->setFlat(true);
        button->setCursor(Qt::ArrowCursor);
        button->setFocusPolicy(Qt::NoFocus);
        if (is_close) {
            // The tray owns shutdown (compatibility-contracts.md §2): closing the
            // window hides it, exactly like the .NET build.
            connect(button, &QPushButton::clicked, this, &QWidget::hide);
        } else {
            connect(button, &QPushButton::clicked, this, &QWidget::showMinimized);
        }
        title_layout->addWidget(button);
    };
    add_title_button("\uE921", "titleBarMinimize", false);
    add_title_button("\uE8BB", "titleBarClose", true);
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
    sidebar->setFixedWidth(240);
    sidebar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
    sidebar->setMinimumSize(210, 1);
    auto* sidebar_layout = new QVBoxLayout(sidebar);
    sidebar_layout->setContentsMargins(10, 14, 10, 10);
    sidebar_layout->setSpacing(0);

    auto* brand = new QWidget(sidebar);
    auto* brand_layout = new QHBoxLayout(brand);
    brand_layout->setContentsMargins(12, 0, 12, 18);
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
    QFont brand_font = navigation_font(17);
    brand_font.setBold(true);
    brand_name->setFont(brand_font);
    auto* brand_tagline = new QLabel(ui_text(UiKey::k47, language_), brand_text);
    brand_tagline->setObjectName(QStringLiteral("brandTagline"));
    brand_tagline->setFont(navigation_font(11));
    brand_text_layout->addWidget(brand_name);
    brand_text_layout->addWidget(brand_tagline);
    brand_layout->addWidget(brand_text, 1);
    sidebar_layout->addWidget(brand);

    nav_ = new QListWidget(sidebar);
    nav_->setObjectName(QStringLiteral("sideNav"));
    nav_->setFrameShape(QFrame::NoFrame);
    nav_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    nav_->setFont(navigation_font(14));
    nav_->setFocusPolicy(Qt::NoFocus);
    // The sidebar is a `*` row as well: its list must not force a minimum height
    // that pushes the footer out of the 640 px window.
    nav_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    nav_->setMinimumSize(0, 1);
    sidebar_layout->addWidget(nav_, 1);
    body->addWidget(sidebar);

    // The page title lives in its own band above the pages, like the reference
    // ("User Activities" on a tinted strip), instead of scrolling away with the rows.
    page_header_ = new QWidget(central);
    page_header_->setObjectName(QStringLiteral("pageHeader"));
    page_header_->setAttribute(Qt::WA_StyledBackground, true);
    auto* header_layout = new QHBoxLayout(page_header_);
    header_layout->setContentsMargins(31, 0, 24, 0);
    page_title_ = new QLabel(page_header_);
    page_title_->setObjectName(QStringLiteral("pageTitle"));
    header_layout->addWidget(page_title_);
    header_layout->addStretch(1);
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
    footer->setFixedHeight(44);
    footer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* footer_layout = new QHBoxLayout(footer);
    // The right margin matches the content column (10 px) so the record button lines
    // up with the fields above it instead of sitting 10 px further left.
    footer_layout->setContentsMargins(18, 8, 10, 12);
    footer_layout->setSpacing(12);
    status_ = new QLabel(footer);
    status_->setObjectName(QStringLiteral("footerStatus"));
    status_->setFont(navigation_font(12));
    record_button_ = new QPushButton(footer);
    record_button_->setObjectName(QStringLiteral("recordButton"));
    record_button_->setProperty("buttonRole", QStringLiteral("record"));
    record_button_->setIconSize(QSize(18, 18));
    record_button_->setMinimumWidth(46);
    connect(record_button_, &QPushButton::clicked, this, &MainWindow::on_recording_toggled);
    engine_state_ = new QLabel(footer);
    hotkey_state_ = new QLabel(footer);
    footer_layout->addWidget(status_, 1);
    footer_layout->addWidget(engine_state_);
    footer_layout->addWidget(hotkey_state_);
    footer_layout->addWidget(record_button_);
    // The footer goes into QMainWindow's status-bar row rather than a row of the
    // central layout: the status bar is laid out by QMainWindow itself, so no
    // page's minimum height can push it off the bottom of the window (which is
    // exactly what happened with a plain QVBoxLayout row).
    statusBar()->addWidget(footer, 1);
    statusBar()->setSizeGripEnabled(false);
    statusBar()->show();

    auto* grip = new QSizeGrip(footer);
    grip->setObjectName(QStringLiteral("footerGrip"));
    footer_layout->addWidget(grip, 0, Qt::AlignBottom | Qt::AlignRight);

    // --- Общие -------------------------------------------------------------
    {
        auto* page = new QWidget(central);
        auto* rows = new QVBoxLayout(page);
        rows->setContentsMargins(0, 0, 0, 0);
        rows->setSpacing(0);
        app_language_ = new QComboBox(page);
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
        rows->addWidget(setting_row(UiKey::k44, language_, app_language_));
        rows->addWidget(setting_row(UiKey::k45, language_, recognition_language_));
        rows->addWidget(setting_row(UiKey::k29, language_, recording_mode_));
        rows->addWidget(setting_row(UiKey::k6, language_, auto_paste_, 0));
        rows->addWidget(setting_row(UiKey::k40, language_, condition_on_previous_text_, 0));
        rows->addWidget(setting_row(UiKey::k28, language_, silence_threshold_));
        add_page_to_nav(page, ui_text(UiKey::k23, language_));
    }

    // --- Модели ------------------------------------------------------------
    {
        auto* page = new QWidget(central);
        auto* rows = new QVBoxLayout(page);
        rows->setContentsMargins(0, 0, 0, 0);
        rows->setSpacing(0);
        engine_ = new QComboBox(page);
        engine_->setObjectName(QStringLiteral("engineCombo"));
        engine_->addItem(QStringLiteral("Whisper"), static_cast<int>(TranscriptionEngine::whisper));
        engine_->addItem(QStringLiteral("Parakeet"), static_cast<int>(TranscriptionEngine::parakeet));
        whisper_size_ = new QComboBox(page);
        whisper_size_->setObjectName(QStringLiteral("whisperSizeCombo"));
        for (const auto size : {ModelSize::tiny, ModelSize::base, ModelSize::small, ModelSize::medium, ModelSize::large}) {
            whisper_size_->addItem(model_size_text(size), static_cast<int>(size));
        }
        parakeet_size_ = new QComboBox(page);
        parakeet_size_->setObjectName(QStringLiteral("parakeetSizeCombo"));
        for (const auto size : {ParakeetModelSize::q4k, ParakeetModelSize::q5k, ParakeetModelSize::q6k, ParakeetModelSize::q8_0}) {
            parakeet_size_->addItem(parakeet_size_text(size), static_cast<int>(size));
        }
        temperature_ = new QDoubleSpinBox(page);
        temperature_->setRange(0.0, 1.0);
        temperature_->setSingleStep(0.05);
        best_of_ = new QSpinBox(page);
        best_of_->setRange(1, 8);
        // The terms dictionary is a multi-line editor under its own label, full width:
        // the user has to see all terms at once and add more easily (Alexander,
        // 2026-10-06). Three lines minimum, grown by the layout as needed.
        terms_ = new QPlainTextEdit(page);
        terms_->setObjectName(QStringLiteral("termsEdit"));
        terms_->setMinimumHeight(terms_->fontMetrics().lineSpacing() * 3 + 16);
        terms_->setTabChangesFocus(true);
        engine_->setMaximumWidth(220);
        rows->addWidget(setting_row(UiKey::k7, language_, engine_));
        // The hint is copy from the .NET page (Models_Engine_Hint): it explains the
        // trade-off the list below then quantifies.
        auto* engine_hint = new QLabel(ui_text(UiKey::k104, language_), page);
        engine_hint->setObjectName(QStringLiteral("engineHint"));
        engine_hint->setProperty("uiKey", static_cast<int>(UiKey::k104));
        engine_hint->setWordWrap(true);
        // A full-width wrapping row, not a right-aligned one: a long hint inside
        // setting_row demanded its whole single-line width, and the page then
        // became wider than the scroll viewport - the right edge of every control
        // on the page was cut off (caught by the layout test on Windows).
        auto* hint_row = new QWidget(page);
        hint_row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        auto* hint_layout = new QHBoxLayout(hint_row);
        hint_layout->setContentsMargins(4, 6, kRowRightInset, 12);
        hint_layout->addWidget(engine_hint, 1);
        rows->addWidget(hint_row);

        // The .NET page picks a model from a list that carries its size, speed,
        // quality and description, so "which model" is answerable without knowing
        // the product. The two size combos stay as hidden state holders, because
        // the presenter reads one control per engine.
        whisper_size_->hide();
        parakeet_size_->hide();


        
        const auto build_model_card = [&](bool whisper) -> QWidget* {
            const ModelRow* specs = whisper ? kWhisperModelRows : kParakeetModelRows;
            const std::size_t count =
                whisper ? std::size(kWhisperModelRows) : std::size(kParakeetModelRows);
            auto* card = new QWidget(page);
            card->setObjectName(whisper ? QStringLiteral("whisperModelsCard")
                                        : QStringLiteral("parakeetModelsCard"));
            auto* card_layout = new QVBoxLayout(card);
            card_layout->setContentsMargins(0, 0, 0, 0);
            card_layout->setSpacing(0);
            for (std::size_t index = 0; index < count; ++index) {
                const ModelRow& spec = specs[index];
                auto* row = new QWidget(card);
                row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
                auto* row_layout = new QHBoxLayout(row);
                row_layout->setContentsMargins(4, 8, kRowRightInset, 8);
                row_layout->setSpacing(12);
                auto* toggle = new ToggleSwitch(row);
                toggle->setObjectName(whisper ? QStringLiteral("whisperModelToggle%1").arg(index)
                                              : QStringLiteral("parakeetModelToggle%1").arg(index));
                row_layout->addWidget(toggle, 0, Qt::AlignTop);
                auto* text_box = new QWidget(row);
                auto* text_layout = new QVBoxLayout(text_box);
                text_layout->setContentsMargins(0, 0, 0, 0);
                text_layout->setSpacing(2);
                auto* name = new QLabel(QString::fromUtf8(spec.name), text_box);
                QFont name_font = name->font();
                name_font.setWeight(QFont::DemiBold);
                name->setFont(name_font);
                auto* meta = new QLabel(
                    ui_text(UiKey::k2, language_)
                        .arg(QString::fromUtf8(model_field(spec.size_ru, spec.size_en, language_)),
                            QString::fromUtf8(model_field(spec.speed_ru, spec.speed_en, language_)),
                            QString::fromUtf8(model_field(spec.quality_ru, spec.quality_en, language_))),
                    text_box);
                meta->setObjectName(QStringLiteral("mutedLabel"));
                // Wrapped, so the row's minimum is its longest word: otherwise the
                // meta line alone forced the whole page wider than the viewport.
                meta->setWordWrap(true);
                auto* description = new QLabel(
                    QString::fromUtf8(model_field(spec.description_ru, spec.description_en, language_)),
                    text_box);
                description->setObjectName(QStringLiteral("mutedLabel"));
                description->setWordWrap(true);
                text_layout->addWidget(name);
                text_layout->addWidget(meta);
                text_layout->addWidget(description);
                // Registered so a language change re-letters the card in place, like the
                // rest of the window.
                model_card_labels_.push_back(
                    ModelCardLabels{name, meta, description, whisper, static_cast<int>(index)});
                row_layout->addWidget(text_box, 1);
                auto* download = new QPushButton(row);
                download->setProperty("buttonRole", QStringLiteral("download"));
                download->setIconSize(QSize(18, 18));
                download->setFixedWidth(46);
                download->setObjectName(whisper ? QStringLiteral("whisperModelButton%1").arg(index)
                                                : QStringLiteral("parakeetModelButton%1").arg(index));
                row_layout->addWidget(download, 0, Qt::AlignTop);
                // The label depends on whether the model is on disk: a downloaded model
                // must offer "Удалить", not a pointless "Скачать" (reported from the
                // running build, about the models that were already working).
                connect(download, &QPushButton::clicked, this, [this, whisper, index] {
                    if (!services_.model_delete) {
                        return;
                    }
                    if (services_.model_delete(whisper, static_cast<int>(index))) {
                        refresh_model_buttons();
                    }
                });
                (whisper ? whisper_model_buttons_ : parakeet_model_buttons_).push_back(download);
                connect(toggle, &QAbstractButton::clicked, this, [this, whisper, index] {
                    select_model(whisper, static_cast<int>(index));
                });
                (whisper ? whisper_model_toggles_ : parakeet_model_toggles_).push_back(toggle);
                card_layout->addWidget(row);
            }
            return card;
        };
        whisper_models_card_ = build_model_card(true);
        parakeet_models_card_ = build_model_card(false);
        rows->addWidget(whisper_models_card_);
        rows->addWidget(parakeet_models_card_);

        connect(engine_, &QComboBox::currentIndexChanged, this, [this](int) { refresh_model_list(); });
        connect(whisper_size_, &QComboBox::currentIndexChanged, this, [this](int) { refresh_model_list(); });
        connect(parakeet_size_, &QComboBox::currentIndexChanged, this, [this](int) { refresh_model_list(); });
        rows->addWidget(setting_row(UiKey::k36, language_, temperature_));
        best_of_->setToolTip(QStringLiteral("Сколько вариантов распознавания движок сравнивает между собой. "
            "Больше — точнее, но медленнее"));
        rows->addWidget(setting_row(UiKey::k55, language_, best_of_));
        rows->addWidget(setting_row(UiKey::k33, language_, nullptr));
        rows->addWidget(terms_);
        add_page_to_nav(page, ui_text(UiKey::k18, language_));
    }

    // --- Хоткеи ------------------------------------------------------------
    {
        auto* page = new QWidget(central);
        auto* rows = new QVBoxLayout(page);
        rows->setContentsMargins(0, 0, 0, 0);
        rows->setSpacing(0);
        record_hotkey_ = new QLineEdit(page);
        record_hotkey_->setObjectName(QStringLiteral("recordHotkeyEdit"));
        cancel_hotkey_ = new QLineEdit(page);
        // The capture button is the only workable way to set a combination: the
        // user presses it, then presses the keys. Disabled with a reason when the
        // platform offers no hook.
        record_hotkey_capture_ = new QPushButton(ui_text(UiKey::k11, language_), page);
        record_hotkey_capture_->setObjectName(QStringLiteral("recordHotkeyCapture"));
        connect(record_hotkey_capture_, &QPushButton::clicked, this, [this] { capture_hotkey_into(true); });
        cancel_hotkey_capture_ = new QPushButton(ui_text(UiKey::k11, language_), page);
        cancel_hotkey_capture_->setObjectName(QStringLiteral("cancelHotkeyCapture"));
        connect(cancel_hotkey_capture_, &QPushButton::clicked, this, [this] { capture_hotkey_into(false); });

        auto* record_row = new QWidget(page);
        record_row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        auto* record_layout = new QHBoxLayout(record_row);
        record_layout->setContentsMargins(4, 6, kRowRightInset, 6);
        record_layout->setSpacing(8);
        auto* record_label = new QLabel(ui_text(UiKey::k12, language_), record_row);
        record_layout->addWidget(record_label, 1, Qt::AlignLeft | Qt::AlignVCenter);
        record_hotkey_->setMaximumWidth(240);
        record_hotkey_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        record_layout->addWidget(record_hotkey_, 0, Qt::AlignRight | Qt::AlignVCenter);
        record_layout->addWidget(record_hotkey_capture_, 0, Qt::AlignRight | Qt::AlignVCenter);
        rows->addWidget(record_row);

        auto* cancel_row = new QWidget(page);
        cancel_row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        auto* cancel_layout = new QHBoxLayout(cancel_row);
        cancel_layout->setContentsMargins(4, 6, kRowRightInset, 6);
        cancel_layout->setSpacing(8);
        auto* cancel_label = new QLabel(ui_text(UiKey::k26, language_), cancel_row);
        cancel_layout->addWidget(cancel_label, 1, Qt::AlignLeft | Qt::AlignVCenter);
        cancel_hotkey_->setMaximumWidth(240);
        cancel_hotkey_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        cancel_layout->addWidget(cancel_hotkey_, 0, Qt::AlignRight | Qt::AlignVCenter);
        cancel_layout->addWidget(cancel_hotkey_capture_, 0, Qt::AlignRight | Qt::AlignVCenter);
        rows->addWidget(cancel_row);

        const bool can_capture = static_cast<bool>(services_.capture_hotkey);
        for (auto* button : {record_hotkey_capture_, cancel_hotkey_capture_}) {
            button->setEnabled(can_capture);
            button->setToolTip(can_capture
                    ? ui_text(UiKey::k20, language_)
                    : ui_text(UiKey::k19, language_));
        }
        add_page_to_nav(page, ui_text(UiKey::k42, language_));
    }

    // --- Микрофон ----------------------------------------------------------
    {
        auto* page = new QWidget(central);
        auto* rows = new QVBoxLayout(page);
        rows->setContentsMargins(0, 0, 0, 0);
        rows->setSpacing(0);
        microphone_ = new QComboBox(page);
        microphone_->setObjectName(QStringLiteral("microphoneCombo"));
        // A combo box sizes itself to its longest item, and a microphone name here
        // is "Набор микрофонов (Технология Intel® Smart Sound для цифровых
        // микрофонов)" - the widget demanded 517 px, so the page grew wider than the
        // scroll viewport and its whole content was cut off (caught by the layout
        // test on Windows, at the minimum window size). Elide the text instead.
        microphone_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        microphone_->setMinimumContentsLength(12);
        rows->addWidget(setting_row(UiKey::k39, language_, microphone_));
        // Noise reduction belongs with the microphone: it describes how the input is
        // recorded, not how the engine transcribes (Alexander, 2026-10-06).
        // Sensitivity: the very value the Windows Sound panel edits, so the slider and the
        // system never disagree. Disabled (with the reason) where the platform has no level
        // control, because a slider that writes nowhere is worse than no slider.
        microphone_level_ = new QSlider(Qt::Horizontal, page);
        microphone_level_->setObjectName(QStringLiteral("microphoneLevelSlider"));
        microphone_level_->setRange(0, 100);
        microphone_level_->setMinimumWidth(160);
        microphone_level_value_ = new QLabel(page);
        microphone_level_value_->setObjectName(QStringLiteral("microphoneLevelValue"));
        auto* level_box = new QWidget(page);
        auto* level_layout = new QHBoxLayout(level_box);
        level_layout->setContentsMargins(0, 0, 0, 0);
        level_layout->setSpacing(8);
        level_layout->addWidget(microphone_level_, 1);
        level_layout->addWidget(microphone_level_value_);
        rows->addWidget(setting_row(UiKey::k57, language_, level_box));

        // The test the .NET build did not have: it told the user to check the level and
        // left them to the control panel.
        microphone_test_ = new QPushButton(ui_text(UiKey::k58, language_), page);
        microphone_test_->setObjectName(QStringLiteral("microphoneTestButton"));
        microphone_test_->setProperty("uiKey", static_cast<int>(UiKey::k58));
        rows->addWidget(setting_row(UiKey::k58, language_, microphone_test_));
        microphone_level_meter_ = new QProgressBar(page);
        microphone_level_meter_->setObjectName(QStringLiteral("microphoneLevelMeter"));
        microphone_level_meter_->setRange(0, 100);
        microphone_level_meter_->setTextVisible(false);
        microphone_test_result_ = new QLabel(page);
        microphone_test_result_->setObjectName(QStringLiteral("microphoneTestResult"));
        microphone_test_result_->setWordWrap(true);
        rows->addWidget(microphone_level_meter_);
        rows->addWidget(microphone_test_result_);
        // Noise reduction belongs with the microphone: it describes how the input is
        // recorded, not how the engine transcribes (Alexander, 2026-10-06).
        rows->addWidget(setting_row(UiKey::k43, language_, noise_reduction_, 0));
        add_page_to_nav(page, ui_text(UiKey::k17, language_));
    }

    // --- Запуск ------------------------------------------------------------
    {
        auto* page = new QWidget(central);
        auto* rows = new QVBoxLayout(page);
        rows->setContentsMargins(0, 0, 0, 0);
        rows->setSpacing(0);
        start_with_windows_ = new ToggleSwitch(page);
        start_minimized_ = new ToggleSwitch(page);
        rows->addWidget(setting_row(UiKey::k14, language_, start_with_windows_, 0));
        rows->addWidget(setting_row(UiKey::k15, language_, start_minimized_, 0));
        add_page_to_nav(page, ui_text(UiKey::k13, language_));
    }

    // --- Внешний вид -------------------------------------------------------
    {
        auto* page = new QWidget(central);
        auto* rows = new QVBoxLayout(page);
        rows->setContentsMargins(0, 0, 0, 0);
        rows->setSpacing(0);
        theme_ = new QComboBox(page);
        theme_->setObjectName(QStringLiteral("themeCombo"));
        for (const auto theme : {AppTheme::light, AppTheme::dark, AppTheme::system}) {
            theme_->addItem(theme_text(theme, language_), static_cast<int>(theme));
        }
        rows->addWidget(setting_row(UiKey::k34, language_, theme_));
        // Hiding on focus loss is a window behaviour, so it lives with the appearance
        // (Alexander, 2026-10-06), not with the general settings.
        rows->addWidget(setting_row(UiKey::k32, language_, hide_on_focus_loss_, 0));
        add_page_to_nav(page, ui_text(UiKey::k5, language_));
    }

    // --- Журнал ------------------------------------------------------------
    {
        auto* page = new QWidget(central);
        auto* box = new QVBoxLayout(page);
        log_view_ = new QTextEdit(page);
        log_view_->setObjectName(QStringLiteral("logView"));
        log_view_->setReadOnly(true);
        // The log is the one thing on its page that should be tall: the trailing
        // stretch that keeps the other pages top-aligned would otherwise squeeze it
        // into a couple of lines.
        log_view_->setMinimumHeight(320);
        box->addWidget(log_view_);
        add_page_to_nav(page, ui_text(UiKey::k9, language_));
    }

    // --- О программе -------------------------------------------------------
    {
        auto* page = new QWidget(central);
        auto* box = new QVBoxLayout(page);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(0);
        auto* about = new QLabel(ui_text(UiKey::k74, language_) + QLatin1Char('\n')
                + ui_text(UiKey::k75, language_), page);
        about->setWordWrap(true);
        box->addWidget(about);

        // The running version, then the update controls: the .NET build checked the
        // release feed quietly on start and offered the installer on this page.
        update_version_ = new QLabel(page);
        update_version_->setObjectName(QStringLiteral("updateVersion"));
        update_version_->setText(QStringLiteral("—"));
        auto* version_row = new QWidget(page);
        auto* version_layout = new QHBoxLayout(version_row);
        version_layout->setContentsMargins(0, 0, 0, 0);
        version_layout->addWidget(update_version_, 1, Qt::AlignRight);
        box->addWidget(setting_row(UiKey::k63, language_, version_row));

        update_status_ = new QLabel(ui_text(UiKey::k73, language_), page);
        update_status_->setObjectName(QStringLiteral("updateStatus"));
        update_status_->setWordWrap(true);
        update_check_ = new QPushButton(ui_text(UiKey::k64, language_), page);
        update_check_->setObjectName(QStringLiteral("updateCheckButton"));
        update_check_->setProperty("uiKey", static_cast<int>(UiKey::k64));
        box->addWidget(setting_row_control(update_status_, update_check_));

        update_notes_ = new QLabel(page);
        update_notes_->setObjectName(QStringLiteral("updateNotes"));
        update_notes_->setWordWrap(true);
        update_notes_->hide();
        box->addWidget(update_notes_);

        update_install_ = new QPushButton(ui_text(UiKey::k68, language_), page);
        update_install_->setObjectName(QStringLiteral("updateInstallButton"));
        update_install_->setProperty("uiKey", static_cast<int>(UiKey::k68));
        update_install_->hide();
        box->addWidget(setting_row(UiKey::k76, language_, update_install_));

        update_progress_ = new QProgressBar(page);
        update_progress_->setObjectName(QStringLiteral("updateProgress"));
        update_progress_->setRange(0, 100);
        update_progress_->hide();
        box->addWidget(update_progress_);

        box->addStretch(1);
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
        }
    });
    connect(pages_, &QStackedWidget::currentChanged, this, [this](int index) {
        refresh_page_title(index);
    });
    refresh_page_title(nav_->currentRow());
    setCentralWidget(central);
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
            [&label](const auto& pair) { return pair.first == label; });
        if (found == built_pages_.end()) {
            continue;
        }
        QWidget* page = found->second;

        auto* scroll = new QScrollArea(pages_);
        scroll->setObjectName(QStringLiteral("pageScroll_") + QString::fromUtf8(entry.key));
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
        auto* outer_layout = new QVBoxLayout(outer);
        outer_layout->setContentsMargins(0, 0, 0, 0);
        outer_layout->setSpacing(0);
        outer_layout->addWidget(page, 1);
        scroll->setWidget(outer);
        scroll->setProperty("pageWidget", QVariant::fromValue(static_cast<QObject*>(page)));
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
        // A fixed row height: the .NET template uses Margin="14,10" around a
        // 14 px line, so every entry is the same size whatever the font metrics.
        item->setSizeHint(QSize(0, 30));
    }
    // The theme may have been applied before the menu existed, so the icons are drawn
    // here as well as on every theme change.
    refresh_navigation_icons();

    // A page that is not in the navigation table would otherwise be unreachable.
    for (const auto& pair : built_pages_) {
        const bool known = std::any_of(kNavigation, kNavigation + std::size(kNavigation),
            [&pair](const NavEntry& entry) { return pair.first == QString::fromUtf8(entry.label); });
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
            if (ui_text(entry.text, language_) == pair.first) {
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
        added->setSizeHint(QSize(0, 30));
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
    field->setReadOnly(true);
    const QString previous_placeholder = field->placeholderText();
    field->setPlaceholderText(ui_text(UiKey::k21, language_));
    set_status_message(ui_text(UiKey::k24, language_));

    services_.capture_hotkey([this, field, button, previous_placeholder](
                                 std::optional<std::string> gesture, QString error) {
        button->setEnabled(true);
        field->setReadOnly(false);
        field->setPlaceholderText(previous_placeholder);
        if (!gesture.has_value()) {
            // Cancelled (Escape) is not an error and must not leave a message.
            set_status_message(error.isEmpty() ? QString() : error);
            return;
        }
        // Assigning fires textChanged, which goes through the presenter and the
        // autosave tick, so the combination is stored and re-registered.
        field->setText(QString::fromStdString(*gesture));
        set_status_message(ui_text(UiKey::k16, language_).arg(QString::fromStdString(*gesture)));
    });
}

void MainWindow::select_model(bool whisper, int index)
{
    // The hidden combo carries the choice to the presenter, exactly as if the user
    // had picked it in the old combo box; the other toggles are cleared because a
    // model choice is exclusive.
    QComboBox* combo = whisper ? whisper_size_ : parakeet_size_;
    if (combo == nullptr || index < 0 || index >= combo->count()) {
        return;
    }
    combo->setCurrentIndex(index);
    refresh_model_list();
}

void MainWindow::refresh_model_list()
{
    const auto engine = static_cast<TranscriptionEngine>(engine_->currentData().toInt());
    if (whisper_models_card_ != nullptr) {
        whisper_models_card_->setVisible(engine == TranscriptionEngine::whisper);
    }
    if (parakeet_models_card_ != nullptr) {
        parakeet_models_card_->setVisible(engine == TranscriptionEngine::parakeet);
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
    if (whisper_size_ != nullptr) {
        sync(whisper_model_toggles_, whisper_size_->currentIndex());
    }
    if (parakeet_size_ != nullptr) {
        sync(parakeet_model_toggles_, parakeet_size_->currentIndex());
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
    select(theme_, static_cast<int>(settings.theme));
    temperature_->setValue(settings.temperature);
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
    connect(theme_, &QComboBox::currentIndexChanged, this, [this](int index) {
        presenter_.update(SettingsChange::appearance, [this, index](domain::AppSettings& settings) {
            settings.theme = static_cast<AppTheme>(theme_->itemData(index).toInt());
        });
        apply_theme();
    });
    connect(temperature_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        presenter_.update(SettingsChange::model, [value](domain::AppSettings& settings) { settings.temperature = value; });
    });
    // bestOf is not a persisted field: the recording state machine always asks for
    // 3 greedy candidates for the final result, so the control is read-only here
    // instead of pretending a setting the .NET app does not store.
    best_of_->setValue(3);
    best_of_->setEnabled(false);
    best_of_->setToolTip(ui_text(UiKey::k41, language_));
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

QWidget* MainWindow::setting_row_control(QWidget* status, QWidget* control)
{
    auto* row = new QWidget();
    row->setObjectName(QStringLiteral("settingsRow"));
    row->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(4, 6, kRowRightInset, 6);
    layout->setSpacing(12);
    row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    // The status takes the room the plain labels take; the action keeps its own column.
    layout->addWidget(status, 4);
    if (control != nullptr) {
        layout->addWidget(control, 0, Qt::AlignRight);
    }
    return row;
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
    for (auto* label : findChildren<QLabel*>()) {
        const QVariant key = label->property("uiKey");
        if (key.isValid()) {
            label->setText(ui_text(static_cast<UiKey>(key.toInt()), language_));
        }
        const QVariant text_key = label->property("uiTextKey");
        if (text_key.isValid()) {
            label->setText(ui_text(static_cast<UiKey>(text_key.toInt()), language_));
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
        for (int index = 0; index < nav_->count() && index < static_cast<int>(std::size(kNavigation)); ++index) {
            nav_->item(index)->setText(ui_text(kNavigation[index].text, language_));
        }
        refresh_page_title(nav_->currentRow());
    }
    if (silence_threshold_ != nullptr) {
        silence_threshold_->setSuffix(ui_text(UiKey::k0, language_));
    }
    if (theme_ != nullptr) {
        // The combo item texts are built from the same table, so they have to be rebuilt.
        const int current = theme_->currentIndex();
        theme_->setItemText(0, theme_text(AppTheme::light, language_));
        theme_->setItemText(1, theme_text(AppTheme::dark, language_));
        theme_->setItemText(2, theme_text(AppTheme::system, language_));
        theme_->setCurrentIndex(current);
    }
    // The model cards are built once, so their four texts are re-applied here.
    for (const auto& card : model_card_labels_) {
        if (card.name == nullptr || card.index < 0) {
            continue;
        }
        const ModelRow* rows = card.whisper ? kWhisperModelRows : kParakeetModelRows;
        const std::size_t count =
            card.whisper ? std::size(kWhisperModelRows) : std::size(kParakeetModelRows);
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
        update_version_->setText(services_.application_version());
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
                self->update_notes_->hide();
                self->update_install_->hide();
                self->update_progress_->hide();
                return;
            }
            self->update_status_->setText(ui_text(UiKey::k67, self->language_).arg(version));
            self->update_notes_->setText(notes);
            self->update_notes_->setVisible(!notes.isEmpty());
            self->update_install_->setVisible(true);
        });
    });

    if (services_.update_install) {
        connect(update_install_, &QPushButton::clicked, this, [this] {
            QPointer<MainWindow> self(this);
            update_install_->setEnabled(false);
            update_progress_->setRange(0, 0); // unknown until the first progress report
            update_progress_->show();
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
        microphone_level_meter_->setEnabled(false);
        return;
    }
    // The window can be rebuilt on a language change while the probe is running, so
    // the reply is guarded by a QPointer rather than a raw this.
    connect(microphone_test_, &QPushButton::clicked, this, [this] {
        QPointer<MainWindow> self(this);
        microphone_test_->setEnabled(false);
        microphone_test_result_->setText(ui_text(UiKey::k59, language_));
        services_.microphone_probe([self](bool heard, double peak, QString detail) {
            if (self == nullptr) {
                return;
            }
            self->microphone_test_->setEnabled(true);
            self->microphone_level_meter_->setValue(
                static_cast<int>(std::lround(std::clamp(peak, 0.0, 1.0) * 100.0)));
            QString verdict = ui_text(heard ? UiKey::k61 : UiKey::k62, self->language_);
            if (!detail.isEmpty()) {
                verdict += QStringLiteral(" (") + detail + QStringLiteral(")");
            }
            self->microphone_test_result_->setText(verdict);
        });
    });
}

void MainWindow::refresh_model_buttons()
{
    // "Скачать" is disabled with the reason, because the model download service is
    // not written yet; "Удалить" is live as soon as the file exists.
    const auto update = [this](const std::vector<QPushButton*>& buttons, bool whisper) {
        for (std::size_t index = 0; index < buttons.size(); ++index) {
            QPushButton* button = buttons[index];
            const bool downloaded = services_.model_is_downloaded
                && services_.model_is_downloaded(whisper, static_cast<int>(index));
            if (downloaded) {
                button->setIcon(drawn_icon(DrawnIcon::remove, 18, QColor(QStringLiteral("#E5484D"))));
                button->setToolTip(ui_text(UiKey::k37, language_));
                button->setProperty("buttonRole", QStringLiteral("remove"));
                button->setEnabled(static_cast<bool>(services_.model_delete));
                button->setToolTip(ui_text(UiKey::k38, language_));
            } else {
                button->setIcon(drawn_icon(DrawnIcon::download, 18, QColor(QStringLiteral("#4FD1A5"))));
                button->setToolTip(ui_text(UiKey::k31, language_));
                button->setProperty("buttonRole", QStringLiteral("download"));
                button->setEnabled(false);
                button->setToolTip(QStringLiteral(
                    "Загрузка моделей появится вместе с сервисом обновлений; сейчас файл модели кладётся в папку моделей вручную"));
            }
            // A dynamic property change only reaches the stylesheet after a repolish.
            button->style()->unpolish(button);
            button->style()->polish(button);
        }
    };
    update(whisper_model_buttons_, true);
    update(parakeet_model_buttons_, false);
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
        for (const auto& entry : kNavigation) {
            if (ui_text(entry.text, language_) == item->text()) {
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
    for (const auto& [button, is_close] : title_button_icons_) {
        QPixmap pixmap(10, 10);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QPen(colour, 1.0));
        if (is_close) {
            painter.drawLine(0, 0, 9, 9);
            painter.drawLine(9, 0, 0, 9);
        } else {
            painter.drawLine(0, 5, 9, 5);
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
    muted_text_ = QString::fromLatin1(p.muted);

    const QString qss = QStringLiteral(R"(
        QWidget {
            background: %1; color: %4;
            font-family: "Inter", "Segoe UI", sans-serif; font-size: 14px;
        }
        /* Title bar: flat, like the system window chrome. */
        #titleBar { background: %1; border-bottom: 1px solid %6; }
        #titleBar QLabel { background: transparent; font-size: 13px; }
        #titleBar QPushButton { background: transparent; border: none; color: %4; padding: 0px; border-radius: 0px; }
        #titleBar QPushButton:hover { background: %3; }
        #titleBarClose:hover { background: #C42B1C; color: #FFFFFF; }
        QPushButton#titleBarClose:hover { background-color: #C42B1C; }
        /* Navigation column: rounded items, an accent bar on the selected one. */
        #sidebar { background: %1; border-right: 1px solid %6; }
        #sideNav { background: transparent; border: none; outline: none; }
        /* Square, full-width items with a hairline between them, like the reference. */
        #sideNav::item {
            color: %4; padding: 6px 16px; margin: 0px; border-bottom: 1px solid %6; border-radius: 0px;
            font-weight: bold;
        }
        #sideNav::item:hover { background: %3; }
        #sideNav::item:selected { background: %8; color: %4; }
        #brandTagline { color: %5; font-size: 12px; }
        /* Page: a large heading, then one card of setting rows. */
        #sectionTitle { color: %4; font-size: 18px; font-weight: 600; letter-spacing: 0px; }
        /* The page title strip: a tinted band that does not scroll with the rows. */
        #pageHeader { background: %9; border-bottom: 1px solid %6; min-height: 56px; max-height: 56px; }
        #pageTitle { color: %4; font-size: 20px; font-weight: 600; background: transparent; }
        #card {
            background: %2; border: 1px solid %6; border-radius: 8px;
        }
        #card QLabel { background: transparent; }
        #settingsRow { background: transparent; border-bottom: 1px solid %6; }
        #settingsRow QLabel { color: %5; }
        #settingsRow QLabel { background: transparent; }
        #mutedLabel, #hintLabel { color: %5; font-size: 12px; }
        /* Controls: Win11 fields are flat, slightly rounded, and outlined. */
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
            background: %2; color: %4; border: 1px solid %6; border-radius: 4px;
            padding: 5px 8px; min-height: 24px;
        }
        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border: 1px solid %7; }
        /* The .NET NumberBox keeps its steppers inside the field; here they were drawn as
           two separate boxes next to it (Alexander, 06.10.2026). They now sit inside the
           field, separated by the same hairline as the border. */
        QSpinBox::up-button, QDoubleSpinBox::up-button {
            subcontrol-origin: border; subcontrol-position: top right;
            width: 18px; margin: 1px 1px 0px 0px; border: none; border-left: 1px solid %6;
            background: %2;
        }
        QSpinBox::down-button, QDoubleSpinBox::down-button {
            subcontrol-origin: border; subcontrol-position: bottom right;
            width: 18px; margin: 0px 1px 1px 0px; border: none; border-left: 1px solid %6;
            background: %2;
        }
        QSpinBox::up-button:hover, QSpinBox::down-button:hover,
        QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover { background: %3; }
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
            background: %2; color: %4; border: 1px solid %6; selection-background-color: %3;
            outline: none;
        }
        QPushButton {
            background: %3; color: %4; border: 1px solid %6; border-radius: 4px;
            padding: 5px 14px; min-height: 24px;
        }
        QPushButton:hover { background: %2; border: 1px solid %7; }
        QPushButton:pressed { background: %2; }
        QPushButton:disabled { color: %5; border: 1px solid %6; }
        /* Semantic buttons: the .NET page coloured them, and the port had them all grey.
           Sky green for download, red for delete, sky blue for the record button. */
        QPushButton[buttonRole="download"] { background: #16302A; border: 1px solid #4FD1A5; color: #4FD1A5; }
        QPushButton[buttonRole="download"]:hover { background: #1D3F36; }
        QPushButton[buttonRole="download"]:disabled { border: 1px solid #2C5F4E; color: #3E8F72; background: #12241F; }
        QPushButton[buttonRole="remove"] { background: #33191C; border: 1px solid #E5484D; color: #E5484D; }
        QPushButton[buttonRole="remove"]:hover { background: #432024; }
        QPushButton[buttonRole="record"] { background: #1B2C4A; border: 1px solid #4C8BF5; color: #DCE9FF; }
        QPushButton[buttonRole="record"]:hover { background: #22385E; }
        QPushButton[buttonRole="record"][recording="true"] { background: #4C8BF5; border: 1px solid #4C8BF5; color: #0B1622; }
        QCheckBox { color: %4; spacing: 8px; }
        QTextEdit { background: %2; color: %4; border: 1px solid %6; border-radius: 8px; }
        /* Footer: a quiet status strip with the record button. */
        #footer { background: %1; border-top: 1px solid %6; }
        #footer QLabel { color: %5; background: transparent; font-size: 12px; }
        QScrollBar:vertical { background: transparent; width: 12px; margin: 0px; }
        QScrollBar::handle:vertical { background: %6; border-radius: 6px; min-height: 28px; margin: 3px; }
        QScrollBar::handle:vertical:hover { background: %5; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }
        QScrollBar:horizontal { height: 0px; }
    )")
        .arg(QString::fromLatin1(p.window), QString::fromLatin1(p.control), QString::fromLatin1(p.hover),
             QString::fromLatin1(p.text), QString::fromLatin1(p.muted), QString::fromLatin1(p.border),
             QString::fromLatin1(p.accent), QString::fromLatin1(p.selection),
             QString::fromLatin1(p.header));
    qApp->setStyleSheet(qss);
    refresh_title_button_icons();
    for (auto* toggle : findChildren<ToggleSwitch*>()) {
        // Win11 toggle: accent when on, a grey track when off, white knob.
        toggle->set_colors(QColor(QString::fromLatin1(p.accent)), QColor(QString::fromLatin1(p.muted)),
            QColor(QString::fromLatin1(p.border)), QColor(QStringLiteral("#FFFFFF")));
    }
    refresh_nav_icons();
}

void MainWindow::refresh_status_summary()
{
    const auto& settings = presenter_.settings();
    engine_state_->setText(
        services_.engine_status
            ? services_.engine_status()
            : ui_text(UiKey::k46, language_));
    hotkey_state_->setText(
        services_.record_hotkey_state
            ? services_.record_hotkey_state()
            : ui_text(UiKey::k54, language_));
    status_->setText(
        ui_text(UiKey::k35, language_)
            .arg(theme_text(settings.theme, language_),
                 presenter_.dirty() ? ui_text(UiKey::k48, language_) : ui_text(UiKey::k52, language_),
                 recording_mode_->currentText()));
    const auto can_record = services_.start_recording != nullptr;
    record_button_->setEnabled(can_record);
    // An icon instead of a word: a filled dot while recording, a square to stop it. The
    // tooltip keeps the meaning explicit.
    record_button_->setIcon(drawn_icon(recording_ ? DrawnIcon::stop : DrawnIcon::record, 18,
        QColor(recording_ ? QStringLiteral("#0B1622") : QStringLiteral("#DCE9FF"))));
    record_button_->setToolTip(recording_ ? ui_text(UiKey::k25, language_) : ui_text(UiKey::k10, language_));
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
        // Rewriting the whole document on every tick put the view back at the top,
        // so the user could not read anything (reported from the running build).
        // Write only when the text really changed, and keep the reader where they
        // were - following the tail only when they were already at the end.
        if (text != log_view_->toPlainText()) {
            QScrollBar* bar = log_view_->verticalScrollBar();
            const int previous = bar->value();
            const bool was_at_end = previous >= bar->maximum() - 4;
            log_view_->setPlainText(text);
            bar->setValue(was_at_end ? bar->maximum() : qMin(previous, bar->maximum()));
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
    status_->setText(text);
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
