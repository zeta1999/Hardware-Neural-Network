#include "utility.hpp"

int ceil_div(int num, int den)
{
	int i = 0;
	while(1) {
		if(num <= (den*i)) {
			return i;
		}
		i++;
	}
}

void send_code(char c)
{
	outbyte(c);
	outbyte('\n');
}
