#include <thread>
#include "syncd.h"

std::mutex g_mutex;
swss::RedisClient *g_redisClient = NULL;

std::map<std::string, std::string> gProfileMap;

void sai_diag_shell()
{
    SWSS_LOG_ENTER();

    sai_status_t status;

    while (true)
    {
        sai_attribute_t attr;
        attr.id = SAI_SWITCH_ATTR_CUSTOM_RANGE_BASE + 1;
        status = sai_switch_api->set_switch_attribute(&attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("open sai shell failed %d", status);
            return;
        }

        sleep(1);
    }
}

sai_object_id_t redis_create_virtual_object_id(
        _In_ sai_object_type_t object_type)
{
    SWSS_LOG_ENTER();

    uint64_t virtual_id = g_redisClient->incr(VIDCOUNTER);

    sai_object_id_t vid = (((sai_object_id_t)object_type) << 48) | virtual_id;

    SWSS_LOG_DEBUG("created virtual object id %llx for object type %x", vid, object_type);

    return vid;
}

sai_object_id_t translate_rid_to_vid(
        _In_ sai_object_id_t rid)
{
    SWSS_LOG_ENTER();

    if (rid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_DEBUG("translated RID null to VID null");

        return SAI_NULL_OBJECT_ID;
    }

    sai_object_id_t vid;

    std::string str_rid;
    std::string str_vid;

    sai_serialize_primitive(rid, str_rid);

    auto pvid = g_redisClient->hget(RIDTOVID, str_rid);

    if (pvid != NULL)
    {
        // object exists
        str_vid = *pvid;

        int index = 0;
        sai_deserialize_primitive(str_vid, index, vid);

        SWSS_LOG_DEBUG("translated RID %llx to VID %llx", rid, vid);

        return vid;
    }

    SWSS_LOG_INFO("spotted new RID %llx", rid);

    sai_object_type_t object_type = sai_object_type_query(rid);

    if (object_type == SAI_OBJECT_TYPE_NULL)
    {
        SWSS_LOG_ERROR("sai_object_type_query returned NULL type for RID %llx", rid);

        exit(EXIT_FAILURE);
    }

    vid = redis_create_virtual_object_id(object_type);

    SWSS_LOG_DEBUG("translated RID %llx to VID %llx", rid, vid);

    sai_serialize_primitive(vid, str_vid);

    g_redisClient->hset(RIDTOVID, str_rid, str_vid);
    g_redisClient->hset(VIDTORID, str_vid, str_rid);

    return vid;
}

template <typename T>
void translate_list_rid_to_vid(
        _In_ T &element)
{
    SWSS_LOG_ENTER();

    for (uint32_t i = 0; i < element.count; i++)
    {
        element.list[i] = translate_rid_to_vid(element.list[i]);
    }
}

void translate_rid_to_vid_list(
        _In_ sai_object_type_t object_type,
        _In_ uint32_t attr_count,
        _In_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    // we receive real id's here, if they are new then create new id
    // for them and put in db, if entry exists in db, use it

    for (uint32_t i = 0; i < attr_count; i++)
    {
        sai_attribute_t &attr = attr_list[i];

        sai_attr_serialization_type_t serialization_type;
        sai_status_t status = sai_get_serialization_type(object_type, attr.id, serialization_type);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("unable to find serialization type for object type %x, attribute %x", object_type, attr.id);

            exit(EXIT_FAILURE);
        }

        switch (serialization_type)
        {
            case SAI_SERIALIZATION_TYPE_OBJECT_ID:
                attr.value.oid = translate_rid_to_vid(attr.value.oid);
                break;

            case SAI_SERIALIZATION_TYPE_OBJECT_LIST:
                translate_list_rid_to_vid(attr.value.objlist);
                break;

            case SAI_SERIALIZATION_TYPE_ACL_FIELD_DATA_OBJECT_ID:
                attr.value.aclfield.data.oid = translate_rid_to_vid(attr.value.aclfield.data.oid);
                break;

            case SAI_SERIALIZATION_TYPE_ACL_FIELD_DATA_OBJECT_LIST:
                translate_list_rid_to_vid(attr.value.aclfield.data.objlist);
                break;

            case SAI_SERIALIZATION_TYPE_ACL_ACTION_DATA_OBJECT_ID:
                attr.value.aclaction.parameter.oid = translate_rid_to_vid(attr.value.aclaction.parameter.oid);
                break;

            case SAI_SERIALIZATION_TYPE_ACL_ACTION_DATA_OBJECT_LIST:
                translate_list_rid_to_vid(attr.value.aclaction.parameter.objlist);
                break;

            case SAI_SERIALIZATION_TYPE_PORT_BREAKOUT:
                translate_list_rid_to_vid(attr.value.portbreakout.port_list);

            default:
                break;
        }
    }
}

