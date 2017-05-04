/*
 * Copyright (c) 2010-2012 Helsinki Institute for Information Technology
 * and University of Helsinki.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * @author Samu Varjonen
 *
 * @note Functionality is based on the HIPL's (www.infrahip.net) debug.c
 */

/**
 * Feature test macro struct ifreq */
#define _BSD_SOURCE

#include <stdio.h>
#include <stdarg.h>
#include <syslog.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <net/if.h>

#include "debug.h"
#include "constants.h"
#include "configuration.h"

/**
 * Syslog option that defines that we include PIDs to the log messages.
 */
#define SYSLOG_OPT        (LOG_PID)
/**
 * Syslog facility that defines that we log to the local level 6.
 */
#define SYSLOG_FACILITY   LOG_LOCAL6

/**
 * A map used to map debug symbols to syslog.
 * @note Must be in the same order as enum debug_level (straight mapping).
 * @note XTRA is mapped to LOG_DEBUG also.
 */
const int debug2syslog_map[] = { LOG_ALERT,
                                 LOG_ERR,
                                 LOG_INFO,
                                 LOG_DEBUG,
                                 LOG_DEBUG};

/**
 * Prefixes used by the debugging.
 * @note Must be in the same order as enum debug_level (straight mapping).
 */
const char *debug_prefix[] = { "DIE", "ERR", "INF", "DBG", "XTR"};

/**
 * Human readable entity names.
 * @note Must be in the same order as enum debug_level (straight mapping).
 */
const char *daemon_prefix[] = { "", " MEPD", " MIPD", " OAMSTATD", " OAMTOOL"};

/** 
 * Production quality code prints debugging stuff on syslog, testing code
 * prints interactively on stderr. Third type LOGTYPE_NOLOG is not listed
 * here and it should not be used. By default the stderr is used.
 */
static enum logtype_t logtype       = LOGTYPE_STDERR;

/**
 * Daemon types none, mepd, mipd, oamstatd. 
 * By default DAEMONTYPE_NONE is used.
 */
static enum daemontype_t daemontype = DAEMONTYPE_NONE;

/**
 * What is debugged. By default all.
 */
static enum logdebug_t logdebug     = LOGDEBUG_ALL;
 
/**
 * Variable used to transfer bootcount around the daemon 
 */
int daemon_bootcount;

/**
 * Maximum number of nodes allowed in memory read from config
 */
int max_meps;

/**
 * Holds the path information to the used bootcount file.
 */
char *oam_bootcount_path = "/tmp/";

/**
 * The running state of the daemon, by default we are stopped.
 */
int oam_daemon_state = OAM_STOPPED;

/**
 * a pointer to the control information database.
 */
struct oam_db  *oam_control_info = NULL;


/**
 * How many interfaces there is 
 */
int ifs_len;

/**
 * Buffer for the interface information.
 */
struct oam_if ifs[20];

/**
 * Lookup table for the CRC32 checksums.
 */
uint32_t crc32_table[256]; 

/**
 * Structure containing information that defines the identity of this entity
 *
 * MEGID
 *
 * An ICC-based ID that concists of 1 to 6 char identifier (ITU) and 
 * of UMC that is an unsigned integer (5 characters long.
 *
 * ICC (1-6):
 * ELISA, JIPPII, CUBIO, TELE2, NEVA, RSLFIN, SONERA, SONGN, TELDA
 *
 * UMC (7-12): matter of organization to which ICC has been assigned
 * MUST be unique.
 */
struct oam_entity *entity;

/**
 * Buffer for multicast DA class 1
 */
unsigned char da1[6];

/**
 * Buffer for multicast DA class 2
 */
unsigned char da2[6];

/**
 * pointer to the log file.
 */
FILE *stat_log = NULL;

/**
 * handle errors generated by log handling
 *
 * @param log_type the type of the log that generated the 
 *                 error (LOGTYPE_STDERR or LOGTYPE_SYSLOG)
 *
 * @note The default policy is to ignore errors (an alternative policy would
 * be to e.g. exit).
 * @note Do not use this function outside of this file at all.
 *
 */
static void oam_handle_log_error(int log_type)
{
    fprintf(stderr, "log (type=%d) failed, ignoring\n", log_type);
}

