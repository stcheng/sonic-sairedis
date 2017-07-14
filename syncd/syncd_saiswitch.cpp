#include "syncd.h"
#include "syncd_saiswitch.h"
#include "sairedis.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <set>

/*
 * NOTE: all those methods could be implemented inside SaiSwitch class so then
 * we could skip using switch_id in params and even they could be public then.
 */

sai_uint32_t SaiSwitch::saiGetPortCount() const
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;

    attr.id = SAI_SWITCH_ATTR_PORT_NUMBER;

    sai_status_t status = sai_metadata_sai_switch_api->get_switch_attribute(m_switch_rid, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_THROW("failed to get port number: %s",
                sai_serialize_status(status).c_str());
    }

    SWSS_LOG_DEBUG("port count is %u", attr.value.u32);

    return attr.value.u32;
}

void SaiSwitch::saiGetMacAddress(
        _Out_ sai_mac_t &mac) const
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;

    attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;

    sai_status_t status = sai_metadata_sai_switch_api->get_switch_attribute(m_switch_rid, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_THROW("failed to get mac address: %s",
                sai_serialize_status(status).c_str());
    }

    SWSS_LOG_DEBUG("mac address is: %s",
            sai_serialize_mac(attr.value.mac).c_str());

    memcpy(mac, attr.value.mac, sizeof(sai_mac_t));
}

void SaiSwitch::getDefaultMacAddress(
        _Out_ sai_mac_t& mac)
{
    SWSS_LOG_ENTER();

    memcpy(mac, m_default_mac_address, sizeof(sai_mac_t));
}

#define MAX_HARDWARE_INFO_LENGTH 0x1000

std::string SaiSwitch::saiGetHardwareInfo() const
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;

    char info[MAX_HARDWARE_INFO_LENGTH];

    memset(info, 0, MAX_HARDWARE_INFO_LENGTH);

    attr.id = SAI_SWITCH_ATTR_SWITCH_HARDWARE_INFO;

    attr.value.s8list.count = MAX_HARDWARE_INFO_LENGTH;
    attr.value.s8list.list = (int8_t*)info;

    sai_status_t status = sai_metadata_sai_switch_api->get_switch_attribute(m_switch_rid, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        /*
         * TODO: We should have THROW here, but currently getting hardware info
         * is not supported, so we just return empty string like it's not set.
         * Later on basing on this entry we will distinquish whether previous
         * switch and next switch are the same.
         */
        SWSS_LOG_WARN("failed to get switch hardware info: %s",
                sai_serialize_status(status).c_str());
    }

    SWSS_LOG_DEBUG("hardware info: '%s'", info);

    return std::string(info);
}

std::vector<sai_object_id_t> SaiSwitch::saiGetPortList() const
{
    SWSS_LOG_ENTER();

    uint32_t portCount = saiGetPortCount();

    std::vector<sai_object_id_t> portList;

    portList.resize(portCount);

    sai_attribute_t attr;

    attr.id = SAI_SWITCH_ATTR_PORT_LIST;
    attr.value.objlist.count = portCount;
    attr.value.objlist.list = portList.data();

    /*
     * NOTE: We assume port list is always returned in the same order.
     */

    sai_status_t status = sai_metadata_sai_switch_api->get_switch_attribute(m_switch_rid, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_THROW("failed to get port list: %s",
                sai_serialize_status(status).c_str());
    }

    portList.resize(attr.value.objlist.count);

    SWSS_LOG_DEBUG("number of ports: %zu", portList.size());

    return portList;
}

std::unordered_map<sai_uint32_t, sai_object_id_t> SaiSwitch::saiGetHardwareLaneMap() const
{
    SWSS_LOG_ENTER();

    std::unordered_map<sai_uint32_t, sai_object_id_t> map;

    const std::vector<sai_object_id_t> portList = saiGetPortList();

    /*
     * NOTE: Currently we don't support port breakout this will need to be
     * addressed in future.
     */

    const int lanesPerPort = 4;

    for (const auto &port_rid : portList)
    {
        sai_uint32_t lanes[lanesPerPort];

        memset(lanes, 0, sizeof(lanes));

        sai_attribute_t attr;

        attr.id = SAI_PORT_ATTR_HW_LANE_LIST;
        attr.value.u32list.count = lanesPerPort;
        attr.value.u32list.list = lanes;

        sai_status_t status = sai_metadata_sai_port_api->get_port_attribute(port_rid, 1, &attr);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_THROW("failed to get hardware lane list port RID %s: %s",
                    sai_serialize_object_id(port_rid).c_str(),
                    sai_serialize_status(status).c_str());
        }

        sai_int32_t laneCount = attr.value.u32list.count;

        for (int j = 0; j < laneCount; j++)
        {
            map[lanes[j]] = port_rid;
        }
    }

    return map;
}

