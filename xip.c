/*
 * BRIEF DESCRIPTION
 *
 * XIP operations.
 *
 * Copyright 2012-2013 Intel Corporation
 * Copyright 2009-2011 Marco Stornelli <marco.stornelli@gmail.com>
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <asm/cpufeature.h>
#include <asm/pgtable.h>
#include "pmfs.h"
#include "xip.h"
/*dedup new add include*/
#include <linux/kernel.h>
#include <linux/string.h>

#include "dedup.c"

/* dedup claim start */
// struct lpn_map_ppn *l_map_p;
// l_map_p = kmalloc(sizeof(struct lpn_map_ppn), GFP_KERNEL);
static LIST_HEAD(hash_map_addr_list);
struct list_head *last_hit;
struct list_head *new_list = &hash_map_addr_list;
bool find_flag = false;
struct rb_root root = RB_ROOT;

/*
	dedup rbtree function
*/
struct hash_map_addr *rb_search_node(struct rb_root *root, size_t hashing)
{
	struct rb_node *entry_node = root->rb_node;
	int result;
	struct hash_map_addr *hash_map_addr_entry;
	
	while(entry_node){
		hash_map_addr_entry = rb_entry(entry_node, struct hash_map_addr, node);
		result = hashing - hash_map_addr_entry->hashing; 
		if(result < 0)
			entry_node = entry_node->rb_left;
		else if(result > 0)
			entry_node = entry_node->rb_right;
		else
			return hash_map_addr_entry;
	}
	return NULL;
}

void rb_insert_node(struct rb_root *root, struct hash_map_addr *hash_map_addr_new)
{
	struct rb_node **entry_node = &(root->rb_node);
	struct rb_node *parent = NULL;
	struct hash_map_addr *hash_map_addr_entry;

	while(*entry_node){
		parent = *entry_node;
		hash_map_addr_entry = rb_entry(*entry_node, struct hash_map_addr, node);
		
		if(hash_map_addr_new->hashing < hash_map_addr_entry->hashing)
			entry_node = &(*entry_node)->rb_left;
		else if(hash_map_addr_new->hashing > hash_map_addr_entry->hashing){
			entry_node = &(*entry_node)->rb_right;
		}	
		else{
			// printk("hashing1:%lu",hash_map_addr_new->hashing);
			// printk("hashing2:%lu",hash_map_addr_entry->hashing);
			if(hash_map_addr_entry){printk("hashing accident!");}
			return;
		}
	}
	printk("hash_map_addr_new->addr:%lu",(long unsigned)hash_map_addr_new->addr);
	printk("new hashing:%lu",hash_map_addr_new->hashing);
	rb_link_node(&hash_map_addr_new->node, parent, entry_node);
	rb_insert_color(&hash_map_addr_new->node, root);
}
/* claim end */

