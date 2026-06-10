# 任务二 索引管理 — 实现计划

## 需求概述

实现 B+ 树索引管理器，支持查找、插入、删除和并发控制。需要修改 **2 个文件**，实现 **14 个函数**。

## 修改文件清单

| 文件路径 | 子任务 | 分值 |
|----------|--------|------|
| `src/index/ix_node_handle.cpp` | 2.1 结点内查找 + 2.2 结点内插入 + 2.3 结点内删除 | 70 |
| `src/index/ix_index_handle.cpp` | 2.1 B+树查找 + 2.2 B+树插入 + 2.3 B+树删除 + 2.4 并发 | 30 |

---

## Phase 1：IxNodeHandle — 结点内操作（6 个函数）

### 1.1 查找类函数（任务 2.1）

#### `lower_bound(const char *target)` — 二分查找第一个 ≥ target 的 key_idx
- 使用二分查找（`binary_search = true`）
- `int l = 0, r = page_hdr->num_key`
- `mid = (l + r) / 2`，若 `ix_compare(get_key(mid), target, ...) < 0` 则 `l = mid + 1`，否则 `r = mid`
- 返回 `l`（可能等于 `num_key`）

#### `upper_bound(const char *target)` — 二分查找第一个 > target 的 key_idx
- 类似 lower_bound，但条件改为 `ix_compare(get_key(mid), target, ...) <= 0`
- 返回 `r`（范围从 1 开始）

#### `LeafLookup(const char *key, Rid **value)` — 叶子结点精确查找
- 调用 `lower_bound(key)` 得到 `idx`
- 若 `idx < GetSize() && ix_compare(get_key(idx), key, ...) == 0`：`*value = get_rid(idx)`，返回 `true`
- 否则返回 `false`

#### `InternalLookup(const char *key)` — 内部结点路由
- 调用 `upper_bound(key)` 得到 `idx`
- 返回 `get_rid(idx)->page_no`（即第 `idx` 个孩子结点的页号）

### 1.2 插入类函数（任务 2.2）

#### `insert_pairs(int pos, const char *key, const Rid *rid, int n)` — 批量插入键值对
- 先将 keys 的 `[pos, num_key)` 移到 `[pos+n, num_key+n)`（用 `memmove`）
- 再将 rids 的 `[pos, num_key)` 移到 `[pos+n, num_key+n)`
- 将新 keys 的前 n 个拷贝到 `[pos, pos+n)`
- 将新 rids 的前 n 个拷贝到 `[pos, pos+n)`
- `page_hdr->num_key += n`

#### `Insert(const char *key, const Rid &value)` — 单个键值对插入
- `int pos = lower_bound(key)`
- 若 `pos < GetSize() && ix_compare(get_key(pos), key, ...) == 0`：重复 key，不插入，直接返回 `GetSize()`
- 调用 `insert_pairs(pos, key, &value, 1)`
- 返回 `GetSize()`

### 1.3 删除类函数（任务 2.3）

#### `erase_pair(int pos)` — 删除指定位置的键值对
- 将 keys 的 `[pos+1, num_key)` 移到 `[pos, num_key-1)`
- 将 rids 的 `[pos+1, num_key)` 移到 `[pos, num_key-1)`
- `page_hdr->num_key--`

#### `Remove(const char *key)` — 删除指定 key 的键值对
- `int pos = lower_bound(key)`
- 若 `pos < GetSize() && ix_compare(get_key(pos), key, ...) == 0`：调用 `erase_pair(pos)`
- 返回 `GetSize()`

---

## Phase 2：IxIndexHandle — B+ 树操作（8 个函数）

### 2.1 查找类函数（任务 2.1）

#### `FindLeafPage(const char *key, Operation operation, Transaction *transaction)` — 查找叶子结点
- 获取根结点：`IxNodeHandle *node = FetchNode(file_hdr_.root_page)`
- 循环：若 `!node->IsLeafPage()`，则 `page_id_t child_page = node->InternalLookup(key)`，unpin 当前 node，`node = FetchNode(child_page)`
- 返回叶子结点（调用者需 unpin）

#### `GetValue(const char *key, vector<Rid> *result, Transaction *transaction)` — B+ 树查找
- `root_latch_.lock()` / `std::scoped_lock`（粗粒度并发）
- `IxNodeHandle *leaf = FindLeafPage(key, Operation::FIND, transaction)`
- `Rid *value = nullptr`；若 `leaf->LeafLookup(key, &value)` 成功，`result->push_back(*value)`
- unpin leaf
- 解锁

### 2.2 插入类函数（任务 2.2）

