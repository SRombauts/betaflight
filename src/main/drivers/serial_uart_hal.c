/*
 * This file is part of Cleanflight and Betaflight.
 *
 * Cleanflight and Betaflight are free software. You can redistribute
 * this software and/or modify this software under the terms of the
 * GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Cleanflight and Betaflight are distributed in the hope that they
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Authors:
 * jflyper - Refactoring, cleanup and made pin-configurable
 * Dominic Clifton - Serial port abstraction, Separation of common STM32 code for cleanflight, various cleanups.
 * Hamasaki/Timecop - Initial baseflight code
*/
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "platform.h"

#ifdef USE_UART

#include "build/build_config.h"
#include "build/atomic.h"

#include "common/utils.h"
#include "drivers/io.h"
#include "drivers/nvic.h"
#include "drivers/inverter.h"
#include "drivers/dma.h"
#include "drivers/rcc.h"

#include "drivers/serial.h"
#include "drivers/serial_uart.h"
#include "drivers/serial_uart_impl.h"

static void usartConfigurePinInversion(uartPort_t *uartPort) {
    bool inverted = uartPort->port.options & SERIAL_INVERTED;

    if (inverted)
    {
        if (uartPort->port.mode & MODE_RX)
        {
            uartPort->Handle.AdvancedInit.AdvFeatureInit |= UART_ADVFEATURE_RXINVERT_INIT;
            uartPort->Handle.AdvancedInit.RxPinLevelInvert = UART_ADVFEATURE_RXINV_ENABLE;
        }
        if (uartPort->port.mode & MODE_TX)
        {
            uartPort->Handle.AdvancedInit.AdvFeatureInit |= UART_ADVFEATURE_TXINVERT_INIT;
            uartPort->Handle.AdvancedInit.TxPinLevelInvert = UART_ADVFEATURE_TXINV_ENABLE;
        }
    }
}

// XXX uartReconfigure does not handle resource management properly.

