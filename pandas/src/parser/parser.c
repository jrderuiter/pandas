/*

Copyright (c) 2012, Lambda Foundry, Inc.

See LICENSE for the license

*/

 /*
n   Low-level ascii-file processing for pandas. Combines some elements from
   Python's built-in csv module and Warren Weckesser's textreader project on
   GitHub. See Python Software Foundation License and BSD licenses for these.

  */


#include "parser.h"

#include <ctype.h>
#include <math.h>
#include <float.h>


#define READ_ERROR_OUT_OF_MEMORY   1


#define HAVE_MEMMAP
#define HAVE_GZIP

/*
* restore:
*  RESTORE_NOT     (0):
*      Free memory, but leave the file position wherever it
*      happend to be.
*  RESTORE_INITIAL (1):
*      Restore the file position to the location at which
*      the file_buffer was created.
*  RESTORE_FINAL   (2):
*      Put the file position at the next byte after the
*      data read from the file_buffer.
*/
#define RESTORE_NOT     0
#define RESTORE_INITIAL 1
#define RESTORE_FINAL   2




void *safe_realloc(void *buffer, size_t size) {
    void *result;
    // OS X is weird
    // http://stackoverflow.com/questions/9560609/
    // different-realloc-behaviour-in-linux-and-osx

    result = realloc(buffer, size);

    if (result != NULL) {
        // errno gets set to 12 on my OS Xmachine in some cases even when the
        // realloc succeeds. annoying
        errno = 0;
    } else {
        return buffer;
    }

    return result;
}


void coliter_setup(coliter_t *self, parser_t *parser, int i, int start) {
    // column i, starting at 0
    self->words = parser->words;
    self->col = i;
    self->line_start = parser->line_start + start;
}

coliter_t *coliter_new(parser_t *self, int i) {
    // column i, starting at 0
    coliter_t *iter = (coliter_t*) malloc(sizeof(coliter_t));

    if (NULL == iter) {
        return NULL;
    }

    coliter_setup(iter, self, i, 0);
    return iter;
}



 /* int64_t str_to_int64(const char *p_item, int64_t int_min, int64_t int_max, int *error); */
 /* uint64_t str_to_uint64(const char *p_item, uint64_t uint_max, int *error); */


 void free_if_not_null(void *ptr) {
     if (ptr != NULL) free(ptr);
 }



 /*

   Parser / tokenizer

 */


 void *grow_buffer(void *buffer, int length, int *capacity,
                   int space, int elsize, int *error) {
     int cap = *capacity;

     // Can we fit potentially nbytes tokens (+ null terminators) in the stream?
     while (length + space > cap) {
         cap = cap? cap << 1 : 2;

         buffer = safe_realloc(buffer, elsize * cap);

         if (buffer == NULL) {
             // TODO: error codes
             *error = -1;
         }
     }

     // sigh, multiple return values
     *capacity = cap;
     *error = 0;
     return buffer;
 }


void parser_set_default_options(parser_t *self) {
    self->decimal = '.';
    self->sci = 'E';

    // For tokenization
    self->state = START_RECORD;

    self->delimiter = ','; // XXX
    self->delim_whitespace = 0;

    self->doublequote = 0;
    self->quotechar = '"';
    self->escapechar = 0;
    self->skipinitialspace = 0;
    self->quoting = QUOTE_MINIMAL;
    self->allow_embedded_newline = 1;
    self->strict = 0;

    self->error_bad_lines = 0;
    self->warn_bad_lines = 0;

    self->commentchar = '#';
    self->thousands = '\0';

    self->skipset = NULL;
    self->skip_footer = 0;
}

int get_parser_memory_footprint(parser_t *self) {
    return 0;
}

parser_t* parser_new() {
    return (parser_t*) calloc(1, sizeof(parser_t));
}



int parser_clear_data_buffers(parser_t *self) {
    free_if_not_null(self->stream);
    free_if_not_null(self->words);
    free_if_not_null(self->word_starts);
    free_if_not_null(self->line_start);
    free_if_not_null(self->line_fields);

    return 0;
}

int parser_cleanup(parser_t *self) {
    if (self->cb_cleanup(self->source) < 0) {
        return -1;
    }

    if (parser_clear_data_buffers(self) < 0) {
        return -1;
    }

    // XXX where to put this
    free_if_not_null(self->error_msg);

    if (self->skipset != NULL)
        kh_destroy_int64((kh_int64_t*) self->skipset);

    return 0;
}



int parser_init(parser_t *self) {
    int sz;

    /*
      Initialize data buffers
    */

    self->stream = NULL;
    self->words = NULL;
    self->word_starts = NULL;
    self->line_start = NULL;
    self->line_fields = NULL;

    // token stream
    self->stream = (char*) malloc(STREAM_INIT_SIZE * sizeof(char));
    if (self->stream == NULL) {
        return PARSER_OUT_OF_MEMORY;
    }
    self->stream_cap = STREAM_INIT_SIZE;
    self->stream_len = 0;

    // word pointers and metadata
    sz = STREAM_INIT_SIZE / 10;
    sz = sz? sz : 1;
    self->words = (char**) malloc(sz * sizeof(char*));
    self->word_starts = (int*) malloc(sz * sizeof(int));
    self->words_cap = sz;
    self->words_len = 0;

    // line pointers and metadata
    self->line_start = (int*) malloc(sz * sizeof(int));

    self->line_fields = (int*) malloc(sz * sizeof(int));

    self->lines_cap = sz;
    self->lines = 0;
    self->file_lines = 0;

    if (self->stream == NULL || self->words == NULL ||
        self->word_starts == NULL || self->line_start == NULL ||
        self->line_fields == NULL) {

        parser_cleanup(self);

        return PARSER_OUT_OF_MEMORY;
    }

    /* amount of bytes buffered */
    self->datalen = 0;
    self->datapos = 0;

    self->line_start[0] = 0;
    self->line_fields[0] = 0;

    self->pword_start = self->stream;
    self->word_start = 0;

    self->state = START_RECORD;

    self->error_msg = NULL;

    return 0;
}