sai_object_id_t translate_vid_to_rid(
        _In_ sai_object_id_t vid)
{
    SWSS_LOG_ENTER();

    if (vid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_DEBUG("translated RID null to VID null");
        return SAI_NULL_OBJECT_ID;
    }

    std::string str_vid;
    sai_serialize_primitive(vid, str_vid);

    std::string str_rid;

    auto prid = g_redisClient->hget(VIDTORID, str_vid);

    if (prid == NULL)
    {
        SWSS_LOG_ERROR("unable to get RID for VID: %s", str_vid.c_str());

        exit(EXIT_FAILURE);
    }

    str_rid = *prid;

    sai_object_id_t rid;

    int index = 0;
    sai_deserialize_primitive(str_rid, index, rid);

    SWSS_LOG_DEBUG("translated VID %llx to RID %llx", vid, rid);

    return rid;
}

void translate_vid_to_rid_list(
        _In_ sai_object_type_t object_type,
        _In_ uint32_t attr_count,
        _In_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    // all id's reseived from sairedis should be virtual, so
    // lets translate them to real id's

    for (uint32_t i = 0; i < attr_count; i++)
    {
        sai_attribute_t &attr = attr_list[i];

        sai_attr_serialization_type_t serialization_type;
        sai_status_t status = sai_get_serialization_type(object_type, attr.id, serialization_type);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("unable to find serialization type for object type %x, attribute %x", object_type, attr.id);

            exit(EXIT_FAILURE);
        }

        switch (serialization_type)
        {
            case SAI_SERIALIZATION_TYPE_OBJECT_ID:
                attr.value.oid = translate_vid_to_rid(attr.value.oid);
                break;

            case SAI_SERIALIZATION_TYPE_OBJECT_LIST:
                translate_list_vid_to_rid(attr.value.objlist);
                break;

            case SAI_SERIALIZATION_TYPE_ACL_FIELD_DATA_OBJECT_ID:
                attr.value.aclfield.data.oid = translate_vid_to_rid(attr.value.aclfield.data.oid);
                break;

            case SAI_SERIALIZATION_TYPE_ACL_FIELD_DATA_OBJECT_LIST:
                translate_list_vid_to_rid(attr.value.aclfield.data.objlist);
                break;

            case SAI_SERIALIZATION_TYPE_ACL_ACTION_DATA_OBJECT_ID:
                attr.value.aclaction.parameter.oid = translate_vid_to_rid(attr.value.aclaction.parameter.oid);
                break;

            case SAI_SERIALIZATION_TYPE_ACL_ACTION_DATA_OBJECT_LIST:
                translate_list_vid_to_rid(attr.value.aclaction.parameter.objlist);
                break;

            case SAI_SERIALIZATION_TYPE_PORT_BREAKOUT:
                translate_list_vid_to_rid(attr.value.portbreakout.port_list);

            default:
                break;
        }
    }
}

void internal_syncd_get_send(
        _In_ sai_object_type_t object_type,
        _In_ sai_status_t status,
        _In_ uint32_t attr_count,
        _In_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    std::vector<swss::FieldValueTuple> entry;

    if (status == SAI_STATUS_SUCCESS)
    {
        translate_rid_to_vid_list(object_type, attr_count, attr_list);

        // XXX: normal serialization + translate reverse
        entry = SaiAttributeList::serialize_attr_list(
                object_type,
                attr_count,
                attr_list,
                false);
    }
    else if (status == SAI_STATUS_BUFFER_OVERFLOW)
    {
        // in this case we got correct values for list, but list was too small
        // so serialize only count without list itself, sairedis will need to take this
        // into accoung when deseralzie
        // if there was a list somewhere, count will be changed to actual value
        // different attributes can have different lists, many of them may
        // serialize only count, and will need to support that on the receiver

        entry = SaiAttributeList::serialize_attr_list(
                object_type,
                attr_count,
                attr_list,
                true);
    }
    else
    {
        // some other error, dont send attributes at all
    }

    std::string str_status;
    sai_serialize_primitive(status, str_status);

    std::string key = str_status;

    // since we have only one get at a time, we don't have to serialize
    // object type and object id, only get status is required
    getResponse->set(key, entry, "getresponse");
    getResponse->del(key, "delgetresponse");
}