/**
 * "multiplexer" for correctly outputting all debug messages
 *
 * @param debug_level the urgency of the message (DEBUG_LEVEL_XX)
 * @param file the file from where the debug call was made
 * @param line the line of the debug call in the source file
 * @param function the name of function where the debug call is located
 * @param fmt the output format of the debug message as in printf(3)
 * @param args the variable argument list to be output
 * @note This function is to be used only from the oam_print_str
 * debugging function. Do not use outside of this file.
 */
static void oam_vlog(int debug_level, const char *file, const int line,
                     const char *function, const char *fmt, va_list args)
{
    char syslog_msg[DEBUG_MSG_MAX_LEN] = "";
    int syslog_level                   = debug2syslog_map[debug_level];
    char prefix[DEBUG_PREFIX_MAX]      = "\0";
    int printed                        = 0;

    printed = snprintf(prefix, DEBUG_PREFIX_MAX, "%s (%s:%d@%s)",
                       debug_prefix[debug_level],   
		       file, line, function);

    switch (logtype) {
    case LOGTYPE_NOLOG:
        break;
    case LOGTYPE_STDERR:
        if (strlen(prefix) > 0) {
            printed = fprintf(stderr, "%s: ", prefix);
            if (printed < 0) {
                goto err;
            }
        } else {
            /* LOGFMT_SHORT: no prefix */
        }

        printed = vfprintf(stderr, fmt, args);
        if (printed < 0) {
            goto err;
        }
        break;
    case LOGTYPE_SYSLOG:
        openlog(NULL, SYSLOG_OPT, SYSLOG_FACILITY);
        printed = vsnprintf(syslog_msg, DEBUG_MSG_MAX_LEN, fmt, args);
        syslog(syslog_level | SYSLOG_FACILITY, "%s %s", prefix, syslog_msg);
        /* the result of vsnprintf depends on glibc version; handle them both
         * (note about barriers: printed has \0 excluded,
         * DEBUG_MSG_MAX_LEN has \0 included) */
        if (printed < 0 || printed > DEBUG_MSG_MAX_LEN - 1) {
            syslog(syslog_level | SYSLOG_FACILITY,
                   "%s", "previous msg was truncated!!!");
        }
        closelog();
        break;
    default:
        printed = fprintf(stderr, "oam_vlog(): undefined logtype: %d", logtype);
        exit(1);
    }

    /* logging was succesful */
    return;

err:
    oam_handle_log_error(logtype);
}

/**
 * a wrapper that filters unnecessary logging for oam_vlog()
 *
 * @param debug_level the debug level
 * @param file a file handle where to print
 * @param line the line number
 * @param function the calling function
 * @param fmt printf formatting options
 * @param ... variable number of strings or integers to print
 *        according to the @c fmt parameter
 * @note Do not call this function outside of this file at all.
 */

void oam_print_str(int debug_level,
                   const char *file,
                   int line,
                   const char *function,
                   const char *fmt,
                   ...)
{
    va_list args;
    va_start(args, fmt);
    if ((debug_level == DEBUG_LEVEL_INFO && logdebug != LOGDEBUG_NONE) ||
        (debug_level == DEBUG_LEVEL_XTRA && logdebug == LOGDEBUG_XTRA) ||
        (debug_level == DEBUG_LEVEL_XTRA && logdebug == LOGDEBUG_ALL) ||
        (debug_level == DEBUG_LEVEL_DEBUG && logdebug == LOGDEBUG_ALL) ||
        (debug_level == DEBUG_LEVEL_ERROR && logdebug != LOGDEBUG_NONE) ||
        (debug_level == DEBUG_LEVEL_DIE)) {
        oam_vlog(debug_level, file, line, function, fmt, args);
    }
    va_end(args);
}

/**
 * @brief Sets logging to stderr or syslog.
 *
 * Defines where the daemon DEBUG, INFO, ERROR etc. messages are printed.
 *
 * @param new_logtype the type of logging output, either LOGTYPE_STDERR or
 *                    LOGTYPE_SYSLOG
 */
