#define _GNU_SOURCE

#include "com.h"
#include "dbg.h"
#include "ipc_resp.h"
#include "scopestdlib.h"
#include "runtimecfg.h"

#define ARRAY_SIZE(arr) ((sizeof(arr))/(sizeof(arr[0])))

static const char* cmdMetaName[] = {
    [META_REQ_JSON]         = "completeRequestJson",
    [META_REQ_JSON_PARTIAL] = "incompleteRequestJson",
};

#define CMD_META_SIZE  (ARRAY_SIZE(cmdMetaName))

static const char* cmdScopeName[] = {
    [IPC_CMD_GET_SUPPORTED_CMD]    = "getSupportedCmd",
    [IPC_CMD_GET_SCOPE_STATUS]     = "getScopeStatus",
    [IPC_CMD_GET_SCOPE_CFG]        = "getScopeCfg",
    [IPC_CMD_SET_SCOPE_CFG]        = "setScopeCfg",
    [IPC_CMD_GET_TRANSPORT_STATUS] = "getTransportStatus",
};

#define CMD_SCOPE_SIZE  (ARRAY_SIZE(cmdScopeName))

extern void doAndReplaceConfig(void *);
extern log_t *g_log;
extern mtc_t *g_mtc;
extern ctl_t *g_ctl;

#define WRAP_PRIV_SIZE (2)

// Wrapper for scope message response
struct scopeRespWrapper{
    cJSON *resp;                // Scope message response
    void *priv[WRAP_PRIV_SIZE]; // Additional resources allocated to create response
};

/*
 * Creates the scope response wrapper object
 */
static scopeRespWrapper *
respWrapperCreate(void) {
    scopeRespWrapper *wrap = scope_calloc(1, sizeof(scopeRespWrapper));
    if (!wrap) {
        return NULL;
    }
    wrap->resp = NULL;
    for (int i = 0; i < WRAP_PRIV_SIZE; ++i) {
        wrap->priv[i] = NULL;
    }
    return wrap;
}

/*
 * Destroys the scope response wrapper object
 */
void
ipcRespWrapperDestroy(scopeRespWrapper *wrap) {
    if (wrap->resp) {
        cJSON_free(wrap->resp);
    }
    for (int i = 0; i < WRAP_PRIV_SIZE; ++i) {
        if (wrap->priv[i]) {
            cJSON_free(wrap->priv[i]);
        }  
    }

    scope_free(wrap);
}

/*
 * Returns the scope message response string representation
 */
char *
ipcRespScopeRespStr(scopeRespWrapper *wrap) {
    return cJSON_PrintUnformatted(wrap->resp);
}

/*
 * Creates the wrapper for generic response (scope message and ipc message)
 * Used by following requests: IPC_CMD_UNKNOWN, IPC_CMD_SET_SCOPE_CFG
 */
scopeRespWrapper *
ipcRespStatus(ipc_resp_status_t status) {
    scopeRespWrapper *wrap = respWrapperCreate();
    if (!wrap) {
        return NULL;
    }
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        goto allocFail;
    }
    wrap->resp = resp;
    if (!cJSON_AddNumberToObjLN(resp, "status", status)) {
        goto allocFail;
    }

    return wrap;

allocFail:
    ipcRespWrapperDestroy(wrap);
    return NULL; 
}

/*
 * Creates descriptor for meta and scope command used in IPC_CMD_GET_SUPPORTED_CMD
 */
static cJSON*
createCmdDesc(int id, const char *name) {
    cJSON *cmdDesc = cJSON_CreateObject();
    if (!cmdDesc) {
        return NULL;
    }

    if (!cJSON_AddNumberToObject(cmdDesc, "id", id)) {
        cJSON_free(cmdDesc);
        return NULL;
    }

    if (!cJSON_AddStringToObject(cmdDesc, "name", name)) {
        cJSON_free(cmdDesc);
        return NULL;
    }

    return cmdDesc;
}

/*
 * Creates the wrapper for response to IPC_CMD_GET_SUPPORTED_CMD
 * TODO: use unused attribute later
 */
