/* 
 * Copyright (C) 2008 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <crm_internal.h>

#include <sys/param.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

#include <clplumbing/uids.h>
#include <clplumbing/cl_uuid.h>
#include <clplumbing/cl_malloc.h>
#include <clplumbing/Gmain_timeout.h>

#include <crm/crm.h>
#include <crm/cib.h>
#include <crm/msg_xml.h>
#include <crm/common/ipc.h>
#include <crm/common/cluster.h>
#include <crm/common/ctrl.h>
#include <crm/common/xml.h>
#include <crm/common/msg.h>

#include <cibio.h>
#include <callbacks.h>
#include <cibmessages.h>
#include <cibprimatives.h>
#include "common.h"

extern gboolean cib_is_master;
extern const char* cib_root;
extern gboolean stand_alone;
extern enum cib_errors cib_status;
extern gboolean can_write(int flags);
extern enum cib_errors cib_perform_command(
    HA_Message *request, HA_Message **reply, crm_data_t **cib_diff, gboolean privileged);


static HA_Message *
cib_prepare_common(HA_Message *root, const char *section)
{
    HA_Message *data = NULL;
	
    /* extract the CIB from the fragment */
    if(root == NULL) {
	return NULL;

    } else if(safe_str_eq(crm_element_name(root), XML_TAG_FRAGMENT)
	      || safe_str_eq(crm_element_name(root), F_CIB_CALLDATA)) {
	data = find_xml_node(root, XML_TAG_CIB, TRUE);
	if(data != NULL) {
	    crm_debug_3("Extracted CIB from %s", TYPE(root));
	} else {
	    crm_log_xml_debug_4(root, "No CIB");
	}
		
    } else {
	data = root;
    }

    /* grab the section specified for the command */
    if(section != NULL
       && data != NULL
       && safe_str_eq(crm_element_name(data), XML_TAG_CIB)){
	int rc = revision_check(data, the_cib, 0/* call_options */);
	if(rc == cib_ok) {
	    data = get_object_root(section, data);
	    if(data != NULL) {
		crm_debug_3("Extracted %s from CIB", section);
	    } else {
		crm_log_xml_debug_4(root, "No Section");
	    }
	} else {
	    crm_debug_2("Revision check failed");
	}
    }

    crm_log_xml_debug_4(root, "cib:input");
    return copy_xml(data);
}

static gboolean
verify_section(const char *section)
{
    if(section == NULL) {
	return TRUE;
    } else if(safe_str_eq(section, XML_TAG_CIB)) {
	return TRUE;
    } else if(safe_str_eq(section, XML_CIB_TAG_STATUS)) {
	return TRUE;
    } else if(safe_str_eq(section, XML_CIB_TAG_CRMCONFIG)) {
	return TRUE;
    } else if(safe_str_eq(section, XML_CIB_TAG_NODES)) {
	return TRUE;
    } else if(safe_str_eq(section, XML_CIB_TAG_RESOURCES)) {
	return TRUE;
    } else if(safe_str_eq(section, XML_CIB_TAG_CONSTRAINTS)) {
	return TRUE;
    }
    return FALSE;
}


static enum cib_errors
cib_prepare_none(HA_Message *request, HA_Message **data, const char **section)
{
    *data = NULL;
    *section = cl_get_string(request, F_CIB_SECTION);
    if(verify_section(*section) == FALSE) {
	return cib_bad_section;
    }
    return cib_ok;
}

static enum cib_errors
cib_prepare_data(HA_Message *request, HA_Message **data, const char **section)
{
    HA_Message *input_fragment = get_message_xml(request, F_CIB_CALLDATA);
    *section = cl_get_string(request, F_CIB_SECTION);
    *data = cib_prepare_common(input_fragment, *section);
    free_xml(input_fragment);
    if(verify_section(*section) == FALSE) {
	return cib_bad_section;
    }
    return cib_ok;
}

static enum cib_errors
cib_prepare_sync(HA_Message *request, HA_Message **data, const char **section)
{
    *section = cl_get_string(request, F_CIB_SECTION);
    *data = request;
    if(verify_section(*section) == FALSE) {
	return cib_bad_section;
    }
    return cib_ok;
}

static enum cib_errors
cib_prepare_diff(HA_Message *request, HA_Message **data, const char **section)
{
    HA_Message *input_fragment = NULL;
    const char *update     = cl_get_string(request, F_CIB_GLOBAL_UPDATE);

    *data = NULL;
    *section = NULL;

    if(crm_is_true(update)) {
	input_fragment = get_message_xml(request,F_CIB_UPDATE_DIFF);
		
    } else {
	input_fragment = get_message_xml(request, F_CIB_CALLDATA);
    }

    CRM_CHECK(input_fragment != NULL,crm_log_message(LOG_WARNING, request));
    *data = cib_prepare_common(input_fragment, NULL);
    free_xml(input_fragment);
    return cib_ok;
}

