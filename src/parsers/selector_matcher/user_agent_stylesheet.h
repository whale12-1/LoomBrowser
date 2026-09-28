#pragma once

// Минимальный user-agent stylesheet — то, что в настоящем браузере
// включено по умолчанию. Регистрируется в каскаде с Origin::UserAgent,
// поэтому авторский CSS перекрывает его при любой возможности.
//
// Источник вдохновения: WebKit/Source/WebCore/css/html.css (упрощённый).

namespace ua_css {

	inline constexpr const char* kSource = R"CSS(
/* === Блочные элементы по умолчанию === */
html, body, div, p, h1, h2, h3, h4, h5, h6,
article, section, header, footer, nav, main, aside,
ul, ol, li, dl, dt, dd, pre, blockquote, figure, figcaption,
form, fieldset, hr, table, thead, tbody, tfoot, tr, address
{ display: block; }

/* === Inline-элементы по умолчанию === */
span, a, em, strong, b, i, u, s, code, small, sub, sup,
label, cite, q, abbr, time, br, img,
input, button, select, textarea
{ display: inline; }

/* === Базовые отступы body === */
body { margin: 8px; }

/* === Заголовки === */
h1 { font-size: 2em;    margin: 0.67em 0; font-weight: bold; }
h2 { font-size: 1.5em;  margin: 0.83em 0; font-weight: bold; }
h3 { font-size: 1.17em; margin: 1em    0; font-weight: bold; }
h4 { font-size: 1em;    margin: 1.33em 0; font-weight: bold; }
h5 { font-size: 0.83em; margin: 1.67em 0; font-weight: bold; }
h6 { font-size: 0.67em; margin: 2.33em 0; font-weight: bold; }

/* === Абзацы и списки === */
p     { margin: 1em 0; }
ul, ol { margin: 1em 0; padding-left: 40px; }
li    { margin: 0; }

/* === Цитаты и преформатированный текст === */
blockquote { margin: 1em 40px; }
pre        { margin: 1em 0; white-space: pre; }

/* === Прочее === */
hr { border-top-width: 1px; margin: 0.5em auto; }

/* === Инлайновые стилистики === */
b, strong { font-weight: bold; }
i, em     { font-style: italic; }
small     { font-size: 0.83em; }
sub, sup  { font-size: 0.83em; }
a         { text-decoration: underline; }
)CSS";

} // namespace ua_css