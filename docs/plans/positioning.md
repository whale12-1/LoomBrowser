# Positioning: relative + absolute

## Scope

Реализуем CSS positioning для block-level боксов.

**Включено в первую версию:**
- position: relative (один inset на ось)
- position: absolute (однозначные комбинации inset)
- left: 50% для absolute от ширины containing block
- Blockification: absolute inline становится block
- Viewport-sized initial containing block

**Отложено явно:**
- position: fixed, sticky
- vertical % inset (top: 50%)
- left + right одновременно, top + bottom одновременно
- auto-width / shrink-to-fit
- inline containing blocks

## Архитектурные решения

1. **flow_x/flow_y** хранятся в каждом LayoutNode — исходная потоковая позиция.
2. **Индекс CB → absolute descendants** строится в Phase 1 после wrap_anonymous_blocks.
3. **Порядок:** flow-layout → захват static position → relative-сдвиги → absolute.
4. **Relative — пост-обработка flow-позиции.** Absolute — отдельная внепоточная раскладка.
5. **Имена полей:** inset_top, inset_right, inset_bottom, inset_left.

## Верификация

Автоматическая сборка/тесты в Cline **не запускаются** (окружение не настроено).
Верификация:
1. Cline пишет код + тесты.
2. Пользователь собирает вручную в Visual Studio.
3. Если что-то не компилируется или тесты падают — новый чат Cline с логом ошибки.
4. Финальный ревью — в браузерном чате.

## Группы работ

### Группа A: Foundation
**Статус:** ✅ DONE
**Файлы:** style_storage_soa.h, style_tree_builder.h, layout_node.h, tests/style_tree_builder_tests.cpp
**Задачи:**
1. Inset properties (top/right/bottom/left) в CSS pipeline
2. flow_x/flow_y в LayoutNode
3. Тесты на CSS + baseline

### Группа B: Relative
**Статус:** ✅ DONE
**Зависит от:** A
**Файлы:** layout_tree_builder.h, tests/layout_tree_builder_tests.cpp

### Группа C: Absolute core
**Статус:** ✅ DONE
**Зависит от:** B
**Файлы:** layout_tree_builder.h, tests/layout_tree_builder_tests.cpp

#### C1: Индекс containing block и blockification
**Статус:** ✅ DONE
- `absolute_children` в `LayoutNode`
- Blockification absolute inline-боксов
- Индекс absolute descendants → containing block
- Регрессионные тесты

#### C2: Absolute layout
**Статус:** ✅ DONE
- Внепоточная раскладка absolute-боксов
- Absolute-боксы пропускаются при раскладке flow-потомков
- Реализованы left/right/top/bottom и заданные width/height; проценты top/bottom при auto-height CB отложены
- Добавлены регрессионные тесты позиционирования и отсутствия влияния на flow

#### C3: Дополнительные absolute-возможности
**Статус:** ✅ DONE

## Known limitation (C3)

Absolute positioning корректно работает, когда прямой parent
absolute-узла совпадает с его containing block.

Если absolute зарегистрирован у grand-parent (или выше) через
static-родителей, его geometry.x/y задаются относительно CB, но
display_list_builder накапливает координаты через фактического
parent. Это может дать неверный абсолютный сдвиг.

Не является блокером для текущих тестов. Отдельная задача —
пересмотр модели координат для out-of-flow узлов.

### Группа D: Static position + polish
**Статус:** ⬜ TODO
**Зависит от:** C

## Текущий фокус

Активная группа: **C1**
Следующее действие: реализовать absolute layout (C2).