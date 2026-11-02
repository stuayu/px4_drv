// misc_posix.c - POSIX implementation of request_firmware / release_firmware

#include "misc_posix.h"

#include <fcntl.h>
#include <sys/stat.h>

int request_firmware(const struct firmware **p, const char *name, struct device *dummy)
{
	int ret = 0;
	int fd = -1;
	struct stat st;
	struct firmware *fw;
	uint8_t *buf, *data;
	ssize_t remain, r;

	(void)dummy;

	fd = open(name, O_RDONLY);
	if (fd < 0) {
		ret = -ENOENT;
		goto fail;
	}

	if (fstat(fd, &st) < 0) {
		ret = -EIO;
		goto fail;
	}

	buf = (uint8_t *)malloc(sizeof(*fw) + (size_t)st.st_size);
	if (!buf) {
		ret = -ENOMEM;
		goto fail;
	}

	fw   = (struct firmware *)buf;
	data = buf + sizeof(*fw);

	remain = (ssize_t)st.st_size;
	while (remain > 0) {
		r = read(fd, data + (st.st_size - remain), (size_t)remain);
		if (r <= 0) {
			ret = -EIO;
			free(buf);
			goto fail;
		}
		remain -= r;
	}

	fw->size = (size_t)st.st_size;
	fw->data = data;
	*p = fw;

	close(fd);
	return 0;

fail:
	if (fd >= 0)
		close(fd);
	return ret;
}

void release_firmware(const struct firmware *fw)
{
	free((void *)fw);
}
