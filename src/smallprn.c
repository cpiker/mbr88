/* smallprn.c -- Minimal printf/fprintf/sprintf for 16-bit ELKS target
 *   to support mbrpatch.c
 *
 * MIT License -- Copyright (c) 2026 Chris Piker
 *
 *   Permission is hereby granted, free of charge, to any person obtaining
 *   a copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to
 *   permit persons to whom the Software is furnished to do so, subject to
 *   the following conditions:
 *
 *   The above copyright notice and this permission notice shall be
 *   included in all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 *   BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 *   ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 *   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

/* Supported format specifiers (sufficient for mbrpatch on ELKS):
 *
 *   %c            single character
 *   %s            null-terminated string
 *   %d            signed int (decimal)
 *   %u            unsigned int (decimal)
 *   %x            unsigned int (lowercase hex)
 *   %X            unsigned int (uppercase hex)
 *   %ld           signed long (decimal)
 *   %lu           unsigned long (decimal)
 *   %lx           unsigned long (lowercase hex)
 *   %lX           unsigned long (uppercase hex)
 *
 * Width and flags:
 *   [-]           left-justify within field
 *   [0]           zero-pad (numeric fields only; right-justified)
 *   [1-9][0-9]*   minimum field width
 *   [.][0-9]+     maximum string length (precision, %s only)
 *
 * Deliberately omitted to keep code and stack small:
 *   %f %e %g      no floating point -- not needed and pulls in FP library
 *   %*            no runtime-width arguments -- use fixed widths at call site
 *   %n            never safe, never needed
 *   %p            pointer -- add if needed, trivially derived from %lx
 *
 * Output is written character-by-character via a sink function pointer,
 * making the same core routine serve printf (stdout), fprintf (any FILE*),
 * and sprintf/snprintf (buffer).  No heap allocation is used anywhere.
 * The largest stack frame is _fmt_core: two char[12] buffers for number
 * conversion plus a handful of int-sized locals, well under 64 bytes total.
 *
 * On 8088/8086, 'int' and 'unsigned' are 16-bit; 'long' is 32-bit.
 * unsigned long arithmetic is used throughout to handle both widths cleanly.
 */

#ifdef __IA16__   /* Only compiled in for the ELKS cross-build */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>   /* write(), STDOUT_FILENO, STDERR_FILENO */

/* -------------------------------------------------------------------------
 * Sink abstraction -- routes formatted characters to their destination
 * -----------------------------------------------------------------------*/

/* Every output path is described by one of these.
 * fn   is called once per character; ctx is fn's private state pointer.
 * For sprintf/snprintf, ctx points at a mini_snprintf_ctx (below).
 * For printf/fprintf, ctx points at the FILE* and fn calls fputc(). */
typedef struct {
	void (*fn)(char c, void *ctx);
	void  *ctx;
} mini_sink;

/* FILE* sink -- writes one char to a stdio FILE */
static void sink_file(char c, void *ctx)
{
	fputc((unsigned char)c, (FILE *)ctx);
}

/* snprintf sink state */
typedef struct {
	char  *buf;     /* current write position */
	int    rem;     /* bytes remaining (excludes null terminator slot) */
} mini_snp_ctx;

/* snprintf sink -- writes into a bounded buffer, always null-terminates */
static void sink_buf(char c, void *ctx)
{
	mini_snp_ctx *s = (mini_snp_ctx *)ctx;
	if (s->rem > 0) {
		*s->buf++ = c;
		s->rem--;
		*s->buf = '\0';   /* keep null terminator current */
	}
}

/* -------------------------------------------------------------------------
 * Number-to-string conversion helpers (no libc dependency)
 * -----------------------------------------------------------------------*/

/* Maximum characters needed for a base-10 unsigned long on 16-bit:
 * 2^32-1 = 4294967295 = 10 digits.  +1 for the terminator. */
#define NUMBUFSZ 12

