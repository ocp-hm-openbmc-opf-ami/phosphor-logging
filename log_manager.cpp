#include "config.h"

#include "log_manager.hpp"

#include "elog_entry.hpp"
#include "elog_meta.hpp"
#include "elog_serialize.hpp"
#include "extensions.hpp"
#include "lib/lg2_commit.hpp"
#include "paths.hpp"
#include "util.hpp"

#include <sys/stat.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-journal.h>
#include <unistd.h>

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/vtable.hpp>
#include <xyz/openbmc_project/State/Host/server.hpp>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace std::chrono;
extern const std::map<
    phosphor::logging::metadata::Metadata,
    std::function<phosphor::logging::metadata::associations::Type>>
    meta;
static constexpr auto mapperBusName = "xyz.openbmc_project.ObjectMapper";
static constexpr auto mapperObjPath = "/xyz/openbmc_project/object_mapper";
static constexpr auto mapperIntf = "xyz.openbmc_project.ObjectMapper";
constexpr auto dbusProperty = "org.freedesktop.DBus.Properties";
constexpr auto policyInterface = "xyz.openbmc_project.Logging.Settings";
constexpr auto policyLinear =
    "xyz.openbmc_project.Logging.Settings.Policy.Linear";
constexpr auto policyDefault =
    "xyz.openbmc_project.Logging.Settings.Policy.Circular";

/*D-bus details for BMC Global enable */
static constexpr const char* settingService = "xyz.openbmc_project.Settings";
static constexpr const char* globalEnblObjpath =
    "/xyz/openbmc_project/control/globalenables";
static constexpr const char* globalEnblInterface =
    "xyz.openbmc_project.Control.BMC.Globalenables";
using DBusInterface = std::string;
using DBusService = std::string;
using DBusPath = std::string;
using DBusInterfaceList = std::vector<DBusInterface>;
using DBusSubTree =
    std::map<DBusPath, std::map<DBusService, DBusInterfaceList>>;