#### `insert_entry(const char *key, const Rid &value, Transaction *transaction)` — B+ 树插入
- 加锁 `root_latch_`
- `IxNodeHandle *leaf = FindLeafPage(key, Operation::INSERT, transaction)`
- `int size = leaf->Insert(key, value)`
- 若 `size == leaf->GetMaxSize()`：
  - `IxNodeHandle *new_leaf = Split(leaf)`
  - `InsertIntoParent(leaf, new_leaf->get_key(0), new_leaf, transaction)`
  - unpin new_leaf
  - 若 leaf 是最右叶子（`leaf->GetPageNo() == file_hdr_.last_leaf`），更新 `file_hdr_.last_leaf = new_leaf->GetPageNo()`
- unpin leaf
- 解锁
- 返回 `true`

#### `Split(IxNodeHandle *node)` — 结点分裂
- 创建新结点：`IxNodeHandle *new_node = CreateNode()`
- 初始化 new_node 的 `page_hdr`：`parent = node->GetParentPageNo()`、`is_leaf = node->IsLeafPage()`、`num_key = 0`
- 计算分裂点：`int mid = (node->GetSize() + 1) / 2`
- 将 node 的 `[mid, num_key)` 移到 new_node：`new_node->insert_pairs(0, node->get_key(mid), node->get_rid(mid), node->GetSize() - mid)`
- 更新 node：`node->SetSize(mid)`
- 若是叶子结点：更新 prev_leaf/next_leaf 链表
- 若是内部结点：遍历 new_node 的所有孩子，调用 `maintain_child(new_node, i)` 更新父指针
- 返回 new_node

#### `InsertIntoParent(IxNodeHandle *old_node, const char *key, IxNodeHandle *new_node, Transaction *transaction)` — 递归向上插入
- 若 `old_node->IsRootPage()`：
  - 创建新根 `IxNodeHandle *new_root = CreateNode()`
  - 初始化：`is_leaf = false`、`num_key = 0`、`parent = IX_NO_PAGE`
  - 插入 old_node 和 new_node：先 `insert_pair(0, old_node->get_key(0), Rid{old_node->GetPageNo(), -1})`，再 `insert_pair(1, key, Rid{new_node->GetPageNo(), -1})`
  - 更新 old_node 和 new_node 的 parent
  - 更新 `file_hdr_.root_page = new_root->GetPageNo()`
  - unpin new_root
  - return
- 否则获取父结点：`IxNodeHandle *parent = FetchNode(old_node->GetParentPageNo())`
- 找到 old_node 在 parent 中的位置：`int idx = parent->find_child(old_node)`
- 在 parent 中 idx+1 处插入 `(key, Rid{new_node->GetPageNo(), -1})`
- 更新 new_node 的 parent
- 若 `parent->GetSize() == parent->GetMaxSize()`：
  - `IxNodeHandle *new_parent = Split(parent)`
  - `InsertIntoParent(parent, new_parent->get_key(0), new_parent, transaction)`
  - unpin new_parent
- unpin parent

### 2.3 删除类函数（任务 2.3）

#### `delete_entry(const char *key, Transaction *transaction)` — B+ 树删除
- 加锁 `root_latch_`
- `IxNodeHandle *leaf = FindLeafPage(key, Operation::DELETE, transaction)`
- `int new_size = leaf->Remove(key)`
- 若 `new_size < leaf->GetMinSize()`：`CoalesceOrRedistribute(leaf, transaction)`
- unpin leaf
- 解锁
- 返回 `true`

#### `CoalesceOrRedistribute(IxNodeHandle *node, Transaction *transaction)` — 合并或重分配决策
- 若 `node->IsRootPage()`：`return AdjustRoot(node)`
- 若 `node->GetSize() >= node->GetMinSize()`：不需要操作，返回 `false`
- 获取 parent：`FetchNode(node->GetParentPageNo())`
- 找到 node 在 parent 中的位置：`int idx = parent->find_child(node)`
- 找兄弟结点（优先前驱）：
  - 若 `idx > 0`：`neighbor = FetchNode(parent->ValueAt(idx - 1))`，`neighbor_idx = idx - 1`
  - 否则：`neighbor = FetchNode(parent->ValueAt(idx + 1))`，`neighbor_idx = idx + 1`
- 若 `node->GetSize() + neighbor->GetSize() >= node->GetMinSize() * 2`：
  - `Redistribute(neighbor, node, parent, idx)`
  - unpin neighbor, parent
  - 返回 `false`
- 否则：
  - `bool parent_should_delete = Coalesce(&neighbor, &node, &parent, idx, transaction)`
  - unpin neighbor, parent
  - 返回 `parent_should_delete`

#### `Redistribute(IxNodeHandle *neighbor_node, IxNodeHandle *node, IxNodeHandle *parent, int index)` — 重分配
- **情况 1**：`index == 0`（neighbor 在右边，是后继）
  - 从 neighbor 移动第一个键值对到 node 的末尾
  - `node->insert_pair(node->GetSize(), neighbor_node->get_key(0), *neighbor_node->get_rid(0))`
  - `neighbor_node->erase_pair(0)`
  - 更新 parent 中 node 对应的 key：`parent->set_key(index, node->get_key(node->GetSize() - 1))` 或用 `maintain_parent`
  - 若 node 是内部结点：`maintain_child(node, node->GetSize() - 1)`
