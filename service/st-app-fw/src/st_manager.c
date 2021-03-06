/****************************************************************************
 *
 * Copyright 2018 Samsung Electronics All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the License.
 *
 ****************************************************************************/

#include <stdlib.h>

#include "oc_api.h"
#include "oc_core_res.h"
#include "port/oc_clock.h"
#include "port/oc_connectivity.h"
#ifdef OC_SECURITY
#include "security/oc_doxm.h"
#include "security/oc_pstat.h"
#endif
#include "st_cloud_manager.h"
#include "st_data_manager.h"
#include "st_easy_setup.h"
#include "st_fota_manager.h"
#include "st_manager.h"
#include "st_port.h"
#include "st_process.h"
#include "st_resource_manager.h"
#include "st_store.h"

#define EXIT_WITH_ERROR(err)                                                   \
  do {                                                                         \
    st_err_ret = err;                                                          \
    goto exit;                                                                 \
  } while (0);

#define SOFT_AP_PWD "1111122222"
#define SOFT_AP_CHANNEL (6)
#define AP_CONNECT_RETRY_LIMIT (20)

extern int st_register_resources(int device);
extern int st_fota_manager_start(void);
extern void st_fota_manager_stop(void);

static st_status_t g_main_status = ST_STATUS_IDLE;
static st_status_cb_t g_st_status_cb = NULL;

static sc_properties st_vendor_props;

static sec_provisioning_info g_prov_resource;

static int device_index = 0;

static bool g_start_fail = false;

static void set_st_manager_status(st_status_t status);

static void st_manager_evt_stop_handler(void);

static void st_manager_evt_reset_handler(void);

typedef struct
{
  oc_string_t model_number;
  oc_string_t platform_version;
  oc_string_t os_version;
  oc_string_t hardware_version;
  oc_string_t firmware_version;
  oc_string_t vendor_id;
} platform_cb_data_t;

static platform_cb_data_t platform_cb_data;

static void free_platform_cb_data(void)
{
  if(oc_string(platform_cb_data.model_number))
    oc_free_string(&platform_cb_data.model_number);
  if(oc_string(platform_cb_data.platform_version))
    oc_free_string(&platform_cb_data.platform_version);
  if(oc_string(platform_cb_data.os_version))
    oc_free_string(&platform_cb_data.os_version);
  if(oc_string(platform_cb_data.hardware_version))
    oc_free_string(&platform_cb_data.hardware_version);
  if(oc_string(platform_cb_data.firmware_version))
    oc_free_string(&platform_cb_data.firmware_version);
  if(oc_string(platform_cb_data.vendor_id))
    oc_free_string(&platform_cb_data.vendor_id);
}

static platform_cb_data_t* clone_platform_cb_data(st_specification_t *spec)
{
  if(!spec) return NULL;
  free_platform_cb_data();
  oc_new_string(&platform_cb_data.model_number,
                oc_string(spec->platform.model_number),
                oc_string_len(spec->platform.model_number));
  oc_new_string(&platform_cb_data.platform_version,
                oc_string(spec->platform.platform_version),
                oc_string_len(spec->platform.platform_version));
  oc_new_string(&platform_cb_data.os_version,
                oc_string(spec->platform.os_version),
                oc_string_len(spec->platform.os_version));
  oc_new_string(&platform_cb_data.hardware_version,
                oc_string(spec->platform.hardware_version),
                oc_string_len(spec->platform.hardware_version));
  oc_new_string(&platform_cb_data.firmware_version,
                oc_string(spec->platform.firmware_version),
                oc_string_len(spec->platform.firmware_version));
  oc_new_string(&platform_cb_data.vendor_id,
                oc_string(spec->platform.vendor_id),
                oc_string_len(spec->platform.vendor_id));
  return &platform_cb_data;
}

static void
init_platform_cb(void *data)
{
  if(!data) return;
  platform_cb_data_t *platform = data;
  oc_set_custom_platform_property(mnmo, oc_string(platform->model_number));
  oc_set_custom_platform_property(mnpv, oc_string(platform->platform_version));
  oc_set_custom_platform_property(mnos, oc_string(platform->os_version));
  oc_set_custom_platform_property(mnhw, oc_string(platform->hardware_version));
  oc_set_custom_platform_property(mnfv, oc_string(platform->firmware_version));
  oc_set_custom_platform_property(vid, oc_string(platform->vendor_id));
}

