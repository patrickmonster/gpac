/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2017
 *					All rights reserved
 *
 *  This file is part of GPAC / filters sub-project
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terfsess of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include "filter_session.h"


GF_FilterPacket *gf_filter_pck_new_alloc(GF_FilterPid *pid, u32 data_size, char **data)
{
	GF_FilterPacket *pck=NULL;
	u32 i, count;

	if (PID_IS_INPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to allocate a packet on an input PID in filter %s\n", pid->filter->name));
		return NULL;
	}

	count = gf_fq_count(pid->filter->pcks_alloc_reservoir);
	for (i=0; i<count; i++) {
		GF_FilterPacket *cur = gf_fq_get(pid->filter->pcks_alloc_reservoir, i);
		if (cur->alloc_size >= data_size) {
			if (!pck || (pck->alloc_size>cur->alloc_size)) {
				pck = cur;
			}
		}
	}

	//TODO - stop allocating after a while...
	if (!pck) {
		GF_SAFEALLOC(pck, GF_FilterPacket);
		pck->data = gf_malloc(sizeof(char)*data_size);
		pck->alloc_size = data_size;
	} else {
		//pop first item and swap pointers. We can safely do this since this filter
		//is the only one accessing the queue in pop mode, all others are just pushing to it
		//this may however imply that we don't get the best matching block size if new packets
		//were added to the list

		GF_FilterPacket *head_pck = gf_fq_pop(pid->filter->pcks_alloc_reservoir);
		char *data = pck->data;
		u32 alloc_size = pck->alloc_size;
		pck->data = head_pck->data;
		pck->alloc_size = head_pck->alloc_size;
		head_pck->data = data;
		head_pck->alloc_size = alloc_size;
		pck = head_pck;
	}

	pck->pck = pck;
	pck->data_length = data_size;
	pck->pid = pid;
	if (data) *data = pck->data;
	pck->data_block_start = pck->data_block_end = GF_TRUE;

	assert(pck->pid);
	return pck;
}

GF_FilterPacket *gf_filter_pck_new_shared(GF_FilterPid *pid, const char *data, u32 data_size, packet_destructor destruct)
{
	GF_FilterPacket *pck;

	if (PID_IS_INPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to allocate a packet on an input PID in filter %s\n", pid->filter->name));
		return NULL;
	}

	pck = gf_fq_pop(pid->filter->pcks_shared_reservoir);
	if (!pck) {
		GF_SAFEALLOC(pck, GF_FilterPacket);
		if (!pck) return NULL;
	}
	pck->pck = pck;
	pck->pid = pid;
	pck->data = (char *) data;
	pck->data_length = data_size;
	pck->filter_owns_mem = GF_TRUE;
	pck->destructor = destruct;
	pck->data_block_start = pck->data_block_end = GF_TRUE;

	assert(pck->pid);
	return pck;
}

GF_FilterPacket *gf_filter_pck_new_ref(GF_FilterPid *pid, const char *data, u32 data_size, GF_FilterPacket *reference)
{
	GF_FilterPacket *pck;
	if (!reference) return NULL;
	reference=reference->pck;
	pck = gf_filter_pck_new_shared(pid, data, data_size, NULL);
	pck->reference = reference;
	assert(reference->reference_count);
	ref_count_inc(&reference->reference_count);
	if (!data || !data_size) {
		pck->data = reference->data;
		pck->data_length = reference->data_length;
	}
	return pck;
}

