 /*
 *  Copyright (c) 2003-2004 Atheros Communications, Inc., All Rights Reserved
 *
 *  Transmit HAL functions
 */

#ifdef BUILD_AR5513

#ident "$Id: //depot/sw/branches/AV_dev/src/hal/ar5513/ar5513Transmit.c#20 $"

/* Standard HAL Headers */
#include "wlantype.h"
#include "wlandrv.h"
#include "wlanPhy.h"
#include "wlanchannel.h"
#include "wlanframe.h"
#include "wlanSend.h"
#include "wlanext.h"
#include "halApi.h"
#include "hal.h"
#include "halDevId.h"
#include "ui.h"
#include "display.h"
#include "ratectrl.h"
#include "vport.h"

/* Headers for HW private items */
#include "ar5513MacReg.h"
#include "ar5513Beacon.h"
#include "ar5513Reset.h"
#include "ar5513KeyCache.h"
#include "ar5513Transmit.h"
#include "ar5513Misc.h"
#include "ar5513Mac.h"

#include "pktlog.h"

#ifdef BUILD_AP
#include "ar5hwc.h"  /* for some debug flags only - should get rid */

#define SHOW_RETRIES 1
#endif

#if defined(AR5513)
#include "ar5513reg.h"  /* ar5513 system definitions */
#endif /* AR5513 */

#define COMP_FRAME_LEN_THRESHOLD    256         /* no compression for frames longer than this threshold */

/* number of rates a frame can be tried */
#define MAX_RATE_SERIES (4)

#ifdef WME
#define MULTI_RATE_RETRY_ENABLE 1
static A_BOOL multiRateRetryEnable = 1;
#else
static A_BOOL multiRateRetryEnable = 0;
#endif

#if defined(DEBUG) && defined(MULTI_RATE_RETRY_ENABLE)
#define MULTI_RATE_DEBUG 
#endif

#ifdef MULTI_RATE_DEBUG
A_UINT32 multiRateDebugLevel = 0;
typedef struct TxRateSeries{
    A_UINT32    requestedRate;
    A_UINT32    transmittedRate;
} TX_RATE_SERIES_STAT;
TX_RATE_SERIES_STAT txRateSeriesStat[MAX_RATE_SERIES];
#endif

/*
**  Transmit RSSI Combining table for dual chain RSSI
**  computations. This algorithm and lookup table was 
**  provided by the Algorithm team. The algorithm matches
**  that implemented in hardware. Falcon descriptors 
**  fail to include this piece of information.
*/

#define TX_RSSI_COMBINE_TABLE_SIZE  10
static A_RSSI txRssiTable[TX_RSSI_COMBINE_TABLE_SIZE] = {
    3, 3, 2, 2, 1, 1, 1, 1, 1, 1
};

static INLINE void
ar5513SetupDescBurst(WLAN_DEV_INFO *pdevInfo, ATHEROS_DESC *pTxDesc,WLAN_PHY phyType);

static void
ar5513SetIntVeolInTxDesc(WLAN_DEV_INFO *pdevInfo, ATHEROS_DESC *pDesc, A_BOOL value, A_BOOL queued);

/**************************************************************
 * ar5513UpdateTxTrigLevel
 *
 * Update Tx FIFO trigger level.
 *
 * Set bIncTrigLevel to TRUE to increase the trigger level.
 * Set bIncTrigLevel to FALSE to decrease the trigger level.
 *
 * Returns TRUE if the trigger level was updated
 */
A_BOOL
ar5513UpdateTxTrigLevel(WLAN_DEV_INFO *pDev, A_BOOL bIncTrigLevel)
{
    UINT32 regValue, curTrigLevel;

    ASSERT(pDev);

    /* Disable chip interrupts. This is because halUpdateTrigLevel
     * is called from both ISR and non-ISR contexts.
     */
    halDisableInterrupts(pDev, HAL_INT_GLOBAL);

    regValue     = readPlatformReg(pDev, MAC_TXCFG);
    curTrigLevel = (regValue & MAC_FTRIG_M) >> MAC_FTRIG_S;

    pDev->txPrefetchStats.dmaUnderrunCount++;
    if (pDev->txPrefetchStats.hungCount == 0) {
        A_REG_WR(pDev, MAC_D_FPCTL, MAC_D_FPCTL_PREFETCH_EN |
                 A_FIELD_VALUE(MAC_D_FPCTL, DCU, 0) |
                 A_FIELD_VALUE(MAC_D_FPCTL, BURST_PREFETCH, 1));
    }

    if (bIncTrigLevel) {
        /* Increase the trigger level if not already at the maximum */
        if (curTrigLevel < MAX_TX_FIFO_THRESHOLD) {
            /* increase the trigger level */
            curTrigLevel++;
        } else {
            /* no update to the trigger level */

            /* re-enable chip interrupts */
            halEnableInterrupts(pDev, HAL_INT_GLOBAL);

            return FALSE;
        }
    } else {

        /* decrease the trigger level if not already at the minimum */
        if (curTrigLevel > MIN_TX_FIFO_THRESHOLD) {
            /* decrease the trigger level */
            curTrigLevel--;
        } else {
            /* no update to the trigger level */

            /* re-enable chip interrupts */
            halEnableInterrupts(pDev, HAL_INT_GLOBAL);

            return FALSE;
        }
    }

    /* Update the trigger level */
    writePlatformReg(pDev, MAC_TXCFG, (regValue & (~MAC_FTRIG_M)) |
                     ((curTrigLevel << MAC_FTRIG_S) & MAC_FTRIG_M));

    /* re-enable chip interrupts */
    halEnableInterrupts(pDev, HAL_INT_GLOBAL);

    return TRUE;
}

/**************************************************************************
 * ar5513ResetTxQueue
 *
 * Set the retry, aifs, cwmin/max, readyTime regs for specified queue
 * Assumes:
 *  phwChannel has been set to point to the current channel
 */
