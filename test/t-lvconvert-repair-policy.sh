#!/bin/bash
# Copyright (C) 2008 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. ./test-utils.sh

prepare_vg 4

# Clean-up and create a 2-way mirror, where the the
# leg devices are always on $dev[12] and the log
# is always on $dev3.  ($dev4 behaves as a spare)
cleanup() {
	vgreduce --removemissing $vg
	for d in "$@"; do enable_dev $d; done
	for d in "$@"; do vgextend $vg $d; done
	lvremove -ff $vg/mirror
	lvcreate -m 1 --ig -l 2 -n mirror $vg $dev1 $dev2 $dev3:0
}

repair() {
	lvconvert --repair --use-policies --config "$1" $vg/mirror
}

lvcreate -m 1 -L 1 -n mirror $vg
lvchange -a n $vg/mirror

# Fail a leg of a mirror.
disable_dev $dev1
lvchange --partial -a y $vg/mirror
repair 'activation { mirror_image_fault_policy = "remove" }'
check linear $vg mirror
cleanup $dev1

# Fail a leg of a mirror.
# Expected result: Mirror (leg replaced)
disable_dev $dev1
repair 'activation { mirror_image_fault_policy = "replace" }'
check mirror $vg mirror
lvs | grep mirror_mlog
cleanup $dev1

# Fail a leg of a mirror (use old name for policy specification)
# Expected result: Mirror (leg replaced)
disable_dev $dev1
repair 'activation { mirror_device_fault_policy = "replace" }'
check mirror $vg mirror
lvs | grep mirror_mlog
cleanup $dev1

# Fail a leg of a mirror w/ no available spare
# Expected result: 2-way with corelog
disable_dev $dev2 $dev4
repair 'activation { mirror_image_fault_policy = "replace" }'
check mirror $vg mirror
lvs | not grep mirror_mlog
cleanup $dev2 $dev4

# Fail the log device of a mirror w/ no available spare
# Expected result: mirror w/ corelog
disable_dev $dev3 $dev4
lvconvert --repair --use-policies --config 'activation { mirror_image_fault_policy = "replace" }' $vg/mirror
check mirror $vg mirror
lvs | not grep mirror_mlog
cleanup $dev3 $dev4
