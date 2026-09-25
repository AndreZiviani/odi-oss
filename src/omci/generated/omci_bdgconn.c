/* Hand-written, not generated: command 51 takes a 160-byte structure the
 * extractor could not size from the binary, so tools/omci-drv-abi.py emits
 * nothing for it -- see the commented-out declaration in omci_drv.h.
 *
 * Only activeBdgConn is here. deactiveBdgConn takes a single word, so the
 * extractor did emit it, and defining it again here is a duplicate symbol.
 * See ../omci_bdgconn.h for the structure. */
#include "../omci_bdgconn.h"
#include "omci_drv.h"

int omci_activeBdgConn(void *desc160)
{
	return omci_drv_call(OMCI_BDGCONN_CMD, desc160, OMCI_BDGCONN_LEN);
}