static INLINE void
ar5513ResetTxQueue(WLAN_DEV_INFO *pDev, int queueNum, HAL_TX_QUEUE_INFO *queueInfo)
{
    A_UINT32 queueOffset = queueNum * sizeof(A_UINT32);
    A_UINT32 retryReg, value;

    if (queueInfo->mode == TXQ_MODE_INACTIVE) {
        return;
    }

    /* set cwMin/Max and AIFS values */
    writePlatformReg(pDev, MAC_D0_LCL_IFS + queueOffset,
                     A_FIELD_VALUE(MAC_D_LCL_IFS, CWMIN, LOG_TO_CW(queueInfo->logCwMin)) |
                     A_FIELD_VALUE(MAC_D_LCL_IFS, CWMAX, LOG_TO_CW(queueInfo->logCwMax)) |
                     A_FIELD_VALUE(MAC_D_LCL_IFS, AIFS, queueInfo->aifs));

    retryReg = A_FIELD_VALUE(MAC_D_RETRY_LIMIT, STA_SH, INIT_SSH_RETRY) |
               A_FIELD_VALUE(MAC_D_RETRY_LIMIT, STA_LG, INIT_SLG_RETRY);
    if (pDev->staConfig.swretryEnabled) {
        value = pDev->staConfig.hwTxRetries;
        if (value > MAC_D_RETRY_LIMIT_FR_SH_M) {
            value = MAC_D_RETRY_LIMIT_FR_SH_M;
        }
        retryReg |= A_FIELD_VALUE(MAC_D_RETRY_LIMIT, FR_LG, value) |
                    A_FIELD_VALUE(MAC_D_RETRY_LIMIT, FR_SH, value);
    } else {
        retryReg |= A_FIELD_VALUE(MAC_D_RETRY_LIMIT, FR_LG, INIT_LG_RETRY) |
                    A_FIELD_VALUE(MAC_D_RETRY_LIMIT, FR_SH, INIT_SH_RETRY);
    }

    /* Set retry limit values */
    writePlatformReg(pDev, MAC_D0_RETRY_LIMIT + queueOffset, retryReg);

    /* enable early termination on QCUs */
    writePlatformReg(pDev, MAC_Q0_MISC + queueOffset, MAC_Q_MISC_DCU_EARLY_TERM_REQ);

    if (queueInfo->cbrPeriod) {
        writePlatformReg(pDev, MAC_Q0_CBRCFG + queueOffset,
                A_FIELD_VALUE(MAC_Q_CBRCFG, INTERVAL, queueInfo->cbrPeriod) |
                A_FIELD_VALUE(MAC_Q_CBRCFG, OVF_THRESH, queueInfo->cbrOverflowLimit));

        writePlatformReg(pDev, MAC_Q0_MISC + queueOffset,
                readPlatformReg(pDev, MAC_Q0_MISC + queueOffset) |
                MAC_Q_MISC_FSP_CBR |
                (queueInfo->cbrOverflowLimit ? MAC_Q_MISC_CBR_EXP_CNTR_LIMIT_EN : 0));
    }

    if (queueInfo->readyTime) {
        writePlatformReg(pDev, MAC_Q0_RDYTIMECFG + queueOffset,
                A_FIELD_VALUE(MAC_Q_RDYTIMECFG, DURATION, queueInfo->readyTime) |
                MAC_Q_RDYTIMECFG_EN);
    }

    if (queueInfo->burstTime) {
        writePlatformReg(pDev, MAC_D0_CHNTIME + queueOffset,
                A_FIELD_VALUE(MAC_D_CHNTIME, DUR, queueInfo->burstTime) |
                MAC_D_CHNTIME_EN);
        if (queueInfo->qFlags & TXQ_FLAG_RDYTIME_EXP_POLICY_ENABLE) {
            writePlatformReg(pDev, MAC_Q0_MISC + queueOffset,
                                readPlatformReg(pDev, MAC_Q0_MISC + queueOffset) |
                                MAC_Q_MISC_RDYTIME_EXP_POLICY);
                }        
    }

    if (queueInfo->qFlags & TXQ_FLAG_BACKOFF_DISABLE) {
        writePlatformReg(pDev, MAC_D0_MISC + queueOffset,
                readPlatformReg(pDev, MAC_D0_MISC + queueOffset) |
                MAC_D_MISC_POST_FR_BKOFF_DIS);
    }

    if (queueInfo->qFlags & TXQ_FLAG_FRAG_BURST_BACKOFF_ENABLE) {
        writePlatformReg(pDev, MAC_D0_MISC + queueOffset,
                readPlatformReg(pDev, MAC_D0_MISC + queueOffset) |
                MAC_D_MISC_FRAG_BKOFF_EN);
    }

    if (queueInfo->mode == TXQ_MODE_BEACON) {
        /* Configure QCU for beacons */
        value = MAC_Q_MISC_FSP_DBA_GATED | MAC_Q_MISC_CBR_INCR_DIS1 | MAC_Q_MISC_BEACON_USE;
        writePlatformReg(pDev, MAC_Q0_MISC + queueOffset,
                readPlatformReg(pDev, MAC_Q0_MISC + queueOffset) | value);

        /* Configure DCU for beacons */
        value = A_FIELD_VALUE(MAC_D_MISC, ARB_LOCKOUT_CNTRL, MAC_D_MISC_ARB_LOCKOUT_CNTRL_GLOBAL) |
                MAC_D_MISC_POST_FR_BKOFF_DIS | MAC_D_MISC_BEACON_USE;
        writePlatformReg(pDev, MAC_D0_MISC + queueOffset,
                readPlatformReg(pDev, MAC_D0_MISC + queueOffset) | value);
    }

#define EXTREMEWME
#ifdef EXTREMEWME
/*
 * Yes, this is a hack and not the right way to do it, but it does get
 * the lockout bits and backoff set for the high-pri WME queues for testing.
 * we need to either extend the meaning of queueInfo->mode,
 * or create something like queueInfo->dcumode.
 */
    if (queueNum==TXQ_ID_FOR_AC2||queueNum==TXQ_ID_FOR_AC3) {
        value = A_FIELD_VALUE(MAC_D_MISC, ARB_LOCKOUT_CNTRL, MAC_D_MISC_ARB_LOCKOUT_CNTRL_GLOBAL) |
                MAC_D_MISC_POST_FR_BKOFF_DIS;
        writePlatformReg(pDev, MAC_D0_MISC + queueOffset,
                readPlatformReg(pDev, MAC_D0_MISC + queueOffset) | value);
    }
#endif
#ifdef UPSD
    if (queueNum==TXQ_ID_FOR_UPSD) {
        value = A_FIELD_VALUE(MAC_D_MISC, ARB_LOCKOUT_CNTRL, MAC_D_MISC_ARB_LOCKOUT_CNTRL_GLOBAL) |
                MAC_D_MISC_POST_FR_BKOFF_DIS;
        writePlatformReg(pDev, MAC_D0_MISC + queueOffset,
                readPlatformReg(pDev, MAC_D0_MISC + queueOffset) | value);
    }
#endif

    if (queueInfo->mode == TXQ_MODE_CAB) {
        /* Configure QCU for CAB (Crap After Beacon) frames */

        ASSERT(queueInfo->readyTime);

        /* 
         * No longer Enable MAC_Q_MISC_RDYTIME_EXP_POLICY,
         * bug #6079.  There is an issue with the CAB Queue
         * not properly refreshing the Tx descriptor if
         * the TXE clear setting is used.
         */
        value = MAC_Q_MISC_FSP_DBA_GATED | MAC_Q_MISC_CBR_INCR_DIS1 | MAC_Q_MISC_CBR_INCR_DIS0;
        writePlatformReg(pDev, MAC_Q0_MISC + queueOffset,
                readPlatformReg(pDev, MAC_Q0_MISC + queueOffset) | value);

        /* Configure DCU for CAB */
        writePlatformReg(pDev, MAC_D0_MISC + queueOffset,
                readPlatformReg(pDev, MAC_D0_MISC + queueOffset) |
                A_FIELD_VALUE(MAC_D_MISC, ARB_LOCKOUT_CNTRL, MAC_D_MISC_ARB_LOCKOUT_CNTRL_GLOBAL));
    }

    if (queueInfo->mode == TXQ_MODE_PSPOLL) {
        /*
         * We may configure psPoll QCU to be TIM-gated in the
         * future; TIM_GATED bit is not enabled currently because
         * of a hardware problem in Oahu that overshoots the TIM
         * bitmap in beacon and may find matching associd bit in
         * non-TIM elements and send PS-poll PS poll processing
         * will be done in software
         */
        writePlatformReg(pDev, MAC_Q0_MISC + queueOffset,
                readPlatformReg(pDev, MAC_Q0_MISC + queueOffset) |
                MAC_Q_MISC_CBR_INCR_DIS1);
    }

    if (queueInfo->qFlags & TXQ_FLAG_COMPRESSION_ENABLE) {
    
        if (queueInfo->physCompBuf) {
            /* 
             * set starting address of compression scratch buffer
             * enable compression in Q_MISC register 
             */
            ASSERT(queueInfo->mode != TXQ_MODE_BEACON);
            A_REG_WR(pDev, MAC_Q_CBBS, (80 + 2*queueNum));
            A_REG_WR(pDev, MAC_Q_CBBA, queueInfo->physCompBuf);
            A_REG_WR(pDev, MAC_Q_CBC,  HAL_COMP_BUF_MAX_SIZE/1024);
            A_REG_WR(pDev, MAC_Q0_MISC + queueOffset,
                     A_REG_RD(pDev, MAC_Q0_MISC + queueOffset)
                     | MAC_Q_MISC_QCU_COMP_EN);
        }
    }    

    /*
     * Always update the secondary interrupt mask registers - this
     * could be a new queue getting enabled in a running system or
     * hw getting re-initialized during a reset!
     * Since we don't differentiate between tx interrupts corresponding
     * to individual queues - secondary tx mask regs are always unmasked;
     * tx interrupts are enabled/disabled for all queues collectively
     * using the primary mask reg
     */
    A_REG_RMW_FIELD(pDev, MAC_IMR_S0, QCU_TXOK,   pDev->pHalInfo->txNormalIntMask);
    A_REG_RMW_FIELD(pDev, MAC_IMR_S1, QCU_TXERR,  pDev->pHalInfo->txNormalIntMask);
    A_REG_RMW_FIELD(pDev, MAC_IMR_S2, QCU_TXURN,  pDev->pHalInfo->txNormalIntMask);
    A_REG_RMW_FIELD(pDev, MAC_IMR_S0, QCU_TXDESC, pDev->pHalInfo->txDescIntMask);

#ifdef MULTI_RATE_DEBUG
    memset(txRateSeriesStat, 0, sizeof(TX_RATE_SERIES_STAT)*MAX_RATE_SERIES);
#endif
}


/**************************************************************
 * ar5513SetupTxQueue
 *
 * Allocates and initializes a DCU/QCU combination for tx
 *
 */
int
ar5513SetupTxQueue(WLAN_DEV_INFO *pDev, HAL_TX_QUEUE_INFO *queueInfo)
{
    int      queueNum  = queueInfo->priority;
    A_UINT32 queueMask = (1 << queueNum);

    ASSERT(queueNum < HAL_NUM_TX_QUEUES);
    ASSERT(!(pDev->pHalInfo->txQueueAllocMask & queueMask));

    pDev->pHalInfo->txQueueAllocMask |= queueMask;
    if (queueInfo->qFlags & TXQ_FLAG_TXINT_ENABLE) {
        pDev->pHalInfo->txNormalIntMask |= queueMask;
    }
    if (queueInfo->qFlags & TXQ_FLAG_TXDESCINT_ENABLE) {
        pDev->pHalInfo->txDescIntMask |= queueMask;
    }

    ar5513ResetTxQueue(pDev, queueNum, queueInfo);

    return queueNum;
}

/**************************************************************
 * ar5513ReleaseTxQueue
 *
 * Frees up a DCU/QCU combination
 *
 */
void
ar5513ReleaseTxQueue(WLAN_DEV_INFO *pDev, int queueNum)
{
    A_UINT32 queueMask = (1 << queueNum);

    ASSERT(pDev->pHalInfo->txQueueAllocMask & queueMask);

    pDev->pHalInfo->txQueueAllocMask &= ~queueMask;
    pDev->pHalInfo->txNormalIntMask  &= ~queueMask;
    pDev->pHalInfo->txDescIntMask    &= ~queueMask;
}

/**************************************************************
 * ar5513GetTxDP
 *
 * Get the TXDP for the specified queue
 */
A_UINT32
ar5513GetTxDP(WLAN_DEV_INFO *pDev, int queueNum)
{
    A_UINT32 txdpReg;

    ASSERT(pDev->pHalInfo->txQueueAllocMask & (1 << queueNum));

    txdpReg = MAC_Q0_TXDP + (queueNum * sizeof(A_UINT32));
    return (A_UINT32) A_DATA_P2V(readPlatformReg(pDev, txdpReg));
}

