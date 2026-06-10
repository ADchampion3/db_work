# 任务一 存储管理 — 代码修改总结

## 修改文件清单

共修改 **5 个文件**：

| 文件路径 | 任务 | 分值 |
|----------|------|------|
| `src/storage/disk_manager.cpp` | 任务 1.1.1 磁盘存储管理器 | 10 |
| `src/replacer/lru_replacer.cpp` | 任务 1.1.2 缓冲池替换策略 | 20 |
| `src/storage/buffer_pool_manager.cpp` | 任务 1.1.3 缓冲池管理器 | 40 |
| `src/record/rm_file_handle.cpp` | 任务 1.2.1 记录操作 | 30 |
| `src/record/rm_scan.cpp` | 任务 1.2.2 记录迭代器 | 含在上方 |

---

## 一、DiskManager（任务 1.1.1）

**核心思路**：封装 Linux POSIX I/O 接口（`open/read/write/close/lseek/unlink/stat`），实现磁盘文件的页面级读写与文件生命周期管理。

| 函数 | 实现逻辑 |
|------|----------|
| `write_page` | `lseek(fd, page_no * PAGE_SIZE, SEEK_SET)` 定位偏移量 → `write(fd, offset, num_bytes)` 写入，失败抛 `UnixError` |
| `read_page` | `lseek` 定位 → `read(fd, offset, num_bytes)` 读取，失败抛 `UnixError` |
| `AllocatePage` | 调用 `fd2pageno_[fd].fetch_add(1)` 原子自增并返回旧值，线程安全 |
| `is_file` | `struct stat` + `S_ISREG` 判断是否为普通文件 |
| `create_file` | 先调 `is_file` 检查不存在 → `open(O_CREAT \| O_RDWR, 0666)` 创建 → 立即 `close` |
| `destroy_file` | 检查文件存在 → 检查 `path2fd_` 中无记录（文件已关闭）→ `unlink()` 删除 |
| `open_file` | 检查文件存在 → 检查 `path2fd_` 无记录（未重复打开）→ `open(O_RDWR)` → 更新 `path2fd_` 和 `fd2path_` 映射表 |
| `close_file` | 检查 `fd2path_` 中存在（文件已打开）→ `close()` → 从两个映射表中删除 |

**关键设计**：
- 使用 `std::unordered_map<string,int> path2fd_` 和 `std::unordered_map<int,string> fd2path_` 双向映射管理文件打开状态
- `fd2pageno_` 为 `std::atomic<page_id_t>` 数组，支持并发分配页号

---

## 二、LRUReplacer（任务 1.1.2）

**核心思路**：使用 `std::list<frame_id_t>` + `std::unordered_map<frame_id_t, iterator>` 实现 O(1) 的 LRU 淘汰策略，所有操作用 `std::scoped_lock` 保证线程安全。

| 函数 | 实现逻辑 |
|------|----------|
| `Victim` | 若链表空返回 `false`；否则取链表**尾部**（最久未使用）的 frame_id，从链表和哈希表中删除 |
| `Pin` | 在哈希表中查找，若存在则从链表和哈希表中移除（固定 = 不可被淘汰） |
| `Unpin` | 若已在哈希表中则直接返回（去重）；插入到链表**头部**（最近使用），并更新哈希表 |
| `Size` | 加锁后返回 `LRUlist_.size()` |

**数据结构语义**：
- `LRUlist_`：头部 = 最近 unpin，尾部 = 最久未 unpin（淘汰候选）
- `LRUhash_`：frame_id → list 迭代器，实现 O(1) 查找与删除

---

## 三、BufferPoolManager（任务 1.1.3）

**核心思路**：管理缓冲池中内存页面与磁盘页面的映射，使用 `page_table_`（PageId → frame_id 哈希表）定位页面，`free_list_` 管理空闲帧，`replacer_` 在无空闲帧时选择淘汰页。

### 辅助函数

| 函数 | 实现逻辑 |
|------|----------|
| `FindVictimPage` | 优先从 `free_list_` 取空闲帧；否则调 `replacer_->Victim()` 淘汰一帧 |
| `UpdatePage` | ① 脏页写回磁盘 ② 从 `page_table_` 删除旧映射 ③ 插入新映射 ④ `ResetMemory()` 清数据，重置 `id_/is_dirty_/pin_count_` |

### 公有函数

