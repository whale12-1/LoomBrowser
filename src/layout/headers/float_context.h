#pragma once
#include <vector>
#include <algorithm>
#include <cstddef>

// =============================================================================
//  FloatContext Ч активные float'ы одного BFC.
//
//   оординаты Ч в системе content-box ближайшего BFC-контейнера:
//  (0, 0) Ч левый верхний угол внутренней (content) области.
//
//  Ќе знает про Qt, DOM, стили, шрифты Ч только геометри€.
//
//  ∆изненный цикл:
//    * —оздаЄтс€ в начале layout BFC-контейнера.
//    * ѕередаЄтс€ по ссылке всем потомкам, участвующим в этом BFC
//      (обычные block/inline-дети, пока не упрЄмс€ в новый BFC).
//    * ¬стретили float Ч add().  оординаты Ч margin-box float'а
//      (по CSS 2.1 І9.5 обтекание идЄт по outer = margin-box).
//    * ƒл€ обычного блока Ч available_range() сужает доступную ширину.
//    * clear: left/right/both Ч clear_y() сдвигает блок вниз.
//    * ¬ход в новый BFC (overflow:hidden, inline-block, float,
//      table-cell, absolute) Ч создаЄм Ќќ¬џ… FloatContext. –одительский
//      туда не проникает.
// =============================================================================
class FloatContext {
public:
    struct ActiveFloat {
        float left;      // лева€ граница margin-box
        float right;     // права€ граница margin-box
        float top;       // верхн€€ граница (margin-top)
        float bottom;    // нижн€€ граница (margin-bottom)
        bool  is_left;   // true дл€ float: left, false дл€ float: right
    };

    void clear() { floats_.clear(); }

    void add(const ActiveFloat& f) { floats_.push_back(f); }

    // ============================================================
    //  —вободный x-диапазон на заданной y.
    //
    //  bfc_content_width Ч полна€ ширина content-box BFC.
    //  out_left  Ч права€ граница всех активных на y левых float'ов
    //              (0, если их нет).
    //  out_right Ч лева€ граница всех активных на y правых float'ов
    //              (bfc_content_width, если их нет).
    //
    //  ƒиапазон полуоткрытый: [top, bottom) Ч float на y == bottom
    //  уже не вли€ет.
    // ============================================================
    void available_range(float y,
        float bfc_content_width,
        float& out_left,
        float& out_right) const
    {
        out_left = 0.0f;
        out_right = bfc_content_width;
        for (const auto& f : floats_) {
            if (y < f.top || y >= f.bottom) continue;
            if (f.is_left) out_left = std::max(out_left, f.right);
            else           out_right = std::min(out_right, f.left);
        }
        // «ащита: если float'ы перекрылись (некорректна€ вЄрстка или
        // float шире контейнера) Ч не даЄм отрицательной ширины.
        if (out_left > out_right) out_left = out_right;
    }

    // ============================================================
    //  ƒл€ clear: left/right/both Ч минимальный y >= from_y, на
    //  котором нет активных float'ов указанных сторон.
    // ============================================================
    float clear_y(bool clear_left, bool clear_right, float from_y) const {
        float y = from_y;
        for (const auto& f : floats_) {
            if (clear_left && f.is_left && f.bottom > y) y = f.bottom;
            if (clear_right && !f.is_left && f.bottom > y) y = f.bottom;
        }
        return y;
    }

    // ============================================================
    //  Ќижн€€ граница всех float'ов. »спользуетс€, когда BFC-контейнер
    //  должен раст€нутьс€ под ними: например, overflow:hidden, flow-root,
    //  или clearfix через ::after { clear: both }.
    // ============================================================
    float max_bottom() const {
        float b = 0.0f;
        for (const auto& f : floats_) b = std::max(b, f.bottom);
        return b;
    }

    bool   empty() const { return floats_.empty(); }
    size_t size()  const { return floats_.size(); }

    // ќпциональна€ оптимизаци€: удалить float'ы, которые уже закончились
    // выше y. ѕолезно при длинных BFC.
    void remove_ended_before(float y) {
        floats_.erase(
            std::remove_if(floats_.begin(), floats_.end(),
                [y](const ActiveFloat& f) { return f.bottom <= y; }),
            floats_.end());
    }

private:
    std::vector<ActiveFloat> floats_;
};