void oam_set_logtype(int new_logtype)
{
    logtype = new_logtype;
    /* Setting the Daemontype to none as the syslog already tells this info*/
    if (new_logtype == LOGTYPE_SYSLOG) {
	 oam_set_daemontype(DAEMONTYPE_NONE);
    }
}
/**
 * Function to set the log level to decired level.
 *
 * @param level String that describes the level
 *
 * @note allowed levels all, medium, none
 */
void oam_set_loglevel(char *level)
{
     if (!strcmp(level, "all")) {
         OAM_INFO("Loglevel set to all\n");
         logdebug = LOGDEBUG_ALL;
     } else if (!strcmp(level, "medium")) {
         OAM_INFO("Loglevel set to medium\n");
         logdebug = LOGDEBUG_MEDIUM;
     } else if (!strcmp(level, "xtra")) {
         OAM_INFO("Loglevel set to XTRA\n");
         logdebug = LOGDEBUG_XTRA;
     } else if (!strcmp(level, "none")) {
         OAM_INFO("Loglevel set to none\n");
         logdebug = LOGDEBUG_NONE;
     } else {
         OAM_INFO("Incorrect loglevel (%s) setting to medium\n", level);
         logdebug = LOGDEBUG_MEDIUM;
     }
}

/**
 * Get the logtype currently in use.
 *
 * @return integer representing the logtype.
 * @see enum logtype_t
 */
int oam_get_logtype() {
     return logtype;
}
 
/**
 * @brief Sets the daemon type.
 *
 * Defines which daemon this is for the macros (MEPD, MIPD, OAMSTATD).
 *
 * @param new_daemontype the type of daemon string for debug  output, either DAEMONTYPE_MEP, DAEMONTYPE_MIP, DAEMONTYPE_OAMSTAT, or DAEMONTYPE_NONE.
 */
void oam_set_daemontype(int new_daemontype)
{
    daemontype = new_daemontype;
}

/**
 * Convert a 4-bit value to a hexadecimal string representation.
 *
 * @param q     the 4-bit value (an integer between 0 and 15) to convert to hex.
 * @return      1 byte of hexadecimal output as a string character.
 */
static char oam_quad2hex(const char q)
{
    if (q < 10) {
        return '0' + q;
    } else {
        return 'A' + (q - 10);
    }
}


/**
 * Convert a single byte to a hexadecimal string representation.
 *
 * @param c     the byte to convert to hex.
 * @param bfr   the buffer to write the 2 bytes of hexadecimal output to.
 */
static void oam_byte2hex(const char c, char *bfr)
{
    const int high_quad = (c & 0xF0) >> 4;
    const int low_quad  = c & 0x0F;
    *bfr       = oam_quad2hex(high_quad);
    *(bfr + 1) = oam_quad2hex(low_quad);
}

/**
 * Write the hexadecimal string representation of a memory area to a buffer.
 * At most @a in_len bytes of the input memory are read and at most @a out_len bytes of th
e output buffer are written, whichever comes first.
 * If at least one byte was converted, the hexadecimal representation is terminated by 0.
 * If the memory regions of @a in and @a out overlap, the result of the operation is undef
ined.
 *
 * @param in        the address of the start of the memory area to convert.
 * @param in_len    the size in bytes of the memory area to convert.
 * @param out       the address of the buffer to write the hexadecimal representation to.
 * @param out_len   the size in bytes of the out buffer to write to.
 * @return          the number of bytes from in that were converted.
 */
static size_t oam_mem2hex(const void *in, const size_t in_len, char *out, const size_t out_len)
{
    if (in_len > 0 && out_len > 2) {
        const unsigned char *in_cur  = in;
        const unsigned char *in_end  = in_cur + in_len;
        char                *out_cur = out;
        const char          *out_end = out_cur + out_len;

        // terminate if either we reach the end of in or if there is not enough room to write another hex digit and the terminating 0 into out.
        while (in_cur < in_end && out_cur <= (out_end - 3)) {
            oam_byte2hex(*in_cur, out_cur);

            in_cur  += 1;
            out_cur += 2;
        }

        *out_cur = '\0';

        return in_cur - (const unsigned char *) in;
    } else {
        return 0;
    }
}

