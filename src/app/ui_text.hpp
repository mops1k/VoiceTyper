#pragma once

#include "domain/settings.hpp"

#include <QString>

namespace voicetyper::app {

/// Every user-visible string of the settings window, in one place.
///
/// The UI used to be hard-coded Russian, so switching the interface language in the
/// settings did nothing at all (reported from the running build). The Russian column
/// is the product's source language; the English one follows
/// VoiceTyper.Core/Localization/Strings.en.resx where a key existed there.
enum class UiKey : int {
    k0,
    k1,
    k2,
    k3,
    k4,
    k5,
    k6,
    k7,
    k8,
    k9,
    k10,
    k11,
    k12,
    k13,
    k14,
    k15,
    k16,
    k17,
    k18,
    k19,
    k20,
    k21,
    k22,
    k23,
    k24,
    k25,
    k26,
    k27,
    k28,
    k29,
    k30,
    k31,
    k32,
    k33,
    k34,
    k35,
    k36,
    k37,
    k38,
    k39,
    k40,
    k41,
    k42,
    k43,
    k44,
    k45,
    k46,
    k47,
    k48,
    k49,
    k50,
    k51,
    k52,
    k53,
    k54,
    k55,
    k56,
    k57,
    k58,
    k59,
    k60,
    k61,
    k62,
    k63,
    k64,
    k65,
    k66,
    k67,
    k68,
    k69,
    k70,
    k71,
    k72,
    k73,
    k74,
    k75,
    k76,

    /// Оверлей.
    k77,
    k78,
    k79,
    /// Меню трея.
    k80,
    k81,
    k82,
    k83,
    /// Статусы в футере и сообщения движка.
    k84,
    k85,
    k86,
    k87,
    k88,
    k89,
    k90,
    k91,
    k92,
    k93,
    k94,
    k95,
    k96,
    k97,
    k98,
    /// Диалог удаления модели.
    k99,
    k100,
    /// Сообщения при неудачном старте.
    k101,
    k102,
    /// Отказ установки обновления.
    k103,
    /// Подсказка о движке на странице «Модели».
    k104,
    /// Загрузка моделей.
    k105,
    k106,
    /// Ход загрузки модели и её отмена.
    k107,
    k108,
    k109,
    /// Как работает словарь терминов: термины ищутся и по звучанию, пары — строгое правило.
    k110,
    /// Not a string: the number of keys. The table is checked against it at compile
    /// time, so a key without an entry (or an entry without a key - which silently
    /// shifts every later label, as happened once) cannot survive a build.
    k111,
    k112,
    k113,
    k114,
    k115,
    k116,
    k117,
    k118,
    k119,
    k120,
    k121,
    k122,
    k123,
    /// Страница «Запуск».
    k124,
    k125,
    k126,
    k127,
    k128,
    k129,
    /// Страница «Хоткеи».
    k130,
    k131,
    k132,
    k133,
    k134,
    k135,
    k136,
    k137,
    /// Страница «Журнал».
    k138,
    k139,
    k140,
    k141,
    k142,
    /// Страница «Микрофон» (макет Figma Make: карточки «Источник звука»/«Обработка звука»
    /// и блок .microphone-test).
    k143,
    k144,
    k145,
    k146,
    k147,
    k148,
    k149,
    k150,
    k151,
    k152,
    /// Страница «О программе» (HERO-карточка .about-hero, .privacy-note, .about-links).
    k153,
    k154,
    k155,
    k156,
    k157,
    k158,
    k159,
    k160,
    /// Страница «Внешний вид»: карточка «Тема приложения» с плитками-превью
    /// (.theme-options / .theme-preview), карточка «Интерфейс» и кнопка сброса
    /// оформления (.reset-button).
    k161,
    k162,
    k163,
    k164,
    k165,
    k166,
    k167,
    k168,
    k169,
    /// Общее с «Моделями»: бейдж активной строки (.model-badge).
    k170,
    /// Страница «Модели»: карточки «Движок и модели» и «Точность распознавания»
    /// с блоком словаря (.dictionary).
    k171,
    k172,
    k173,
    k174,
    k175,
    k176,
    k177,
    k178,
    k179,
    k180,
    k181,
    /// Деградация хоткея: KGlobalAccel без сигнала отпускания (только press).
    k182,
    /// Карточка «Геймпад» на странице хоткеев.
    k183,
    k184,
    k185,
    k186,
    k187,
    k188,
    /// Подтверждение захваченной кнопки геймпада.
    k189,
    kCount,
};

/// The text of `key` in `language`. An unknown language falls back to Russian, which
/// is what the product ships first.
/// The language the interface is currently in.
///
/// The composition sets it at startup and on every language change. It plays the role
/// CultureInfo.CurrentUICulture played in the .NET build: status strings are produced in
/// many lambdas that own no settings presenter, and threading the language through each
/// of them would mean a capture in every one - and a silent mistake in the first missed.
void set_current_language(domain::AppLanguage language);
[[nodiscard]] domain::AppLanguage current_language();

[[nodiscard]] QString ui_text(UiKey key, domain::AppLanguage language);

} // namespace voicetyper::app
