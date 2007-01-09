/* -*- mode: C; fill-column: 100; c-basic-offset: 4; -*-
 *****************************************************************************
 *
 * This example source code introduces a libcurl-based C library
 * buffered I/O interface to URLs.  It supports fopen(), fread(),
 * fgets(), feof(), fclose(), rewind().
 *
 * Using this code you replace your program's fopen() with url_fopen()
 * and it becomes possible to read and write remote streams instead of
 * local files.
 *
 * Coyright (c)2003 Simtec Electronics
 *
 * Re-implemented by Vincent Sanders <vince@kyllikki.org> with extensive
 * reference to original curl example code
 *
 * Enhanced by Brent Baccala <cosine@freesoft.org> (2006) to implement
 * writes as well as reads and to work with the GNU C library's custom
 * FILE cookies.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * This example requires libcurl 7.9.7 or later and the GNU C library.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

#include "url_fopen.h"

#include <curl/curl.h>

struct curl_cookie
{
    CURL *curl;
    CURLM *multi_handle;

    char operation;		/* read (r), write (w), or append (a) */
    char *URL;			/* we malloc this ourselves */

    char *buffer;               /* buffer to store cached data */
    int buffer_len;             /* currently allocated buffers length */
    int buffer_pos;             /* end of data in buffer */
    int still_running;          /* Is background url fetch still in progress */
};


/* curl calls this routine to get more data */
static size_t
write_callback(char *buffer,
               size_t size,
               size_t nitems,
               void *userp)
{
    char *newbuff;
    int rembuff;

    struct curl_cookie *cookie = (struct curl_cookie *) userp;
    size *= nitems;

    rembuff=cookie->buffer_len - cookie->buffer_pos;//remaining space in buffer

    if(size > rembuff)
    {
        //not enuf space in buffer
        newbuff=realloc(cookie->buffer,cookie->buffer_len + (size - rembuff));
        if(newbuff==NULL)
        {
            fprintf(stderr,"callback buffer grow failed\n");
            size=rembuff;
        }
        else
        {
            /* realloc suceeded increase buffer size*/
            cookie->buffer_len+=size - rembuff;
            cookie->buffer=newbuff;

            /*printf("Callback buffer grown to %d bytes\n",cookie->buffer_len);*/
        }
    }

    memcpy(&cookie->buffer[cookie->buffer_pos], buffer, size);
    cookie->buffer_pos += size;

    /*fprintf(stderr, "callback %d size bytes\n", size);*/

    return size;
}

/* curl calls this routine to send more data */
static size_t
read_callback(char *buffer,
               size_t size,
               size_t nitems,
               void *userp)
{
    struct curl_cookie *cookie = (struct curl_cookie *) userp;

    size *= nitems;

    if (size > cookie->buffer_len) size = cookie->buffer_len;

    memcpy(buffer, cookie->buffer, size);
    cookie->buffer += size;
    cookie->buffer_len -= size;

    /* fprintf(stderr, "read_callback returns %d\n", size); */

    return size;
}

/* use to attempt to fill the read buffer up to requested number of bytes */
static int
fill_buffer(struct curl_cookie *cookie,int want,int waittime)
{
    fd_set fdread;
    fd_set fdwrite;
    fd_set fdexcep;
    int maxfd;
    struct timeval timeout;
    int rc;

    /* only attempt to fill buffer if transactions still running and buffer
     * doesnt exceed required size already
     */
    if((!cookie->still_running) || (cookie->buffer_pos > want))
        return 0;

    /* attempt to fill buffer */
    do
    {
        FD_ZERO(&fdread);
        FD_ZERO(&fdwrite);
        FD_ZERO(&fdexcep);

        /* set a suitable timeout to fail on */
        timeout.tv_sec = 60; /* 1 minute */
        timeout.tv_usec = 0;

        /* get file descriptors from the transfers */
        curl_multi_fdset(cookie->multi_handle, &fdread, &fdwrite, &fdexcep, &maxfd);

        /* In a real-world program you OF COURSE check the return code of the
           function calls, *and* you make sure that maxfd is bigger than -1
           so that the call to select() below makes sense! */

	if (maxfd >= 0) {

	    rc = select(maxfd+1, &fdread, &fdwrite, &fdexcep, &timeout);

	    if ((rc == -1) && (errno != EINTR)) {
		perror("select");
		break;
	    }
	}

	/* no sockets to wait on or timeout or readable/writable sockets */
	/* note we *could* be more efficient and not wait for
	 * CURLM_CALL_MULTI_PERFORM to clear here and check it on re-entry
	 * but that gets messy */
	while(curl_multi_perform(cookie->multi_handle, &cookie->still_running) ==
	      CURLM_CALL_MULTI_PERFORM);

    } while(cookie->still_running && (cookie->buffer_pos < want));
    return 1;
}