/**
 * Print raw hexdump starting from address @c str of length @c len. Do not call
 * this function from the outside of the debug module, use the HIP_HEXDUMP macro
 * instead.
 *
 * @param file     the file from where the debug call was made
 * @param line     the line of the debug call in the source file
 * @param function the name of function where the debug call is located
 * @param prefix   the prefix string will printed before the hexdump
 * @param str      pointer to the beginning of the data to be hexdumped
 * @param len      the length of the data to be hexdumped
 */
void oam_hexdump(const char *file, int line, const char *function,
                 const char *prefix, const void *str, const int len)
{
    if (len > 0) {
        const size_t buffer_size = (len * 2) + 1;
        char        *buffer      = malloc(buffer_size);
        if (buffer != NULL) {
            oam_mem2hex(str, len, buffer, buffer_size);
            oam_print_str(DEBUG_LEVEL_DEBUG, file, line, function, "%s0x%s\n", prefix, buffer);
            free(buffer);
        } else {
            OAM_DIE("memory allocation failed\n");
        }
    }
}

/**
 * output a fatal error and exit
 *
 * @param file the file from where the debug call was made
 * @param line the line of the debug call in the source file
 * @param function the name of function where the debug call is located
 * @param fmt the output format of the debug message as in printf(3)
 *
 * @note The variable size argument list (...) is used as in printf(3).
 * Do not call this function from the outside of the debug module,
 * use the OAM_DIE macro instead.
 */
void oam_die(const char *file, int line, const char *function,
             const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    oam_print_str(DEBUG_LEVEL_DIE, file, line, function, fmt, args);
    va_end(args);
    exit(1);
}

/**
 * Defines the maximum log line length.
 */
#define STAT_LINE_SIZE_MAX 1024
/**
 * Defines the maximum length of the prefix used in logging.
 */
#define STAT_PREFIX_SIZE_MAX 64

/**
 * Statistics printing 
 *
 * @param fmt the output format of the debug message as in printf(3)
 *
 * @note do NOT use directly use OAM_STAT macro instead.
 */
void stat_print(const char *fmt, ...) 
{ 
    int err;
    va_list args;
    char stat_msg[STAT_LINE_SIZE_MAX];
    char prefix[STAT_PREFIX_SIZE_MAX];
    char time_string[OAM_TIME_STRING_LEN];
    time_t now;
    extern FILE *stat_log;

    memset(stat_msg, '\0', sizeof(stat_msg));
    memset(prefix, '\0', sizeof(prefix));

    va_start(args, fmt);
    now = time(0);
    err = oam_format_time(&now, time_string, sizeof(time_string)); 
    if (err) {
        OAM_DEBUG("Problems in time formatting\n");
    }
    sprintf(prefix, "%s:",
                time_string);
    vsnprintf(stat_msg, STAT_LINE_SIZE_MAX, fmt, args);
    
    fprintf(stat_log, "%s %s", prefix, stat_msg); 
    fflush(stat_log); 

    va_end(args);
}

/**
 * Give now in formatted form 
 * 
 * @param time_to_format Time to format
 * @param time_string Where the time is stored
 * @param length of the buffer for formatted time
 *
 * @return 0 on success, non-zero else 
 *
 * @note Give at least "2011-12-12 11:11:11\0" space
 */
int oam_format_time(time_t *time_to_format, char *time_string, int length) 
{
    struct tm *timeinfo;
    if (length < OAM_TIME_STRING_LEN) {
        OAM_ERROR("Too short buffer for time conversion\n");
    }
    memset(time_string, '\0', sizeof(time_string));
    timeinfo = localtime(time_to_format);
    strftime(time_string, 
             length, 
             "%Y-%m-%d %H:%M:%S", 
             timeinfo);
    return 0;    
}

/**
 * Debug print the current logging level
 */
void oam_print_current_loglevel(void) {
    switch (logdebug) {
    case LOGDEBUG_ALL:
        OAM_INFO("Debug level is ALL\n");
        break;
    case LOGDEBUG_MEDIUM:
        OAM_INFO("Debug level is MEDIUM\n");
        break;
    case LOGDEBUG_XTRA:
        OAM_XTRA("Debug level is XTRA\n");
        break;
    case LOGDEBUG_NONE:
        /* None */
        break;
    }
}