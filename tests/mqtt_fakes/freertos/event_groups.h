#pragma once

#include "FreeRTOS.h"

typedef struct fake_event_group *EventGroupHandle_t;

EventGroupHandle_t xEventGroupCreate(void);
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                               BaseType_t clear_on_exit,
                               BaseType_t wait_for_all,
                               TickType_t ticks);

