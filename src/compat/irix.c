#include "compat/irix.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

int
sgug_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	int n;

	if (size == 0)
		return 0;

	n = vsnprintf(buf, size, fmt, ap);

	/* n < 0 is IRIX signalling truncation; n >= size is C99 reporting the
	 * length it would have written. Both mean the output was clamped. */
	if (n < 0 || (size_t)n >= size) {
		buf[size - 1] = '\0';
		return (int)(size - 1);
	}
	return n;
}

int
sgug_snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = sgug_vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

static const char *const MONTHS[12] = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

sgug_time_t
sgug_now(void)
{
	return (sgug_time_t)time(NULL);
}

int64_t
sgug_monotonic_ms(void)
{
	struct timeval tv;

	/*
	 * IRIX 6.5 has clock_gettime but no CLOCK_MONOTONIC. Callers use this
	 * only for deltas over seconds-to-minutes, where a stepped wall clock
	 * costs at worst one extra retry.
	 */
	if (gettimeofday(&tv, NULL) != 0)
		return 0;
	return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Days since 1970-01-01 for a proleptic Gregorian date. Howard Hinnant's
 * days_from_civil, which avoids any dependence on libc timezone state. */
static int64_t
days_from_civil(int64_t y, unsigned m, unsigned d)
{
	int64_t era, yoe, doy, doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = y - era * 400;
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

sgug_time_t
sgug_parse_http_date(const char *s)
{
	char mon[4];
	int day, year, hh, mm, ss, i, month;

	if (s == NULL)
		return -1;

	/* Skip the day-of-week and comma; its value is redundant. */
	while (*s != '\0' && *s != ',')
		s++;
	if (*s != ',')
		return -1;
	s++;

	if (sscanf(s, " %d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6)
		return -1;

	month = -1;
	for (i = 0; i < 12; i++) {
		if (strcmp(mon, MONTHS[i]) == 0) {
			month = i + 1;
			break;
		}
	}
	if (month < 0 || day < 1 || day > 31 || hh > 23 || mm > 59 || ss > 60)
		return -1;

	return days_from_civil(year, (unsigned)month, (unsigned)day) * 86400 +
	    (sgug_time_t)hh * 3600 + (sgug_time_t)mm * 60 + ss;
}

void
sgug_format_iso8601(sgug_time_t t, char *out, size_t outlen)
{
	time_t narrowed;
	struct tm tm;

	if (outlen == 0)
		return;

	narrowed = (time_t)t;
	if (gmtime_r(&narrowed, &tm) == NULL) {
		out[0] = '\0';
		return;
	}

	/* Seven fractional digits, always zero. The service parses this with
	 * .NET's round-trip format and rejects other precisions. */
	sgug_snprintf(out, outlen, "%04d-%02d-%02dT%02d:%02d:%02d.0000000Z",
	    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
	    tm.tm_hour, tm.tm_min, tm.tm_sec);
}

#if defined(__sgi)

/*
 * OpenSSL's DSO module references this, but IRIX resolves it from rld at load
 * time, so it is undefined even inside libc.so.1 and no static link can satisfy
 * it. Only dlfcn_pathbyaddr() calls it, and that path is reached solely when
 * loading engines, which we never do.
 */
void *
_rld_new_interface(unsigned long op, ...)
{
	(void)op;
	return NULL;
}

size_t
strnlen(const char *s, size_t maxlen)
{
	size_t n = 0;

	while (n < maxlen && s[n] != '\0')
		n++;
	return n;
}

void *
memmem(const void *hay, size_t haylen, const void *needle, size_t needlelen)
{
	const unsigned char *h = hay;
	const unsigned char *n = needle;
	size_t i;

	if (needlelen == 0)
		return (void *)hay;
	if (haylen < needlelen)
		return NULL;

	for (i = 0; i + needlelen <= haylen; i++) {
		if (h[i] == n[0] && memcmp(h + i, n, needlelen) == 0)
			return (void *)(h + i);
	}
	return NULL;
}

char *
strcasestr(const char *hay, const char *needle)
{
	size_t nlen = strlen(needle);
	size_t i;

	if (nlen == 0)
		return (char *)hay;

	for (i = 0; hay[i] != '\0'; i++) {
		size_t j;

		for (j = 0; j < nlen; j++) {
			if (tolower((unsigned char)hay[i + j]) !=
			    tolower((unsigned char)needle[j]))
				break;
		}
		if (j == nlen)
			return (char *)(hay + i);
		if (hay[i + j] == '\0')
			break;
	}
	return NULL;
}

extern char **environ;

int
setenv(const char *name, const char *value, int overwrite)
{
	char *buf;
	size_t len;

	if (name == NULL || *name == '\0' || strchr(name, '=') != NULL)
		return -1;
	if (!overwrite && getenv(name) != NULL)
		return 0;

	len = strlen(name) + strlen(value) + 2;
	buf = malloc(len);
	if (buf == NULL)
		return -1;

	sgug_snprintf(buf, len, "%s=%s", name, value);

	/* putenv takes ownership of the storage, so this leaks one allocation
	 * per distinct name. The runner sets a bounded set of variables once
	 * per job, and the alternative is corrupting environ. */
	return putenv(buf);
}

int
unsetenv(const char *name)
{
	size_t namelen;
	char **src, **dst;

	if (name == NULL || *name == '\0' || strchr(name, '=') != NULL)
		return -1;

	namelen = strlen(name);
	for (src = dst = environ; *src != NULL; src++) {
		if (strncmp(*src, name, namelen) == 0 && (*src)[namelen] == '=')
			continue;
		*dst++ = *src;
	}
	*dst = NULL;
	return 0;
}

time_t
timegm(struct tm *tm)
{
	int64_t days = days_from_civil(tm->tm_year + 1900,
	    (unsigned)(tm->tm_mon + 1), (unsigned)tm->tm_mday);

	return (time_t)(days * 86400 + tm->tm_hour * 3600 +
	    tm->tm_min * 60 + tm->tm_sec);
}

char *
mkdtemp(char *tmpl)
{
	static const char SET[] =
	    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	size_t len = strlen(tmpl);
	int attempt;

	if (len < 6 || strcmp(tmpl + len - 6, "XXXXXX") != 0)
		return NULL;

	for (attempt = 0; attempt < 128; attempt++) {
		struct timeval tv;
		unsigned long seed;
		int i;

		gettimeofday(&tv, NULL);
		seed = (unsigned long)tv.tv_usec ^
		    ((unsigned long)getpid() << 16) ^ (unsigned long)attempt;

		for (i = 0; i < 6; i++) {
			seed = seed * 1103515245UL + 12345UL;
			tmpl[len - 6 + i] = SET[(seed >> 16) % (sizeof(SET) - 1)];
		}

		if (mkdir(tmpl, 0700) == 0)
			return tmpl;
	}
	return NULL;
}

void
explicit_bzero(void *p, size_t n)
{
	volatile unsigned char *q = p;

	while (n-- > 0)
		*q++ = 0;
}

#endif /* __sgi */
