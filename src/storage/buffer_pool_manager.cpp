#include "buffer_pool_manager.h"

/**
 * @brief 从free_list或replacer中得到可淘汰帧页的 *frame_id
 * @param frame_id 帧页id指针,返回成功找到的可替换帧id
 * @return true: 可替换帧查找成功 , false: 可替换帧查找失败
 */
bool BufferPoolManager::FindVictimPage(frame_id_t *frame_id) {
    // 1. 先从free_list_中找空闲帧
    if (!free_list_.empty()) {
        *frame_id = free_list_.front();
        free_list_.pop_front();
        return true;
    }
    // 2. free_list_为空，使用replacer淘汰页面
    return replacer_->Victim(frame_id);
}

/**
 * @brief 更新页面数据, 为脏页则需写入磁盘，更新page元数据(data, is_dirty, page_id)和page table
 *
 * @param page 写回页指针
 * @param new_page_id 写回页新page_id
 * @param new_frame_id 写回页新帧frame_id
 */
void BufferPoolManager::UpdatePage(Page *page, PageId new_page_id, frame_id_t new_frame_id) {
    // 1. 如果页面有有效page_id且是脏页，写回磁盘
    if (page->GetPageId().page_no != INVALID_PAGE_ID && page->IsDirty()) {
        disk_manager_->write_page(page->GetPageId().fd, page->GetPageId().page_no, page->GetData(), PAGE_SIZE);
    }
    // 2. 如果页面有有效page_id，从页表中删除旧映射
    if (page->GetPageId().page_no != INVALID_PAGE_ID) {
        page_table_.erase(page->GetPageId());
    }
    // 3. 更新页表
    page_table_[new_page_id] = new_frame_id;
    // 4. 重置page的元数据
    page->ResetMemory();
    page->id_ = new_page_id;
    page->is_dirty_ = false;
    page->pin_count_ = 0;
}

/**
 * Fetch the requested page from the buffer pool.
 * 如果页表中存在page_id（说明该page在缓冲池中），并且pin_count++。
 * 如果页表不存在page_id（说明该page在磁盘中），则找缓冲池victim page，将其替换为磁盘中读取的page，pin_count置1。
 * @param page_id id of page to be fetched
 * @return the requested page
 */
Page *BufferPoolManager::FetchPage(PageId page_id) {
    std::scoped_lock lock{latch_};

    // 1. 在页表中查找
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        // 页面在缓冲池中
        frame_id_t frame_id = it->second;
        Page *page = &pages_[frame_id];
        page->pin_count_++;
        replacer_->Pin(frame_id);
        return page;
    }

    // 2. 页面不在缓冲池中，找一个可替换的帧
    frame_id_t frame_id;
    if (!FindVictimPage(&frame_id)) {
        return nullptr;
    }

    Page *page = &pages_[frame_id];

    // 3. 更新页面
    UpdatePage(page, page_id, frame_id);

    // 4. 从磁盘读取页面数据
    disk_manager_->read_page(page_id.fd, page_id.page_no, page->GetData(), PAGE_SIZE);

    // 5. 固定页面
    page->pin_count_ = 1;
    replacer_->Pin(frame_id);

    return page;
}

/**
 * Unpin the target page from the buffer pool. 取消固定pin_count>0的在缓冲池中的page
 * @param page_id id of page to be unpinned
 * @param is_dirty true if the page should be marked as dirty, false otherwise
 * @return false if the page pin count is <= 0 before this call, true otherwise
 */
bool BufferPoolManager::UnpinPage(PageId page_id, bool is_dirty) {
    std::scoped_lock lock{latch_};

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }

    frame_id_t frame_id = it->second;
    Page *page = &pages_[frame_id];

    if (page->pin_count_ <= 0) {
        return false;
    }

    page->pin_count_--;
    if (is_dirty) {
        page->is_dirty_ = true;
    }
    if (page->pin_count_ == 0) {
        replacer_->Unpin(frame_id);
    }
    return true;
}

/**
 * Flushes the target page to disk. 将page写入磁盘；不考虑pin_count
 * @param page_id id of page to be flushed, cannot be INVALID_PAGE_ID
 * @return false if the page could not be found in the page table, true otherwise
 */
bool BufferPoolManager::FlushPage(PageId page_id) {
    std::scoped_lock lock{latch_};

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }

    frame_id_t frame_id = it->second;
    Page *page = &pages_[frame_id];

    disk_manager_->write_page(page->GetPageId().fd, page->GetPageId().page_no, page->GetData(), PAGE_SIZE);
    page->is_dirty_ = false;
    return true;
}

/**
 * Creates a new page in the buffer pool. 相当于从磁盘中移动一个新建的空page到缓冲池某个位置
 * @param[out] page_id id of created page
 * @return nullptr if no new pages could be created, otherwise pointer to new page
 */
Page *BufferPoolManager::NewPage(PageId *page_id) {
    std::scoped_lock lock{latch_};

    // 1. 找一个可替换的帧
    frame_id_t frame_id;
    if (!FindVictimPage(&frame_id)) {
        return nullptr;
    }

    // 2. 分配新的页面编号
    PageId new_page_id;
    new_page_id.fd = page_id->fd;
    new_page_id.page_no = disk_manager_->AllocatePage(page_id->fd);

    // 3. 更新页面
    Page *page = &pages_[frame_id];
    UpdatePage(page, new_page_id, frame_id);

    // 4. 固定页面
    page->pin_count_ = 1;
    replacer_->Pin(frame_id);

    // 5. 设置传出参数
    *page_id = new_page_id;

    return page;
}

/**
 * @brief Deletes a page from the buffer pool.
 * @param page_id id of page to be deleted
 * @return false if the page exists but could not be deleted, true if the page didn't exist or deletion succeeded
 */
bool BufferPoolManager::DeletePage(PageId page_id) {
    std::scoped_lock lock{latch_};

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        // 页面不在缓冲池中
        disk_manager_->DeallocatePage(page_id.page_no);
        return true;
    }

    frame_id_t frame_id = it->second;
    Page *page = &pages_[frame_id];

    // 如果页面仍在使用中，不能删除
    if (page->pin_count_ > 0) {
        return false;
    }

    // 从页表中删除
    page_table_.erase(page_id);

    // 如果是脏页，写回磁盘
    if (page->IsDirty()) {
        disk_manager_->write_page(page->GetPageId().fd, page->GetPageId().page_no, page->GetData(), PAGE_SIZE);
    }

    // 重置页面元数据
    page->ResetMemory();
    page->id_ = {page->GetPageId().fd, INVALID_PAGE_ID};
    page->is_dirty_ = false;
    page->pin_count_ = 0;

    // 归还到空闲列表
    free_list_.push_back(frame_id);

    // 从replacer中移除
    replacer_->pin(frame_id);

    disk_manager_->DeallocatePage(page_id.page_no);
    return true;
}

/**
 * @brief Flushes all the pages in the buffer pool to disk.
 *
 * @param fd 指定的diskfile open句柄
 */
void BufferPoolManager::FlushAllPages(int fd) {
    // example for disk write
    std::scoped_lock lock{latch_};
    for (size_t i = 0; i < pool_size_; i++) {
        Page *page = &pages_[i];
        if (page->GetPageId().fd == fd && page->GetPageId().page_no != INVALID_PAGE_ID) {
            disk_manager_->write_page(page->GetPageId().fd, page->GetPageId().page_no, page->GetData(), PAGE_SIZE);
            page->is_dirty_ = false;
        }
    }
}