/**************************************************************
 * ar5513SetTxDP
 *
 * Set the TxDP for the specified queue
 */
void
ar5513SetTxDP(WLAN_DEV_INFO *pDev, int queueNum, A_UINT32 txdp)
{
    A_UINT32 txdpReg;
    A_UINT32 txeStart, txeHigh;
#if defined(DEBUG)
    A_UINT32 txdpDebug;
#endif /* DEBUG */

    ASSERT(pDev->pHalInfo->txQueueAllocMask & (1 << queueNum));

    /*
     * Make sure that TXE is deasserted before setting the TXDP.  If TXE
     * is still asserted, setting TXDP will have no effect.
     */
    if (queueNum != TXQ_ID_FOR_GBURST) {
        /* 
         * TXE Seems to high on hitting VEOL conditions in Burst Queue 
         * even if we set the ReadyTimeVEOL expiration to bring down TXE
         * back, and this seems to be happenning with selected cards 
         * Refer to Bug 10644
         */
#if defined(AR5513)
    /*
    **  XXXX - GDS DEBUG 06/14/04
    ** WMAC driver assumes that DMA is idle, however, Falcon DMA can
    ** be waiting to transmit. Thus, TXE can be stuck high.
    */
    extern int sysCountGet(void);

    txeHigh = FALSE;
    txeStart = sysCountGet();
    while ((txdpReg = readPlatformReg(pDev, MAC_Q_TXE)) & (1 << queueNum)) {
        txeHigh = TRUE;
    }

    if (txeHigh == TRUE) {
        logMsg("ar5513SetTxDP: MAC_Q_TXE stuck; %d,  0x%08lX\n",
           sysCountGet() - txeStart,txdpReg,0,0,0,0);
    }
#else
        ASSERT(!(readPlatformReg(pDev, MAC_Q_TXE) & (1 << queueNum)));
#endif 
    }

    txdpReg = MAC_Q0_TXDP + (queueNum * sizeof(A_UINT32));
    writePlatformReg(pDev, txdpReg, A_DATA_V2P(txdp));

#if defined(DEBUG)
    if ((txdpDebug = readPlatformReg(pDev, txdpReg)) != A_DATA_V2P(txdp)) {
    logMsg("ar5513SetTxDP: txdp MISMATCH -> Exp: 0x%08lX  Act: 0x%08lX\n",
           A_DATA_V2P(txdp), txdpDebug);
    ASSERT(0);
    }
#if 0
    else {
    if (queueNum != 4) {
        logMsg("ar5513SetTxDP: queue = %d  txdp = 0x%08lX\n",queueNum,
           A_DATA_V2P(txdp));
    }
    }
#endif
#endif /* DEBUG */
}

/**************************************************************
 * ar5513StartTxDma
 *
 * Set Transmit Enable bits for the specified queue
 */
void
ar5513StartTxDma(WLAN_DEV_INFO *pDev, int queueNum)
{
    A_UINT32 queueMask = (1 << queueNum);

    ASSERT(pDev->pHalInfo->txQueueAllocMask & queueMask);

    // Check to be sure we're not enabling a queue that has its TXD bit set.
    ASSERT( !(readPlatformReg(pDev, MAC_Q_TXD) & queueMask) );

    writePlatformReg(pDev, MAC_Q_TXE, queueMask);
}

/**************************************************************
 * ar5513NumTxPending
 *
 * Returns:
 *   0      if specified queue is stopped
 *   num of pending frames otherwise
 */
A_UINT32
ar5513NumTxPending(WLAN_DEV_INFO *pDev, int queueNum)
{
    A_UINT32 queueMask = (1 << queueNum);
    A_UINT32 pfcReg    = MAC_Q0_STS + (queueNum * sizeof(A_UINT32));
    A_UINT32 numPending;

    if (!(pDev->pHalInfo->txQueueAllocMask & queueMask)) {
        return 0;
    }

    numPending = readPlatformReg(pDev, pfcReg) & MAC_Q_STS_PEND_FR_CNT_M;
    if (!numPending) {
        /*
         * Pending frame count (PFC) can momentarily go to zero while TXE
         * remains asserted.  In other words a PFC of zero is not sufficient
         * to say that the queue has stopped.
         */
        if (readPlatformReg(pDev, MAC_Q_TXE) & queueMask) {
            /* arbitrarily return 1 */
            numPending = 1;
        }
    }

    return numPending;
}



/**************************************************************
 * ar5513StopTxDma
 *
 * Stop transmit on the specified queue
 */
void
ar5513StopTxDma(WLAN_DEV_INFO *pDev, int queueNum, int msec)
{
    A_UINT32 queueMask = (1 << queueNum);
    int      wait      = msec * 100;

    ASSERT(pDev->pHalInfo->txQueueAllocMask & queueMask);

    writePlatformReg(pDev, MAC_Q_TXD, queueMask);

    while (ar5513NumTxPending(pDev, queueNum)) {
        if ((--wait) < 0) {
#ifdef DEBUG
            uiPrintf("ar5513StopTxDma: failed to stop Tx DMA in %d msec\n", msec);
#endif
            break;
        }
        udelay(10);
    }
    writePlatformReg(pDev, MAC_Q_TXD, 0);
}

/**************************************************************
 * ar5513GetTxFilter
 *
 * Get tx filter from specified queue
 */
int
ar5513GetTxFilter(WLAN_DEV_INFO *pDev, int queueNum, int index)
{
    A_UINT32 txblkReg;
    A_UINT32 value;

    ASSERT(queueNum < HAL_NUM_TX_QUEUES);
    ASSERT(index < MAC_KEY_CACHE_SIZE);

    txblkReg = CALC_TXBLK_ADDR(queueNum, index);
    value    = readPlatformReg(pDev, txblkReg);
    
    if (value & (CALC_TXBLK_VALUE(index))) {
        return 1;
    } else {
        return 0;
    }
}

/**************************************************************
 * ar5513SetTxFilter
 *
 * Set tx filter on the specified queue
 */
void
ar5513SetTxFilter(WLAN_DEV_INFO *pDev, int queueNum, int index, int value)
{
    A_UINT32 txblkReg, txblkValue;
    A_UINT32 slice, bitmask;

    ASSERT(queueNum < HAL_NUM_TX_QUEUES);
    ASSERT(index < MAC_KEY_CACHE_SIZE);

    /* Individual tx filter bits are set/reset via MMR 0 */ 
    txblkReg = TXBLK_FROM_MMR(0);

    slice   = index / 8;
    bitmask = 1 << (index / 16);
    txblkValue = bitmask | (slice << MAC_D_TXBLK_WRITE_SLICE_S) |
                (value << MAC_D_TXBLK_WRITE_COMMAND_S);

    writePlatformReg(pDev, txblkReg, txblkValue);

    ASSERT(ar5513GetTxFilter(pDev, queueNum, index) == value);
}

/* Number of tx filter mmr registers */
#define HAL_TX_FILTER_MMR_SIZE  \
        ((HAL_NUM_TX_QUEUES * MAC_KEY_CACHE_SIZE) / (sizeof(A_UINT32) * 8))

/**************************************************************
 * ar5513GetTxFilterArraySize
 *
 * Get size of tx filter array (number of 32 bit entries)
 */
int
ar5513GetTxFilterArraySize(WLAN_DEV_INFO *pDev)
{
    return HAL_TX_FILTER_MMR_SIZE;
}

/**************************************************************
 * ar5513GetTxFilterArray
 *
 * Return tx filter array (MMRs)
 */
void
ar5513GetTxFilterArray(WLAN_DEV_INFO *pDev, A_UINT32 *pMmrArray, A_UINT count)
{
    A_UINT32 mmrIndex;
    A_UINT32 txblkReg;
    A_UINT32 value;

    ASSERT(count <= HAL_TX_FILTER_MMR_SIZE);

    /* Read tx filter */
    for (mmrIndex = 0; mmrIndex < count; mmrIndex++) {
        txblkReg = TXBLK_FROM_MMR(mmrIndex);
        value    = readPlatformReg(pDev, txblkReg);
        pMmrArray[mmrIndex] = value;
    }
}

/**************************************************************
 * ar5513SetTxFilterArray
 *
 * Write tx filter array (used to restore tx filter after chip reset)
 */
void
ar5513SetTxFilterArray(WLAN_DEV_INFO *pDev, A_UINT32 *pMmrArray, A_UINT count)
{
    A_UINT32 mmrIndex;
    A_UINT32 txblkReg, txblkValue;
    A_UINT32 bitmask, slice, dcu;

    ASSERT(count <= HAL_TX_FILTER_MMR_SIZE);

    /* Tx filter bits are set via MMR 0 */ 
    txblkReg = TXBLK_FROM_MMR(0);

    for (mmrIndex = 0; mmrIndex < count; mmrIndex++) {

        /* MMR must be all zero (after mac reset) */
        ASSERT(0 == readPlatformReg(pDev, TXBLK_FROM_MMR(mmrIndex)));

        dcu     = mmrIndex / 4;
        slice   = ((mmrIndex & 0x3) * 2);

        /* two slices (16 bits) per MMR (32 bit) */
        bitmask = (pMmrArray[mmrIndex] & 0xFFFF);
        txblkValue = bitmask | (slice << MAC_D_TXBLK_WRITE_SLICE_S) |
                     (dcu << MAC_D_TXBLK_WRITE_DCU_S) |
                     (1 << MAC_D_TXBLK_WRITE_COMMAND_S);
        writePlatformReg(pDev, txblkReg, txblkValue);


        bitmask = (pMmrArray[mmrIndex] >> 16);
        slice++;
        txblkValue = bitmask | (slice << MAC_D_TXBLK_WRITE_SLICE_S) |
                     (dcu << MAC_D_TXBLK_WRITE_DCU_S) |
                     (1 << MAC_D_TXBLK_WRITE_COMMAND_S);
        writePlatformReg(pDev, txblkReg, txblkValue);

        ASSERT(pMmrArray[mmrIndex] == readPlatformReg(pDev, TXBLK_FROM_MMR(mmrIndex)));
    }
}

