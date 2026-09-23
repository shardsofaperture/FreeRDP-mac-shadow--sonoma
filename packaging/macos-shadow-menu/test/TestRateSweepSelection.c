#include "../RateSweepSelection.h"
#include <stdio.h>

int main(void)
{
	static const struct
	{
		const char* argument;
		unsigned rate;
		unsigned bytesPerSecond;
	} cases[] = {
	    { "--rate-sweep-kib=150", 150, 153600 },
	    { "--rate-sweep-kib=200", 200, 204800 },
	    { "--rate-sweep-kib=250", 250, 256000 },
	    { "--rate-sweep-kib=300", 300, 307200 },
	    { "--rate-sweep-kib=400", 400, 409600 },
	};
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
	{
		const unsigned selected = shadow_rate_sweep_select(cases[i].argument);
		if (selected != cases[i].rate || selected * 1024U != cases[i].bytesPerSecond)
			return 1;
		printf("%u KiB/s = %u B/s\n", selected, selected * 1024U);
	}
	static const char* invalid[] = { NULL, "", "--rate-sweep-kib=0", "--rate-sweep-kib=175",
	                                 "--rate-sweep-kib=350", "--rate-sweep-kib=401",
	                                 "--rate-sweep-kib=200x", "--rate-sweep-kib=0200",
	                                 "--rate-sweep-kib=200 --help", "--other=200" };
	for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
	{
		if (shadow_rate_sweep_select(invalid[i]) != 0)
			return 2;
	}
	return 0;
}