static int
app_init(void)
{
  st_specification_t *spec = st_data_mgr_get_spec_info();
  platform_cb_data_t *platform_data = clone_platform_cb_data(spec);
  int ret = oc_init_platform(oc_string(spec->platform.manufacturer_name),
                             init_platform_cb, platform_data);
  ret |= oc_add_device("/oic/d", oc_string(spec->device.device_type),
                       oc_string(spec->device.device_name),
                       oc_string(spec->device.spec_version),
                       oc_string(spec->device.data_model_version), NULL, NULL);
  return ret;
}

static void
register_resources(void)
{
  if (st_register_resources(device_index) != 0) {
    st_print_log("[ST_MGR] register_resources failed.\n");
  }
}

void
easy_setup_handler(st_easy_setup_status_t status)
{
  if (status == EASY_SETUP_FINISH) {
    st_print_log("[ST_MGR] Easy setup succeed!!!\n");
    set_st_manager_status(ST_STATUS_EASY_SETUP_DONE);
  } else if (status == EASY_SETUP_RESET) {
    st_print_log("[ST_MGR] Easy setup reset!!!\n");
    set_st_manager_status(ST_STATUS_RESET);
  } else if (status == EASY_SETUP_FAIL) {
    st_print_log("[ST_MGR] Easy setup failed!!!\n");
    g_start_fail = true;
    set_st_manager_status(ST_STATUS_STOP);
  }
}

void
cloud_manager_handler(st_cloud_manager_status_t status)
{
  if (status == CLOUD_MANAGER_FINISH) {
    st_print_log("[ST_MGR] Cloud manager succeed!!!\n");
    set_st_manager_status(ST_STATUS_CLOUD_MANAGER_DONE);
  } else if (status == CLOUD_MANAGER_FAIL) {
    st_print_log("[ST_MGR] Cloud manager failed!!!\n");
    g_start_fail = true;
    set_st_manager_status(ST_STATUS_STOP);
  } else if (status == CLOUD_MANAGER_RE_CONNECTING) {
    st_print_log("[ST_MGR] Cloud manager re connecting!!!\n");
    set_st_manager_status(ST_STATUS_CLOUD_MANAGER_PROGRESSING);
  } else if (status == CLOUD_MANAGER_RESET) {
    st_print_log("[ST_MGR] Cloud manager reset!!!\n");
    set_st_manager_status(ST_STATUS_RESET);
  }
}

static void
set_sc_prov_info(void)
{
  // Set prov info properties
  int target_size = 1;
  char uuid[OC_UUID_LEN];
  int i = 0;

  g_prov_resource.targets = (sec_provisioning_info_targets *)calloc(
    target_size, sizeof(sec_provisioning_info_targets));
  if (!g_prov_resource.targets) {
    st_print_log("[ST_MGR] g_prov_resource calloc Error\n");
    return;
  }

  st_specification_t *spec = st_data_mgr_get_spec_info();
  for (i = 0; i < target_size; i++) {
    oc_uuid_to_str(oc_core_get_device_id(device_index), uuid, OC_UUID_LEN);
    oc_new_string(&g_prov_resource.targets[i].target_di, uuid, strlen(uuid));
    oc_new_string(&g_prov_resource.targets[i].target_rt,
                  oc_string(spec->device.device_type),
                  oc_string_len(spec->device.device_type));
    g_prov_resource.targets[i].published = false;
  }
  g_prov_resource.targets_size = target_size;
  g_prov_resource.owned = false;
  oc_uuid_to_str(oc_core_get_device_id(device_index), uuid, OC_UUID_LEN);
  oc_new_string(&g_prov_resource.easysetup_di, uuid, strlen(uuid));

  if (set_sec_prov_info(&g_prov_resource) == ES_ERROR)
    st_print_log("[ST_MGR] SetProvInfo Error\n");

  st_print_log("[ST_MGR] set_sc_prov_info OUT\n");
}

static void
unset_sc_prov_info(void)
{
  // Come from  target_size in set_sc_prov_info
  int target_size = 1, i = 0;

  oc_free_string(&g_prov_resource.easysetup_di);
  for (i = 0; i < target_size; i++) {
    oc_free_string(&g_prov_resource.targets[i].target_di);
    oc_free_string(&g_prov_resource.targets[i].target_rt);
  }

  free(g_prov_resource.targets);
}

static void
st_vendor_props_initialize(void)
{
  memset(&st_vendor_props, 0, sizeof(sc_properties));
  st_specification_t  *specification = st_data_mgr_get_spec_info();
  if (!specification) {
    st_print_log("[ST_MGR] specification list not exist");
    return;
  }

  st_print_log("[ST_MGR] specification model no %s",oc_string(specification->platform.model_number));
  oc_new_string(&st_vendor_props.model, oc_string(specification->platform.model_number),
                oc_string_len(specification->platform.model_number));
}