void parser_free(parser_t *self) {
    // opposite of parser_init
    parser_cleanup(self);
    free(self);
}

int make_stream_space(parser_t *self, size_t nbytes) {
    int i, status, cap;
    void *orig_ptr;

    // Can we fit potentially nbytes tokens (+ null terminators) in the stream?

    /* TRACE(("maybe growing buffers\n")); */

    /*
      TOKEN STREAM
    */

    orig_ptr = (void *) self->stream;
    self->stream = (char*) grow_buffer((void *) self->stream,
                                       self->stream_len,
                                       &self->stream_cap, nbytes * 2,
                                       sizeof(char), &status);

    if (status != 0) {
        return PARSER_OUT_OF_MEMORY;
    }

    // realloc sets errno when moving buffer?
    if (self->stream != orig_ptr) {
        // uff
        /* TRACE(("Moving word pointers\n")) */

        self->pword_start = self->stream + self->word_start;

        for (i = 0; i < self->words_len; ++i)
        {
            self->words[i] = self->stream + self->word_starts[i];
        }
    }


    /*
      WORD VECTORS
    */

    cap = self->words_cap;
    self->words = (char**) grow_buffer((void *) self->words,
                                       self->words_len,
                                       &self->words_cap, nbytes,
                                       sizeof(char*), &status);
    if (status != 0) {
        return PARSER_OUT_OF_MEMORY;
    }


    // realloc took place
    if (cap != self->words_cap) {
        self->word_starts = (int*) safe_realloc((void *) self->word_starts,
                                                sizeof(int) * self->words_cap);
        if (self->word_starts == NULL) {
            return PARSER_OUT_OF_MEMORY;
        }
    }


    /*
      LINE VECTORS
    */

    cap = self->lines_cap;
    self->line_start = (int*) grow_buffer((void *) self->line_start,
                                          self->lines,
                                          &self->lines_cap, nbytes,
                                          sizeof(int), &status);
    if (status != 0) {
        return PARSER_OUT_OF_MEMORY;
    }

    // realloc took place
    if (cap != self->lines_cap) {
        self->line_fields = (int*) safe_realloc((void *) self->line_fields,
                                                sizeof(int) * self->lines_cap);

        if (self->line_fields == NULL) {
            return PARSER_OUT_OF_MEMORY;
        }
    }


    /* TRACE(("finished growing buffers\n")); */

    return 0;
}


int inline push_char(parser_t *self, char c) {
    /* TRACE(("pushing %c \n", c)) */
    self->stream[self->stream_len++] = c;
    return 0;
}

int inline end_field(parser_t *self) {
    // XXX cruft
    self->numeric_field = 0;

    // null terminate token
    push_char(self, '\0');

    // set pointer and metadata
    self->words[self->words_len] = self->pword_start;

    TRACE(("Saw word %s at: %d\n", self->pword_start, self->word_start))

    self->word_starts[self->words_len] = self->word_start;
    self->words_len++;

    // increment line field count
    self->line_fields[self->lines]++;

    // New field begin in stream
    self->pword_start = self->stream + self->stream_len;
    self->word_start = self->stream_len;

    return 0;
}

int inline end_line(parser_t *self) {
    int fields;
    khiter_t k;  /* for hash set detection */
    int ex_fields = -1;

    fields = self->line_fields[self->lines];

    if (self->lines > 0) {
        ex_fields = self->line_fields[self->lines - 1];
    }

    if (self->skipset != NULL) {
        k = kh_get_int64((kh_int64_t*) self->skipset, self->file_lines);

        if (k != ((kh_int64_t*)self->skipset)->n_buckets) {
            TRACE(("Skipping row %d\n", self->file_lines));
            // increment file line count
            self->file_lines++;

            // skip the tokens from this bad line
            self->line_start[self->lines] += fields;

            // reset field count
            self->line_fields[self->lines] = 0;
            return 0;
        }
    }

    if (!(self->lines <= self->header + 1) && fields != ex_fields) {
        // increment file line count
        self->file_lines++;

        // skip the tokens from this bad line
        self->line_start[self->lines] += fields;

        // reset field count
        self->line_fields[self->lines] = 0;

        // file_lines is now the _actual_ file line number (starting at 1)

        if (self->error_bad_lines) {
            self->error_msg = (char*) malloc(100);
            sprintf(self->error_msg, "Expected %d fields in line %d, saw %d\n",
                    ex_fields, self->file_lines, fields);

            TRACE(("Error at line %d, %d fields\n", self->file_lines, fields));

            return -1;
        } else {
            // simply skip bad lines
            if (self->warn_bad_lines) {
                // print error message
                printf("Skipping line %d: expected %d fields, saw %d\n",
                       self->file_lines, ex_fields, fields);
            }
        }
    } else {
        // increment both line counts
        self->file_lines++;

        self->lines++;

        // good line, set new start point
        self->line_start[self->lines] = (self->line_start[self->lines - 1] +
                                         fields);

        TRACE(("new line start: %d\n", self->line_start[self->lines]));

        // new line start with 0 fields
        self->line_fields[self->lines] = 0;
    }

    TRACE(("Finished line, at %d\n", self->lines));

    return 0;
}



int parser_add_skiprow(parser_t *self, int64_t row) {
    khiter_t k;
    kh_int64_t *set;
    int ret = 0;

    if (self->skipset == NULL) {
        self->skipset = (void*) kh_init_int64();
    }

    set = (kh_int64_t*) self->skipset;

    k = kh_put_int64(set, row, &ret);
    set->keys[k] = row;

    return 0;
}

