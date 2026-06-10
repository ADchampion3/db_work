#include "ix_index_handle.h"

#include "ix_scan.h"

IxIndexHandle::IxIndexHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
    : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
    // init file_hdr_
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, (char *)&file_hdr_, sizeof(file_hdr_));
    // disk_manager管理的fd对应的文件中，设置从原来编号+1开始分配page_no
    disk_manager_->set_fd2pageno(fd, disk_manager_->get_fd2pageno(fd) + 1);
}

/**
 * @brief 用于查找指定键所在的叶子结点
 *
 * @param key 要查找的目标key值
 * @param operation 查找到目标键值对后要进行的操作类型
 * @param transaction 事务参数，如果不需要则默认传入nullptr
 * @return 返回目标叶子结点
 * @note need to Unpin the leaf node outside!
 */
IxNodeHandle *IxIndexHandle::FindLeafPage(const char *key, Operation operation, Transaction *transaction) {
    IxNodeHandle *node = FetchNode(file_hdr_.root_page);

    // 从根节点向下查找，直到叶子结点
    while (!node->IsLeafPage()) {
        page_id_t child_page_no = node->InternalLookup(key);
        buffer_pool_manager_->UnpinPage(node->GetPageId(), false);
        node = FetchNode(child_page_no);
    }

    return node;
}

/**
 * @brief 用于查找指定键在叶子结点中的对应的值result
 *
 * @param key 查找的目标key值
 * @param result 用于存放结果的容器
 * @param transaction 事务指针
 * @return bool 返回目标键值对是否存在
 */
bool IxIndexHandle::GetValue(const char *key, std::vector<Rid> *result, Transaction *transaction) {
    std::scoped_lock<std::mutex> lock(root_latch_);

    IxNodeHandle *leaf = FindLeafPage(key, Operation::FIND, transaction);

    Rid *value = nullptr;
    bool found = leaf->LeafLookup(key, &value);
    if (found) {
        result->push_back(*value);
    }

    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
    return found;
}

/**
 * @brief 将指定键值对插入到B+树中
 *
 * @param (key, value) 要插入的键值对
 * @param transaction 事务指针
 * @return 是否插入成功
 */
bool IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *transaction) {
    std::scoped_lock<std::mutex> lock(root_latch_);

    // 查找目标叶子结点
    IxNodeHandle *leaf = FindLeafPage(key, Operation::INSERT, transaction);

    // 在叶子结点中插入键值对
    int new_size = leaf->Insert(key, value);

    // 3. 如果结点已满（size == max_size），进行分裂
    if (new_size == leaf->GetMaxSize()) {
        bool is_last_leaf = (leaf->GetPageNo() == file_hdr_.last_leaf);

        IxNodeHandle *new_leaf = Split(leaf);
        InsertIntoParent(leaf, new_leaf->get_key(0), new_leaf, transaction);

        if (is_last_leaf) {
            file_hdr_.last_leaf = new_leaf->GetPageNo();
        }

        buffer_pool_manager_->UnpinPage(new_leaf->GetPageId(), true);
    }

    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true);
    return true;
}

/**
 * @brief 将传入的一个node拆分(Split)成两个结点，在node的右边生成一个新结点new node
 *
 * @param node 需要拆分的结点
 * @return 拆分得到的new_node
 * @note 本函数执行完毕后，原node和new node都需要在函数外面进行unpin
 */