- **情况 2**：`index > 0`（neighbor 在左边，是前驱）
  - 从 neighbor 移动最后一个键值对到 node 的开头
  - `node->insert_pair(0, neighbor_node->get_key(neighbor_node->GetSize() - 1), *neighbor_node->get_rid(neighbor_node->GetSize() - 1))`
  - `neighbor_node->erase_pair(neighbor_node->GetSize() - 1)`
  - 更新 parent 中 node 对应的 key
  - 若 node 是内部结点：`maintain_child(node, 0)`

#### `Coalesce(...)` — 合并
- 若 `index == 0`：交换 neighbor_node 和 node（保证 neighbor 在左，node 在右）
- 将 node 的所有键值对移到 neighbor：`neighbor_node->insert_pairs(neighbor_node->GetSize(), node->get_key(0), node->get_rid(0), node->GetSize())`
- 若是内部结点：遍历被移动的孩子，`maintain_child(neighbor_node, i)`
- 若是叶子结点：更新 prev_leaf/next_leaf（调用 `erase_leaf(node)`）
- 若 node 是 last_leaf：更新 `file_hdr_.last_leaf`
- 删除 parent 中 node 的键值对：`parent->erase_pair(index_of_node_in_parent)`
- 释放 node：`release_node_handle(*node)`，unpin node page，DeletePage
- 递归检查 parent：`CoalesceOrRedistribute(parent, transaction)`

#### `AdjustRoot(IxNodeHandle *old_root_node)` — 根结点调整
- 若 `old_root_node->IsLeafPage() && old_root_node->GetSize() == 0`：
  - `file_hdr_.root_page = IX_NO_PAGE`（空树）
  - `release_node_handle(*old_root_node)`
  - 返回 `true`
- 若 `!old_root_node->IsLeafPage() && old_root_node->GetSize() == 1`：
  - `page_id_t new_root = old_root_node->RemoveAndReturnOnlyChild()`
  - `file_hdr_.root_page = new_root`
  - 更新新根的 parent：`IxNodeHandle *new_root_node = FetchNode(new_root)`，`new_root_node->SetParentPageNo(IX_NO_PAGE)`
  - unpin new_root_node
  - `release_node_handle(*old_root_node)`
  - 返回 `true`
- 其他情况：返回 `false`

---

## Phase 3：并发控制（任务 2.4）— 粗粒度 Tree 级

在 `insert_entry`、`delete_entry`、`GetValue` 三个入口函数中加 `std::scoped_lock<std::mutex> lock(root_latch_)` 即可。

---

## 实现顺序与依赖关系

```
Phase 1 (IxNodeHandle)     Phase 2 (IxIndexHandle)
┌─────────────────────┐    ┌──────────────────────────┐
│ 1. lower_bound      │───→│ 4. FindLeafPage          │
│ 2. upper_bound      │───→│ 5. GetValue              │
│ 3. LeafLookup       │←───│                          │
│    InternalLookup   │←───│                          │
├─────────────────────┤    ├──────────────────────────┤
│ 6. insert_pairs     │───→│ 8. insert_entry          │
│ 7. Insert           │───→│ 9. Split                 │
│                     │    │ 10. InsertIntoParent      │
├─────────────────────┤    ├──────────────────────────┤
│ 8. erase_pair       │───→│ 11. delete_entry         │
│ 9. Remove           │───→│ 12. CoalesceOrRedistribute│
│                     │    │ 13. Redistribute          │
│                     │    │ 14. Coalesce              │
│                     │    │ 15. AdjustRoot            │
└─────────────────────┘    └──────────────────────────┘
```

## 风险评估

| 风险 | 级别 | 说明 |
|------|------|------|
| Split 后 key 范围错误 | HIGH | 分裂点选择和 key 拷贝需精确 |
| InsertIntoParent 递归 | HIGH | 根结点特殊处理容易遗漏 |
| Coalesce 双向链表维护 | HIGH | 叶子结点 prev/next 指针更新 |
| Unpin 泄漏 | MEDIUM | 每个 FetchNode/CreateNode 都需配对 UnpinPage |
| Redistribute 方向判断 | MEDIUM | index==0 vs index>0 移动方向不同 |
| 并发粒度选择 | LOW | 选粗粒度（Tree 级）最简单 |

## 预估复杂度

- Phase 1 结点内操作：约 150 行代码，**中等**难度
- Phase 2 B+ 树操作：约 250 行代码，**高**难度（递归逻辑复杂）
- Phase 3 并发控制：约 10 行代码，**低**难度（粗粒度方案）