int parser_buffer_bytes(parser_t *self, size_t nbytes) {
    int status;
    size_t bytes_read;
    void *src = self->source;

    status = 0;
    self->datapos = 0;
    self->data = self->cb_io(self->source, nbytes, &bytes_read, &status);
    self->datalen = bytes_read;

    if (status != REACHED_EOF && self->data == NULL) {
        self->error_msg = (char*) malloc(200);

        if (status == CALLING_READ_FAILED) {
            sprintf(self->error_msg, ("Calling read(nbytes) on source failed. "
                                      "Try engine='python'."));
        } else {
            sprintf(self->error_msg, "Unknown error in IO callback");
        }
        return -1;
    }

    TRACE(("datalen: %d\n", self->datalen));

    return status;
}


/*

  Tokenization macros and state machine code

*/

//    printf("pushing %c\n", c);

#define PUSH_CHAR(c)                           \
    *stream++ = c;                             \
    slen++;

// This is a little bit of a hack but works for now

#define END_FIELD()                                \
    self->stream_len = slen;                   \
    if (end_field(self) < 0) {                 \
        goto parsingerror;                     \
    }                                          \
    stream = self->stream + self->stream_len;  \
    slen = self->stream_len;

#define END_LINE()                                                      \
    self->stream_len = slen;                                            \
    if (end_line(self) < 0) {                                           \
        goto parsingerror;                                              \
    }                                                                   \
    self->state = START_RECORD;                                         \
    if (line_limit > 0 && self->lines == start_lines + line_limit) {    \
        goto linelimit;                                                 \
                                                                        \
    }                                                                   \
    stream = self->stream + self->stream_len;                           \
    slen = self->stream_len;

#define IS_WHITESPACE(c) ((c == ' ' || c == '\t'))

typedef int (*parser_op)(parser_t *self, size_t line_limit);

#define _TOKEN_CLEANUP()                                                \
    self->stream_len = slen;                                            \
    self->datapos = i;                                                  \
    TRACE(("datapos: %d, datalen: %d\n", self->datapos, self->datalen));



int tokenize_delimited(parser_t *self, size_t line_limit)
{
    int i, slen, start_lines;
    char c;
    char *stream;
    char *buf = self->data + self->datapos;

    start_lines = self->lines;

    if (make_stream_space(self, self->datalen - self->datapos) < 0) {
        self->error_msg = "out of memory";
        return -1;
    }

    stream = self->stream + self->stream_len;
    slen = self->stream_len;

    TRACE(("%s\n", buf));

    for (i = self->datapos; i < self->datalen; ++i)
    {
        // Next character in file
        c = *buf++;

        TRACE(("Iter: %d Char: %c Line %d field_count %d, state %d\n",
               i, c, self->file_lines + 1, self->line_fields[self->lines],
               self->state));

        switch(self->state) {
        case START_RECORD:
            // start of record

            if (c == '\n') {
                // \n\r possible?
                END_LINE();
                break;
            } else if (c == '\r') {
                self->state = EAT_CRNL;
                break;
            }

            /* normal character - handle as START_FIELD */
            self->state = START_FIELD;
            /* fallthru */
        case START_FIELD:
            /* expecting field */
            if (c == '\n') {
                END_FIELD();
                END_LINE();
                /* self->state = START_RECORD; */
            } else if (c == '\r') {
                END_FIELD();
                self->state = EAT_CRNL;
            }
            else if (c == self->quotechar &&
                     self->quoting != QUOTE_NONE) {
                /* start quoted field */
                self->state = IN_QUOTED_FIELD;
            }
            else if (c == self->escapechar) {
                /* possible escaped character */
                self->state = ESCAPED_CHAR;
            }
            else if (c == ' ' && self->skipinitialspace)
                /* ignore space at start of field */
                ;
            else if (c == self->delimiter) {
                /* save empty field */
                END_FIELD();
            }
            else {
                /* begin new unquoted field */
                if (self->quoting == QUOTE_NONNUMERIC)
                    self->numeric_field = 1;

                // TRACE(("pushing %c", c));
                PUSH_CHAR(c);
                self->state = IN_FIELD;
            }
            break;

        case ESCAPED_CHAR:
            /* if (c == '\0') */
            /*  c = '\n'; */

            PUSH_CHAR(c);
            self->state = IN_FIELD;
            break;

        case IN_FIELD:
            /* in unquoted field */
            if (c == '\n') {
                END_FIELD();
                END_LINE();
                /* self->state = START_RECORD; */
            } else if (c == '\r') {
                END_FIELD();
                self->state = EAT_CRNL;
            }
            else if (c == self->escapechar) {
                /* possible escaped character */
                self->state = ESCAPED_CHAR;
            }
            else if (c == self->delimiter) {
                // End of field. End of line not reached yet
                END_FIELD();
                self->state = START_FIELD;
            }
            else {
                /* normal character - save in field */
                PUSH_CHAR(c);
            }
            break;

        case IN_QUOTED_FIELD:
            /* in quoted field */
            if (c == self->escapechar) {
                /* Possible escape character */
                self->state = ESCAPE_IN_QUOTED_FIELD;
            }
            else if (c == self->quotechar &&
                     self->quoting != QUOTE_NONE) {
                if (self->doublequote) {
                    /* doublequote; " represented by "" */
                    self->state = QUOTE_IN_QUOTED_FIELD;
                }
                else {
                    /* end of quote part of field */
                    self->state = IN_FIELD;
                }
            }
            else {
                /* normal character - save in field */
                PUSH_CHAR(c);
            }
            break;

        case ESCAPE_IN_QUOTED_FIELD:
            /* if (c == '\0') */
            /*  c = '\n'; */

            PUSH_CHAR(c);
            self->state = IN_QUOTED_FIELD;
            break;

        case QUOTE_IN_QUOTED_FIELD:
            /* doublequote - seen a quote in an quoted field */
            if (self->quoting != QUOTE_NONE && c == self->quotechar) {
                /* save "" as " */

                PUSH_CHAR(c);
                self->state = IN_QUOTED_FIELD;
            }
            else if (c == self->delimiter) {
                // End of field. End of line not reached yet

                END_FIELD();
                self->state = START_FIELD;
            }
            else if (c == '\n') {
                END_FIELD();
                END_LINE();
                /* self->state = START_RECORD; */
            }
            else if (c == '\r') {
                END_FIELD();
                self->state = EAT_CRNL;
            }
            else if (!self->strict) {
                PUSH_CHAR(c);
                self->state = IN_FIELD;
            }
            else {
                self->error_msg = (char*) malloc(50);
                sprintf(self->error_msg, "'%c' expected after '%c'",
                        self->delimiter, self->quotechar);
                goto parsingerror;
            }
            break;

        case EAT_CRNL:
            if (c == '\n') {
                END_LINE();
                /* self->state = START_RECORD; */
            } else {
                /* self->error_msg = ("new-line character seen in" */
                /*                 " unquoted field - do you need" */
                /*                 " to open the file in " */
                /*                 "universal-newline mode?"); */
                goto parsingerror;
            }
            break;
        default:
            break;

        }
    }

    _TOKEN_CLEANUP();

    TRACE(("Finished tokenizing input\n"))

    return 0;

parsingerror:
    i++;
    _TOKEN_CLEANUP();

    return -1;

linelimit:
    i++;
    _TOKEN_CLEANUP();

    return 0;
}

