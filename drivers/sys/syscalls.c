/**
 * @file syscalls.c
 * Implements all operating system subroutines expected by red hat's newlib
 *
 * Note that many of these implementations are stubs, since many portions
 * of a full operating system are not supported.
 */

/* RedHat re-entrant errno include */
#include <errno.h>
#undef errno
extern int errno;

#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/times.h>

#include <config.h>
#include <gpio/gpio.h>
#include <sys/err.h>
#include <uart/uart.h>

extern char _ebss; // Defined by linker

/** Minimal environment implementation */
char *__env[1] = {0};
char **environ = __env;

static char *current_sbrk = &_ebss;
static UART_handle_t uart_logger = NULL;

/**
 * Exits the system.
 * @param status: Exit code
 */
void _exit(int status) {
    if (SYSEXIT == SYSEXIT_MIN) {
        while (1)
            ;
    } else {
        printf("System exited with code %i\n", status);
    }
}
/**
 * Sets the system break. Required for dynamic memory allocation.
 * @param incr: Increment to raise program break by, in bytes
 * @return new program break
 */
void *_sbrk(int incr) {
    void *old_brk;
    if (SYSHEAPSIZE != 0) {
        old_brk = current_sbrk;
        // Set the new break
        current_sbrk += incr;
        // Todo: check for stack collision
        return old_brk;
    } else {
        // No memory allocation
        return (void *)-1;
    }
}

/**
 * Writes to a system device. In this implementation, the only system device
 * available is the UART logger, or semihosting if it is enabled.
 * @param fd: File descriptor
 * @param buf: buffer to write to device
 * @param count: number of bytes to write from buf
 * @return -1 on error, or number of bytes written on success
 */
int _write(int file, char *buf, int len) {
    syserr_t err;
    int remaining = len;
    int ret;
    if (SYSLOG == SYSLOG_LPUART1) {
        ret = UART_write(uart_logger, (unsigned char *)buf, len, &err);
        if (ret == -1) {
            errno = err;
        }
        return len;
    } else if (SYSLOG == SYSLOG_SEMIHOST) {
        while (remaining--) {
            /**
             * Ensure buf is in r1, then call bkpt instruction with semihosting
             * immediate. Set r0 to 0x03 to indicate a WRITEC operation
             */
            asm("mov r0, #0x03\n"
                "mov r1, %0\n"
                "bkpt 0xAB\n" 
                : 
                : "r"(buf)
                : "r0", "r1");
            // Advance the buffer
            buf++;
        }
        return len;
    } else {
        // No defined way to write to system device
        return -1;
    }
}

/* Libc initialization handlers */

#if SYSLOG == SYSLOG_LPUART1
// Define constructor and destructor for LPUART1

/**
 * Initializes LPUART1 for use as a serial logger
 */
static void lpuart_init(void) {
    syserr_t ret;
    UART_config_t lpuart_config = UART_DEFAULT_CONFIG;
    GPIO_config_t uart_pincofig = GPIO_DEFAULT_CONFIG;
    /* Both GPIO pins should be pulled up and have high output speeds */
    uart_pincofig.alternate_func = GPIO_af8; // Per device datasheet, need AF8
    uart_pincofig.mode = GPIO_mode_afunc;
    uart_pincofig.output_speed = GPIO_speed_vhigh;
    uart_pincofig.pullup_pulldown = GPIO_pullup;
    // PA2 is TX pin
    ret = GPIO_config(GPIO_PORT_A, GPIO_PIN_2, &uart_pincofig);
    if (ret != SYS_OK) {
        while (1)
            ; // spin
    }
    ret = GPIO_config(GPIO_PORT_A, GPIO_PIN_3, &uart_pincofig);
    if (ret != SYS_OK) {
        while (1)
            ; // spin
    }
    lpuart_config.UART_baud_rate = UART_baud_115200;
    lpuart_config.UART_wordlen = UART_word_8n1;
    lpuart_config.UART_textmode = UART_txtmode_en;
    uart_logger = UART_open(LPUART_1, &lpuart_config, &ret);
    if (ret != SYS_OK || uart_logger == NULL) {
        while (1)
            ; // spin
    }
}