void gf_filter_packet_destroy(GF_FilterPacket *pck)
{
	GF_FilterPid *pid = pck->pid;
	if (!pck->pid) {
		assert(pck->pid);
	}
	if (pck->destructor) pck->destructor(pid->filter, pid, pck);

	if (pck->pid_props) {
		GF_PropertyMap *props = pck->pid_props;
		pck->pid_props = NULL;
		assert(props->reference_count);
		if (ref_count_dec(&props->reference_count) == 0) {
			gf_list_del_item(pck->pid->properties, props);
			gf_props_del(props);
		}
	}

	if (pck->props) {
		GF_PropertyMap *props = pck->props;
		pck->props=NULL;
		gf_props_del(props);
	}
	pck->data_length = 0;
	pck->pid = NULL;

	if (pck->filter_owns_mem) {

		if (pck->reference) {
			assert(pck->reference->reference_count);
			if (ref_count_dec(&pck->reference->reference_count) == 0) {
				gf_filter_packet_destroy(pck->reference);
			}
			pck->reference = NULL;
		}

		gf_fq_add(pid->filter->pcks_shared_reservoir, pck);
	} else {
		gf_fq_add(pid->filter->pcks_alloc_reservoir, pck);
	}
}

static Bool gf_filter_aggregate_packets(GF_FilterPidInst *dst)
{
	u32 size=0, pos=0;
	char *data;
	GF_FilterPacket *final;
	u32 i, count;
	//no need to lock the packet list since only the dispatch thread operates on it

	count=gf_list_count(dst->pck_reassembly);
	//no packet to reaggregate
	if (!count) return GF_FALSE;
	for (i=0; i<count; i++) {
		GF_FilterPacketInstance *pck = gf_list_get(dst->pck_reassembly, i);
		assert(pck);
		assert(!pck->pck->data_block_start || !pck->pck->data_block_end);
		size += pck->pck->data_length;
	}

	final = gf_filter_pck_new_alloc(dst->pid, size, &data);
	pos=0;
	for (i=0; i<count; i++) {
		GF_FilterPacket *pck;
		GF_FilterPacketInstance *pcki = gf_list_get(dst->pck_reassembly, i);
		assert(pcki);
		pck = pcki->pck;
		memcpy(data+pos, pcki->pck->data, pcki->pck->data_length);
		pos += pcki->pck->data_length;
		gf_filter_pck_merge_properties(pcki->pck, final);

		gf_list_rem(dst->pck_reassembly, i);

		//destroy pcki
		if (i+1<count) {
			pcki->pck = NULL;
			pcki->pid = NULL;

			gf_fq_add(pck->pid->filter->pcks_inst_reservoir, pcki);
		} else {
			pcki->pck = final;
			ref_count_inc(&final->reference_count);

			ref_count_inc(&dst->filter->pending_packets);

			gf_fq_add(dst->packets, pcki);

		}
		//unref pck
		if (ref_count_dec(&pck->reference_count)==0) {
			gf_filter_packet_destroy(pck);
		}

		count--;
		i--;
	}
	return GF_TRUE;
}

