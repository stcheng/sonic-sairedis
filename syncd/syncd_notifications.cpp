#include "syncd.h"
#include "sairedis.h"

void send_notification(
        _In_ std::string op,
        _In_ std::string data,
        _In_ std::vector<swss::FieldValueTuple> &entry)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_INFO("%s %s", op.c_str(), data.c_str());

    notifications->send(op, data, entry);

    SWSS_LOG_DEBUG("notification send successfull");
}

void send_notification(
        _In_ std::string op,
        _In_ std::string data)
{
    SWSS_LOG_ENTER();

    std::vector<swss::FieldValueTuple> entry;

    send_notification(op, data, entry);
}

void on_switch_state_change(
        _In_ sai_object_id_t switch_rid,
        _In_ sai_switch_oper_status_t switch_oper_status)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    SWSS_LOG_ENTER();

    sai_object_id_t switch_vid = translate_rid_to_vid(switch_rid, SAI_NULL_OBJECT_ID);

    std::string s = sai_serialize_switch_oper_status(switch_vid, switch_oper_status);

    send_notification("switch_state_change", s);
}

sai_fdb_entry_type_t getFdbEntryType(
        _In_ uint32_t count,
        _In_ const sai_attribute_t *list)
{
    for (uint32_t idx = 0; idx < count; idx++)
    {
        const sai_attribute_t &attr = list[idx];

        if (attr.id == SAI_FDB_ENTRY_ATTR_TYPE)
        {
            return (sai_fdb_entry_type_t)attr.value.s32;
        }
    }

    SWSS_LOG_WARN("unknown fdb entry type");

    int ret = -1;
    return (sai_fdb_entry_type_t)ret;
}

void redisPutFdbEntryToAsicView(
        _In_ const sai_fdb_event_notification_data_t *fdb)
{
    // NOTE: this fdb entry already contains translated RID to VID

    std::vector<swss::FieldValueTuple> entry;

    entry = SaiAttributeList::serialize_attr_list(
            SAI_OBJECT_TYPE_FDB_ENTRY,
            fdb->attr_count,
            fdb->attr,
            false);

    sai_object_type_t objectType = SAI_OBJECT_TYPE_FDB_ENTRY;

    std::string strObjectType = sai_serialize_object_type(objectType);

    std::string strFdbEntry = sai_serialize_fdb_entry(fdb->fdb_entry);

    std::string key = ASIC_STATE_TABLE + (":" + strObjectType + ":" + strFdbEntry);

    if (fdb->fdb_entry.switch_id == SAI_NULL_OBJECT_ID ||
        fdb->fdb_entry.bridge_id == SAI_NULL_OBJECT_ID ||
        sai_metadata_get_fdb_entry_bridge_type_name(fdb->fdb_entry.bridge_type) == NULL)
    {
        SWSS_LOG_WARN("skipped to put int db: %s", strFdbEntry.c_str());
        return;
    }

    for (const auto &e: entry)
    {
        const std::string &strField = fvField(e);
        const std::string &strValue = fvValue(e);

        g_redisClient->hset(key, strField, strValue);
    }

    // currently we need to add type manually since fdb event don't contain type
    sai_attribute_t attr;

    attr.id = SAI_FDB_ENTRY_ATTR_TYPE;
    attr.value.s32 = SAI_FDB_ENTRY_TYPE_DYNAMIC;

    auto meta = sai_metadata_get_attr_metadata(objectType, attr.id);

    if (meta == NULL)
    {
        SWSS_LOG_THROW("unable to get metadata for object type %s, attribute %d",
                sai_serialize_object_type(objectType).c_str(),
                attr.id);
        /*
         * TODO We should notify orch agent here. And also this probably should
         * not be here, but on redis side, getting through metadata.
         */
    }

    std::string strAttrId = sai_serialize_attr_id(*meta);
    std::string strAttrValue = sai_serialize_attr_value(*meta, attr);

    g_redisClient->hset(key, strAttrId, strAttrValue);
}

void on_fdb_event(
        _In_ uint32_t count,
        _In_ sai_fdb_event_notification_data_t *data)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    SWSS_LOG_ENTER();

    SWSS_LOG_DEBUG("fdb event count: %d", count);

    for (uint32_t i = 0; i < count; i++)
    {
        sai_fdb_event_notification_data_t *fdb = &data[i];

        SWSS_LOG_DEBUG("fdb %u: type: %d", i, fdb->event_type);

        translate_rid_to_vid_list(SAI_OBJECT_TYPE_FDB_ENTRY, fdb->fdb_entry.switch_id, fdb->attr_count, fdb->attr);

        /*
         * Currently because of bcrm bug, we need to install fdb entries in
         * asic view and currently this event don't have fdb type which is
         * required on creation.
         */

        redisPutFdbEntryToAsicView(fdb);
    }

    std::string s = sai_serialize_fdb_event_ntf(count, data);

    send_notification("fdb_event", s);
}