std::string SaiSwitch::getRedisLanesKey() const
{
    SWSS_LOG_ENTER();

    /*
     * Each switch will have it's own lanes in format LANES:oid:0xYYYYYYYY.
     *
     * NOTE: To support multple switches LANES needs to be made per switch.
     *
     * return std::string(LANES) + ":" + sai_serialize_object_id(m_switch_vid);
     */

    return std::string(LANES);
}

void SaiSwitch::redisClearLaneMap() const
{
    SWSS_LOG_ENTER();

    auto key = getRedisLanesKey();

    g_redisClient->del(key);
}

std::unordered_map<sai_uint32_t, sai_object_id_t> SaiSwitch::redisGetLaneMap() const
{
    SWSS_LOG_ENTER();

    auto key = getRedisLanesKey();

    auto hash = g_redisClient->hgetall(key);

    SWSS_LOG_DEBUG("previous lanes: %lu", hash.size());

    std::unordered_map<sai_uint32_t, sai_object_id_t> map;

    for (auto &kv: hash)
    {
        const std::string &str_key = kv.first;
        const std::string &str_value = kv.second;

        sai_uint32_t lane;
        sai_object_id_t portId;

        sai_deserialize_number(str_key, lane);

        sai_deserialize_object_id(str_value, portId);

        map[lane] = portId;
    }

    return map;
}

void SaiSwitch::redisSaveLaneMap(
        _In_ const std::unordered_map<sai_uint32_t, sai_object_id_t> &map) const
{
    SWSS_LOG_ENTER();

    redisClearLaneMap();

    for (auto const &it: map)
    {
        sai_uint32_t lane = it.first;
        sai_object_id_t portId = it.second;

        std::string strLane = sai_serialize_number(lane);
        std::string strPortId = sai_serialize_object_id(portId);

        auto key = getRedisLanesKey();

        g_redisClient->hset(key, strLane, strPortId);
    }
}

std::unordered_map<sai_object_id_t, sai_object_id_t> SaiSwitch::redisGetObjectMap(
        _In_ const std::string &key)
{
    SWSS_LOG_ENTER();

    auto hash = g_redisClient->hgetall(key);

    std::unordered_map<sai_object_id_t, sai_object_id_t> map;

    for (auto &kv: hash)
    {
        const std::string &str_key = kv.first;
        const std::string &str_value = kv.second;

        sai_object_id_t objectIdKey;
        sai_object_id_t objectIdValue;

        sai_deserialize_object_id(str_key, objectIdKey);

        sai_deserialize_object_id(str_value, objectIdValue);

        map[objectIdKey] = objectIdValue;
    }

    return map;
}

std::unordered_map<sai_object_id_t, sai_object_id_t> SaiSwitch::redisGetVidToRidMap() const
{
    SWSS_LOG_ENTER();

    /*
     * NOTE: To support multiple switches VIDTORID must be per switch.
     */

    return redisGetObjectMap(VIDTORID);
}

std::unordered_map<sai_object_id_t, sai_object_id_t> SaiSwitch::redisGetRidToVidMap() const
{
    SWSS_LOG_ENTER();

    /*
     * NOTE: To support multiple switches RIDTOVID must be per switch.
     */

    return redisGetObjectMap(RIDTOVID);
}

void SaiSwitch::helperCheckLaneMap()
{
    SWSS_LOG_ENTER();

    auto redisLaneMap = redisGetLaneMap();

    auto laneMap = saiGetHardwareLaneMap();

    if (redisLaneMap.size() == 0)
    {
        SWSS_LOG_INFO("no lanes defined in redis, seems like it is first syncd start");

        redisSaveLaneMap(laneMap);

        redisLaneMap = laneMap; // copy
    }

    if (laneMap.size() != redisLaneMap.size())
    {
        SWSS_LOG_THROW("lanes map size differ: %lu vs %lu", laneMap.size(), redisLaneMap.size());
    }

    /*
     * We assume there are at least 32 ports and port number
     * can be divided by 16 without rest.
     */

    if (laneMap.size() < 32 || (laneMap.size() % 16 != 0))
    {
        // TODO throw here
        SWSS_LOG_ERROR("LANES size is %zu, something is wrong!", laneMap.size());
    }

    for (auto kv: laneMap)
    {
        sai_uint32_t lane = kv.first;
        sai_object_id_t portId = kv.second;

        if (redisLaneMap.find(lane) == redisLaneMap.end())
        {
            SWSS_LOG_THROW("lane %u not found in redis", lane);
        }

        if (redisLaneMap[lane] != portId)
        {
            // if this happens, we need to remap VIDTORID and RIDTOVID
            SWSS_LOG_THROW("FIXME: lane port id differs: %s vs %s, port ids must be remapped",
                    sai_serialize_object_id(portId).c_str(),
                    sai_serialize_object_id(redisLaneMap[lane]).c_str());
        }
    }
}

