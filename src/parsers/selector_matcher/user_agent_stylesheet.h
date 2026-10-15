#pragma once

// ����������� user-agent stylesheet � ��, ��� � ��������� ��������
// �������� �� ���������. �������������� � ������� � Origin::UserAgent,
// ������� ��������� CSS ����������� ��� ��� ����� �����������.
//
// �������� �����������: WebKit/Source/WebCore/css/html.css (����������).

namespace ua_css {

	inline constexpr const char* kSource = R"CSS(
/* === ������� �������� �� ��������� === */
html, body, div, p, h1, h2, h3, h4, h5, h6,
article, section, header, footer, nav, main, aside,
ul, ol, li, dl, dt, dd, pre, blockquote, figure, figcaption,
form, fieldset, hr, table, thead, tbody, tfoot, tr, address
{ display: block; }

/* === Inline-�������� �� ��������� === */
span, a, em, strong, b, i, u, s, code, small, sub, sup,
label, cite, q, abbr, time, br, img,
input, button, select, textarea
{ display: inline; }

/* === ������� ������� body === */
body { margin: 8px; }

/* === ��������� === */
h1 { font-size: 2em;    margin: 0.67em 0; font-weight: bold; }
h2 { font-size: 1.5em;  margin: 0.83em 0; font-weight: bold; }
h3 { font-size: 1.17em; margin: 1em    0; font-weight: bold; }
h4 { font-size: 1em;    margin: 1.33em 0; font-weight: bold; }
h5 { font-size: 0.83em; margin: 1.67em 0; font-weight: bold; }
h6 { font-size: 0.67em; margin: 2.33em 0; font-weight: bold; }

/* === ������ � ������ === */
p     { margin: 1em 0; }
ul, ol { margin: 1em 0; padding-left: 40px; }
li    { margin: 0; }

/* === ������ � ������������������ ����� === */
blockquote { margin: 1em 40px; }
pre        { margin: 1em 0; white-space: pre; }

/* === ������ === */
hr { border-top-width: 1px; border-color: #808080; margin: 0.5em auto; }

/* === ���������� ���������� === */
b, strong { font-weight: bold; }
i, em     { font-style: italic; }
small     { font-size: 0.83em; }
sub, sup  { font-size: 0.83em; }
/* Картинка по умолчанию не шире родителя (как принято в вебе). */
img       { max-width: 100%; }
a         { text-decoration: underline; }
a[href]   { color: #0000EE; }
)CSS";

} // namespace ua_css