void on_port_state_change(
        _In_ uint32_t count,
        _In_ sai_port_oper_status_notification_t *data)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    SWSS_LOG_ENTER();

    SWSS_LOG_DEBUG("port notification count: %u", count);

    for (uint32_t i = 0; i < count; i++)
    {
        sai_port_oper_status_notification_t *oper_stat = &data[i];

        /*
         * We are using switch_rid as null, since port should be already
         * defined inside local db after creation.
         *
         * If this will be faster than return from create port then we can use
         * query switch id and extract rid of switch id and then convert it to
         * switch vid.
         */

        oper_stat->port_id = translate_rid_to_vid(oper_stat->port_id, SAI_NULL_OBJECT_ID);
    }

    std::string s = sai_serialize_port_oper_status_ntf(count, data);

    send_notification("port_state_change", s);
}

void on_switch_shutdown_request(
        _In_ sai_object_id_t switch_rid)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    SWSS_LOG_ENTER();

    sai_object_id_t switch_vid = translate_rid_to_vid(switch_rid, SAI_NULL_OBJECT_ID);

    std::string s = sai_serialize_switch_shutdown_request(switch_vid);

    send_notification("switch_shutdown_request", s);
}

void on_packet_event(
        _In_ sai_object_id_t switch_id,
        _In_ const void *buffer,
        _In_ sai_size_t buffer_size,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    SWSS_LOG_ENTER();

    SWSS_LOG_ERROR("not implemented");

    /*
       TODO translate switch id
       std::string s;
       sai_serialize_primitive(buffer_size, s);

       sai_serialize_buffer(buffer, buffer_size, s);

       std::vector<swss::FieldValueTuple> entry;

       entry = SaiAttributeList::serialize_attr_list(
       SAI_OBJECT_TYPE_PACKET,
       attr_count,
       attr_list,
       false);

    // since attr_list is const, we can't replace rid's
    // we need to create copy of that list

    SaiAttributeList copy(SAI_OBJECT_TYPE_PACKET, entry, false);

    translate_rid_to_vid_list(SAI_OBJECT_TYPE_PACKET, copy.get_attr_count(), copy.get_attr_list());

    entry = SaiAttributeList::serialize_attr_list(
    SAI_OBJECT_TYPE_PACKET,
    copy.get_attr_count(),
    copy.get_attr_list(),
    false);

    send_notification("packet_event", s, entry);
    */
}

sai_switch_state_change_notification_fn     on_switch_state_change_ntf = on_switch_state_change;
sai_switch_shutdown_request_notification_fn on_switch_shutdown_request_ntf = on_switch_shutdown_request;
sai_fdb_event_notification_fn               on_fdb_event_ntf = on_fdb_event;
sai_port_state_change_notification_fn       on_port_state_change_ntf = on_port_state_change;
sai_packet_event_notification_fn            on_packet_event_ntf = on_packet_event;

void check_notifications_pointers(
        _In_ uint32_t attr_count,
        _In_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    /*
     * This function should only be called on CREATE/SET api when object is
     * SWITCH.
     *
     * Notifications pointers needs to be corrected since those we receive from
     * sairedis are in sairedis memory space and here we are using those ones
     * we declared in syncd memory space.
     *
     * Also notice that we are using the same pointers for ALL switches.
     */

    for (uint32_t index = 0; index < attr_count; ++index)
    {
        sai_attribute_t &attr = attr_list[index];

        auto meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_SWITCH, attr.id);

        if (meta->attrvaluetype != SAI_ATTR_VALUE_TYPE_POINTER)
        {
            continue;
        }

        /*
         * Does not matter if pointer is valid or not, we just want the
         * previous value.
         */

        sai_pointer_t prev = attr.value.ptr;

        if (prev == NULL)
        {
            /*
             * If pointer is NULL, then fine, let it be.
             */

            continue;
        }

        switch (attr.id)
        {
            case SAI_SWITCH_ATTR_SWITCH_STATE_CHANGE_NOTIFY:
                attr.value.ptr = (void*)on_switch_state_change_ntf;
                break;

            case SAI_SWITCH_ATTR_SHUTDOWN_REQUEST_NOTIFY:
                attr.value.ptr = (void*)on_switch_shutdown_request_ntf;
                break;

            case SAI_SWITCH_ATTR_FDB_EVENT_NOTIFY:
                attr.value.ptr = (void*)on_fdb_event_ntf;
                break;

            case SAI_SWITCH_ATTR_PORT_STATE_CHANGE_NOTIFY:
                attr.value.ptr = (void*)on_port_state_change_ntf;
                break;

            case SAI_SWITCH_ATTR_PACKET_EVENT_NOTIFY:
                attr.value.ptr = (void*)on_packet_event_ntf;
                break;

            default:

                SWSS_LOG_ERROR("pointer for %s is not handled, FIXME!", meta->attridname);
                continue;
        }

        /*
         * Here we translated pointer, just log it.
         */

        SWSS_LOG_INFO("%s: %lp (orch) => %lp (syncd)", meta->attridname, prev, attr.value.ptr);
    }
}