static enum cib_errors
cib_cleanup_query(const char *op, HA_Message **data, HA_Message **output) 
{
    CRM_DEV_ASSERT(*data == NULL);
    return cib_ok;
}

static enum cib_errors
cib_cleanup_data(const char *op, HA_Message **data, HA_Message **output) 
{
    free_xml(*output);
    free_xml(*data);
    return cib_ok;
}

static enum cib_errors
cib_cleanup_output(const char *op, HA_Message **data, HA_Message **output) 
{
    free_xml(*output);
    return cib_ok;
}

static enum cib_errors
cib_cleanup_none(const char *op, HA_Message **data, HA_Message **output) 
{
    CRM_DEV_ASSERT(*data == NULL);
    CRM_DEV_ASSERT(*output == NULL);
    return cib_ok;
}

static enum cib_errors
cib_cleanup_sync(const char *op, HA_Message **data, HA_Message **output) 
{
    /* data is non-NULL but doesnt need to be free'd */
    CRM_DEV_ASSERT(*output == NULL);
    return cib_ok;
}

/*
  typedef struct cib_operation_s
  {
  const char* 	operation;
  gboolean	modifies_cib;
  gboolean	needs_privileges;
  gboolean	needs_quorum;
  enum cib_errors (*prepare)(HA_Message *, crm_data_t**, const char **);
  enum cib_errors (*cleanup)(crm_data_t**, crm_data_t**);
  enum cib_errors (*fn)(
  const char *, int, const char *,
  crm_data_t*, crm_data_t*, crm_data_t**, crm_data_t**);
  } cib_operation_t;
*/
/* technically bump does modify the cib...
 * but we want to split the "bump" from the "sync"
 */
static cib_operation_t cib_server_ops[] = {
    {NULL,		   FALSE, FALSE, FALSE, cib_prepare_none, cib_cleanup_none,   cib_process_default},
    {CIB_OP_QUERY,     FALSE, FALSE, FALSE, cib_prepare_none, cib_cleanup_query,  cib_process_query},
    {CIB_OP_MODIFY,    TRUE,  TRUE,  TRUE,  cib_prepare_data, cib_cleanup_data,   cib_process_modify},
    {CIB_OP_UPDATE,    TRUE,  TRUE,  TRUE,  cib_prepare_data, cib_cleanup_data,   cib_process_change},
    {CIB_OP_APPLY_DIFF,TRUE,  TRUE,  TRUE,  cib_prepare_diff, cib_cleanup_data,   cib_process_diff},
    {CIB_OP_SLAVE,     FALSE, TRUE,  FALSE, cib_prepare_none, cib_cleanup_none,   cib_process_readwrite},
    {CIB_OP_SLAVEALL,  FALSE, TRUE,  FALSE, cib_prepare_none, cib_cleanup_none,   cib_process_readwrite},
    {CIB_OP_SYNC_ONE,  FALSE, TRUE,  FALSE, cib_prepare_sync, cib_cleanup_sync,   cib_process_sync_one},
    {CIB_OP_MASTER,    FALSE, TRUE,  FALSE, cib_prepare_none, cib_cleanup_none,   cib_process_readwrite},
    {CIB_OP_ISMASTER,  FALSE, TRUE,  FALSE, cib_prepare_none, cib_cleanup_none,   cib_process_readwrite},
    {CIB_OP_BUMP,      TRUE,  TRUE,  TRUE,  cib_prepare_none, cib_cleanup_output, cib_process_bump},
    {CIB_OP_REPLACE,   TRUE,  TRUE,  TRUE,  cib_prepare_data, cib_cleanup_data,   cib_process_replace},
    {CIB_OP_CREATE,    TRUE,  TRUE,  TRUE,  cib_prepare_data, cib_cleanup_data,   cib_process_change},
    {CIB_OP_DELETE,    TRUE,  TRUE,  TRUE,  cib_prepare_data, cib_cleanup_data,   cib_process_delete},
    {CIB_OP_DELETE_ALT,TRUE,  TRUE,  TRUE,  cib_prepare_data, cib_cleanup_data,   cib_process_change},
    {CIB_OP_SYNC,      FALSE, TRUE,  FALSE, cib_prepare_sync, cib_cleanup_sync,   cib_process_sync},
    {CRM_OP_QUIT,	   FALSE, TRUE,  FALSE, cib_prepare_none, cib_cleanup_none,   cib_process_quit},
    {CRM_OP_PING,	   FALSE, FALSE, FALSE, cib_prepare_none, cib_cleanup_output, cib_process_ping},
    {CIB_OP_ERASE,     TRUE,  TRUE,  TRUE,  cib_prepare_none, cib_cleanup_output, cib_process_erase},
    {CRM_OP_NOOP,	   FALSE, FALSE, FALSE, cib_prepare_none, cib_cleanup_none,   cib_process_default},
    {"cib_shutdown_req",FALSE, TRUE, FALSE, cib_prepare_sync, cib_cleanup_sync,   cib_process_shutdown_req},
};