/**************************************************************
 * ar5513PauseTx
 *
 * Pause/unpause transmission on the specified queue
 */
void
ar5513PauseTx(WLAN_DEV_INFO *pDev, A_UINT32 queueMask, A_BOOL pause)
{
    if (pause) {
        queueMask &= MAC_D_TXPSE_CTRL_M;         /* only least significant 10 bits */

        ASSERT(pDev->pHalInfo->txQueueAllocMask & queueMask);

        writePlatformReg(pDev, MAC_D_TXPSE, queueMask);
        /* wait until pause request has been served */
        while (!(readPlatformReg(pDev, MAC_D_TXPSE) & MAC_D_TXPSE_STATUS))
            ;
    } else {
        writePlatformReg(pDev, MAC_D_TXPSE, 0);
    }
}

#ifdef MULTI_RATE_RETRY_ENABLE
/**************************************************************
 * ar5513SetRates 
 *      Sets up rate series.
 * 
 * HACK ALERT... we need a better long term stratergy than the following 
 * while loop...
 *
 * Returns:
 *      void
 */
LOCAL void
ar5513SetRates(WLAN_DEV_INFO *pdevInfo, AR5513_TX_CONTROL *pTxControl, 
               const RATE_TABLE *pRateTbl, A_UINT16 rateIdx, A_BOOL shortPreamble,
               A_BOOL lowestRatePolicy)
{
    A_UINT16 rateCode, rateSeriesCnt;

#ifdef WME
    lowestRatePolicy = TRUE;
    pdevInfo->staConfig.swretryEnabled = 0;
#endif

    for (rateSeriesCnt = 0; rateSeriesCnt < MAX_RATE_SERIES; rateSeriesCnt++) {

#ifdef MULTI_RATE_DEBUG
        txRateSeriesStat[rateSeriesCnt].requestedRate++; 
#endif

        rateCode = pRateTbl->info[rateIdx].rateCode
                 | (shortPreamble ? pRateTbl->info[rateIdx].shortPreamble : 0);

        switch(rateSeriesCnt) {
        case 0:
            pTxControl->TXRate0      = rateCode;  
            pTxControl->TXDataTries0 = pdevInfo->staConfig.hwTxRetries; 

            break;

        case 1:
            pTxControl->TXRate1      = rateCode; 
            pTxControl->TXDataTries1 = pdevInfo->staConfig.hwTxRetries; 
            break;


        case 2:
            pTxControl->TXRate2      = rateCode;
            pTxControl->TXDataTries2 = pdevInfo->staConfig.hwTxRetries; 
            break;

        case 3:
            /* 
             * if the lowest rate in the series is required to be 6Mbps 
             * or whatever for a given a/b/g rate then force the last 
             * element in the series to the lowest rate
             */
            if (lowestRatePolicy == TRUE) {
                rateCode = pRateTbl->info[LOWEST_RATE_INDEX].rateCode
                         | (shortPreamble ?  pRateTbl->info[LOWEST_RATE_INDEX].shortPreamble : 0);
            }
            pTxControl->TXRate3 = rateCode; 
            pTxControl->TXDataTries3 = pdevInfo->staConfig.hwTxRetries; 
            break;

        default:
            ASSERT(0);
        }
    
        /* 
         * just use the same rate index if the frame is mc/bc or fragmented
         * or RTS/CTS is enabled.
         * Additionally, if we've reached the lowest rate in the rate table,
         * fill the remaining series entries with the lowest rate.
         */
        if (rateIdx) {
            rateIdx--;
        }
    }

    /*
     * inform the MAC to override the duration field on the MAC header
     * with one set in it's internal rate to duration table 
     * Only do this if not RTS/CTS, not bc/mc and not fragmented.
     */
    pTxControl->durUpdateEn = 1;
} 
#endif

/**************************************************************
 * ar5513SetIntVeolInTxDesc 
 * 
 *  Set the Interrupt Req and VEOL for the frame 
 */
static void
ar5513SetIntVeolInTxDesc(WLAN_DEV_INFO *pdevInfo, ATHEROS_DESC *pDesc, A_BOOL value, A_BOOL queued)
{
    AR5513_TX_CONTROL       *pDescTxControl = NULL;

    /* 
     * If the descriptor is already queued one, then for efficiency purpose 
     * this function swaps only fields which contain more, IntReq & VEOL 
     * in the instead of swapping all the fields  of swapping the entire 
     * descriptor and reversing it back. For swapping this code makes the 
     * assumption that IntReq and VEOL resides in the first hw word and 
     * more bit resides in the second hw word. 
     */
    if (pDesc && pdevInfo->pHalInfo->swSwapDesc && queued) {
        pDesc->hw.word[0] = cpu2le32(pDesc->hw.word[0]);
        pDesc->hw.word[1] = cpu2le32(pDesc->hw.word[1]);
    }
    while (pDesc && pDesc->hw.txControl.more) {
        pDescTxControl = TX_CONTROL(pDesc);
        pDescTxControl->interruptReq = (value)?1:0;
        if (pdevInfo->pHalInfo->swSwapDesc && queued) {
            pDesc->hw.word[0] = cpu2le32(pDesc->hw.word[0]);
            pDesc->hw.word[1] = cpu2le32(pDesc->hw.word[1]);
        }
        pDesc = pDesc->pNextVirtPtr;
        if (pdevInfo->pHalInfo->swSwapDesc && queued) {
            pDesc->hw.word[0] = cpu2le32(pDesc->hw.word[0]);
            pDesc->hw.word[1] = cpu2le32(pDesc->hw.word[1]);
        }
    }
    /* pDesc should'nt reach NULL without reaching the Last Desc*/
    ASSERT(pDesc);

    /* Set the Last Desc interrupt Req and VEOL Bit */
    pDescTxControl = TX_CONTROL(pDesc);
    pDescTxControl->interruptReq = (value)?1:0;
    pDescTxControl->VEOL         = (value)?1:0;
    if (pdevInfo->pHalInfo->swSwapDesc && queued) {
        pDesc->hw.word[0] = cpu2le32(pDesc->hw.word[0]);
        pDesc->hw.word[1] = cpu2le32(pDesc->hw.word[1]);
    }
}                                         

/**************************************************************
 * ar5513SetupDescBurst 
 * 
 *  Sets the Descriptor for bursting if the frame qualifies for it 
 *  
 */
