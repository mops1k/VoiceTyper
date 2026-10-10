#pragma once

// Linux composition root entry point. Declared in its own header so main.cpp
// does not have to know how the composition is assembled.

namespace voicetyper::app {
/// Assembles the real Linux backends and runs the application.
int run(int argc, char** argv);
} // namespace voicetyper::app