enum cib_errors
cib_get_operation_id(const char *op, int *operation) 
{
    int lpc = 0;
    int max_msg_types = DIMOF(cib_server_ops);

    for (lpc = 0; lpc < max_msg_types; lpc++) {
	if (safe_str_eq(op, cib_server_ops[lpc].operation)) {
	    *operation = lpc;
	    return cib_ok;
	}
    }
    crm_err("Operation %s is not valid", op);
    *operation = -1;
    return cib_operation;
}

HA_Message *
cib_msg_copy(HA_Message *msg, gboolean with_data) 
{
	int lpc = 0;
	const char *field = NULL;
	const char *value = NULL;
	HA_Message *value_struct = NULL;

	static const char *field_list[] = {
		F_XML_TAGNAME	,
		F_TYPE		,
		F_CIB_CLIENTID  ,
		F_CIB_CALLOPTS  ,
		F_CIB_CALLID    ,
		F_CIB_OPERATION ,
		F_CIB_ISREPLY   ,
		F_CIB_SECTION   ,
		F_CIB_HOST	,
		F_CIB_RC	,
		F_CIB_DELEGATED	,
		F_CIB_OBJID	,
		F_CIB_OBJTYPE	,
		F_CIB_EXISTING	,
		F_CIB_SEENCOUNT	,
		F_CIB_TIMEOUT	,
		F_CIB_CALLBACK_TOKEN	,
		F_CIB_GLOBAL_UPDATE	,
		F_CIB_CLIENTNAME	,
		F_CIB_NOTIFY_TYPE	,
		F_CIB_NOTIFY_ACTIVATE
	};
	
	static const char *data_list[] = {
		F_CIB_CALLDATA  ,
		F_CIB_UPDATE	,
		F_CIB_UPDATE_RESULT
	};

	HA_Message *copy = ha_msg_new(10);
	CRM_ASSERT(copy != NULL);
	
	for(lpc = 0; lpc < DIMOF(field_list); lpc++) {
		field = field_list[lpc];
		value = cl_get_string(msg, field);
		if(value != NULL) {
			ha_msg_add(copy, field, value);
		}
	}
	for(lpc = 0; with_data && lpc < DIMOF(data_list); lpc++) {
		field = data_list[lpc];
		value_struct = get_message_xml(msg, field);
		if(value_struct != NULL) {
			add_message_xml(copy, field, value_struct);
		}
		free_xml(value_struct);
	}

	return copy;
}

enum cib_errors
cib_perform_op(
    const char *op, int call_options, const char *section, crm_data_t *input,
    gboolean manage_counters, gboolean *config_changed,
    crm_data_t *current_cib, crm_data_t **result_cib, crm_data_t **output)
{
    int rc = cib_ok;
    int call_type = 0;
    crm_data_t *scratch = NULL;

    CRM_CHECK(output != NULL && result_cib != NULL && config_changed != NULL,
	      return cib_output_data);
    
    *output = NULL;
    *result_cib = NULL;
    rc = cib_get_operation_id(op, &call_type);
    *config_changed = FALSE;

    if(rc != cib_ok) {
	return rc;
    }
    
    if(cib_server_ops[call_type].modifies_cib == FALSE) {
	return cib_server_ops[call_type].fn(
	    op, call_options, section, input, current_cib, result_cib, output);
    }
    
    scratch = copy_xml(current_cib);
    rc = cib_server_ops[call_type].fn(
	op, call_options, section, input, current_cib, &scratch, output);    

    CRM_CHECK(current_cib != scratch, return cib_unknown);
    
    if(rc == cib_ok) {

	CRM_CHECK(scratch != NULL, return cib_unknown);
	
	if(do_id_check(scratch, NULL, TRUE, FALSE)) {
	    rc = cib_id_check;
	    if(call_options & cib_force_diff) {
		crm_err("Global update introduces id collision!");
	    }
	}
	
	if(rc == cib_ok) {
	    fix_plus_plus_recursive(scratch);
	    *config_changed = cib_config_changed(current_cib, scratch, NULL);
	    
	    if(manage_counters && *config_changed) {
		cib_update_counter(scratch, XML_ATTR_NUMUPDATES, TRUE);
		cib_update_counter(scratch, XML_ATTR_GENERATION, FALSE);
	    } else if(manage_counters) {
		cib_update_counter(scratch, XML_ATTR_NUMUPDATES, FALSE);
	    }	
	}
    }

    *result_cib = scratch;
    return rc;
}
