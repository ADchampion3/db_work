#include "ix_node_handle.h"

/**
 * @brief 在当前node中查找第一个>=target的key_idx
 *
 * @return key_idx，范围为[0,num_key)，如果返回的key_idx=num_key，则表示target大于最后一个key
 * @note 返回key index（同时也是rid index），作为slot no
 */
int IxNodeHandle::lower_bound(const char *target) const {
    // 二分查找第一个 >= target 的 key_idx
    int l = 0, r = page_hdr->num_key;
    while (l < r) {
        int mid = (l + r) / 2;
        if (ix_compare(get_key(mid), target, file_hdr->col_type, file_hdr->col_len) < 0) {
            l = mid + 1;
        } else {
            r = mid;
        }
    }
    return l;
}

/**
 * @brief 在当前node中查找第一个>target的key_idx
 *
 * @return key_idx，范围为[1,num_key)，如果返回的key_idx=num_key，则表示target大于等于最后一个key
 * @note 注意此处的范围从1开始
 */
int IxNodeHandle::upper_bound(const char *target) const {
    // 二分查找第一个 > target 的 key_idx
    int l = 0, r = page_hdr->num_key;
    while (l < r) {
        int mid = (l + r) / 2;
        if (ix_compare(get_key(mid), target, file_hdr->col_type, file_hdr->col_len) <= 0) {
            l = mid + 1;
        } else {
            r = mid;
        }
    }
    return l;
}

/**
 * @brief 用于叶子结点根据key来查找该结点中的键值对
 * 值value作为传出参数，函数返回是否查找成功
 *
 * @param key 目标key
 * @param[out] value 传出参数，目标key对应的Rid
 * @return 目标key是否存在
 */
bool IxNodeHandle::LeafLookup(const char *key, Rid **value) {
    int idx = lower_bound(key);
    if (idx < page_hdr->num_key && ix_compare(get_key(idx), key, file_hdr->col_type, file_hdr->col_len) == 0) {
        *value = get_rid(idx);
        return true;
    }
    return false;
}

/**
 * 用于内部结点（非叶子节点）查找目标key所在的孩子结点（子树）
 * @param key 目标key
 * @return page_id_t 目标key所在的孩子节点（子树）的存储页面编号
 */
page_id_t IxNodeHandle::InternalLookup(const char *key) {
    int idx = upper_bound(key);
    return get_rid(idx)->page_no;
}

/**
 * @brief 在指定位置插入n个连续的键值对
 * 将key的前n位插入到原来keys中的pos位置；将rid的前n位插入到原来rids中的pos位置
 *
 * @param pos 要插入键值对的位置
 * @param (key, rid) 连续键值对的起始地址，也就是第一个键值对，可以通过(key, rid)来获取n个键值对
 * @param n 键值对数量
 * @note [0,pos)           [pos,num_key)
 *                            key_slot
 *                            /      \
 *                           /        \
 *       [0,pos)     [pos,pos+n)   [pos+n,num_key+n)
 *                      key           key_slot
 */
void IxNodeHandle::insert_pairs(int pos, const char *key, const Rid *rid, int n) {
    int num_key = page_hdr->num_key;
    int col_len = file_hdr->col_len;

    memmove(get_key(pos + n), get_key(pos), (num_key - pos) * col_len);
    memmove(get_rid(pos + n), get_rid(pos), (num_key - pos) * sizeof(Rid));
    memcpy(get_key(pos), key, n * col_len);
    memcpy(get_rid(pos), rid, n * sizeof(Rid));

    page_hdr->num_key += n;
}

/**
 * @brief 用于在结点中的指定位置插入单个键值对
 */
void IxNodeHandle::insert_pair(int pos, const char *key, const Rid &rid) { insert_pairs(pos, key, &rid, 1); };

/**
 * @brief 用于在结点中插入单个键值对。
 * 函数返回插入后的键值对数量
 *
 * @param (key, value) 要插入的键值对
 * @return int 键值对数量
 */
int IxNodeHandle::Insert(const char *key, const Rid &value) {
    int pos = lower_bound(key);

    // 检查key是否重复
    if (pos < page_hdr->num_key && ix_compare(get_key(pos), key, file_hdr->col_type, file_hdr->col_len) == 0) {
        // key已存在，不插入
        return page_hdr->num_key;
    }

    // 插入键值对
    insert_pairs(pos, key, &value, 1);
    return page_hdr->num_key;
}

/**
 * @brief 用于在结点中的指定位置删除单个键值对
 *
 * @param pos 要删除键值对的位置
 */
void IxNodeHandle::erase_pair(int pos) {
    int num_key = page_hdr->num_key;
    int col_len = file_hdr->col_len;

    // 移动keys
    memmove(get_key(pos), get_key(pos + 1), (num_key - pos - 1) * col_len);

    // 移动rids
    memmove(get_rid(pos), get_rid(pos + 1), (num_key - pos - 1) * sizeof(Rid));

    // 更新键数量
    page_hdr->num_key--;
}

/**
 * @brief 用于在结点中删除指定key的键值对。函数返回删除后的键值对数量
 *
 * @param key 要删除的键值对key值
 * @return 完成删除操作后的键值对数量
 */
int IxNodeHandle::Remove(const char *key) {
    int pos = lower_bound(key);

    // 检查key是否存在
    if (pos < page_hdr->num_key && ix_compare(get_key(pos), key, file_hdr->col_type, file_hdr->col_len) == 0) {
        erase_pair(pos);
    }

    return page_hdr->num_key;
}

/**
 * @brief 由parent调用，寻找child，返回child在parent中的rid_idx∈[0,page_hdr->num_key)
 *
 * @param child
 * @return int
 */
int IxNodeHandle::find_child(IxNodeHandle *child) {
    int rid_idx;
    for (rid_idx = 0; rid_idx < page_hdr->num_key; rid_idx++) {
        if (get_rid(rid_idx)->page_no == child->GetPageNo()) {
            break;
        }
    }
    assert(rid_idx < page_hdr->num_key);
    return rid_idx;
}

/**
 * @brief used in internal node to remove the last key in root node, and return the last child
 *
 * @return the last child
 */
page_id_t IxNodeHandle::RemoveAndReturnOnlyChild() {
    assert(GetSize() == 1);
    page_id_t child_page_no = ValueAt(0);
    erase_pair(0);
    assert(GetSize() == 0);
    return child_page_no;
}
