/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Lustre Lite I/O Page Cache
 *
 *  Copyright (c) 2001, 2002 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/iobuf.h>
#include <linux/errno.h>
#include <linux/smp_lock.h>
#include <linux/unistd.h>
#include <linux/version.h>
#include <asm/system.h>
#include <asm/uaccess.h>

#include <linux/fs.h>
#include <linux/stat.h>
#include <asm/uaccess.h>
#include <asm/segment.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/smp_lock.h>

#define DEBUG_SUBSYSTEM S_LLITE

#include <linux/lustre_mds.h>
#include <linux/lustre_lite.h>
#include <linux/lustre_lib.h>

/*
 * Remove page from dirty list
 */
static void __set_page_clean(struct page *page)
{
        struct address_space *mapping = page->mapping;
        struct inode *inode;

        if (!mapping)
                return;

#if (LINUX_VERSION_CODE <= KERNEL_VERSION(2,5,0))
        spin_lock(&pagecache_lock);
#endif

        list_del(&page->list);
        list_add(&page->list, &mapping->clean_pages);

        inode = mapping->host;
        if (list_empty(&mapping->dirty_pages)) {
                CDEBUG(D_INODE, "inode clean\n");
                inode->i_state &= ~I_DIRTY_PAGES;
        }
#if (LINUX_VERSION_CODE <= KERNEL_VERSION(2,5,0))
        spin_unlock(&pagecache_lock);
#endif
        EXIT;
}

inline void set_page_clean(struct page *page)
{
        if (PageDirty(page)) {
                ClearPageDirty(page);
                __set_page_clean(page);
        }
}

/* SYNCHRONOUS I/O to object storage for an inode */
static int ll_brw(int cmd, struct inode *inode, struct page *page, int create)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        struct lov_stripe_md *lsm = lli->lli_smd;
        struct obd_brw_set *set;
        struct brw_page pg;
        int rc;
        ENTRY;

        set = obd_brw_set_new();
        if (set == NULL)
                RETURN(-ENOMEM);

        pg.pg = page;
        pg.off = ((obd_off)page->index) << PAGE_SHIFT;

        if (cmd == OBD_BRW_WRITE && (pg.off + PAGE_SIZE > inode->i_size))
                pg.count = inode->i_size % PAGE_SIZE;
        else
                pg.count = PAGE_SIZE;

        pg.flag = create ? OBD_BRW_CREATE : 0;

        set->brw_callback = ll_brw_sync_wait;
        rc = obd_brw(cmd, ll_i2obdconn(inode), lsm, 1, &pg, set);
        if (rc) {
                if (rc != -EIO)
                        CERROR("error from obd_brw: rc = %d\n", rc);
        } else {
                rc = ll_brw_sync_wait(set, CB_PHASE_START);
                if (rc)
                        CERROR("error from callback: rc = %d\n", rc);
        }
        obd_brw_set_free(set);

        RETURN(rc);
}

/* returns the page unlocked, but with a reference */
static int ll_readpage(struct file *file, struct page *page)
{
        struct inode *inode = page->mapping->host;
        obd_off offset = ((obd_off)page->index) << PAGE_SHIFT;
        int rc = 0;
        ENTRY;

        if (!PageLocked(page))
                LBUG();

        if (inode->i_size <= offset) {
                memset(kmap(page), 0, PAGE_SIZE);
                kunmap(page);
                GOTO(readpage_out, rc);
        }

        if (PageUptodate(page)) {
                CERROR("Explain this please?\n");
                GOTO(readpage_out, rc);
        }

        rc = ll_brw(OBD_BRW_READ, inode, page, 0);
        EXIT;

 readpage_out:
        if (!rc)
                SetPageUptodate(page);
        unlock_page(page);
        return 0;
} /* ll_readpage */