int tokenize_whitespace(parser_t *self, size_t line_limit)
{
    int i, slen, start_lines;
    char c;
    char *stream;
    char *buf = self->data + self->datapos;

    start_lines = self->lines;

    if (make_stream_space(self, self->datalen - self->datapos) < 0) {
        self->error_msg = "out of memory";
        return -1;
    }

    stream = self->stream + self->stream_len;
    slen = self->stream_len;

    TRACE(("%s\n", buf));

    for (i = self->datapos; i < self->datalen; ++i)
    {
        // Next character in file
        c = *buf++;

        TRACE(("Iter: %d Char: %c Line %d field_count %d\n",
               i, c, self->file_lines + 1, self->line_fields[self->lines]));

        switch(self->state) {

        case EAT_WHITESPACE:
            if (!IS_WHITESPACE(c)) {
                // END_FIELD();
                self->state = START_FIELD;
                // Fall through to subsequent state
            } else {
                // if whitespace char, keep slurping
                break;
            }

        case START_RECORD:
            // start of record
            if (c == '\n') {
                // \n\r possible?
                END_LINE();
                break;
            } else if (c == '\r') {
                self->state = EAT_CRNL;
                break;
            }

            /* normal character - handle as START_FIELD */
            self->state = START_FIELD;
            /* fallthru */
        case START_FIELD:
            /* expecting field */
            if (c == '\n') {
                END_FIELD();
                END_LINE();
                /* self->state = START_RECORD; */
            } else if (c == '\r') {
                END_FIELD();
                self->state = EAT_CRNL;
            }
            else if (c == self->quotechar &&
                     self->quoting != QUOTE_NONE) {
                /* start quoted field */
                self->state = IN_QUOTED_FIELD;
            }
            else if (c == self->escapechar) {
                /* possible escaped character */
                self->state = ESCAPED_CHAR;
            }
            /* else if (c == ' ' && self->skipinitialspace) */
            /*     /\* ignore space at start of field *\/ */
            /*     ; */
            else if (IS_WHITESPACE(c)) {
                self->state = EAT_WHITESPACE;
            }
            else {
                /* begin new unquoted field */
                if (self->quoting == QUOTE_NONNUMERIC)
                    self->numeric_field = 1;

                // TRACE(("pushing %c", c));
                PUSH_CHAR(c);
                self->state = IN_FIELD;
            }
            break;

        case ESCAPED_CHAR:
            /* if (c == '\0') */
            /*  c = '\n'; */

            PUSH_CHAR(c);
            self->state = IN_FIELD;
            break;

        case IN_FIELD:
            /* in unquoted field */
            if (c == '\n') {
                END_FIELD();
                END_LINE();
                /* self->state = START_RECORD; */
            } else if (c == '\r') {
                END_FIELD();
                self->state = EAT_CRNL;
            }
            else if (c == self->escapechar) {
                /* possible escaped character */
                self->state = ESCAPED_CHAR;
            }
            else if (IS_WHITESPACE(c)) {
                // End of field. End of line not reached yet
                END_FIELD();
                self->state = EAT_WHITESPACE;
            }
            else {
                /* normal character - save in field */
                PUSH_CHAR(c);
            }
            break;

        case IN_QUOTED_FIELD:
            /* in quoted field */
            if (c == self->escapechar) {
                /* Possible escape character */
                self->state = ESCAPE_IN_QUOTED_FIELD;
            }
            else if (c == self->quotechar &&
                     self->quoting != QUOTE_NONE) {
                if (self->doublequote) {
                    /* doublequote; " represented by "" */
                    self->state = QUOTE_IN_QUOTED_FIELD;
                }
                else {
                    /* end of quote part of field */
                    self->state = IN_FIELD;
                }
            }
            else {
                /* normal character - save in field */
                PUSH_CHAR(c);
            }
            break;

        case ESCAPE_IN_QUOTED_FIELD:
            /* if (c == '\0') */
            /*  c = '\n'; */

            PUSH_CHAR(c);
            self->state = IN_QUOTED_FIELD;
            break;

        case QUOTE_IN_QUOTED_FIELD:
            /* doublequote - seen a quote in an quoted field */
            if (self->quoting != QUOTE_NONE && c == self->quotechar) {
                /* save "" as " */

                PUSH_CHAR(c);
                self->state = IN_QUOTED_FIELD;
            }
            else if (IS_WHITESPACE(c)) {
                // End of field. End of line not reached yet

                END_FIELD();
                self->state = EAT_WHITESPACE;
            }
            else if (c == '\n') {
                END_FIELD();
                END_LINE();
                /* self->state = START_RECORD; */
            }
            else if (c == '\r') {
                END_FIELD();
                self->state = EAT_CRNL;
            }
            else if (!self->strict) {
                PUSH_CHAR(c);
                self->state = IN_FIELD;
            }
            else {
                self->error_msg = (char*) malloc(50);
                sprintf(self->error_msg, "'%c' expected after '%c'",
                        self->delimiter, self->quotechar);
                goto parsingerror;
            }
            break;

        case EAT_CRNL:
            if (c == '\n') {
                END_LINE();
                /* self->state = START_RECORD; */
            } else {
                /* self->error_msg = ("new-line character seen in" */
                /*                 " unquoted field - do you need" */
                /*                 " to open the file in " */
                /*                 "universal-newline mode?"); */
                goto parsingerror;
            }
            break;
        default:
            break;


        }

    }

    _TOKEN_CLEANUP();

    TRACE(("Finished tokenizing input\n"))

    return 0;

parsingerror:
    i++;
    _TOKEN_CLEANUP();

    return -1;

linelimit:
    i++;
    _TOKEN_CLEANUP();

    return 0;
}


