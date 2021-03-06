/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
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

#include "tools.h"

static int _monitor_lvs_in_vg(struct cmd_context *cmd,
			       struct volume_group *vg, int reg)
{
	struct lv_list *lvl;
	struct logical_volume *lv;
	struct lvinfo info;
	int lv_active;
	int count = 0;

	dm_list_iterate_items(lvl, &vg->lvs) {
		lv = lvl->lv;

		if (!lv_info(cmd, lv, &info, 0, 0))
			lv_active = 0;
		else
			lv_active = info.exists;

		/*
		 * FIXME: Need to consider all cases... PVMOVE, etc
		 */
		if ((lv->status & PVMOVE) || !lv_active)
			continue;

		if (!monitor_dev_for_events(cmd, lv, reg)) {
			continue;
		} else
			count++;
	}

	/*
	 * returns the number of _new_ monitored devices
	 */

	return count;
}

static int _poll_lvs_in_vg(struct cmd_context *cmd,
			   struct volume_group *vg)
{
	struct lv_list *lvl;
	struct logical_volume *lv;
	struct lvinfo info;
	int lv_active;
	int count = 0;

	dm_list_iterate_items(lvl, &vg->lvs) {
		lv = lvl->lv;

		if (!lv_info(cmd, lv, &info, 0, 0))
			lv_active = 0;
		else
			lv_active = info.exists;

		if (lv_active &&
		    (lv->status & (PVMOVE|CONVERTING|MERGING))) {
			lv_spawn_background_polling(cmd, lv);
			count++;
		}
	}

	/*
	 * returns the number of polled devices
	 * - there is no way to know if lv is already being polled
	 */

	return count;
}

static int _activate_lvs_in_vg(struct cmd_context *cmd,
			       struct volume_group *vg, int activate)
{
	struct lv_list *lvl;
	struct logical_volume *lv;
	int count = 0, expected_count = 0;

	dm_list_iterate_items(lvl, &vg->lvs) {
		lv = lvl->lv;

		if (!lv_is_visible(lv))
			continue;

		/* Only request activation of snapshot origin devices */
		if ((lv->status & SNAPSHOT) || lv_is_cow(lv))
			continue;

		/* Only request activation of mirror LV */
		if ((lv->status & MIRROR_IMAGE) || (lv->status & MIRROR_LOG))
			continue;

		/* Can't deactivate a pvmove LV */
		/* FIXME There needs to be a controlled way of doing this */
		if (((activate == CHANGE_AN) || (activate == CHANGE_ALN)) &&
		    ((lv->status & PVMOVE) ))
			continue;

		expected_count++;

		if (activate == CHANGE_AN) {
			if (!deactivate_lv(cmd, lv)) {
				stack;
				continue;
			}
		} else if (activate == CHANGE_ALN) {
			if (!deactivate_lv_local(cmd, lv)) {
				stack;
				continue;
			}
		} else if (lv_is_origin(lv) || (activate == CHANGE_AE)) {
			if (!activate_lv_excl(cmd, lv)) {
				stack;
				continue;
			}
		} else if (activate == CHANGE_ALY) {
			if (!activate_lv_local(cmd, lv)) {
				stack;
				continue;
			}
		} else if (!activate_lv(cmd, lv)) {
			stack;
			continue;
		}

		if (background_polling() &&
		    activate != CHANGE_AN && activate != CHANGE_ALN &&
		    (lv->status & (PVMOVE|CONVERTING|MERGING)))
			lv_spawn_background_polling(cmd, lv);

		count++;
	}

	if (expected_count)
		log_verbose("%s %d logical volumes in volume group %s",
			    (activate == CHANGE_AN || activate == CHANGE_ALN)?
			    "Deactivated" : "Activated", count, vg->name);

	return (expected_count != count) ? ECMD_FAILED : ECMD_PROCESSED;
}

