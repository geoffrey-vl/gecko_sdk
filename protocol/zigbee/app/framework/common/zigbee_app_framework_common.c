/***************************************************************************//**
 * @brief ZigBee Application Framework common code.
 *******************************************************************************
 * # License
 * <b>Copyright 2020 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * The licensor of this software is Silicon Laboratories Inc. Your use of this
 * software is governed by the terms of Silicon Labs Master Software License
 * Agreement (MSLA) available at
 * www.silabs.com/about-us/legal/master-software-license-agreement. This
 * software is distributed to you in Source Code format and is governed by the
 * sections of the MSLA applicable to Source Code.
 *
 ******************************************************************************/

#include PLATFORM_HEADER
#include "hal.h"
#include "stack/include/ember.h"
#include "zigbee_app_framework_common.h"
#include "zigbee_app_framework_callback.h"

#ifdef SL_COMPONENT_CATALOG_PRESENT
#include "sl_component_catalog.h"
#endif

#if (defined(SL_CATALOG_ZIGBEE_ZCL_FRAMEWORK_CORE_PRESENT) || (defined(EMBER_SCRIPTED_TEST))) \
  || (defined(EMBER_AF_NCP) && defined(SL_CATALOG_ZIGBEE_AF_SUPPORT_PRESENT))
extern EmberStatus emberAfPushNetworkIndex(uint8_t networkIndex);
extern EmberStatus emberAfPushEndpointNetworkIndex(uint8_t endpoint);
extern EmberStatus emberAfPopNetworkIndex(void);
extern uint8_t emberAfIndexFromEndpoint(uint8_t endpoint);
#else
#define emberAfPushNetworkIndex(networkIndex)
#define emberAfPushEndpointNetworkIndex(endpoint)
#define emberAfPopNetworkIndex()
#define emberAfIndexFromEndpoint(endpoint) (0xFF)
#endif

extern void emberAfMainInitCallback(void);
extern void emberAfMainTickCallback(void);

EmberEventQueue sli_zigbee_af_app_event_queue;
#if defined(SL_CATALOG_KERNEL_PRESENT)
osMutexId_t app_event_mutex_id;
static const osMutexAttr_t app_event_mutex_attr = {
  .name = "Zigbee App Framework Mutex",
  .attr_bits = osMutexRecursive | osMutexPrioInherit,
};
#endif // defined(SL_CATALOG_KERNEL_PRESENT)

//------------------------------------------------------------------------------
// Init and tick callbacks

#ifndef EZSP_HOST
void sli_zigbee_stack_init_callback(void)
{
  EmberStatus status;

  // Initialize the radio and the stack.  If this fails, we have to assert
  // because something is wrong.
  status = emberInit();
  assert(status == EMBER_SUCCESS);
}
#endif // EZSP_HOST

extern void sli_zigbee_af_event_init(void);
extern void sli_zigbee_af_local_data_init(void);
extern void sli_zigbee_af_initDone(void);

#if (defined(SL_CATALOG_ZIGBEE_DEBUG_PRINT_PRESENT) && !defined(EMBER_TEST) && !defined(EZSP_HOST) && !defined(PRO_COMPLIANCE))
#define EXTENDED_RESET_INFO
#include "cortexm3/diagnostic.h"
#include "sl_zigbee_debug_print.h"
#endif // (defined(SL_CATALOG_ZIGBEE_DEBUG_PRINT_PRESENT) && !defined(EMBER_TEST) && !defined(EZSP_HOST) && !defined(PRO_COMPLIANCE))

