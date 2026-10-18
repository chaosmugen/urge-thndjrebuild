// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/render/drawable_controller.h"

#include <vector>

#include "base/debug/logging.h"

#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
// Defined in content/screen/renderscreen_impl.cc. Marker: URGE_DIAG_GATE
extern "C" bool UrgeDiagEnabled();

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
  // TEMPORARY audit and guard. Marker: URGE_SCISSOR_AUDIT
  //
  // The probe has narrowed the defect to "in one frame the tilemap's draw does not cover part of
  // the map": those pixels never reach the tilemap's pixel shader at all, the layer below shows
  // through, and the draw calls, submitted ranges, bindings and vertex data are all unchanged. A
  // scissor stack that is one Pop out of balance does exactly that - the base entry is popped,
  // Reset() then reads an empty stack, and from there every draw is clipped to whatever rectangle
  // that produces. Pop() had no size guard, so this could not be observed before.
  const size_t depth = stack_.size();
  if (depth <= 1) {
    if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
    LOG(INFO) << "[ScissorAudit] UNDERFLOW: Pop with depth=" << depth
              << " top=(" << stack_.top().x << "," << stack_.top().y << ","
              << stack_.top().width << "x" << stack_.top().height << ")";
    // The bottom entry is the viewport scissor and is never popped; leaving it unbalanced would
    // read an empty stack in Reset() (undefined) and leave the scissor state wrong for the rest
    // of the frame, so restore it before returning.
    Reset();
    return;
  }
  stack_.pop();
  Reset();
}

void ScissorStack::Reset() {
  SetScissor(stack_.top());
}

// TEMPORARY scissor-rect audit. Marker: URGE_SCISSOR_AUDIT
//
// Auditing the stack's depth was not enough: Pop() can be balanced while the rectangle itself is
// the wrong one (a stale entry from another node), and a stale rectangle clips exactly one region
// for one frame - surviving a duplicate draw of the same geometry, with every count, binding,
// matrix and node order unchanged. This folds each rectangle that is actually programmed, together
// with the stack depth at the time, into a digest; BroadCastNotification compares that digest once
// per frame and prints only when the frame's sequence of rectangles changes.
int32_t g_scissor_current[4] = {0, 0, 0, 0};