IxNodeHandle *IxIndexHandle::Split(IxNodeHandle *node) {
  
    IxNodeHandle *new_node = CreateNode();
    new_node->page_hdr->is_leaf = node->IsLeafPage();
    new_node->page_hdr->num_key = 0;
    new_node->page_hdr->parent = node->GetParentPageNo();
    new_node->page_hdr->next_free_page_no = IX_NO_PAGE;

    // 计算分裂点
    int total = node->GetSize();
    int mid = (total + 1) / 2;  // 左半保留 [0, mid)，右半移到新结点 [mid, total)
    int right_size = total - mid;


    new_node->insert_pairs(0, node->get_key(mid), node->get_rid(mid), right_size);
    node->SetSize(mid);

    if (node->IsLeafPage()) {
        new_node->page_hdr->next_leaf = node->GetNextLeaf();
        new_node->page_hdr->prev_leaf = node->GetPageNo();

        if (node->GetNextLeaf() != IX_NO_PAGE) {
            IxNodeHandle *next_leaf = FetchNode(node->GetNextLeaf());
            next_leaf->SetPrevLeaf(new_node->GetPageNo());
            buffer_pool_manager_->UnpinPage(next_leaf->GetPageId(), true);
        }
        node->SetNextLeaf(new_node->GetPageNo());
    } else {
        for (int i = 0; i < new_node->GetSize(); i++) {
            maintain_child(new_node, i);
        }
    }

    return new_node;
}

/**
 * @brief Insert key & value pair into internal page after split
 * 拆分(Split)后，向上找到old_node的父结点
 * 将new_node的第一个key插入到父结点，其位置在 父结点指向old_node的孩子指针 之后
 * 如果插入后>=maxsize，则必须继续拆分父结点，然后在其父结点的父结点再插入，即需要递归
 * 直到找到的old_node为根结点时，结束递归（此时将会新建一个根R，关键字为key，old_node和new_node为其孩子）
 *
 * @param (old_node, new_node) 原结点为old_node，old_node被分裂之后产生了新的右兄弟结点new_node
 * @param key 要插入parent的key
 * @note 一个结点插入了键值对之后需要分裂，分裂后左半部分的键值对保留在原结点，在参数中称为old_node，
 * 右半部分的键值对分裂为新的右兄弟节点，在参数中称为new_node（参考Split函数来理解old_node和new_node）
 * @note 本函数执行完毕后，new node和old node都需要在函数外面进行unpin
 */
void IxIndexHandle::InsertIntoParent(IxNodeHandle *old_node, const char *key, IxNodeHandle *new_node,
                                     Transaction *transaction) {
    if (old_node->IsRootPage()) {
        IxNodeHandle *new_root = CreateNode();
        new_root->page_hdr->is_leaf = false;
        new_root->page_hdr->num_key = 0;
        new_root->page_hdr->parent = IX_NO_PAGE;
        new_root->page_hdr->next_free_page_no = IX_NO_PAGE;

        new_root->insert_pair(0, old_node->get_key(0), Rid{old_node->GetPageNo(), -1});
        // 再插入new_node
        new_root->insert_pair(1, key, Rid{new_node->GetPageNo(), -1});

        // 更新old_node和new_node的父指针
        old_node->SetParentPageNo(new_root->GetPageNo());
        new_node->SetParentPageNo(new_root->GetPageNo());

        // 更新根结点
        file_hdr_.root_page = new_root->GetPageNo();

        buffer_pool_manager_->UnpinPage(new_root->GetPageId(), true);
        return;
    }


    IxNodeHandle *parent = FetchNode(old_node->GetParentPageNo());

    int idx = parent->find_child(old_node);

    parent->insert_pair(idx + 1, key, Rid{new_node->GetPageNo(), -1});

    new_node->SetParentPageNo(parent->GetPageNo());

    if (parent->GetSize() == parent->GetMaxSize()) {
        IxNodeHandle *new_parent = Split(parent);
        InsertIntoParent(parent, new_parent->get_key(0), new_parent, transaction);
        buffer_pool_manager_->UnpinPage(new_parent->GetPageId(), true);
    }

    buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
}

/**
 * @brief 用于删除B+树中含有指定key的键值对
 *
 * @param key 要删除的key值
 * @param transaction 事务指针
 * @return 是否删除成功
 */