std::string SaiSwitch::getRedisHiddenKey() const
{
    SWSS_LOG_ENTER();

    /*
     * Each switch will have it's own hidden in format HIDDEN:oid:0xYYYYYYYY.
     *
     * NOTE: To support multiple switches HIDDEN needs to me made per switch.
     *
     * return std::string(HIDDEN) + ":" + sai_serialize_object_id(m_switch_vid);
     */

    return std::string(HIDDEN);
}

void SaiSwitch::redisSetDummyAsicStateForRealObjectId(
        _In_ sai_object_id_t rid) const
{
    SWSS_LOG_ENTER();

    sai_object_id_t vid = translate_rid_to_vid(rid, m_switch_vid);

    sai_object_type_t objectType = sai_object_type_query(rid);

    if (objectType == SAI_OBJECT_TYPE_NULL)
    {
        SWSS_LOG_THROW("sai_object_type_query returned NULL type for RID: %s",
                sai_serialize_object_id(rid).c_str());
    }

    std::string strObjectType = sai_serialize_object_type(objectType);

    std::string strVid = sai_serialize_object_id(vid);

    std::string strKey = ASIC_STATE_TABLE + (":" + strObjectType + ":" + strVid);

    g_redisClient->hset(strKey, "NULL", "NULL");
}

sai_object_id_t SaiSwitch::getVid() const
{
    SWSS_LOG_ENTER();

    return m_switch_vid;
}

sai_object_id_t SaiSwitch::getRid() const
{
    SWSS_LOG_ENTER();

    return m_switch_rid;
}

std::string SaiSwitch::getHardwareInfo() const
{
    SWSS_LOG_ENTER();

    /*
     * Since this attribute is CREATE_ONLY this value will not change in entire
     * life of switch.
     */

    return m_hardware_info;
}

bool SaiSwitch::isDefaultCreatedRid(
        _In_ sai_object_id_t rid) const
{
    SWSS_LOG_ENTER();

    return m_discovered_rids.find(rid) != m_discovered_rids.end();
}

std::set<sai_object_id_t> SaiSwitch::getExistingObjects() const
{
    SWSS_LOG_ENTER();

    return m_discovered_rids;
}

void SaiSwitch::removeExistingObjectReference(
        _In_ sai_object_id_t rid)
{
    SWSS_LOG_ENTER();

    auto it = m_discovered_rids.find(rid);

    if (it == m_discovered_rids.end())
    {
        SWSS_LOG_THROW("unable to find existing RID %s",
                sai_serialize_object_id(rid).c_str());
    }

    SWSS_LOG_INFO("removing ref RID %s",
            sai_serialize_object_id(rid).c_str());

    m_discovered_rids.erase(it);
}

void SaiSwitch::removeExistingObject(
        _In_ sai_object_id_t rid)
{
    SWSS_LOG_ENTER();

    auto it = m_discovered_rids.find(rid);

    if (it == m_discovered_rids.end())
    {
        SWSS_LOG_THROW("unable to find existing RID %s",
                sai_serialize_object_id(rid).c_str());
    }

    sai_object_type_t ot = sai_object_type_query(rid);

    if (ot == SAI_OBJECT_TYPE_NULL)
    {
        SWSS_LOG_THROW("sai_object_type_query returned NULL on RID %s",
                sai_serialize_object_id(rid).c_str());
    }

    auto info = sai_metadata_get_object_type_info(ot);

    sai_object_meta_key_t meta_key = { .objecttype = ot, .objectkey = {.key = { .object_id = rid } } };

    SWSS_LOG_INFO("removing %s", sai_serialize_object_meta_key(meta_key).c_str());

    sai_status_t status = info->remove(&meta_key);

    if (status == SAI_STATUS_SUCCESS)
    {
        m_discovered_rids.erase(it);
    }
    else
    {
        SWSS_LOG_ERROR("failed to remove %s RID %s: %s",
                info->objecttypename,
                sai_serialize_object_id(rid).c_str(),
                sai_serialize_status(status).c_str());
    }
}