static ssize_t
do_xip_mapping_read(struct address_space *mapping,
		    struct file_ra_state *_ra,
		    struct file *filp,
		    char __user *buf,
		    size_t len,
		    loff_t *ppos)
{
	struct inode *inode = mapping->host;
	pgoff_t index, end_index;
	unsigned long offset;
	loff_t isize, pos;
	size_t copied = 0, error = 0;
	timing_t memcpy_time;

	pos = *ppos;
	index = pos >> PAGE_SHIFT;
	offset = pos & ~PAGE_MASK;

	isize = i_size_read(inode);
	if (!isize)
		goto out;

	end_index = (isize - 1) >> PAGE_SHIFT;
	do {
		unsigned long nr, left;
		void *xip_mem;
		unsigned long xip_pfn;
		int zero = 0;

		/* nr is the maximum number of bytes to copy from this page */
		nr = PAGE_SIZE;
		if (index >= end_index) {
			if (index > end_index)
				goto out;
			nr = ((isize - 1) & ~PAGE_MASK) + 1;
			if (nr <= offset) {
				goto out;
			}
		}
		nr = nr - offset;
		if (nr > len - copied)
			nr = len - copied;

		error = pmfs_get_xip_mem(mapping, index, 0,
					&xip_mem, &xip_pfn);
		if (unlikely(error)) {
			if (error == -ENODATA) {
				/* sparse */
				zero = 1;
			} else
				goto out;
		}

		/* If users can be writing to this page using arbitrary
		 * virtual addresses, take care about potential aliasing
		 * before reading the page on the kernel side.
		 */
		if (mapping_writably_mapped(mapping))
			/* address based flush */ ;

		/*
		 * Ok, we have the mem, so now we can copy it to user space...
		 *
		 * The actor routine returns how many bytes were actually used..
		 * NOTE! This may not be the same as how much of a user buffer
		 * we filled up (we may be padding etc), so we can only update
		 * "pos" here (the actor routine has to update the user buffer
		 * pointers and the remaining count).
		 */
		PMFS_START_TIMING(memcpy_r_t, memcpy_time);
		if (!zero)
			left = __copy_to_user(buf+copied, xip_mem+offset, nr);
		else
			left = __clear_user(buf + copied, nr);
		PMFS_END_TIMING(memcpy_r_t, memcpy_time);

		if (left) {
			error = -EFAULT;
			goto out;
		}

		copied += (nr - left);
		offset += (nr - left);
		index += offset >> PAGE_SHIFT;
		offset &= ~PAGE_MASK;
	} while (copied < len);

out:
	*ppos = pos + copied;
	if (filp)
		file_accessed(filp);

	return (copied ? copied : error);
}

ssize_t
xip_file_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
	if (!access_ok(VERIFY_WRITE, buf, len))
		return -EFAULT;

	return do_xip_mapping_read(filp->f_mapping, &filp->f_ra, filp,
			    buf, len, ppos);
}

/*
 * Wrappers. We need to use the rcu read lock to avoid
 * concurrent truncate operation. No problem for write because we held
 * i_mutex.
 */
ssize_t pmfs_xip_file_read(struct file *filp, char __user *buf,
			    size_t len, loff_t *ppos)
{
	ssize_t res;
	timing_t xip_read_time;

	PMFS_START_TIMING(xip_read_t, xip_read_time);
//	rcu_read_lock();
	res = xip_file_read(filp, buf, len, ppos);
//	rcu_read_unlock();
	PMFS_END_TIMING(xip_read_t, xip_read_time);
	return res;
}

static inline void pmfs_flush_edge_cachelines(loff_t pos, ssize_t len,
	void *start_addr)
{
	if (unlikely(pos & 0x7))
		pmfs_flush_buffer(start_addr, 1, false);
	if (unlikely(((pos + len) & 0x7) && ((pos & (CACHELINE_SIZE - 1)) !=
			((pos + len) & (CACHELINE_SIZE - 1)))))
		pmfs_flush_buffer(start_addr + len, 1, false);
}

static inline size_t memcpy_to_nvmm(char *kmem, loff_t offset,
	const char __user *buf, size_t bytes)
{
	size_t copied;

	if (support_clwb) {
		copied = bytes - __copy_from_user(kmem + offset, buf, bytes);
		pmfs_flush_buffer(kmem + offset, copied, 0);
	} else {
		copied = bytes - __copy_from_user_inatomic_nocache(kmem +
						offset, buf, bytes);
	}
	/* dedup start */
	// printk("kmem+offset length:%d",(int)strlen(kmem+offset));
	// printk("kmem+offset:%s\n",kmem+offset);
	// printk("kmem+offset:%lu\n",kmem+offset);
	// printk("kmem length:%d",(int)strlen(kmem));
	// printk("kmem:%s\n",kmem);
	// printk("kmem+offset:%lu\n",kmem);
	// printk("bytes value:%zu",bytes);
	// printk("copied value:%zu",copied);
	// printk("buf:%s",buf);
	// printk("kmem+offset:%s",kmem+offset);
	/* end */
	return copied;
}

