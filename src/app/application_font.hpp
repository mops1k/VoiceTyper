#pragma once

#include <QString>

namespace voicetyper::app {

/// Makes the bundled Source Sans 3 family the application font and registers the
/// bundled icon family.
///
/// Alexander asked for a nicer typeface than the system one (2026-10-06): Segoe UI
/// Variable is picked up by Qt as a variable font and its weights come out uneven, so
/// the UI type looks off. Source Sans 3 is bundled as a Qt resource instead (SIL OFL
/// 1.1, see assets/fonts/LICENSE-SourceSans3.txt). It is the one typeface of the whole
/// window, Cyrillic included: Selawik, the previous choice, has no Cyrillic, so Russian
/// text fell through to the host fallback (Segoe UI on Windows, DejaVu on Linux) and
/// the layout metrics depended on the machine - measured 12.10.2026, replaced by
/// Alexander's decision. A build without the resource keeps the system font.
///
/// The navigation-glyph family (FluentSystemIcons, MIT) is registered in the same call,
/// so every entry point - both compositions, the snapshot tool and the tests - gets the
/// same icons; see icon_font_family().
void install_application_font();

/// The family of the bundled navigation-glyph font (FluentSystemIcons-Regular,
/// MIT, assets/fonts/FluentSystemIcons-Regular.ttf).
///
/// The navigation rows paint their glyphs with this family (NavigationIconDelegate in
/// main_window.cpp). Segoe MDL2 Assets was the original of that artwork, but it exists
/// on Windows only and cannot be redistributed, so the icons are shipped as a resource
/// and Windows and Linux draw the very same glyphs. QFont falls back to the system
/// family if the resource is missing, so the labels stay readable.
QString icon_font_family();

} // namespace voicetyper::app