void uartReconfigure(uartPort_t *uartPort)
{
    /*RCC_PeriphCLKInitTypeDef RCC_PeriphClkInit;
    RCC_PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USART1|RCC_PERIPHCLK_USART2|RCC_PERIPHCLK_USART3|
            RCC_PERIPHCLK_UART4|RCC_PERIPHCLK_UART5|RCC_PERIPHCLK_USART6|RCC_PERIPHCLK_UART7|RCC_PERIPHCLK_UART8;
    RCC_PeriphClkInit.Usart1ClockSelection = RCC_USART1CLKSOURCE_SYSCLK;
    RCC_PeriphClkInit.Usart2ClockSelection = RCC_USART2CLKSOURCE_SYSCLK;
    RCC_PeriphClkInit.Usart3ClockSelection = RCC_USART3CLKSOURCE_SYSCLK;
    RCC_PeriphClkInit.Uart4ClockSelection = RCC_UART4CLKSOURCE_SYSCLK;
    RCC_PeriphClkInit.Uart5ClockSelection = RCC_UART5CLKSOURCE_SYSCLK;
    RCC_PeriphClkInit.Usart6ClockSelection = RCC_USART6CLKSOURCE_SYSCLK;
    RCC_PeriphClkInit.Uart7ClockSelection = RCC_UART7CLKSOURCE_SYSCLK;
    RCC_PeriphClkInit.Uart8ClockSelection = RCC_UART8CLKSOURCE_SYSCLK;
    HAL_RCCEx_PeriphCLKConfig(&RCC_PeriphClkInit);*/

    HAL_UART_DeInit(&uartPort->Handle);
    uartPort->Handle.Init.BaudRate = uartPort->port.baudRate;
    // according to the stm32 documentation wordlen has to be 9 for parity bits
    // this does not seem to matter for rx but will give bad data on tx!
    uartPort->Handle.Init.WordLength = (uartPort->port.options & SERIAL_PARITY_EVEN) ? UART_WORDLENGTH_9B : UART_WORDLENGTH_8B;
    uartPort->Handle.Init.StopBits = (uartPort->port.options & SERIAL_STOPBITS_2) ? USART_STOPBITS_2 : USART_STOPBITS_1;
    uartPort->Handle.Init.Parity = (uartPort->port.options & SERIAL_PARITY_EVEN) ? USART_PARITY_EVEN : USART_PARITY_NONE;
    uartPort->Handle.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    uartPort->Handle.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
    uartPort->Handle.Init.Mode = 0;

    if (uartPort->port.mode & MODE_RX)
        uartPort->Handle.Init.Mode |= UART_MODE_RX;
    if (uartPort->port.mode & MODE_TX)
        uartPort->Handle.Init.Mode |= UART_MODE_TX;


    usartConfigurePinInversion(uartPort);

#ifdef TARGET_USART_CONFIG
    void usartTargetConfigure(uartPort_t *);
    usartTargetConfigure(uartPort);
#endif

    if (uartPort->port.options & SERIAL_BIDIR)
    {
        HAL_HalfDuplex_Init(&uartPort->Handle);
    }
    else
    {
        HAL_UART_Init(&uartPort->Handle);
    }

    // Receive DMA or IRQ
    if (uartPort->port.mode & MODE_RX)
    {
#ifdef USE_DMA
        if (uartPort->rxDMAResource)
        {
            uartPort->rxDMAHandle.Instance = (DMA_ARCH_TYPE *)uartPort->rxDMAResource;
#if !defined(STM32H7)
            uartPort->rxDMAHandle.Init.Channel = uartPort->rxDMAChannel;
#else 
            uartPort->txDMAHandle.Init.Request = uartPort->rxDMARequest;
#endif
            uartPort->rxDMAHandle.Init.Direction = DMA_PERIPH_TO_MEMORY;
            uartPort->rxDMAHandle.Init.PeriphInc = DMA_PINC_DISABLE;
            uartPort->rxDMAHandle.Init.MemInc = DMA_MINC_ENABLE;
            uartPort->rxDMAHandle.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
            uartPort->rxDMAHandle.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
            uartPort->rxDMAHandle.Init.Mode = DMA_CIRCULAR;
            uartPort->rxDMAHandle.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
            uartPort->rxDMAHandle.Init.FIFOThreshold = DMA_FIFO_THRESHOLD_1QUARTERFULL;
            uartPort->rxDMAHandle.Init.PeriphBurst = DMA_PBURST_SINGLE;
            uartPort->rxDMAHandle.Init.MemBurst = DMA_MBURST_SINGLE;
            uartPort->rxDMAHandle.Init.Priority = DMA_PRIORITY_MEDIUM;


            HAL_DMA_DeInit(&uartPort->rxDMAHandle);
            HAL_DMA_Init(&uartPort->rxDMAHandle);
            /* Associate the initialized DMA handle to the UART handle */
            __HAL_LINKDMA(&uartPort->Handle, hdmarx, uartPort->rxDMAHandle);

            HAL_UART_Receive_DMA(&uartPort->Handle, (uint8_t*)uartPort->port.rxBuffer, uartPort->port.rxBufferSize);

            uartPort->rxDMAPos = __HAL_DMA_GET_COUNTER(&uartPort->rxDMAHandle);
        } else
#endif
        {
            /* Enable the UART Parity Error Interrupt */
            SET_BIT(uartPort->USARTx->CR1, USART_CR1_PEIE);

            /* Enable the UART Error Interrupt: (Frame error, noise error, overrun error) */
            SET_BIT(uartPort->USARTx->CR3, USART_CR3_EIE);

            /* Enable the UART Data Register not empty Interrupt */
            SET_BIT(uartPort->USARTx->CR1, USART_CR1_RXNEIE);

            /* Enable Idle Line detection */
            SET_BIT(uartPort->USARTx->CR1, USART_CR1_IDLEIE);
        }
    }

    // Transmit DMA or IRQ
    if (uartPort->port.mode & MODE_TX) {
#ifdef USE_DMA
        if (uartPort->txDMAResource) {
            uartPort->txDMAHandle.Instance = (DMA_ARCH_TYPE *)uartPort->txDMAResource;
#if !defined(STM32H7)
            uartPort->txDMAHandle.Init.Channel = uartPort->txDMAChannel;
#else 
            uartPort->txDMAHandle.Init.Request = uartPort->txDMARequest;
#endif
            uartPort->txDMAHandle.Init.Direction = DMA_MEMORY_TO_PERIPH;
            uartPort->txDMAHandle.Init.PeriphInc = DMA_PINC_DISABLE;
            uartPort->txDMAHandle.Init.MemInc = DMA_MINC_ENABLE;
            uartPort->txDMAHandle.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
            uartPort->txDMAHandle.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
            uartPort->txDMAHandle.Init.Mode = DMA_NORMAL;
            uartPort->txDMAHandle.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
            uartPort->txDMAHandle.Init.FIFOThreshold = DMA_FIFO_THRESHOLD_1QUARTERFULL;
            uartPort->txDMAHandle.Init.PeriphBurst = DMA_PBURST_SINGLE;
            uartPort->txDMAHandle.Init.MemBurst = DMA_MBURST_SINGLE;
            uartPort->txDMAHandle.Init.Priority = DMA_PRIORITY_MEDIUM;


            HAL_DMA_DeInit(&uartPort->txDMAHandle);
            HAL_StatusTypeDef status = HAL_DMA_Init(&uartPort->txDMAHandle);
            if (status != HAL_OK)
            {
                while (1);
            }
            /* Associate the initialized DMA handle to the UART handle */
            __HAL_LINKDMA(&uartPort->Handle, hdmatx, uartPort->txDMAHandle);

            __HAL_DMA_SET_COUNTER(&uartPort->txDMAHandle, 0);
        } else
#endif
        {

            /* Enable the UART Transmit Data Register Empty Interrupt */
            SET_BIT(uartPort->USARTx->CR1, USART_CR1_TXEIE);
        }
    }
    return;
}

