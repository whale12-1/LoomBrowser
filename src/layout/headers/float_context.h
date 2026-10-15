#pragma once
#include <vector>
#include <algorithm>
#include <cstddef>

// =============================================================================
//  FloatContext Ч набор активных float'ов одного BFC.
//
//  Ѕольше не хранит границы BFC: диапазон свободного места задаЄтс€
//  вызывающим кодом (CB Ч content-box того блока, чьи float'ы мы
//  позиционируем или относительно которого сужаем line box).
//  ¬се координаты Ч јЅ—ќЋё“Ќџ≈.
// =============================================================================
class FloatContext {
public:
    struct ActiveFloat {
        float left;
        float right;
        float top;
        float bottom;
        bool  is_left;
    };

    void clear() { floats_.clear(); }
    void add(const ActiveFloat& f) { floats_.push_back(f); }

    // —вободный x-диапазон на абсолютной y в пределах [cb_left, cb_right].
    // ƒиапазон полуоткрытый сверху: [top, bottom).
    void available_range(float abs_y,
        float cb_left,
        float cb_right,
        float& out_left,
        float& out_right) const
    {
        out_left = cb_left;
        out_right = cb_right;
        for (const auto& f : floats_) {
            if (abs_y < f.top || abs_y >= f.bottom) continue;
            if (f.is_left) out_left = std::max(out_left, f.right);
            else           out_right = std::min(out_right, f.left);
        }
        if (out_left > out_right) out_left = out_right;
    }

    float clear_y(bool clear_left, bool clear_right, float from_y) const {
        float y = from_y;
        for (const auto& f : floats_) {
            if (clear_left && f.is_left && f.bottom > y) y = f.bottom;
            if (clear_right && !f.is_left && f.bottom > y) y = f.bottom;
        }
        return y;
    }

    float max_bottom() const {
        float b = 0.0f;
        for (const auto& f : floats_) b = std::max(b, f.bottom);
        return b;
    }

    bool   empty() const { return floats_.empty(); }
    size_t size()  const { return floats_.size(); }

    const std::vector<ActiveFloat>& all() const { return floats_; }

    void remove_ended_before(float y) {
        floats_.erase(
            std::remove_if(floats_.begin(), floats_.end(),
                [y](const ActiveFloat& f) { return f.bottom <= y; }),
            floats_.end());
    }

private:
    std::vector<ActiveFloat> floats_;
};