namespace phosphor
{
namespace logging
{
namespace internal
{
inline auto getLevel(const std::string& errMsg)
{
    auto reqLevel = Entry::Level::Error; // Default to Error

    auto levelmap = g_errLevelMap.find(errMsg);
    if (levelmap != g_errLevelMap.end())
    {
        reqLevel = static_cast<Entry::Level>(levelmap->second);
    }

    return reqLevel;
}

int Manager::getRealErrSize(LogType logType)
{
    return realErrorsMap[logType].size();
}

int Manager::getInfoErrSize(LogType logType)
{
    return infoErrorsMap[logType].size();
}

uint32_t Manager::commit(uint64_t transactionId, std::string errMsg)
{
    auto level = getLevel(errMsg);
    _commit(transactionId, std::move(errMsg), level);
    return entryId;
}

uint32_t Manager::commitWithLvl(uint64_t transactionId, std::string errMsg,
                                uint32_t errLvl)
{
    _commit(transactionId, std::move(errMsg),
            static_cast<Entry::Level>(errLvl));
    return entryId;
}

void Manager::_commit(uint64_t transactionId [[maybe_unused]],
                      std::string&& errMsg, Entry::Level errLvl)
{
    std::map<std::string, std::string> additionalData{};

    // When running as a test-case, the system may have a LOT of journal
    // data and we may not have permissions to do some of the journal sync
    // operations.  Just skip over them.
    if (!IS_UNIT_TEST)
    {
        static constexpr auto transactionIdVar =
            std::string_view{"TRANSACTION_ID"};
        // Length of 'TRANSACTION_ID' string.
        static constexpr auto transactionIdVarSize = transactionIdVar.size();
        // Length of 'TRANSACTION_ID=' string.
        static constexpr auto transactionIdVarOffset = transactionIdVarSize + 1;

        // Flush all the pending log messages into the journal
        util::journalSync();

        sd_journal* j = nullptr;
        int rc = sd_journal_open(&j, SD_JOURNAL_LOCAL_ONLY);
        if (rc < 0)
        {
            lg2::error("Failed to open journal: {ERROR}", "ERROR",
                       strerror(-rc));
            return;
        }

        std::string transactionIdStr = std::to_string(transactionId);
        std::set<std::string> metalist;
        auto metamap = g_errMetaMap.find(errMsg);
        if (metamap != g_errMetaMap.end())
        {
            metalist.insert(metamap->second.begin(), metamap->second.end());
        }

        // Add _PID field information in AdditionalData.
        metalist.insert("_PID");

        // Read the journal from the end to get the most recent entry first.
        // The result from the sd_journal_get_data() is of the form
        // VARIABLE=value.
        SD_JOURNAL_FOREACH_BACKWARDS(j)
        {
            const char* data = nullptr;
            size_t length = 0;

            // Look for the transaction id metadata variable
            rc = sd_journal_get_data(j, transactionIdVar.data(),
                                     (const void**)&data, &length);
            if (rc < 0)
            {
                // This journal entry does not have the TRANSACTION_ID
                // metadata variable.
                continue;
            }

            // journald does not guarantee that sd_journal_get_data() returns
            // NULL terminated strings, so need to specify the size to use to
            // compare, use the returned length instead of anything that relies
            // on NULL terminators like strlen(). The data variable is in the
            // form of 'TRANSACTION_ID=1234'. Remove the TRANSACTION_ID
            // characters plus the (=) sign to do the comparison. 'data +
            // transactionIdVarOffset' will be in the form of '1234'. 'length -
            // transactionIdVarOffset' will be the length of '1234'.
            if ((length <= (transactionIdVarOffset)) ||
                (transactionIdStr.compare(
                     0, transactionIdStr.size(), data + transactionIdVarOffset,
                     length - transactionIdVarOffset) != 0))
            {
                // The value of the TRANSACTION_ID metadata is not the requested
                // transaction id number.
                continue;
            }

            // Search for all metadata variables in the current journal entry.
            for (auto i = metalist.cbegin(); i != metalist.cend();)
            {
                rc = sd_journal_get_data(j, (*i).c_str(), (const void**)&data,
                                         &length);
                if (rc < 0)
                {
                    // Metadata variable not found, check next metadata
                    // variable.
                    i++;
                    continue;
                }

                // Metadata variable found, save it and remove it from the set.
                std::string metadata(data, length);
                /* if (auto pos = metadata.find('='); pos != std::string::npos)
                 {
                     auto key = metadata.substr(0, pos);
                     auto value = metadata.substr(pos + 1);
                     additionalData.emplace(std::move(key), std::move(value));
                 }*/
                i = metalist.erase(i);
            }
            if (metalist.empty())
            {
                // All metadata variables found, break out of journal loop.
                break;
            }
        }
        if (!metalist.empty())
        {
            // Not all the metadata variables were found in the journal.
            for (auto& metaVarStr : metalist)
            {
                lg2::info("Failed to find metadata: {META_FIELD}", "META_FIELD",
                          metaVarStr);
            }
        }

        sd_journal_close(j);
    }
    createEntry(errMsg, errLvl, additionalData);
}

std::string Manager::getSelPolicy()
{
    DBusSubTree subtree;

    auto method = this->busLog.new_method_call(mapperBusName, mapperObjPath,
                                               mapperIntf, "GetSubTree");
    method.append(std::string{"/"}, 0,
                  std::vector<std::string>{policyInterface});
    auto reply = this->busLog.call(method);
    reply.read(subtree);

    if (subtree.empty())
    {
        lg2::info("Compatible interface not on D-Bus. Continuing with default "
                  "Circular Policy");
        return policyDefault;
    }

    const auto& object = *(subtree.begin());
    const auto& policyPath = object.first;
    const auto& policyService = object.second.begin()->first;

    std::variant<std::string> property;
    method = this->busLog.new_method_call(
        policyService.c_str(), policyPath.c_str(), dbusProperty, "Get");
    method.append(policyInterface, "SelPolicy");

    try
    {
        auto reply = this->busLog.call(method);
        reply.read(property);
    }
    catch (...)
    {
        lg2::error("Error reading SelPolicy  property. Continuing with default "
                   "Circular Policy");
        return policyDefault;
    }

    return std::get<std::string>(property);
}

inline std::string toString(LogType type)
{
    switch (type)
    {
        case LogType::DEFAULT:
            return "default";
        case LogType::IPMI:
            return "ipmi";
        case LogType::RAID:
            return "raid";
        default:
            return "unknown";
    }
}

void Manager::updateEntryLimits(LogType logType)
{
    std::string logTypeStr = toString(logType);

    bool newErrorFlag = realErrorsMap[logType].size() >= ERROR_CAP;
    bool newInfoFlag = infoErrorsMap[logType].size() >= ERROR_INFO_CAP;

    // Update internal state
    bool oldErrorFlag = errorFlags[logTypeStr];
    bool oldInfoFlag = infoFlags[logTypeStr];

    errorFlags[logTypeStr] = newErrorFlag;
    infoFlags[logTypeStr] = newInfoFlag;

    auto updateFlagIfChanged =
        [this](const std::string& flagName,
               const std::map<std::string, bool>& fullFlagMap, bool changed) {
            if (!changed)
                return;

            auto methodCall = busLog.new_method_call(
                "xyz.openbmc_project.Settings",
                "/xyz/openbmc_project/logging/settings",
                "org.freedesktop.DBus.Properties", "Set");

            methodCall.append(
                "xyz.openbmc_project.Logging.Settings", flagName,
                std::variant<std::map<std::string, bool>>(fullFlagMap));

            try
            {
                busLog.call(methodCall);
            }
            catch (const sdbusplus::exception_t& e)
            {
                lg2::error("Failed to update flag {FLAG} via D-Bus: {ERROR}",
                           "FLAG", flagName, "ERROR", e);
            }
        };

    updateFlagIfChanged("ErrorFlags", errorFlags, oldErrorFlag != newErrorFlag);
    updateFlagIfChanged("InfoFlags", infoFlags, oldInfoFlag != newInfoFlag);
}

void Manager::updateLastEntryId(uint16_t lastEntryId)
{
    auto methodCall = busLog.new_method_call(
        "xyz.openbmc_project.Settings", "/xyz/openbmc_project/logging/settings",
        "org.freedesktop.DBus.Properties", "Set");

    std::variant<uint16_t> value = lastEntryId;
    methodCall.append("xyz.openbmc_project.Logging.Settings", "lastEntryId",
                      value);
    try
    {
        busLog.call(methodCall);
    }
    catch (const sdbusplus::exception_t& e)
    {
        lg2::error("Failed to update lastEntryId : {ERROR}", "ERROR", e);
    }
}

void backupIPMIEntries()
{
    namespace fs = std::filesystem;

    try
    {
        fs::create_directories(IPMI_BACKUP_PATH);

        for (const auto& file : fs::directory_iterator(IPMI_BACKUP_PATH))
        {
            fs::remove_all(file);
        }

        for (const auto& file : fs::directory_iterator(ERRLOG_PERSIST_PATH_SEL))
        {
            if (fs::is_regular_file(file))
            {
                fs::path dst = fs::path(IPMI_BACKUP_PATH) /
                               file.path().filename();
                fs::copy_file(file.path(), dst,
                              fs::copy_options::overwrite_existing);
            }
        }

        lg2::info("IPMI log backup completed to {PATH}", "PATH",
                  IPMI_BACKUP_PATH);
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to backup IPMI logs: {ERROR}", "ERROR", e.what());
    }
}

inline void Manager::enforceCappingLimit(LogType logType, Entry::Level errLvl)
{
    if (errLvl < Entry::sevLowerLimit)
    {
        if (realErrorsMap[logType].size() >= ERROR_CAP)
        {
            erase(logType, realErrorsMap[logType].front());
        }
    }
    else
    {
        if (infoErrorsMap[logType].size() >= ERROR_INFO_CAP)
        {
            erase(logType, infoErrorsMap[logType].front());
        }
    }
}

inline void Manager::updateEntryCount()
{
    auto methodCall = busLog.new_method_call(
        "xyz.openbmc_project.Settings", "/xyz/openbmc_project/logging/settings",
        "org.freedesktop.DBus.Properties", "Set");

    LogType logType = LogType::IPMI;
    uint16_t ipmiEntryCount =
        realErrorsMap[logType].size() + infoErrorsMap[logType].size();

    std::variant<uint16_t> value = ipmiEntryCount;
    methodCall.append("xyz.openbmc_project.Logging.Settings", "ipmiEntryCount",
                      value);
    try
    {
        busLog.call(methodCall);
    }
    catch (const sdbusplus::exception_t& e)
    {
        lg2::error("Failed to update ipmiEntryCount : {ERROR}", "ERROR", e);
    }
}
auto Manager::createEntry(std::string errMsg, Entry::Level errLvl,
                          std::map<std::string, std::string> additionalData,
                          const FFDCEntries& ffdc)
    -> sdbusplus::message::object_path
{
    LogType logType = LogType::DEFAULT;

    for (const auto& [key, value] : additionalData)
    {
        if (key == "LOGTYPE" && value == "RAID")
        {
            logType = LogType::RAID;
        }
        else if (key == "RECORD_TYPE")
        {
            logType = LogType::IPMI;
        }
    }

    // SEL policy check only for IPMI
    if (logType == LogType::IPMI)
    {
        bool selEnabled;
        sdbusplus::bus::bus bus = sdbusplus::bus::new_default();
        auto method =
            bus.new_method_call(settingService, globalEnblObjpath,
                                "org.freedesktop.DBus.Properties", "Get");
        // Append the interface and property name to the method call
        method.append(globalEnblInterface, "Sel");

        try
        {
            auto reply = bus.call(method);
            // Extract the value from the response
            std::variant<bool> value;
            reply.read(value);
            selEnabled = std::get<bool>(value);
        }
        catch (const sdbusplus::exception_t& e)
        {
            lg2::error("Failed to get D-Bus property:{ERROR}", "ERROR", e);
            selEnabled = false;
        }
        if (!selEnabled)
        {
            lg2::info("SEL is disabled");
            return sdbusplus::message::object_path{};
            ;
        }

        if (!Extensions::disableDefaultLogCaps())
        {
            std::string currentPolicy = getSelPolicy();
            if (currentPolicy == policyLinear)
            {
                if (errLvl < Entry::sevLowerLimit)
                {
                    if (realErrorsMap[logType].size() >= ERROR_CAP)
                    {
                        lg2::info(
                            "Linear SEL: Error Capacity limit reached {ERROR_CAP}",
                            "ERROR_CAP", ERROR_CAP);
                        return sdbusplus::message::object_path{};
                        ;
                    }
                }
                else
                {
                    // Adding ipmi full event if info_error filled before max
                    // limit (error + info_error).
                    if ((infoErrorsMap[logType].size() == ERROR_INFO_CAP) &&
                        (infoErrorsMap[logType].size() +
                             realErrorsMap[logType].size() ==
                         ERROR_CAP + ERROR_INFO_CAP - 1))
                    {
                        std::string fullEvent = "System_Event_Log";
                        auto it = std::find_if(
                            additionalData.begin(), additionalData.end(),
                            [&fullEvent](const auto& pair) {
                                return pair.second.find(fullEvent) !=
                                       std::string::npos;
                            });
                        if (it != additionalData.end())
                        {
                            errLvl = Entry::Level::Critical;
                        }
                        else
                        {
                            return sdbusplus::message::object_path{};
                            ;
                        }
                    }
                    else if (infoErrorsMap[logType].size() >= ERROR_INFO_CAP)
                    {
                        lg2::info(
                            "Linear SEL: Information Error Capacity limit "
                            "reached {ERROR_CAP}",
                            "ERROR_CAP", ERROR_INFO_CAP);
                        return sdbusplus::message::object_path{};
                        ;
                    }
                }
            }
            else
            {
                enforceCappingLimit(logType, errLvl);
            }
        }
    }
    else
    {
        enforceCappingLimit(logType, errLvl);
    }

    // Generate new entry ID and update entryIdCounterMap
    entryId = ++entryIdCounterMap[logType];

    std::string objPath;
    std::string entryPath;

    if (logType == LogType::RAID)
    {
        objPath = std::string(OBJ_ENTRY_RAID) + "/" + std::to_string(entryId);
        entryPath = getEntrySerializePath(entryId, ERRLOG_PERSIST_PATH_RAID);
    }
    else if (logType == LogType::IPMI)
    {
        if (entryId > std::numeric_limits<uint16_t>::max())
        {
            lg2::info("Maximum limit of two bytes reached roll over starts");
            std::string logTypeStr = "ipmi";
            backupIPMIEntries();
            entryIdCounterMap[logType] = 1;
            entryId = entryIdCounterMap[logType];
            eraseLogTypeEntries(logTypeStr, 0);
        }
        objPath = std::string(OBJ_ENTRY_SEL) + "/" + std::to_string(entryId);
        entryPath = getEntrySerializePath(entryId, ERRLOG_PERSIST_PATH_SEL);
        updateLastEntryId(static_cast<uint16_t>(entryId));
        updateEntryCount();
    }
    else
    {
        objPath = std::string(OBJ_ENTRY) + "/" + std::to_string(entryId);
        entryPath = getEntrySerializePath(entryId, ERRLOG_PERSIST_PATH);
    }

    if (errLvl >= Entry::sevLowerLimit)
    {
        infoErrorsMap[logType].push_back(entryId);
    }
    else
    {
        realErrorsMap[logType].push_back(entryId);
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    // auto objPath = std::string(OBJ_ENTRY) + '/' + std::to_string(entryId);

    AssociationList objects{};
    auto additionalDataVec = util::additional_data::combine(additionalData);
    processMetadata(errMsg, additionalDataVec, objects);

    auto e = std::make_unique<Entry>(
        busLog, objPath, entryId,
        ms, // Milliseconds since 1970
        errLvl, std::move(errMsg), std::move(additionalData),
        std::move(objects), fwVersion, entryPath, *this);

    if (logType == LogType::RAID)
    {
        serialize(*e, ERRLOG_PERSIST_PATH_RAID);
    }
    else if (logType == LogType::IPMI)
    {
        serialize(*e, ERRLOG_PERSIST_PATH_SEL);
    }
    else
    {
        serialize(*e);
    }

    entries.insert(
        std::make_pair(std::make_pair(logType, entryId), std::move(e)));

    if (logType == LogType::DEFAULT)
    {
        // TODO: Currently, `quiesceOnError` and `Pels` are supported for
        // default logging events. Support for different types of logs can be
        // added in the future.
        if (isQuiesceOnErrorEnabled() && (errLvl < Entry::sevLowerLimit) &&
            isCalloutPresent(*e))
        {
            quiesceOnError(entryId);
        }

        // Add entry before calling the extensions so that they have access to
        // it
        doExtensionLogCreate(
            *(entries.find(std::make_pair(logType, entryId))->second), ffdc);
    }

    // Note: No need to close the file descriptors in the FFDC.
    updateEntryLimits(logType);
    if (logType == LogType::IPMI)
    {
        updateEntryCount();
    }
    return objPath;
}

auto Manager::createFromEvent(
    sdbusplus::exception::generated_event_base&& event)
    -> sdbusplus::message::object_path
{
    auto [msg, level, data] = lg2::details::extractEvent(std::move(event));
    return this->createEntry(msg, level, std::move(data));
}

bool Manager::isQuiesceOnErrorEnabled()
{
    // When running under tests, the Logging.Settings service will not be
    // present.  Assume false.
    if (IS_UNIT_TEST)
    {
        return false;
    }

    std::variant<bool> property;

    auto method = this->busLog.new_method_call(
        "xyz.openbmc_project.Settings", "/xyz/openbmc_project/logging/settings",
        "org.freedesktop.DBus.Properties", "Get");

    method.append("xyz.openbmc_project.Logging.Settings", "QuiesceOnHwError");

    try
    {
        auto reply = this->busLog.call(method);
        reply.read(property);
    }
    catch (const sdbusplus::exception_t& e)
    {
        lg2::error("Error reading QuiesceOnHwError property: {ERROR}", "ERROR",
                   e);
        return false;
    }

    return std::get<bool>(property);
}

bool Manager::isCalloutPresent(const Entry& entry)
{
    for (const auto& c : std::views::keys(entry.additionalData()))
    {
        if (c.find("CALLOUT_") != std::string::npos)
        {
            return true;
        }
    }

    return false;
}

void Manager::findAndRemoveResolvedBlocks()
{
    for (auto& entry : entries)
    {
        const auto& [logType, entryId] = entry.first;
        if (logType != LogType::DEFAULT)
        {
            continue;
        }
        if (entry.second->resolved())
        {
            checkAndRemoveBlockingError(entryId);
        }
    }
}

void Manager::onEntryResolve(sdbusplus::message_t& msg)
{
    using Interface = std::string;
    using Property = std::string;
    using Value = std::string;
    using Properties = std::map<Property, std::variant<Value>>;

    Interface interface;
    Properties properties;

    msg.read(interface, properties);

    for (const auto& p : properties)
    {
        if (p.first == "Resolved")
        {
            findAndRemoveResolvedBlocks();
            return;
        }
    }
}

void Manager::checkAndQuiesceHost()
{
    using Host = sdbusplus::server::xyz::openbmc_project::state::Host;

    // First check host state
    std::variant<Host::HostState> property;

    auto method = this->busLog.new_method_call(
        "xyz.openbmc_project.State.Host", "/xyz/openbmc_project/state/host0",
        "org.freedesktop.DBus.Properties", "Get");

    method.append("xyz.openbmc_project.State.Host", "CurrentHostState");

    try
    {
        auto reply = this->busLog.call(method);
        reply.read(property);
    }
    catch (const sdbusplus::exception_t& e)
    {
        // Quiescing the host is a "best effort" type function. If unable to
        // read the host state or it comes back empty, just return.
        // The boot block object will still be created and the associations to
        // find the log will be present. Don't want a dependency with
        // phosphor-state-manager service
        lg2::info("Error reading QuiesceOnHwError property: {ERROR}", "ERROR",
                  e);
        return;
    }

    auto hostState = std::get<Host::HostState>(property);
    if (hostState != Host::HostState::Running)
    {
        return;
    }

    auto quiesce = this->busLog.new_method_call(
        "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
        "org.freedesktop.systemd1.Manager", "StartUnit");

    quiesce.append("obmc-host-graceful-quiesce@0.target");
    quiesce.append("replace");

    this->busLog.call_noreply(quiesce);
}

void Manager::quiesceOnError(const uint32_t entryId)
{
    // Verify we don't already have this entry blocking
    auto it = find_if(this->blockingErrors.begin(), this->blockingErrors.end(),
                      [&](const std::unique_ptr<Block>& obj) {
                          return obj->entryId == entryId;
                      });
    if (it != this->blockingErrors.end())
    {
        // Already recorded so just return
        lg2::debug(
            "QuiesceOnError set and callout present but entry already logged");
        return;
    }

    lg2::info("QuiesceOnError set and callout present");

    auto blockPath =
        std::string(OBJ_LOGGING) + "/block" + std::to_string(entryId);
    auto blockObj = std::make_unique<Block>(this->busLog, blockPath, entryId);
    this->blockingErrors.push_back(std::move(blockObj));

    // Register call back if log is resolved
    using namespace sdbusplus::bus::match::rules;
    auto entryPath = std::string(OBJ_ENTRY) + '/' + std::to_string(entryId);
    auto callback = std::make_unique<sdbusplus::bus::match_t>(
        this->busLog,
        propertiesChanged(entryPath, "xyz.openbmc_project.Logging.Entry"),
        std::bind(std::mem_fn(&Manager::onEntryResolve), this,
                  std::placeholders::_1));

    propChangedEntryCallback.insert(
        std::make_pair(entryId, std::move(callback)));

    checkAndQuiesceHost();
}

void Manager::doExtensionLogCreate(const Entry& entry, const FFDCEntries& ffdc)
{
    // Make the association <endpointpath>/<endpointtype> paths
    std::vector<std::string> assocs;
    for (const auto& [forwardType, reverseType, endpoint] :
         entry.associations())
    {
        std::string e{endpoint};
        e += '/' + reverseType;
        assocs.push_back(e);
    }

    for (auto& create : Extensions::getCreateFunctions())
    {
        try
        {
            create(entry.message(), entry.id(), entry.timestamp(),
                   entry.severity(), entry.additionalData(), assocs, ffdc);
        }
        catch (const std::exception& e)
        {
            lg2::error(
                "An extension's create function threw an exception: {ERROR}",
                "ERROR", e);
        }
    }
}

void Manager::processMetadata(const std::string& /*errorName*/,
                              const std::vector<std::string>& additionalData,
                              AssociationList& objects) const
{
    // additionalData is a list of "metadata=value"
    constexpr auto separator = '=';
    for (const auto& entryItem : additionalData)
    {
        auto found = entryItem.find(separator);
        if (std::string::npos != found)
        {
            auto metadata = entryItem.substr(0, found);
            auto iter = meta.find(metadata);
            if (meta.end() != iter)
            {
                (iter->second)(metadata, additionalData, objects);
            }
        }
    }
}

void Manager::checkAndRemoveBlockingError(uint32_t entryId)
{
    // First look for blocking object and remove
    auto it = find_if(blockingErrors.begin(), blockingErrors.end(),
                      [&](const std::unique_ptr<Block>& obj) {
                          return obj->entryId == entryId;
                      });
    if (it != blockingErrors.end())
    {
        blockingErrors.erase(it);
    }

    // Now remove the callback looking for the error to be resolved
    auto resolveFind = propChangedEntryCallback.find(entryId);
    if (resolveFind != propChangedEntryCallback.end())
    {
        propChangedEntryCallback.erase(resolveFind);
    }

    return;
}

void Manager::erase(LogType logType, uint32_t entryId, bool deferUpdates)
{
    auto entryFound = entries.find(std::make_pair(logType, entryId));
    if (entries.end() != entryFound)
    {
        for (auto& func : Extensions::getDeleteProhibitedFunctions())
        {
            try
            {
                bool prohibited = false;
                func(entryId, prohibited);
                if (prohibited)
                {
                    throw sdbusplus::xyz::openbmc_project::Common::Error::
                        Unavailable();
                }
            }
            catch (const sdbusplus::xyz::openbmc_project::Common::Error::
                       Unavailable& e)
            {
                throw;
            }
            catch (const std::exception& e)
            {
                lg2::error("An extension's deleteProhibited function threw an "
                           "exception: {ERROR}",
                           "ERROR", e);
            }
        }

        // Delete the persistent representation of this error.
        fs::path errorPath;
        if (logType == LogType::IPMI)
        {
            errorPath = ERRLOG_PERSIST_PATH_SEL;
        }
        else if (logType == LogType::DEFAULT)
        {
            errorPath = ERRLOG_PERSIST_PATH;
        }
        else if (logType == LogType::RAID)
        {
            errorPath = ERRLOG_PERSIST_PATH_RAID;
        }
        errorPath /= std::to_string(entryId);
        fs::remove(errorPath);

        auto removeId = [](std::list<uint32_t>& ids, uint32_t id) {
            auto it = std::find(ids.begin(), ids.end(), id);
            if (it != ids.end())
            {
                ids.erase(it);
            }
        };
        if (entryFound->second->severity() >= Entry::sevLowerLimit)
        {
            removeId(infoErrorsMap[logType], entryId);
        }
        else
        {
            removeId(realErrorsMap[logType], entryId);
        }
        entries.erase(entryFound);

        if (logType == LogType::DEFAULT)
        {
            checkAndRemoveBlockingError(entryId);

            for (auto& remove : Extensions::getDeleteFunctions())
            {
                try
                {
                    remove(entryId);
                }
                catch (const std::exception& e)
                {
                    lg2::error(
                        "An extension's delete function threw an exception: "
                        "{ERROR}",
                        "ERROR", e);
                }
            }
        }
        else
        {
            lg2::error("Invalid entry ID ({ID}) to delete", "ID", entryId);
        }

        if (!deferUpdates)
        {
            updateEntryLimits(logType);
        }
        if (logType == LogType::IPMI)
        {
            if (!deferUpdates)
            {
                if (entryId)
                {
                    updateLastEntryId(--entryId);
                }
                updateEntryCount();
            }
        }
    }
}

void Manager::restore()
{
    auto sanity = [](const auto& id, const auto& restoredId) {
        return id == restoredId;
    };

    // Iterate over all log types
    for (const auto& logType : {LogType::DEFAULT, LogType::IPMI, LogType::RAID})
    {
        fs::path dir;
        std::string objEntry;

        switch (logType)
        {
            case LogType::DEFAULT:
                dir = ERRLOG_PERSIST_PATH;
                objEntry = OBJ_ENTRY;
                break;
            case LogType::IPMI:
                dir = ERRLOG_PERSIST_PATH_SEL;
                objEntry = OBJ_ENTRY_SEL;
                break;
            case LogType::RAID:
                dir = ERRLOG_PERSIST_PATH_RAID;
                objEntry = OBJ_ENTRY_RAID;
                break;
        }

        if (!fs::exists(dir) || fs::is_empty(dir))
        {
            continue;
        }

        // Special handling for IPMI: sort files by modification time
        if (logType == LogType::IPMI)
        {
            std::vector<std::tuple<time_t, long, fs::path>> sortedFiles;

            for (auto& file : fs::directory_iterator(dir))
            {
                try
                {
                    auto id = file.path().filename().string();
                    auto idNum = std::stol(id);
                    struct stat st;
                    if (stat(file.path().c_str(), &st) == 0)
                    {
                        sortedFiles.emplace_back(st.st_mtime, idNum,
                                                 file.path());
                    }
                }
                catch (...)
                {
                    continue; // skip invalid files
                }
            }

            std::stable_sort(sortedFiles.begin(), sortedFiles.end(),
                             [](const auto& a, const auto& b) {
                                 auto t1 = std::get<0>(a), t2 = std::get<0>(b);
                                 if (t1 != t2)
                                     return t1 < t2;
                                 return std::get<1>(a) < std::get<1>(b);
                             });

            // Now restore in sorted order
            for (const auto& [timestamp, idNum, path] : sortedFiles)
            {
                auto idStr = std::to_string(idNum);
                auto e = std::make_unique<Entry>(busLog, objEntry + '/' + idStr,
                                                 idNum, *this);

                if (deserialize(path, *e))
                {
                    if (sanity(static_cast<uint32_t>(idNum), e->id()))
                    {
                        e->path(path, true);

                        if (e->severity() >= Entry::sevLowerLimit)
                        {
                            infoErrorsMap[logType].push_back(idNum);
                        }
                        else
                        {
                            realErrorsMap[logType].push_back(idNum);
                        }

                        entries.emplace(std::make_pair(logType, idNum),
                                        std::move(e));
                        entryIdCounterMap[logType] = idNum;
                        updateLastEntryId(idNum);
                        updateEntryCount();
                    }
                    else
                    {
                        lg2::error(
                            "Failed in sanity check while restoring error entry. "
                            "Ignoring error entry {ID_NUM}/{ENTRY_ID}.",
                            "ID_NUM", idNum, "ENTRY_ID", e->id());
                    }
                }
            }
        }
        else
        {
            for (auto& file : fs::directory_iterator(dir))
            {
                auto id = file.path().filename().c_str();
                auto idNum = std::stol(id);
                auto e = std::make_unique<Entry>(busLog, objEntry + '/' + id,
                                                 idNum, *this);

                if (deserialize(file.path(), *e))
                {
                    // validate the restored error entry id
                    if (sanity(static_cast<uint32_t>(idNum), e->id()))
                    {
                        e->path(file.path(), true);

                        if (e->severity() >= Entry::sevLowerLimit)
                        {
                            infoErrorsMap[logType].push_back(idNum);
                        }
                        else
                        {
                            realErrorsMap[logType].push_back(idNum);
                        }

                        entries.emplace(std::make_pair(logType, idNum),
                                        std::move(e));
                    }
                    else
                    {
                        lg2::error(
                            "Failed in sanity check while restoring error entry. "
                            "Ignoring error entry {ID_NUM}/{ENTRY_ID}.",
                            "ID_NUM", idNum, "ENTRY_ID", e->id());
                    }
                }
            }
            auto it = entries.lower_bound(std::make_pair(logType, 0));
            if (it != entries.end())
            {
                auto lastIt = it;
                for (;
                     lastIt != entries.end() && lastIt->first.first == logType;
                     ++lastIt)
                {}
                if (lastIt != it)
                {
                    --lastIt;
                    entryIdCounterMap[logType] = lastIt->first.second;
                }
            }
        }
    }
}

std::string Manager::readFWVersion()
{
    auto version = util::getOSReleaseValue("VERSION_ID");

    if (!version)
    {
        lg2::error("Unable to read BMC firmware version");
    }

    return version.value_or("");
}

auto Manager::create(const std::string& message, Entry::Level severity,
                     const std::map<std::string, std::string>& additionalData,
                     const FFDCEntries& ffdc) -> sdbusplus::message::object_path
{
    return createEntry(message, severity, additionalData, ffdc);
}

uint16_t Manager::eraseLogTypeEntries(std::string& logTypeStr, uint32_t entryId)
{
    if (logTypeStr.empty())
    {
        // Case 1: logTypeStr is empty – not allowed
        throw sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument();
    }

    LogType type;

    // Convert logType string to enum
    if (logTypeStr == "ipmi")
    {
        type = LogType::IPMI;
    }
    else if (logTypeStr == "raid")
    {
        type = LogType::RAID;
    }
    else if (logTypeStr == "default")
    {
        type = LogType::DEFAULT;
    }
    else
    {
        // Case 1: Invalid log type string
        throw sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument();
    }

    uint16_t erasedCount = 0;

    if (entryId == 0)
    {
        // Case 2: Delete all entries of the specified log type.
        // Use deferUpdates=true to skip per-entry D-Bus calls; do one batch
        // update after the loop.
        for (auto iter = entries.begin(); iter != entries.end();)
        {
            if (iter->first.first == type)
            {
                auto current = iter++;
                erase(current->first.first, current->first.second,
                      /*deferUpdates=*/true);
                ++erasedCount;
            }
            else
            {
                ++iter;
            }
        }

        // Reset entry counter for this logType
        entryIdCounterMap[type] = 0;

        // Single batch D-Bus update after all entries are erased.
        if (type == LogType::IPMI)
        {
            updateLastEntryId(0);
            updateEntryCount();
        }
    }
    else
    {
        // Case 3: Delete specific entry ID of the specified log type
        auto key = std::make_pair(type, entryId);
        auto iter = entries.find(key);
        if (iter != entries.end())
        {
            erase(type, entryId);
            erasedCount = 1;
        }
        else
        {
            // Entry not found
            throw sdbusplus::xyz::openbmc_project::Common::Error::
                InvalidArgument();
        }
    }

    if (!entryId)
    {
        updateEntryLimits(type);
    }

    return erasedCount;
}

} // namespace internal
} // namespace logging
} // namespace phosphor