std::vector<sai_port_stat_t> SaiSwitch::saiGetSupportedCounters() const
{
    SWSS_LOG_ENTER();

    auto ports = saiGetPortList();

    if (ports.size() == 0)
    {
        SWSS_LOG_THROW("no ports are defined on switch");
    }

    sai_object_id_t port_rid = ports.at(0);

    std::vector<sai_port_stat_t> supportedCounters;

    for (uint32_t idx = 0; idx < sai_metadata_enum_sai_port_stat_t.valuescount; ++idx)
    {
        sai_port_stat_t counter = (sai_port_stat_t)sai_metadata_enum_sai_port_stat_t.values[idx];

        uint64_t value;

        sai_status_t status = sai_metadata_sai_port_api->get_port_stats(port_rid, 1, &counter, &value);

        if (status != SAI_STATUS_SUCCESS)
        {
            const std::string &name = sai_serialize_port_stat(counter);

            SWSS_LOG_DEBUG("counter %s is not supported on port RID %s: %s",
                    name.c_str(),
                    sai_serialize_object_id(port_rid).c_str(),
                    sai_serialize_status(status).c_str());

            continue;
        }

        supportedCounters.push_back(counter);
    }

    SWSS_LOG_NOTICE("supported %zu of %d",
            supportedCounters.size(),
            sai_metadata_enum_sai_port_stat_t.valuescount);

    return supportedCounters;
}

void SaiSwitch::collectCounters(
        _In_ swss::Table &countersTable) const
{
    SWSS_LOG_ENTER();

    if (m_supported_counters.size() == 0)
    {
        /*
         * There are not supported counters :(
         */

        return;
    }

    uint32_t countersSize = (uint32_t)m_supported_counters.size();

    std::vector<uint64_t> counters;

    counters.resize(countersSize);

    auto ports = saiGetPortList();

    for (auto &port_rid: ports)
    {
        sai_status_t status = sai_metadata_sai_port_api->get_port_stats(
                port_rid,
                countersSize,
                m_supported_counters.data(),
                counters.data());

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("failed to collect counters for port RID %s: %s",
                    sai_serialize_object_id(port_rid).c_str(),
                    sai_serialize_status(status).c_str());
            continue;
        }

        sai_object_id_t vid = translate_rid_to_vid(port_rid, m_switch_vid);

        std::string strPortId = sai_serialize_object_id(vid);

        // for counters, use port vid as printf "%llx" format
        std::stringstream ss;
        ss << std::hex << vid;
        strPortId = ss.str();

        std::vector<swss::FieldValueTuple> values;

        for (size_t idx = 0; idx < counters.size(); idx++)
        {
            const std::string &field = sai_serialize_port_stat(m_supported_counters[idx]);
            const std::string &value = std::to_string(counters[idx]);

            swss::FieldValueTuple fvt(field, value);

            values.push_back(fvt);
        }

        countersTable.set(strPortId, values, "");
    }
}

/**
 * @brief Helper function to get attribute oid from switch.
 *
 * Helper will try to obtain oid value for given attribute id.  On success, it
 * will try to obtain this value from redis database.  When value is not in
 * redis yet, it will store it, but when value was already there, it will
 * compare redis value to current oid and when they are different, it will
 * throw exception requesting for fix. When oid values are equal, function
 * returns current value.
 *
 * @param attr_id Attribute id to obtain oid from it.
 *
 * @return Valid object id (riD) if present, SAI_NULL_OBJECT_ID on failure.
 */
