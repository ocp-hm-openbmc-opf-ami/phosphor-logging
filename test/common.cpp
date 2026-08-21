#include "config.h"

const char* PERSIST_PATH_ROOT = "/tmp/phosphor-logging";
const bool IS_UNIT_TEST = true;

#ifdef UNIT_TEST
const char* ERRLOG_PERSIST_PATH = "/tmp/phosphor-logging/errors";
const char* ERRLOG_PERSIST_PATH_SEL = "/tmp/phosphor-logging/ipmi/errors";
const char* ERRLOG_PERSIST_PATH_RAID = "/tmp/phosphor-logging/raid/errors";
const char* IPMI_BACKUP_PATH = "/tmp/phosphor-logging/ipmi_rollover_backup";
const char* EXTENSION_PERSIST_DIR = "/tmp/phosphor-logging/extensions";
#endif