static ssize_t
__pmfs_xip_file_write(struct address_space *mapping, const char __user *buf,
          size_t count, loff_t pos, loff_t *ppos)
{
	struct inode    *inode = mapping->host;
	struct super_block *sb = inode->i_sb;
	long        status = 0;
	size_t      bytes;
	ssize_t     written = 0;
	struct pmfs_inode *pi;
	timing_t memcpy_time, write_time;

	PMFS_START_TIMING(internal_write_t, write_time);
	pi = pmfs_get_inode(sb, inode->i_ino);
	do {
		unsigned long index;
		unsigned long offset;
		size_t copied;
		void *xmem;
		unsigned long xpfn;
		//dedup claiming start
		// unsigned hashing = 0;
		// unsigned long *temp = kmalloc(sizeof(unsigned long), GFP_KERNEL);
		// int i;
		
		// struct hash_map_addr *hash_map_addr_entry, *hash_map_addr_temp;
		// hash_map_addr_temp = kmalloc(sizeof(*hash_map_addr_temp), GFP_KERNEL);
		//end

		offset = (pos & (sb->s_blocksize - 1)); /* Within page */
		index = pos >> sb->s_blocksize_bits;
		bytes = sb->s_blocksize - offset;
		if (bytes > count)
			bytes = count;

		status = pmfs_get_xip_mem(mapping, index, 1, &xmem, &xpfn);
		
		if (status)
			break;

		PMFS_START_TIMING(memcpy_w_t, memcpy_time);
		pmfs_xip_mem_protect(sb, xmem + offset, bytes, 1);
		copied = memcpy_to_nvmm((char *)xmem, offset, buf, bytes);
		pmfs_xip_mem_protect(sb, xmem + offset, bytes, 0);
		PMFS_END_TIMING(memcpy_w_t, memcpy_time);
		
		/* page data hashing compute */
		/* 2 and 3 is randomly setting */
		// for(i=0;i<128;i++)
		// {
		// 	memcpy(temp,(char*)xmem+i*sizeof(unsigned),sizeof(unsigned));
		// 	hashing += *temp;
		// 	hashing += (hashing << 3);
		// 	hashing ^= (hashing >> 2);
		// }

		/* find from last hit point */
		// hash_map_addr_entry = list_entry(last_hit.next, struct hash_map_addr, list);
 		// if(find_flag == true && hashing == hash_map_addr_entry->hashing)
		// {
		// 	hash_map_addr_entry->count++;
		// 	last_hit.next = last_hit.next->next;
		// 	// printk("fast hit!\n");
		// 	/* add reference content */
		// 	goto find;
		// }
		
		// // last_hit.next = hash_map_addr_list.next;
		// hash_map_addr_entry = rb_search_node(&root, hashing);
		// if(hash_map_addr_entry){
		// 	hash_map_addr_entry->count++;
		// 	last_hit.next = hash_map_addr_entry->list.next;
		// 	find_flag = true;
		// 	// printk("hit!\n");
		// 	goto find;
		// 	/*add reference content */
		// }
		/* hash_map_addr_entry ponit reuse for traverse */
		// list_for_each_entry(hash_map_addr_entry,&hash_map_addr_list,list)
		// {	
		// 	if(hash_map_addr_entry->hashing == hashing)
		// 	{		
		// 		hash_map_addr_entry->count++;
		// 		// printk("find the hashing!\n");
		// 		// printk("hashing in this map entry:%lu\n",hash_map_addr_entry->hashing);
		// 		// printk("count in this map entry:%u\n",hash_map_addr_entry->count);
		// 		// find_flag = 1;
		// 		last_hit.next = hash_map_addr_entry->list.next;
		// 		find_flag = true;
		// 		// printk("general hit, reference count:%u\n", hash_map_addr_entry->count);
		// 		goto find;
		// 	}
		// }
		
		// // not dup, insert new index
		// find_flag = false;
		// // printk("not hash hit\n");
		// hash_map_addr_temp->hashing = hashing;
		// hash_map_addr_temp->count = 1;
		// // hash_map_addr_temp->addr = kmalloc(6*sizeof(char), GFP_KERNEL);
		// hash_map_addr_temp->addr = xmem;
		// INIT_LIST_HEAD(&hash_map_addr_temp->list);
		// list_add_tail(&hash_map_addr_temp->list, &hash_map_addr_list);
		rb_insert_node(&root, list_entry(new_list->next, struct hash_map_addr, list));
		// find:
		/* end */

		/* if start or end dest address is not 8 byte aligned, 
	 	 * __copy_from_user_inatomic_nocache uses cacheable instructions
	 	 * (instead of movnti) to write. So flush those cachelines. */
		pmfs_flush_edge_cachelines(pos, copied, xmem + offset);

        	if (likely(copied > 0)) {
			status = copied;

			if (status >= 0) {
				written += status;
				count -= status;
				pos += status;
				buf += status;
			}
		}
		if (unlikely(copied != bytes))
			if (status >= 0)
				status = -EFAULT;
		if (status < 0)
			break;	
	} while (count);

	*ppos = pos;
	/*
 	* No need to use i_size_read() here, the i_size
 	* cannot change under us because we hold i_mutex.
 	*/
	if (pos > inode->i_size) {
		i_size_write(inode, pos);
		pmfs_update_isize(inode, pi);
	}

	PMFS_END_TIMING(internal_write_t, write_time);
	return written ? written : status;
}