static int _vgchange_monitoring(struct cmd_context *cmd, struct volume_group *vg)
{
	int active, monitored;

	if ((active = lvs_in_vg_activated(vg)) &&
	    dmeventd_monitor_mode() != DMEVENTD_MONITOR_IGNORE) {
		monitored = _monitor_lvs_in_vg(cmd, vg, dmeventd_monitor_mode());
		log_print("%d logical volume(s) in volume group "
			    "\"%s\" %smonitored",
			    monitored, vg->name, (dmeventd_monitor_mode()) ? "" : "un");
	}

	return ECMD_PROCESSED;
}

static int _vgchange_background_polling(struct cmd_context *cmd, struct volume_group *vg)
{
	int polled;

	if (lvs_in_vg_activated(vg) && background_polling()) {
	        polled = _poll_lvs_in_vg(cmd, vg);
		log_print("Background polling started for %d logical volume(s) "
			  "in volume group \"%s\"",
			  polled, vg->name);
	}

	return ECMD_PROCESSED;
}

static int _vgchange_available(struct cmd_context *cmd, struct volume_group *vg)
{
	int lv_open, active, monitored;
	int available, ret;
	int activate = 1;

	/*
	 * Safe, since we never write out new metadata here. Required for
	 * partial activation to work.
	 */
	cmd->handles_missing_pvs = 1;

	available = arg_uint_value(cmd, available_ARG, 0);

	if ((available == CHANGE_AN) || (available == CHANGE_ALN))
		activate = 0;

	/* FIXME: Force argument to deactivate them? */
	if (!activate && (lv_open = lvs_in_vg_opened(vg))) {
		log_error("Can't deactivate volume group \"%s\" with %d open "
			  "logical volume(s)", vg->name, lv_open);
		return ECMD_FAILED;
	}

	/* FIXME Move into library where clvmd can use it */
	if (activate)
		check_current_backup(vg);

	if (activate && (active = lvs_in_vg_activated(vg))) {
		log_verbose("%d logical volume(s) in volume group \"%s\" "
			    "already active", active, vg->name);
		if (dmeventd_monitor_mode() != DMEVENTD_MONITOR_IGNORE) {
			monitored = _monitor_lvs_in_vg(cmd, vg, dmeventd_monitor_mode());
			log_verbose("%d existing logical volume(s) in volume "
				    "group \"%s\" %smonitored",
				    monitored, vg->name,
				    dmeventd_monitor_mode() ? "" : "un");
		}
	}

	ret = _activate_lvs_in_vg(cmd, vg, available);

	log_print("%d logical volume(s) in volume group \"%s\" now active",
		  lvs_in_vg_activated(vg), vg->name);
	return ret;
}

static int _vgchange_alloc(struct cmd_context *cmd, struct volume_group *vg)
{
	alloc_policy_t alloc;

	alloc = arg_uint_value(cmd, alloc_ARG, ALLOC_NORMAL);

	if (!archive(vg)) {
		stack;
		return ECMD_FAILED;
	}

	/* FIXME: make consistent with vg_set_alloc_policy() */
	if (alloc == vg->alloc) {
		log_error("Volume group allocation policy is already %s",
			  get_alloc_string(vg->alloc));
		return ECMD_FAILED;
	}
	if (!vg_set_alloc_policy(vg, alloc)) {
		stack;
		return ECMD_FAILED;
	}

	if (!vg_write(vg) || !vg_commit(vg)) {
		stack;
		return ECMD_FAILED;
	}

	backup(vg);

	log_print("Volume group \"%s\" successfully changed", vg->name);

	return ECMD_PROCESSED;
}

static int _vgchange_resizeable(struct cmd_context *cmd,
				struct volume_group *vg)
{
	int resizeable = !strcmp(arg_str_value(cmd, resizeable_ARG, "n"), "y");

	if (resizeable && vg_is_resizeable(vg)) {
		log_error("Volume group \"%s\" is already resizeable",
			  vg->name);
		return ECMD_FAILED;
	}