void sli_zigbee_app_framework_init_callback(void)
{
  // Init the event queue.
  sli_zigbee_initialize_event_queue(&sli_zigbee_af_app_event_queue);

#if defined(SL_CATALOG_KERNEL_PRESENT)
  // Create mutex for Zigbee App Framework Event
  app_event_mutex_id = osMutexNew(&app_event_mutex_attr);
  if (app_event_mutex_id == NULL) {
    // Unable to create mutex
    assert(0);
  }
#endif // defined(SL_CATALOG_KERNEL_PRESENT)

  // Call the application init callback.
  emberAfMainInitCallback();

  // Call the generated init callbacks.
  sli_zigbee_af_event_init();
  sli_zigbee_af_local_data_init();
  sli_zigbee_af_initDone();

#if defined(EXTENDED_RESET_INFO)
  #ifndef SL_CATALOG_ZIGBEE_ZCL_FRAMEWORK_CORE_PRESENT
  sl_zigbee_app_debug_println("Reset info: 0x%x (%s)",
                              halGetResetInfo(),
                              halGetResetString());
  #endif //SL_CATALOG_ZIGBEE_ZCL_FRAMEWORK_CORE_PRESENT

  sl_zigbee_app_debug_println("Extended Reset info: 0x%2X (%s)",
                              halGetExtendedResetInfo(),
                              halGetExtendedResetString());

  if (halResetWasCrash()) {
    // We pass port 0 here though this parameter is unused in the legacy HAL
    // version of the diagnostic code.
    halPrintCrashSummary(0);
    halPrintCrashDetails(0);
    halPrintCrashData(0);
  }
#endif // EXTENDED_RESET_INFO
}

#ifndef EZSP_HOST
void sli_zigbee_stack_tick_callback(void)
{
  // Pet the watchdog.
  halResetWatchdog();
  // Call the stack tick API.
  emberTick();
}
#endif // EZSP_HOST

extern void sli_zigbee_af_tick(void);

void sli_zigbee_app_framework_tick_callback(void)
{
  // Pet the watchdog.
  halResetWatchdog();

  // Call the application tick callback.
  emberAfMainTickCallback();

  // Call the generated tick callback (tick the components that subscribed to it).
  sli_zigbee_af_tick();

  // Run the application event queue.
  sli_zigbee_af_run_events();
}

//------------------------------------------------------------------------------
// Event System

static void event_common_handler(sl_zigbee_event_t *event)
{
  bool is_network_event = sli_zigbee_event_is_network_event(event);
  bool is_endpoint_event = sli_zigbee_event_is_endpoint_event(event);

  assert(!(is_network_event && is_endpoint_event));

  if (is_network_event) {
    emberAfPushNetworkIndex(sli_zigbee_event_get_network_index(event));
  } else if (is_endpoint_event) {
    emberAfPushEndpointNetworkIndex(sli_zigbee_event_get_endpoint(event));
  }

  assert(event->dataPtr);

  emberEventSetInactive(event);

  // Endpoint events pass the endpoint to the handler, other events pass the
  // event itself.
  // TODO: we should clean this up by always pass the event end let the code
  // in the handler retrieve the endpoint.
  if (is_endpoint_event) {
    ((void (*)(uint8_t))(event->dataPtr))(sli_zigbee_event_get_endpoint(event));
  } else {
    ((void (*)(sl_zigbee_event_t *))(event->dataPtr))(event);
  }

  if (is_network_event || is_endpoint_event) {
    emberAfPopNetworkIndex();
  }
}

void sli_zigbee_event_init(sl_zigbee_event_t *event,
                           const char *event_name,
                           void *handler,
                           uint8_t network_index,
                           uint8_t endpoint)
{
  // We do not support events that are multiplexed by network and endpoint
  // simultaneously.
  assert(!(network_index < 0xFF && endpoint < 0xFF));

#ifdef SL_CATALOG_KERNEL_PRESENT
  osStatus_t ret = osMutexAcquire(app_event_mutex_id, osWaitForever);
  assert(ret == osOK);
#endif // SL_CATALOG_KERNEL_PRESENT
  event->next = NULL;
  event->actions.queue = &sli_zigbee_af_app_event_queue;
  event->actions.handler = event_common_handler;
  event->actions.marker = NULL;
  event->dataPtr = handler;
  event->data = 0;
  event->actions.name = event_name;

  if (network_index < 0xFF) {
    sli_zigbee_event_set_network_event(event);
    sli_zigbee_event_set_network_index(event, network_index);
  }
  if (endpoint < 0xFF) {
    sli_zigbee_event_set_endpoint_event(event);
    sli_zigbee_event_set_endpoint(event, endpoint);
  }
#ifdef SL_CATALOG_KERNEL_PRESENT
  ret = osMutexRelease(app_event_mutex_id);
  assert(ret == osOK);
#endif // SL_CATALOG_KERNEL_PRESENT
}