/* optimized path for file write that doesn't require a transaction. In this
 * path we don't need to allocate any new data blocks. So the only meta-data
 * modified in path is inode's i_size, i_ctime, and i_mtime fields */
static ssize_t pmfs_file_write_fast(struct super_block *sb, struct inode *inode,
	struct pmfs_inode *pi, const char __user *buf, size_t count, loff_t pos,
	loff_t *ppos, u64 block)
{
	void *xmem = pmfs_get_block(sb, block);
	size_t copied, ret = 0, offset;
	timing_t memcpy_time;

	offset = pos & (sb->s_blocksize - 1);

	PMFS_START_TIMING(memcpy_w_t, memcpy_time);
	pmfs_xip_mem_protect(sb, xmem + offset, count, 1);
	copied = memcpy_to_nvmm((char *)xmem, offset, buf, count);
	pmfs_xip_mem_protect(sb, xmem + offset, count, 0);
	PMFS_END_TIMING(memcpy_w_t, memcpy_time);

	pmfs_flush_edge_cachelines(pos, copied, xmem + offset);

	if (likely(copied > 0)) {
		pos += copied;
		ret = copied;
	}
	if (unlikely(copied != count && copied == 0))
		ret = -EFAULT;
	*ppos = pos;
	inode->i_ctime = inode->i_mtime = current_time(inode);
	if (pos > inode->i_size) {
		/* make sure written data is persistent before updating
	 	* time and size */
		PERSISTENT_MARK();
		i_size_write(inode, pos);
		PERSISTENT_BARRIER();
		pmfs_memunlock_inode(sb, pi);
		pmfs_update_time_and_size(inode, pi);
		pmfs_memlock_inode(sb, pi);
	} else {
		u64 c_m_time;
		/* update c_time and m_time atomically. We don't need to make the data
		 * persistent because the expectation is that the close() or an explicit
		 * fsync will do that. */
		c_m_time = (inode->i_ctime.tv_sec & 0xFFFFFFFF);
		c_m_time = c_m_time | (c_m_time << 32);
		pmfs_memunlock_inode(sb, pi);
		pmfs_memcpy_atomic(&pi->i_ctime, &c_m_time, 8);
		pmfs_memlock_inode(sb, pi);
	}
	pmfs_flush_buffer(pi, 1, false);
	return ret;
}

/*
 * blk_off is used in different ways depending on whether the edge block is
 * at the beginning or end of the write. If it is at the beginning, we zero from
 * start-of-block to 'blk_off'. If it is the end block, we zero from 'blk_off' to
 * end-of-block
 */