/* Convert unsigned long to ASCII in the given base (8, 10, or 16).
 * Writes into buf[] (must be NUMBUFSZ bytes) right-justified from the end,
 * returns a pointer to the first digit within buf.
 * upper: non-zero selects A-F rather than a-f for hex. */
static char *ulong_to_str(unsigned long v, int base, int upper, char buf[NUMBUFSZ])
{
	static const char lo[] = "0123456789abcdef";
	static const char hi[] = "0123456789ABCDEF";
	const char *digits = upper ? hi : lo;
	char *p = buf + NUMBUFSZ - 1;

	*p = '\0';
	do {
		*--p = digits[v % (unsigned)base];
		v   /= (unsigned)base;
	} while (v);

	return p;
}

/* -------------------------------------------------------------------------
 * Core formatter
 * -----------------------------------------------------------------------*/

static void _fmt_core(mini_sink *sink, const char *fmt, va_list ap)
{
	char nbuf[NUMBUFSZ];    /* number conversion workspace */
	char padbuf[NUMBUFSZ];  /* padding workspace (zero-pad string) */

	for (; *fmt; fmt++) {

		/* Pass through ordinary characters directly */
		if (*fmt != '%') {
			sink->fn(*fmt, sink->ctx);
			continue;
		}

		fmt++;   /* step past '%' */

		/* --- Parse flags --- */
		int left_just = 0;
		int zero_pad  = 0;

		if (*fmt == '-') { left_just = 1; fmt++; }
		if (*fmt == '0') { zero_pad  = 1; fmt++; }

		/* --- Parse minimum field width --- */
		int width = 0;
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');

		/* --- Parse precision (for %s: maximum chars to print) --- */
		int prec     = -1;   /* -1 = no precision specified */
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			while (*fmt >= '0' && *fmt <= '9')
				prec = prec * 10 + (*fmt++ - '0');
		}

		/* --- Parse length modifier --- */
		int is_long = 0;
		if (*fmt == 'l') { is_long = 1; fmt++; }

		/* --- Dispatch on conversion character --- */
		char       *str;
		int         slen;
		int         pad;
		int         i;
		unsigned long uval;
		long          sval;

		switch (*fmt) {

		case 'c':
			/* Single character; width still honoured */
			{
				char ch = (char)va_arg(ap, int);
				if (!left_just && width > 1) {
					for (i = 1; i < width; i++)
						sink->fn(' ', sink->ctx);
				}
				sink->fn(ch, sink->ctx);
				if (left_just && width > 1) {
					for (i = 1; i < width; i++)
						sink->fn(' ', sink->ctx);
				}
			}
			break;

		case 's':
			str  = va_arg(ap, char *);
			if (!str) str = "(null)";
			slen = (int)strlen(str);
			/* Precision caps the string length */
			if (prec >= 0 && slen > prec) slen = prec;
			pad = width - slen;
			if (pad < 0) pad = 0;
			if (!left_just)
				for (i = 0; i < pad; i++) sink->fn(' ', sink->ctx);
			for (i = 0; i < slen; i++) sink->fn(str[i], sink->ctx);
			if (left_just)
				for (i = 0; i < pad; i++) sink->fn(' ', sink->ctx);
			break;

		case 'd':
			/* Signed decimal */
			sval = is_long ? va_arg(ap, long) : (long)va_arg(ap, int);
			{
				int neg = (sval < 0);
				uval = neg ? (unsigned long)(-sval) : (unsigned long)sval;
				str  = ulong_to_str(uval, 10, 0, nbuf);
				slen = (int)strlen(str);
				pad  = width - slen - (neg ? 1 : 0);
				if (pad < 0) pad = 0;
				if (!left_just) {
					if (zero_pad) {
						if (neg) sink->fn('-', sink->ctx);
						for (i = 0; i < pad; i++) sink->fn('0', sink->ctx);
					} else {
						for (i = 0; i < pad; i++) sink->fn(' ', sink->ctx);
						if (neg) sink->fn('-', sink->ctx);
					}
				} else {
					if (neg) sink->fn('-', sink->ctx);
				}
				for (i = 0; i < slen; i++) sink->fn(str[i], sink->ctx);
				if (left_just)
					for (i = 0; i < pad; i++) sink->fn(' ', sink->ctx);
			}
			break;

		case 'u':
			uval = is_long ? va_arg(ap, unsigned long)
			               : (unsigned long)va_arg(ap, unsigned int);
			goto emit_unsigned_decimal;

		case 'x':
		case 'X':
			uval = is_long ? va_arg(ap, unsigned long)
			               : (unsigned long)va_arg(ap, unsigned int);
			{
				int upper = (*fmt == 'X');
				str  = ulong_to_str(uval, 16, upper, nbuf);
				slen = (int)strlen(str);
				pad  = width - slen;
				if (pad < 0) pad = 0;
				if (!left_just) {
					char pc = zero_pad ? '0' : ' ';
					for (i = 0; i < pad; i++) sink->fn(pc, sink->ctx);
				}
				for (i = 0; i < slen; i++) sink->fn(str[i], sink->ctx);
				if (left_just)
					for (i = 0; i < pad; i++) sink->fn(' ', sink->ctx);
			}
			break;

		case '%':
			sink->fn('%', sink->ctx);
			break;

		default:
			/* Unknown specifier -- emit as-is so bugs are visible */
			sink->fn('%', sink->ctx);
			sink->fn(*fmt, sink->ctx);
			break;
		}

		/* Shared unsigned decimal path (jumped to from 'u' case) */
		if (0) {
emit_unsigned_decimal:
			str  = ulong_to_str(uval, 10, 0, nbuf);
			slen = (int)strlen(str);
			pad  = width - slen;
			if (pad < 0) pad = 0;
			if (!left_just) {
				char pc = zero_pad ? '0' : ' ';
				for (i = 0; i < pad; i++) sink->fn(pc, sink->ctx);
			}
			for (i = 0; i < slen; i++) sink->fn(str[i], sink->ctx);
			if (left_just)
				for (i = 0; i < pad; i++) sink->fn(' ', sink->ctx);
		}

		(void)padbuf;   /* reserved for future use; suppresses warning */
	}
}