sai_object_id_t SaiSwitch::helperGetSwitchAttrOid(
        _In_ sai_attr_id_t attr_id)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;

    /*
     * Get original value from the ASIC.
     */

    auto meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_SWITCH, attr_id);

    if (meta == NULL)
    {
        SWSS_LOG_THROW("can't get switch attribute %d metadata", attr_id);
    }

    if (meta->attrvaluetype != SAI_ATTR_VALUE_TYPE_OBJECT_ID)
    {
        SWSS_LOG_THROW("atribute %s is not OID attribute", meta->attridname);
    }

    attr.id = attr_id;

    sai_status_t status = sai_metadata_sai_switch_api->get_switch_attribute(m_switch_rid, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("failed to get %s: %s",
                meta->attridname,
                sai_serialize_status(status).c_str());

        return SAI_NULL_OBJECT_ID;
    }

    SWSS_LOG_INFO("%s RID %s",
            meta->attridname,
            sai_serialize_object_id(attr.value.oid).c_str());

    sai_object_id_t rid = attr.value.oid;

    sai_object_id_t redis_rid = SAI_NULL_OBJECT_ID;

    if (rid == SAI_NULL_OBJECT_ID)
    {
        return rid;
    }

    /*
     * Get value value of the same attribute from redis.
     */

    auto key = getRedisHiddenKey();

    auto ptr_redis_rid_str = g_redisClient->hget(key, meta->attridname);

    if (ptr_redis_rid_str == NULL)
    {
        /*
         * Redis value of this attribute is not present yet, save it!
         */

        redisSetDummyAsicStateForRealObjectId(rid);

        SWSS_LOG_INFO("redis %s id is not defined yet in redis", meta->attridname);

        std::string rid_str = sai_serialize_object_id(rid);

        g_redisClient->hset(key, meta->attridname, rid_str);

        m_default_rid_map[attr_id] = rid;

        return rid;
    }

    sai_deserialize_object_id(*ptr_redis_rid_str, redis_rid);

    if (rid != redis_rid)
    {
        /*
         * In that case we need to remap VIDTORID and RIDTOVID. This is
         * required since if previous value will be used in redis maps, and it
         * will be invalid when that value will be used to call SAI api.
         */

        SWSS_LOG_THROW("FIXME: %s RID differs: %s (asic) vs %s (redis), ids must be remapped v2r/r2v",
                meta->attridname,
                sai_serialize_object_id(rid).c_str(),
                sai_serialize_object_id(redis_rid).c_str());
    }

    m_default_rid_map[attr_id] = rid;

    return rid;
}

sai_object_id_t SaiSwitch::getSwitchDefaultAttrOid(
        _In_ sai_attr_id_t attr_id) const
{
    SWSS_LOG_ENTER();

    auto it = m_default_rid_map.find(attr_id);

    if (it == m_default_rid_map.end())
    {
        auto meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_SWITCH, attr_id);

        const char* name = (meta) ? meta->attridname : "UNKNOWN";

        SWSS_LOG_THROW("attribute %s (%d) not found in default RID map", name, attr_id);
    }

    return it->second;
}

bool SaiSwitch::isNonRemovableRid(
        _In_ sai_object_id_t rid) const
{
    SWSS_LOG_ENTER();

    if (rid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_THROW("NULL rid passed");
    }

    if (!isDefaultCreatedRid(rid))
    {
        /*
         * Non discovered obejct, it can be removed.
         */

        return false;
    }

    for (const auto &p: m_default_rid_map)
    {
        if (p.second == rid)
        {
            return true;
        }
    }

    sai_object_type_t ot = sai_object_type_query(rid);

    /*
     * List of objects after init (mlnx 2700):
     *
     * PORT: 33                     // prevent
     * VIRTUAL_ROUTER: 1            // default
     * STP: 1                       // default
     * HOSTIF_TRAP_GROUP: 1         // default
     * QUEUE: 528                   // prevent
     * SCHEDULER_GROUP: 512         // prevent
     * INGRESS_PRIORITY_GROUP: 256  // prevent
     * HASH: 2                      // prevent
     * SWITCH: 1                    // prevent
     * VLAN: 1                      // default
     * VLAN_MEMBER: 32              // can be removed
     * STP_PORT: 32                 // can be removed (cpu don't belong to stp)
     * BRIDGE: 1                    // default
     * BRIDGE_PORT: 33              // can be removed but cpu bridge port can't
     */

    switch (ot)
    {
        case SAI_OBJECT_TYPE_VLAN_MEMBER:
        case SAI_OBJECT_TYPE_STP_PORT:
        case SAI_OBJECT_TYPE_BRIDGE_PORT:
            return false;

        case SAI_OBJECT_TYPE_PORT:
        case SAI_OBJECT_TYPE_QUEUE:
        case SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP:
        case SAI_OBJECT_TYPE_SCHEDULER_GROUP:
        case SAI_OBJECT_TYPE_HASH:

            /*
             * TODO: Some vendors support removing of those objects then we
             * need to came up with different approach. Probably SaiSwitch
             * will need to decide whether it's possible to remove object.
             */

            return true;

        default:
            break;
    }

    SWSS_LOG_WARN("can't determine wheter object %s RID %s can be removed, FIXME",
            sai_serialize_object_type(ot).c_str(),
            sai_serialize_object_id(rid).c_str());

    return true;
}