	if (!resizeable && !vg_is_resizeable(vg)) {
		log_error("Volume group \"%s\" is already not resizeable",
			  vg->name);
		return ECMD_FAILED;
	}

	if (!archive(vg)) {
		stack;
		return ECMD_FAILED;
	}

	if (resizeable)
		vg->status |= RESIZEABLE_VG;
	else
		vg->status &= ~RESIZEABLE_VG;

	if (!vg_write(vg) || !vg_commit(vg)) {
		stack;
		return ECMD_FAILED;
	}

	backup(vg);

	log_print("Volume group \"%s\" successfully changed", vg->name);

	return ECMD_PROCESSED;
}

static int _vgchange_clustered(struct cmd_context *cmd,
			       struct volume_group *vg)
{
	int clustered = !strcmp(arg_str_value(cmd, clustered_ARG, "n"), "y");

	if (clustered && (vg_is_clustered(vg))) {
		log_error("Volume group \"%s\" is already clustered",
			  vg->name);
		return ECMD_FAILED;
	}

	if (!clustered && !(vg_is_clustered(vg))) {
		log_error("Volume group \"%s\" is already not clustered",
			  vg->name);
		return ECMD_FAILED;
	}

	if (!archive(vg)) {
		stack;
		return ECMD_FAILED;
	}

	if (!vg_set_clustered(vg, clustered))
		return ECMD_FAILED;

	if (!vg_write(vg) || !vg_commit(vg)) {
		stack;
		return ECMD_FAILED;
	}

	backup(vg);

	log_print("Volume group \"%s\" successfully changed", vg->name);

	return ECMD_PROCESSED;
}

static int _vgchange_logicalvolume(struct cmd_context *cmd,
				   struct volume_group *vg)
{
	uint32_t max_lv = arg_uint_value(cmd, logicalvolume_ARG, 0);

	if (!archive(vg)) {
		stack;
		return ECMD_FAILED;
	}

	if (!vg_set_max_lv(vg, max_lv)) {
		stack;
		return ECMD_FAILED;
	}

	if (!vg_write(vg) || !vg_commit(vg)) {
		stack;
		return ECMD_FAILED;
	}

	backup(vg);

	log_print("Volume group \"%s\" successfully changed", vg->name);

	return ECMD_PROCESSED;
}

static int _vgchange_physicalvolumes(struct cmd_context *cmd,
				     struct volume_group *vg)
{
	uint32_t max_pv = arg_uint_value(cmd, maxphysicalvolumes_ARG, 0);

	if (arg_sign_value(cmd, maxphysicalvolumes_ARG, 0) == SIGN_MINUS) {
		log_error("MaxPhysicalVolumes may not be negative");
		return EINVALID_CMD_LINE;
	}

	if (!archive(vg)) {
		stack;
		return ECMD_FAILED;
	}

	if (!vg_set_max_pv(vg, max_pv)) {
		stack;
		return ECMD_FAILED;
	}

	if (!vg_write(vg) || !vg_commit(vg)) {
		stack;
		return ECMD_FAILED;
	}

	backup(vg);

	log_print("Volume group \"%s\" successfully changed", vg->name);

	return ECMD_PROCESSED;
}

static int _vgchange_pesize(struct cmd_context *cmd, struct volume_group *vg)
{
	uint32_t extent_size;

	if (arg_sign_value(cmd, physicalextentsize_ARG, 0) == SIGN_MINUS) {
		log_error("Physical extent size may not be negative");
		return EINVALID_CMD_LINE;
	}

	extent_size = arg_uint_value(cmd, physicalextentsize_ARG, 0);
	/* FIXME: remove check - redundant with vg_change_pesize */
	if (extent_size == vg->extent_size) {
		log_error("Physical extent size of VG %s is already %s",
			  vg->name, display_size(cmd, (uint64_t) extent_size));
		return ECMD_PROCESSED;
	}

	if (!archive(vg)) {
		stack;
		return ECMD_FAILED;
	}

	if (!vg_set_extent_size(vg, extent_size)) {
		stack;
		return EINVALID_CMD_LINE;
	}

	if (!vg_write(vg) || !vg_commit(vg)) {
		stack;
		return ECMD_FAILED;
	}

	backup(vg);

	log_print("Volume group \"%s\" successfully changed", vg->name);

	return ECMD_PROCESSED;
}

