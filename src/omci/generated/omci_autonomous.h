#ifndef OMCI_AUTONOMOUS_H
#define OMCI_AUTONOMOUS_H

#include <stdint.h>

struct omci_instance {
	uint16_t classId;
	uint16_t inst;
};

extern const struct omci_instance omci_autonomous[];
extern const unsigned omci_autonomous_count;

#endif
