/* Handling strings that are given partially in the source encoding and
   partially in Unicode.
   Copyright (C) 2001-2018 Free Software Foundation, Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

/* Specification.  */
#include "xg-mixed-string.h"

#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "error-progname.h"
#include "unistr.h"
#include "xalloc.h"

#include "xg-pos.h"

#include "gettext.h"
#define _(str) gettext (str)


void
mixed_string_buffer_init (struct mixed_string_buffer *bp,
                          lexical_context_ty lcontext,
                          const char *logical_file_name,
                          int line_number)
{
  bp->utf8_buffer = NULL;
  bp->utf8_buflen = 0;
  bp->utf8_allocated = 0;
  bp->utf16_surr = 0;
  bp->curr_buffer = NULL;
  bp->curr_buflen = 0;
  bp->curr_allocated = 0;
  bp->lcontext = lcontext;
  bp->logical_file_name = logical_file_name;
  bp->line_number = line_number;
}

bool
mixed_string_buffer_is_empty (const struct mixed_string_buffer *bp)
{
  return (bp->utf8_buflen == 0 && bp->utf16_surr == 0 && bp->curr_buflen == 0);
}

/* Auxiliary function: Append a byte to bp->curr.  */
static inline void
mixed_string_buffer_append_to_curr_buffer (struct mixed_string_buffer *bp,
                                           unsigned char c)
{
  if (bp->curr_buflen == bp->curr_allocated)
    {
      bp->curr_allocated = 2 * bp->curr_allocated + 10;
      bp->curr_buffer = xrealloc (bp->curr_buffer, bp->curr_allocated);
    }
  bp->curr_buffer[bp->curr_buflen++] = c;
}

/* Auxiliary function: Ensure count more bytes are available in bp->utf8.  */
static inline void
mixed_string_buffer_grow_utf8_buffer (struct mixed_string_buffer *bp,
                                         size_t count)
{
  if (bp->utf8_buflen + count > bp->utf8_allocated)
    {
      size_t new_allocated = 2 * bp->utf8_allocated + 10;
      if (new_allocated < bp->utf8_buflen + count)
        new_allocated = bp->utf8_buflen + count;
      bp->utf8_allocated = new_allocated;
      bp->utf8_buffer = xrealloc (bp->utf8_buffer, new_allocated);
    }
}

/* Auxiliary function: Append a Unicode character to bp->utf8.
   uc must be < 0x110000.  */
static inline void
mixed_string_buffer_append_to_utf8_buffer (struct mixed_string_buffer *bp,
                                           ucs4_t uc)
{
  unsigned char utf8buf[6];
  int count = u8_uctomb (utf8buf, uc, 6);

  if (count < 0)
    /* The caller should have ensured that uc is not out-of-range.  */
    abort ();

  mixed_string_buffer_grow_utf8_buffer (bp, count);
  memcpy (bp->utf8_buffer + bp->utf8_buflen, utf8buf, count);
  bp->utf8_buflen += count;
}

/* Auxiliary function: Handle the attempt to append a lone surrogate to
   bp->utf8.  */
static void
mixed_string_buffer_append_lone_surrogate (struct mixed_string_buffer *bp,
                                           ucs4_t uc)
{
  /* A half surrogate is invalid, therefore use U+FFFD instead.
     It may be valid in a particular programming language.
     But a half surrogate is invalid in UTF-8:
       - RFC 3629 says
           "The definition of UTF-8 prohibits encoding character
            numbers between U+D800 and U+DFFF".
       - Unicode 4.0 chapter 3
         <http://www.unicode.org/versions/Unicode4.0.0/ch03.pdf>
         section 3.9, p.77, says
           "Because surrogate code points are not Unicode scalar
            values, any UTF-8 byte sequence that would otherwise
            map to code points D800..DFFF is ill-formed."
         and in table 3-6, p. 78, does not mention D800..DFFF.
       - The unicode.org FAQ question "How do I convert an unpaired
         UTF-16 surrogate to UTF-8?" has the answer
           "By representing such an unpaired surrogate on its own
            as a 3-byte sequence, the resulting UTF-8 data stream
            would become ill-formed."
     So use U+FFFD instead.  */
  error_with_progname = false;
  error (0, 0, _("%s:%d: warning: lone surrogate U+%04X"),
         logical_file_name, line_number, uc);
  error_with_progname = true;
  mixed_string_buffer_append_to_utf8_buffer (bp, 0xfffd);
}