static INLINE void
ar5513SetupDescBurst(WLAN_DEV_INFO *pdevInfo, ATHEROS_DESC *pTxDesc,WLAN_PHY phyType)
{
    AR5513_TX_CONTROL       *pTxControl     = TX_CONTROL(pTxDesc);
    AR5513_TX_CONTROL       *pDescTxControl = NULL;
    QUEUE_DETAILS           *pQueue         = &pTxDesc->pVportBss->bss.burstQueue;
    WLAN_DATA_MAC_HEADER    *pHdr           = pTxDesc->pBufferVirtPtr.header;
    
    ASSERT(!isGrp(&pHdr->address1));
    
    if ((pdevInfo->protectOn) && 
        (phyType == WLAN_PHY_OFDM ) &&  
        (!pHdr->frameControl.moreFrag) && 
        (WLAN_GET_FRAGNUM(pHdr->seqControl) == 0)) 
    {
        ar5513SetIntVeolInTxDesc(pdevInfo, pTxDesc, TRUE, FALSE);

        /* 
         * Check to see if this would form part of a Burst when the 
         * desc is queued
         */
        if (pQueue->qBurstCount) {
            if ((pQueue->qBurstCount == 1) || 
                (pQueue->pendBFrameCount % pdevInfo->staConfig.burstSeqThreshold == 0) ||
                ((pQueue->burstCTSDur + pTxControl->RTSCTSDur) > 
                 TXOP_TO_US(pTxDesc->pVportBss->bss.phyChAPs.ac[ACI_BE].txOpLimit)))
            {
                /* Start a new burst sequence */
                if (!pQueue->pendBFrameCount) {
                    /* 
                     * The last one added to the queue was an frame which 
                     * didn't need any bursting manipulations.So if the 
                     * hardware hasn't started transmitting it, update the 
                     * veol of the last desc in the frame. If hw has started
                     * processing the last sequence then just update queueingBurst
                     * to indicate the caller has to trigger the tx to hw  
                     */
                    if (pQueue->qBurstCount > 1) {
                        ASSERT(pQueue->pBurstTailDesc);
                        ar5513SetIntVeolInTxDesc(pdevInfo, pQueue->pBurstTailDesc, TRUE, TRUE);
                    } else {
                        INIT_WLAN_INTR_LOCK(intKey);
                        LOCK_WLAN_INTR(intKey);
                        pQueue->queuingBurst = FALSE;
                        UNLOCK_WLAN_INTR(intKey);
                    }
                }
                pQueue->pBurstHeadDesc   = pTxDesc;
                pQueue->pBurstTailDesc   = pTxDesc;
                pQueue->pendBFrameCount  = 1;
                pQueue->burstCTSDur      = pTxControl->RTSCTSDur;
                pQueue->qBurstCount++;
            } else {
                /* 
                 * There is a designated Burst head existing already
                 * So just update the desc at the head with information
                 * from this decriptor and invalidate the IntReq and VEOL in 
                 * the last frame in the Burst Sequence 
                 */
                ar5513SetIntVeolInTxDesc(pdevInfo, pQueue->pBurstTailDesc, FALSE, TRUE);
                /* Update the CTS dur for the frame in the head of the Queue */
                if (pdevInfo->pHalInfo->swSwapDesc) {
                    pQueue->pBurstHeadDesc->hw.word[2] = cpu2le32(pQueue->pBurstHeadDesc->hw.word[2]);
                }
                pDescTxControl = TX_CONTROL(pQueue->pBurstHeadDesc);
                ASSERT(pQueue->burstCTSDur == pDescTxControl->RTSCTSDur);
                pQueue->burstCTSDur +=  pTxControl->RTSCTSDur;
                pDescTxControl->RTSCTSDur =  pQueue->burstCTSDur;
                
                if (pdevInfo->pHalInfo->swSwapDesc) {
                    pQueue->pBurstHeadDesc->hw.word[2] = cpu2le32(pQueue->pBurstHeadDesc->hw.word[2]);
                }
                /* 
                 * Disable the CTS & RTS enable Bit, this disables 
                 * the RTS even if RTS was set because of Theshold value but 
                 * this should since we also have CTS turned on for this frame. 
                 */
                pTxControl->RTSEnable = 0;
                pTxControl->CTSEnable = 0;
                pTxControl->RTSCTSDur = 0;
                /* Now set the pBurstTailDesc to this descriptor */
                pQueue->pBurstTailDesc = pTxDesc;
                pQueue->pendBFrameCount++;
                
            }
        } else {
            /* 
             * No Burst pending make this frame count as first burst
             * any further packets queued up will count as new burst
             * sequence 
             */
            pQueue->qBurstCount       = 1;
            pQueue->pendBFrameCount   = 1;
        }
    } else {
        if (pTxControl->interruptReq) {
            /* Reset the VEOL and IntReq retransmitted frames */
            ar5513SetIntVeolInTxDesc(pdevInfo, pTxDesc, FALSE, FALSE);
        }
        if (pQueue->qBurstCount) {
            /* 
             * Frames which doesn't need any protection are coming in. 
             * Since we have pending burst frames, frames with this 
             * attribute are made to look like another burst with the 
             * that the vEOL bit is not set. The veol for the last 
             * frame would be set when a new protected burst seq
             * starts
             */
            if (pQueue->pendBFrameCount) {

                pQueue->pBurstTailDesc   = pTxDesc;
                pQueue->pendBFrameCount  = 0;
                pQueue->qBurstCount++;
            } else {
                /* 
                 * Keep track of the last frame to set the veol bit 
                 * when other "need to be protected" frames come in 
                 */
                if (pQueue->qBurstCount == 1) {
                    
                    INIT_WLAN_INTR_LOCK(intKey);
                    LOCK_WLAN_INTR(intKey);
                    pQueue->queuingBurst = FALSE;
                    UNLOCK_WLAN_INTR(intKey);
                }
                pQueue->pBurstTailDesc = pTxDesc;
            }
        }     
    }
}


/* Descriptor Access Functions */