void sli_zigbee_network_event_init(sl_zigbee_event_t *event,
                                   const char *event_name,
                                   void (*handler)(sl_zigbee_event_t *))
{
  uint8_t i;
  for (i = 0; i < EMBER_SUPPORTED_NETWORKS; i++) {
    sli_zigbee_event_init(event, event_name, (void*)handler, i, 0xFF);
    event++;
  }
}

sl_zigbee_event_t* sli_zigbee_get_event_ptr(sl_zigbee_event_t *event,
                                            uint8_t endpoint)
{
  bool is_network_event = sli_zigbee_event_is_network_event(event);
  bool is_endpoint_event = sli_zigbee_event_is_endpoint_event(event);

  // We do not support events that are multiplexed by network and endpoint
  // simultaneously.
  assert(!(is_network_event && is_endpoint_event));

  if (endpoint < 0xFF) {
    // If we are passing a valid endpoint to this API, we enforce that the event
    // is actually an endpoint event.
    assert(is_endpoint_event);

    uint8_t endpoint_index = emberAfIndexFromEndpoint(endpoint);
    assert(endpoint_index < 0xFF);
    return event + endpoint_index;
  }

  if (is_network_event) {
#if EMBER_SUPPORTED_NETWORKS == 1
    // We enforce that the network-0 event is the one passed in.
    assert(sli_zigbee_event_get_network_index(event) == 0);
#endif // EMBER_SUPPORTED_NETWORKS == 1
    event = event + emberGetCurrentNetwork();
  }

  return event;
}

// Event initialisation routine for the event that gets activated from ISRs.
void sl_zigbee_af_isr_event_init(sl_zigbee_event_t *event,
                                 void (*handler)(sl_zigbee_event_t *))
{
  sl_zigbee_event_init(event, handler);
  event->actions.marker = sli_zigbee_isr_event_marker;
}

void sli_zigbee_af_event_set_delay_ms(sl_zigbee_event_t *event, uint8_t endpoint, uint32_t delay)
{
#ifdef SL_CATALOG_KERNEL_PRESENT
  osStatus_t ret;
  if (event->actions.marker != &sli_zigbee_isr_event_marker) {
    // ISR event doesn't need mutex protection
    ret = osMutexAcquire(app_event_mutex_id, osWaitForever);
    assert(ret == osOK);
  }
#endif // SL_CATALOG_KERNEL_PRESENT
  emberEventSetDelayMs(sli_zigbee_get_event_ptr(event, endpoint), delay);
#ifdef SL_CATALOG_KERNEL_PRESENT
  if (event->actions.marker != &sli_zigbee_isr_event_marker) {
    ret = osMutexRelease(app_event_mutex_id);
    assert(ret == osOK);
  }
#endif // SL_CATALOG_KERNEL_PRESENT
}

void sli_zigbee_af_event_set_active(sl_zigbee_event_t *event, uint8_t endpoint)
{
#ifdef SL_CATALOG_KERNEL_PRESENT
  osStatus_t ret;
  if (event->actions.marker != &sli_zigbee_isr_event_marker) {
    // ISR event doesn't need mutex protection
    ret = osMutexAcquire(app_event_mutex_id, osWaitForever);
    assert(ret == osOK);
  }
#endif // SL_CATALOG_KERNEL_PRESENT
  emberEventSetActive(sli_zigbee_get_event_ptr(event, endpoint));
#ifdef SL_CATALOG_KERNEL_PRESENT
  if (event->actions.marker != &sli_zigbee_isr_event_marker) {
    ret = osMutexRelease(app_event_mutex_id);
    assert(ret == osOK);
  }
#endif // SL_CATALOG_KERNEL_PRESENT
}