static inline void pmfs_clear_edge_blk (struct super_block *sb, struct
	pmfs_inode *pi, bool new_blk, unsigned long block, size_t blk_off,
	bool is_end_blk)
{
	void *ptr;
	size_t count;
	unsigned long blknr;

	if (new_blk) {
		blknr = block >> (pmfs_inode_blk_shift(pi) -
			sb->s_blocksize_bits);
		ptr = pmfs_get_block(sb, __pmfs_find_data_block(sb, pi, blknr));
		if (ptr != NULL) {
			if (is_end_blk) {
				ptr = ptr + blk_off - (blk_off % 8);
				count = pmfs_inode_blk_size(pi) -
					blk_off + (blk_off % 8);
			} else
				count = blk_off + (8 - (blk_off % 8));
			pmfs_memunlock_range(sb, ptr,  pmfs_inode_blk_size(pi));
			memset_nt(ptr, 0, count);
			pmfs_memlock_range(sb, ptr,  pmfs_inode_blk_size(pi));
		}
	}
}

ssize_t pmfs_xip_file_write(struct file *filp, const char __user *buf,
          size_t len, loff_t *ppos)
{
	struct address_space *mapping = filp->f_mapping;
	struct inode    *inode = mapping->host;
	struct super_block *sb = inode->i_sb;
	pmfs_transaction_t *trans;
	struct pmfs_inode *pi;
	ssize_t     written = 0;
	loff_t pos;
	u64 block;
	bool new_sblk = false, new_eblk = false;
	size_t count, offset, eblk_offset, ret;
	unsigned long start_blk, end_blk, num_blocks, max_logentries;
	bool same_block;
	timing_t xip_write_time, xip_write_fast_time;

	//dedup claiming start
	unsigned long *temp = kmalloc(sizeof(unsigned long), GFP_KERNEL);
	size_t i,j,hashing = 0;
	char *xmem;

	struct hash_map_addr *hash_map_addr_entry, *hash_map_addr_temp;
	hash_map_addr_temp = kmalloc(sizeof(*hash_map_addr_temp), GFP_KERNEL);
	//end

	PMFS_START_TIMING(xip_write_t, xip_write_time);

	sb_start_write(inode->i_sb);
	inode_lock(inode);

	if (!access_ok(VERIFY_READ, buf, len)) {
		ret = -EFAULT;
		goto out;
	}
	pos = *ppos;
	count = len;
	if (count == 0) {
		ret = 0;
		goto out;
	}

	pi = pmfs_get_inode(sb, inode->i_ino);

	offset = pos & (sb->s_blocksize - 1);
	num_blocks = ((count + offset - 1) >> sb->s_blocksize_bits) + 1;
	/* offset in the actual block size block */
	offset = pos & (pmfs_inode_blk_size(pi) - 1);
	start_blk = pos >> sb->s_blocksize_bits;
	end_blk = start_blk + num_blocks - 1;

	block = pmfs_find_data_block(inode, start_blk);

	//dedup insert start
	// printk("num_blocks:%lu\n",num_blocks);
	// printk("ino:%lu\n",inode->i_ino);
	// printk("offset:%lu\n",offset);
	// printk("pos:%llu\n",pos);
	// printk("s_blocksize_bits:%u",sb->s_blocksize_bits);
	// printk("start_blk:%lu\n",start_blk);
	// printk("end_blk:%lu\n",end_blk);
	// printk("count:%lu\n",count);
	// printk("start_blk>>5:%lu\n",start_blk>>5);
	// printk("strlen(buf):%lu\n",(long unsigned)strlen(*buf));
	// printk("\n");
	//end

	/* Referring to the inode's block size, not 4K */
	same_block = (((count + offset - 1) >>
			pmfs_inode_blk_shift(pi)) == 0) ? 1 : 0;
	if (block && same_block) {
		PMFS_START_TIMING(xip_write_fast_t, xip_write_fast_time);
		ret = pmfs_file_write_fast(sb, inode, pi, buf, count, pos,
			ppos, block);
		PMFS_END_TIMING(xip_write_fast_t, xip_write_fast_time);
		goto out;
	}
	max_logentries = num_blocks / MAX_PTRS_PER_LENTRY + 2;
	if (max_logentries > MAX_METABLOCK_LENTRIES)
		max_logentries = MAX_METABLOCK_LENTRIES;

	trans = pmfs_new_transaction(sb, MAX_INODE_LENTRIES + max_logentries);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		goto out;
	}
	pmfs_add_logentry(sb, trans, pi, MAX_DATA_PER_LENTRY, LE_DATA);

	ret = file_remove_privs(filp);
	if (ret) {
		pmfs_abort_transaction(sb, trans);
		goto out;
	}
	inode->i_ctime = inode->i_mtime = current_time(inode);
	pmfs_update_time(inode, pi);

	i = count;
	xmem = kmalloc(pmfs_inode_blk_size(pi),GFP_KERNEL);
	do{	
		if (i>pmfs_inode_blk_size(pi)){
			copy_from_user(xmem, buf, pmfs_inode_blk_size(pi));
			printk("i:%lu",i);
			for(j=0;j<128;j++){
			memcpy(temp,xmem+j*sizeof(unsigned),sizeof(unsigned long));
			hashing += *temp;
			hashing += (hashing << 3);
			hashing ^= (hashing >> 2);
			}
		}
		else{
			copy_from_user(xmem, buf, i);
			printk("last i:%lu",i);
			for(j=0;j<128&&(j<i/sizeof(unsigned long));j++){
			memcpy(temp,xmem+j*sizeof(unsigned),sizeof(unsigned long));
			hashing += *temp;
			hashing += (hashing << 3);
			hashing ^= (hashing >> 2);
			}
		}
		
 		if(find_flag == true)
		{
			if(last_hit->next == NULL)
				;
			else{	
				hash_map_addr_entry = list_entry(last_hit->next, struct hash_map_addr, list);
				if(hashing == hash_map_addr_entry->hashing){
					hash_map_addr_entry->count++;
					last_hit = last_hit->next;
					printk("fast hit!\n");
					/* add reference content */
					goto find;
				}
			}
		}
		
		hash_map_addr_entry = rb_search_node(&root, hashing);
		if(hash_map_addr_entry){
			hash_map_addr_entry->count++;
			last_hit = &hash_map_addr_entry->list;
			find_flag = true;
			printk("hit!\n");
			goto find;
			/*add reference content */
		}

		find_flag = false;
		printk("not hash hit\n");
		hash_map_addr_temp->hashing = hashing;
		hash_map_addr_temp->count = 1;
		hash_map_addr_temp->addr = (void*)xmem;

		INIT_LIST_HEAD(&hash_map_addr_temp->list);
		list_add_tail(&hash_map_addr_temp->list, &hash_map_addr_list);
		// rb_insert_node(&root, hash_map_addr_temp);
		
		find:
		// printk("pmfs_inode_blk_size(pi):%u",pmfs_inode_blk_size(pi));
		// printk("count:%u",count);
		printk("hashing:%lu",hashing);
		printk("\n");
		
		i -= pmfs_inode_blk_size(pi);
	}while(i-1<count);

	/* We avoid zeroing the alloc'd range, which is going to be overwritten
	 * by this system call anyway */
	if (offset != 0) {
		if (pmfs_find_data_block(inode, start_blk) == 0)
		    new_sblk = true;
	}

	eblk_offset = (pos + count) & (pmfs_inode_blk_size(pi) - 1);
	if ((eblk_offset != 0) &&
			(pmfs_find_data_block(inode, end_blk) == 0))
		new_eblk = true;

	/* don't zero-out the allocated blocks */
	pmfs_alloc_blocks(trans, inode, start_blk, num_blocks, false);

	/* now zero out the edge blocks which will be partially written */
	pmfs_clear_edge_blk(sb, pi, new_sblk, start_blk, offset, false);
	pmfs_clear_edge_blk(sb, pi, new_eblk, end_blk, eblk_offset, true);

	written = __pmfs_xip_file_write(mapping, buf, count, pos, ppos);
	if (written < 0 || written != count)
		pmfs_dbg_verbose("write incomplete/failed: written %ld len %ld"
			" pos %llx start_blk %lx num_blocks %lx\n",
			written, count, pos, start_blk, num_blocks);

	pmfs_commit_transaction(sb, trans);
	ret = written;
