/* Intrusive red-black tree, NULL leaves are black. Included only by hash_table.c. */
static int compare_key(uint64_t hash, const void *key, size_t length, const hash_table_entry *entry)
{
    if (hash != entry->hash) {
        return hash < entry->hash ? -1 : 1;
    }
    const size_t common = length < entry->key_len ? length : entry->key_len;
    const int bytes = common ? memcmp(key, entry->key, common) : 0;
    if (bytes) {
        return bytes;
    }
    return length == entry->key_len ? 0 : (length < entry->key_len ? -1 : 1);
}

static int is_red(const hash_table_entry *entry)
{
    return entry && entry->red;
}

static hash_table_entry *tree_find(hash_table_entry *root, uint64_t hash, const void *key, size_t length)
{
    while (root) {
        const int comparison = compare_key(hash, key, length, root);
        if (!comparison) {
            return root;
        }
        root = comparison < 0 ? root->left : root->right;
    }
    return NULL;
}

/* Direction 0 rotates left, direction 1 rotates right. */
static void rotate_tree(hash_table_entry **root, hash_table_entry *node, int direction)
{
    hash_table_entry *pivot = direction ? node->left : node->right;
    if (direction) {
        node->left = pivot->right;
        if (node->left) {
            node->left->parent = node;
        }
        pivot->right = node;
    } else {
        node->right = pivot->left;
        if (node->right) {
            node->right->parent = node;
        }
        pivot->left = node;
    }
    pivot->parent = node->parent;
    if (!node->parent) {
        *root = pivot;
    } else if (node == node->parent->left) {
        node->parent->left = pivot;
    } else {
        node->parent->right = pivot;
    }
    node->parent = pivot;
}

static void tree_insert(hash_table_entry **root, hash_table_entry *node)
{
    hash_table_entry *parent = NULL;
    hash_table_entry **position = root;
    while (*position) {
        parent = *position;
        const int comparison = compare_key(node->hash, node->key, node->key_len, parent);
        position = comparison < 0 ? &parent->left : &parent->right;
    }
    node->left = node->right = NULL;
    node->parent = parent;
    node->red = 1;
    *position = node;
    while (is_red(node->parent)) {
        parent = node->parent;
        hash_table_entry *grandparent = parent->parent;
        const int parent_right = parent == grandparent->right;
        hash_table_entry *uncle = parent_right ? grandparent->left : grandparent->right;
        if (is_red(uncle)) {
            parent->red = 0;
            uncle->red = 0;
            grandparent->red = 1;
            node = grandparent;
        } else {
            if (node == (parent_right ? parent->left : parent->right)) {
                node = parent;
                rotate_tree(root, node, parent_right);
                parent = node->parent;
            }
            parent->red = 0;
            grandparent->red = 1;
            rotate_tree(root, grandparent, !parent_right);
        }
    }
    (*root)->red = 0;
}

static void transplant_tree(hash_table_entry **root, hash_table_entry *old, hash_table_entry *replacement)
{
    if (!old->parent) {
        *root = replacement;
    } else if (old == old->parent->left) {
        old->parent->left = replacement;
    } else {
        old->parent->right = replacement;
    }
    if (replacement) {
        replacement->parent = old->parent;
    }
}

static void fix_tree_delete(hash_table_entry **root, hash_table_entry *node, hash_table_entry *parent)
{
    while (node != *root && !is_red(node)) {
        const int node_right = node == parent->right;
        hash_table_entry *sibling = node_right ? parent->left : parent->right;
        if (is_red(sibling)) {
            sibling->red = 0;
            parent->red = 1;
            rotate_tree(root, parent, node_right);
            sibling = node_right ? parent->left : parent->right;
        }
        /* A valid tree's double-black non-root always has a sibling. */
        hash_table_entry *near_child = node_right ? sibling->right : sibling->left;
        hash_table_entry *far_child = node_right ? sibling->left : sibling->right;
        if (!is_red(near_child) && !is_red(far_child)) {
            sibling->red = 1;
            node = parent;
            parent = node->parent;
        } else {
            if (!is_red(far_child)) {
                near_child->red = 0;
                sibling->red = 1;
                rotate_tree(root, sibling, !node_right);
                sibling = node_right ? parent->left : parent->right;
                far_child = node_right ? sibling->left : sibling->right;
            }
            sibling->red = parent->red;
            parent->red = 0;
            far_child->red = 0;
            rotate_tree(root, parent, node_right);
            node = *root;
            parent = NULL;
        }
    }
    if (node) {
        node->red = 0;
    }
}

static void tree_delete(hash_table_entry **root, hash_table_entry *node)
{
    hash_table_entry *removed = node;
    hash_table_entry *child;
    hash_table_entry *parent;
    int removed_red = removed->red;
    if (!node->left) {
        child = node->right;
        parent = node->parent;
        transplant_tree(root, node, child);
    } else if (!node->right) {
        child = node->left;
        parent = node->parent;
        transplant_tree(root, node, child);
    } else {
        removed = node->right;
        while (removed->left) {
            removed = removed->left;
        }
        removed_red = removed->red;
        child = removed->right;
        if (removed->parent == node) {
            parent = removed;
            if (child) {
                child->parent = removed;
            }
        } else {
            parent = removed->parent;
            transplant_tree(root, removed, child);
            removed->right = node->right;
            removed->right->parent = removed;
        }
        transplant_tree(root, node, removed);
        removed->left = node->left;
        removed->left->parent = removed;
        removed->red = node->red;
    }
    if (!removed_red) {
        fix_tree_delete(root, child, parent);
    }
}