scopeRespWrapper *
ipcRespGetScopeCmds(const cJSON * unused) {
    SCOPE_BUILD_ASSERT(IPC_CMD_UNKNOWN == CMD_SCOPE_SIZE, "cmdScopeName must be inline with ipc_scope_req_t");

    scopeRespWrapper *wrap = respWrapperCreate();
    if (!wrap) {
        return NULL;
    }
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        goto allocFail;
    }
    wrap->resp = resp;
    if (!cJSON_AddNumberToObjLN(resp, "status", IPC_RESP_OK)) {
        goto allocFail;
    }

    cJSON *metaCmds = cJSON_CreateArray();
    if (!metaCmds) {
        goto allocFail;
    }

    wrap->priv[0] = metaCmds;
    for (int id = 0; id < CMD_META_SIZE; ++id){
        cJSON *singleCmd = createCmdDesc(id, cmdMetaName[id]);
        if (!singleCmd) {
            goto allocFail;
        }
        cJSON_AddItemToArray(metaCmds, singleCmd);
    }
    cJSON_AddItemToObjectCS(resp, "commands_meta", metaCmds);

    cJSON *scopeCmds = cJSON_CreateArray();
    if (!scopeCmds) {
        goto allocFail;
    }

    wrap->priv[1] = scopeCmds;
    for (int id = 0; id < CMD_SCOPE_SIZE; ++id){
        cJSON *singleCmd = createCmdDesc(id, cmdScopeName[id]);
        if (!singleCmd) {
            goto allocFail;
        }
        cJSON_AddItemToArray(scopeCmds, singleCmd);
    }
    cJSON_AddItemToObjectCS(resp, "commands_scope", scopeCmds);

    return wrap;

allocFail:
    ipcRespWrapperDestroy(wrap);
    return NULL; 
}

/*
 * Creates the wrapper for response to IPC_CMD_GET_SCOPE_STATUS
 * TODO: use unused attribute later
 */
scopeRespWrapper *
ipcRespGetScopeStatus(const cJSON *unused) {
    scopeRespWrapper *wrap = respWrapperCreate();
    if (!wrap) {
        return NULL;
    }
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        goto allocFail;
    }
    wrap->resp = resp;
    if (!cJSON_AddNumberToObjLN(resp, "status", IPC_RESP_OK)) {
        goto allocFail;
    }
    if (!cJSON_AddBoolToObjLN(resp, "scoped", (g_cfg.funcs_attached))) {
        goto allocFail;
    }
    return wrap;

allocFail:
    ipcRespWrapperDestroy(wrap);
    return NULL; 
}

/*
 * Creates the wrapper for response to IPC_CMD_GET_SCOPE_CFG
 * TODO: use unused attribute later
 */
scopeRespWrapper *
ipcRespGetScopeCfg(const cJSON *unused) {
    scopeRespWrapper *wrap = respWrapperCreate();
    if (!wrap) {
        return NULL;
    }
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        goto allocFail;
    }
    wrap->resp = resp;

    cJSON *cfg = jsonConfigurationObject(g_cfg.staticfg);
    if (!cfg) {
        if (!cJSON_AddNumberToObjLN(resp, "status", IPC_RESP_SERVER_ERROR)) {
            goto allocFail;
        }
        return wrap;
    }
    wrap->priv[0] = cfg;

    cJSON_AddItemToObjectCS(resp, "cfg", cfg);
    
    if (!cJSON_AddNumberToObjLN(resp, "status", IPC_RESP_OK)) {
        goto allocFail;
    }
    
    return wrap;

allocFail:
    ipcRespWrapperDestroy(wrap);
    return NULL;
}

/*
 * Creates the wrapper for response to IPC_CMD_UNKNOWN
 * TODO: use unused attribute later
 */
scopeRespWrapper *
ipcRespStatusNotImplemented(const cJSON *unused) {
    return ipcRespStatus(IPC_RESP_NOT_IMPLEMENTED);
}

/*
 * Process the request IPC_CMD_SET_SCOPE_CFG
 */
static bool
ipcProcessSetCfg(const cJSON *scopeReq) {
    bool res = FALSE;
    // Verify if scope request is based on JSON-format
    cJSON *cfgKey = cJSON_GetObjectItem(scopeReq, "cfg");
    if (!cfgKey || !cJSON_IsObject(cfgKey)) {
        return res;
    }
    char *cfgStr = cJSON_PrintUnformatted(cfgKey);
    config_t *cfg = cfgFromString(cfgStr);
    doAndReplaceConfig(cfg);
    res = TRUE;
    return res;
}

/*
 * Creates the wrapper for response to IPC_CMD_SET_SCOPE_CFG
 */
scopeRespWrapper *
ipcRespSetScopeCfg(const cJSON *scopeReq) {
    if (ipcProcessSetCfg(scopeReq)) {
        return ipcRespStatus(IPC_RESP_OK);
    }
    return ipcRespStatus(IPC_RESP_SERVER_ERROR);
}

/*
 * The interface*Func are functions accessors definitions, used to retrieve information about:
 * - enablement of specific interface
 * - transport status of specific interface
 */
typedef transport_status_t (*interfaceStatusFunc)(void);
typedef bool               (*interfaceEnabledFunc)(void);

