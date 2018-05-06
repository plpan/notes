#include <stdio.h>

int main() {
	int my_var = 10;
	int my_int = 5;
	__asm__ __volatile__(
		"   lock       ;\n"
		"   addl %1,%0 ;\n"
		: "=m"  (my_var)
		: "ir"  (my_int), "m" (my_var)
		: /* no clobber-list */
		);
	printf("my_var=%d\n", my_var);
	return 0;
}