GF_Err gf_filter_pck_send(GF_FilterPacket *pck)
{
	u32 i, count, nb_dispatch=0;
	GF_FilterPid *pid;
	assert(pck);
	assert(pck->pid);
	pid = pck->pid;

	if (PCK_IS_INPUT(pck)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to dispatch input packet on output PID in filter %s\n", pck->pid->filter->name));
		return GF_BAD_PARAM;
	}
	//we have seen packets and a new property map was created, post a config update
	if (pid->nb_pck_sent && !pid->request_property_map) {
		count = gf_list_count(pck->pid->destinations);
		for (i=0; i<count; i++) {
			GF_FilterPidInst *dst = gf_list_get(pck->pid->destinations, i);

			assert(dst->filter->freg->configure_pid);
			gf_fs_post_task(pid->filter->session, gf_filter_pid_reconfigure_task, dst->filter, pid, "reconfig_pid", NULL);
		}
	}
	
	assert(pck->pid);
	pid->nb_pck_sent++;
	//we have dispatched a packet, any new pid_set_property after this packet will trigger a new property map
	pid->request_property_map = GF_TRUE;

	//pid properties applying to this packet are the last defined ones
	pck->pid_props = gf_list_last(pid->properties);
	if (pck->pid_props) ref_count_inc(&pck->pid_props->reference_count);


	//todo - check amount of packets size/time to return a WOULD_BLOCK


	//protect packet from destruction - this could happen
	//1) during aggregation of packets
	//2) after dispatching to the packet queue of the next filter, that packet may be consumed
	//by its destination before we are done adding to the other destination
	ref_count_inc(&pck->reference_count);

	assert(pck->pid);
	count = gf_list_count(pck->pid->destinations);
	for (i=0; i<count; i++) {
		GF_FilterPidInst *dst = gf_list_get(pck->pid->destinations, i);
		if (dst->filter->freg->process) {
			Bool post_task=GF_FALSE;
			GF_FilterPacketInstance *inst;

			inst = gf_fq_pop(pck->pid->filter->pcks_inst_reservoir);
			if (!inst) {
				GF_SAFEALLOC(inst, GF_FilterPacketInstance);
				if (!inst) return GF_OUT_OF_MEM;
			}
			inst->pck = pck;
			inst->pid = dst;

			ref_count_inc(&pck->reference_count);
			nb_dispatch++;

			GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Dispatching packet from filter %s to filter %s\n", pid->filter->name, dst->filter->name));

			if (dst->requires_full_data_block) {

				//missed end of previous, aggregate all before excluding this packet
				if (pck->data_block_start) {
					if (!dst->last_block_ended) {
						GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("Filter %s: Missed end of block signaling but got start of block - reaggregating packet\n", pid->filter->name));

						//post process task if we have been reaggregating a packet
						post_task = gf_filter_aggregate_packets(dst);
					}
					dst->last_block_ended = GF_TRUE;
				}

				//block end, aggregate all before and including this packet
				if (pck->data_block_end) {
					//not starting at this packet, append and aggregate
					if (!pck->data_block_start) {
						//insert packet into reassembly (no need to lock here)
						gf_list_add(dst->pck_reassembly, inst);

						gf_filter_aggregate_packets(dst);
					}
					//single block packet, direct dispatch in packet queue (aggregation done before)
					else {
						assert(dst->last_block_ended);
						ref_count_inc(&dst->filter->pending_packets);

						gf_fq_add(dst->packets, inst);
					}
					dst->last_block_ended = GF_TRUE;
					post_task = GF_TRUE;
				}
				//new block start or continuation
				else {
					//insert packet into reassembly (no need to lock here)
					gf_list_add(dst->pck_reassembly, inst);

					dst->last_block_ended = GF_FALSE;
					//block not complete, don't post process task
				}

			} else {
				ref_count_inc(&dst->filter->pending_packets);

				gf_fq_add(dst->packets, inst);
				post_task = GF_TRUE;
			}
			if (post_task) {
				gf_fs_post_task(pid->filter->session, gf_filter_process_task, dst->filter, pid, "process", NULL);
			}
		}
	}

	//unprotect the packet now that it is safely dispatched
	if (ref_count_dec(&pck->reference_count) == 0) {
		if (!nb_dispatch) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("No PID destination on filter %s for packet - discarding\n", pid->filter->name));
		}
		gf_filter_packet_destroy(pck);
	}
	return GF_OK;
}

void gf_filter_pck_ref(GF_FilterPacket *pck)
{
	assert(pck);
	pck=pck->pck;
	ref_count_inc(&pck->reference_count);
}

void gf_filter_pck_unref(GF_FilterPacket *pck)
{
	assert(pck);
	pck=pck->pck;
	if (ref_count_dec(&pck->reference_count) == 0) {
		gf_filter_packet_destroy(pck);
	}
}

const char *gf_filter_pck_get_data(GF_FilterPacket *pck, u32 *size)
{
	assert(pck);
	assert(size);
	//get true packet pointer
	pck=pck->pck;
	*size = pck->data_length;
	return (const char *)pck->data;
}