out:
	inode_unlock(inode);
	sb_end_write(inode->i_sb);
	PMFS_END_TIMING(xip_write_t, xip_write_time);
	return ret;
}

/* OOM err return with xip file fault handlers doesn't mean anything.
 * It would just cause the OS to go an unnecessary killing spree !
 */
static int __pmfs_xip_file_fault(struct vm_area_struct *vma,
				  struct vm_fault *vmf)
{
	struct address_space *mapping = vma->vm_file->f_mapping;
	struct inode *inode = mapping->host;
	pgoff_t size;
	void *xip_mem;
	unsigned long xip_pfn;
	int err;

	size = (i_size_read(inode) + PAGE_SIZE - 1) >> PAGE_SHIFT;
	if (vmf->pgoff >= size) {
		pmfs_dbg("[%s:%d] pgoff >= size(SIGBUS). vm_start(0x%lx),"
			" vm_end(0x%lx), pgoff(0x%lx), VA(%lx), size 0x%lx\n",
			__func__, __LINE__, vma->vm_start, vma->vm_end,
			vmf->pgoff, (unsigned long)vmf->address, size);
		return VM_FAULT_SIGBUS;
	}

	err = pmfs_get_xip_mem(mapping, vmf->pgoff, 1, &xip_mem, &xip_pfn);
	if (unlikely(err)) {
		pmfs_dbg("[%s:%d] get_xip_mem failed(OOM). vm_start(0x%lx),"
			" vm_end(0x%lx), pgoff(0x%lx), VA(%lx)\n",
			__func__, __LINE__, vma->vm_start, vma->vm_end,
			vmf->pgoff, (unsigned long)vmf->address);
		return VM_FAULT_SIGBUS;
	}