bool IxIndexHandle::delete_entry(const char *key, Transaction *transaction) {
    std::scoped_lock<std::mutex> lock(root_latch_);

    // 获取目标叶子结点
    IxNodeHandle *leaf = FindLeafPage(key, Operation::DELETE, transaction);

    // 在叶子结点中删除键值对
    int old_size = leaf->GetSize();
    int new_size = leaf->Remove(key);

    if (new_size == old_size) {
        buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true);
        return false;
    }

    CoalesceOrRedistribute(leaf, transaction);

    return true;
}

/**
 * @brief 用于处理合并和重分配的逻辑，用于删除键值对后调用
 *
 * @param node 执行完删除操作的结点
 * @param transaction 事务指针
 * @param root_is_latched 传出参数：根节点是否上锁，用于并发操作
 * @return 是否需要删除结点
 * @note User needs to first find the sibling of input page.
 * If sibling's size + input page's size >= 2 * page's minsize, then redistribute.
 * Otherwise, merge(Coalesce).
 */
bool IxIndexHandle::CoalesceOrRedistribute(IxNodeHandle *node, Transaction *transaction) {
    // 结点不需要调整
    if (node->GetSize() >= node->GetMinSize()) {
        buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
        return false;
    }

    // 根结点
    if (node->IsRootPage()) {
        bool deleted = AdjustRoot(node);
        buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
        return deleted;
    }

    // 获取父结点和兄弟结点
    IxNodeHandle *parent = FetchNode(node->GetParentPageNo());
    int idx = parent->find_child(node);

    // 寻找兄弟结点
    IxNodeHandle *neighbor = nullptr;
    int neighbor_idx;
    if (idx > 0) {
        neighbor = FetchNode(parent->ValueAt(idx - 1));
        neighbor_idx = idx;
    } else {
        neighbor = FetchNode(parent->ValueAt(idx + 1));
        neighbor_idx = idx;
    }

    if (node->GetSize() + neighbor->GetSize() >= node->GetMinSize() * 2) {
        Redistribute(neighbor, node, parent, neighbor_idx);
        buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
        buffer_pool_manager_->UnpinPage(neighbor->GetPageId(), true);
        buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
        return false;
    } else {
        return Coalesce(&neighbor, &node, &parent, neighbor_idx, transaction);
    }
}

/**
 * @brief 用于当根结点被删除了一个键值对之后的处理
 *
 * @param old_root_node 原根节点
 * @return bool 根结点是否需要被删除
 * @note size of root page can be less than min size and this method is only called within coalesceOrRedistribute()
 */
bool IxIndexHandle::AdjustRoot(IxNodeHandle *old_root_node) {
    if (!old_root_node->IsLeafPage() && old_root_node->GetSize() == 1) {
        page_id_t new_root_page = old_root_node->RemoveAndReturnOnlyChild();

        // 更新新根的parent为INVALID
        IxNodeHandle *new_root = FetchNode(new_root_page);
        new_root->SetParentPageNo(IX_NO_PAGE);
        buffer_pool_manager_->UnpinPage(new_root->GetPageId(), true);

        // 更新文件头中的根结点
        file_hdr_.root_page = new_root_page;

        // 释放旧根
        release_node_handle(*old_root_node);
        return true;
    }

    // 叶子结点且为空：整棵B+树变空
    if (old_root_node->IsLeafPage() && old_root_node->GetSize() == 0) {
        file_hdr_.root_page = IX_NO_PAGE;
        release_node_handle(*old_root_node);
        return true;
    }

    return false;
}

/**
 * @brief 重新分配node和兄弟结点neighbor_node的键值对
 * Redistribute key & value pairs from one page to its sibling page. If index == 0, move sibling page's first key
 * & value pair into end of input "node", otherwise move sibling page's last key & value pair into head of input "node".
 *
 * @param neighbor_node sibling page of input "node"
 * @param node input from method coalesceOrRedistribute()
 * @param parent the parent of "node" and "neighbor_node"
 * @param index node在parent中的rid_idx
 * @note node是之前刚被删除过一个key的结点
 * index=0，则neighbor是node后继结点，表示：node(left)      neighbor(right)
 * index>0，则neighbor是node前驱结点，表示：neighbor(left)  node(right)
 * 注意更新parent结点的相关kv对
 */
