// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/render/drawable_controller.h"

#include <vector>

#include "Graphics/GraphicsEngine/interface/DeviceContext.h"

namespace content {

///////////////////////////////////////////////////////////////////////////////
// SortKey Implement

static int64_t g_creation_stamp = 0;

SortKey::SortKey() : weight{0, 0, ++g_creation_stamp} {}

SortKey::SortKey(int64_t key1) : weight{key1, 0, ++g_creation_stamp} {}

SortKey::SortKey(int64_t key1, int64_t key2)
    : weight{key1, key2, ++g_creation_stamp} {}

SortKey::SortKey(int64_t key1, int64_t key2, int64_t key3)
    : weight{key1, key2, key3} {}

///////////////////////////////////////////////////////////////////////////////
// ScissorStack Implement

ScissorStack::ScissorStack(Diligent::IDeviceContext* context,
                           const base::Rect& first)
    : context_(context) {
  stack_.push(first);
  Reset();
}

base::Rect ScissorStack::Current() {
  return stack_.top();
}

bool ScissorStack::Push(const base::Rect& scissor) {
  auto intersect = base::MakeIntersect(stack_.top(), scissor);
  if (intersect.width && intersect.height) {
    stack_.push(intersect);
    Reset();

    return true;
  }

  return false;
}

void ScissorStack::Pop() {
  stack_.pop();
  Reset();
}

void ScissorStack::Reset() {
  SetScissor(stack_.top());
}

void ScissorStack::SetScissor(const base::Rect& bound) {
  Diligent::Rect render_scissor(bound.x, bound.y, bound.x + bound.width,
                                bound.y + bound.height);
  context_->SetScissorRects(1, &render_scissor, UINT32_MAX, UINT32_MAX);
}

///////////////////////////////////////////////////////////////////////////////
// DrawableNode Implement

DrawableNode::DrawableNode(DrawNodeController* controller,
                           const SortKey& default_key,
                           bool visible)
    : controller_(controller),
      key_(default_key),
      visible_(visible),
      batch_id_(0),
      batch_self_(nullptr) {
  if (controller_)
    controller_->InsertChildNodeInternal(this);
}

DrawableNode::~DrawableNode() {
  DisposeNode();
}

void DrawableNode::RegisterEventHandler(const NotificationHandler& handler) {
  DCHECK(!handler.is_null() && handler_.is_null());
  handler_ = handler;
}

void DrawableNode::SetBlendTypeProvider(BlendTypeProvider provider) {
  blend_provider_ = std::move(provider);
}

int32_t DrawableNode::GetBlendType() const {
  return blend_provider_ ? blend_provider_() : 0;
}

void DrawableNode::RebindController(DrawNodeController* controller) {
  if (controller == controller_)
    return;

  // Setup new parent controller
  controller_ = controller;

  // Rebind orderly node link
  base::LinkNode<DrawableNode>::RemoveFromList();
  controller_->InsertChildNodeInternal(this);
}

void DrawableNode::DisposeNode() {
  base::LinkNode<DrawableNode>::RemoveFromList();
  controller_ = nullptr;
}

void DrawableNode::SetNodeVisibility(bool visible) {
  visible_ = visible;
}

bool DrawableNode::GetVisibility() const {
  return visible_;
}

void DrawableNode::SetNodeSortWeight(int64_t weight1) {
  if (!controller_)
    return;

  if (key_.weight[0] == weight1)
    return;

  key_.weight[0] = weight1;
  ReorderDrawableNodeInternal();
}

void DrawableNode::SetNodeSortWeight(int64_t weight1, int64_t weight2) {
  if (!controller_)
    return;

  if (key_.weight[0] == weight1 && key_.weight[1] == weight2)
    return;

  key_.weight[0] = weight1;
  key_.weight[1] = weight2;
  ReorderDrawableNodeInternal();
}

void DrawableNode::SetNodeSortWeight(int64_t weight1,
                                     int64_t weight2,
                                     int64_t weight3) {
  if (!controller_)
    return;

  if (key_.weight[0] == weight1 && key_.weight[1] == weight2 &&
      key_.weight[2] == weight3)
    return;

  key_.weight[0] = weight1;
  key_.weight[1] = weight2;
  key_.weight[2] = weight3;
  ReorderDrawableNodeInternal();
}

DrawableNode* DrawableNode::GetPreviousNode() {
  auto* node = previous();
  return node ? node->value() : nullptr;
}

DrawableNode* DrawableNode::GetNextNode() {
  auto* node = next();
  return node ? node->value() : nullptr;
}

ViewportInfo* DrawableNode::GetParentViewport() {
  return controller_ ? &controller_->CurrentViewport() : nullptr;
}

void DrawableNode::ReorderDrawableNodeInternal() {
  DCHECK(controller_);

  // Fetch associate nodes
  auto* previous_node = GetPreviousNode();
  auto* next_node = GetNextNode();

  if (!previous_node && !next_node)
    return;

  // Check node reordering direction
  bool next_is_smaller = next_node && next_node->key_ < key_;
  bool prev_is_bigger = previous_node && previous_node->key_ > key_;

  // Treat absurdly large key gaps as stale/dangling neighbors (freed memory
  // yields weights like -2^40..-2^46); a valid z is a small integer.
  constexpr int64_t kAbsurdGap = int64_t{1} << 40;
  bool next_suspect =
      next_is_smaller &&
      (key_.weight[0] - next_node->key_.weight[0]) > kAbsurdGap;
  bool prev_suspect =
      prev_is_bigger &&
      (previous_node->key_.weight[0] - key_.weight[0]) > kAbsurdGap;

  if ((next_is_smaller && prev_is_bigger) || next_suspect || prev_suspect) {
    // Ambiguous neighbors (e.g. a stale/dangling node nearby produced
    // inconsistent keys). Fall back to a full remove + sorted re-insert
    // instead of a local bubble walk that can append the node to the wrong
    // end of the list.
    base::LinkNode<DrawableNode>::RemoveFromList();
    controller_->InsertChildNodeInternal(this);
    return;
  }

  bool ascending_order = !previous_node || next_is_smaller;
  bool descending_order = !next_node || prev_is_bigger;

  if (!ascending_order && !descending_order) {
    // No need to reorder, already in correct position
    return;
  }

  // min -> max
  if (ascending_order) {
    DrawableNode* current_node = next_node;
    DCHECK(current_node);

    // Remove from current list, and insert before the first node with a greater
    base::LinkNode<DrawableNode>::RemoveFromList();
    while (current_node != controller_->children_list_.end()) {
      if (current_node->key_ > key_) {
        base::LinkNode<DrawableNode>::InsertBefore(current_node);
        return;
      }

      current_node = current_node->GetNextNode();
    }

    // Reached the end of the list, append to the end.
    controller_->children_list_.Append(this);
    return;
  }

  // max -> min
  if (descending_order) {
    DrawableNode* current_node = previous_node;
    DCHECK(current_node);

    // Remove from current list, and insert before the first node with a greater
    base::LinkNode<DrawableNode>::RemoveFromList();
    while (current_node != controller_->children_list_.end()) {
      if (current_node->key_ < key_) {
        base::LinkNode<DrawableNode>::InsertAfter(current_node);
        return;
      }

      current_node = current_node->GetPreviousNode();
    }

    // Reached the end of the list, append to the end.
    controller_->children_list_.Prepend(this);
    return;
  }

  NOTREACHED();
}

DrawNodeController::DrawNodeController() = default;

DrawNodeController::~DrawNodeController() {
  // Reset child controller association and fully unlink every child before
  // the list storage is destroyed. Otherwise children destroyed later (GC)
  // would call RemoveFromList through a destroyed list / freed memory,
  // leaving dangling entries that corrupt the render order (e.g. an opaque
  // plane ending up after the tilemap ground and covering it).
  for (auto* it = children_list_.head(); it != children_list_.end();) {
    auto* next_it = it->next();
    it->value()->controller_ = nullptr;
    it->RemoveFromList();
    it = next_it;
  }
}

void DrawNodeController::BroadCastNotification(
    DrawableNode::RenderStage nid,
    DrawableNode::RenderControllerParams* params,
    int32_t blend_filter) {
  // Snapshot the child list before notifying. Handlers are allowed to mutate
  // the list (e.g. the tilemap recreates/re-orders its above layers every
  // frame during BEFORE_RENDER); iterating a live doubly-linked list while it
  // is being unlinked/relinked can skip nodes or corrupt the render order.
  std::vector<DrawableNode*> snapshot;
  snapshot.reserve(64);
  for (auto* it = children_list_.head(); it != children_list_.end();
       it = it->next())
    snapshot.push_back(it->value());

  for (auto* node : snapshot) {
    if (blend_filter >= 0 &&
        (node->GetBlendType() == 0) != (blend_filter == 0))
      continue;
    // Broadcast render job notification
    if (node->visible_)
      node->handler_.Run(nid, params);
  }
}

void DrawNodeController::InsertChildNodeInternal(DrawableNode* node) {
  // Defensive: make sure the node is not still linked into any list before
  // inserting (no-op when the node is already unlinked).
  node->RemoveFromList();

  // From min to max.
  for (auto* it = children_list_.head(); it != children_list_.end();
       it = it->next()) {
    // Insert before the first node with a greater key.
    // This will keep the list sorted in ascending order.
    if (node->key_ < it->value()->key_)
      return node->InsertBefore(it);
  }

  // Max key, append to the end.
  children_list_.Append(node);
}

void DrawableFlashController::Setup(
    const std::optional<base::Vec4>& flash_color,
    int32_t duration) {
  if (duration > 0) {
    duration_ = duration;
    count_ = 0;
    color_ = flash_color.has_value() ? flash_color.value() : base::Vec4();
    alpha_ = color_.w;
    invalid_ = !flash_color.has_value();
  }
}

void DrawableFlashController::Update() {
  if (duration_) {
    if (++count_ > duration_) {
      duration_ = 0;
      invalid_ = false;
      return;
    } else {
      float progress = static_cast<float>(count_) / duration_;
      color_.w = alpha_ * (1.0f - progress);
    }
  }
}

}  // namespace content