	pmfs_dbg_mmapv("[%s:%d] vm_start(0x%lx), vm_end(0x%lx), pgoff(0x%lx), "
			"BlockSz(0x%lx), VA(0x%lx)->PA(0x%lx)\n", __func__,
			__LINE__, vma->vm_start, vma->vm_end, vmf->pgoff,
			PAGE_SIZE, (unsigned long)vmf->address,
			(unsigned long)xip_pfn << PAGE_SHIFT);

	err = vm_insert_mixed(vma, (unsigned long)vmf->address,
			pfn_to_pfn_t(xip_pfn));

	if (err == -ENOMEM)
		return VM_FAULT_SIGBUS;
	/*
	 * err == -EBUSY is fine, we've raced against another thread
	 * that faulted-in the same page
	 */
	if (err != -EBUSY)
		BUG_ON(err);
	return VM_FAULT_NOPAGE;
}

static int pmfs_xip_file_fault(struct vm_fault *vmf)
{
	int ret = 0;
	timing_t fault_time;

	PMFS_START_TIMING(mmap_fault_t, fault_time);
	rcu_read_lock();
	ret = __pmfs_xip_file_fault(vmf->vma, vmf);
	rcu_read_unlock();
	PMFS_END_TIMING(mmap_fault_t, fault_time);
	return ret;
}

