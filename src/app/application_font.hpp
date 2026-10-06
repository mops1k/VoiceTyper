#pragma once

namespace voicetyper::app {

/// Makes the bundled Inter family the application font.
///
/// Alexander asked for a nicer typeface than the system one (2026-10-06): Segoe UI
/// Variable is picked up by Qt as a variable font and its weights come out uneven, so
/// the UI type looks off. Inter is bundled as a Qt resource instead (SIL OFL, see
/// assets/fonts/LICENSE-Inter.txt), which also makes the Windows and Linux builds look
/// the same. A build without the resource keeps the system font.
void install_application_font();

} // namespace voicetyper::app
