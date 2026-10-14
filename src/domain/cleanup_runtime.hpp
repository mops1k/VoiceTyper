#pragma once

#include <string>
#include <string_view>

namespace voicetyper::domain {

/// The local language-model runtime behind the optional cleaner (plan p_5d68d66503ca).
///
/// This is the seam that keeps the domain clean: the port and its checks know nothing about
/// llama.cpp, and the native library implements this interface. Everything the cleaner does is
/// best-effort, so the methods report failure by return value instead of throwing or aborting:
/// a runtime that cannot load a file, times out or refuses to answer just means "deliver what
/// the rules produced".
class CleanupRuntime {
public:
    virtual ~CleanupRuntime() = default;

    CleanupRuntime(const CleanupRuntime&) = delete;
    CleanupRuntime& operator=(const CleanupRuntime&) = delete;

    /// Makes the model at `path` ready. Called once per path: a second call with the same path
    /// must be cheap and must not reload the weights. On failure it returns false and fills
    /// `error` with a short reason for the log.
    [[nodiscard]] virtual bool load(const std::string& path, std::string& error) = 0;

    /// True when a model is ready and `clean` may be called.
    [[nodiscard]] virtual bool loaded() const = 0;

    /// Cleans one finished transcript. `prompt_appendix` is the user's addition to the prompt and
    /// may be empty: the base instruction is never replaced, only extended (Alexander,
    /// 14.10.2026), so a useless or contradictory addition costs quality, not correctness - the
    /// caller's checks are independent of the prompt and still stand. Returns false (with `error`
    /// filled) when the model could not answer at all; a returned answer may still be refused by
    /// the caller's checks.
    [[nodiscard]] virtual bool clean(std::string_view text, std::string_view prompt_appendix,
        std::string& cleaned, std::string& error) = 0;

    /// Releases the weights. Safe to call when nothing is loaded.
    virtual void unload() = 0;

protected:
    CleanupRuntime() = default;
};

} // namespace voicetyper::domain