void ll_truncate(struct inode *inode)
{
        struct obdo oa = {0};
        struct lov_stripe_md *lsm = ll_i2info(inode)->lli_smd;
        struct lustre_handle *lockhs = NULL;
        int err;
        ENTRY;

        if (!lsm) {
                /* object not yet allocated */
                inode->i_mtime = inode->i_ctime = CURRENT_TIME;
                return;
        }

        oa.o_id = lsm->lsm_object_id;
        oa.o_mode = inode->i_mode;
        oa.o_valid = OBD_MD_FLID | OBD_MD_FLMODE | OBD_MD_FLTYPE;

        CDEBUG(D_INFO, "calling punch for "LPX64" (all bytes after "LPD64")\n",
               oa.o_id, inode->i_size);

        err = ll_size_lock(inode, lsm, inode->i_size, LCK_PW, &lockhs);
        if (err) {
                CERROR("ll_size_lock failed: %d\n", err);
                /* FIXME: What to do here?  It's too late to back out... */
                LBUG();
        }

        /* truncate == punch from new size to absolute end of file */
        err = obd_punch(ll_i2obdconn(inode), &oa, lsm, inode->i_size,
                        OBD_OBJECT_EOF);
        if (err) { 
                LBUG();
                CERROR("obd_truncate fails (%d) ino %ld\n", err,
                       inode->i_ino);
        } else
                obdo_to_inode(inode, &oa, oa.o_valid);

        err = ll_size_unlock(inode, lsm, LCK_PW, lockhs);
        if (err)
                CERROR("ll_size_unlock failed: %d\n", err);

        EXIT;
        return;
} /* ll_truncate */

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))

static int ll_prepare_write(struct file *file, struct page *page, unsigned from,
                            unsigned to)
{
        struct inode *inode = page->mapping->host;
        obd_off offset = ((obd_off)page->index) << PAGE_SHIFT;
        int rc = 0;
        char *addr;
        ENTRY;

        addr = kmap(page);
        if (!PageLocked(page))
                LBUG();

        if (Page_Uptodate(page))
                GOTO(prepare_done, rc);

        /* We're completely overwriting an existing page, so _don't_ set it up
         * to date until commit_write */
        if (from == 0 && to == PAGE_SIZE)
                RETURN(0);

        /* We are writing to a new page, no need to read old data */
        if (inode->i_size <= offset) {
                memset(addr, 0, PAGE_SIZE);
                GOTO(prepare_done, rc=0);
        }

        rc = ll_brw(OBD_BRW_READ, inode, page, 0);

        EXIT;
 prepare_done:
        if (!rc)
                SetPageUptodate(page);

        return rc;
}

/* returns the page unlocked, but with a reference */
static int ll_writepage(struct page *page)
{
        struct inode *inode = page->mapping->host;
        int err;
        ENTRY;

        if (!PageLocked(page))
                LBUG();

        err = ll_brw(OBD_BRW_WRITE, inode, page, 1);
        if ( !err ) {
                //SetPageUptodate(page);
                set_page_clean(page);
        } else {
                CERROR("ll_brw failure %d\n", err);
        }
        unlock_page(page);
        RETURN(err);
}


/* SYNCHRONOUS I/O to object storage for an inode -- object attr will be updated
 * too */
static int ll_commit_write(struct file *file, struct page *page,
                           unsigned from, unsigned to)
{
        struct inode *inode = page->mapping->host;
        struct ll_inode_info *lli = ll_i2info(inode);
        struct lov_stripe_md *md = lli->lli_smd;
        struct brw_page pg;
        struct obd_brw_set *set;
        int rc, create = 1;
        loff_t size;
        ENTRY;

        pg.pg = page;
        pg.count = to;
        pg.off = (((obd_off)page->index) << PAGE_SHIFT);
        pg.flag = create ? OBD_BRW_CREATE : 0;

        set = obd_brw_set_new();
        if (set == NULL)
                RETURN(-ENOMEM);

        SetPageUptodate(page);

        if (!PageLocked(page))
                LBUG();

        CDEBUG(D_INODE, "commit_page writing (off "LPD64"), count "LPD64"\n",
               pg.off, pg.count);

        set->brw_callback = ll_brw_sync_wait;
        rc = obd_brw(OBD_BRW_WRITE, ll_i2obdconn(inode), md, 1, &pg, set);
        if (rc)
                CERROR("error from obd_brw: rc = %d\n", rc);
        else {
                rc = ll_brw_sync_wait(set, CB_PHASE_START);
                if (rc)
                        CERROR("error from callback: rc = %d\n", rc);
        }
        obd_brw_set_free(set);
        kunmap(page);

        size = pg.off + pg.count;
        /* do NOT truncate when writing in the middle of a file */
        if (size > inode->i_size)
                inode->i_size = size;

        RETURN(rc);
} /* ll_commit_write */