/* use to remove want bytes from the front of a files buffer */
static int
use_buffer(struct curl_cookie *cookie,int want)
{
    /* sort out buffer */
    if((cookie->buffer_pos - want) <=0)
    {
        /* ditch buffer - write will recreate */
        if(cookie->buffer)
            free(cookie->buffer);

        cookie->buffer=NULL;
        cookie->buffer_pos=0;
        cookie->buffer_len=0;
    }
    else
    {
        /* move rest down make it available for later */
        memmove(cookie->buffer,
                &cookie->buffer[want],
                (cookie->buffer_pos - want));

        cookie->buffer_pos -= want;
    }
    return 0;
}


int url_close (void *ptr)
{
    fd_set fdread;
    fd_set fdwrite;
    fd_set fdexcep;
    int maxfd;
    int rc;
    struct curl_cookie *cookie = ptr;
    int ret=0;/* default is good return */

    /* fprintf(stderr, "Cleaner\n"); */

    /* For a write or append, block until transfer done */

    while((cookie->operation != 'r') && cookie->still_running)
    {
        FD_ZERO(&fdread);
        FD_ZERO(&fdwrite);
        FD_ZERO(&fdexcep);

        /* get file descriptors from the transfers */
        curl_multi_fdset(cookie->multi_handle, &fdread, &fdwrite, &fdexcep, &maxfd);

        /* In a real-world program you OF COURSE check the return code of the
           function calls, *and* you make sure that maxfd is bigger than -1
           so that the call to select() below makes sense! */

	if (maxfd >= 0) {

	    rc = select(maxfd+1, &fdread, &fdwrite, &fdexcep, NULL);

	    if ((rc == -1) && (errno != EINTR)) {
		perror("select");
		ret = -1;
		break;
	    }
	}

	/* no sockets to wait on or timeout or readable/writable sockets */
	/* note we *could* be more efficient and not wait for
	 * CURLM_CALL_MULTI_PERFORM to clear here and check it on re-entry
	 * but that gets messy */
	while(curl_multi_perform(cookie->multi_handle, &cookie->still_running) ==
	      CURLM_CALL_MULTI_PERFORM);
    }

    /* make sure the easy handle is not in the multi handle anymore */
    curl_multi_remove_handle(cookie->multi_handle, cookie->curl);
    curl_multi_cleanup(cookie->multi_handle);

    /* cleanup */
    curl_easy_cleanup(cookie->curl);

    if(cookie->buffer) {
        free(cookie->buffer);/* free any allocated buffer space */
	cookie->buffer = NULL;
    }

    free(cookie);

    return ret;
}

ssize_t url_read(void *ptr, char *buffer, size_t size)
{
    size_t want;
    struct curl_cookie *cookie = ptr;

    want = size;

    fill_buffer(cookie,want,1);

    /* check if theres data in the buffer - if not fill_buffer()
     * either errored or EOF */
    if(!cookie->buffer_pos)
	return 0;

    /* ensure only available data is considered */
    if(cookie->buffer_pos < want)
	want = cookie->buffer_pos;

    /* xfer data to caller */
    memcpy(buffer, cookie->buffer, want);

    use_buffer(cookie,want);

    /* fprintf(stderr, "(fread) return %d bytes %d left\n", want,cookie->buffer_pos); */

    return want;
}