int parser_handle_eof(parser_t *self) {
    TRACE(("handling eof, datalen: %d\n", self->datalen))
    if (self->datalen == 0 && (self->state != START_RECORD)) {
        // test cases needed here
        // TODO: empty field at end of line
        TRACE(("handling eof\n"));

        if (self->state == IN_FIELD) {
            if (end_field(self) < 0)
                return -1;
        } else if (self->state == QUOTE_IN_QUOTED_FIELD) {
            if (end_field(self) < 0)
                return -1;
        } else if (self->state == IN_QUOTED_FIELD) {
            self->error_msg = (char*) malloc(100);
            sprintf(self->error_msg, "EOF inside string starting at line %d",
                    self->file_lines);
            return -1;
        }

        if (end_line(self) < 0)
            return -1;

        return 0;
    }
    else if (self->datalen == 0 && (self->state == START_RECORD)) {
        return 0;
    }

    return -1;
}

int parser_consume_rows(parser_t *self, size_t nrows) {
    int i, offset, word_deletions, char_count;

    if (nrows > self->lines) {
        nrows = self->lines;
    }

    /* do nothing */
    if (nrows == 0)
        return 0;

    /* cannot guarantee that nrows + 1 has been observed */
    word_deletions = self->line_start[nrows - 1] + self->line_fields[nrows - 1];
    char_count = (self->word_starts[word_deletions - 1] +
                  strlen(self->words[word_deletions - 1]) + 1);

    TRACE(("Deleting %d words, %d chars\n", word_deletions, char_count));

    /* move stream, only if something to move */
    if (char_count < self->stream_len) {
        memmove((void*) self->stream, (void*) (self->stream + char_count),
                self->stream_len - char_count);
    }
    /* buffer counts */
    self->stream_len -= char_count;

    /* move token metadata */
    for (i = 0; i < self->words_len - word_deletions; ++i) {
        offset = i + word_deletions;

        self->words[i] = self->words[offset] - char_count;
        self->word_starts[i] = self->word_starts[offset] - char_count;
    }
    self->words_len -= word_deletions;

    /* move current word pointer to stream */
    self->pword_start -= char_count;
    self->word_start -= char_count;

    /* printf("Line_start: "); */
    /* for (i = 0; i < self->lines; ++i) { */
    /*     printf("%d ", self->line_start[i]); */
    /* } */
    /* printf("\n"); */

    /* move line metadata */
    for (i = 0; i < self->lines - nrows + 1; ++i)
    {
        offset = i + nrows;
        self->line_start[i] = self->line_start[offset] - word_deletions;

        /* TRACE(("First word in line %d is now %s\n", i, */
        /*        self->words[self->line_start[i]])); */

        self->line_fields[i] = self->line_fields[offset];
    }
    self->lines -= nrows;
    /* self->line_fields[self->lines] = 0; */

    /* printf("Line_start: "); */
    /* for (i = 0; i < self->lines; ++i) { */
    /*     printf("%d ", self->line_start[i]); */
    /* } */
    /* printf("\n"); */

    return 0;
}

static size_t _next_pow2(size_t sz) {
    size_t result = 1;
    while (result < sz) result *= 2;
    return result;
}

int parser_trim_buffers(parser_t *self) {
    /*
      Free memory
     */
    size_t new_cap;

    /* trim stream */
    new_cap = _next_pow2(self->stream_len);
    if (new_cap < self->stream_cap) {
        self->stream = safe_realloc((void*) self->stream, new_cap);
        self->stream_cap = new_cap;
    }

    /* trim words, word_starts */
    new_cap = _next_pow2(self->words_len);
    if (new_cap < self->words_cap) {
        self->words = (char**) safe_realloc((void*) self->words,
                                            new_cap * sizeof(char*));
        self->word_starts = (int*) safe_realloc((void*) self->word_starts,
                                                new_cap * sizeof(int));
        self->words_cap = new_cap;
    }

    /* trim line_start, line_fields */
    new_cap = _next_pow2(self->lines);
    if (new_cap < self->lines_cap) {
        self->line_start = (int*) safe_realloc((void*) self->line_start,
                                               new_cap * sizeof(int));
        self->line_fields = (int*) safe_realloc((void*) self->line_fields,
                                                new_cap * sizeof(int));
        self->lines_cap = new_cap;
    }

    return 0;
}