/**
 * Closes LPUART1 at exit
 */
static void lpuart_deinit(void) { UART_close(uart_logger); }

#endif

/**
 * Performs system initialization that requires libc. Called at boot
 * by __libc_init_array()
 */
void _init(void) {
#if SYSLOG == SYSLOG_LPUART1
    // Call LPUART1 constuctor
    lpuart_init();
#endif
}

/**
 * Performs system deinitialization that requires libc. Called by exit()
 */
void _fini(void) {
#if SYSLOG == SYSLOG_LPUART1
    // Call LPUART1 destructor
    lpuart_deinit();
#endif
}

/* All handlers defined below are "stubs" simply provided to link correctly */

/**
 * Reads from a file. Stub implementation.
 * @param file: file descriptor
 * @param ptr: buffer to read into
 * @param len: length to read
 * @return -1 on error, or number of bytes read on success
 */
int _read(int file, char *ptr, int len) { return 0; }

/**
 * Closes a file. Stub implementation as we have no filesystem
 * @param file: file descriptor to close
 * @return -1 on error, 0 on success
 */
int _close(int file) { return -1; }

/**
 * Transfer control to a new process. Stub implementation.
 * @param name: Process name
 * @param argv: Process arguments
 * @param env: process environment
 * @return -1 on error. Does not return on success.
 */
int _execve(char *name, char **argv, char **env) {
    errno = ENOMEM;
    return -1;
}

/**
 * Creates a new process. Stub implementation
 * @return -1 on error, 0 on success (or child pid to parent process)
 */
int _fork(void) {
    errno = EAGAIN;
    return -1;
}

/**
 * Stats a file. Stub implementation
 * @param file file descriptor to stat
 * @param st stat structure
 * @return 0 on success, or -1 on error
 */
int _fstat(int file, struct stat *st) {
    st->st_mode = S_IFCHR;
    return 0;
}

/**
 * Status of a file by name. Stub implementation.
 * @param file: name of file
 * @param st: stat structure
 * @return 0 on success, or -1 on error
 */
int _stat(const char *file, struct stat *st) {
    st->st_mode = S_IFCHR;
    return 0;
}

/**
 * Gets a process PID. Stub implementation
 * @return process pid
 */
int _getpid(void) { return 1; }

/**
 * Checks if a device is a tty. For this code, all devices are terminals
 * @param file: file descriptor to check
 * @return 1 if the device is tty, or 0 if not
 */
int _isatty(int file) { return 1; }

/**
 * Kills a process. Stub implementation.
 * @param pid: process ID to kill
 * @param sig: signal to send process
 * @return 0 on success, or -1 on error
 */
int _kill(int pid, int sig) {
    errno = EINVAL;
    return -1;
}

/**
 * Seeks to a position in a file. Sub implementation.
 * @param file: file descriptor
 * @param ptr: file offset to seek to
 * @param whence: controls seek behavior
 * @return offset location on success, or -1 on error
 */
int _lseek(int file, int ptr, int dir) { return 0; }

/**
 * Open a file. Stub implementation.
 * @param name: filename to open
 * @param flag: file flags
 * @param mode: file mode
 * @return file descriptor on success, or -1 on error
 */
int _open(const char *name, int flags, int mode) { return -1; }

/**
 * Process timing information. Stub implementation
 * @param buf: Timing buffer
 * @return -1 on error, or number of clock ticks elapsed on success
 */
clock_t _times(struct tms *buf) { return -1; }

/**
 * Remove a file's directory entry. Stub implementation
 * @param name: filename to delete
 * @return 0 on success, or -1 on error
 */
int _unlink(char *name) {
    errno = ENOENT;
    return -1;
}

/**
 * Wait for a child process. Stub implementation
 * @param status: filled with exit status of process
 * @return -1 on error, or process ID of termated child on success
 */
int _wait(int *status) {
    errno = ECHILD;
    return -1;
}