static void
st_vendor_props_shutdown(void)
{
  oc_free_string(&st_vendor_props.model);
}

static void
st_main_reset(void)
{
#ifdef OC_SECURITY
  oc_sec_reset();
#endif /* OC_SECURITY */
  st_store_info_initialize();
  if (st_store_dump() <= 0) {
    st_print_log("[ST_MGR] st_store_dump failed.\n");
  }
}

static oc_event_callback_retval_t
status_callback(void *data)
{
  (void)data;
  if (g_st_status_cb)
    g_st_status_cb(g_main_status);

  return OC_EVENT_DONE;
}

static void
set_st_manager_status(st_status_t status)
{
  g_main_status = status;
  oc_set_delayed_callback(NULL, status_callback, 0);
  _oc_signal_event_loop();
}

static void
set_main_status_sync(st_status_t status)
{
  st_process_app_sync_lock();
  set_st_manager_status(status);
  st_process_app_sync_unlock();
}

st_error_t
st_manager_initialize(void)
{

  if (g_main_status != ST_STATUS_IDLE) {
    if (g_main_status == ST_STATUS_INIT) {
      return ST_ERROR_STACK_ALREADY_INITIALIZED;
    } else {
      return ST_ERROR_STACK_RUNNING;
    }
  }

#ifdef OC_SECURITY
#ifdef __TIZENRT__
  oc_storage_config("/mnt/st_things_creds");
#else
  oc_storage_config("./st_things_creds");
#endif
#endif /* OC_SECURITY */

  if (st_process_init() != 0) {
    st_print_log("[ST_MGR] st_process_init failed.\n");
    return ST_ERROR_OPERATION_FAILED;
  }

  if (st_port_specific_init() != 0) {
    st_print_log("[ST_MGR] st_port_specific_init failed!");
    st_process_destroy();
    return ST_ERROR_OPERATION_FAILED;
  }

  oc_set_max_app_data_size(3072);

  st_unregister_status_handler();
  set_main_status_sync(ST_STATUS_INIT);

  return ST_ERROR_NONE;
}

static int
st_manager_stack_init(void)
{
  static const oc_handler_t handler = {.init = app_init,
                                       .signal_event_loop = st_process_signal,
#ifdef OC_SERVER
                                       .register_resources = register_resources
#endif
  };

  if (st_store_load() < 0) {
    st_print_log("[ST_MGR] Could not load store informations.\n");
    return -1;
  }

  if (st_data_mgr_info_load() != 0) {
    st_print_log("[ST_MGR] st_data_mgr_info_load failed!\n");
    return -1;
  }

  st_vendor_props_initialize();

  if (st_is_easy_setup_finish() != 0) {
#ifndef WIFI_SCAN_IN_SOFT_AP_SUPPORTED
    st_wifi_ap_t *ap_list = NULL;
    st_wifi_scan(&ap_list);
    st_wifi_set_cache(ap_list);
#endif

    // Turn on soft-ap
    st_print_log("[ST_MGR] Soft AP turn on.\n");

    char ssid[MAX_SSID_LEN + 1];
    st_specification_t *spec = st_data_mgr_get_spec_info();
    if (st_gen_ssid(ssid, oc_string(spec->device.device_name),
                    oc_string(spec->platform.manufacturer_name),
                    oc_string(spec->platform.model_number)) != 0) {
      return -1;
    }
    st_turn_on_soft_AP(ssid, SOFT_AP_PWD, SOFT_AP_CHANNEL);
  }

  if (oc_main_init(&handler) != 0) {
    st_print_log("[ST_MGR] oc_main_init failed!\n");
    return -1;
  }

  char uuid[OC_UUID_LEN] = { 0 };
  oc_uuid_to_str(oc_core_get_device_id(0), uuid, OC_UUID_LEN);
  st_print_log("[ST_MGR] uuid : %s\n", uuid);

  set_sc_prov_info();
  st_fota_manager_start();
  st_data_mgr_info_free();

  int i = 0;
  int device_num = 0;
  device_num = oc_core_get_num_devices();
  for (i = 0; i < device_num; i++) {
    oc_endpoint_t *ep = oc_connectivity_get_endpoints(i);
    st_print_log("[ST_MGR] === device(%d) endpoint info. ===\n", i);
    while (ep) {
      oc_string_t ep_str;
      if (oc_endpoint_to_string(ep, &ep_str) == 0) {
        st_print_log("[ST_MGR] -> %s\n", oc_string(ep_str));
        oc_free_string(&ep_str);
      }
      ep = ep->next;
    }
  }

  if (st_process_start() != 0) {
    st_print_log("[ST_MGR] st_process_start failed.\n");
    return -1;
  }

  return 0;
}