swss::ConsumerTable *getRequest = NULL;
swss::ProducerTable *getResponse = NULL;
swss::ProducerTable *notifications = NULL;

const char* dummy_profile_get_value(
        _In_ sai_switch_profile_id_t profile_id,
        _In_ const char* variable)
{
    auto it = gProfileMap.find(variable);

    if (it == gProfileMap.end())
        return NULL;

    return it->second.c_str();

}

int dummy_profile_get_next_value(
        _In_ sai_switch_profile_id_t profile_id,
        _Out_ const char** variable,
        _Out_ const char** value)
{
    UNREFERENCED_PARAMETER(profile_id);
    UNREFERENCED_PARAMETER(variable);
    UNREFERENCED_PARAMETER(value);

    return -1;
}

const service_method_table_t test_services = {
    dummy_profile_get_value,
    dummy_profile_get_next_value
};

sai_status_t handle_generic(
        _In_ sai_object_type_t object_type,
        _In_ std::string &str_object_id,
        _In_ sai_common_api_t api,
        _In_ uint32_t attr_count,
        _In_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    int index = 0;
    sai_object_id_t object_id;
    sai_deserialize_primitive(str_object_id, index, object_id);

    SWSS_LOG_DEBUG("common generic api: %d", api);

    switch(api)
    {
        case SAI_COMMON_API_CREATE:
            {
                SWSS_LOG_DEBUG("generic create for object type %x", object_type);

                create_fn create = common_create[object_type];

                if (create == NULL)
                {
                    SWSS_LOG_ERROR("create function is not defined for object type %x", object_type);
                    exit(EXIT_FAILURE);
                }

                sai_object_id_t real_object_id;
                sai_status_t status = create(&real_object_id, attr_count, attr_list);

                if (status == SAI_STATUS_SUCCESS)
                {
                    // object was created so new object id was generated
                    // we need to save virtual id's to redis db

                    std::string str_vid;
                    std::string str_rid;

                    sai_serialize_primitive(object_id, str_vid);
                    sai_serialize_primitive(real_object_id, str_rid);

                    g_redisClient->hset(VIDTORID, str_vid, str_rid);
                    g_redisClient->hset(RIDTOVID, str_rid, str_vid);

                    SWSS_LOG_INFO("saved VID %s to RID %s", str_vid.c_str(), str_rid.c_str());
                }
                else
                {
                    SWSS_LOG_ERROR("failed to create %d", status);
                }

                return status;
            }

        case SAI_COMMON_API_REMOVE:
            {
                SWSS_LOG_DEBUG("generic remove for object type %x", object_type);

                remove_fn remove = common_remove[object_type];

                if (remove == NULL)
                {
                    SWSS_LOG_ERROR("remove function is not defined for object type %x", object_type);
                    exit(EXIT_FAILURE);
                }

                sai_object_id_t rid = translate_vid_to_rid(object_id);

                std::string str_vid;
                sai_serialize_primitive(object_id, str_vid);

                std::string str_rid;
                sai_serialize_primitive(rid, str_rid);

                g_redisClient->hdel(VIDTORID, str_vid);
                g_redisClient->hdel(RIDTOVID, str_rid);

                return remove(rid);
            }

        case SAI_COMMON_API_SET:
            {
                SWSS_LOG_DEBUG("generic set for object type %x", object_type);

                set_attribute_fn set = common_set_attribute[object_type];

                if (set == NULL)
                {
                    SWSS_LOG_ERROR("set function is not defined for object type %x", object_type);
                    exit(EXIT_FAILURE);
                }

                sai_object_id_t rid = translate_vid_to_rid(object_id);

                return set(rid, attr_list);
            }

        case SAI_COMMON_API_GET:
            {
                SWSS_LOG_DEBUG("generic get for object type %x", object_type);

                get_attribute_fn get = common_get_attribute[object_type];

                if (get == NULL)
                {
                    SWSS_LOG_ERROR("get function is not defined for object type %x", object_type);
                    exit(EXIT_FAILURE);
                }

                sai_object_id_t rid = translate_vid_to_rid(object_id);

                return get(rid, attr_count, attr_list);
            }

        default:
            SWSS_LOG_ERROR("generic other apis not implemented");
            exit(EXIT_FAILURE);
    }
}