serialPort_t *uartOpen(UARTDevice_e device, serialReceiveCallbackPtr callback, void *callbackData, uint32_t baudRate, portMode_e mode, portOptions_e options)
{
    uartPort_t *s = serialUART(device, baudRate, mode, options);

    if (!s) {
        return (serialPort_t *)s;
    }

#ifdef USE_DMA
    s->txDMAEmpty = true;
#endif

    // common serial initialisation code should move to serialPort::init()
    s->port.rxBufferHead = s->port.rxBufferTail = 0;
    s->port.txBufferHead = s->port.txBufferTail = 0;
    // callback works for IRQ-based RX ONLY
    s->port.rxCallback = callback;
    s->port.rxCallbackData = callbackData;
    s->port.mode = mode;
    s->port.baudRate = baudRate;
    s->port.options = options;

    uartReconfigure(s);

    return (serialPort_t *)s;
}

void uartSetBaudRate(serialPort_t *instance, uint32_t baudRate)
{
    uartPort_t *uartPort = (uartPort_t *)instance;
    uartPort->port.baudRate = baudRate;
    uartReconfigure(uartPort);
}

void uartSetMode(serialPort_t *instance, portMode_e mode)
{
    uartPort_t *uartPort = (uartPort_t *)instance;
    uartPort->port.mode = mode;
    uartReconfigure(uartPort);
}

#ifdef USE_DMA
void uartTryStartTxDMA(uartPort_t *s)
{
    ATOMIC_BLOCK(NVIC_PRIO_SERIALUART_TXDMA) {
        if (IS_DMA_ENABLED(s->txDMAResource)) {
            // DMA is already in progress
            return;
        }

        HAL_UART_StateTypeDef state = HAL_UART_GetState(&s->Handle);
        if ((state & HAL_UART_STATE_BUSY_TX) == HAL_UART_STATE_BUSY_TX) {
            return;
        }

        if (s->port.txBufferHead == s->port.txBufferTail) {
            // No more data to transmit
            s->txDMAEmpty = true;
            return;
        }

        uint16_t size;
        uint32_t fromwhere = s->port.txBufferTail;

        if (s->port.txBufferHead > s->port.txBufferTail) {
            size = s->port.txBufferHead - s->port.txBufferTail;
            s->port.txBufferTail = s->port.txBufferHead;
        } else {
            size = s->port.txBufferSize - s->port.txBufferTail;
            s->port.txBufferTail = 0;
        }
        s->txDMAEmpty = false;

        HAL_UART_Transmit_DMA(&s->Handle, (uint8_t *)&s->port.txBuffer[fromwhere], size);
    }
}
#endif

uint32_t uartTotalRxBytesWaiting(const serialPort_t *instance)
{
    uartPort_t *s = (uartPort_t*)instance;

#ifdef USE_DMA
    if (s->rxDMAResource) {
        uint32_t rxDMAHead = __HAL_DMA_GET_COUNTER(s->Handle.hdmarx);

        if (rxDMAHead >= s->rxDMAPos) {
            return rxDMAHead - s->rxDMAPos;
        } else {
            return s->port.rxBufferSize + rxDMAHead - s->rxDMAPos;
        }
    }
#endif

    if (s->port.rxBufferHead >= s->port.rxBufferTail) {
        return s->port.rxBufferHead - s->port.rxBufferTail;
    } else {
        return s->port.rxBufferSize + s->port.rxBufferHead - s->port.rxBufferTail;
    }
}

uint32_t uartTotalTxBytesFree(const serialPort_t *instance)
{
    uartPort_t *s = (uartPort_t*)instance;

    uint32_t bytesUsed;

    if (s->port.txBufferHead >= s->port.txBufferTail) {
        bytesUsed = s->port.txBufferHead - s->port.txBufferTail;
    } else {
        bytesUsed = s->port.txBufferSize + s->port.txBufferHead - s->port.txBufferTail;
    }

#ifdef USE_DMA
    if (s->txDMAResource) {
        /*
         * When we queue up a DMA request, we advance the Tx buffer tail before the transfer finishes, so we must add
         * the remaining size of that in-progress transfer here instead:
         */
        bytesUsed += __HAL_DMA_GET_COUNTER(s->Handle.hdmatx);

        /*
         * If the Tx buffer is being written to very quickly, we might have advanced the head into the buffer
         * space occupied by the current DMA transfer. In that case the "bytesUsed" total will actually end up larger
         * than the total Tx buffer size, because we'll end up transmitting the same buffer region twice. (So we'll be
         * transmitting a garbage mixture of old and new bytes).
         *
         * Be kind to callers and pretend like our buffer can only ever be 100% full.
         */
        if (bytesUsed >= s->port.txBufferSize - 1) {
            return 0;
        }
    }
#endif

    return (s->port.txBufferSize - 1) - bytesUsed;
}