st_error_t
st_manager_start(void)
{
  if (g_main_status == ST_STATUS_IDLE) {
    return ST_ERROR_STACK_NOT_INITIALIZED;
  } else if (g_main_status != ST_STATUS_INIT) {
    return ST_ERROR_STACK_RUNNING;
  }

  st_store_t *store_info = NULL;
  st_error_t st_err_ret = ST_ERROR_NONE;
  int conn_cnt = 0;
  int quit = 0;
  g_start_fail = false;

  while (quit != 1) {
    switch (g_main_status) {
    case ST_STATUS_INIT:
      st_process_app_sync_lock();
      if (st_manager_stack_init() < 0) {
        st_process_app_sync_unlock();
        EXIT_WITH_ERROR(ST_ERROR_OPERATION_FAILED);
      }
      store_info = NULL;
      set_st_manager_status(ST_STATUS_EASY_SETUP_START);
      st_process_app_sync_unlock();
      break;
    case ST_STATUS_EASY_SETUP_START:
      st_process_app_sync_lock();
      if (st_is_easy_setup_finish() == 0) {
        set_st_manager_status(ST_STATUS_EASY_SETUP_DONE);
      } else {
        if (st_easy_setup_start(&st_vendor_props, easy_setup_handler) != 0) {
          st_print_log("[ST_MGR] Failed to start easy setup!\n");
          st_process_app_sync_unlock();
          EXIT_WITH_ERROR(ST_ERROR_OPERATION_FAILED);
        }
        set_st_manager_status(ST_STATUS_EASY_SETUP_PROGRESSING);
      }
      st_process_app_sync_unlock();
      break;
    case ST_STATUS_EASY_SETUP_PROGRESSING:
    case ST_STATUS_CLOUD_MANAGER_PROGRESSING:
      st_sleep(1);
      st_print_log(".");
      fflush(stdout);
      break;
    case ST_STATUS_EASY_SETUP_DONE:
      st_print_log("\n");
      st_process_app_sync_lock();
      st_easy_setup_stop();
      store_info = st_store_get_info();
      if (!store_info || !store_info->status) {
        st_print_log("[ST_MGR] could not get cloud informations.\n");
        st_process_app_sync_unlock();
        EXIT_WITH_ERROR(ST_ERROR_OPERATION_FAILED);
      }
      set_st_manager_status(ST_STATUS_WIFI_CONNECTING);
      st_process_app_sync_unlock();
      break;
    case ST_STATUS_WIFI_CONNECTING:
      st_process_app_sync_lock();
      st_turn_off_soft_AP();
      st_connect_wifi(oc_string(store_info->accesspoint.ssid),
                      oc_string(store_info->accesspoint.pwd));
      set_st_manager_status(ST_STATUS_WIFI_CONNECTION_CHECKING);
      st_process_app_sync_unlock();
      break;
    case ST_STATUS_WIFI_CONNECTION_CHECKING:
      st_process_app_sync_lock();
      int ret =
        st_cloud_manager_check_connection(&store_info->cloudinfo.ci_server);
      st_process_app_sync_unlock();
      if (ret != 0) {
        st_print_log("[ST_MGR] AP is not connected.\n");
        conn_cnt++;
        if (conn_cnt > AP_CONNECT_RETRY_LIMIT) {
          conn_cnt = 0;
          set_main_status_sync(ST_STATUS_RESET);
        } else if (conn_cnt == (AP_CONNECT_RETRY_LIMIT >> 1)) {
          set_main_status_sync(ST_STATUS_WIFI_CONNECTING);
        }
        st_sleep(3);
      } else {
        conn_cnt = 0;
        set_main_status_sync(ST_STATUS_CLOUD_MANAGER_START);
      }
      break;
    case ST_STATUS_CLOUD_MANAGER_START:
      st_process_app_sync_lock();
      if (st_cloud_manager_start(store_info, device_index,
                                 cloud_manager_handler) != 0) {
        st_print_log("[ST_MGR] Failed to start cloud manager!\n");
        st_process_app_sync_unlock();
        EXIT_WITH_ERROR(ST_ERROR_OPERATION_FAILED);
      }
      set_st_manager_status(ST_STATUS_CLOUD_MANAGER_PROGRESSING);
      st_process_app_sync_unlock();
      break;
    case ST_STATUS_CLOUD_MANAGER_DONE:
      st_print_log("\n");
      set_main_status_sync(ST_STATUS_DONE);
      break;
    case ST_STATUS_DONE:
      st_sleep(1);
      break;
    case ST_STATUS_RESET:
      st_process_stop();
      st_process_app_sync_lock();
      st_manager_evt_reset_handler();
      set_st_manager_status(ST_STATUS_INIT);
      st_process_app_sync_unlock();
      break;
    case ST_STATUS_STOP:
      quit = 1;
      if (g_start_fail) {
        EXIT_WITH_ERROR(ST_ERROR_OPERATION_FAILED);
      }
      break;
    default:
      st_print_log("[ST_MGR] un-supported main step.\n");
      break;
    }
  }

exit:
  st_process_stop();
  st_process_app_sync_lock();
  st_manager_evt_stop_handler();
  g_main_status = ST_STATUS_INIT;
  st_process_app_sync_unlock();
  return st_err_ret;
}