sai_status_t handle_fdb(
        _In_ std::string &str_object_id,
        _In_ sai_common_api_t api,
        _In_ uint32_t attr_count,
        _In_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    int index = 0;
    sai_fdb_entry_t fdb_entry;
    sai_deserialize_primitive(str_object_id, index, fdb_entry);

    switch(api)
    {
        case SAI_COMMON_API_CREATE:
            return sai_fdb_api->create_fdb_entry(&fdb_entry, attr_count, attr_list);

        case SAI_COMMON_API_REMOVE:
            return sai_fdb_api->remove_fdb_entry(&fdb_entry);

        case SAI_COMMON_API_SET:
            return sai_fdb_api->set_fdb_entry_attribute(&fdb_entry, attr_list);

        case SAI_COMMON_API_GET:
            return sai_fdb_api->get_fdb_entry_attribute(&fdb_entry, attr_count, attr_list);

        default:
            SWSS_LOG_ERROR("fdb other apis not implemented");
            exit(EXIT_FAILURE);
    }
}

sai_status_t handle_switch(
        _In_ std::string &str_object_id,
        _In_ sai_common_api_t api,
        _In_ uint32_t attr_count,
        _In_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    switch(api)
    {
        case SAI_COMMON_API_CREATE:
            return SAI_STATUS_NOT_SUPPORTED;

        case SAI_COMMON_API_REMOVE:
            return SAI_STATUS_NOT_SUPPORTED;

        case SAI_COMMON_API_SET:
            return sai_switch_api->set_switch_attribute(attr_list);

        case SAI_COMMON_API_GET:
            return sai_switch_api->get_switch_attribute(attr_count, attr_list);

        default:
            SWSS_LOG_ERROR("switch other apis not implemented");
            exit(EXIT_FAILURE);
    }
}

sai_status_t handle_neighbor(
        _In_ std::string &str_object_id,
        _In_ sai_common_api_t api,
        _In_ uint32_t attr_count,
        _In_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    int index = 0;
    sai_neighbor_entry_t neighbor_entry;
    sai_deserialize_neighbor_entry(str_object_id, index, neighbor_entry);

    neighbor_entry.rif_id = translate_vid_to_rid(neighbor_entry.rif_id);

    switch(api)
    {
        case SAI_COMMON_API_CREATE:
            return sai_neighbor_api->create_neighbor_entry(&neighbor_entry, attr_count, attr_list);

        case SAI_COMMON_API_REMOVE:
            return sai_neighbor_api->remove_neighbor_entry(&neighbor_entry);

        case SAI_COMMON_API_SET:
            return sai_neighbor_api->set_neighbor_attribute(&neighbor_entry, attr_list);

        case SAI_COMMON_API_GET:
            return sai_neighbor_api->get_neighbor_attribute(&neighbor_entry, attr_count, attr_list);

        default:
            SWSS_LOG_ERROR("neighbor other apis not implemented");
            exit(EXIT_FAILURE);
    }
}

sai_status_t handle_route(
        _In_ std::string &str_object_id,
        _In_ sai_common_api_t api,
        _In_ uint32_t attr_count,
        _In_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    int index = 0;
    sai_unicast_route_entry_t route_entry;
    sai_deserialize_route_entry(str_object_id, index, route_entry);

    route_entry.vr_id = translate_vid_to_rid(route_entry.vr_id);

    switch(api)
    {
        case SAI_COMMON_API_CREATE:
            return sai_route_api->create_route(&route_entry, attr_count, attr_list);

        case SAI_COMMON_API_REMOVE:
            return sai_route_api->remove_route(&route_entry);

        case SAI_COMMON_API_SET:
            return sai_route_api->set_route_attribute(&route_entry, attr_list);

        case SAI_COMMON_API_GET:
            return sai_route_api->get_route_attribute(&route_entry, attr_count, attr_list);

        default:
            SWSS_LOG_ERROR("route other apis not implemented");
            exit(EXIT_FAILURE);
    }
}

sai_status_t handle_vlan(
        _In_ std::string &str_object_id,
        _In_ sai_common_api_t api,
        _In_ uint32_t attr_count,
        _In_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    int index = 0;
    sai_vlan_id_t vlan_id;
    sai_deserialize_primitive(str_object_id, index, vlan_id);

    switch(api)
    {
        case SAI_COMMON_API_CREATE:
            return sai_vlan_api->create_vlan(vlan_id);

        case SAI_COMMON_API_REMOVE:
            return sai_vlan_api->remove_vlan(vlan_id);

        case SAI_COMMON_API_SET:
            return sai_vlan_api->set_vlan_attribute(vlan_id, attr_list);

        case SAI_COMMON_API_GET:
            return sai_vlan_api->get_vlan_attribute(vlan_id, attr_count, attr_list);

        default:
            SWSS_LOG_ERROR("vlan other apis not implemented");
            exit(EXIT_FAILURE);
    }
}

