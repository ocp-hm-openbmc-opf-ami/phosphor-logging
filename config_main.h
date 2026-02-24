#pragma once

#ifdef STORAGE_EMMC_SDCARD
const char* ERRLOG_PERSIST_PATH = "/etc/extlog/phosphor-logging/errors";
const char* EXTENSION_PERSIST_DIR = "/etc/extlog/phosphor-logging/extensions";
const char* ERRLOG_PERSIST_PATH_SEL = "/etc/extlog/phosphor-logging/ipmi/errors";
const char* ERRLOG_PERSIST_PATH_RAID = "/etc/extlog/phosphor-logging/raid/errors";
const char* IPMI_BACKUP_PATH = "/etc/extlog/phosphor-logging/ipmi_rollover_backup";
#else
const char* ERRLOG_PERSIST_PATH = "/var/lib/phosphor-logging/errors";
const char* EXTENSION_PERSIST_DIR = "/var/lib/phosphor-logging/extensions";
const char* ERRLOG_PERSIST_PATH_SEL = "/var/lib/phosphor-logging/ipmi/errors";
const char* ERRLOG_PERSIST_PATH_RAID = "/var/lib/phosphor-logging/raid/errors";
const char* IPMI_BACKUP_PATH = "/var/lib/phosphor-logging/ipmi_rollover_backup";


#endif

const bool IS_UNIT_TEST = false;
