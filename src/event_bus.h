#ifndef LE_EVENT_BUS_H
#define LE_EVENT_BUS_H
#include <stddef.h>
#define LE_EVENT_COUNT 64
struct le_event{unsigned long id;char type[20];char data[256];}; struct le_event_bus{struct le_event items[LE_EVENT_COUNT];unsigned long next;size_t count;};
void event_bus_init(struct le_event_bus*);void event_bus_publish(struct le_event_bus*,const char*,const char*);size_t event_bus_since(struct le_event_bus*,unsigned long,struct le_event*,size_t);
#endif