void SaiSwitch::saiDiscover(
        _In_ sai_object_id_t rid,
        _Inout_ std::set<sai_object_id_t> &discovered)
{
    SWSS_LOG_ENTER();

    /*
     * NOTE: This method is only good after switch init since we are making
     * assumptions that tere are no ACL after initialization.
     *
     * NOTE: Input set could be a map of sets, this way we will also have
     * dependency on each oid.
     */

    if (rid == SAI_NULL_OBJECT_ID)
    {
        return;
    }

    if (discovered.find(rid) != discovered.end())
    {
        return;
    }

    sai_object_type_t ot = sai_object_type_query(rid);

    if (ot == SAI_OBJECT_TYPE_NULL)
    {
        SWSS_LOG_THROW("sai_object_type_query: rid %s returned NULL object type",
                sai_serialize_object_id(rid).c_str());
    }

    SWSS_LOG_DEBUG("processing %s: %s",
            sai_serialize_object_id(rid).c_str(),
            sai_serialize_object_type(ot).c_str());

    /*
     * We will ignore STP ports by now, since when removing bridge port, then
     * associated stp port is automatically removed, and we don't use STP in
     * out solution.  This causing inconsestincy with redis ASIC view vs
     * actual ASIC asic state.
     *
     * TODO: This needs to be solved by sending discovered state to sairedis
     * metadata db for reference count.
     *
     * XXX: workaround
     */

    if (ot != SAI_OBJECT_TYPE_STP_PORT)
    {
        discovered.insert(rid);
    }

    // TODO later use sai_metadata_get_object_type_info(ot);
    const sai_object_type_info_t *info =  sai_metadata_get_object_type_info(ot);

    /*
     * We will query only oid object types
     * then we don't need meta key, but we need to add to metadata
     * pointers to only generic functions.
     */

    sai_object_meta_key_t mk = { .objecttype = ot, .objectkey = { .key = { .object_id = rid } } };

    for (int idx = 0; info->attrmetadata[idx] != NULL; ++idx)
    {
        const sai_attr_metadata_t *md = info->attrmetadata[idx];

        /*
         * Note that we don't care about ACL object id's since
         * we assume that there are no ACLs on switch after init.
         */

        sai_attribute_t attr;

        attr.id = md->attrid;

        if (md->attrvaluetype == SAI_ATTR_VALUE_TYPE_OBJECT_ID)
        {
            if (md->defaultvaluetype == SAI_DEFAULT_VALUE_TYPE_CONST)
            {
                /*
                 * This means that default value for this object is
                 * SAI_NULL_OBJECT_ID, since this is discovery after
                 * create, we don't need to query this attribute.
                 */

                //continue;
            }

            if (md->objecttype == SAI_OBJECT_TYPE_STP &&
                    md->attrid == SAI_STP_ATTR_BRIDGE_ID)
            {
                // XXX workaround (for mlnx)
                SWSS_LOG_WARN("skipping since it causes crash: %s", md->attridname);
                continue;
            }

            if (md->objecttype == SAI_OBJECT_TYPE_BRIDGE_PORT)
            {
                if (md->attrid == SAI_BRIDGE_PORT_ATTR_TUNNEL_ID ||
                        md->attrid == SAI_BRIDGE_PORT_ATTR_RIF_ID)
                {
                    /*
                     * We know that bridge port is binded on PORT, no need
                     * to query those attributes.
                     */

                    continue;
                }
            }

            SWSS_LOG_DEBUG("getting %s for %s", md->attridname,
                    sai_serialize_object_id(rid).c_str());

            sai_status_t status = info->get(&mk, 1, &attr);

            if (status != SAI_STATUS_SUCCESS)
            {
                /*
                 * We failed to get value, maybe it's not supported ?
                 */

                SWSS_LOG_INFO("%s: %s on %s",
                        md->attridname,
                        sai_serialize_status(status).c_str(),
                        sai_serialize_object_id(rid).c_str());

                continue;
            }

            m_defaultOidMap[rid][attr.id] = attr.value.oid;

            if (!md->allownullobjectid && attr.value.oid == SAI_NULL_OBJECT_ID)
            {
                // SWSS_LOG_WARN("got null on %s, but not allowed", md->attridname);
            }

            if (attr.value.oid != SAI_NULL_OBJECT_ID)
            {
                ot = sai_object_type_query(attr.value.oid);

                if (ot == SAI_OBJECT_TYPE_NULL)
                {
                    SWSS_LOG_THROW("when query %s (on %s RID %s) got value %s sai_object_type_query returned NULL object type",
                            md->attridname,
                            sai_serialize_object_type(md->objecttype).c_str(),
                            sai_serialize_object_id(rid).c_str(),
                            sai_serialize_object_id(attr.value.oid).c_str());
                }
            }

            saiDiscover(attr.value.oid, discovered); // recursion
        }
        else if (md->attrvaluetype == SAI_ATTR_VALUE_TYPE_OBJECT_LIST)
        {
            if (md->defaultvaluetype == SAI_DEFAULT_VALUE_TYPE_EMPTY_LIST)
            {
                /*
                 * This means that default value for this object is
                 * empty list, since this is discovery after
                 * create, we don't need to query this attribute.
                 */

                //continue;
            }

            SWSS_LOG_DEBUG("getting %s for %s", md->attridname,
                    sai_serialize_object_id(rid).c_str());

            sai_object_id_t local[SAI_DISCOVERY_LIST_MAX_ELEMENTS];

            attr.value.objlist.count = SAI_DISCOVERY_LIST_MAX_ELEMENTS;
            attr.value.objlist.list = local;

            sai_status_t status = info->get(&mk, 1, &attr);

            if (status != SAI_STATUS_SUCCESS)
            {
                /*
                 * We failed to get value, maybe it's not supported ?
                 */

                SWSS_LOG_INFO("%s: %s on %s",
                        md->attridname,
                        sai_serialize_status(status).c_str(),
                        sai_serialize_object_id(rid).c_str());

                continue;
            }

            SWSS_LOG_DEBUG("list count %s %u", md->attridname, attr.value.objlist.count);

            for (uint32_t i = 0; i < attr.value.objlist.count; ++i)
            {
                sai_object_id_t oid = attr.value.objlist.list[i];

                ot = sai_object_type_query(oid);

                if (ot == SAI_OBJECT_TYPE_NULL)
                {
                    SWSS_LOG_THROW("when query %s (on %s RID %s) got value %s sai_object_type_query returned NULL object type",
                            md->attridname,
                            sai_serialize_object_type(md->objecttype).c_str(),
                            sai_serialize_object_id(rid).c_str(),
                            sai_serialize_object_id(oid).c_str());
                }

                saiDiscover(oid, discovered); // recursion
            }
        }
    }
}