static int pmfs_find_and_alloc_blocks(struct inode *inode, sector_t iblock,
				       sector_t *data_block, int create)
{
	int err = -EIO;
	u64 block;
	pmfs_transaction_t *trans;
	struct pmfs_inode *pi;

	block = pmfs_find_data_block(inode, iblock);

	if (!block) {
		struct super_block *sb = inode->i_sb;
		if (!create) {
			err = -ENODATA;
			goto err;
		}

		pi = pmfs_get_inode(sb, inode->i_ino);
		trans = pmfs_current_transaction();
		if (trans) {
			err = pmfs_alloc_blocks(trans, inode, iblock, 1, true);
			if (err) {
				pmfs_dbg_verbose("[%s:%d] Alloc failed!\n",
					__func__, __LINE__);
				goto err;
			}
		} else {
			/* 1 lentry for inode, 1 lentry for inode's b-tree */
			trans = pmfs_new_transaction(sb, MAX_INODE_LENTRIES);
			if (IS_ERR(trans)) {
				err = PTR_ERR(trans);
				goto err;
			}

			rcu_read_unlock();
			inode_lock(inode);

			pmfs_add_logentry(sb, trans, pi, MAX_DATA_PER_LENTRY,
				LE_DATA);
			err = pmfs_alloc_blocks(trans, inode, iblock, 1, true);

			pmfs_commit_transaction(sb, trans);

			inode_unlock(inode);
			rcu_read_lock();
			if (err) {
				pmfs_dbg_verbose("[%s:%d] Alloc failed!\n",
					__func__, __LINE__);
				goto err;
			}
		}
		block = pmfs_find_data_block(inode, iblock);
		if (!block) {
			pmfs_dbg("[%s:%d] But alloc didn't fail!\n",
				  __func__, __LINE__);
			err = -ENODATA;
			goto err;
		}
	}
	pmfs_dbg_mmapvv("iblock 0x%lx allocated_block 0x%llx\n", iblock,
			 block);

	*data_block = block;
	err = 0;

err:
	return err;
}

static inline int __pmfs_get_block(struct inode *inode, pgoff_t pgoff,
				    int create, sector_t *result)
{
	int rc = 0;

	rc = pmfs_find_and_alloc_blocks(inode, (sector_t)pgoff, result,
					 create);
	return rc;
}

int pmfs_get_xip_mem(struct address_space *mapping, pgoff_t pgoff, int create,
		      void **kmem, unsigned long *pfn)
{
	int rc;
	sector_t block = 0;
	struct inode *inode = mapping->host;

	rc = __pmfs_get_block(inode, pgoff, create, &block);
	if (rc) {
		pmfs_dbg1("[%s:%d] rc(%d), sb->physaddr(0x%llx), block(0x%llx),"
			" pgoff(0x%lx), flag(0x%x), PFN(0x%lx)\n", __func__,
			__LINE__, rc, PMFS_SB(inode->i_sb)->phys_addr,
			block, pgoff, create, *pfn);
		return rc;
	}

	*kmem = pmfs_get_block(inode->i_sb, block);
	*pfn = pmfs_get_pfn(inode->i_sb, block);

	pmfs_dbg_mmapvv("[%s:%d] sb->physaddr(0x%llx), block(0x%lx),"
		" pgoff(0x%lx), flag(0x%x), PFN(0x%lx)\n", __func__, __LINE__,
		PMFS_SB(inode->i_sb)->phys_addr, block, pgoff, create, *pfn);

	/* dedup start */
	// printk("PMFS_SB(inode->i_sb)->phys_addr:%llu\n", PMFS_SB(inode->i_sb)->phys_addr);
	// printk("block:%lu\n",block);
	// printk("block value:%lu\n",block>>12);
	// printk("pfn:%lu\n",*pfn);
	// printk("kmem:%lu\n",(unsigned long)*kmem);
	/* end */

	return 0;
}

static const struct vm_operations_struct pmfs_xip_vm_ops = {
	.fault	= pmfs_xip_file_fault,
};

int pmfs_xip_file_mmap(struct file *file, struct vm_area_struct *vma)
{
//	BUG_ON(!file->f_mapping->a_ops->get_xip_mem);

	file_accessed(file);

	vma->vm_flags |= VM_MIXEDMAP;

	vma->vm_ops = &pmfs_xip_vm_ops;
	pmfs_dbg_mmap4k("[%s:%d] MMAP 4KPAGE vm_start(0x%lx),"
			" vm_end(0x%lx), vm_flags(0x%lx), "
			"vm_page_prot(0x%lx)\n", __func__,
			__LINE__, vma->vm_start, vma->vm_end,
			vma->vm_flags, pgprot_val(vma->vm_page_prot));

	return 0;
}