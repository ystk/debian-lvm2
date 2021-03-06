/*
 * Copyright (C) 2008,2010 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#ifndef _LVM2APP_MISC_H
#define _LVM2APP_MISC_H

#include "lib.h"
#include "lvm2app.h"
#include "toolcontext.h"
#include "metadata-exported.h"
#include "archiver.h"
#include "locking.h"
#include "lvm-string.h"
#include "lvmcache.h"
#include "metadata.h"

struct dm_list *tag_list_copy(struct dm_pool *p, struct dm_list *tag_list);

#endif