void debug_print_parser(parser_t *self) {
    int j, line;
    char *token;

    for (line = 0; line < self->lines; ++line)
    {
        printf("(Parsed) Line %d: ", line);

        for (j = 0; j < self->line_fields[j]; ++j)
        {
            token = self->words[j + self->line_start[line]];
            printf("%s ", token);
        }
        printf("\n");
    }
}

int clear_parsed_lines(parser_t *self, size_t nlines) {
    // TODO. move data up in stream, shift relevant word pointers

    return 0;
}


/*
  nrows : number of rows to tokenize (or until reach EOF)
  all : tokenize all the data vs. certain number of rows
 */

int _tokenize_helper(parser_t *self, size_t nrows, int all) {
    parser_op tokenize_bytes;

    int status = 0;
    int start_lines = self->lines;

    if (self->delim_whitespace) {
        tokenize_bytes = tokenize_whitespace;
    } else {
        tokenize_bytes = tokenize_delimited;
    }

    if (self->state == FINISHED) {
        return 0;
    }

    TRACE(("Asked to tokenize %d rows\n", (int) nrows));

    while (1) {
        if (!all && self->lines - start_lines >= nrows)
            break;

        if (self->datapos == self->datalen) {
            status = parser_buffer_bytes(self, self->chunksize);

            if (status == REACHED_EOF) {
                // close out last line
                status = parser_handle_eof(self);
                self->state = FINISHED;
                break;
            } else if (status != 0) {
                return status;
            }
        }

        TRACE(("Trying to process %d bytes\n", self->datalen - self->datapos));
        TRACE(("sourcetype: %c, status: %d\n", self->sourcetype, status));

        status = tokenize_bytes(self, nrows);

        if (status < 0) {
            // XXX
            TRACE(("Status %d returned from tokenize_bytes, breaking\n",
                   status));
            status = -1;
            break;
        }
    }
    TRACE(("leaving tokenize_helper\n"));
    return status;
}

int tokenize_nrows(parser_t *self, size_t nrows) {
    int status = _tokenize_helper(self, nrows, 0);
    return status;
}

int tokenize_all_rows(parser_t *self) {
    int status = _tokenize_helper(self, -1, 1);
    return status;
}

/*
  Iteration through ragged matrix structure
*/

/* int test_tokenize(char *fname) { */
/*     parser_t *self; */
/*     coliter_t citer; */
/*     int status = 0; */
/*     int nbytes = CHUNKSIZE; */

/*     clock_t start = clock(); */

/*     self = parser_new(); */
/*     self->chunksize = nbytes; */

/*     // self->source = malloc(sizeof(file_source)); */

/*     FILE* fp = fopen(fname, "rb"); */
/*     parser_file_source_init(self, fp); */

/*     parser_set_default_options(self); */

/*     if (parser_init(self) < 0) { */
/*         return -1; */
/*     } */

/*     self->header = 0; */

/*     status = tokenize_all_rows(self); */

/*     if (status != 0) { */
/*         if (self->error_msg == NULL) { */
/*             printf("PARSE_ERROR: no message\n"); */
/*         } */
/*         else { */
/*             printf("PARSE_ERROR: %s", self->error_msg); */
/*         } */
/*     } */


/*     // debug_print_parser(parser); */

/*     // return 0; */
/*     /\* if (status < 0) { *\/ */
/*     /\*  return status; *\/ */
/*     /\* } *\/ */

/*     /\* int i, words = 0; *\/ */
/*     /\* for (i = 0; i < parser.stream_len; ++i) *\/ */
/*     /\* { *\/ */
/*     /\*     if (parser.stream[i] == '\0') words++; *\/ */
/*     /\* } *\/ */

/*     printf("Time elapsed: %f\n", ((double)clock() - start) / CLOCKS_PER_SEC); */
/*     /\* return 0; *\/ */

/*     int i, j, error, columns; */
/*     char *word; */
/*     double data; */

/*     columns = self->line_fields[0]; */

/*     printf("columns: %d\n", columns); */
/*     printf("errno is %d\n", errno); */

/*     for (j = 0; j < columns; ++j) */
/*     { */
/*         coliter_setup(&citer, self, j, 0); */

/*         for (i = 0; i < self->lines; ++i) */
/*         { */
/*             if (j >= self->line_fields[i]) continue; */

/*             word = COLITER_NEXT(citer); */
/*             error = to_double(word, &data, self->sci, self->decimal); */
/*             if (error != 1) { */
/*                 printf("error at %d, errno: %d\n", i, errno); */
/*                 printf("failed: %s %d\n", word, (int) strlen(word)); */
/*                 break; */
/*             } */
/*         } */
/*     } */

/*     /\* for (j = 0; j < columns; ++j) *\/ */
/*     /\* { *\/ */
/*     /\*  // citer = coliter_new(&parser, j); *\/ */
/*     /\*  for (i = 0; i < parser->lines; ++i) *\/ */
/*     /\*  { *\/ */
/*     /\*      if (j >= parser->line_fields[i]) continue; *\/ */
/*     /\*      word = parser->words[parser->line_start[i] + j]; *\/ */
/*     /\*      error = to_double(word, &data, parser->sci, parser->decimal); *\/ */
/*     /\*      if (error != 1) { *\/ */
/*     /\*          printf("error at %d, errno: %d\n", i, errno); *\/ */
/*     /\*          printf("failed: %s %d\n", word, (int) strlen(word)); *\/ */
/*     /\*          break; *\/ */
/*     /\*      } *\/ */
/*     /\*  } *\/ */
/*     /\*  // free(citer); *\/ */
/*     /\* } *\/ */