void IxIndexHandle::Redistribute(IxNodeHandle *neighbor_node, IxNodeHandle *node, IxNodeHandle *parent, int index) {
    if (index == 0) {
        // index==0: node在左(index 0)，neighbor在右(index 1)
        // 从neighbor取第一个键值对放到node末尾
        node->insert_pair(node->GetSize(), neighbor_node->get_key(0), *neighbor_node->get_rid(0));
        neighbor_node->erase_pair(0);

        // neighbor的最小key变了，需要更新parent中neighbor对应的key
        maintain_parent(neighbor_node);

        // 如果是内部结点，更新被移动的孩子结点的父指针
        maintain_child(node, node->GetSize() - 1);
    } else {
        // index>0: neighbor在左(index idx-1)，node在右(index idx)
        // 从neighbor取最后一个键值对放到node开头
        int last = neighbor_node->GetSize() - 1;
        node->insert_pair(0, neighbor_node->get_key(last), *neighbor_node->get_rid(last));
        neighbor_node->erase_pair(last);

        // node的最小key变了，需要更新parent中node对应的key
        maintain_parent(node);

        // 如果是内部结点，更新被移动的孩子结点的父指针
        maintain_child(node, 0);
    }
}

/**
 * @brief 合并(Coalesce)函数是将node和其直接前驱进行合并，也就是和它左边的neighbor_node进行合并；
 * 假设node一定在右边。如果上层传入的index=0，说明node在左边，那么交换node和neighbor_node，保证node在右边；合并到左结点，实际上就是删除了右结点；
 * Move all the key & value pairs from one page to its sibling page, and notify buffer pool manager to delete this page.
 * Parent page must be adjusted to take info of deletion into account. Remember to deal with coalesce or redistribute
 * recursively if necessary.
 *
 * @param neighbor_node sibling page of input "node" (neighbor_node是node的前结点)
 * @param node input from method coalesceOrRedistribute() (node结点是需要被删除的)
 * @param parent parent page of input "node"
 * @param index node在parent中的rid_idx
 * @return true means parent node should be deleted, false means no deletion happend
 * @note Assume that *neighbor_node is the left sibling of *node (neighbor -> node)
 */
bool IxIndexHandle::Coalesce(IxNodeHandle **neighbor_node, IxNodeHandle **node, IxNodeHandle **parent, int index,
                             Transaction *transaction) {
    // 确定左结点和右结点
    IxNodeHandle *left_node, *right_node;
    if (index == 0) {
        // node在左，neighbor在右
        left_node = *node;
        right_node = *neighbor_node;
    } else {
        // neighbor在左，node在右
        left_node = *neighbor_node;
        right_node = *node;
    }

    int right_size = right_node->GetSize();
    left_node->insert_pairs(left_node->GetSize(), right_node->get_key(0), right_node->get_rid(0), right_size);

    if (!right_node->IsLeafPage()) {
        for (int i = 0; i < right_size; i++) {
            maintain_child(left_node, left_node->GetSize() - right_size + i);
        }
    }

    if (right_node->IsLeafPage()) {
        if (right_node->GetPageNo() == file_hdr_.last_leaf) {
            file_hdr_.last_leaf = left_node->GetPageNo();
        }
        erase_leaf(right_node);
    }

    // 在parent中删除right_node对应的键值对
    int right_idx_in_parent = (*parent)->find_child(right_node);
    (*parent)->erase_pair(right_idx_in_parent);

    // 释放并删除right_node
    release_node_handle(*right_node);
    buffer_pool_manager_->UnpinPage(right_node->GetPageId(), true);
    buffer_pool_manager_->DeletePage(right_node->GetPageId());

    buffer_pool_manager_->UnpinPage(left_node->GetPageId(), true);

    return CoalesceOrRedistribute(*parent, transaction);
}

