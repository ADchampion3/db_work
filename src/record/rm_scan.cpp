#include "rm_scan.h"

#include "rm_file_handle.h"

/**
 * @brief 初始化file_handle和rid
 *
 * @param file_handle
 */
RmScan::RmScan(const RmFileHandle *file_handle) : file_handle_(file_handle) {
    // 初始化rid_，指向第一个存放了记录的位置
    // 从第1个page开始搜索（page_no=1，page_no=0是文件头）
    rid_.page_no = RM_FIRST_RECORD_PAGE;
    rid_.slot_no = -1;

    // 如果没有数据页，直接设为结束
    if (rid_.page_no >= file_handle_->get_file_hdr().num_pages) {
        rid_.page_no = RM_NO_PAGE;
        rid_.slot_no = -1;
        return;
    }

    // 找到第一个有记录的位置
    next();
}

/**
 * @brief 找到文件中下一个存放了记录的位置
 */
void RmScan::next() {
    RmFileHdr file_hdr = file_handle_->get_file_hdr();

    // 遍历所有page
    while (rid_.page_no < file_hdr.num_pages) {
        RmPageHandle page_handle = file_handle_->fetch_page_handle(rid_.page_no);

        // 在当前page中查找下一个有记录的slot
        int slot_no = Bitmap::next_bit(true, page_handle.bitmap, file_hdr.num_records_per_page, rid_.slot_no);

        file_handle_->buffer_pool_manager_->UnpinPage(page_handle.page->GetPageId(), false);

        if (slot_no < file_hdr.num_records_per_page) {
            // 找到了有记录的slot
            rid_.slot_no = slot_no;
            return;
        }

        // 当前page中没有更多记录，查找下一个page
        rid_.page_no++;
        rid_.slot_no = -1;
    }

    // 所有page都遍历完了，到达末尾
    rid_.page_no = RM_NO_PAGE;
    rid_.slot_no = -1;
}

/**
 * @brief 判断是否到达文件末尾
 */
bool RmScan::is_end() const {
    return rid_.page_no == RM_NO_PAGE;
}

/**
 * @brief RmScan内部存放的rid
 */
Rid RmScan::rid() const {
    return rid_;
}