void SaiSwitch::helperDiscover()
{
    SWSS_LOG_ENTER();

    /*
     * Preform discovery on the switch to obtain ASIC view of
     * objects that are created internally.
     */

    m_discovered_rids.clear();

    {
        SWSS_LOG_TIMER("discover");

        saiDiscover(m_switch_rid, m_discovered_rids);
    }

    SWSS_LOG_NOTICE("discovered objects count: %zu", m_discovered_rids.size());

    std::map<sai_object_type_t,int> map;

    for (sai_object_id_t rid: m_discovered_rids)
    {
        /*
         * We don't need to check for null since saiDiscovery already checked
         * that.
         */

        map[sai_object_type_query(rid)]++;
    }

    for (const auto &p: map)
    {
        SWSS_LOG_NOTICE("%s: %d", sai_serialize_object_type(p.first).c_str(), p.second);
    }
}

void SaiSwitch::helperPutDiscoveredRidsToRedis()
{
    SWSS_LOG_ENTER();

    SWSS_LOG_TIMER("put discovered objects to redis");

    /*
     * There is a problem:
     *
     * After switch creation, on the switch objects are created internally like
     * VLAN members, queys, SGs etc.  Some of those obejct's are removable.
     * User can decide that he don't want VLAN members and he will remove them.
     * Those obejcts will be removed from ASIC view in redis as well.
     *
     * Now after hard reinit, syncd will picku up what is in the db and it will
     * try to recreate ASIC state.  First it will create switch, and this
     * switch will create those VLAN members again inside ASIC and it will try
     * to put them back to the DB, since we need to keep track of all default
     * objects.
     *
     * We need a way to decide whether we need to put those objects to DB or
     * not. Since we are performing syncd hard reinit and recreating switch
     * that there was something in the DB already. And basing on that we can
     * deduce that we don't need to put again all our discovered objects to the
     * DB, since some of thsoe obects could be removed as statet at the
     * beginning.
     *
     * Hard reinit is performed before taking any action from the redis queue.
     * But when user will decide to create switch, table consumer will put that
     * switch to the DB right away before calling SaiSwitch constructor.  But
     * that switch will be the only object in the ASIC table, so out simple
     * deduction here could be checking if there is more than object in the db.
     * If there are at least 2, then we don't need to save discovered oids.
     *
     * We will use at least number 32, since that should be at least number of
     * ports that previously discovered. We could also query for PORTs.  We can
     * also deduce this using HIDDEN key objects, if they are defined then that
     * would mean we already put objects to db at the first place.
     *
     * PS. This is not the best way to solve this problem, but works.
     *
     * TODO: Some of those objects could be removed, like vlan members etc, we
     * could actually put those objects back, but only those obejcts which we
     * would consider non removable, and this is hard to determine now. A
     * method getNonRemovableObjects would be nice, then we could put those
     * objects to the view every time, and not put only discovered objects that
     * reflect removable obejcts liek vlan member.
     */

    auto keys = g_redisClient->keys(ASIC_STATE_TABLE ":*");

    const int ObjectsTreshold = 32;

    bool manyObjectsPresent = keys.size() > ObjectsTreshold;

    SWSS_LOG_NOTICE("objects in ASIC state table present: %zu", keys.size());

    if (manyObjectsPresent)
    {
        SWSS_LOG_NOTICE("will NOT put discovered objects into db");

        // TODO or just check here and remove directly form ASIC_VIEW ?
        return;
    }

    SWSS_LOG_NOTICE("putting ALL discovered objects to redis");

    for (sai_object_id_t rid: m_discovered_rids)
    {
        /*
         * We also could thing of optimizing this since it's one call to redis
         * per rid, and probably this should be ATOMIC.
         *
         * NOTE: We are also storing read only object's here, like default
         * virtual router, CPU, default trap group, etc.
         */

        redisSetDummyAsicStateForRealObjectId(rid);
    }
}