st_error_t
st_manager_reset(void)
{
  if (g_main_status == ST_STATUS_IDLE)
    return ST_ERROR_STACK_NOT_INITIALIZED;

  st_process_stop();
  st_process_app_sync_lock();
  st_manager_evt_reset_handler();
  set_st_manager_status(ST_STATUS_INIT);
  st_process_app_sync_unlock();
  return ST_ERROR_NONE;
}

st_error_t
st_manager_stop(void)
{
  if (g_main_status == ST_STATUS_IDLE) {
    return ST_ERROR_STACK_NOT_INITIALIZED;
  } else if (g_main_status == ST_STATUS_INIT) {
    return ST_ERROR_STACK_NOT_STARTED;
  }
  set_main_status_sync(ST_STATUS_STOP);
  return ST_ERROR_NONE;
}

st_error_t
st_manager_deinitialize(void)
{
  if (g_main_status == ST_STATUS_IDLE) {
    return ST_ERROR_STACK_NOT_INITIALIZED;
  } else if (g_main_status != ST_STATUS_INIT) {
    return ST_ERROR_STACK_RUNNING;
  }

  st_free_device_profile();
  st_unregister_status_handler();
  st_unregister_otm_confirm_handler();
  st_turn_off_soft_AP();
  st_vendor_props_shutdown();
  st_port_specific_destroy();
  st_process_destroy();

  set_main_status_sync(ST_STATUS_IDLE);
  return ST_ERROR_NONE;
}

bool
st_register_otm_confirm_handler(st_otm_confirm_cb_t cb)
{
  if (!cb) {
    st_print_log("Failed to register otm confirm handler\n");
    return false;
  }

#ifdef OC_SECURITY
  oc_sec_set_owner_cb((oc_sec_change_owner_cb_t)cb);
  return true;
#else
  st_print_log("Un-secured build can't handle otm confirm\n");
  return false;
#endif
}

void
st_unregister_otm_confirm_handler(void)
{
#ifdef OC_SECURITY
  oc_sec_set_owner_cb(NULL);
#else
  st_print_log("Un-secured build can't handle otm confirm\n");
#endif
}

bool
st_register_status_handler(st_status_cb_t cb)
{
  if (!cb) {
    st_print_log("Failed to register status - invalid parameter\n");
    return false;
  }
  if (g_st_status_cb) {
    st_print_log("Failed to register status handler - already registered\n");
    return false;
  }

  g_st_status_cb = cb;
  return true;
}

void
st_unregister_status_handler(void)
{
  g_st_status_cb = NULL;
}

static void
st_manager_evt_stop_handler(void)
{
  unset_sc_prov_info();

  st_easy_setup_stop();
  st_print_log("[ST_MGR] easy setup stop done\n");

  st_cloud_manager_stop(device_index);
  st_print_log("[ST_MGR] cloud manager stop done\n");

  st_fota_manager_stop();
  st_print_log("[ST_MGR] fota manager stop done\n");

  st_store_info_initialize();

  deinit_provisioning_info_resource();

  oc_main_shutdown();

  free_platform_cb_data();
}

static void
st_manager_evt_reset_handler(void)
{
  st_main_reset();
  st_manager_evt_stop_handler();
  st_print_log("[ST_MGR] reset finished\n");
}
