#include <stdarg.h>
#include "guest-util.h"

/* Only understands %u, %s */
void printf(const char *fmt, ...)
{
	va_list ap;
	unsigned val;
	char intbuf[20], *p;

	va_start(ap, fmt);
	while (*fmt) {
		if (*fmt != '%') {
			putc(*(fmt++));
			continue;
		}
		fmt++;
		switch (*fmt) {
		case 'u':
			fmt++;
			val = va_arg(ap, int);
			if (!val) {
				putc('0');
				continue;
			}
			p = &intbuf[19];
			*(p--) = '\0';
			while (val) {
				*(p--) = (val % 10) + '0';
				val /= 10;
			}
			print(p+1);
			break;
		case 's':
			fmt++;
			p = va_arg(ap, char *);
			print(p);
			break;
		default:
			putc('%');
			continue;
		}
	}
	va_end(ap);
}