/** -- 以下为辅助函数 -- */
/**
 * @brief 获取一个指定结点
 *
 * @param page_no
 * @return IxNodeHandle*
 * @note pin the page, remember to unpin it outside!
 */
IxNodeHandle *IxIndexHandle::FetchNode(int page_no) const {
    // assert(page_no < file_hdr_.num_pages); // 不再生效，由于删除操作，page_no可以大于个数
    Page *page = buffer_pool_manager_->FetchPage(PageId{fd_, page_no});
    IxNodeHandle *node = new IxNodeHandle(&file_hdr_, page);
    return node;
}

/**
 * @brief 创建一个新结点
 *
 * @return IxNodeHandle*
 * @note pin the page, remember to unpin it outside!
 * 注意：对于Index的处理是，删除某个页面后，认为该被删除的页面是free_page
 * 而first_free_page实际上就是最新被删除的页面，初始为IX_NO_PAGE
 * 在最开始插入时，一直是create node，那么first_page_no一直没变，一直是IX_NO_PAGE
 * 与Record的处理不同，Record将未插入满的记录页认为是free_page
 */
IxNodeHandle *IxIndexHandle::CreateNode() {
    file_hdr_.num_pages++;
    PageId new_page_id = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    // 从3开始分配page_no，第一次分配之后，new_page_id.page_no=3，file_hdr_.num_pages=4
    Page *page = buffer_pool_manager_->NewPage(&new_page_id);
    // 注意，和Record的free_page定义不同，此处【不能】加上：file_hdr_.first_free_page_no = page->GetPageId().page_no
    IxNodeHandle *node = new IxNodeHandle(&file_hdr_, page);
    return node;
}

/**
 * @brief 从node开始更新其父节点的第一个key，一直向上更新直到根节点
 *
 * @param node
 */
void IxIndexHandle::maintain_parent(IxNodeHandle *node) {
    IxNodeHandle *curr = node;
    while (curr->GetParentPageNo() != IX_NO_PAGE) {
        // Load its parent
        IxNodeHandle *parent = FetchNode(curr->GetParentPageNo());
        int rank = parent->find_child(curr);
        char *parent_key = parent->get_key(rank);
        // char *child_max_key = curr.get_key(curr.page_hdr->num_key - 1);
        char *child_first_key = curr->get_key(0);
        if (memcmp(parent_key, child_first_key, file_hdr_.col_len) == 0) {
            assert(buffer_pool_manager_->UnpinPage(parent->GetPageId(), true));
            break;
        }
        memcpy(parent_key, child_first_key, file_hdr_.col_len);  // 修改了parent node
        curr = parent;

        assert(buffer_pool_manager_->UnpinPage(parent->GetPageId(), true));
    }
}

/**
 * @brief 要删除leaf之前调用此函数，更新leaf前驱结点的next指针和后继结点的prev指针
 *
 * @param leaf 要删除的leaf
 */
void IxIndexHandle::erase_leaf(IxNodeHandle *leaf) {
    assert(leaf->IsLeafPage());

    IxNodeHandle *prev = FetchNode(leaf->GetPrevLeaf());
    prev->SetNextLeaf(leaf->GetNextLeaf());
    buffer_pool_manager_->UnpinPage(prev->GetPageId(), true);

    IxNodeHandle *next = FetchNode(leaf->GetNextLeaf());
    next->SetPrevLeaf(leaf->GetPrevLeaf());  // 注意此处是SetPrevLeaf()
    buffer_pool_manager_->UnpinPage(next->GetPageId(), true);
}

/**
 * @brief 删除node时，更新file_hdr_.num_pages
 *
 * @param node
 */
void IxIndexHandle::release_node_handle(IxNodeHandle &node) { file_hdr_.num_pages--; }