void ScissorStack::SetScissor(const base::Rect& bound) {
  g_scissor_current[0] = bound.x;
  g_scissor_current[1] = bound.y;
  g_scissor_current[2] = bound.width;
  g_scissor_current[3] = bound.height;
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

// TEMPORARY. Marker: URGE_LINKAGE
// A frame counter the linkage traces can share with the snapshot listing: the controller cannot
// reach the sprite batch, and the enqueue stage is the once-per-frame event.
static int32_t g_urge_probe_frame = 0;

// TEMPORARY. Marker: URGE_TONE
// How many nodes each tone pass actually drew. The compass in the corner changes colour for one
// frame now and then (110,70,3 <-> 181,133,48) with every submitted batch identical, which is what
// a tone pass applied a different number of times looks like; these counters make that visible.
int32_t g_urge_pass0_drawn = 0;
int32_t g_urge_pass1_drawn = 0;

// True for the nodes this investigation follows: the two stacked panels' background (150) and
// contents (152) nodes. Their contents node is missing from the list on a good share of frames,
// which is why the panel is not drawn at all on those frames.
static bool UrgeProbeNode(DrawableNode* node) {
  if (!node)
    return false;
  const int64_t z = node->GetSortKeys()->weight[0];
  return z == 150 || z == 152;
}

void DrawableNode::SetToneExempt(bool value) {
  tone_exempt_ = value;
}

bool DrawableNode::IsToneExempt() const {
  return tone_exempt_;
}

void DrawableNode::RebindController(DrawNodeController* controller) {
  if (controller == controller_)
    return;

  // Setup new parent controller
  controller_ = controller;
  if (UrgeProbeNode(this))
    LOG(INFO) << "[Link] f=" << g_urge_probe_frame << " rebind n=" << static_cast<const void*>(this)
              << " z0=" << GetSortKeys()->weight[0] << " to=" << static_cast<const void*>(controller);

  // Rebind orderly node link
  base::LinkNode<DrawableNode>::RemoveFromList();
  controller_->InsertChildNodeInternal(this);
}

void DrawableNode::DisposeNode() {
  if (UrgeProbeNode(this))
    LOG(INFO) << "[Link] f=" << g_urge_probe_frame << " dispose n=" << static_cast<const void*>(this)
              << " z0=" << GetSortKeys()->weight[0];
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
  // base::LinkedList is circular, so for the head node previous() returns the
  // list's root sentinel. LinkNode::value() is just static_cast<T*>(this), so
  // returning value() for the sentinel hands out a DrawableNode* that aliases
  // the 16-byte sentinel; reading key_ (offset 24) or any other member off it
  // read past the end of the owning DrawNodeController. Report the sentinel as
  // "no neighbor" instead.
  if (!controller_)
    return nullptr;
  auto* node = previous();
  if (!node || node == controller_->children_list_.end())
    return nullptr;
  return node->value();
}

DrawableNode* DrawableNode::GetNextNode() {
  // See GetPreviousNode(): for the tail node next() is the same
  // sentinel.
  if (!controller_)
    return nullptr;
  auto* node = next();
  if (!node || node == controller_->children_list_.end())
    return nullptr;
  return node->value();
}

ViewportInfo* DrawableNode::GetParentViewport() {
  return controller_ ? &controller_->CurrentViewport() : nullptr;
}

void DrawableNode::ReorderDrawableNodeInternal() {
  DCHECK(controller_);
  if (UrgeProbeNode(this))
    LOG(INFO) << "[Link] f=" << g_urge_probe_frame << " reorder n=" << static_cast<const void*>(this)
              << " z0=" << GetSortKeys()->weight[0];

  // Fetch associate nodes
  auto* previous_node = GetPreviousNode();
  auto* next_node = GetNextNode();

  if (!previous_node && !next_node)
    return;

  // Check node reordering direction
  bool next_is_smaller = next_node && next_node->key_ < key_;
  bool prev_is_bigger = previous_node && previous_node->key_ > key_;

  if (next_is_smaller && prev_is_bigger) {
    // Ambiguous neighbors: both a forward and a backward walk look valid, so
    // fall back to a full remove + sorted re-insert instead of a local bubble
    // walk that can append the node to the wrong end of the list.
    if (UrgeProbeNode(this))
      LOG(INFO) << "[Link] f=" << g_urge_probe_frame << " reorder_full n="
                << static_cast<const void*>(this);
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
    // GetNextNode() returns nullptr once the list sentinel is reached, so the
    // walk terminates on nullptr rather than on the sentinel itself.
    while (current_node) {
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
    // See the ascending branch: the walk terminates on nullptr, since
    // GetPreviousNode() returns nullptr once the list sentinel is reached.
    while (current_node) {
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

  // Temporary probe. The stages walk this snapshot; a node that is missing from it for a
  // frame never enqueues and never draws, so its part of the picture is lost while every
  // counter that measures what *was* enqueued stays correct. A snapshot smaller than the
  // live list is that exact situation, so it is logged immediately (with a heartbeat).
  {
    static int64_t s_probe_calls = 0;
    ++s_probe_calls;
    int32_t live = 0;
    for (auto* it = children_list_.head(); it != children_list_.end();
         it = it->next())
      ++live;
    const int32_t count = static_cast<int32_t>(snapshot.size());
    if (count != live || s_probe_calls % 5000 == 0)
      if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
      LOG(INFO) << "[NodeAudit] call=" << s_probe_calls << " stage="
                << static_cast<int32_t>(nid) << " snapshot=" << count
                << " live=" << live
                << (count != live ? "  <== MISMATCH" : "");
  }

  // TEMPORARY per-frame listing of the snapshot. Marker: URGE_SNAPSHOT
  //
  // A window that is missing from this list for a frame is never walked, so it neither composes nor
  // draws, and its part of the picture is missing while everything that *was* submitted stays
  // correct. Listed by pointer so it can be joined with [WinKind], which maps a window to its
  // nodes; the position in the list matters as much as the presence, since the walk can also drop
  // a node after this point.
  if (static_cast<int32_t>(nid) == 0)
    ++g_urge_probe_frame;
  if (UrgeDiagEnabled()) {  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
    for (size_t i = 0; i < snapshot.size(); ++i)
      LOG(INFO) << "[Snap] f=" << g_urge_probe_frame << " stage=" << static_cast<int32_t>(nid)
                << " i=" << i
                << " n=" << static_cast<const void*>(snapshot[i])
                << " z0=" << snapshot[i]->GetSortKeys()->weight[0]
                << " z1=" << snapshot[i]->GetSortKeys()->weight[1]
                << " vis=" << (snapshot[i]->visible_ ? 1 : 0);
  }

  // TEMPORARY order audit over tagged nodes. Marker: URGE_ORDER_AUDIT
  //
  // Every counter has been ruled out for the one-frame region loss - parameters, atlas, tint
  // branch, draw counts, submitted ranges, bindings, vertex buffer ownership, scissor stack,
  // matrices, snapshot integrity - yet in a defect frame the tilemap's pixels are not there and
  // the layer below shows through. A paint order that swaps for a single frame would do that with
  // every counter unchanged. A digest of the whole snapshot is useless (the tilemap recreates its
  // above layers every frame and re-sets their keys, so the list churns); this folds in only the
  // nodes that carry a stable tag, together with the position each was visited at, and is printed
  // only when that sequence changes - which leaves the log volume and the timing untouched.
  {
    const int32_t order_slot = static_cast<int32_t>(nid) & 3;
    static uint64_t s_order_digest[4] = {};
    uint64_t order_digest = 1469598103934665603ull;
    int32_t order_tagged = 0;
    int32_t order_position = 0;
    for (auto* node : snapshot) {
      const int32_t node_tag = node->GetDebugTag();
      if (node_tag != 0) {
        order_digest ^= static_cast<uint64_t>(node_tag) + 0x9e3779b97f4a7c15ull;
        order_digest *= 1099511628211ull;
        order_digest ^= static_cast<uint64_t>(order_position);
        order_digest *= 1099511628211ull;
        ++order_tagged;
      }
      ++order_position;
    }
    if (order_tagged != 0 && s_order_digest[order_slot] != 0 &&
        s_order_digest[order_slot] != order_digest)
      if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
      LOG(INFO) << "[OrderAudit] stage=" << static_cast<int32_t>(nid)
                << " tagged=" << order_tagged << " digest "
                << s_order_digest[order_slot] << "->" << order_digest;
    if (order_tagged != 0)
      s_order_digest[order_slot] = order_digest;
  }

  // TEMPORARY. Marker: URGE_DISPATCH
  // The two stacked panels (sort weight 150 for their background nodes, 152 for their contents)
  // are present and visible in every snapshot this walk takes, yet one of them does not draw on a
  // good share of the frames. This records what happens to them at the point of dispatch, so the
  // remaining question - is the handler called at all - has an answer.
  for (auto* node : snapshot) {
    const int64_t z0 = node->GetSortKeys()->weight[0];
    const bool tracked = (z0 == 150 || z0 == 152);
    const bool tone_skipped =
        blend_filter >= 0 && node->IsToneExempt() != (blend_filter == 1);
    if (tracked && tone_skipped)
      LOG(INFO) << "[Dispatch] f=" << g_urge_probe_frame << " stage=" << static_cast<int32_t>(nid)
                << " n=" << static_cast<const void*>(node) << " z0=" << z0
                << " skipped=tone_filter";
    if (tone_skipped)
      continue;
    // Broadcast render job notification
    //
    // A drawable is drawn only when its viewport is visible as well. A viewport's visibility lives
    // on its own node and never reached the nodes inside it, so hiding a viewport left its whole
    // content being drawn - which is how the map's interface kept rendering during a battle, its
    // gold panel sitting on the same rectangle as the battle's turn panel and blending with it.
    bool visible = node->visible_;
    if (visible) {
      ViewportInfo* parent_viewport = node->GetParentViewport();
      if (parent_viewport)
        visible = parent_viewport->visible;
    }
    if (tracked)
      LOG(INFO) << "[Dispatch] f=" << g_urge_probe_frame << " stage=" << static_cast<int32_t>(nid)
                << " n=" << static_cast<const void*>(node) << " z0=" << z0
                << " skipped=none vis=" << (visible ? 1 : 0)
                << " calling=" << (visible ? 1 : 0);
    if (visible) {
      node->handler_.Run(nid, params);
      if (blend_filter == 0)
        ++g_urge_pass0_drawn;
      else if (blend_filter == 1)
        ++g_urge_pass1_drawn;
    }
  }
}

void DrawNodeController::InsertChildNodeInternal(DrawableNode* node) {
  if (UrgeProbeNode(node))
    LOG(INFO) << "[Link] f=" << g_urge_probe_frame << " insert n=" << static_cast<const void*>(node)
              << " z0=" << node->GetSortKeys()->weight[0]
              << " into=" << static_cast<const void*>(this);
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