void sli_zigbee_af_event_set_inactive(sl_zigbee_event_t *event, uint8_t endpoint)
{
#ifdef SL_CATALOG_KERNEL_PRESENT
  osStatus_t ret;
  if (event->actions.marker != &sli_zigbee_isr_event_marker) {
    // ISR event doesn't need mutex protection
    ret = osMutexAcquire(app_event_mutex_id, osWaitForever);
    assert(ret == osOK);
  }
#endif // SL_CATALOG_KERNEL_PRESENT
  emberEventSetInactive(sli_zigbee_get_event_ptr(event, endpoint));
#ifdef SL_CATALOG_KERNEL_PRESENT
  if (event->actions.marker != &sli_zigbee_isr_event_marker) {
    ret = osMutexRelease(app_event_mutex_id);
    assert(ret == osOK);
  }
#endif // SL_CATALOG_KERNEL_PRESENT
}

bool sli_zigbee_af_event_is_scheduled(sl_zigbee_event_t *event, uint8_t endpoint)
{
  bool result = false;
#ifdef SL_CATALOG_KERNEL_PRESENT
  osStatus_t ret;
  if (event->actions.marker != &sli_zigbee_isr_event_marker) {
    // ISR event doesn't need mutex protection
    ret = osMutexAcquire(app_event_mutex_id, osWaitForever);
    assert(ret == osOK);
  }
#endif // SL_CATALOG_KERNEL_PRESENT
  result = emberEventIsScheduled(sli_zigbee_get_event_ptr(event, endpoint));
#ifdef SL_CATALOG_KERNEL_PRESENT
  if (event->actions.marker != &sli_zigbee_isr_event_marker) {
    ret = osMutexRelease(app_event_mutex_id);
    assert(ret == osOK);
  }
#endif // SL_CATALOG_KERNEL_PRESENT
  return result;
}

uint32_t sli_zigbee_af_event_get_remaining_ms(sl_zigbee_event_t *event, uint8_t endpoint)
{
  uint32_t remaining_ms = 0;
#ifdef SL_CATALOG_KERNEL_PRESENT
  osStatus_t ret;
  if (event->actions.marker != &sli_zigbee_isr_event_marker) {
    // ISR event doesn't need mutex protection
    ret = osMutexAcquire(app_event_mutex_id, osWaitForever);
    assert(ret == osOK);
  }
#endif // SL_CATALOG_KERNEL_PRESENT
  remaining_ms = emberEventGetRemainingMs(sli_zigbee_get_event_ptr(event, endpoint));
#ifdef SL_CATALOG_KERNEL_PRESENT
  if (event->actions.marker != &sli_zigbee_isr_event_marker) {
    ret = osMutexRelease(app_event_mutex_id);
    assert(ret == osOK);
  }
#endif // SL_CATALOG_KERNEL_PRESENT
  return remaining_ms;
}

uint32_t sli_zigbee_af_ms_to_next_event()
{
  uint32_t next_ms = 0;
#ifdef SL_CATALOG_KERNEL_PRESENT
  osStatus_t ret = osMutexAcquire(app_event_mutex_id, osWaitForever);
  assert(ret == osOK);
#endif // SL_CATALOG_KERNEL_PRESENT
  next_ms = emberMsToNextQueueEvent(&sli_zigbee_af_app_event_queue);
#ifdef SL_CATALOG_KERNEL_PRESENT
  ret = osMutexRelease(app_event_mutex_id);
  assert(ret == osOK);
#endif // SL_CATALOG_KERNEL_PRESENT
  return next_ms;
}
void sli_zigbee_af_run_events()
{
#ifdef SL_CATALOG_KERNEL_PRESENT
  osStatus_t ret = osMutexAcquire(app_event_mutex_id, osWaitForever);
  assert(ret == osOK);
#endif // SL_CATALOG_KERNEL_PRESENT
  emberRunEventQueue(&sli_zigbee_af_app_event_queue);
#ifdef SL_CATALOG_KERNEL_PRESENT
  ret = osMutexRelease(app_event_mutex_id);
  assert(ret == osOK);
#endif // SL_CATALOG_KERNEL_PRESENT
}

//------------------------------------------------------------------------------
// Callbacks stubs

WEAK(void emberAfMainInitCallback(void))
{
}

WEAK(void emberAfMainTickCallback(void))
{
}