void SaiSwitch::helperInternalOids()
{
    SWSS_LOG_ENTER();

    auto info = sai_metadata_get_object_type_info(SAI_OBJECT_TYPE_SWITCH);

    for (int idx = 0; info->attrmetadata[idx] != NULL; ++idx)
    {
        const sai_attr_metadata_t *md = info->attrmetadata[idx];

        if (md->attrvaluetype == SAI_ATTR_VALUE_TYPE_OBJECT_ID &&
                md->defaultvaluetype == SAI_DEFAULT_VALUE_TYPE_SWITCH_INTERNAL)
        {
            helperGetSwitchAttrOid(md->attrid);
        }
    }
}

sai_object_id_t SaiSwitch::getDefaultValueForOidAttr(
        _In_ sai_object_id_t rid,
        _In_ sai_attr_id_t attr_id)
{
    SWSS_LOG_ENTER();

    auto it = m_defaultOidMap.find(rid);

    if (it == m_defaultOidMap.end())
    {
        return SAI_NULL_OBJECT_ID;
    }

    auto ita = it->second.find(attr_id);

    if (ita == it->second.end())
    {
        return SAI_NULL_OBJECT_ID;
    }

    return ita->second;
}

/*
 * NOTE: If real ID will change during hard restarts, then we need to remap all
 * VID/RID, but we can only do that if we will save entire tree with all
 * dependencises.
 */

SaiSwitch::SaiSwitch(
        _In_ sai_object_id_t switch_vid,
        _In_ sai_object_id_t switch_rid)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_TIMER("constructor");

    m_switch_rid = switch_rid;
    m_switch_vid = switch_vid;

    m_hardware_info = saiGetHardwareInfo();

    /*
     * Discover put objects to redis needs to be called before checking lane
     * map and ports, since it will deduce whether put discoverd objects to
     * redis to not interfere with possible user created objects previously.
     *
     * TODO: When user will use sairedis we need to send discovered view
     * with all objects dependencies to sairedis so metadata db could
     * be populated, and all references could be increased.
     */

    helperDiscover();

    helperPutDiscoveredRidsToRedis();

    helperInternalOids();

    helperCheckLaneMap();

    m_supported_counters = saiGetSupportedCounters();

    saiGetMacAddress(m_default_mac_address);
}