/*     printf("Time elapsed: %f\n", ((double)clock() - start) / CLOCKS_PER_SEC); */

/*     /\* for (i = 0; i < parser.words_len; ++i) *\/ */
/*     /\* { *\/ */
/*     /\*     error = to_double(parser.words[i], &data, parser.sci, parser.decimal); *\/ */
/*     /\*  if (error != 1) { *\/ */
/*     /\*      ; *\/ */
/*     /\*      printf("failed: %s %d\n", parser.words[i], *\/ */
/*     /\*             (int) strlen(parser.words[i])); *\/ */
/*     /\*  } else { *\/ */
/*     /\*      ; *\/ */
/*     /\*      /\\* printf("success: %.4f\n", data); *\\/ *\/ */
/*     /\*  } *\/ */
/*     /\* } *\/ */


/*     /\* for (i = 0; i < parser.words_len; ++i) *\/ */
/*     /\* { *\/ */
/*     /\*     error = to_double(parser.words[i], &data, parser.sci, parser.decimal); *\/ */
/*     /\*  if (error != 1) { *\/ */
/*     /\*      ; *\/ */
/*             /\* printf("failed: %s %d\n", parser.words[i], *\/ */
/*             /\*     (int) strlen(parser.words[i])); *\/ */
/*     /\*  } else { *\/ */
/*     /\*      ; *\/ */
/*     /\*      /\\* printf("success: %.4f\n", data); *\\/ *\/ */
/*     /\*  } *\/ */
/*     /\* } *\/ */


/*     /\* printf("saw %d words\n", words); *\/ */

/*     // debug_print_parser(&parser); */

/*     parser_free(self); */

/*     fclose(fp); */

/*     /\* if (parser_cleanup(self) < 0) { *\/ */
/*     /\*     return -1; *\/ */
/*     /\* } *\/ */

/*     /\* free(parser); *\/ */

/*     return status; */
/* } */


void test_count_lines(char *fname) {
    clock_t start = clock();

    char *buffer, *tmp;
    size_t bytes, lines = 0;
    int i;
    FILE *fp = fopen(fname, "rb");

    buffer = (char*) malloc(CHUNKSIZE * sizeof(char));

    while(1) {
        tmp = buffer;
        bytes = fread((void *) buffer, sizeof(char), CHUNKSIZE, fp);
        // printf("Read %d bytes\n", bytes);

        if (bytes == 0) {
            break;
        }

        for (i = 0; i < bytes; ++i)
        {
            if (*tmp++ == '\n') {
                lines++;
            }
        }
    }


    printf("Saw %d lines\n", (int) lines);

    free(buffer);
    fclose(fp);

    printf("Time elapsed: %f\n", ((double)clock() - start) / CLOCKS_PER_SEC);
}


/* int main(int argc, char *argv[]) */
/* { */
/*     // import_array(); */

/*     TRACE(("hello: %s\n", "Wes")); */

/*     test_tokenize("/Users/wesm/code/pandas/pandas/io/tests/test1.csv"); */

/*     // char *msg = (char*) malloc(50); */
/*     // sprintf(msg, "Hello: %s\n", "wes"); */
/*     // printf("%s", msg); */

/*     /\* int i; *\/ */
/*     /\* for (i = 0; i < 10; ++i) *\/ */
/*     /\* { *\/ */
/*     /\*  test_count_lines("../foo.csv"); *\/ */
/*     /\* } *\/ */

/*     return 0; */
/* } */


// forward declaration
double inline xstrtod(const char *p, char **q, char decimal, char sci, int skip_trailing);


inline void lowercase(char *p) {
    for ( ; *p; ++p) *p = tolower(*p);
}

inline void uppercase(char *p) {
    for ( ; *p; ++p) *p = toupper(*p);
}


/*
 *  `item` must be the nul-terminated string that is to be
 *  converted to a double.
 *
 *  To be successful, to_double() must use *all* the characters
 *  in `item`.  E.g. "1.q25" will fail.  Leading and trailing
 *  spaces are allowed.
 *
 *  `sci` is the scientific notation exponent character, usually
 *  either 'E' or 'D'.  Case is ignored.
 *
 *  `decimal` is the decimal point character, usually either
 *  '.' or ','.
 *
 */

int inline to_double(char *item, double *p_value, char sci, char decimal)
{
    char *p_end;

    *p_value = xstrtod(item, &p_end, decimal, sci, TRUE);

    return (errno == 0) && (!*p_end);
}


int inline to_complex(char *item, double *p_real, double *p_imag, char sci, char decimal)
{
    char *p_end;

    *p_real = xstrtod(item, &p_end, decimal, sci, FALSE);
    if (*p_end == '\0') {
        *p_imag = 0.0;
        return errno == 0;
    }
    if (*p_end == 'i' || *p_end == 'j') {
        *p_imag = *p_real;
        *p_real = 0.0;
        ++p_end;
    }
    else {
        if (*p_end == '+') {
            ++p_end;
        }
        *p_imag = xstrtod(p_end, &p_end, decimal, sci, FALSE);
        if (errno || ((*p_end != 'i') && (*p_end != 'j'))) {
            return FALSE;
        }
        ++p_end;
    }
    while(*p_end == ' ') {
        ++p_end;
    }
    return *p_end == '\0';
}