/*
 * singleInterface is structure contains the interface object
 */
struct singleInterface {
    const char *name;
    interfaceEnabledFunc enabled;
    interfaceStatusFunc status;
};

/*
 * logTransportEnabled retrieves status if "log" interface is enabled
 */
static bool
logTransportEnabled(void) {
    return TRUE;
}

/*
 * logTransportStatus retrieves the status of "log" interface
 */
static transport_status_t
logTransportStatus(void) {
    return logConnectionStatus(g_log);
}

/*
 * metricTransportEnabled retrieves status if "mtc" interface is enabled
 */
static bool
metricTransportEnabled(void) {
    return mtcEnabled(g_mtc);
}

/*
 * metricsTransportStatus retrieves the status of "metric" interface
 */
static transport_status_t
metricsTransportStatus(void) {
    return mtcConnectionStatus(g_mtc);
}

/*
 * eventsTransportEnabled retrieves the status of "events" interface
 */
static bool
eventsTransportEnabled(void) {
    return cfgEvtEnable(g_cfg.staticfg);
}

/*
 * eventsTransportStatus retrieves the status of "events" interface
 */
static transport_status_t
eventsTransportStatus(void) {
    return ctlConnectionStatus(g_ctl, CFG_CTL);
}

/*
 * payloadTransportEnabled retrieves status if "payload" interface is enabled
 */
static bool
payloadTransportEnabled(void) {
    return ctlPayEnable(g_ctl);
}

/*
 * payloadTransportStatus retrieves the status of "payload" interface
 */
static transport_status_t
payloadTransportStatus(void) {
    return ctlConnectionStatus(g_ctl, CFG_LS);
}

static const
struct singleInterface scope_interfaces[] = {
    {.name = "log",     .enabled = logTransportEnabled,     .status = logTransportStatus},
    {.name = "metrics", .enabled = metricTransportEnabled,  .status = metricsTransportStatus},
    {.name = "events",  .enabled = eventsTransportEnabled,  .status = eventsTransportStatus},
    {.name = "payload", .enabled = payloadTransportEnabled, .status = payloadTransportStatus},
};

#define TOTAL_INTERFACES (sizeof(scope_interfaces)/sizeof(scope_interfaces[0]))

/*
 * Creates the wrapper for response to IPC_CMD_GET_TRANSPORT_STATUS
 * TODO: use unused attribute later
 */
scopeRespWrapper *
ipcRespGetTransportStatus(const cJSON *unused) {
    scopeRespWrapper *wrap = respWrapperCreate();
    if (!wrap) {
        return NULL;
    }
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        goto allocFail;
    }
    wrap->resp = resp;
    if (!cJSON_AddNumberToObjLN(resp, "status", IPC_RESP_OK)) {
        goto allocFail;
    }

    cJSON *interfaces = cJSON_CreateArray();
    if (!interfaces) {
        goto allocFail;
    }
    wrap->priv[0] = interfaces;
    for (int index = 0; index < TOTAL_INTERFACES; ++index){
        // Skip preparing the interface info if it is disabled 
        if (!scope_interfaces[index].enabled()) {
            continue;
        }

        cJSON *singleInterface = cJSON_CreateObject();
        if (!singleInterface) {
            goto allocFail;
        }

        transport_status_t status = scope_interfaces[index].status();

        if (!cJSON_AddStringToObject(singleInterface, "name", scope_interfaces[index].name)) {
            goto allocFail;
        }

        if (!cJSON_AddStringToObject(singleInterface, "config", status.configString)) {
            goto allocFail;
        }

        if (status.isConnected == TRUE) {
            if (!cJSON_AddTrueToObject(singleInterface, "connected")) {
                goto allocFail;
            }
        } else {
            if (!cJSON_AddFalseToObject(singleInterface, "connected")) {
                goto allocFail;
            }
            if (!cJSON_AddNumberToObject(singleInterface, "attempts", status.connectAttemptCount)) {
                goto allocFail;
            }

            // TODO: Add failure string always ?
            if (status.failureString) {
                if (!cJSON_AddStringToObject(singleInterface, "failure_details", status.failureString)) {
                    goto allocFail;
                }
            }
        }
        cJSON_AddItemToArray(interfaces, singleInterface);
    }
    cJSON_AddItemToObjectCS(resp, "interfaces", interfaces);
    return wrap;

allocFail:
    ipcRespWrapperDestroy(wrap);
    return NULL; 
}

/*
 * Creates the wrapper for failed case in processing scope msg
 */
scopeRespWrapper *
ipcRespStatusScopeError(ipc_resp_status_t status) {
    return ipcRespStatus(status);
}