void
ar5513SetupTxDesc(WLAN_DEV_INFO *pdevInfo, ATHEROS_DESC *pTxDesc, A_UINT32 hwIndex)
{
    AR5513_TX_CONTROL    *pTxControl      = TX_CONTROL(pTxDesc);
    WLAN_DATA_MAC_HEADER *pHdr            = pTxDesc->pBufferVirtPtr.header;
    SIB_ENTRY            *pSib            = pTxDesc->pDestSibEntry;
    const RATE_TABLE     *pRateTable      = pTxDesc->pVportBss->bss.pRateTable;
#ifdef MULTI_RATE_RETRY_ENABLE
    A_BOOL               doMultiRates     = multiRateRetryEnable;
#endif
    DURATION             nav, ackDuration;
    A_BOOL               shortPreamble;
    A_UINT16             rateIndex, ctrlRateIndex;
    A_BOOL               toGroup, gCheck, turbogCheck;

    shortPreamble = pdevInfo->nonErpPreamble ? FALSE : USE_SHORT_PREAMBLE(pdevInfo, pSib, pHdr);

    /*
     * Unless otherwise conditionally disabled below, enable Beamforming
     * if operating as Dual Chain, and not sw retried, and not CB63, and
     * synth state workaround true.
     *
     * Moving forward, tx diversity needs to flip chain/antenna for the first
     * hw try of each sw retry attempt.
     */
#ifdef NDIS_HW
    pTxControl->bf_enable = 0;
#else
    pTxControl->bf_enable = (pdevInfo->staConfig.txChainCtrl == DUAL_CHAIN &&
                             pdevInfo->pHalInfo->done_synth_state_check_2_4 &&
                             pTxDesc->swretryCount == 0) ? 1 : 0;
    /* Workaround for 2.4 synth adds additional check above */
#endif

    /*
     * this frame is being submitted to the hw - clear up the
     * status bits! need to clear up the status bits for the
     * last descriptor only
     */
    A_MEM_ZERO((void *) TX_STATUS(pTxDesc->pTxLastDesc),sizeof(AR5513_TX_STATUS));

    /* look up the rate index */
    rateIndex     = rcRateFind(pdevInfo, pTxDesc);
    ctrlRateIndex = pRateTable->info[rateIndex].controlRate;

    ASSERT(!pdevInfo->staConfig.rateCtrlEnable ||
           ( pRateTable->info[rateIndex].valid &&
             pRateTable->info[ctrlRateIndex].valid ));

    /* Select transmit power */
    pTxControl->transmitPwrCtrl = MAX_RATE_POWER;
    pTxControl->TXDataTries0    = 0; /* facilitate optimization */
    pTxControl->TXDataTries1    = 0;
    pTxControl->TXDataTries2    = 0;
    pTxControl->TXDataTries3    = 0;
    pTxControl->TXDataTries0    = 1 + pdevInfo->staConfig.hwTxRetries;

    /*
     * HW has an issue (bug#9825) which may put the wrong
     * timestamp on a probe resp - if indicated as a probe
     * response in the tx desc control. The WAR is to mark
     * it as a beacon in the tx desc control. The tx desc
     * control for either of these two pkt types implies
     * adding the timestamp (and nothing else); just that
     * the adding the timestamp for pkt type probe resp
     * is broken.
     */
    pTxControl->PktType =
        ((pHdr->frameControl.fType == FRAME_MGT) &&
        (pHdr->frameControl.fSubtype == SUBT_PROBE_RESP))
        ? HAL_DESC_PKT_TYPE_BEACON
        : HAL_DESC_PKT_TYPE_NORMAL;

    /* Special timebase synchronization frame for AR5513 */
    if (pTxDesc->isSyncFrame) {
        pTxControl->PktType = HAL_DESC_PKT_TYPE_SYNC;
        pTxDesc->isSyncFrame = 0;
    }

    if (hwIndex != HWINDEX_INVALID) {
        pTxControl->destIdx = hwIndex;
        pTxControl->destIdxValid = 1;
    }

    toGroup = isGrp(&pHdr->address1);

    if (pdevInfo->staConfig.swretryEnabled && pSib) {
#ifdef AR5513_QOS
        pTxControl->clearDestMask = pTxDesc->needClearDest;
#else
        pTxControl->clearDestMask = pSib->needClearDest ? 1 : 0;
        pSib->needClearDest       = FALSE;
#endif
    } else {
        /*
         * Non-head fragments should have a clearDestMask always off as
         * we should never send a frag if the preceding frag got dropped
         */
        pTxControl->clearDestMask = (WLAN_GET_FRAGNUM(pHdr->seqControl) == 0) ? 1 : 0;
    }

    if (toGroup) {
        pTxControl->noAck     = 1;
        pTxControl->RTSEnable = 0;
        pTxControl->CTSEnable = 0;

        /* Disable Beam Form Enable bit in TX control, MC/BC are never beamformed */
        pTxControl->bf_enable = 0;

#ifdef MULTI_RATE_RETRY_ENABLE
        doMultiRates = FALSE;
#endif
    } else {
        /*
         * Set RTSCTS if MPDU is above the threshold; for fragmented frames
         * the hardware will automatically ignore the values except for the
         * 1st frag in a (new or retried) burst - checked with hw guys (see
         * bug #5914)
         */
        pTxControl->RTSEnable  = (pTxControl->frameLength > RTS_THRESHOLD(pdevInfo)) ? 1 : 0;
        /* SuperAG FastFrames and AR5513 AV Superframes will skip RTS/CTS */
        if (pTxDesc->ffFlag || pTxDesc->jfFlag) {
            pTxControl->RTSEnable = 0;
        }
        pTxControl->CTSEnable  = 0;
        pTxControl->RTSCTSRate = pRateTable->info[ctrlRateIndex].rateCode
                               | (shortPreamble ? pRateTable->info[ctrlRateIndex].shortPreamble : 0);
    }

    /*
     * All XR frames originating at the AP need CTS protection; CTS
     * only may be broken for stations in XR mode in the current HW
     * - the scenario where CTS protection may be useful for XR STAs
     * is rather vague anyway
     */
    if ((pRateTable->info[rateIndex].phy == WLAN_PHY_XR)
        && (pdevInfo->localSta->serviceType == WLAN_AP_SERVICE))
    {
        /*
         * CTS enable overrides RTS enable, topmost reason being HW
         * can't handle otherwise. Besides, any anticipated usage model
         * itself will be quite screwy.
         */

        pTxControl->RTSEnable = 0;
        pTxControl->CTSEnable = 1;
        pTxControl->RTSCTSRate = XR_CTS_RATE(pdevInfo);
    }

    /*
     * OFDM mode 'g' frames need protection if AP requires so for the BSS 
     *
     * 11g mode fragments use CCK rates if protection is on.
     */
    gCheck = 0;
    turbogCheck = 0;
    if (pSib) {    
        gCheck = IS_CHAN_G(pdevInfo->staConfig.pChannel->channelFlags) &&
                pSib && (pSib->wlanMode == STA_MODE_G);
        turbogCheck = IS_CHAN_108G(pdevInfo->staConfig.pChannel->channelFlags) &&
                pSib && (pSib->wlanMode == STA_MODE_G);
    }
    
    if (gCheck && pdevInfo->protectOn && 
         pRateTable->info[rateIndex].phy == WLAN_PHY_OFDM && !toGroup)
    {
        /*
         * Venice TODO: mc rate index from controlRate of defaultRateIndex
         */
        pTxControl->RTSCTSRate = pRateTable->info[pdevInfo->protectRateIdx].rateCode
                               | (shortPreamble ? pRateTable->info[pdevInfo->protectRateIdx].shortPreamble : 0);

        /*
         * nonERP protect using configured type (cts-only or rts-cts)
         * unless RTS already selected above due to RTS_THRESHOLD,
         * in which case protect with RTS-CTS anyway.
         */
        if ((pHdr->frameControl.moreFrag || WLAN_GET_FRAGNUM(pHdr->seqControl) != 0) && 
            pSib->wlanMode == STA_MODE_G)
        {
            /* 11g mode fragments use CCK rates if protection is on. */
            /* note that we are not supporting an 11g mode with only ofdm rates */
            rateIndex     = rcGetBestCckRate(pTxDesc, rateIndex);
            ctrlRateIndex = pRateTable->info[rateIndex].controlRate;

            /* TODO: may need to modify based on status of bugs #5914 and #6546 */
            pTxControl->RTSCTSRate = pRateTable->info[ctrlRateIndex].rateCode
                                   | (shortPreamble ? pRateTable->info[ctrlRateIndex].shortPreamble : 0);
        } else if (!pTxControl->RTSEnable) {
            if (PROT_TYPE_RTSCTS == pdevInfo->staConfig.protectionType) {
                /* RTS-CTS protection */
                pTxControl->RTSEnable = 1;
                pTxControl->CTSEnable = 0;
            } else {
                /* CTS-ONLY protection */
                pTxControl->RTSEnable = 0;
                pTxControl->CTSEnable = 1;
            }
        }
    }

    pTxControl->TXRate0 = pRateTable->info[rateIndex].rateCode
                        | (shortPreamble ? pRateTable->info[rateIndex].shortPreamble : 0);

    /* helpful below */    
    ackDuration = shortPreamble?pRateTable->info[rateIndex].spAckDuration:pRateTable->info[rateIndex].lpAckDuration;

    pTxControl->RTSCTSDur = 0;
    pTxControl->PKTDur0  = 0;
    if (pTxControl->RTSEnable || pTxControl->CTSEnable) {

        /* data tx time*/
        pTxControl->PKTDur0 = PHY_COMPUTE_PKT_TX_TIME(pRateTable, pTxControl->frameLength,
                                                     rateIndex, shortPreamble);
#ifdef MULTI_RATE_RETRY_ENABLE
        doMultiRates = FALSE;
#endif
    }

    /* update transmit rate stats */

    if (pSib) {
        pSib->stats.txRateKb = A_RATE_LPF(pSib->stats.txRateKb, pRateTable->info[rateIndex].rateKbps);
        pdevInfo->localSta->stats.txRateKb = pSib->stats.txRateKb;
    }

    /* Set the frame control duration */
    nav = pTxControl->noAck ? 0 : ackDuration;
    if (pHdr->frameControl.moreFrag) {
        A_UINT32 nextFragLen = pTxDesc->pTxLastDesc->pNextVirtPtr->hw.txControl.frameLength;

        /* add another '+ sifs + ack' + sifs + time for next frag */
        nav += ackDuration;
        nav += PHY_COMPUTE_TX_TIME(pRateTable, nextFragLen, rateIndex, shortPreamble);
#ifdef MULTI_RATE_RETRY_ENABLE
        doMultiRates = FALSE;
#endif
    }
    WLAN_SET_DURATION_NAV(pHdr->durationNav, nav);

    ASSERT(!((pdevInfo->staConfig.modeCTS == PROT_MODE_NONE) && pdevInfo->protectOn));
    if ((gCheck || turbogCheck) &&
         (pdevInfo->staConfig.abolt & ABOLT_BURST) &&  
         (pdevInfo->staConfig.modeCTS != PROT_MODE_NONE)) 
    {
        ar5513SetupDescBurst(pdevInfo, pTxDesc, pRateTable->info[rateIndex].phy);
    }

    /*
     * if we're not performing RTS/CTS and the frame is not mc/bc and
     * it's not fragmented then we can use multiple rates i.e a different
     * rate for each rate index. Otherwise just use the same rate for 
     * all indicies and don't let the h/w override the duration field.
     * 
     */
#ifdef MULTI_RATE_RETRY_ENABLE
    if (doMultiRates) {
        ar5513SetRates(pdevInfo, pTxControl, pRateTable, rateIndex, shortPreamble, (A_BOOL)pTxDesc->swretryCount);
    }
#endif

#ifdef MULTI_RATE_DEBUG
    if (multiRateDebugLevel > 5) {
        uiPrintf("\nTxRate0 = %d, TxRate1 = %d, TxRate2 = %d, TxRate3 = %d\n",
        pTxControl->TXRate0, 
        pTxControl->TXRate1, 
        pTxControl->TXRate2, 
        pTxControl->TXRate3);
        uiPrintf("doMultiRates %d, Hw Reties = %d\n", doMultiRates, pTxControl->TXDataTries0);
    }
#endif

}

/*
 * The following is used to avoid constantly checking to see if there's a valid
 * SIB when incrementing stats, etc.
 */
LOCAL WLAN_STATS dummySibStats;

LOCAL A_UINT32
ar5513RateSeriesToRateIdx (AR5513_TX_CONTROL *pTxControl, A_UINT32 rateSeries)
{
    switch (rateSeries) {
    case 0:
        return pTxControl->TXRate0;
        break;
    case 1:
        return pTxControl->TXRate1;
        break;
    case 2:
        return pTxControl->TXRate2;
        break;
    case 3:
        return pTxControl->TXRate3;
        break;
    default:
        ASSERT(0);
        return 0;
    }
}

/*
 * Processing of HW TX descriptor.
 */