int inline to_longlong(char *item, long long *p_value)
{
    char *p_end;

    // Try integer conversion.  We explicitly give the base to be 10. If
    // we used 0, strtoll() would convert '012' to 10, because the leading 0 in
    // '012' signals an octal number in C.  For a general purpose reader, that
    // would be a bug, not a feature.
    *p_value = strtoll(item, &p_end, 10);

    // Allow trailing spaces.
    while (isspace(*p_end)) ++p_end;

    return (errno == 0) && (!*p_end);
}

int inline to_longlong_thousands(char *item, long long *p_value, char tsep)
{
	int i, pos, status, n = strlen(item), count = 0;
	char *tmp;
    char *p_end;

	for (i = 0; i < n; ++i)
	{
		if (*(item + i) == tsep) {
			count++;
		}
	}

	if (count == 0) {
		return to_longlong(item, p_value);
	}

	tmp = (char*) malloc((n - count + 1) * sizeof(char));
	if (tmp == NULL) {
		return 0;
	}

	pos = 0;
	for (i = 0; i < n; ++i)
	{
		if (item[i] != tsep)
			tmp[pos++] = item[i];
	}

	tmp[pos] = '\0';

	status = to_longlong(tmp, p_value);
	free(tmp);

	return status;
}

int inline to_boolean(char *item, uint8_t *val) {
	char *tmp;
	int i, status = 0;

    static const char *tstrs[2] = {"TRUE", "YES"};
    static const char *fstrs[2] = {"FALSE", "NO"};

	tmp = malloc(sizeof(char) * (strlen(item) + 1));
	strcpy(tmp, item);
	uppercase(tmp);

    for (i = 0; i < 2; ++i)
    {
        if (strcmp(tmp, tstrs[i]) == 0) {
            *val = 1;
            goto done;
        }
    }

    for (i = 0; i < 2; ++i)
    {
        if (strcmp(tmp, fstrs[i]) == 0) {
            *val = 0;
            goto done;
        }
    }

    status = -1;

done:
	free(tmp);
	return status;
}

// #define TEST

#ifdef TEST

int main(int argc, char *argv[])
{
    double x, y;
	long long xi;
    int status;
    char *s;

    //s = "0.10e-3-+5.5e2i";
    // s = "1-0j";
    // status = to_complex(s, &x, &y, 'e', '.');
	s = "123,789";
	status = to_longlong_thousands(s, &xi, ',');
    printf("s = '%s'\n", s);
    printf("status = %d\n", status);
    printf("x = %d\n", (int) xi);

    // printf("x = %lg,  y = %lg\n", x, y);

    return 0;
}
#endif

// ---------------------------------------------------------------------------
// Implementation of xstrtod

//
// strtod.c
//
// Convert string to double
//
// Copyright (C) 2002 Michael Ringgaard. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
// 3. Neither the name of the project nor the names of its contributors
//    may be used to endorse or promote products derived from this software
//    without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
// OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
// OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
// SUCH DAMAGE.
//
// -----------------------------------------------------------------------
// Modifications by Warren Weckesser, March 2011:
// * Rename strtod() to xstrtod().
// * Added decimal and sci arguments.
// * Skip trailing spaces.
// * Commented out the other functions.
//

double inline xstrtod(const char *str, char **endptr, char decimal,
					  char sci, int skip_trailing)
{
  double number;
  int exponent;
  int negative;
  char *p = (char *) str;
  double p10;
  int n;
  int num_digits;
  int num_decimals;

  errno = 0;

  // Skip leading whitespace
  while (isspace(*p)) p++;

  // Handle optional sign
  negative = 0;
  switch (*p)
  {
    case '-': negative = 1; // Fall through to increment position
    case '+': p++;
  }

  number = 0.;
  exponent = 0;
  num_digits = 0;
  num_decimals = 0;

  // Process string of digits
  while (isdigit(*p))
  {
    number = number * 10. + (*p - '0');
    p++;
    num_digits++;
  }

  // Process decimal part
  if (*p == decimal)
  {
    p++;

    while (isdigit(*p))
    {
      number = number * 10. + (*p - '0');
      p++;
      num_digits++;
      num_decimals++;
    }

    exponent -= num_decimals;
  }

  if (num_digits == 0)
  {
    errno = ERANGE;
    return 0.0;
  }

  // Correct for sign
  if (negative) number = -number;

  // Process an exponent string
  if (toupper(*p) == toupper(sci))
  {
    // Handle optional sign
    negative = 0;
    switch (*++p)
    {
      case '-': negative = 1;   // Fall through to increment pos
      case '+': p++;
    }

    // Process string of digits
    n = 0;
    while (isdigit(*p))
    {
      n = n * 10 + (*p - '0');
      p++;
    }

    if (negative)
      exponent -= n;
    else
      exponent += n;
  }


  if (exponent < DBL_MIN_EXP  || exponent > DBL_MAX_EXP)
  {

    errno = ERANGE;
    return HUGE_VAL;
  }

  // Scale the result
  p10 = 10.;
  n = exponent;
  if (n < 0) n = -n;
  while (n)
  {
    if (n & 1)
    {
      if (exponent < 0)
        number /= p10;
      else
        number *= p10;
    }
    n >>= 1;
    p10 *= p10;
  }


  if (number == HUGE_VAL) {
	  errno = ERANGE;
  }

  if (skip_trailing) {
      // Skip trailing whitespace
      while (isspace(*p)) p++;
  }

  if (endptr) *endptr = p;


  return number;
}

/*
float strtof(const char *str, char **endptr)
{
  return (float) strtod(str, endptr);
}


long double strtold(const char *str, char **endptr)
{
  return strtod(str, endptr);
}

double atof(const char *str)
{
  return strtod(str, NULL);
}
*/

// End of xstrtod code
// ---------------------------------------------------------------------------