/* Auxiliary function: Flush bp->utf16_surr into bp->utf8_buffer.  */
static inline void
mixed_string_buffer_flush_utf16_surr (struct mixed_string_buffer *bp)
{
  if (bp->utf16_surr != 0)
    {
      mixed_string_buffer_append_lone_surrogate (bp, bp->utf16_surr);
      bp->utf16_surr = 0;
    }
}

/* Auxiliary function: Flush bp->curr_buffer into bp->utf8_buffer.  */
static inline void
mixed_string_buffer_flush_curr_buffer (struct mixed_string_buffer *bp,
                                       int line_number)
{
  if (bp->curr_buflen > 0)
    {
      char *curr;
      size_t count;

      mixed_string_buffer_append_to_curr_buffer (bp, '\0');

      /* Convert from the source encoding to UTF-8.  */
      curr = from_current_source_encoding (bp->curr_buffer, bp->lcontext,
                                           bp->logical_file_name,
                                           line_number);

      /* Append it to bp->utf8_buffer.  */
      count = strlen (curr);
      mixed_string_buffer_grow_utf8_buffer (bp, count);
      memcpy (bp->utf8_buffer + bp->utf8_buflen, curr, count);
      bp->utf8_buflen += count;

      if (curr != bp->curr_buffer)
        free (curr);
      bp->curr_buflen = 0;
    }
}

void
mixed_string_buffer_append_char (struct mixed_string_buffer *bp, int c)
{
  /* Switch from Unicode character mode to multibyte character mode.  */
  mixed_string_buffer_flush_utf16_surr (bp);

  /* When a newline is seen, convert the accumulated multibyte sequence.
     This ensures a correct line number in the error message in case of
     a conversion error.  The "- 1" is to account for the newline.  */
  if (c == '\n')
    mixed_string_buffer_flush_curr_buffer (bp, bp->line_number - 1);

  mixed_string_buffer_append_to_curr_buffer (bp, (unsigned char) c);
}

void
mixed_string_buffer_append_unicode (struct mixed_string_buffer *bp, int c)
{
  /* Switch from multibyte character mode to Unicode character mode.  */
  mixed_string_buffer_flush_curr_buffer (bp, bp->line_number);

  /* Test whether this character and the previous one form a Unicode
     surrogate character pair.  */
  if (bp->utf16_surr != 0 && (c >= 0xdc00 && c < 0xe000))
    {
      unsigned short utf16buf[2];
      ucs4_t uc;

      utf16buf[0] = bp->utf16_surr;
      utf16buf[1] = c;
      if (u16_mbtouc (&uc, utf16buf, 2) != 2)
        abort ();

      mixed_string_buffer_append_to_utf8_buffer (bp, uc);
      bp->utf16_surr = 0;
    }
  else
    {
      mixed_string_buffer_flush_utf16_surr (bp);

      if (c >= 0xd800 && c < 0xdc00)
        bp->utf16_surr = c;
      else if (c >= 0xdc00 && c < 0xe000)
        mixed_string_buffer_append_lone_surrogate (bp, c);
      else
        mixed_string_buffer_append_to_utf8_buffer (bp, c);
    }
}

void
mixed_string_buffer_destroy (struct mixed_string_buffer *bp)
{
  free (bp->utf8_buffer);
  free (bp->curr_buffer);
}

char *
mixed_string_buffer_result (struct mixed_string_buffer *bp)
{
  char *utf8_buffer;

  /* Flush all into bp->utf8_buffer.  */
  mixed_string_buffer_flush_utf16_surr (bp);
  mixed_string_buffer_flush_curr_buffer (bp, bp->line_number);
  /* NUL-terminate it.  */
  mixed_string_buffer_grow_utf8_buffer (bp, 1);
  bp->utf8_buffer[bp->utf8_buflen] = '\0';

  /* Free curr_buffer.  */
  utf8_buffer = bp->utf8_buffer;
  free (bp->curr_buffer);

  /* Return it.  */
  return utf8_buffer;
}