static int _vgchange_tag(struct cmd_context *cmd, struct volume_group *vg,
			 int arg)
{
	const char *tag;

	if (!(tag = arg_str_value(cmd, arg, NULL))) {
		log_error("Failed to get tag");
		return ECMD_FAILED;
	}

	if (!archive(vg)) {
		stack;
		return ECMD_FAILED;
	}

	if (!vg_change_tag(vg, tag, arg == addtag_ARG)) {
		stack;
		return ECMD_FAILED;
	}

	if (!vg_write(vg) || !vg_commit(vg)) {
		stack;
		return ECMD_FAILED;
	}

	backup(vg);

	log_print("Volume group \"%s\" successfully changed", vg->name);

	return ECMD_PROCESSED;
}

static int _vgchange_uuid(struct cmd_context *cmd __attribute((unused)),
			  struct volume_group *vg)
{
	struct lv_list *lvl;

	if (lvs_in_vg_activated(vg)) {
		log_error("Volume group has active logical volumes");
		return ECMD_FAILED;
	}

	if (!archive(vg)) {
		stack;
		return ECMD_FAILED;
	}

	if (!id_create(&vg->id)) {
		log_error("Failed to generate new random UUID for VG %s.",
			  vg->name);
		return ECMD_FAILED;
	}

	dm_list_iterate_items(lvl, &vg->lvs) {
		memcpy(&lvl->lv->lvid, &vg->id, sizeof(vg->id));
	}

	if (!vg_write(vg) || !vg_commit(vg)) {
		stack;
		return ECMD_FAILED;
	}

	backup(vg);

	log_print("Volume group \"%s\" successfully changed", vg->name);

	return ECMD_PROCESSED;
}

static int _vgchange_refresh(struct cmd_context *cmd, struct volume_group *vg)
{
	log_verbose("Refreshing volume group \"%s\"", vg->name);

	if (!vg_refresh_visible(cmd, vg)) {
		stack;
		return ECMD_FAILED;
	}

	return ECMD_PROCESSED;
}

static int vgchange_single(struct cmd_context *cmd, const char *vg_name,
			   struct volume_group *vg,
			   void *handle __attribute((unused)))
{
	int dmeventd_mode, r = ECMD_FAILED;

	if (vg_is_exported(vg)) {
		log_error("Volume group \"%s\" is exported", vg_name);
		return ECMD_FAILED;
	}

	if (!get_activation_monitoring_mode(cmd, vg, &dmeventd_mode))
		return ECMD_FAILED;

	init_dmeventd_monitor(dmeventd_mode);

	/*
	 * FIXME: DEFAULT_BACKGROUND_POLLING should be "unspecified".
	 * If --poll is explicitly provided use it; otherwise polling
	 * should only be started if the LV is not already active. So:
	 * 1) change the activation code to say if the LV was actually activated
	 * 2) make polling of an LV tightly coupled with LV activation
	 *
	 * Do not initiate any polling if --sysinit option is used.
	 */
	init_background_polling(arg_count(cmd, sysinit_ARG) ? 0 :
						arg_int_value(cmd, poll_ARG,
						DEFAULT_BACKGROUND_POLLING));

	if (arg_count(cmd, available_ARG))
		r = _vgchange_available(cmd, vg);

	else if (arg_count(cmd, monitor_ARG))
		r = _vgchange_monitoring(cmd, vg);

	else if (arg_count(cmd, poll_ARG))
		r = _vgchange_background_polling(cmd, vg);

	else if (arg_count(cmd, resizeable_ARG))
		r = _vgchange_resizeable(cmd, vg);

	else if (arg_count(cmd, logicalvolume_ARG))
		r = _vgchange_logicalvolume(cmd, vg);

	else if (arg_count(cmd, maxphysicalvolumes_ARG))
		r = _vgchange_physicalvolumes(cmd, vg);

	else if (arg_count(cmd, addtag_ARG))
		r = _vgchange_tag(cmd, vg, addtag_ARG);

	else if (arg_count(cmd, deltag_ARG))
		r = _vgchange_tag(cmd, vg, deltag_ARG);

	else if (arg_count(cmd, physicalextentsize_ARG))
		r = _vgchange_pesize(cmd, vg);

	else if (arg_count(cmd, uuid_ARG))
		r = _vgchange_uuid(cmd, vg);

	else if (arg_count(cmd, alloc_ARG))
		r = _vgchange_alloc(cmd, vg);

	else if (arg_count(cmd, clustered_ARG))
		r = _vgchange_clustered(cmd, vg);

	else if (arg_count(cmd, refresh_ARG))
		r = _vgchange_refresh(cmd, vg);

	return r;
}

