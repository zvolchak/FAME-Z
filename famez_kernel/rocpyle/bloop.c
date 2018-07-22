// gcc -Werror -o bloop.exe bloop.c

// bloop "5:01234567890123456789" /dev/wherever

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

int keepgoing = 1;

void stopit(int signum)
{
	keepgoing = 0;
}

int main(int argc, char *argv[])
{
	int fd, ret, w, mult, len, target;
	char gbuf[300], *dev;
	time_t started;

	if (argc != 4) {
		fprintf(stderr, "usage: %s target mult dev\n", argv[0]);
		exit(1);
	}

	target = strtoul(argv[1], NULL, 10);
	mult = strtoul(argv[2], NULL, 10);
	dev = argv[3];

	if (mult > 18) {
		printf("Multipler capped at 18\n");
		mult = 18;
	}
	memset(gbuf, 0, sizeof(gbuf));
	sprintf(gbuf, "%d:", target);
	for (w = 0; w < mult; w++) strcat(gbuf, "0123456789");
	len = strlen(gbuf);
	printf("Emitting %d bytes \"%s\"\n", len, gbuf);

	signal(SIGINT, stopit);
	w = 0;

	if ((fd = open(dev, O_WRONLY | O_CREAT | O_TRUNC, 0666)) == -1) {
		perror("open failed");
		exit(1);
	}

	started = time(NULL);
	while (keepgoing) {
		if (write(fd, gbuf, len) != len) {
			perror("write failed");
			exit(1);
		}
		w++;
	}
	started = time(NULL) - started;
	len = mult * 10;
	printf("%lu writes, %lu bytes in %lu secs = %lu w/s, %lu b/s\n",
		w, w * len, started, w / started, w * len / started);
	exit(0);
}