/* -------------------------------------------------------------------------
 * Public API -- drop-in replacements for the standard functions
 * -----------------------------------------------------------------------*/

int mini_vfprintf(FILE *f, const char *fmt, va_list ap)
{
	mini_sink s;
	s.fn  = sink_file;
	s.ctx = f;
	_fmt_core(&s, fmt, ap);
	return 0;   /* return value not used by mbrpatch; full char count omitted */
}

int mini_fprintf(FILE *f, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	mini_vfprintf(f, fmt, ap);
	va_end(ap);
	return 0;
}

int mini_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	mini_vfprintf(stdout, fmt, ap);
	va_end(ap);
	return 0;
}

int mini_vsnprintf(char *buf, int size, const char *fmt, va_list ap)
{
	mini_snp_ctx ctx;
	mini_sink    s;

	if (!buf || size <= 0) return 0;
	buf[0]  = '\0';
	ctx.buf = buf;
	ctx.rem = size - 1;   /* reserve slot for null terminator */
	s.fn    = sink_buf;
	s.ctx   = &ctx;
	_fmt_core(&s, fmt, ap);
	return 0;
}

int mini_snprintf(char *buf, int size, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	mini_vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return 0;
}

int mini_sprintf(char *buf, const char *fmt, ...)
{
	/* Unbounded -- caller guarantees buffer is large enough.
	 * Use mini_snprintf in preference wherever possible. */
	va_list ap;
	va_start(ap, fmt);
	mini_vsnprintf(buf, 32767, fmt, ap);
	va_end(ap);
	return 0;
}

#endif /* __IA16__ */
