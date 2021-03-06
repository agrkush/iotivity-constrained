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

#ifndef ST_PROCESS_H
#define ST_PROCESS_H

#include "st_port.h"

int st_process_init(void);
int st_process_start(void);
int st_process_stop(void);
int st_process_destroy(void);

void st_process_signal(void);
void st_process_app_sync_lock(void);
void st_process_app_sync_unlock(void);
#endif /* ST_PROCESS_H */