int vgchange(struct cmd_context *cmd, int argc, char **argv)
{
	if (!
	    (arg_count(cmd, available_ARG) + arg_count(cmd, logicalvolume_ARG) +
	     arg_count(cmd, maxphysicalvolumes_ARG) +
	     arg_count(cmd, resizeable_ARG) + arg_count(cmd, deltag_ARG) +
	     arg_count(cmd, addtag_ARG) + arg_count(cmd, uuid_ARG) +
	     arg_count(cmd, physicalextentsize_ARG) +
	     arg_count(cmd, clustered_ARG) + arg_count(cmd, alloc_ARG) +
	     arg_count(cmd, monitor_ARG) + arg_count(cmd, poll_ARG) +
	     arg_count(cmd, refresh_ARG))) {
		log_error("Need 1 or more of -a, -c, -l, -p, -s, -x, "
			  "--refresh, --uuid, --alloc, --addtag, --deltag, "
			  "--monitor or --poll");
		return EINVALID_CMD_LINE;
	}

	/* FIXME Cope with several changes at once! */
	if (arg_count(cmd, available_ARG) + arg_count(cmd, logicalvolume_ARG) +
	    arg_count(cmd, maxphysicalvolumes_ARG) +
	    arg_count(cmd, resizeable_ARG) + arg_count(cmd, deltag_ARG) +
	    arg_count(cmd, addtag_ARG) + arg_count(cmd, alloc_ARG) +
	    arg_count(cmd, uuid_ARG) + arg_count(cmd, clustered_ARG) +
	    arg_count(cmd, physicalextentsize_ARG) > 1) {
		log_error("Only one of -a, -c, -l, -p, -s, -x, --uuid, "
			  "--alloc, --addtag or --deltag allowed");
		return EINVALID_CMD_LINE;
	}

	if ((arg_count(cmd, ignorelockingfailure_ARG) ||
	     arg_count(cmd, sysinit_ARG)) && !arg_count(cmd, available_ARG)) {
		log_error("Only -a premitted with --ignorelockingfailure and --sysinit");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, poll_ARG) && arg_count(cmd, sysinit_ARG)) {
		log_error("Only one of --poll and --sysinit permitted.");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, available_ARG) == 1
	    && arg_count(cmd, autobackup_ARG)) {
		log_error("-A option not necessary with -a option");
		return EINVALID_CMD_LINE;
	}

	return process_each_vg(cmd, argc, argv,
			       (arg_count(cmd, available_ARG)) ?
			       0 : READ_FOR_UPDATE,
			       NULL,
			       &vgchange_single);
}