| 函数 | 实现逻辑 |
|------|----------|
| `FetchPage` | ① 在 `page_table_` 中查找，命中则 `pin_count++` + `replacer_->Pin()` 直接返回 ② 未命中则 `FindVictimPage` 获取可替换帧 ③ `UpdatePage` 处理脏页和页表 ④ `disk_manager_->read_page` 从磁盘读取 ⑤ `pin_count_=1`，`replacer_->Pin()` |
| `NewPage` | ① `FindVictimPage` 获取可替换帧 ② `AllocatePage` 分配新页号 ③ `UpdatePage` 更新页表和元数据 ④ `pin_count_=1`，`replacer_->Pin()` ⑤ 通过参数传出新 PageId |
| `UnpinPage` | ① 在页表中查找 ② `pin_count_--`，若 `is_dirty` 则置脏 ③ `pin_count_` 降为 0 时调 `replacer_->Unpin()` |
| `FlushPage` | 在页表中查找 → `write_page` 强制写磁盘 → 清脏位（不考虑 pin_count） |
| `DeletePage` | ① 不在页表中 → 直接 `DeallocatePage` 返回 true ② `pin_count_>0` → 返回 false ③ 脏页写回 → 从页表删除 → 重置页面 → 帧归还 `free_list_` → `replacer_->Pin()` 从淘汰列表移除 |
| `FlushAllPages` | 遍历所有帧，匹配 fd 且有效页号的页面全部写磁盘并清脏位（已提供，未修改） |

**并发控制**：所有公有函数使用 `std::scoped_lock latch_` 加锁，保证原子性。

---

## 四、RmFileHandle（任务 1.2.1）

**核心思路**：基于堆文件组织形式，每个文件由多个 Page 组成（Page 0 存文件头 `RmFileHdr`，后续 Page 存记录数据）。使用 `RmPageHandle` 封装单个 Page 的 `page_hdr`/`bitmap`/`slots` 三段式结构。空闲页通过 `first_free_page_no` → `next_free_page_no` 链表管理。

### 辅助函数

| 函数 | 实现逻辑 |
|------|----------|
| `fetch_page_handle` | 构造 PageId → `buffer_pool_manager_->FetchPage()` → 封装为 `RmPageHandle` 返回（调用者需负责 Unpin） |
| `create_new_page_handle` | `buffer_pool_manager_->NewPage()` → 初始化 `page_hdr`（`num_records=0, next_free_page_no=-1`）→ `Bitmap::init` 清零 → 更新 `file_hdr_.num_pages++` 和 `file_hdr_.first_free_page_no` |
| `create_page_handle` | `file_hdr_.first_free_page_no != -1` → `fetch_page_handle` 获取空闲页；否则 `create_new_page_handle` 新建 |
| `release_page_handle` | 将当前页的 `next_free_page_no` 指向原空闲链头 → `file_hdr_.first_free_page_no` 更新为当前页（头插法链入空闲链表） |

### 公有函数

| 函数 | 实现逻辑 |
|------|----------|
| `get_record` | `fetch_page_handle` → `page_handle.get_slot(slot_no)` 拷贝记录数据到 `RmRecord` → Unpin |
| `insert_record` | `create_page_handle` 获取空闲页 → `Bitmap::first_bit(false, ...)` 找空闲 slot → `memcpy` 写入数据 → `Bitmap::set` 标记占用 → `num_records++` → 页面满时更新 `file_hdr_.first_free_page_no` 跳过此页 → Unpin(dirty) |
| `delete_record` | `fetch_page_handle` → `Bitmap::reset` 清除标记 → `num_records--` → 若从满变未满（`num_records == num_records_per_page - 1`）调 `release_page_handle` → Unpin(dirty) |
| `update_record` | `fetch_page_handle` → `memcpy` 覆盖 slot 数据 → Unpin(dirty) |

---

## 五、RmScan（任务 1.2.2）

**核心思路**：遍历文件中所有数据页，利用 Bitmap 查找每个页面中已存放记录的 slot，通过 `rid_` 维护当前迭代位置。

| 函数 | 实现逻辑 |
|------|----------|
| 构造函数 | `rid_` 初始化为 `{RM_FIRST_RECORD_PAGE, -1}`；若无数据页直接设为结束状态 `{RM_NO_PAGE, -1}`；否则调 `next()` 定位首条记录 |
| `next` | 循环遍历 page_no ∈ [1, num_pages)：对每个页调 `fetch_page_handle` → `Bitmap::next_bit(true, bitmap, num_records_per_page, slot_no)` 找下一个已占用 slot → 找到则更新 `rid_` 并返回 → 当前页无记录则 page_no++、slot_no=-1 继续下一页 → 全部遍历完设 `page_no=RM_NO_PAGE` |
| `is_end` | `rid_.page_no == RM_NO_PAGE` |
| `rid` | 直接返回 `rid_` |

**注意事项**：
- 因 `RmScan` 持有 `const RmFileHandle*`，而 `get_file_hdr()` 非 const 方法，改为直接访问友元私有成员 `file_handle_->file_hdr_` 解决 const 兼容问题
- 每次调用 `fetch_page_handle` 后必须 `UnpinPage` 防止缓冲池泄漏

---

## 整体调用关系图

```
RmScan / RmFileHandle
    ↓ FetchPage / NewPage / UnpinPage
BufferPoolManager
    ↓ read_page / write_page / AllocatePage
DiskManager
    ↓ POSIX I/O
Linux 文件系统
```