static GF_Err gf_filter_pck_set_property_full(GF_FilterPacket *pck, u32 prop_4cc, const char *prop_name, char *dyn_name, const GF_PropertyValue *value)
{
	u32 hash;
	assert(pck);
	assert(pck->pid);
	if (PCK_IS_INPUT(pck)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to set property on an input packet in filter %s\n", pck->pid->filter->name));
		return GF_BAD_PARAM;
	}
	//get true packet pointer
	pck=pck->pck;
	hash = gf_props_hash_djb2(prop_4cc, prop_name ? prop_name : dyn_name);

	if (!pck->props) {
		pck->props = gf_props_new(pck->pid->filter);
	} else {
		gf_props_remove_property(pck->props, hash, prop_4cc, prop_name ? prop_name : dyn_name);
	}
	return gf_props_insert_property(pck->props, hash, prop_4cc, prop_name, dyn_name, value);
}

GF_Err gf_filter_pck_set_property(GF_FilterPacket *pck, u32 prop_4cc, const GF_PropertyValue *value)
{
	return gf_filter_pck_set_property_full(pck, prop_4cc, NULL, NULL, value);
}

GF_Err gf_filter_pck_set_property_str(GF_FilterPacket *pck, const char *name, const GF_PropertyValue *value)
{
	return gf_filter_pck_set_property_full(pck, 0, name, NULL, value);
}

GF_Err gf_filter_pck_set_property_dyn(GF_FilterPacket *pck, char *name, const GF_PropertyValue *value)
{
	return gf_filter_pck_set_property_full(pck, 0, NULL, name, value);
}

GF_Err gf_filter_pck_merge_properties(GF_FilterPacket *pck_src, GF_FilterPacket *pck_dst)
{
	if (PCK_IS_INPUT(pck_dst)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to set property on an input packet in filter %s\n", pck_dst->pid->filter->name));
		return GF_BAD_PARAM;
	}
	//we allow copying over properties from dest packets to dest packets

	//get true packet pointer
	pck_src=pck_src->pck;
	pck_dst=pck_dst->pck;

	if (!pck_src->props) {
		if (pck_dst->props) {
			gf_props_del(pck_dst->props);
			pck_dst->props=NULL;
		}
		return GF_OK;
	}

	if (!pck_dst->props) {
		pck_dst->props = gf_props_new(pck_dst->pid->filter);

		if (!pck_dst->props) return GF_OUT_OF_MEM;
	}
	return gf_props_merge_property(pck_dst->props, pck_src->props);
}

const GF_PropertyValue *gf_filter_pck_get_property(GF_FilterPacket *pck, u32 prop_4cc)
{
	//get true packet pointer
	pck = pck->pck;
	if (!pck->props) return NULL;
	return gf_props_get_property(pck->props, prop_4cc, NULL);
}

const GF_PropertyValue *gf_filter_pck_get_property_str(GF_FilterPacket *pck, const char *prop_name)
{
	//get true packet pointer
	pck = pck->pck;
	if (!pck->props) return NULL;
	return gf_props_get_property(pck->props, 0, prop_name);
}

GF_Err gf_filter_pck_set_framing(GF_FilterPacket *pck, Bool is_start, Bool is_end)
{
	if (PCK_IS_INPUT(pck)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to set framing info on an input packet in filter %s\n", pck->pid->filter->name));
		return GF_BAD_PARAM;
	}
	//get true packet pointer
	pck=pck->pck;

	pck->data_block_start = is_start;
	pck->data_block_end = is_end;
	return GF_OK;
}

GF_Err gf_filter_pck_get_framing(GF_FilterPacket *pck, Bool *is_start, Bool *is_end)
{
	//get true packet pointer
	pck=pck->pck;

	if (is_start) *is_start = pck->data_block_start;
	if (is_end) *is_end = pck->data_block_end;
	return GF_OK;
}

const GF_PropertyValue *gf_filter_pck_enum_properties(GF_FilterPacket *pck, u32 *idx, u32 *prop_4cc, const char **prop_name)
{
	if (!pck->pck->props) return NULL;
	return gf_props_enum_property(pck->pck->props, idx, prop_4cc, prop_name);
}
