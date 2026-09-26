#include "gpon_status.h"

int gpon_proc_parse_state(const char *buf, unsigned len, uint32_t *state)
{
	unsigned i;
	uint32_t v = 0;

	if (len < 7 || buf[0] != 's' || buf[1] != 't' || buf[2] != 'a' ||
	    buf[3] != 't' || buf[4] != 'e' || buf[5] != ' ')
		return -1;
	for (i = 6; i < len && buf[i] >= '0' && buf[i] <= '9'; i++)
		v = v * 10 + (uint32_t)(buf[i] - '0');
	if (i == 6 || v > 7)
		return -1;
	*state = v;
	return 0;
}

int gpon_proc_parse_los(const char *buf, unsigned len, uint32_t *los)
{
	static const char key[] = "last_los_ms ";
	unsigned i = 0;

	while (i < len) {
		unsigned k = 0, j;

		while (key[k] && i + k < len && buf[i + k] == key[k])
			k++;
		if (!key[k]) {
			j = i + k;
			if (j >= len || buf[j] < '0' || buf[j] > '9')
				return -1;
			while (j < len && buf[j] >= '0' && buf[j] <= '9')
				j++;
			if (j + 8 > len || buf[j] != ' ' || buf[j + 1] != 's' ||
			    buf[j + 2] != 't' || buf[j + 3] != 'a' ||
			    buf[j + 4] != 't' || buf[j + 5] != 'e' ||
			    buf[j + 6] != ' ')
				return -1;
			j += 7;
			if (buf[j] != '0' && buf[j] != '1')
				return -1;
			if (j + 1 < len && buf[j + 1] != '\n')
				return -1;
			*los = (uint32_t)(buf[j] - '0');
			return 0;
		}
		while (i < len && buf[i] != '\n')
			i++;
		i++;
	}
	return -1;
}