bool isUartTransmitBufferEmpty(const serialPort_t *instance)
{
    const uartPort_t *s = (uartPort_t *)instance;
#ifdef USE_DMA
    if (s->txDMAResource)
        return s->txDMAEmpty;
    else
#endif
        return s->port.txBufferTail == s->port.txBufferHead;
}

uint8_t uartRead(serialPort_t *instance)
{
    uint8_t ch;
    uartPort_t *s = (uartPort_t *)instance;

#ifdef USE_DMA
    if (s->rxDMAResource) {
        ch = s->port.rxBuffer[s->port.rxBufferSize - s->rxDMAPos];
        if (--s->rxDMAPos == 0)
            s->rxDMAPos = s->port.rxBufferSize;
    } else
#endif
    {
        ch = s->port.rxBuffer[s->port.rxBufferTail];
        if (s->port.rxBufferTail + 1 >= s->port.rxBufferSize) {
            s->port.rxBufferTail = 0;
        } else {
            s->port.rxBufferTail++;
        }
    }

    return ch;
}

void uartWrite(serialPort_t *instance, uint8_t ch)
{
    uartPort_t *s = (uartPort_t *)instance;

    s->port.txBuffer[s->port.txBufferHead] = ch;

    if (s->port.txBufferHead + 1 >= s->port.txBufferSize) {
        s->port.txBufferHead = 0;
    } else {
        s->port.txBufferHead++;
    }

#ifdef USE_DMA
    if (s->txDMAResource) {
        uartTryStartTxDMA(s);
    } else
#endif
    {
        __HAL_UART_ENABLE_IT(&s->Handle, UART_IT_TXE);
    }
}

const struct serialPortVTable uartVTable[] = {
    {
        .serialWrite = uartWrite,
        .serialTotalRxWaiting = uartTotalRxBytesWaiting,
        .serialTotalTxFree = uartTotalTxBytesFree,
        .serialRead = uartRead,
        .serialSetBaudRate = uartSetBaudRate,
        .isSerialTransmitBufferEmpty = isUartTransmitBufferEmpty,
        .setMode = uartSetMode,
        .setCtrlLineStateCb = NULL,
        .setBaudRateCb = NULL,
        .writeBuf = NULL,
        .beginWrite = NULL,
        .endWrite = NULL,
    }
};

#ifdef USE_UART1
// USART1 Rx/Tx IRQ Handler
void USART1_IRQHandler(void)
{
    uartPort_t *s = &(uartDevmap[UARTDEV_1]->port);
    uartIrqHandler(s);
}
#endif

#ifdef USE_UART2
// USART2 Rx/Tx IRQ Handler
void USART2_IRQHandler(void)
{
    uartPort_t *s = &(uartDevmap[UARTDEV_2]->port);
    uartIrqHandler(s);
}
#endif

#ifdef USE_UART3
// USART3 Rx/Tx IRQ Handler
void USART3_IRQHandler(void)
{
    uartPort_t *s = &(uartDevmap[UARTDEV_3]->port);
    uartIrqHandler(s);
}
#endif

#ifdef USE_UART4
// UART4 Rx/Tx IRQ Handler
void UART4_IRQHandler(void)
{
    uartPort_t *s = &(uartDevmap[UARTDEV_4]->port);
    uartIrqHandler(s);
}
#endif

#ifdef USE_UART5
// UART5 Rx/Tx IRQ Handler
void UART5_IRQHandler(void)
{
    uartPort_t *s = &(uartDevmap[UARTDEV_5]->port);
    uartIrqHandler(s);
}
#endif

#ifdef USE_UART6
// USART6 Rx/Tx IRQ Handler
void USART6_IRQHandler(void)
{
    uartPort_t *s = &(uartDevmap[UARTDEV_6]->port);
    uartIrqHandler(s);
}
#endif

#ifdef USE_UART7
// UART7 Rx/Tx IRQ Handler
void UART7_IRQHandler(void)
{
    uartPort_t *s = &(uartDevmap[UARTDEV_7]->port);
    uartIrqHandler(s);
}
#endif

#ifdef USE_UART8
// UART8 Rx/Tx IRQ Handler
void UART8_IRQHandler(void)
{
    uartPort_t *s = &(uartDevmap[UARTDEV_8]->port);
    uartIrqHandler(s);
}
#endif
#endif
