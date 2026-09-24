#pragma once
#include <cstdint>
#include <string>
#include <cstddef>

namespace style {

    // ---- Display / Position / etc. ----
    enum class Display : uint8_t {
        None, Contents, Inline, Block, InlineBlock, Flex, InlineFlex,
        Grid, InlineGrid, Table, TableRow, TableCell
    };

    enum class Position : uint8_t { Static, Relative, Absolute, Fixed, Sticky };
    enum class Float : uint8_t { None, Left, Right };
    enum class Overflow : uint8_t { Visible, Hidden, Scroll, Auto };
    enum class Visibility : uint8_t { Visible, Hidden, Collapse };
    enum class BoxSizing : uint8_t { ContentBox, BorderBox };
    enum class TextAlign : uint8_t { Start, End, Left, Right, Center, Justify };
    enum class FontStyle : uint8_t { Normal, Italic, Oblique };
    enum class WhiteSpace : uint8_t { Normal, NoWrap, Pre, PreWrap, PreLine };
    enum class FlexDir : uint8_t { Row, RowReverse, Column, ColumnReverse };
    enum class Justify : uint8_t { Start, End, Center, SpaceBetween, SpaceAround, SpaceEvenly };
    enum class AlignItems : uint8_t { Stretch, FlexStart, FlexEnd, Center, Baseline };

    // ---- Идентификатор CSS-свойства. Используется в таблице каскада ----
    enum class Prop : uint16_t {
        Display, Position, Float, Overflow, Visibility, BoxSizing, ZIndex, Opacity,
        Color, BackgroundColor,
        FontSize, FontWeight, FontStyle, LineHeight, TextAlign, LetterSpacing, WhiteSpace,
        Width, Height, MinWidth, MaxWidth, MinHeight, MaxHeight,
        MarginTop, MarginRight, MarginBottom, MarginLeft,
        PaddingTop, PaddingRight, PaddingBottom, PaddingLeft,
        BorderTopWidth, BorderRightWidth, BorderBottomWidth, BorderLeftWidth,
        FlexDirection, JustifyContent, AlignItems, FlexGrow, FlexShrink, FlexBasis,
        COUNT
    };

    constexpr size_t PROP_COUNT = static_cast<size_t>(Prop::COUNT);
    constexpr size_t PROP_MASK_WORDS = (PROP_COUNT + 63) / 64;

    inline size_t prop_index(Prop p) { return static_cast<size_t>(p); }

    // Наследуемые свойства — критично для правильного дерева.
    inline bool is_inherited(Prop p) {
        switch (p) {
        case Prop::Color: case Prop::FontSize: case Prop::FontWeight:
        case Prop::FontStyle: case Prop::LineHeight: case Prop::TextAlign:
        case Prop::LetterSpacing: case Prop::WhiteSpace: case Prop::Visibility:
            return true;
        default: return false;
        }
    }

    // ---- Длина. Откладываем resolve относительных единиц до layout ----
    enum class Unit : uint8_t {
        Px, Em, Rem, Percent, Vw, Vh, Vmin, Vmax, Pt, Cm, Mm, In,
        Auto, None
    };

    struct Length {
        float value = 0.0f;
        Unit  unit = Unit::Px;

        bool is_auto() const { return unit == Unit::Auto; }
        bool is_none() const { return unit == Unit::None; }
        bool is_zero() const { return value == 0.0f && unit == Unit::Px; }
    };

    // ---- Цвет 0xRRGGBBAA ----
    struct Color {
        uint8_t r = 0, g = 0, b = 0, a = 255;
        uint32_t pack() const {
            return (uint32_t(r) << 24) | (uint32_t(g) << 16) | (uint32_t(b) << 8) | a;
        }
        static Color unpack(uint32_t v) {
            return { uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v) };
        }
    };

} // namespace style