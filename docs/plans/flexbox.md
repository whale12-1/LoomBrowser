# Flexbox plan

## Group A — foundation

### A1 — flex foundation

- [x] Add `is_flex_container` helper in `layout_tree_builder.h`.
- [x] Route flex containers before the `BoxType` switch in `layout_node`.
- [x] Keep `display: flex` nodes as `BoxType::Block`.
- [x] Add `layout_flex_container` as a block-layout fallback.
- [x] Add A1 regression tests for style recognition, box type, unchanged block
  behavior, and block-layout fallback.
- [ ] A1 build verification in Visual Studio (not run by the coding agent).

### A2 — flex algorithm

- [ ] Replace the A1 block-layout fallback with the real flex algorithm.
- [x] Implement flex child normalization and text grouping as part of A2.
- [ ] Add coverage for flex direction, sizing, alignment, and wrapping.

## Scope constraints

A1 does not implement `normalize_flex_children`, text grouping,
`wrap_anonymous_blocks` changes, `layout_block_container` changes, new
`LayoutNode` fields, or changes to `style_types.h` and `style_storage_soa.h`.
