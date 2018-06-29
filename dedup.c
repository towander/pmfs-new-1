#include <linux/list.h>
#include <linux/rbtree.h>

// struct hash_map_addr{
//     unsigned hashing;
//     unsigned long hashing_md5;
//     void *addr;
//     unsigned int count;
//     struct list_head list; 
// };

struct hash_map_addr{
    size_t hashing;
    void *hashing_md5;
    void *addr;
    size_t length;
    size_t count;
    struct list_head list;
    struct rb_node node;
    struct list_head hashing_list;     
};

struct ref_map{
    void *virt_addr;
    void *phys_addr;
    struct hash_map_addr *hma;
    struct list_head list;
    struct rb_node node;
};