sai_status_t handle_trap(
        _In_ std::string &str_object_id,
        _In_ sai_common_api_t api,
        _In_ uint32_t attr_count,
        _In_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    int index = 0;
    sai_object_id_t dummy_id;
    sai_deserialize_primitive(str_object_id, index, dummy_id);

    sai_hostif_trap_id_t trap_id = (sai_hostif_trap_id_t)dummy_id;

    switch(api)
    {
        case SAI_COMMON_API_SET:
            return sai_hostif_api->set_trap_attribute(trap_id, attr_list);

        case SAI_COMMON_API_GET:
            return sai_hostif_api->get_trap_attribute(trap_id, attr_count, attr_list);

        default:
            SWSS_LOG_ERROR("trap other apis not implemented");
            exit(EXIT_FAILURE);
    }
}

sai_status_t processEvent(swss::ConsumerTable &consumer)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    SWSS_LOG_ENTER();

    swss::KeyOpFieldsValuesTuple kco;
    consumer.pop(kco);

    const std::string &key = kfvKey(kco);
    const std::string &op = kfvOp(kco);

    std::string str_object_type = key.substr(0, key.find(":"));
    std::string str_object_id = key.substr(key.find(":")+1);

    SWSS_LOG_INFO(
            "key: %s op: %s objtype: %s objid: %s",
            key.c_str(),
            op.c_str(),
            str_object_type.c_str(),
            str_object_id.c_str());

    sai_common_api_t api = SAI_COMMON_API_MAX;

    if (op == "create")
        api = SAI_COMMON_API_CREATE;
    else if (op == "remove")
        api = SAI_COMMON_API_REMOVE;
    else if (op == "set")
        api = SAI_COMMON_API_SET;
    else if (op == "get")
        api = SAI_COMMON_API_GET;
    else
    {
        if (op != "delget")
            SWSS_LOG_ERROR("api %s is not implemented", op.c_str());

        return SAI_STATUS_NOT_SUPPORTED;
    }

    std::stringstream ss;

    int index = 0;
    sai_object_type_t object_type;
    sai_deserialize_primitive(str_object_type, index, object_type);

    if (object_type >= SAI_OBJECT_TYPE_MAX)
    {
        SWSS_LOG_ERROR("undefined object type %d", object_type);
        return SAI_STATUS_NOT_SUPPORTED;
    }

    const std::vector<swss::FieldValueTuple> &values = kfvFieldsValues(kco);

    SaiAttributeList list(object_type, values, false);

    if (api != SAI_COMMON_API_GET)
        translate_vid_to_rid_list(object_type, list.get_attr_count(), list.get_attr_list());

    sai_attribute_t *attr_list = list.get_attr_list();
    uint32_t attr_count = list.get_attr_count();

    sai_status_t status;
    switch (object_type)
    {
        case SAI_OBJECT_TYPE_FDB:
            status = handle_fdb(str_object_id, api, attr_count, attr_list);
            break;

        case SAI_OBJECT_TYPE_SWITCH:
            status = handle_switch(str_object_id, api, attr_count, attr_list);
            break;

        case SAI_OBJECT_TYPE_NEIGHBOR:
            status = handle_neighbor(str_object_id, api, attr_count, attr_list);
            break;

        case SAI_OBJECT_TYPE_ROUTE:
            status = handle_route(str_object_id, api, attr_count, attr_list);
            break;

        case SAI_OBJECT_TYPE_VLAN:
            status = handle_vlan(str_object_id, api, attr_count, attr_list);
            break;

        case SAI_OBJECT_TYPE_TRAP:
            status = handle_trap(str_object_id, api, attr_count, attr_list);
            break;

        default:
            status = handle_generic(object_type, str_object_id, api, attr_count, attr_list);
            break;
    }

    if (api == SAI_COMMON_API_GET)
    {
        internal_syncd_get_send(object_type, status, attr_count, attr_list);
    }
    else if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("failed to execute api: %s: %u", op.c_str(), status);

        exit(EXIT_FAILURE);
    }

    return status;
}