static int start_cookie(struct curl_cookie *cookie);

#ifdef O_LARGEFILE

int url_seekptr64 (void *ptr, off64_t *position, int whence)
{
    struct curl_cookie *cookie = ptr;

    /* only support rewinds, for now */

    if ((whence != SEEK_SET) || (*position != 0)) return -1;

    curl_multi_remove_handle(cookie->multi_handle, cookie->curl);
    curl_multi_cleanup(cookie->multi_handle);
    curl_easy_cleanup(cookie->curl);
    cookie->still_running = 0;
    cookie->buffer_pos = 0;

    /* printf("(url rewind)\n"); */

    return start_cookie(cookie);
}

off64_t url_seek64 (void *ptr, off64_t position, int whence)
{
    if (url_seekptr64(ptr, &position, whence) == -1) return (off64_t)-1;
    else return position;
}

#endif

int url_seekptr (void *ptr, off_t *position, int whence)
{
    struct curl_cookie *cookie = ptr;

    /* only support rewinds, for now */

    if ((whence != SEEK_SET) || (*position != 0)) return -1;

    curl_multi_remove_handle(cookie->multi_handle, cookie->curl);
    curl_multi_cleanup(cookie->multi_handle);
    curl_easy_cleanup(cookie->curl);
    cookie->still_running = 0;
    cookie->buffer_pos = 0;

    /* printf("(url rewind)\n"); */

    return start_cookie(cookie);
}

off_t url_seek (void *ptr, off_t position, int whence)
{
    if (url_seekptr(ptr, &position, whence) == -1) return (off_t)-1;
    else return position;
}

ssize_t url_write(void *ptr, const char *buffer, size_t size)
{
    fd_set fdread;
    fd_set fdwrite;
    fd_set fdexcep;
    int maxfd;
    int rc;

    struct curl_cookie *cookie = ptr;

    /* fprintf(stderr, "Writer\n"); */

    if (!cookie->still_running) return -1;

    cookie->buffer = (char *) buffer;
    cookie->buffer_len = size;

    /* Try to transfer without blocking */

    while((curl_multi_perform(cookie->multi_handle, &cookie->still_running) ==
	   CURLM_CALL_MULTI_PERFORM) && (cookie->buffer_len > 0));

    /* Now block until we've transfered the entire buffer */

    while((cookie->still_running) && (cookie->buffer_len > 0))
    {
        FD_ZERO(&fdread);
        FD_ZERO(&fdwrite);
        FD_ZERO(&fdexcep);

	/* fprintf(stderr, "Blocking in writer\n"); */

        /* get file descriptors from the transfers */
        curl_multi_fdset(cookie->multi_handle, &fdread, &fdwrite, &fdexcep, &maxfd);

	if (maxfd == -1) break;

        /* In a real-world program you OF COURSE check the return code of the
           function calls, *and* you make sure that maxfd is bigger than -1
           so that the call to select() below makes sense! */

        rc = select(maxfd+1, &fdread, &fdwrite, &fdexcep, NULL);

	if (rc == -1) break;

        switch(rc) {
        case -1:
            /* select error */
            break;

        case 0:
            break;

        default:
            /* timeout or readable/writable sockets */
            /* note we *could* be more efficient and not wait for
             * CURLM_CALL_MULTI_PERFORM to clear here and check it on re-entry
             * but that gets messy */
            while(curl_multi_perform(cookie->multi_handle, &cookie->still_running) ==
                  CURLM_CALL_MULTI_PERFORM);

            break;
        }
    }

    cookie->buffer = NULL;

    return (size - cookie->buffer_len);
}