A_STATUS
ar5513ProcessTxDesc(WLAN_DEV_INFO *pdevInfo, ATHEROS_DESC *pTxDesc)
{
    ATHEROS_DESC      *pFirst      = pTxDesc->pTxFirstDesc;
    AR5513_TX_STATUS  *pTxStatus   = TX_STATUS(pTxDesc);
    AR5513_TX_CONTROL *pTxControl  = TX_CONTROL(pFirst);
    SIB_ENTRY         *pSib        = pFirst->pDestSibEntry;
    WLAN_FRAME_HEADER *pWlanHdr    = pFirst->pBufferVirtPtr.header;
    WLAN_STATS        *pLocalStats = &pdevInfo->localSta->stats;
    WLAN_STATS        *pSibStats   = pSib ? &pSib->stats : &dummySibStats;
    A_UINT32          txRate;
    A_RSSI            rssi;
    A_RSSI            rssi1, rssi2, max_rssi, diff_rssi;

    ASSERT(pTxDesc->status.tx.status == NOT_DONE);

    /* ensure we have the status correctly */
    A_TX_DESC_CACHE_INVAL(pTxDesc);
    if (!ar5513GetTxDescDone(pdevInfo, pTxDesc, TRUE)) {
        return A_EBUSY;
    }
    
    ar5513SwapHwDesc(pdevInfo, pFirst, pTxDesc, 1);
#ifdef MULTI_RATE_RETRY_ENABLE
    txRate = ar5513RateSeriesToRateIdx(pTxControl, pTxStatus->finalTSIdx);
#else
    txRate = pTxControl->TXRate0;
#endif


    if (pTxStatus->pktTransmitOK) {
        pTxDesc->status.tx.status = TRANSMIT_OK;

        /*
         * Workaround for AR5513 2.4 GHz synth state problem in E2.0.
         *
         * Upon reading first successfully received packet, as
         * signalled by done_synth_state_check_2_4 == 0,
         * update phase cal info for the WAR logic
         */
        if (!pdevInfo->pHalInfo->done_synth_state_check_2_4 &&
             pdevInfo->staConfig.txChainCtrl == DUAL_CHAIN)
        {
            ar5513UpdateSynth2_4War(pdevInfo);
        }

    } else if (pTxStatus->excessiveRetries) {
        ASSERT(!pTxStatus->filtered);
        pTxDesc->status.tx.status = EXCESSIVE_RETRIES;
    } else {
        ASSERT(pTxStatus->filtered);
        pTxDesc->status.tx.status = FILTERED;
    }

    /* Update software copies of the HW status */
    pTxDesc->status.tx.seqNum     = (A_UINT16)pTxStatus->txSeqNum;
    pTxDesc->status.tx.timestamp  = (A_UINT16)pTxStatus->sendTimestamp;
    pTxDesc->status.tx.retryCount = (A_UINT16)pTxStatus->dataFailCnt +
                                    (A_UINT16)pTxStatus->RTSFailCnt;
    pTxDesc->status.tx.rate       = (A_UINT8)txRate;

    if (! pTxControl->noAck) {
        /* This is based on checking rx instead of tx so acks can be MRC */
    switch (pdevInfo->staConfig.rxChainCtrl) {
        case CHAIN_FIXED_A:
            if (pTxStatus->ackChain0AntSel) {
                rssi = (A_RSSI) pTxStatus->ackRssiAnt1Chain0;
            } else {
                rssi = (A_RSSI) pTxStatus->ackRssiAnt0Chain0;
            }
            break;
        case CHAIN_FIXED_B:
            if (pTxStatus->ackChain1AntSel) {
                rssi = (A_RSSI) pTxStatus->ackRssiAnt1Chain1;
            } else {
                rssi = (A_RSSI) pTxStatus->ackRssiAnt0Chain1;
            }
            break;
        case DUAL_CHAIN:
            if (pTxStatus->ackChain0AntSel) {
                rssi1 = (A_RSSI) pTxStatus->ackRssiAnt1Chain0;
            } else {
                rssi1 = (A_RSSI) pTxStatus->ackRssiAnt0Chain0;
            }
    
            if (pTxStatus->ackChain1AntSel) {
                rssi2 = (A_RSSI) pTxStatus->ackRssiAnt1Chain1;
            } else {
                rssi2 = (A_RSSI) pTxStatus->ackRssiAnt0Chain1;
            }
            if (((char)rssi1 != 0x80) && ((char)rssi2 != 0x80)) {
                if (rssi1 > rssi2) {
                    max_rssi = rssi1;
                    diff_rssi = rssi1 - rssi2;
                } else {
                    max_rssi = rssi2;
                    diff_rssi = rssi2 - rssi1;
                }
            } else if ((char)rssi1 != 0x80) {
                max_rssi = rssi1;
                diff_rssi = TX_RSSI_COMBINE_TABLE_SIZE;
            } else if ((char)rssi2 != 0x80){
                max_rssi = rssi2;
                diff_rssi = TX_RSSI_COMBINE_TABLE_SIZE;
            } else { /* both rssi1 and rssi2 are invalid */
                max_rssi = (pSib && pSib->txRateCtrl.rssiLast) ? pSib->txRateCtrl.rssiLast : 30;
                diff_rssi = TX_RSSI_COMBINE_TABLE_SIZE;
            }

            if (diff_rssi < TX_RSSI_COMBINE_TABLE_SIZE) {
                rssi = max_rssi + txRssiTable[diff_rssi];
            }
            else {
                rssi = max_rssi;
            }
            break;
        default:
            rssi = (pSib && pSib->txRateCtrl.rssiLast) ? pSib->txRateCtrl.rssiLast : 30;
            break;
        }
    }
    else {
        rssi = (pSib && pSib->txRateCtrl.rssiLast) ? pSib->txRateCtrl.rssiLast : 30;
    }

    PKTLOG_TX_PKT(pdevInfo,
            pTxControl->frameLength,
            txRate,
            rssi,
            pTxDesc->status.tx.retryCount,
            (gSlotTime | pdevInfo->useShortSlotTime ? 1 : 0) << 4 |
            pTxStatus->filtered         << 3 |
            pTxStatus->fifoUnderrun     << 2 |
            pTxStatus->excessiveRetries << 1 |
            pTxStatus->pktTransmitOK    << 0,
            *(A_UINT16*)&pWlanHdr->frameControl,
            (A_UINT16)pTxStatus->txSeqNum,
            pdevInfo->staConfig.txChainCtrl << 8 | /* 2 bits: A, B, Dual */
            pTxControl->bf_enable           << 6 |
            pTxStatus->beamFormEnabled      << 5 |
            pTxStatus->ackChainStrong       << 4 |
            pTxStatus->ackChain1AntReq      << 3 |
            pTxStatus->ackChain0AntReq      << 2 |
            pTxStatus->ackChain1AntSel      << 1 |
            pTxStatus->ackChain0AntSel      << 0,
            (A_UINT16)pTxStatus->sendTimestamp,
            (A_RSSI) pTxStatus->ackRssiAnt0Chain0,
            (A_RSSI) pTxStatus->ackRssiAnt1Chain0,
            (A_RSSI) pTxStatus->ackRssiAnt0Chain1,
            (A_RSSI) pTxStatus->ackRssiAnt1Chain1);

    /* Update statistics */
    if (pTxStatus->pktTransmitOK) {
        pLocalStats->ackRssi = A_RSSI_LPF(pLocalStats->ackRssi, rssi);
        pSibStats->ackRssi   = A_RSSI_LPF(pSibStats->ackRssi, rssi);

    if (pTxStatus->beamFormEnabled) {
        pLocalStats->TxUnicastBeamFormed++;  /* AV10 STA */
        pSibStats->TxUnicastBeamFormed++;    /* AV10 AP  */
    }

#if defined(DEBUG) || !defined(BUILD_AP) || defined(SHOW_RETRIES)
        /*
         * Update stats if there were any retries for the good frame.
         *
         * If the frame saw excessive retries, do not change the antenna.
         * This matches Oahu, and is ok for Crete unless the HW retry count
         * is set to less than three.
         *
         * Toggle antenna setting in SIB.  The hardware switches the
         * antennas every two transmit failures.  This is for the 2nd retry
         * (3rd transmit),  3th retry (4th transmit), etc.
         *
         * For excessive retries keep the antenna the same.
         */
        // TODO: Needs Venice port hw multirate retry tx series
        if (pTxStatus->dataFailCnt || pTxStatus->RTSFailCnt) {
            int shortCount = 0;
            int longCount  = 0;

            pLocalStats->TotalRetries++;
            pSibStats->TotalRetries++;

            if (!pTxControl->RTSEnable) {
                shortCount = pTxStatus->dataFailCnt;
            } else {
                shortCount = pTxStatus->RTSFailCnt;
                longCount  = pTxStatus->dataFailCnt;
            }

            /*
             * Short retry count precedence over long retry count is
             * copied from AR5211 Hal.
             */
            if (shortCount) {
                pLocalStats->shortFrameRetryBins[shortCount]++;
                pSibStats->shortFrameRetryBins[shortCount]++;
            } else {
                pLocalStats->RetryBins[longCount]++;
                pSibStats->RetryBins[longCount]++;
            }
        }

#endif
    } else {
        pLocalStats->TransmitErrors++;
        pSibStats->TransmitErrors++;

#if defined(DEBUG) || !defined(BUILD_AP) || defined(SHOW_RETRIES)
        if (pTxStatus->excessiveRetries) {
            DRV_LOG(DRV_DEBUG_INT, ("Transmit excessive retries\n"));
            pLocalStats->TxExcessiveRetries++;
            pSibStats->TxExcessiveRetries++;
        } else if (pTxStatus->filtered) {
            DRV_LOG(DRV_DEBUG_INT,("Transmit filtered\n"));
            pLocalStats->TxFiltered++;
            pSibStats->TxFiltered++;
        } else if (pTxStatus->fifoUnderrun) {
            DRV_LOG(DRV_DEBUG_INT,("Transmit fifo underrun\n"));
            pLocalStats->TxDmaUnderrun++;
            pSibStats->TxDmaUnderrun++;
        } else {
            apPanic("Unknown transmit error.");
        }
#endif
    }

    /*
     * Don't bother counting the broadcast/multicast frames
     * because the frame won't send ACK back; oahu doesn't
     * send acks in promiscuous mode leading to excessive
     * retries for all frames - so don't update ratectrl in
     * such a situation
     */
    ASSERT(txRate);
    if (!pTxStatus->filtered &&
        (pdevInfo->rxFilterReg & HAL_RX_UCAST) &&
        !isGrp(&pWlanHdr->address1))
    {
#ifdef MULTI_RATE_RETRY_ENABLE
        A_UINT32 failedRateSeries = 0;
        A_UINT32 failedRateIdx;

        if (multiRateRetryEnable) {
            /* for all the other rates that failed inform rcUpdate */
            while (failedRateSeries < pTxStatus->finalTSIdx) {
                failedRateIdx = ar5513RateSeriesToRateIdx(pTxControl, failedRateSeries);
                rcUpdate(pdevInfo, pSib,
                        pFirst->pVportBss->bss.pRateTable->rateCodeToIndex[failedRateIdx],
                        pTxControl->frameLength,
                        1, /* Inform Rate Ctrl that this rate was bad..*/
                        pdevInfo->staConfig.hwTxRetries,
                        rssi,
                        (A_UINT8)pTxStatus->txAnt);
                failedRateSeries++;
            }

            /* 
             * For those frames that multirate retry failed, allow just one 
             * s/w retry.
             */
            if ((!pTxControl->RTSEnable) && (!pTxControl->RTSEnable) &&
                (!pWlanHdr->frameControl.moreFrag) && pFirst->swretryCount)
            {
                pFirst->swretryCount = pdevInfo->staConfig.swRetryMaxRetries; 
            }
        }
#endif

        rcUpdate(pdevInfo, 
                 pSib,
                 pFirst->pVportBss->bss.pRateTable->rateCodeToIndex[txRate],
                 pTxControl->frameLength,
                 (A_BOOL)pTxStatus->excessiveRetries,
                 pTxDesc->status.tx.retryCount,
                 rssi,
                 (A_UINT8)pTxStatus->txAnt);

#if defined(PT_2_PT_ANT_DIV) || !defined(BUILD_AP)
        ar5513ProcAntennaData(pdevInfo, pSib, pTxDesc, 0);
#else /* ! PT_2_PT_ANT_DIV */
        ar5513ProcAckAntennaData(pdevInfo, pSib, pTxDesc);
#endif /* ! PT_2_PT_ANT_DIV */
    }

    return A_OK;
}