static int ll_direct_IO(int rw, struct inode *inode, struct kiobuf *iobuf,
                        unsigned long blocknr, int blocksize)
{
        obd_count bufs_per_obdo = iobuf->nr_pages;
        struct ll_inode_info *lli = ll_i2info(inode);
        struct lov_stripe_md *lsm = lli->lli_smd;
        struct brw_page *pga;
        struct obd_brw_set *set;
        int i, rc = 0;
        ENTRY;

        if (!lsm || !lsm->lsm_object_id)
                RETURN(-ENOMEM);

        if (blocksize != PAGE_SIZE) {
                CERROR("direct_IO blocksize != PAGE_SIZE\n");
                RETURN(-EINVAL);
        }

        set = obd_brw_set_new();
        if (set == NULL)
                RETURN(-ENOMEM);

        OBD_ALLOC(pga, sizeof(*pga) * bufs_per_obdo);
        if (!pga) {
                obd_brw_set_free(set);
                RETURN(-ENOMEM);
        }

        /* NB: we can't use iobuf->maplist[i]->index for the offset
         * instead of "blocknr" because ->index contains garbage.
         */
        for (i = 0; i < bufs_per_obdo; i++, blocknr++) {
                pga[i].pg = iobuf->maplist[i];
                pga[i].count = PAGE_SIZE;
                pga[i].off = (obd_off)blocknr << PAGE_SHIFT;
                pga[i].flag = OBD_BRW_CREATE;
        }

        set->brw_callback = ll_brw_sync_wait;
        rc = obd_brw(rw == WRITE ? OBD_BRW_WRITE : OBD_BRW_READ,
                     ll_i2obdconn(inode), lsm, bufs_per_obdo, pga, set);
        if (rc)
                CERROR("error from obd_brw: rc = %d\n", rc);
        else {
                rc = ll_brw_sync_wait(set, CB_PHASE_START);
                if (rc)
                        CERROR("error from callback: rc = %d\n", rc);
        }
        obd_brw_set_free(set);
        if (rc == 0)
                rc = bufs_per_obdo * PAGE_SIZE;

        OBD_FREE(pga, sizeof(*pga) * bufs_per_obdo);
        RETURN(rc);
}

int ll_flush_inode_pages(struct inode * inode)
{
        obd_count        bufs_per_obdo = 0;
        obd_size         *count = NULL;
        obd_off          *offset = NULL;
        obd_flag         *flags = NULL;
        int              err = 0;

        ENTRY;

        spin_lock(&pagecache_lock);

        spin_unlock(&pagecache_lock);


        OBD_ALLOC(count, sizeof(*count) * bufs_per_obdo);
        OBD_ALLOC(offset, sizeof(*offset) * bufs_per_obdo);
        OBD_ALLOC(flags, sizeof(*flags) * bufs_per_obdo);
        if (!count || !offset || !flags)
                GOTO(out, err=-ENOMEM);

#if 0
        for (i = 0 ; i < bufs_per_obdo ; i++) {
                count[i] = PAGE_SIZE;
                offset[i] = ((obd_off)(iobuf->maplist[i])->index) << PAGE_SHIFT;
                flags[i] = OBD_BRW_CREATE;
        }

        err = obd_brw(OBD_BRW_WRITE, ll_i2obdconn(inode),
                      ll_i2info(inode)->lli_smd, bufs_per_obdo,
                      iobuf->maplist, count, offset, flags, NULL, NULL);
        if (err == 0)
                err = bufs_per_obdo * 4096;
#endif
 out:
        OBD_FREE(flags, sizeof(*flags) * bufs_per_obdo);
        OBD_FREE(count, sizeof(*count) * bufs_per_obdo);
        OBD_FREE(offset, sizeof(*offset) * bufs_per_obdo);
        RETURN(err);
}

#endif


struct address_space_operations ll_aops = {
        readpage: ll_readpage,
#if (LINUX_VERSION_CODE <= KERNEL_VERSION(2,5,0))
        direct_IO: ll_direct_IO,
        writepage: ll_writepage,
        sync_page: block_sync_page,
        prepare_write: ll_prepare_write,
        commit_write: ll_commit_write,
        bmap: NULL
#endif
};
