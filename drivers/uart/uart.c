/**
 * @file uart.c
 * Implements UART and LPUART support for STM32L4xxxx
 */
#include <stdlib.h>
#include <string.h>

#include <device/device.h>
#include <util/ringbuf.h>
#include <sys/isr.h>
#include <util/util.h>

#include "uart.h"

/**
 * UART device state
 */
typedef enum {
    UART_dev_closed = 0,
    UART_dev_open = 1,
} UART_state_t;

/**
 * Configuration structure for UART devices
 */
typedef struct {
    UART_config_t cfg;   /*!< User configuration for UART */
    USART_TypeDef *regs; /*!< Register access for this UART */
    UART_state_t state;  /*!< UART state (open or closed) */
    RingBuf_t write_buf; /*!< UART write ring buffer */
    RingBuf_t read_buf;  /*!< UART read ring buffer */
} UART_periph_status_t;

#define UART_RINGBUF_SIZE 80

static UART_periph_status_t UARTS[NUM_UARTS] = {0};
static uint8_t UART_RBUFFS[NUM_UARTS][UART_RINGBUF_SIZE];
static uint8_t UART_WBUFFS[NUM_UARTS][UART_RINGBUF_SIZE];

static void UART_interrupt(UART_periph_t source);
/**
 * Opens a UART or LPUART device for read/write access
 * @param periph: Identifier of UART to open
 * @param config: UART configuration structure
 * @param err: Set on function error
 * @return NULL on error, or a UART handle to the open peripheral
 */