A_BOOL
ar5513GetTxDescDone(WLAN_DEV_INFO *pDev, ATHEROS_DESC *pTxDesc, A_BOOL swap)
{
    AR5513_TX_STATUS *pTxStatus = TX_STATUS(pTxDesc);
    A_BOOL           doneBit    = (A_BOOL)pTxStatus->done;

    if (pDev->pHalInfo->swSwapDesc && swap) {
#ifdef BIG_ENDIAN
#define DESC_SWAP_DONE      0x01000000
#else
#define DESC_SWAP_DONE      0x00000001
#endif
        doneBit = ((pTxDesc->hw.word[10] & DESC_SWAP_DONE) == DESC_SWAP_DONE);
    }

    return doneBit;
}


/**************************************************************
 * ar5513MultiRateRetryEnable 
 *  Enable/Disable multi-rate retry.
 *
 */
void
ar5513MultiRateRetryEnable(A_BOOL flag) 
{
    multiRateRetryEnable = flag;
}

#if defined(DEBUG) || defined(_DEBUG)

void
ar5513DebugPrintTxDesc(WLAN_DEV_INFO *pdevInfo, ATHEROS_DESC *pDesc, A_BOOL verbose)
{
    AR5513_TX_CONTROL *pTxControl = TX_CONTROL(pDesc);
    AR5513_TX_STATUS  *pTxStatus  = TX_STATUS(pDesc);

    A_TX_DESC_CACHE_INVAL(pDesc);

    if (verbose) {
        uiPrintf(CONTROL_1      "%08x  ",        pDesc->hw.word[0]);
        uiPrintf(CONTROL_2      "%08x\n",        pDesc->hw.word[1]);
        uiPrintf(CONTROL_3      "%08x  ",        pDesc->hw.word[2]);
        uiPrintf(CONTROL_4      "%08x\n",        pDesc->hw.word[3]);
        uiPrintf(STATUS_1       "%08x  ",        pDesc->hw.word[8]);
        uiPrintf(STATUS_2       "%08x\n",        pDesc->hw.word[9]);

        uiPrintf(FRAME_LEN        "%03x       ",   pTxControl->frameLength);
        uiPrintf(XMIT_POWER       "%02x        ",  pTxControl->transmitPwrCtrl);
        uiPrintf(RTS_CTS_EN       "%01x\n",        pTxControl->RTSEnable);
        uiPrintf("VEOL           ""%01x         ", pTxControl->VEOL);
        uiPrintf(CLR_DEST_MSK     "%01x         ", pTxControl->clearDestMask);
        uiPrintf(INT_REQ          "%01x         ", pTxControl->interruptReq);
        uiPrintf(ENCRYPT_KEY_VLD  "%01x         ", pTxControl->destIdxValid);
        uiPrintf(BUFFER_LEN       "%03x\n",        pTxControl->bufferLength);
        uiPrintf(MORE             "%01x         ", pTxControl->more);
        uiPrintf(ENCRYPT_KEY_IDX  "%02x        ",  pTxControl->destIdx);
        uiPrintf(ATIM             "%01x\n",        pTxControl->PktType);
        uiPrintf(XMIT_RATE        "%01x\n",        pTxControl->TXRate0);
        if (pTxControl->more) {
            pTxControl = TX_CONTROL(pDesc->pNextVirtPtr);
            uiPrintf("Next Desc:     \n");
            uiPrintf(BUFFER_LEN       "%03x       ", pTxControl->bufferLength);
            uiPrintf(MORE             "%01x\n",      pTxControl->more);
        }

        return;
    }

    uiPrintf("%p:", (void *)pDesc);
    if (!pTxStatus->done) {
        uiPrintf(" .");
    } else {
        if (pTxStatus->pktTransmitOK) {
            uiPrintf(" ok");
        } else if (pTxStatus->excessiveRetries) {
            uiPrintf("  x");
        } else if (pTxStatus->filtered) {
            uiPrintf("  f");
        } else {
            uiPrintf("  ?");
        }
    }
    if (pDesc->staleFlag) {
        uiPrintf(" [STALE]");
    }
    if (pDesc->pDestSibEntry) {
        uiPrintf(" sta%d", pDesc->pDestSibEntry->assocId & 0x3fff);
    }
    if (pTxControl->destIdxValid) {
        uiPrintf(" slot%d%s[%s]", pTxControl->destIdx,
                 pTxControl->clearDestMask ? "+" : "",
                 ar5513DebugGetKeyType(pdevInfo, pTxControl->destIdx));
    }
    if ((pDesc->pTxFirstDesc == pDesc) && pDesc->pBufferVirtPtr.byte) {
        FRAME_CONTROL *fc = &pDesc->pBufferVirtPtr.header->frameControl;
        A_UINT16       rateKbps;

        uiPrintf(" %s[%s%s%s]",
                 halFrameTypeToName[(fc->fType << 4) + fc->fSubtype],
                 fc->wep ? "Encrypt" : "",
                 fc->retry ? "Retry" : "",
                 fc->moreFrag ? "Morefrag" : "");
        uiPrintf(" len:%d/%d", pTxControl->bufferLength, pTxControl->frameLength);
        if (pDesc->pVportBss && pDesc->pVportBss->bss.pRateTable) {
            rateKbps = pDesc->pVportBss->bss.pRateTable->info[
                pDesc->pVportBss->bss.pRateTable->rateCodeToIndex[pTxControl->TXRate0]
                ].rateKbps;
            uiPrintf(" @%d", rateKbps/1000);
        }
    } else {
        uiPrintf(" ---- len:%d", pTxControl->bufferLength);
    }
    if (pDesc->swRetryFlag) {
        uiPrintf(" requeued");
    }
    uiPrintf(" %s", pDesc->hw.txControl.more ? "->" : "|");
    if (pDesc->pNextVirtPtr) {
        if (pDesc->nextPhysPtr != pDesc->pNextVirtPtr->thisPhysPtr) {
            uiPrintf(" {0x%x != 0x%x}", pDesc->nextPhysPtr, pDesc->pNextVirtPtr->thisPhysPtr);
        }
    } else if (pDesc->nextPhysPtr) {
        uiPrintf(" {0x%x != NULL}", pDesc->nextPhysPtr);
    }
    uiPrintf("\n");
}

void
ar5513MonitorRxTxActivity(WLAN_DEV_INFO *pDev, int index, int display)
{
    A_UINT32 cycCnt, rxClearCnt, rxFrameCnt, txFrameCnt;
    A_UINT32 tmpCycCnt;
#if defined(RX_CLEAR_OBS)
    A_UINT32 reg;

    reg = sysRegRead(AR5513_GPIO_DO0);
    sysRegWrite(AR5513_GPIO_DO0, (reg | (1 << 12)));
    udelay(4);
    sysRegWrite(AR5513_GPIO_DO0, (reg & ~(1 << 12)));
#endif /* RX_CLEAR_OBS */

    /* guard against wrap-around during register read */
    do {
        cycCnt     = (A_UINT32) A_REG_RD(pDev, MAC_CCCNT);
        rxClearCnt = (A_UINT32) A_REG_RD(pDev, MAC_RCCNT);
        rxFrameCnt = (A_UINT32) A_REG_RD(pDev, MAC_RFCNT);
        txFrameCnt = (A_UINT32) A_REG_RD(pDev, MAC_TFCNT);
        tmpCycCnt  = (A_UINT32) A_REG_RD(pDev, MAC_CCCNT);
    } while (tmpCycCnt < cycCnt);

    if (display) {
        cycCnt /= 100;
        isrPrintf("[%d] Channel Active = %d%%, Receiving = %d%%, Transmitting = %d%%\n", index, rxClearCnt/cycCnt, rxFrameCnt/cycCnt, txFrameCnt/cycCnt);   
    }

    /* Reset counters for next time */
    A_REG_WR(pDev, MAC_CCCNT, 0);
    A_REG_WR(pDev, MAC_RCCNT, 0);
    A_REG_WR(pDev, MAC_RFCNT, 0);
    A_REG_WR(pDev, MAC_TFCNT, 0);
}

#endif // #ifdef DEBUG

#endif // #ifdef BUILD_AR5513

