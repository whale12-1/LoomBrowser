#pragma once
#include <cstdint>

// CSS Cascade origin (CSS Cascading and Inheritance Level 4).
//
// ѕор€док приоритета дл€ Ќќ–ћјЋ№Ќџ’ деклараций (по возрастанию силы):
//    UserAgent  <  User  <  Author
//
// ƒл€ !important приоритет »Ќ¬≈–“»–”≈“—я:
//    Author  <  User  <  UserAgent
//
// „исловые значени€ выбраны так, чтобы обычные сравнени€ работали
// напр€мую дл€ нормального случа€; дл€ !important сравнение идЄт
// в обратную сторону.
enum class Origin : uint8_t {
    UserAgent = 1,
    User      = 2,
    Author    = 3,
};