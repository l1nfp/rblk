#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
int main(void) {
	char *str;
	str =(char*) malloc(50*1024);
	for (int i=0;i<50;i++){
		for (int j=0;j<1024;j++){
			str[i*1024+j] = 'a' + (i%26);
		}
		printf("write 1024 %c\n",'a'+(i%26));
	}
	sleep(300);
	return 0;
}