void handler(int sig) 
{
    signal(SIGSEGV, SIG_DFL);

    SWSS_LOG_ENTER();

    SWSS_LOG_ERROR("SIGNAL %d", sig);

    void *array[10];
    char **strings;
    size_t size;

    size = backtrace(array, 10);

    SWSS_LOG_ERROR("backtrace() returned %d addresses", size);

    strings = backtrace_symbols(array, size);

    if (strings == NULL) 
    {
        SWSS_LOG_ERROR("backtrace_sumbols() returned NULL");
        exit(EXIT_FAILURE);
    }

    for (size_t j = 0; j < size; j++)
        SWSS_LOG_ERROR("backtrace stack: %s", strings[j]);

    free(strings);

    backtrace_symbols_fd(array, size, STDERR_FILENO);

    exit(EXIT_FAILURE);
}

swss::Logger::Priority redisGetLogLevel()
{
    SWSS_LOG_ENTER();

    auto plevel = g_redisClient->get("LOGLEVEL");

    if (plevel == NULL)
        return swss::Logger::SWSS_NOTICE;

    return swss::Logger::stringToPriority(*plevel);
}

void redisSetLogLevel(swss::Logger::Priority prio)
{
    SWSS_LOG_ENTER();

    std::string level = swss::Logger::priorityToString(prio);

    g_redisClient->set("LOGLEVEL", level);
}

void updateLogLevel()
{
    std::lock_guard<std::mutex> lock(g_mutex);

    auto level = redisGetLogLevel();

    if (level != swss::Logger::getInstance().getMinPrio())
    {
        swss::Logger::getInstance().setMinPrio(level);

        SWSS_LOG_NOTICE("log level changed to %s", swss::Logger::priorityToString(level).c_str());

        // set level to correct one if user set some invalid value
        redisSetLogLevel(level);
    }
}

int main(int argc, char **argv)
{
    swss::Logger::getInstance().setMinPrio(swss::Logger::SWSS_DEBUG);

    SWSS_LOG_ENTER();

    signal(SIGSEGV, handler);

    swss::DBConnector *db = new swss::DBConnector(ASIC_DB, "localhost", 6379, 0);
    swss::DBConnector *dbNtf = new swss::DBConnector(ASIC_DB, "localhost", 6379, 0);

    g_redisClient = new swss::RedisClient(db);

    updateLogLevel();

    swss::ConsumerTable *asicState = new swss::ConsumerTable(db, "ASIC_STATE");

    // at the end we cant use producer consumer concept since
    // if one proces will restart there may be something in the queue
    // also "remove" from response queue will also trigger another "response"
    getRequest = new swss::ConsumerTable(db, "GETREQUEST");
    getResponse  = new swss::ProducerTable(db, "GETRESPONSE");
    notifications = new swss::ProducerTable(dbNtf, "NOTIFICATIONS");

#ifdef MLNXSAI
    std::string mlnx_config_file = "/etc/ssw/ACS-MSN2700/sai_2700.xml";
    gProfileMap[SAI_KEY_INIT_CONFIG_FILE] = mlnx_config_file;
#endif /* MLNX_SAI */

    sai_api_initialize(0, (service_method_table_t*)&test_services);

    populate_sai_apis();

    initialize_common_api_pointers();

#if 1

    sai_status_t status = sai_switch_api->initialize_switch(0, "0xb850", "", &switch_notifications);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("fail to sai_initialize_switch: %d", status);
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < argc; i++)
    {
        if (strcmp(argv[i],"--diag") == 0)
        {
            std::thread bcm_diag_shell_thread = std::thread(sai_diag_shell);
            bcm_diag_shell_thread.detach();
            break;
        }
    }

#else
    sai_switch_api->initialize_switch(
            0,  // profile id
            "dummy_hardware_id",
            "dummy_firmwre_path_name",
            &switch_notifications);
#endif

    SWSS_LOG_INFO("syncd started");

    try
    {
        onSyncdStart();

        SWSS_LOG_INFO("syncd listening for events");

        swss::Select s;

        s.addSelectable(getRequest);
        s.addSelectable(asicState);

        while(true)
        {
            swss::Selectable *sel;

            int fd;

            int result = s.select(&sel, &fd);

            if (result == swss::Select::OBJECT)
            {
                processEvent(*(swss::ConsumerTable*)sel);
            }

            // TODO may be not efficient to do it here
            updateLogLevel();
        }
    }
    catch(const std::exception &e)
    {
        SWSS_LOG_ERROR("Runtime error: %s", e.what());
    }
    catch(...)
    {
        SWSS_LOG_ERROR("Runtime error: unhandled exception");

        handler(SIGSEGV);
    }

    sai_api_uninitialize();
}