static int start_cookie(struct curl_cookie *cookie)
{
    if ((cookie->operation != 'r') && (cookie->operation != 'w') && (cookie->operation != 'a')) {
	errno = EINVAL;
	return -1;
    }

    cookie->curl = curl_easy_init();

    /* I sometimes turn on the VERBOSE option to debug this thing.
     *
     * The FORBID_REUSE option is here to avoid a problem I had when
     * you attempt to transfer files (possibly more than one) by
     * reading part of the file, then coming back and reading the
     * whole thing again.  Some combination provokes a "426 Illegal
     * Seek" from my FTP server; FORBID_REUSE fixes this.
     *
     * The FRESH_CONNECT option is here to avoid a problem I had
     * rewinding an HTTP session.
     */

    curl_easy_setopt(cookie->curl, CURLOPT_URL, cookie->URL);
    curl_easy_setopt(cookie->curl, CURLOPT_VERBOSE, 0);
    curl_easy_setopt(cookie->curl, CURLOPT_FRESH_CONNECT, 1);
    curl_easy_setopt(cookie->curl, CURLOPT_FORBID_REUSE, 1);

    /* Curl's sense of 'read' and 'write' is backwards from ours */

    if (cookie->operation == 'r') {
	curl_easy_setopt(cookie->curl, CURLOPT_WRITEDATA, cookie);
	curl_easy_setopt(cookie->curl, CURLOPT_WRITEFUNCTION, write_callback);
    } else {
	curl_easy_setopt(cookie->curl, CURLOPT_READDATA, cookie);
	curl_easy_setopt(cookie->curl, CURLOPT_READFUNCTION, read_callback);
	curl_easy_setopt(cookie->curl, CURLOPT_UPLOAD, 1);
	if (cookie->operation == 'a') {
	  curl_easy_setopt(cookie->curl, CURLOPT_FTPAPPEND, 1);
	}
    }

    cookie->multi_handle = curl_multi_init();

    curl_multi_add_handle(cookie->multi_handle, cookie->curl);

    cookie->still_running = 1;

    if (cookie->operation == 'r') {

	/* lets start the fetch */
	while(curl_multi_perform(cookie->multi_handle, &cookie->still_running) ==
	      CURLM_CALL_MULTI_PERFORM );

	if((cookie->buffer_pos == 0) && (!cookie->still_running))
	{
	    /* if still_running is 0 now, we should return NULL */

	    /* make sure the easy handle is not in the multi handle anymore */
	    curl_multi_remove_handle(cookie->multi_handle, cookie->curl);
	    curl_multi_cleanup(cookie->multi_handle);

	    /* cleanup */
	    curl_easy_cleanup(cookie->curl);

	    /* XXX sometimes want to free buffer, too */
	    free(cookie->URL);
	    free(cookie);

	    /* it was probably a bad URL */
	    errno = EINVAL;
	    return -1;
	}
    }

    return 0;
}

void * url_open(char *url,const char *operation)
{
    /* this code could check for URLs or types in the 'url' and
       basicly use the real fopen() for standard files */

    struct curl_cookie *cookie;

    if ((operation[0] != 'r') && (operation[0] != 'w') && (operation[0] != 'a')) {
	errno = EINVAL;
	return NULL;
    }

    cookie = (struct curl_cookie *) malloc(sizeof(struct curl_cookie));

    if (cookie == NULL) {
	errno = ENOMEM;
	return NULL;
    }

    memset(cookie, 0, sizeof(struct curl_cookie));

    cookie->URL = malloc(strlen(url) + 1);
    if (cookie->URL == NULL) {
	errno = ENOMEM;
	return NULL;
    }
    strcpy(cookie->URL, url);
    cookie->operation = operation[0];

    if (start_cookie(cookie) == -1) return NULL;

    return cookie;
}

#ifdef __GLIBC__

static cookie_io_functions_t read_functions = {url_read, NULL, url_seekptr64, url_close};
static cookie_io_functions_t write_functions = {NULL, url_write, NULL, url_close};

FILE * url_fopen(char *url,const char *operation)
{
    /* this code could check for URLs or types in the 'url' and
       basicly use the real fopen() for standard files */

    FILE *file;
    struct curl_cookie *cookie;

    cookie = (struct curl_cookie *) url_open(url, operation);

    if (operation[0] == 'r') {
	file = fopencookie(cookie, operation, read_functions);
    } else {
	file = fopencookie(cookie, operation, write_functions);
    }

    return file;
}

#endif