/**
 * @brief 将node的第child_idx个孩子结点的父节点置为node
 */
void IxIndexHandle::maintain_child(IxNodeHandle *node, int child_idx) {
    if (!node->IsLeafPage()) {
        //  Current node is inner node, load its child and set its parent to current node
        int child_page_no = node->ValueAt(child_idx);
        IxNodeHandle *child = FetchNode(child_page_no);
        child->SetParentPageNo(node->GetPageNo());
        buffer_pool_manager_->UnpinPage(child->GetPageId(), true);
    }
}

/**
 * @brief 这里把iid转换成了rid，即iid的slot_no作为node的rid_idx(key_idx)
 * node其实就是把slot_no作为键值对数组的下标
 * 换而言之，每个iid对应的索引槽存了一对(key,rid)，指向了(要建立索引的属性首地址,插入/删除记录的位置)
 *
 * @param iid
 * @return Rid
 * @note iid和rid存的不是一个东西，rid是上层传过来的记录位置，iid是索引内部生成的索引槽位置
 */
Rid IxIndexHandle::get_rid(const Iid &iid) const {
    IxNodeHandle *node = FetchNode(iid.page_no);
    if (iid.slot_no >= node->GetSize()) {
        throw IndexEntryNotFoundError();
    }
    buffer_pool_manager_->UnpinPage(node->GetPageId(), false);  // unpin it!
    return *node->get_rid(iid.slot_no);
}

/** --以下函数将用于lab3执行层-- */
/**
 * @brief FindLeafPage + lower_bound
 *
 * @param key
 * @return Iid
 * @note 上层传入的key本来是int类型，通过(const char *)&key进行了转换
 * 可用*(int *)key转换回去
 */
Iid IxIndexHandle::lower_bound(const char *key) {
    // int int_key = *(int *)key;
    // printf("my_lower_bound key=%d\n", int_key);

    IxNodeHandle *node = FindLeafPage(key, Operation::FIND, nullptr);
    int key_idx = node->lower_bound(key);

    Iid iid = {.page_no = node->GetPageNo(), .slot_no = key_idx};

    // unpin leaf node
    buffer_pool_manager_->UnpinPage(node->GetPageId(), false);
    return iid;
}

/**
 * @brief FindLeafPage + upper_bound
 *
 * @param key
 * @return Iid
 */
Iid IxIndexHandle::upper_bound(const char *key) {
    // int int_key = *(int *)key;
    // printf("my_upper_bound key=%d\n", int_key);

    IxNodeHandle *node = FindLeafPage(key, Operation::FIND, nullptr);
    int key_idx = node->upper_bound(key);

    Iid iid;
    if (key_idx == node->GetSize()) {
        // 这种情况无法根据iid找到rid，即后续无法调用ih->get_rid(iid)
        iid = leaf_end();
    } else {
        iid = {.page_no = node->GetPageNo(), .slot_no = key_idx};
    }

    // unpin leaf node
    buffer_pool_manager_->UnpinPage(node->GetPageId(), false);
    return iid;
}

/**
 * @brief 指向第一个叶子的第一个结点
 * 用处在于可以作为IxScan的第一个
 *
 * @return Iid
 */
Iid IxIndexHandle::leaf_begin() const {
    Iid iid = {.page_no = file_hdr_.first_leaf, .slot_no = 0};
    return iid;
}

/**
 * @brief 指向最后一个叶子的最后一个结点的后一个
 * 用处在于可以作为IxScan的最后一个
 *
 * @return Iid
 */
Iid IxIndexHandle::leaf_end() const {
    IxNodeHandle *node = FetchNode(file_hdr_.last_leaf);
    Iid iid = {.page_no = file_hdr_.last_leaf, .slot_no = node->GetSize()};
    buffer_pool_manager_->UnpinPage(node->GetPageId(), false);  // unpin it!
    return iid;
}