UART_handle_t UART_open(UART_periph_t periph, UART_config_t *config,
                        syserr_t *err) {
    UART_periph_status_t *handle;
    *err = SYS_OK; // Set no error until one occurs
    /**
     * Check parameters. Note that due to limitations on the range of the
     * LPUART1_BRR register, LPUART1 cannot support low baud rates
     * without switching its clock source to LSE or HSI16
     */
    if (periph == LPUART_1 && config->UART_baud_rate < UART_baud_38400) {
        *err = ERR_NOSUPPORT;
        return NULL;
    }
    handle = &UARTS[periph];
    if (handle->state == UART_dev_open) {
        *err = ERR_INUSE;
        return NULL;
    }
    // Set handle state to open
    handle->state = UART_dev_open;
    memcpy(config, &handle->cfg, sizeof(UART_config_t));
    // Setup read and write buffers
    buf_init(&handle->read_buf, UART_RBUFFS[periph], UART_RINGBUF_SIZE);
    buf_init(&handle->write_buf, UART_WBUFFS[periph], UART_RINGBUF_SIZE);
    /**
     * Record the UART peripheral address into the config structure
     * Here we also enable the clock for the relevant UART device
     */
    switch (periph) {
    case LPUART_1:
        SETBITS(RCC->APB1ENR2, RCC_APB1ENR2_LPUART1EN);
        handle->regs = LPUART1;
        break;
    case USART_1:
        SETBITS(RCC->APB2ENR, RCC_APB2ENR_USART1EN);
        handle->regs = USART1;
        break;
    case USART_2:
        SETBITS(RCC->APB1ENR1, RCC_APB1ENR1_USART2EN);
        handle->regs = USART2;
        break;
    case USART_3:
        SETBITS(RCC->APB1ENR1, RCC_APB1ENR1_USART3EN);
        handle->regs = USART3;
        break;
    default:
        *err = ERR_BADPARAM;
        return NULL;
        break;
    }
    /**
     * Configure the UART module according to the UART config provided
     * Register description can be found at p.1238 of datasheet
     */
    /* Configure the number of bits */
    CLEARBITS(handle->regs->CR1, USART_CR1_M_Msk);
    switch (handle->cfg.UART_wordlen) {
    case UART_word_7n1:
        SETBITS(handle->regs->CR1, USART_CR1_M1);
        break;
    case UART_word_8n1:
        // Bit M0 and M1 should be zero, and they have been cleared
        break;
    case UART_word_9n1:
        SETBITS(handle->regs->CR1, USART_CR1_M0);
        break;
    default:
        *err = ERR_BADPARAM;
        return NULL;
        break;
    }
    /* Configure the number of stop bits */
    CLEARBITS(handle->regs->CR2, USART_CR2_STOP_Msk);
    switch (handle->cfg.UART_stopbit) {
    case UART_onestop:
        // Bitfield of 0b00 sets one stop bit
        break;
    case UART_twostop:
        SETBITS(handle->regs->CR2, USART_CR2_STOP_1);
        break;
    default:
        *err = ERR_BADPARAM;
        return NULL;
        break;
    }
    /* Configure the parity setting */
    switch (handle->cfg.UART_parity) {
    case UART_parity_disabled:
        // Ensure PCE bit is cleared
        CLEARBITS(handle->regs->CR1, USART_CR1_PCE);
        break;
    case UART_parity_even:
        // Set PCE bit and clear PS bit
        SETBITS(handle->regs->CR1, USART_CR1_PCE);
        CLEARBITS(handle->regs->CR1, USART_CR1_PS);
        break;
    case UART_parity_odd:
        // Set PCE and PS bits
        SETBITS(handle->regs->CR1, USART_CR1_PS | USART_CR1_PCE);
        break;
    default:
        *err = ERR_BADPARAM;
        return NULL;
        break;
    }
    /* Configure pinswap (swaps TX and RX pins) */
    switch (handle->cfg.UART_pin_swap) {
    case UART_pin_normal:
        CLEARBITS(handle->regs->CR2, USART_CR2_SWAP);
        break;
    case UART_pin_swapped:
        SETBITS(handle->regs->CR2, USART_CR2_SWAP);
        break;
    default:
        *err = ERR_BADPARAM;
        return NULL;
        break;
    }
    /* Select between UART MSB and LSB */
    switch (handle->cfg.UART_bit_order) {
    case UART_lsb_first:
        CLEARBITS(handle->regs->CR2, USART_CR2_MSBFIRST);
        break;
    case UART_msb_first:
        SETBITS(handle->regs->CR2, USART_CR2_MSBFIRST);
        break;
    default:
        *err = ERR_BADPARAM;
        return NULL;
        break;
    }
    /* Configure UART flow control */
    switch (handle->cfg.UART_flowcontrol) {
    case UART_no_flow:
        // Just need to disable the relevant bit fields
        CLEARBITS(handle->regs->CR3, USART_CR3_CTSE | USART_CR3_RTSE);
        break;
    case UART_flow_control:
        // Enable the UART flow control bits
        SETBITS(handle->regs->CR3, USART_CR3_CTSE | USART_CR3_RTSE);
        break;
    default:
        *err = ERR_BADPARAM;
        return NULL;
        break;
    }
    /**
     * Baud rate configuration. See p.1210 for baud rate formula. For
     * 16x oversampling (default) LPUART1 will use 256*fck/LPUARTDIV.
     * USARTx will use fck/USARTDIV. Default clock source for UART devices is
     * PCLK, running at 80MHz, but can be changed using the RCC_CCIPR register
     *
     * Values used below are taken from datasheet pg.1274
     */
    if (periph == LPUART_1) {
        switch (handle->cfg.UART_baud_rate) {
        case UART_baud_38400:
            handle->regs->BRR = 0x82355;
            break;
        case UART_baud_57600:
            handle->regs->BRR = 0x56CE3;
            break;
        case UART_baud_115200:
            handle->regs->BRR = 0x2B671;
            break;
        default:
            *err = ERR_BADPARAM;
            return NULL;
            break;
        }
    } else {
        // For all other UARTS, use the standard baud rate equation
        switch (handle->cfg.UART_baud_rate) {
        case UART_baud_auto:
            /* Special case. Write a starter value to the BRR register */
            handle->regs->BRR = 0x2B6; // 115200 baud
            break;
        case UART_baud_1200:
            handle->regs->BRR = 0x1046B;
            break;
        case UART_baud_2400:
            handle->regs->BRR = 0x8236;
            break;
        case UART_baud_4800:
            handle->regs->BRR = 0x411B;
            break;
        case UART_baud_9600:
            handle->regs->BRR = 0x208E;
            break;
        case UART_baud_19200:
            handle->regs->BRR = 0x1047;
            break;
        case UART_baud_38400:
            handle->regs->BRR = 0x824;
            break;
        case UART_baud_57600:
            handle->regs->BRR = 0x56D;
            break;
        case UART_baud_115200:
            handle->regs->BRR = 0x2B6;
            break;
        }
    }
    // Now, enable the UART
    SETBITS(handle->regs->CR1, USART_CR1_UE);
    // If auto bauding is enabled, here we need to request it
    if (handle->cfg.UART_baud_rate == UART_baud_auto) {
        SETBITS(handle->regs->CR2, USART_CR2_ABREN);
    }
    // Enable the transmitter and receiver
    SETBITS(handle->regs->CR1, USART_CR1_TE);
    SETBITS(handle->regs->CR1, USART_CR1_RE);
    // Register interrupt handler
    set_UART_isr(UART_interrupt); 
    // Enable transmit and receive interrupts
    SETBITS(handle->regs->CR1, USART_CR1_TXEIE);
    SETBITS(handle->regs->CR1, USART_CR1_RXNEIE);
    return handle;
}

/**
 * Reads data from a UART or LPUART device
 * @param handle: UART handle to access
 * @param buf: Buffer to read data into
 * @param len: buffer length
 * @param err: Set on error
 * @return number of bytes read, or -1 on error
 */
int UART_read(UART_handle_t handle, uint8_t *buf, uint32_t len, syserr_t *err);

/**
 * Writes data to a UART or LPUART device
 * @param handle: UART handle to access
 * @param buf: buffer to write data from
 * @param len: buffer length
 * @param err: set on error
 * @return number of bytes written, or -1 on error
 */
int UART_write(UART_handle_t handle, uint8_t *buf, uint32_t len, syserr_t *err);

/**
 * Handles UART interrupts
 * @param source: UART device generating interrupt
 */
static void UART_interrupt(UART_periph_t source) {
    USART_TypeDef *uart;
    switch (source) {
    case LPUART_1:
        uart = LPUART1;
        break;
    case USART_1:
        uart = USART1;
        break;
    case USART_2:
        uart = USART2;
        break;
    case USART_3:
        uart = USART3;
        break;
    default:
        // Cannot handle this interrupt.
        return;
        break;
    }
    /**
     * Now determine what flag caused the interrupt. We need to check for
     * the TXE and RXNE bits
     */
    if (READBITS(uart->ISR, USART_ISR_RXNE)) {
        // We have 
    }
}